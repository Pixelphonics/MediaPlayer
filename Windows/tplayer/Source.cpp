#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <windows.h>
#include <ctime>
#include <libportaudio/portaudio.h>
#include <libsndfile/sndfile.h>
#include <sys/types.h>

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
void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt, FILE *f, double frameDuration, time_t &lastTime, time_t &timeNow, int &firstFrame);
void displayFrame(AVFrame * frame, AVCodecContext *dec_ctx);
int initSDL(AVCodecContext *codec_ctx);
DWORD WINAPI playVideo(void* data);
DWORD WINAPI playAudio(void* data);

SDL_Renderer *renderer;
SDL_Texture *texture;
SDL_Rect r;
int timeElapsed;
int timeDifference;



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
		
		
		printf("%d  ", frame->pts);

		localtime(&timeNow);
		if (!firstFrame) {
			timeDifference = difftime(lastTime, timeNow);
			if ((frame->pts - timeElapsed) > timeDifference) {
				Sleep(frame->pts - timeElapsed - timeDifference - 4);//not sure where this 4ms delay is from, localtime and thread time different? 
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

DWORD WINAPI playVideo(void* data) {
	const char *file = "00_Video.mp4";
	const char *outfile = "00_Video.yuv";

	int ret = 0;
	int videoFPS = 0;
	int firstFrame = 1;
	int VideoStreamIndex = -1;
	double frameDuration = 0;
	timeElapsed = 0;

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


// Port Audio functions
static int paStreamCallback(const void *inputBuffer, void *outputBuffer,
	unsigned long framesPerBuffer,
	const PaStreamCallbackTimeInfo* timeInfo,
	PaStreamCallbackFlags statusFlags,
	void *userData)
{
	paData *data = (paData*)userData;
	float **out = (float**)outputBuffer;
	unsigned long j;
	sf_count_t num_read;

	(void)timeInfo; /* Prevent unused variable warnings. */
	(void)statusFlags;
	(void)inputBuffer;


	for (j = 0; j < NUM_CHANNELS; ++j) {

		/* clear output buffer */
		memset(out[j], 0, sizeof(float) * framesPerBuffer * data->info[j].channels);

		/* read directly into output buffer */
		num_read = sf_read_float(data->file[j], out[j], framesPerBuffer * data->info[j].channels);

		/* If we couldn't read a full frameCount of samples we've reached EOF */
		if (num_read < framesPerBuffer)
		{
			return paComplete;
		}
	}

	return paContinue;
}

DWORD WINAPI playAudio(void* a) {
	// Pa variables
	PaStreamParameters outputParameters;
	PaStream *stream;
	PaError err;
	paData data;

	//Utility variables
	int i = 0;

	// File IO variables
	const char* path = "./"; ///Users/allanhsu/Downloads/Audio WAV and Quicktime Video Files
	int opened = 0;

	DIR *dp;
	struct dirent *ep;

	dp = opendir(path);

	if (dp == NULL) {
		printf("can't open directory.");
		return 1;
	}

	while ((ep = readdir(dp))) {
		printf("filename: %s\n", ep->d_name);
		if (strstr(ep->d_name, ".wav") != NULL) {
			char* filePath;

			asprintf(&filePath, "%s%s", path, ep->d_name);

			/* opening first 32 files and read info */
			if (i < NUM_CHANNELS) {
				data.file[i] = sf_open(filePath, SFM_READ, &data.info[i]);

				free(filePath);
				if (sf_error(data.file[i]) != SF_ERR_NO_ERROR) {
					fprintf(stderr, "%s\n", sf_strerror(data.file[0]));
					fprintf(stderr, "Failed to open: %s\n", ep->d_name);
					return 1;
				}
				++opened;
				printf("file: %s opened successfully.\n", ep->d_name);
				++i;
			}

		}
	}

	(void)closedir(dp);

	if (opened < NUM_CHANNELS) {
		for (i = 0; i < opened; ++i) {
			sf_close(data.file[i]);
		}
		printf("need at least %d wav files.", NUM_CHANNELS);
		getchar();
		return 1;
	}

	err = Pa_Initialize();
	if (err != paNoError) goto error;

	printf("Pa Initialized... \n");

	outputParameters.hostApiSpecificStreamInfo = NULL;

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
		Pa_GetDeviceInfo(outputParameters.device)->defaultSampleRate,
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
	for (i = 0; i < opened; ++i) {
		sf_close(data.file[i]);
	}

	/*  Shuts down portaudio */
	err = Pa_CloseStream(stream);
	if (err != paNoError) goto error;

	Pa_Terminate();
	if (err != paNoError) goto error;

	return 0;
error:
	Pa_Terminate();
	fprintf(stderr, "An error occured while using the portaudio stream\n");
	fprintf(stderr, "Error number: %d\n", err);
	fprintf(stderr, "Error message: %s\n", Pa_GetErrorText(err));

	printf("Enter any key to exit program.");
	getchar();

	return err;
}

int main() {
	
	HANDLE thread = CreateThread(NULL, 0, playVideo, NULL, 0, NULL);
	HANDLE thread2 = CreateThread(NULL, 0, playAudio, NULL, 0, NULL);

	WaitForSingleObject(thread, INFINITE);
	WaitForSingleObject(thread2, INFINITE);
	
	printf("Playback completes!\n");
	printf("Enter any key to exit program.");
	getchar();
	
	return 0;
}
