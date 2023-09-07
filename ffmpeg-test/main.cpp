#include "logger.h"
//#include <stdio.h>
#include <assert.h>
#include <iostream>
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include <libswscale/swscale.h>
#include "SDL2/SDL.h"
#include "SDL2/SDL_thread.h"
}

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main()*/
#endif
// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

typedef struct PacketQueue
{
    AVPacketList    *first_pkt,*last_pkt;
    int             nb_packets;
    int             size;
    SDL_mutex       *mutex;
    SDL_cond        *cond;
}PacketQueue;

PacketQueue audio_q;
int quit = 0;

void packet_queue_init(PacketQueue *q)
{
    LOG(INFO,"packet_queue_init");
    memset(q,0,sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

//该函数的功能是将AVPacket数据包放入PacketQueue的末尾，
//维护队列中的数据包数量和总大小，并通过互斥锁和条件变量来实现线程安全的操作。
int packet_queue_put(PacketQueue* q,AVPacket *pkt)
{
    AVPacketList *pkt_l;
    AVPacket newPkt;
    if(av_packet_ref(&newPkt,pkt) < 0)
    {
        LOG(ERROR,"copy AVPacket failed");
        return -1;
    }
    pkt_l = (AVPacketList*)av_malloc(sizeof(AVPacketList));
    if(!pkt_l)
        return -1;
    pkt_l->pkt = *pkt;
    pkt_l->next = NULL;

    SDL_LockMutex(q->mutex);

    if(!q->last_pkt)
        q->first_pkt = pkt_l;
    else
        //通过q访问，将后一个链表元素与前一个连起来
        q->last_pkt->next = pkt_l;

    q->last_pkt = pkt_l;
    q->nb_packets++;
    q->size += pkt_l->pkt.size;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}
/*该函数实现了从PacketQueue中获取AVPacket数据包的操作。
 * 它会检查队列是否为空，如果不为空，则获取队列中的第一个数据包并更新队列的状态，
 * 如果为空且允许阻塞等待，则在条件变量上等待信号，直到有新的数据包可用。
 * 函数通过互斥锁保证了对队列的线程安全访问。*/
static int packet_queue_get(PacketQueue* q,AVPacket* pkt,int block)
{
    AVPacketList* pkt_l;
    int ret;

    SDL_LockMutex(q->mutex);

    for(;;)
    {
        if(quit)
        {
            ret = -1;
            break;
        }
        pkt_l = q->first_pkt;
        if(pkt_l)
        {
            q->first_pkt = pkt_l->next;
            if(!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size -= pkt_l->pkt.size;
            *pkt = pkt_l->pkt;
            av_free(pkt_l);
            ret = 1;
            break;
        }else if(!block)
        {
            ret = 0;
            break;
        }else
        {
            SDL_CondWait(q->cond,q->mutex);
        }
    }
    SDL_UnlockMutex(q->mutex);
    return ret;

}
//音频解码函数audio_decode_frame，它使用AVCodecContext进行音频解码，
//并将解码后的音频数据存储到audio_buf中。
int audio_decode_frame(AVCodecContext* aCodecCtx, uint8_t *audio_buf, int buf_size)
{
    static  AVPacket   pkt;
    static  uint8_t*   audio_pkt_data = NULL;
    static  int        audio_pkt_size = 0;
    static  AVFrame    frame;
    int     data_size  = 0;

    for(;;)
    {
        while(audio_pkt_size > 0)
        {
            int ret = avcodec_send_packet(aCodecCtx,&pkt);
            if(ret < 0)
            {
                LOG(ERROR,"avcodec_send_packet failed");
                audio_pkt_size = 0;
                break;
            }
            ret = avcodec_receive_frame(aCodecCtx,&frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                //需要更多数据或达到文件末
                audio_pkt_size = 0;
                break;
            }else if(ret < 0)
            {
                //解码出错，处理错误
                audio_pkt_size = 0;
                break;
            }
            data_size = av_samples_get_buffer_size(NULL,
                                                   aCodecCtx->channels,
                                                   frame.nb_samples,
                                                   aCodecCtx->sample_fmt,1);
            assert(data_size <= buf_size);
            memcpy(audio_buf,frame.data[0],data_size);
            audio_pkt_data += data_size;
            audio_pkt_size -= data_size;
            //更新音频数据缓冲区指针
            audio_buf += data_size;
            if(data_size > 0)
            {
                return data_size;
            }
        }
        if(pkt.data)
            av_packet_unref(&pkt);
        if(quit)
        {
            return -1;
        }
        //数据包来源
        if(packet_queue_get(&audio_q,&pkt,1) < 0)
        {
            return -1;
        }
        audio_pkt_data = pkt.data;
        audio_pkt_size = pkt.size;
    }
}

void audio_callback(void* userdata, Uint8 *stream, int len)
{
    //LOG(INFO,"audio_callback");
    AVCodecContext *aCodecCtx = (AVCodecContext*)userdata;
    int len_l,audio_size;

    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3)/2];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;

    while(len > 0)
    {
        if(audio_buf_index >= audio_buf_size)
        {
            //we have already sent all our data; get more
            audio_size = audio_decode_frame(aCodecCtx,audio_buf,sizeof(audio_buf));
            if(audio_size < 0)
            {
                //if error. output silence
                audio_buf_size - 1024; //arbitrary?
                memset(audio_buf,0,audio_buf_size);
            }else
            {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        len_l = audio_buf_size - audio_buf_index;
        if(len_l > len)
        {
            len_l = len;
        }
        memcpy(stream, (uint8_t* )audio_buf + audio_buf_index, len_l);
        len -= len_l;
        stream += len_l;
        audio_buf_index += len_l;
    }
}
void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame)
{
  FILE *pFile;
  char szFilename[32];
  int  y;

  // Open file
  sprintf(szFilename, "frame%d.ppm", iFrame);
  pFile=fopen(szFilename, "wb");
  if(pFile==NULL)
    return;

  // Write header
  fprintf(pFile, "P6\n%d %d\n255\n", width, height);

  // Write pixel data
  for(y=0; y<height; y++)
    fwrite(pFrame->data[0]+y*pFrame->linesize[0], 1, width*3, pFile);

  // Close file
  fclose(pFile);
}

int main(int argc, char *argv[])
{
    LOG(INFO,argv[1]);
    // Initalizing these to NULL prevents segfaults!
    AVFormatContext   *pFormatCtx = NULL;
    int               i, videoStream,audioStream;
    AVCodecContext    *pCodecCtxOrig = NULL;
    //AVCodecContext    *pCodecCtx = NULL;
    const AVCodec     *pCodec = NULL;
    AVFrame           *pFrame = NULL;
    //AVFrame           *pFrameRGB = NULL;
    AVPacket          packet;
    int               frameFinished;
    //int               numBytes;
    //uint8_t           *buffer = NULL;
    //float             aspect_ratio;
    struct SwsContext *sws_ctx = NULL;

    AVCodecContext    *aCodecCtx = NULL;
    AVCodecContext    *aCodecCtxOrig = NULL;
    const AVCodec     *aCodec = NULL;

    SDL_Window        *window_;
    SDL_Renderer      *rendere_;
    SDL_Texture       *texture_;
    SDL_AudioSpec     wanted_spec,spec;

    if(argc < 2)
    {
        LOG(ERROR,"Usage test <file>");
        exit(1);
    }
    //make a screen to put our video
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
    {
        LOG(ERROR,"SDL: Init failed");
        exit(1);
    }
    // Open video file
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL)!=0)
        {
            LOG(ERROR,"Couldn't open file");
            return -1;
        }

    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        {
            LOG(ERROR,"Couldn't find stream information");
            return -1;
        }

    // Dump information about file onto standard error
    //av_dump_format(pFormatCtx, 0, argv[1], 0);

    // Find the first video stream
    videoStream=-1;
    audioStream=-1;
    for(i=0; i<pFormatCtx->nb_streams; i++)
    {
      if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && videoStream < 0)
        {
          videoStream=i;
          //LOG(INFO,"videoStream: ",videoStream);
        }
      if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO && audioStream < 0)
        {
          audioStream=i;
          //LOG(INFO,"audioStream: ",audioStream);
        }
    }
    if(videoStream==-1 || audioStream == -1)
        {
            LOG(ERROR,"Didn't find a video/audio stream");
            return -1;
        }

    // Get a pointer to the codec context for the video stream
    pCodecCtxOrig = avcodec_alloc_context3(NULL);
    aCodecCtxOrig = avcodec_alloc_context3(NULL);

    avcodec_parameters_to_context(pCodecCtxOrig,pFormatCtx->streams[videoStream]->codecpar);
    avcodec_parameters_to_context(aCodecCtxOrig,pFormatCtx->streams[audioStream]->codecpar);
    // Find the decoder for the video stream
    pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
    aCodec = avcodec_find_decoder(aCodecCtxOrig->codec_id);
    if(pCodec==NULL || aCodec == NULL)
        {
          LOG(ERROR,"Unsupported codec!");
          return -1; // Codec not found
        }
//    // Copy context
//    pCodecCtxOrig = avcodec_alloc_context3(pCodec);
//    avcodec_parameters_to_context(pCodecCtxOrig, pFormatCtx->streams[videoStream]->codecpar);

    wanted_spec.freq     =   aCodecCtxOrig->sample_rate;
    wanted_spec.format   =   AUDIO_S16SYS;
    wanted_spec.channels =   aCodecCtxOrig->channels;
    wanted_spec.silence  =   0;
    wanted_spec.samples  =   SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback =   audio_callback;
    wanted_spec.userdata =   aCodecCtxOrig;

    if(SDL_OpenAudio(&wanted_spec,&spec) < 0)
    {
        LOG(ERROR,"SDL_OpenAudio failed");
        return -1;
    }
    // Open acodec
    if(avcodec_open2(aCodecCtxOrig, aCodec, NULL)<0)
        {
            LOG(ERROR,"Could not open acodec");
            return -1;
        }
    packet_queue_init(&audio_q);
    SDL_PauseAudio(0);
    // Open codec
    if(avcodec_open2(pCodecCtxOrig, pCodec, NULL)<0)
        {
            LOG(ERROR,"Could not open codec");
            return -1;
        }
    // Allocate video frame
    pFrame=av_frame_alloc();

    window_ = SDL_CreateWindow("video PLayer",
                               SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED,
                               pCodecCtxOrig->width,
                               pCodecCtxOrig->height,
                               SDL_WINDOW_SHOWN);
    if(window_ == NULL)
    {
        LOG(ERROR,"SDL_CreateWindow failed");
        return -1;
    }
    // 创建渲染器
    rendere_ = SDL_CreateRenderer(window_, -1, 0);
    texture_ = SDL_CreateTexture(rendere_,SDL_PIXELFORMAT_YV12,
                                          SDL_TEXTUREACCESS_STREAMING,
                                          pCodecCtxOrig->width,
                                          pCodecCtxOrig->height);
    // 设置渲染器绘制颜色为黑色
    SDL_SetRenderDrawColor(rendere_, 0, 0, 0, 255);

    // 清空渲染器
    SDL_RenderClear(rendere_);

    // 更新窗口显示
    SDL_RenderPresent(rendere_);

    // initialize SWS context for software scaling
    sws_ctx = sws_getContext( pCodecCtxOrig->width,
                              pCodecCtxOrig->height,
                              pCodecCtxOrig->pix_fmt,
                              pCodecCtxOrig->width,
                              pCodecCtxOrig->height,
                              AV_PIX_FMT_RGB24,
                              SWS_BILINEAR,
                              NULL,
                              NULL,
                              NULL);
    // Read frames and save first five frames to disk
    while(av_read_frame(pFormatCtx, &packet)>=0)
    {
      // Is this a packet from the video stream?
      if(packet.stream_index==videoStream)
      {
        // Decode video frame
          avcodec_send_packet(pCodecCtxOrig,&packet);
          frameFinished = avcodec_receive_frame(pCodecCtxOrig,pFrame);

        // Did we get a video frame?
        if(frameFinished == 0)
        {
          // 将解码后的帧数据拷贝到SDL纹理中
            SDL_UpdateYUVTexture(texture_, NULL, pFrame->data[0], pFrame->linesize[0], pFrame->data[1], pFrame->linesize[1], pFrame->data[2], pFrame->linesize[2]);

            // 清空渲染器
            SDL_RenderClear(rendere_);

            // 将纹理绘制到渲染器
            SDL_RenderCopy(rendere_, texture_, NULL, NULL);

            // 更新窗口显示
            SDL_RenderPresent(rendere_);

            // 等待一段时间，控制视频帧的显示速率
            SDL_Delay(24);
        }
      }else if(packet.stream_index == audioStream)
      {
          packet_queue_put(&audio_q,&packet);
      }else
      {
          // Free the packet that was allocated by av_read_frame
          av_packet_unref(&packet);
      }
    }
    // Free the YUV frame
    av_frame_free(&pFrame);

    // Close the codecs
    //avcodec_close(pCodecCtx);
    avcodec_close(pCodecCtxOrig);
    avcodec_close(aCodecCtx);
    avcodec_close(aCodecCtxOrig);

    // Close the video file
    avformat_close_input(&pFormatCtx);

    // 销毁纹理、渲染器和窗口
    SDL_DestroyTexture(texture_);
    SDL_DestroyRenderer(rendere_);
    SDL_DestroyWindow(window_);

    // 退出SDL
    SDL_Quit();
    return 0;
}
