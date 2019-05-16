#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE

#include <stdio.h>
#include <windows.h>
#include <ctime>
#include <libportaudio/portaudio.h>
#include <libsndfile/sndfile.h>
#include <sys/types.h>
#include <SDLttf/SDL_ttf.h>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <sdl/SDL.h>
}

#include "dirent.h"
#include "asprintf.h"

#define NUM_SECONDS   (5)
#define FRAMES_PER_BUFFER  (512)
#define NUM_CHANNELS (2)

typedef struct
{
	SNDFILE *file[NUM_CHANNELS];
	SF_INFO info[NUM_CHANNELS];
}
paData;


#undef main

void cleanup(AVFormatContext *fmt_ctx, AVCodecContext *CodecCtx, FILE *fin, FILE *fout, AVFrame *frame, AVPacket *pkt);
void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize, FILE *f);
void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt, FILE *f, double frameDuration, time_t &lastTime, time_t &timeNow, int &firstFrame, double &timeBase);
void displayFrame(AVFrame * frame, AVCodecContext *dec_ctx);
int initSDL(AVCodecContext *codec_ctx);
DWORD WINAPI playVideo(void* data);
DWORD WINAPI playAudio(void* data);

SDL_Renderer *renderer;
SDL_Texture *texture;
SDL_Rect r;
int timeElapsed;
int timeDifference;
double timeBase;
float volume = 100;
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



void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt, FILE *f, double frameDuration, time_t &lastTime, time_t &timeNow, int &firstFrame, double &timeBase)
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


		printf("%d  ", frame->pts);
		if (frame->pts == frame->pkt_dts) {
			printf("%d  ", frame->pkt_dts);
		}
		printf("%f  ", timeBase);
		localtime(&timeNow);
		if (!firstFrame) {
			timeDifference = difftime(lastTime, timeNow);
			if ((frame->pts - timeElapsed) > timeDifference) {
				Sleep((frame->pts - timeElapsed - timeDifference)  * timeBase * 1000 - 4);//not sure where this 4ms delay is from, localtime and thread time different? 
			}
		}

		firstFrame = 0;
		localtime(&lastTime);

		timeElapsed = frame->pts;

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

DWORD WINAPI playVideo(void* data) {
	const char *file = "media/demo video.mp4";
	const char *outfile = "media/demo video.yuv";

	int ret = 0;
	int videoFPS = 0;
	int firstFrame = 1;
	int VideoStreamIndex = -1;
	double frameDuration = 0;
	timeElapsed = 0;
	timeBase = 0.001;

	FILE *fin = NULL;
	FILE *fout = NULL;
	time_t timeNow;
	time_t lastTime;
	AVCodec *Codec = NULL;
	AVFrame *frame = NULL;
	AVPacket *pkt = NULL;
	AVCodecContext *codecCtx = NULL;
	AVFormatContext *fmt_ctx = NULL;

	if (ret = avformat_open_input(&fmt_ctx, file, NULL, NULL) < 0) {
		av_log(NULL, AV_LOG_ERROR, "cannot open input file\n");
		cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);
		return -1;
	}


	if (ret = avformat_find_stream_info(fmt_ctx, NULL) < 0) {
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
	if (ret = avcodec_parameters_to_context(codecCtx, fmt_ctx->streams[VideoStreamIndex]->codecpar) < 0)
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
	if (ret = avcodec_open2(codecCtx, Codec, NULL) < 0)
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
		if (ret = av_read_frame(fmt_ctx, pkt) < 0)
		{
			av_log(NULL, AV_LOG_ERROR, "cannot read frame");
			break;
		}
		// if packet data is video data then send it to decoder
		if (pkt->stream_index == VideoStreamIndex)
		{
			decode(codecCtx, frame, pkt, fout, frameDuration, lastTime, timeNow, firstFrame, timeBase);
		}

		// release packet buffers to be allocated again
		av_packet_unref(pkt);

	}

	//flush decoder
	decode(codecCtx, frame, pkt, fout, frameDuration, lastTime, timeNow, firstFrame, timeBase);

	cleanup(fmt_ctx, codecCtx, fin, fout, frame, pkt);

	return 0;
}


// Port Audio functions
static int paStreamCallback(const void *inputBuffer, void *outputBuffer,
	unsigned long framesPerBuffer,
	const PaStreamCallbackTimeInfo* timeInfo,
	PaStreamCallbackFlags statusFlags,
	void *userData)
{
	float **out;
	paData *p_data = (paData*)userData;
	sf_count_t num_read;

	out = (float**)outputBuffer;
	p_data = (paData*)userData;

	for (int i = 0; i < NUM_CHANNELS; i++) {
		/* clear output buffer */
		memset(out[i], 0, sizeof(float) * framesPerBuffer * p_data->info[i].channels);

		/* read directly into output buffer */
		num_read = sf_read_float(p_data->file[i], out[i], framesPerBuffer * p_data->info[i].channels);

		/* adjust the volume of the output */
		for (int j = 0; j < framesPerBuffer; j++) {
			out[i][j] *= (volume / 100);
		}

		/*  If we couldn't read a full frameCount of samples we've reached EOF */
		if (num_read < framesPerBuffer) {
			return paComplete;
		}
	}

	return paContinue;
}

DWORD WINAPI playAudio(void* a) {
	PaStream *stream;
	PaError error;
	paData data;

	/* Open the soundfile */
	for (int i = 0; i < NUM_CHANNELS; i++) {
		char file[13] = "media/";
		char fileNum[4];
		itoa(i + 1, fileNum, 10);
		strcat_s(file, 13, fileNum);
		strcat_s(file, 13, ".wav");
		data.file[i] = sf_open(file, SFM_READ, &data.info[i]);
		if (sf_error(data.file[i]) != SF_ERR_NO_ERROR) {
			fprintf(stderr, "%s\n", sf_strerror(data.file[i]));
			fprintf(stderr, "File: %s\n", file);
			return 1;
		}
	}

	/* init portaudio */
	error = Pa_Initialize();
	if (error != paNoError) {
		fprintf(stderr, "Problem initializing\n");
		return 1;
	}

	/* Open PaStream with values read from the file */
	error = Pa_OpenDefaultStream(&stream, 0, NUM_CHANNELS, paFloat32 | paNonInterleaved, data.info[0].samplerate, FRAMES_PER_BUFFER, paStreamCallback, &data);
	if (error != paNoError) {
		fprintf(stderr, "Problem opening Default Stream\n");
		return 1;
	}

	/* Start the stream */
	error = Pa_StartStream(stream);
	if (error != paNoError) {
		fprintf(stderr, "Problem opening starting Stream\n");
		return 1;
	}

	/* Run until EOF is reached */
	while (Pa_IsStreamActive(stream)) {
		Pa_Sleep(100);
	}

	/* Close the soundfile */
	for (int i = 0; i < NUM_CHANNELS; i++) {
		sf_close(data.file[i]);
	}

	/*  Shut down portaudio */
	error = Pa_CloseStream(stream);
	if (error != paNoError) {
		fprintf(stderr, "Problem closing stream\n");
		return 1;
	}

	error = Pa_Terminate();
	if (error != paNoError) {
		fprintf(stderr, "Problem terminating\n");
		return 1;
	}

	return 0;
}

int main() {

	SDL_Window *initWindow = NULL;
	SDL_Renderer* renderer = NULL;
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
	SDL_Surface* playTextSurface = NULL;
	SDL_Surface* volUpTextSurface = NULL;
	SDL_Surface* volDownTextSurface = NULL;
	SDL_Surface* volTextSurface = NULL;
	SDL_Color textColor = { 0, 0, 0 };
	char volumeText[4];

	/* Init SDL */
	if (SDL_Init(SDL_INIT_VIDEO) < 0)
	{
		fprintf(stderr, "could not init sdl %s\n", SDL_GetError());
		return -1;
	}

	/* Init SDL TTF */
	if (TTF_Init() == -1) {
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
	font = TTF_OpenFont("font/font.ttf", 36);
	if (!font) {
		fprintf(stderr, "could not create sdl font \n");
		return -1;
	}

	/* Set up text surface and text texture for each text entry */
	playTextSurface = TTF_RenderText_Solid(font, "Play", textColor);
	if (!playTextSurface) {
		fprintf(stderr, "could not create sdl text surface \n");
	}

	playTextTexture = SDL_CreateTextureFromSurface(renderer, playTextSurface);
	if (!playTextTexture) {
		fprintf(stderr, "could not create sdl text texture \n");
	}

	volUpTextSurface = TTF_RenderText_Solid(font, "+", textColor);
	if (!volUpTextSurface) {
		fprintf(stderr, "could not create sdl text surface \n");
	}

	volUpTextTexture = SDL_CreateTextureFromSurface(renderer, volUpTextSurface);
	if (!volUpTextTexture) {
		fprintf(stderr, "could not create sdl text texture \n");
	}

	volDownTextSurface = TTF_RenderText_Solid(font, "-", textColor);
	if (!volDownTextSurface) {
		fprintf(stderr, "could not create sdl text surface \n");
	}

	volDownTextTexture = SDL_CreateTextureFromSurface(renderer, volDownTextSurface);
	if (!volDownTextTexture) {
		fprintf(stderr, "could not create sdl text texture \n");
	}

	/* Set the volume text */
	itoa(volume, volumeText, 10);

	volTextSurface = TTF_RenderText_Solid(font, volumeText, textColor);
	if (!volTextSurface) {
		fprintf(stderr, "could not create sdl text surface \n");
	}

	volTextTexture = SDL_CreateTextureFromSurface(renderer, volTextSurface);
	if (!volTextTexture) {
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

	while (initWindow) {
		if (SDL_WaitEvent(&event)) {
			switch (event.type) {
			case SDL_QUIT:
				initWindow = NULL;
				break;
			case SDL_MOUSEBUTTONDOWN:
				if (event.button.button == SDL_BUTTON_LEFT) {
					int x = event.button.x;
					int y = event.button.y;

					/* The play button is clicked */
					if ((x > play.x) && (x < play.x + play.w) && (y > play.y) && (y < play.y + play.h)) {
						/* Destroy the play window before creating the video window */
						SDL_GetWindowPosition(initWindow, &positionX, &positionY);
						SDL_DestroyWindow(initWindow);
						SDL_DestroyRenderer(renderer);
						initWindow = NULL;

						/* Create the video and audio threads */
						HANDLE thread = CreateThread(NULL, 0, playVideo, NULL, 0, NULL);
						HANDLE thread2 = CreateThread(NULL, 0, playAudio, NULL, 0, NULL);

						WaitForSingleObject(thread, INFINITE);
						WaitForSingleObject(thread2, INFINITE);
					}
					/* The volume up button is clicked */
					else if ((x > volUp.x) && (x < volUp.x + volUp.w) && (y > volUp.y) && (y < volUp.y + volUp.h) && (volume < 100)) {
						volume++;
						itoa(volume, volumeText, 10);

						volTextSurface = TTF_RenderText_Solid(font, volumeText, textColor);
						if (!volTextSurface) {
							fprintf(stderr, "could not create sdl text surface \n");
						}

						volTextTexture = SDL_CreateTextureFromSurface(renderer, volTextSurface);
						if (!volTextTexture) {
							fprintf(stderr, "could not create sdl text texture \n");
						}

						SDL_QueryTexture(volTextTexture, NULL, NULL, &vol.w, &vol.h);

						if (volume == 100) {
							vol.x = 35;
						}
						else {
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
					else if ((x > volDown.x) && (x < volDown.x + volDown.w) && (y > volDown.y) && (y < volDown.y + volDown.h) && (volume > 0)) {
						volume--;
						itoa(volume, volumeText, 10);

						volTextSurface = TTF_RenderText_Solid(font, volumeText, textColor);
						if (!volTextSurface) {
							fprintf(stderr, "could not create sdl text surface \n");
						}

						volTextTexture = SDL_CreateTextureFromSurface(renderer, volTextSurface);
						if (!volTextTexture) {
							fprintf(stderr, "could not create sdl text texture \n");
						}

						SDL_QueryTexture(volTextTexture, NULL, NULL, &vol.w, &vol.h);

						if (volume == 100) {
							vol.x = 35;
						}
						else {
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
