// http_media_player.cpp: 定义控制台应用程序的入口点。
//


#include <windows.h>

#include <stdlib.h>
#include <stdio.h>
#include <time.h>


#ifdef _WIN32
//Windows
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#include "libswresample/swresample.h"
#include "SDL2/SDL.h"
#include "SDL2/SDL_thread.h"
};
#else
//Linux...
#ifdef __cplusplus
extern "C"
{
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <SDL2/SDL.h>
#ifdef __cplusplus
};
#endif
#endif


#define LOGI(FORMAT,...)   printf(FORMAT,##__VA_ARGS__)  //__android_log_print(ANDROID_LOG_INFO,"ywl5320",FORMAT,##__VA_ARGS__);
#define LOGE(FORMAT,...)   printf(FORMAT,##__VA_ARGS__)  //__android_log_print(ANDROID_LOG_ERROR,"ywl5320",FORMAT,##__VA_ARGS__);
#define ALOGD(FORMAT,...)   printf(FORMAT,##__VA_ARGS__)  //__android_log_print(ANDROID_LOG_ERROR,"ywl5320",FORMAT,##__VA_ARGS__);



static const int SDL_AUDIO_BUFFER_SIZE = 1024;
static const int MAX_AUDIO_FRAME_SIZE = 192000;




typedef struct PacketQueue
{
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex *mutex;
	SDL_cond *cond;
}PacketQueue;

static int quit = 0;
static int pause = 0;

static PacketQueue audioq;
static PacketQueue videoq;

void packet_queue_init(PacketQueue *q) {
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}


int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

	AVPacketList *pkt1;
	if (av_dup_packet(pkt) < 0) {
		return -1;
	}
	pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = NULL;

	SDL_LockMutex(q->mutex);

	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size;
	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);
	return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block) {
	AVPacketList *pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for (;;) {

		//暂停则取不到
		if (pause) {
			ret = -1;
			break;
		}

		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		}
		else if (!block) {
			ret = 0;
			break;
		}
		else {
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}

int getQueueSize(PacketQueue *q)
{
	return q->nb_packets;
}

static int64_t  play_pts = 0;

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {

	AVFrame *frame = av_frame_alloc();
	int data_size = 0;
	AVPacket pkt;
	int got_frame_ptr;

	SwrContext *swr_ctx;

	//	if (quit)
	//		return -1;

	if (packet_queue_get(&audioq, &pkt, 1) < 0)
		return -1;

	play_pts = pkt.pts;
	

	//printf("play audio pts %lld\n", play_pts);

	int ret = avcodec_decode_audio4(aCodecCtx, frame, &got_frame_ptr, &pkt);

	//int ret = avcodec_send_packet(aCodecCtx, &pkt);
	if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
		return -1;

	//ret = avcodec_receive_frame(aCodecCtx, frame);
	//if (ret < 0 && ret != AVERROR_EOF)
	//	return -1;

	// 设置通道数或channel_layout
	if (frame->channels > 0 && frame->channel_layout == 0)
		frame->channel_layout = av_get_default_channel_layout(frame->channels);
	else if (frame->channels == 0 && frame->channel_layout > 0)
		frame->channels = av_get_channel_layout_nb_channels(frame->channel_layout);

	enum AVSampleFormat dst_format = AV_SAMPLE_FMT_S16;//av_get_packed_sample_fmt((AVSampleFormat)frame->format);

													   //重采样为立体声
	Uint64 dst_layout = AV_CH_LAYOUT_STEREO;
	// 设置转换参数
	swr_ctx = swr_alloc_set_opts(NULL, dst_layout, dst_format, frame->sample_rate,
		frame->channel_layout, (enum AVSampleFormat)frame->format, frame->sample_rate, 0, NULL);
	if (!swr_ctx || swr_init(swr_ctx) < 0)
		return -1;

	// 计算转换后的sample个数 a * b / c
	int dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, frame->sample_rate) + frame->nb_samples, frame->sample_rate, frame->sample_rate, (AVRounding)1);
	// 转换，返回值为转换后的sample个数
	int nb = swr_convert(swr_ctx, &audio_buf, dst_nb_samples, (const uint8_t**)frame->data, frame->nb_samples);

	//根据布局获取声道数
	int out_channels = av_get_channel_layout_nb_channels(dst_layout);
	data_size = out_channels * nb * av_get_bytes_per_sample(dst_format);

	av_frame_free(&frame);
	swr_free(&swr_ctx);
	return data_size;
}

#define AVCODEC_MAX_AUDIO_FRAME_SIZE 192000

void audio_callback(void *userdata, Uint8 *stream, int len) {

	AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;

	int len1, audio_size;

	static uint8_t audio_buff[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

	SDL_memset(stream, 0, len);
	if (getQueueSize(&audioq) > 0)
	{

		//	LOGI("audio pkt nums: %d    queue size: %d\n", audioq.nb_packets, audioq.size);
		while (len > 0)// 想设备发送长度为len的数据
		{
			if (audio_buf_index >= audio_buf_size) // 缓冲区中无数据
			{
				// 从packet中解码数据
				audio_size = audio_decode_frame(aCodecCtx, audio_buff, sizeof(audio_buff));
				if (audio_size < 0) // 没有解码到数据或出错，填充0
				{
					audio_buf_size = 0;
					memset(audio_buff, 0, audio_buf_size);
				}
				else
					audio_buf_size = audio_size;

				audio_buf_index = 0;
			}
			len1 = audio_buf_size - audio_buf_index; // 缓冲区中剩下的数据长度
			if (len1 > len) // 向设备发送的数据长度为len
				len1 = len;

			SDL_MixAudio(stream, audio_buff + audio_buf_index, len, SDL_MIX_MAXVOLUME);

			len -= len1;
			stream += len1;
			audio_buf_index += len1;
		}
	}
	/*	else
	{
	LOGI("pkt nums: %d    queue size: %d\n", audioq.nb_packets, audioq.size);
	LOGI("play complete");
	SDL_CloseAudio();
	SDL_Quit();
	}
	*/
}

static AVFormatContext* pFormatCtx = NULL;


static AVCodecContext* pCodecCtx = NULL;
static AVCodec* pCodec = NULL;

static AVCodecContext* pCodecCtx_audio = NULL;
static AVCodec* pCodec_audio = NULL;
static int videoStream = -1;
static int audioStream = -1;
static long globle_seek_pos = 0;
static bool globle_pause_statue = false;



int play_video_thread()
{
	AVFrame* pFrame = NULL;
	AVFrame* pFrameYUV = NULL;

	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();

	// open codec
	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0)
		return -1; // Could open codec



				   ///////////////////////////////////////////////////////////////////////////
				   //
				   // SDL2.0
				   //
				   //////////////////////////////////////////////////////////////////////////

	SDL_Window* window = SDL_CreateWindow("FFmpeg Decode", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		pCodecCtx->width, pCodecCtx->height, SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS | SDL_WINDOW_MAXIMIZED);
	SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);

	/*	SDL_Texture* bmp = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
	pCodecCtx->width, pCodecCtx->height);
	*/
	int sdl_w, sdl_h;
	SDL_GetWindowSize(window, &sdl_w, &sdl_h);



	SDL_Texture* bmp = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING,
		sdl_w, sdl_h);

	SDL_Rect rect;
	SDL_Rect out_rect;

	double Zoom_Rate_w = 0;
	double Zoom_Rate_h = 0;

	//如果需要居中
	if (sdl_w > pCodecCtx->width)
	{
		rect.x = (sdl_w - pCodecCtx->width) / 2;
		out_rect.w = pCodecCtx->width;// sdl_w - rect.x * 2;
		Zoom_Rate_w = double(sdl_w) / pCodecCtx->width;
	}
	else
	{
		rect.x = 0;
		out_rect.w = sdl_w;
	}

	if (sdl_h > pCodecCtx->height)
	{
		rect.y = (sdl_h - pCodecCtx->height) / 2;
		out_rect.h = pCodecCtx->height;// sdl_h - rect.y * 2;
		Zoom_Rate_h = double(sdl_h) / pCodecCtx->height;
	}
	else
	{
		rect.y = 0;
		out_rect.h = sdl_h;
	}

	if (Zoom_Rate_h > 1 && Zoom_Rate_w > 1)
	{
		//满足放大条件
		if (Zoom_Rate_h > Zoom_Rate_w)
		{
			rect.w = out_rect.w * Zoom_Rate_w; // pCodecCtx->width;// rect.x;
			rect.h = out_rect.h * Zoom_Rate_w;// pCodecCtx->height;
		}
		else
		{
			rect.w = out_rect.w * Zoom_Rate_h; // pCodecCtx->width;// rect.x;
			rect.h = out_rect.h * Zoom_Rate_h;// pCodecCtx->height;
		}

		rect.x = (sdl_w - rect.w) / 2;
		rect.y = (sdl_h - rect.h) / 2;
	}
	else
	{

		rect.w = out_rect.w; // pCodecCtx->width;// rect.x;
		rect.h = out_rect.h;// pCodecCtx->height;
	}


	int curretn_width = pCodecCtx->width;


	int numBytes = 0;
	uint8_t* buffer = NULL;

	numBytes = avpicture_get_size(AV_PIX_FMT_YUV420P, rect.w, rect.h);
	buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));

	avpicture_fill((AVPicture*)pFrameYUV, buffer, AV_PIX_FMT_YUV420P, rect.w, rect.h);
	struct SwsContext* sws_ctx = NULL;
	sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
		rect.w, rect.h, AV_PIX_FMT_YUV420P, SWS_BICUBIC,/*SWS_BILINEAR,*/ NULL, NULL, NULL);



	AVPacket packet;

	DWORD tick;
	int64_t frame_num = 0;
	int wait_times_ms = 0;

	wait_times_ms = av_q2d(pFormatCtx->streams[videoStream]->time_base);


	printf("wait_times_ms %d\n", wait_times_ms);


	while (1)
	{
		//如果开始seek，先暂停播放
		while (globle_seek_pos != 0)
		{
			globle_pause_statue = true;
			SDL_Delay(1);
		};

		if (packet_queue_get(&videoq, &packet, 1) > 0)
		{
			int frameFinished = 0;

			avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
			if (frameFinished)
			{
				//播得快了，要等, 如果没声音的视频，就不用等了
				if (audioStream != -1)
				{
					while (pFrame->pkt_pts != 0 && pFrame->pkt_pts > play_pts)
					{
						//ALOGD("wait packet.pts %d , play_pts %d\n", pFrame->pkt_pts, play_pts);
						SDL_Delay(1);
					};
				}


				if (curretn_width != pCodecCtx->width)
				{
#if 1
					printf("pFrame->width %d  pCodecCTx->width %d\n", pFrame->width, pCodecCtx->width);

					av_free(buffer);
					sws_freeContext(sws_ctx);

					//如果需要居中
					if (sdl_w > pCodecCtx->width)
					{
						rect.x = (sdl_w - pCodecCtx->width) / 2;
						out_rect.w = pCodecCtx->width;// sdl_w - rect.x * 2;
						Zoom_Rate_w = double(sdl_w) / pCodecCtx->width;
					}
					else
					{
						rect.x = 0;
						out_rect.w = sdl_w;
					}

					if (sdl_h > pCodecCtx->height)
					{
						rect.y = (sdl_h - pCodecCtx->height) / 2;
						out_rect.h = pCodecCtx->height;// sdl_h - rect.y * 2;
						Zoom_Rate_h = double(sdl_h) / pCodecCtx->height;
					}
					else
					{
						rect.y = 0;
						out_rect.h = sdl_h;
					}

					if (Zoom_Rate_h > 1 && Zoom_Rate_w > 1)
					{
						//满足放大条件
						if (Zoom_Rate_h > Zoom_Rate_w)
						{
							rect.w = out_rect.w * Zoom_Rate_w; // pCodecCtx->width;// rect.x;
							rect.h = out_rect.h * Zoom_Rate_w;// pCodecCtx->height;
						}
						else
						{
							rect.w = out_rect.w * Zoom_Rate_h; // pCodecCtx->width;// rect.x;
							rect.h = out_rect.h * Zoom_Rate_h;// pCodecCtx->height;
						}

						rect.x = (sdl_w - rect.w) / 2;
						rect.y = (sdl_h - rect.h) / 2;
					}
					else
					{

						rect.w = out_rect.w; // pCodecCtx->width;// rect.x;
						rect.h = out_rect.h;// pCodecCtx->height;
					}


					numBytes = avpicture_get_size(AV_PIX_FMT_YUV420P, rect.w, rect.h);
					buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));

					avpicture_fill((AVPicture*)pFrameYUV, buffer, AV_PIX_FMT_YUV420P, rect.w, rect.h);
					sws_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
						rect.w, rect.h, AV_PIX_FMT_YUV420P, SWS_BICUBIC,/*SWS_BILINEAR,*/ NULL, NULL, NULL);
#endif
					curretn_width = pCodecCtx->width;
					//	avcodec_flush_buffers(pCodecCtx);

				}

				//	if (globle_seek_pos)
				//		continue;

				//此处要得到实际帧率来判断
				//另外要考虑网速缓冲卡住的情况？
				/*	if (frame_num == 0)
				{
				tick = GetTickCount();
				}
				else
				{
				while (GetTickCount() - tick < frame_num * wait_times_ms)
				{
				SDL_Delay(1);
				}
				}
				frame_num++;
				*/

				//播慢了，要追， 就不等了
				if (audioStream != -1)
				{
					if (pFrame->pkt_pts != 0 && pFrame->pkt_pts > play_pts)
						SDL_Delay(wait_times_ms - 1);
				}

				sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data, pFrame->linesize, 0,
					pCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);

				//SDL_UpdateTexture(bmp, &rect, pFrameYUV->data[0], pFrameY UV->linesize[0]);
				SDL_UpdateYUVTexture(bmp, &rect, //NULL,//
					pFrameYUV->data[0], pFrameYUV->linesize[0],
					pFrameYUV->data[1], pFrameYUV->linesize[2],
					pFrameYUV->data[2], pFrameYUV->linesize[1]);
				SDL_RenderClear(renderer);
				SDL_RenderCopy(renderer, bmp, &rect, &rect);  //NULL, NULL); //
				SDL_RenderPresent(renderer);
				//	SDL_Delay(39);
			}
		}
		else
		{
			Sleep(1);
			frame_num = 0;

			//先退出吧, 
			//break;
		}


		if (quit)
			break;
	}

	printf("%s exit!!\n", __FUNCTION__);

	//清除
	while (packet_queue_get(&videoq, &packet, 0) > 0);

	av_free(buffer);
	av_frame_free(&pFrame);
	av_frame_free(&pFrameYUV);

	sws_freeContext(sws_ctx);
	avcodec_close(pCodecCtx);

	SDL_DestroyTexture(bmp);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
}

int play_process()
{
	packet_queue_init(&videoq);

	AVCodecContext  *pCodecCtx = pCodecCtx_audio;
	AVCodec *pCodec = pCodec_audio;

	if (pCodecCtx_audio)
	{
		// Set audio settings from codec info
		SDL_AudioSpec wanted_spec, spec;
		wanted_spec.freq = pCodecCtx->sample_rate;
		wanted_spec.format = AUDIO_S16SYS;
		wanted_spec.channels = pCodecCtx->channels;
		wanted_spec.silence = 0;
		wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
		wanted_spec.callback = audio_callback;
		wanted_spec.userdata = pCodecCtx;

		if (SDL_OpenAudio(&wanted_spec, &spec) < 0)
		{
			LOGE("Open audio failed:");
			audioStream = -1;
			return -1;
		}

		LOGI("come here...");

		avcodec_open2(pCodecCtx, pCodec, NULL);

		SDL_PauseAudio(0);
	}

	AVPacket packet;
	int pause_state = 0;
	while (av_read_frame(pFormatCtx, &packet) >= 0)
	{
		//	

		//	printf("get frame stream %d\n", packet.stream_index);
		//如果下载太快，播的慢，就等下再下载
		while (getQueueSize(&audioq) > 100)
		{
			SDL_Delay(1);
		}

		if (globle_seek_pos && globle_pause_statue)
		{
			SDL_PauseAudio(1);

		
			int seekFlag = avformat_seek_file(pFormatCtx, audioStream, play_pts - 1000 + globle_seek_pos * 1000,
				play_pts + globle_seek_pos * 1000, play_pts + globle_seek_pos *1000 + 1000, AVSEEK_FLAG_FRAME);// AVSEEK_FLAG_ANY);

			if (seekFlag == 0)
			{
				avformat_seek_file(pFormatCtx, videoStream, play_pts - 1000 + globle_seek_pos * 1000,
					play_pts + globle_seek_pos * 1000, play_pts + globle_seek_pos * 1000 + 1000, AVSEEK_FLAG_FRAME);// AVSEEK_FLAG_ANY);

				avcodec_flush_buffers(pCodecCtx);
				avcodec_flush_buffers(pCodecCtx_audio);

				//清空队列
				while (packet_queue_get(&videoq, &packet, 0) > 0);
				while (packet_queue_get(&audioq, &packet, 0) > 0);
			}

			SDL_PauseAudio(0);

			printf("seek Flag %d\n", seekFlag);
			globle_seek_pos = 0;
			globle_pause_statue = false;

		}
		if (packet.stream_index == videoStream)
		{
			packet_queue_put(&videoq, &packet);
			ALOGD("video  packet.pts %d  \n", packet.pts);
		}
		else if (packet.stream_index == audioStream)
		{
			packet_queue_put(&audioq, &packet);
			ALOGD("audio  packet.pts %d  \n", packet.pts);
		}
		else
		{
			av_packet_unref(&packet);
		}



		if (pause)
		{
			SDL_PauseAudio(1);
			while (pause)
			{
				Sleep(10);
			}
			SDL_PauseAudio(0);
		}


		if (quit)
			break;

		Sleep(5);

	}




	//这里等播完
	while (getQueueSize(&audioq) > 0)
	{
		Sleep(100);
	}
	while (getQueueSize(&videoq) > 0);
	{
		Sleep(100);
	}

	printf("%s exit \n", __FUNCTION__);


	SDL_CloseAudio();
	SDL_Quit();

	avcodec_close(pCodecCtx);
	return 0;
}



static HANDLE hVideo_Thread = NULL;			//video 线程句柄
static HANDLE hAudio_Thread = NULL;			//audio 服务线程句柄	
static HANDLE hPlay_Thread = NULL;			//audio 服务线程句柄	



#undef main 
int main(int argc, char **argv)
{
	printf("http media player lunch!\n");

	//1.注册支持的文件格式及对应的codec
	av_register_all();
	avformat_network_init();


	unsigned version = avformat_version();

#define AV_VERSION_MAJOR(a) ((a) >> 16)
#define AV_VERSION_MINOR(a) (((a) & 0x00FF00) >> 8)
#define AV_VERSION_MICRO(a) ((a) & 0xFF)

	printf("avformat_version  %d.%d.%d \n", AV_VERSION_MAJOR(version), AV_VERSION_MINOR(version), AV_VERSION_MICRO(version));



	//char* filenName = "http://live.g3proxy.lecloud.com/gslb?stream_id=lb_yxlm_1800&tag=live&ext=m3u8&sign=live_tv&platid=10&splatid=1009";
	//char *filenName = "jxtg3.mkv";
	//char filename[] = "http://112.29.209.240/moviets.tc.qq.com/AHHA7ZWYwLrSdkVd5y0aF1RZTHHRWW9RjJJNrgLZjbGs/q5o5J-45JGPX-1tS9h2qm30ttKvIMyScJSiOEkvUWZo7fdpk-_kixNmjtH43recqduW6cPrXQxdQAzJEUPrKZ2SxAFmBFWgHVXPhzK_Ki1F-7aJ0T5kV3Q/h0023dqy3l8.321002.ts.m3u8?ver=4&sdtfrom=v3000&platform=10403&appver=6.0.6";
	//char filename[] = "http://172.31.23.58/blue7.mp4";
	//char filename[] = "http://172.31.23.58/20mbps.mp4";
	//char filename[] = "http://172.31.23.58/6.mkv";
	//char filename[] = "http://172.31.23.58/video.mkv";
	//char filename[] = "rtsp://172.31.23.58:8554/";
	//char filename[] = "http://172.31.23.58/pipe.mp4";
	char filename[] = "http://192.168.31.78/men.rmvb";
	//	char filename[] = "http://192.168.1.165:7001/1/a0d7fa33-a1c6-57e9-9823-9869a8c48d45.mp4";
	// 2.打开video文件
	// 读取文件头，将格式相关信息存放在AVFormatContext结构体中
	if (avformat_open_input(&pFormatCtx, filename, NULL, NULL) != 0)
		return -1; // 打开失败

				   // 检测文件的流信息
	if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
		return -1; // 没有检测到流信息 stream infomation

				   // 在控制台输出文件信息
	av_dump_format(pFormatCtx, 0, filename, 0);

	//查找第一个视频流 video stream
	int i = 0;
	for (i = 0; i < pFormatCtx->nb_streams; i++)
	{
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			videoStream = i;
		}
		else if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			audioStream = i;
		}

	}

	if (videoStream == -1)
		return -1; // 没有查找到视频流video stream


	AVCodecContext* pCodecCtxOrg = NULL;

	pCodecCtxOrg = pFormatCtx->streams[videoStream]->codec; // codec context
	
															// 找到video stream的 decoder
	pCodec = avcodec_find_decoder(pCodecCtxOrg->codec_id);
	if (!pCodec)
		return -1;
	

	// 不直接使用从AVFormatContext得到的CodecContext，要复制一个
	pCodecCtx = avcodec_alloc_context3(pCodec);
	if (avcodec_copy_context(pCodecCtx, pCodecCtxOrg) != 0)
		return -1;
	

	//可以播放视频
	SDL_Init(SDL_INIT_VIDEO |  SDL_INIT_TIMER);
	hVideo_Thread = ::CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)play_video_thread, 0, 0, NULL);


	Sleep(1000);

	AVCodecContext* pCodecCtxOrg_Audio = NULL;

	if (audioStream != -1)
	{
		pCodecCtxOrg_Audio = pFormatCtx->streams[audioStream]->codec; // codec context
		// 找到audio stream的 decoder
		pCodec_audio = avcodec_find_decoder(pCodecCtxOrg_Audio->codec_id);
		if (pCodec_audio)
		{
			// 不直接使用从AVFormatContext得到的CodecContext，要复制一个
			pCodecCtx_audio = avcodec_alloc_context3(pCodec_audio);
			if (avcodec_copy_context(pCodecCtx_audio, pCodecCtxOrg_Audio) != 0)
				return -1;
			
		}
	}


	hPlay_Thread = ::CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)play_process, 0, 0, NULL);


	getchar();

	//pause = 1;

	globle_seek_pos = 100;

	/*
	int ret, ret2;
	ret = av_seek_frame(pFormatCtx, 0, 20 * 1000, AVSEEK_FLAG_ANY);
	printf("ret %d\n", ret);
	ret2 = av_seek_frame(pFormatCtx, 1, 20 * 1000, AVSEEK_FLAG_ANY);
	printf("ret2 %d \n", ret2);
	*/

	while (1)
	{
		getchar();

		globle_seek_pos += 500;
	}

	//	pause = 0;


	while (1);

	quit = 1;

	Sleep(1000);

	CloseHandle(hPlay_Thread);
	CloseHandle(hVideo_Thread);


	if (pCodecCtxOrg)
		avcodec_close(pCodecCtx);

	if (pCodecCtxOrg)
		avcodec_close(pCodecCtxOrg);

	if (pCodecCtxOrg_Audio)
		avcodec_close(pCodecCtxOrg_Audio);

	avformat_close_input(&pFormatCtx);

	return 0;


	avformat_network_deinit();
    return 0;
}

