#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE

#include <stdio.h>
#include <stdlib.h>
#include <ctime>
#include <unistd.h>
#include <libportaudio/portaudio.h>
#include <libportaudio/sndfile.h>
#include <sys/types.h>
#include <libportaudio/pa_mac_core.h>
#include <dirent.h>
#include <pthread.h>
#include <cstring>
#include <Headers/SDL_ttf.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <SDL2/SDL.h>
}

#undef main

#define NUM_SECONDS   (5)
#define FRAMES_PER_BUFFER  (512)
#define NUM_CHANNELS (32)

typedef struct
{
    SNDFILE *file[NUM_CHANNELS];
    SF_INFO info[NUM_CHANNELS];
}
paData;

void cleanup(AVFormatContext *fmt_ctx, AVCodecContext *CodecCtx, FILE *fin, FILE *fout, AVFrame *frame, AVPacket *pkt);
void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize, FILE *f);
void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt, FILE *f, double &frameDuration, int &firstFrame, double &timeBase, Uint32 &lTime, Uint32 &elapsed);
void displayFrame(AVFrame * frame, AVCodecContext *dec_ctx, int &firstFrame, double &timeBase, double &frameDuration, Uint32 &lTime, Uint32 &elapsed);
int initSDL(AVCodecContext *codec_ctx);
void *audio_function( void *ptr );

SDL_Renderer *renderer;
SDL_Texture *texture;
SDL_Rect r;
float volume = 50;
int positionX = 0;
int positionY = 0;

void cleanup(AVFormatContext *fmt_ctx, AVCodecContext *CodecCtx, FILE *fin, FILE *fout, AVFrame *frame, AVPacket *pkt) {
    
    if (fin)
        fclose(fin);
    
    if (fout)
        fclose(fout);
    
    if (CodecCtx)
        avcodec_close(CodecCtx);
    
    if (fmt_ctx)
        avformat_close_input(&fmt_ctx);
    
    if (frame)
        av_frame_free(&frame);
    
    if (pkt)
        av_packet_free(&pkt);
}

void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize, FILE *f)
{
    // write header
    fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
    // loop until all rows are written to file
    for (int i = 0; i < ysize; i++)
        fwrite(buf + i * wrap, 1, xsize, f);
}

void displayFrame(AVFrame * frame, AVCodecContext *dec_ctx, int &firstFrame, double &timeBase, double &frameDuration, Uint32 &lTime, Uint32 &elapsed)
{
    // pass frame data to texture then copy texture to renderer and present renderer
    SDL_UpdateYUVTexture(texture, &r, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2]);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    SDL_Event event;
    SDL_PollEvent(&event);
    
    if (!firstFrame) {
        SDL_Delay((Uint32)((frame->best_effort_timestamp - frameDuration) * timeBase));
        lTime = SDL_GetTicks();
        frameDuration = frame->best_effort_timestamp;
        firstFrame = 1;
    }
    else {
        elapsed = SDL_GetTicks() - lTime;
        if (elapsed < (Uint32)((frame->best_effort_timestamp - frameDuration) * timeBase * 1000)) {
            //linux
            usleep((Uint32)(((frame->pts - frameDuration) * timeBase * 1000) - elapsed) * 1000);
            //std::this_thread::sleep_for(std::chrono::microseconds((Uint32)(((frame->pts - frameDuration) * timeBase * 1000) - elapsed) * 1000));
            //printf(" %d ", (Uint32)(((frame->pts - frameDuration) * timeBase * 1000) - elapsed));
        }
        
        lTime = SDL_GetTicks();
        frameDuration = frame->best_effort_timestamp;
    }
}


void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt, FILE *f, double &frameDuration, int &firstFrame, double &timeBase, Uint32 &lTime, Uint32 &elapsed)
{
    int ret;
    
    //send packet to decoder
    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        exit(1);
    }
    while (ret >= 0) {
        // receive frame from decoder
        // we may receive multiple frames or we may consume all data from decoder, then return to main loop
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            // something wrong, quit program
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }
        //printf("processing frame %3d\n", dec_ctx->frame_number);
        fflush(stdout);
        
        
        // display frame on sdl window
        displayFrame(frame, dec_ctx, firstFrame, timeBase, frameDuration, lTime, elapsed);
        
    }
}


int initSDL(AVCodecContext *codec_ctx)
{
    SDL_Window *window = NULL;
    
    // init SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        fprintf(stderr, "could not init sdl %s\n", SDL_GetError());
        return -1;
    }
    
    //create sdl window
    window = SDL_CreateWindow("Preview", positionX, positionY, codec_ctx->width, codec_ctx->height, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!window)
    {
        fprintf(stderr, "could not create sdl window \n");
        return -1;
    }
    
    // specify rectangle position and scale
    r.x = 0;
    r.y = 0;
    r.w = codec_ctx->width;
    r.h = codec_ctx->height;
    
    // create sdl renderer
    renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer)
    {
        fprintf(stderr, "could not create sdl renderer \n");
        return -1;
    }
    
    // create texture
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, codec_ctx->width, codec_ctx->height);
    if (!texture)
    {
        fprintf(stderr, "could not create sdl texture \n");
        return -1;
    }
    
    return 0;
}

// portaudio functions
static int paStreamCallback(const void *inputBuffer, void *outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData)
{
    float **out;
    paData *p_data = (paData *)userData;
    sf_count_t num_read;
    
    out = (float **)outputBuffer;
    p_data = (paData *)userData;
    
    for (int i = 0; i < NUM_CHANNELS; i++)
    {
        /* clear output buffer */
        memset(out[i], 0, sizeof(float) * framesPerBuffer * p_data->info[i].channels);
        
        /* read directly into output buffer */
        num_read = sf_read_float(p_data->file[i], out[i], framesPerBuffer * p_data->info[i].channels);
        
        /* adjust the volume of the output */
        for (int j = 0; j < framesPerBuffer; j++)
        {
            out[i][j] *= (volume / 100);
        }
        
        /*  If we couldn't read a full frameCount of samples we've reached EOF */
        if (num_read < framesPerBuffer)
        {
            return paComplete;
        }
    }
    
    return paContinue;
}

void *audio_function( void *ptr ){
    // Pa variables
    PaStreamParameters outputParameters;
    PaStream *stream;
    PaError err;
    paData data;
    
    //Utility variables
    int i = 0;
    
    /*
    // File IO variables
    const char* path = "./"; ///Users/allanhsu/Downloads/Audio WAV and Quicktime Video Files
    int opened = 0;
     */
    
    /* Open the soundfile */
    for (int i = 0; i < NUM_CHANNELS; i++) {
        char file[13] = "media/";
		char fileNum[4];
        sprintf(fileNum, "%d", i + 1);
        strcat(file, fileNum);
        strcat(file, ".wav");
        data.file[i] = sf_open(file, SFM_READ, &data.info[i]);
        if (sf_error(data.file[i]) != SF_ERR_NO_ERROR) {
            fprintf(stderr, "%s\n", sf_strerror(data.file[i]));
            fprintf(stderr, "File: %s\n", file);
        }
    }
    /*
     if (opened < NUM_CHANNELS) {
     for (i = 0; i < opened; ++i) {
     sf_close(data.file[i]);
     }
     printf("need at least %d wav files.", NUM_CHANNELS);
     getchar();
     return (void *) 0;
     }
     */
#ifdef __APPLE__
    PaMacCoreStreamInfo macInfo;
    //const SInt32 channelMap[NUM_CHANNELS] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31};
    //const SInt32 channelMap[NUM_CHANNELS] = { 0,1 };
#endif
    
    err = Pa_Initialize();
    if (err != paNoError) goto error;
    
    printf("Pa Initialized... \n");
    
    /** setup host specific info */
#ifdef __APPLE__
    PaMacCore_SetupStreamInfo(&macInfo, paMacCorePlayNice);
    //PaMacCore_SetupChannelMap(&macInfo, channelMap, NUM_CHANNELS);
    printf("PaMacCore ChannelMap set. \n");
    outputParameters.hostApiSpecificStreamInfo = &macInfo;
#else
    printf("Channel mapping not supported on this platform. Using windows Settings.\n");
    outputParameters.hostApiSpecificStreamInfo = NULL;
#endif
    
    /* outputParameters regardless of host */
    outputParameters.device = Pa_GetDefaultOutputDevice(); /* default output device */
    if (outputParameters.device == paNoDevice) {
        fprintf(stderr, "Error: No default output device.\n");
        goto error;
    }
    outputParameters.channelCount = NUM_CHANNELS; /* 32 channels output */
    outputParameters.sampleFormat = paFloat32 | paNonInterleaved; /* 32 bit floating point output */
    outputParameters.suggestedLatency = Pa_GetDeviceInfo(outputParameters.device)->defaultHighOutputLatency;
    
    
    /* Open PaStream with values read from the file */
    err = Pa_OpenStream(
                        &stream,
                        NULL, /* no input */
                        &outputParameters,
                        Pa_GetDeviceInfo(outputParameters.device)->defaultSampleRate ,
                        FRAMES_PER_BUFFER,
                        paClipOff,      /* we won't output out of range samples so don't bother clipping them */
                        paStreamCallback,
                        &data);
    if (err != paNoError) goto error;
    
    /* Starts Stream here */
    err = Pa_StartStream(stream);
    if (err != paNoError) goto error;
    
    /* Wait for the whole duration of file to finish playing */
    while (Pa_IsStreamActive(stream)) {
        Pa_Sleep(100);
    }
    
    /* Closes all opened audio files */
    for (i = 0; i < NUM_CHANNELS; ++i) {
        sf_close(data.file[i]);
    }
    
    /*  Shuts down portaudio */
    err = Pa_CloseStream(stream);
    if (err != paNoError) goto error;
    
    Pa_Terminate();
    if (err != paNoError) goto error;
    
    printf("Playback completes!\n");
    printf("Enter any key to exit program.");
    getchar();
    
    return 0;
error:
    Pa_Terminate();
    fprintf(stderr, "An error occured while using the portaudio stream\n");
    fprintf(stderr, "Error number: %d\n", err);
    fprintf(stderr, "Error message: %s\n", Pa_GetErrorText(err));
    
    printf("Enter any key to exit program.");
    getchar();
    
    return (void *) 0;
}


int main() {
    
    const char *file = "media/video.mp4";
	const char *outfile = "media/video.yuv";
    
    int ret = 0;
    int videoFPS = 0;
    int firstFrame = 0;
    int VideoStreamIndex = -1;
    double frameDuration = 0;
    double timeBase = 0;
    Uint32 lTime;
    Uint32 elapsed;
    
    FILE *fin = NULL;
    FILE *fout = NULL;
    AVCodec *Codec = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    AVCodecContext *codecCtx = NULL;
    AVFormatContext *fmt_ctx = NULL;
    
    SDL_Window *initWindow = NULL;
    SDL_Renderer *renderer = NULL;
    SDL_Event event;
    SDL_Texture *playTextTexture = NULL;
    SDL_Texture *volUpTextTexture = NULL;
    SDL_Texture *volDownTextTexture = NULL;
    SDL_Texture *volTextTexture = NULL;
    SDL_Rect play;
    SDL_Rect volUp;
    SDL_Rect volDown;
    SDL_Rect vol;
    TTF_Font *font = NULL;
    SDL_Surface *playTextSurface = NULL;
    SDL_Surface *volUpTextSurface = NULL;
    SDL_Surface *volDownTextSurface = NULL;
    SDL_Surface *volTextSurface = NULL;
    SDL_Color textColor = {0, 0, 0};
    char volumeText[4];
    
    pthread_t thread1;
    
    ret = avformat_open_input(&fmt_ctx, file, NULL, NULL);
    
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot open input file\n");
        cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
        return -1;
    }
    
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "cannot get stream info\n");
        cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
        return -2;
    }
    
    for (int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            VideoStreamIndex = i;
            break;
        }
    }
    
    if (VideoStreamIndex < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "No video stream\n");
        cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
        return -3;
    }
    
    videoFPS = av_q2d(fmt_ctx->streams[VideoStreamIndex]->r_frame_rate);
    frameDuration = 1000.0 / videoFPS;
    timeBase = av_q2d(fmt_ctx->streams[VideoStreamIndex]->time_base);
    printf("fps is %d\n", videoFPS);
    
    // dump video stream info
    av_dump_format(fmt_ctx, VideoStreamIndex, file, false);
    
    //alloc memory for codec context
    codecCtx = avcodec_alloc_context3(NULL);
    
    // retrieve codec params from format context
    ret = avcodec_parameters_to_context(codecCtx, fmt_ctx->streams[VideoStreamIndex]->codecpar);
    
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot get codec parameters\n");
        cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
        return -4;
    }
    
    // find decoding codec
    Codec = avcodec_find_decoder(codecCtx->codec_id);
    
    if (Codec == NULL)
    {
        av_log(NULL, AV_LOG_ERROR, "No decoder found\n");
        cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
        return -5;
    }
    
    // try to open codec
    ret = avcodec_open2(codecCtx, Codec, NULL);
    
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
        cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
        return -6;
    }
    
    printf("\nDecoding codec is : %s\n", Codec->name);
    
    //init packet
    pkt = av_packet_alloc();
    av_init_packet(pkt);
    if (!pkt)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot init packet\n");
        cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
        return -7;
    }
    
    // init frame
    frame = av_frame_alloc();
    if (!frame)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot init frame\n");
        cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
        return -8;
    }
    
    //open input file
    fin = fopen(file, "rb");
    if (!fin)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
        return -9;
    }
    
    // open output file
    fout = fopen(outfile, "w");
    if (!fout)
    {
        av_log(NULL, AV_LOG_ERROR, "Cannot open output file\n");
        cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
        return -10;
    }
    
    /* Init SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0)
    {
        fprintf(stderr, "could not init sdl %s\n", SDL_GetError());
        return -1;
    }
    
    /* Init SDL TTF */
    if (TTF_Init() == -1)
    {
        fprintf(stderr, "could not init sdl_ttf %s\n");
        return -1;
    }
    
    /* Create play window */
    initWindow = SDL_CreateWindow("Emplace AV", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 280, 50, 0);
    if (!initWindow)
    {
        fprintf(stderr, "could not create sdl window \n");
        return -1;
    }
    
    /* Create renderer for window */
    renderer = SDL_CreateRenderer(initWindow, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer)
    {
        fprintf(stderr, "could not create sdl renderer \n");
        return -1;
    }
    
    /* Get font */
    font = TTF_OpenFont("font.ttf", 36);
    if (!font)
    {
        fprintf(stderr, "could not create sdl font \n");
        return -1;
    }
    
    /* Set up text surface and text texture for each text entry */
    playTextSurface = TTF_RenderText_Solid(font, "Play", textColor);
    if (!playTextSurface)
    {
        fprintf(stderr, "could not create sdl text surface \n");
    }
    
    playTextTexture = SDL_CreateTextureFromSurface(renderer, playTextSurface);
    if (!playTextTexture)
    {
        fprintf(stderr, "could not create sdl text texture \n");
    }
    
    volUpTextSurface = TTF_RenderText_Solid(font, "+", textColor);
    if (!volUpTextSurface)
    {
        fprintf(stderr, "could not create sdl text surface \n");
    }
    
    volUpTextTexture = SDL_CreateTextureFromSurface(renderer, volUpTextSurface);
    if (!volUpTextTexture)
    {
        fprintf(stderr, "could not create sdl text texture \n");
    }
    
    volDownTextSurface = TTF_RenderText_Solid(font, "-", textColor);
    if (!volDownTextSurface)
    {
        fprintf(stderr, "could not create sdl text surface \n");
    }
    
    volDownTextTexture = SDL_CreateTextureFromSurface(renderer, volDownTextSurface);
    if (!volDownTextTexture)
    {
        fprintf(stderr, "could not create sdl text texture \n");
    }
    
    /* Set the volume text */
    sprintf(volumeText, "%d", (int)volume);
    
    
    volTextSurface = TTF_RenderText_Solid(font, volumeText, textColor);
    if (!volTextSurface)
    {
        fprintf(stderr, "could not create sdl text surface \n");
    }
    
    volTextTexture = SDL_CreateTextureFromSurface(renderer, volTextSurface);
    if (!volTextTexture)
    {
        fprintf(stderr, "could not create sdl text texture \n");
    }
    
    SDL_QueryTexture(playTextTexture, NULL, NULL, &play.w, &play.h);
    SDL_QueryTexture(volUpTextTexture, NULL, NULL, &volUp.w, &volUp.h);
    SDL_QueryTexture(volDownTextTexture, NULL, NULL, &volDown.w, &volDown.h);
    SDL_QueryTexture(volTextTexture, NULL, NULL, &vol.w, &vol.h);
    
    /* Set the x and y positions of the text */
    play.x = 200;
    play.y = 0;
    volUp.x = 110;
    volUp.y = 0;
    volDown.x = 10;
    volDown.y = 0;
    vol.x = 35;
    vol.y = 0;
    
    /* Render the window and text */
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, playTextTexture, NULL, &play);
    SDL_RenderCopy(renderer, volUpTextTexture, NULL, &volUp);
    SDL_RenderCopy(renderer, volDownTextTexture, NULL, &volDown);
    SDL_RenderCopy(renderer, volTextTexture, NULL, &vol);
    SDL_RenderPresent(renderer);
    
    while (initWindow)
    {
        if (SDL_WaitEvent(&event))
        {
            switch (event.type)
            {
                case SDL_QUIT:
                    initWindow = NULL;
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (event.button.button == SDL_BUTTON_LEFT)
                    {
                        int x = event.button.x;
                        int y = event.button.y;
                        
                        /* The play button is clicked */
                        if ((x > play.x) && (x < play.x + play.w) && (y > play.y) && (y < play.y + play.h))
                        {
                            /* Destroy the play window before creating the video window */
                            SDL_GetWindowPosition(initWindow, &positionX, &positionY);
                            SDL_DestroyWindow(initWindow);
                            SDL_DestroyRenderer(renderer);
                            initWindow = NULL;
                            
                            // init sdl
                            if (initSDL(codecCtx) < 0)
                            {
                                av_log(NULL, AV_LOG_ERROR, "init sdl failed\n");
                                cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
                                return -11;
                            }
                            
                            pthread_create( &thread1, NULL, audio_function, NULL);
                            
                            // main loop
                            while (1)
                            {
                                // read an encoded packet from file
                                ret = av_read_frame(fmt_ctx, pkt);
                                
                                if (ret < 0)
                                {
                                    av_log(NULL, AV_LOG_ERROR, "cannot read frame\n");
                                    break;
                                }
                                // if packet data is video data then send it to decoder
                                if (pkt->stream_index == VideoStreamIndex)
                                {
                                    decode(codecCtx, frame, pkt, fout, frameDuration, firstFrame, timeBase, lTime, elapsed);
                                }
                                
                                // release packet buffers to be allocated again
                                av_packet_unref(pkt);
                                
                            }
                            
                            //flush decoder
                            decode(codecCtx, frame, pkt, fout, frameDuration, firstFrame, timeBase, lTime, elapsed);
                            
                            cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
                            
                            pthread_join( thread1, NULL);
                        }
                        /* The volume up button is clicked */
                        else if ((x > volUp.x) && (x < volUp.x + volUp.w) && (y > volUp.y) && (y < volUp.y + volUp.h) && (volume < 100))
                        {
                            volume++;
                            sprintf(volumeText, "%d", (int)volume);
                            
                            volTextSurface = TTF_RenderText_Solid(font, volumeText, textColor);
                            if (!volTextSurface)
                            {
                                fprintf(stderr, "could not create sdl text surface \n");
                            }
                            
                            volTextTexture = SDL_CreateTextureFromSurface(renderer, volTextSurface);
                            if (!volTextTexture)
                            {
                                fprintf(stderr, "could not create sdl text texture \n");
                            }
                            
                            SDL_QueryTexture(volTextTexture, NULL, NULL, &vol.w, &vol.h);
                            
                            if (volume == 100)
                            {
                                vol.x = 35;
                            }
                            else
                            {
                                vol.x = 45;
                            }
                            vol.y = 0;
                            
                            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                            SDL_RenderClear(renderer);
                            SDL_RenderCopy(renderer, playTextTexture, NULL, &play);
                            SDL_RenderCopy(renderer, volUpTextTexture, NULL, &volUp);
                            SDL_RenderCopy(renderer, volDownTextTexture, NULL, &volDown);
                            SDL_RenderCopy(renderer, volTextTexture, NULL, &vol);
                            SDL_RenderPresent(renderer);
                        }
                        /* The volume down button is clicked */
                        else if ((x > volDown.x) && (x < volDown.x + volDown.w) && (y > volDown.y) && (y < volDown.y + volDown.h) && (volume > 0))
                        {
                            volume--;
                            sprintf(volumeText, "%d", (int)volume);
                            
                            volTextSurface = TTF_RenderText_Solid(font, volumeText, textColor);
                            if (!volTextSurface)
                            {
                                fprintf(stderr, "could not create sdl text surface \n");
                            }
                            
                            volTextTexture = SDL_CreateTextureFromSurface(renderer, volTextSurface);
                            if (!volTextTexture)
                            {
                                fprintf(stderr, "could not create sdl text texture \n");
                            }
                            
                            SDL_QueryTexture(volTextTexture, NULL, NULL, &vol.w, &vol.h);
                            
                            if (volume == 100)
                            {
                                vol.x = 35;
                            }
                            else
                            {
                                vol.x = 45;
                            }
                            vol.y = 0;
                            
                            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                            SDL_RenderClear(renderer);
                            SDL_RenderCopy(renderer, playTextTexture, NULL, &play);
                            SDL_RenderCopy(renderer, volUpTextTexture, NULL, &volUp);
                            SDL_RenderCopy(renderer, volDownTextTexture, NULL, &volDown);
                            SDL_RenderCopy(renderer, volTextTexture, NULL, &vol);
                            SDL_RenderPresent(renderer);
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }
    
    return 0;
}
