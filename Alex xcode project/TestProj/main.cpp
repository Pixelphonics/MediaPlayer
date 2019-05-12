#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <ctime>
#include <unistd.h>

extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <SDL2/SDL.h>
}

#undef main

void cleanup(AVFormatContext *fmt_ctx, AVCodecContext *CodecCtx, FILE *fin, FILE *fout, AVFrame *frame, AVPacket *pkt);
void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize, FILE *f);
void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt, FILE *f, double frameDuration, time_t &lastTime, time_t &timeNow, int &firstFrame);
void displayFrame(AVFrame * frame, AVCodecContext *dec_ctx);
int initSDL(AVCodecContext *codec_ctx);


SDL_Renderer *renderer;
SDL_Texture *texture;
SDL_Rect r;

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

void displayFrame(AVFrame * frame, AVCodecContext *dec_ctx)
{
    // pass frame data to texture then copy texture to renderer and present renderer
    SDL_UpdateYUVTexture(texture, &r, frame->data[0], frame->linesize[0], frame->data[1], frame->linesize[1], frame->data[2], frame->linesize[2]);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    SDL_Event event;
    SDL_PollEvent(&event);
}


void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt, FILE *f, double frameDuration, time_t &lastTime, time_t &timeNow, int &firstFrame)
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
        printf("processing frame %3d\n", dec_ctx->frame_number);
        fflush(stdout);
        
        
        // display frame on sdl window
        displayFrame(frame, dec_ctx);
        
        // control this sleep to pace the playback
        usleep(20000);
        
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
    window = SDL_CreateWindow("Preview", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, codec_ctx->width, codec_ctx->height, 0);
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


int main() {
    
    const char *file = "onmyway.mp4";
    const char *outfile = "test.yuv";
    
    int ret = 0;
    int videoFPS = 0;
    int firstFrame = 1;
    int VideoStreamIndex = -1;
    double frameDuration = 0;
    
    FILE *fin = NULL;
    FILE *fout = NULL;
    time_t timeNow;
    time_t lastTime;
    AVCodec *Codec = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    AVCodecContext *codecCtx = NULL;
    AVFormatContext *fmt_ctx = NULL;
    
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
    
    // init sdl
    if (initSDL(codecCtx) < 0)
    {
        av_log(NULL, AV_LOG_ERROR, "init sdl failed\n");
        cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
        return -11;
    }
    
    
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
            decode(codecCtx, frame, pkt, fout, frameDuration, lastTime, timeNow, firstFrame);
            
        }
        
        // release packet buffers to be allocated again
        av_packet_unref(pkt);
        
    }
    
    //flush decoder
    decode(codecCtx, frame, pkt, fout, frameDuration, lastTime, timeNow, firstFrame);
    
    cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
    
    return 0;
}

