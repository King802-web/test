#include "logger.h"
//#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <iostream>
extern "C"
{
#include "libavutil/avstring.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include <libswscale/swscale.h>
#include "SDL2/SDL.h"
#include "SDL2/SDL_thread.h"
//#include "SDL2/SDL_render.h"
}

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main()*/
#endif
// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
//#define av_frame_alloc avcodec_alloc_frame
//#define av_frame_free  avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIO_SIZE (5 * 16 * 1024)
#define MAX_VIDEO_SIZE (5 * 256 * 1024)
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

typedef struct PacketQueue
{
    AVPacketList    *first_pkt,*last_pkt;
    int             nb_packets;
    int             size;
    SDL_mutex       *mutex;
    SDL_cond        *cond;
}PacketQueue;

typedef struct VideoPicture
{
    //SDL_Window      *windows;
    SDL_Renderer    *render;
    SDL_Texture     *texture;
    int             width,height; //source height & width
    int             allocated;
}VideoPicture;

typedef struct VideoState
{
    AVFormatContext *pFormatCtx;
    int             videoStream, audioStream;
    AVStream        *audio_st;
    AVCodecContext  *audio_ctx;
    PacketQueue     audio_q;
    uint8_t         audio_buf[(MAX_AUDIO_FRAME_SIZE * 3)/2];
    unsigned int    audio_buf_size;
    unsigned int    audio_buf_index;
    AVFrame         audio_frame;
    AVPacket        audio_pkt;
    uint8_t         *audio_pkt_data;
    int             audio_pkt_size;
    AVStream        *video_st;
    AVCodecContext  *video_ctx;
    PacketQueue     video_q;
    struct SwsContext *sws_ctx;

    VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
    int             pictq_size,pictq_rindex,pictq_windex;
    SDL_mutex       *pictq_mutex;
    SDL_cond        *pictq_cond;
    SDL_Thread      *parse_tid;
    SDL_Thread      *video_tid;
    char            fileName[1024];
    int             quit;
    //SDL_Window      *window_;
    //SDL_Renderer    *renderer;
    //SDL_Texture     *texture;

}VideoState;

//SDL_Surface         *screen;
SDL_Window          *screen;
SDL_mutex           *screen_mutex;

//Since we only have one decoding thread,the Big Struct can be global in case we need it.
VideoState *global_video_state;

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
        if(global_video_state->quit)
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
int audio_decode_frame(VideoState* is, uint8_t *audio_buf, int buf_size)
{
//    static  AVPacket   pkt;
//    static  uint8_t*   audio_pkt_data = NULL;
//    static  int        audio_pkt_size = 0;
//    static  AVFrame    frame;
//    int     data_size  = 0;
    int len_l,data_size = 0;
    AVPacket *pkt = &is->audio_pkt;

    for(;;)
    {
        while(is->audio_pkt_size > 0)
        {
            int ret = avcodec_send_packet(is->audio_ctx,pkt);
            if(ret < 0)
            {
                LOG(ERROR,"avcodec_send_packet failed");
                is->audio_pkt_size = 0;
                break;
            }
            ret = avcodec_receive_frame(is->audio_ctx,&is->audio_frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                //需要更多数据或达到文件末
                is->audio_pkt_size = 0;
                break;
            }else if(ret < 0)
            {
                //解码出错，处理错误
                is->audio_pkt_size = 0;
                break;
            }
            data_size = av_samples_get_buffer_size(NULL,
                                                   is->audio_ctx->channels,
                                                   is->audio_frame.nb_samples,
                                                   is->audio_ctx->sample_fmt,1);
            assert(data_size <= buf_size);
            memcpy(audio_buf,is->audio_frame.data[0],data_size);
            is->audio_pkt_data += data_size;
            is->audio_pkt_size -= data_size;
            //更新音频数据缓冲区指针
            audio_buf += data_size;
            if(data_size > 0)
            {
                return data_size;
            }
        }
        if(pkt->data)
            av_packet_unref(pkt);
        if(is->quit)
        {
            return -1;
        }
        //数据包来源
        if(packet_queue_get(&is->audio_q,pkt,1) < 0)
        {
            return -1;
        }
        is->audio_pkt_data = pkt->data;
        is->audio_pkt_size = pkt->size;
    }
}

void audio_callback(void* userdata, Uint8 *stream, int len)
{
    //LOG(INFO,"audio_callback");
    //AVCodecContext *aCodecCtx = (AVCodecContext*)userdata;
    VideoState *is = (VideoState*)userdata;
    int len_l,audio_size;

//    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3)/2];
//    static unsigned int audio_buf_size = 0;
//    static unsigned int audio_buf_index = 0;

    while(len > 0)
    {
        if(is->audio_buf_index >= is->audio_buf_size)
        {
            //we have already sent all our data; get more
            audio_size = audio_decode_frame(is,is->audio_buf,sizeof(is->audio_buf));
            if(audio_size < 0)
            {
                //if error. output silence
                is->audio_buf_size - 1024; //arbitrary?
                memset(is->audio_buf,0,is->audio_buf_size);
            }else
            {
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len_l = is->audio_buf_size - is->audio_buf_index;
        if(len_l > len)
        {
            len_l = len;
        }
        memcpy(stream, (uint8_t* )is->audio_buf + is->audio_buf_index, len_l);
        len -= len_l;
        stream += len_l;
        is->audio_buf_index += len_l;
    }
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval,void *opaque)
{
    SDL_Event          event;
    event.type       = FF_REFRESH_EVENT;
    event.user.data1 = opaque;
    SDL_PushEvent(&event);
    return 0; //0 means stop timer
}

//schedule a video refresh in 'delay' ms
static void schedule_refresh(VideoState *is, int delay)
{
    SDL_AddTimer(delay,sdl_refresh_timer_cb,is);
}

void video_display(VideoState* is)
{
    SDL_Rect       rect;
    VideoPicture   *vp;
    float          aspect_ratio;
    int            w,h,x,y,i,SDL_w,SDL_h;

    vp = &is->pictq[is->pictq_rindex];
    if(vp->render)
    {
        if(is->video_ctx->sample_aspect_ratio.num == 0)
        {
          aspect_ratio = 0;
        } else
        {
          aspect_ratio = av_q2d(is->video_ctx->sample_aspect_ratio) *
        is->video_ctx->width / is->video_ctx->height;
        }
        if(aspect_ratio <= 0.0)
        {
          aspect_ratio = (float)is->video_ctx->width / (float)is->video_ctx->height;
        }
        SDL_GetWindowSize(screen,&SDL_w,&SDL_h);
        h = SDL_h;
        w = ((int)rint(h * aspect_ratio)) & -3;
        if(w > SDL_w)
        {
          w = SDL_w;
          h = ((int)rint(w / aspect_ratio)) & -3;
        }
        x = (SDL_w - w) / 2;
        y = (SDL_h - h) / 2;

        rect.x = x;
        rect.y = y;
        rect.w = w;
        rect.h = h;
        SDL_LockMutex(screen_mutex);
        //SDL_DisplayYUVOverlay(vp->bmp, &rect);
        //SDL_Renderer* renderer = SDL_GetRenderer(screen);
        vp->render = SDL_GetRenderer(screen);
        //SDL_Texture*  texture = SDL_CreateTextureFromSurface(renderer,vp->bmp);
        SDL_Texture* texture = SDL_CreateTexture(vp->render,SDL_PIXELFORMAT_YV12,
                                                          SDL_TEXTUREACCESS_STREAMING,
                                                          w,h);
        SDL_RenderClear(vp->render);
        SDL_RenderCopy(vp->render,texture,NULL,&rect);
        SDL_RenderPresent(vp->render);
        //SDL_DestroyTexture(texture);
        SDL_UnlockMutex(screen_mutex);
    }

}

void video_refresh_timer(void *userdata)
{

  VideoState *is = (VideoState *)userdata;
  VideoPicture *vp;

  if(is->video_st)
  {
    if(is->pictq_size == 0)
    {
      schedule_refresh(is, 1);
    } else
    {
      vp = &is->pictq[is->pictq_rindex];
      /* Now, normally here goes a ton of code
     about timing, etc. we're just going to
     guess at a delay for now. You can
     increase and decrease this value and hard code
     the timing - but I don't suggest that ;)
     We'll learn how to do it for real later.
      */
      schedule_refresh(is, 40);

      /* show the picture! */
      video_display(is);

      /* update queue for next picture! */
      if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE)
      {
         is->pictq_rindex = 0;
      }
      SDL_LockMutex(is->pictq_mutex);
      is->pictq_size--;
      SDL_CondSignal(is->pictq_cond);
      SDL_UnlockMutex(is->pictq_mutex);
    }
  } else
  {
    schedule_refresh(is, 100);
  }
}


void alloc_picture(void *userdata)
{

  VideoState *is = (VideoState *)userdata;
  VideoPicture *vp;

  vp = &is->pictq[is->pictq_windex];
  if(vp->texture)
  {
    // we already have one make another, bigger/smaller
    //SDL_FreeYUVOverlay(vp->bmp);
      SDL_DestroyTexture(vp->texture);
  }
  // Allocate a place to put our YUV image on that screen
  SDL_LockMutex(screen_mutex);
  vp->texture = SDL_CreateTexture(vp->render,
                                SDL_PIXELFORMAT_YV12,
                                SDL_TEXTUREACCESS_STREAMING,
                                is->video_ctx->width,
                                is->video_ctx->height);
  SDL_UnlockMutex(screen_mutex);

  vp->width = is->video_ctx->width;
  vp->height = is->video_ctx->height;
  vp->allocated = 1;

}

int queue_picture(VideoState *is, AVFrame *pFrame)
{
  VideoPicture *vp;
  int dst_pix_fmt;
  //AVPicture pict;
  //AVPictureStructure pict;

  /* wait until we have space for a new pic */
  SDL_LockMutex(is->pictq_mutex);
  while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit)
  {
    SDL_CondWait(is->pictq_cond, is->pictq_mutex);
  }
  SDL_UnlockMutex(is->pictq_mutex);

  if(is->quit)
    return -1;

  // windex is set to 0 initially
  vp = &is->pictq[is->pictq_windex];

  /* allocate or resize the buffer! */
  if(!vp->texture ||
     vp->width != is->video_ctx->width ||
     vp->height != is->video_ctx->height)
  {
    SDL_Event event;

    vp->allocated = 0;
    alloc_picture(is);
    if(is->quit)
    {
      return -1;
    }
  }
  /* We have a place to put our picture on the queue */
  if(vp->render)
  {
      if(vp->texture)
      {

        SDL_LockTexture(vp->texture,NULL,NULL,NULL);

        dst_pix_fmt = AV_PIX_FMT_YUV420P;

        void* pixels;
        int pitch;

        SDL_LockTexture(vp->texture,NULL,&pixels,&pitch);
        uint8_t *pict_data[AV_NUM_DATA_POINTERS] = { 0 };
        int pict_linesize[AV_NUM_DATA_POINTERS];
        /* point pict at the queue */
        pict_data[0] = (uint8_t *)pixels;
        pict_data[1] = (uint8_t *)pixels + pitch * vp->height;
        pict_data[2] = (uint8_t *)pixels + pitch * vp->height * 5 / 4;
        pict_linesize[0] = pitch;
        pict_linesize[1] = pitch / 2;
        pict_linesize[2] = pitch / 2;

        sws_scale(is->sws_ctx,(uint8_t const* const*)pFrame->data,
                  pFrame->linesize,0,is->video_ctx->height,
                  pict_data,pict_linesize);

        SDL_UnlockTexture(vp->texture);
        /* now we inform our display thread that we have a pic ready */
        if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)
        {
          is->pictq_windex = 0;
        }
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
      }
  }
  return 0;
}

int video_thread(void *arg) {
  VideoState *is = (VideoState *)arg;
  AVPacket pkt1, *packet = &pkt1;
  int frameFinished;
  AVFrame *pFrame;

  pFrame = av_frame_alloc();

  for(;;)
  {
    if(packet_queue_get(&is->video_q, packet, 1) < 0)
    {
      // means we quit getting packets
      break;
    }
    // Decode video frame
    avcodec_send_packet(is->video_ctx, packet);

    // Receive decoded frames from the decoder
    while (avcodec_receive_frame(is->video_ctx, pFrame) == 0)
    {
      // Process the decoded frame
      if (queue_picture(is, pFrame) < 0)
      {
        break;
      }
    }
    av_packet_unref(packet);
  }
  av_frame_free(&pFrame);
  return 0;
}

int stream_component_open(VideoState *is, int stream_index) {

  AVFormatContext *pFormatCtx = is->pFormatCtx;
  AVCodecContext *codecCtx = NULL;
  const AVCodec *codec = NULL;
  SDL_AudioSpec wanted_spec, spec;

  if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
    return -1;
  }

  codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codecpar->codec_id);
  if(!codec) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }

  codecCtx = avcodec_alloc_context3(codec);
  if(avcodec_parameters_to_context(codecCtx, pFormatCtx->streams[stream_index]->codecpar) < 0) {
    fprintf(stderr, "Couldn't copy codec context");
    return -1; // Error copying codec context
  }


  if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
    // Set audio settings from codec info
    wanted_spec.freq = codecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = codecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = is;

    if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
      fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
      return -1;
    }
  }
  if(avcodec_open2(codecCtx, codec, NULL) < 0) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }

  switch(codecCtx->codec_type) {
  case AVMEDIA_TYPE_AUDIO:
    is->audioStream = stream_index;
    is->audio_st = pFormatCtx->streams[stream_index];
    is->audio_ctx = codecCtx;
    is->audio_buf_size = 0;
    is->audio_buf_index = 0;
    memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
    packet_queue_init(&is->audio_q);
    SDL_PauseAudio(0);
    break;
  case AVMEDIA_TYPE_VIDEO:
    is->videoStream = stream_index;
    is->video_st = pFormatCtx->streams[stream_index];
    is->video_ctx = codecCtx;
    packet_queue_init(&is->video_q);
    is->video_tid = SDL_CreateThread(video_thread,"video_thread", is);
    is->sws_ctx = sws_getContext(is->video_ctx->width, is->video_ctx->height,
                 is->video_ctx->pix_fmt, is->video_ctx->width,
                 is->video_ctx->height, AV_PIX_FMT_YUV420P,
                 SWS_BILINEAR, NULL, NULL, NULL
                 );
    break;
  default:
    break;
  }
}

int decode_thread(void *arg) {

  VideoState *is = (VideoState *)arg;
  AVFormatContext *pFormatCtx;
  AVPacket pkt1, *packet = &pkt1;

  int video_index = -1;
  int audio_index = -1;
  int i;

  is->videoStream=-1;
  is->audioStream=-1;

  global_video_state = is;

  // Open video file
  if(avformat_open_input(&pFormatCtx, is->fileName, NULL, NULL)!=0)
    return -1; // Couldn't open file

  is->pFormatCtx = pFormatCtx;

  // Retrieve stream information
  if(avformat_find_stream_info(pFormatCtx, NULL)<0)
    return -1; // Couldn't find stream information

  // Dump information about file onto standard error
  av_dump_format(pFormatCtx, 0, is->fileName, 0);

  // Find the first video stream

  for(i=0; i<pFormatCtx->nb_streams; i++) {
    if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO &&
       video_index < 0) {
      video_index=i;
    }
    if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO &&
       audio_index < 0) {
      audio_index=i;
    }
  }
  if(audio_index >= 0) {
    stream_component_open(is, audio_index);
  }
  if(video_index >= 0) {
    stream_component_open(is, video_index);
  }

  if(is->videoStream < 0 || is->audioStream < 0) {
    fprintf(stderr, "%s: could not open codecs\n", is->fileName);
    goto fail;
  }

  // main decode loop

  for(;;) {
    if(is->quit) {
      break;
    }
    // seek stuff goes here
    if(is->audio_q.size > MAX_AUDIOQ_SIZE ||
       is->video_q.size > MAX_VIDEOQ_SIZE) {
      SDL_Delay(10);
      continue;
    }
    if(av_read_frame(is->pFormatCtx, packet) < 0) {
      if(is->pFormatCtx->pb->error == 0) {
    SDL_Delay(100); /* no error; wait for user input */
    continue;
      } else {
    break;
      }
    }
    // Is this a packet from the video stream?
    if(packet->stream_index == is->videoStream) {
      packet_queue_put(&is->video_q, packet);
    } else if(packet->stream_index == is->audioStream) {
      packet_queue_put(&is->audio_q, packet);
    } else {
      av_packet_unref(packet);
    }
  }
  /* all done - wait for it */
  while(!is->quit) {
    SDL_Delay(100);
  }

 fail:
  if(1){
    SDL_Event event;
    event.type = FF_QUIT_EVENT;
    event.user.data1 = is;
    SDL_PushEvent(&event);
  }
  return 0;
}



int main(int argc, char *argv[]) {

  SDL_Event       event;

  VideoState      *is;

  is = (VideoState*)av_mallocz(sizeof(VideoState));

  if(argc < 2) {
    fprintf(stderr, "Usage: test <file>\n");
    exit(1);
  }
  // Register all formats and codecs

  if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
    exit(1);
  }
   screen = SDL_CreateWindow("video player",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,800,600,SDL_WINDOW_OPENGL);
  // Make a screen to put our video
#ifndef __DARWIN__
        //screen = SDL_SetVideoMode(640, 480, 0, 0);
#else
        screen = SDL_SetVideoMode(640, 480, 24, 0);
#endif
  if(!screen) {
    fprintf(stderr, "SDL: could not set video mode - exiting\n");
    exit(1);
  }

  screen_mutex = SDL_CreateMutex();

  av_strlcpy(is->fileName, argv[1], sizeof(is->fileName));

  is->pictq_mutex = SDL_CreateMutex();
  is->pictq_cond = SDL_CreateCond();

  schedule_refresh(is, 40);

  is->parse_tid = SDL_CreateThread(decode_thread,"decode_thread", is);
  if(!is->parse_tid) {
    av_free(is);
    return -1;
  }
  for(;;) {

    SDL_WaitEvent(&event);
    switch(event.type) {
    case FF_QUIT_EVENT:
    case SDL_QUIT:
      is->quit = 1;
      SDL_Quit();
      return 0;
      break;
    case FF_REFRESH_EVENT:
      video_refresh_timer(event.user.data1);
      break;
    default:
      break;
    }
  }
  return 0;

}

















//typedef struct PacketQueue
//{
//    AVPacketList    *first_pkt,*last_pkt;
//    int             nb_packets;
//    int             size;
//    SDL_mutex       *mutex;
//    SDL_cond        *cond;
//}PacketQueue;

//PacketQueue audio_q;
//int quit = 0;

//void packet_queue_init(PacketQueue *q)
//{
//    LOG(INFO,"packet_queue_init");
//    memset(q,0,sizeof(PacketQueue));
//    q->mutex = SDL_CreateMutex();
//    q->cond = SDL_CreateCond();
//}

////该函数的功能是将AVPacket数据包放入PacketQueue的末尾，
////维护队列中的数据包数量和总大小，并通过互斥锁和条件变量来实现线程安全的操作。
//int packet_queue_put(PacketQueue* q,AVPacket *pkt)
//{
//    AVPacketList *pkt_l;
//    AVPacket newPkt;
//    if(av_packet_ref(&newPkt,pkt) < 0)
//    {
//        LOG(ERROR,"copy AVPacket failed");
//        return -1;
//    }
//    pkt_l = (AVPacketList*)av_malloc(sizeof(AVPacketList));
//    if(!pkt_l)
//        return -1;
//    pkt_l->pkt = *pkt;
//    pkt_l->next = NULL;

//    SDL_LockMutex(q->mutex);

//    if(!q->last_pkt)
//        q->first_pkt = pkt_l;
//    else
//        //通过q访问，将后一个链表元素与前一个连起来
//        q->last_pkt->next = pkt_l;

//    q->last_pkt = pkt_l;
//    q->nb_packets++;
//    q->size += pkt_l->pkt.size;
//    SDL_CondSignal(q->cond);
//    SDL_UnlockMutex(q->mutex);
//    return 0;
//}
///*该函数实现了从PacketQueue中获取AVPacket数据包的操作。
// * 它会检查队列是否为空，如果不为空，则获取队列中的第一个数据包并更新队列的状态，
// * 如果为空且允许阻塞等待，则在条件变量上等待信号，直到有新的数据包可用。
// * 函数通过互斥锁保证了对队列的线程安全访问。*/
//static int packet_queue_get(PacketQueue* q,AVPacket* pkt,int block)
//{
//    AVPacketList* pkt_l;
//    int ret;

//    SDL_LockMutex(q->mutex);

//    for(;;)
//    {
//        if(quit)
//        {
//            ret = -1;
//            break;
//        }
//        pkt_l = q->first_pkt;
//        if(pkt_l)
//        {
//            q->first_pkt = pkt_l->next;
//            if(!q->first_pkt)
//                q->last_pkt = NULL;
//            q->nb_packets--;
//            q->size -= pkt_l->pkt.size;
//            *pkt = pkt_l->pkt;
//            av_free(pkt_l);
//            ret = 1;
//            break;
//        }else if(!block)
//        {
//            ret = 0;
//            break;
//        }else
//        {
//            SDL_CondWait(q->cond,q->mutex);
//        }
//    }
//    SDL_UnlockMutex(q->mutex);
//    return ret;

//}
////音频解码函数audio_decode_frame，它使用AVCodecContext进行音频解码，
////并将解码后的音频数据存储到audio_buf中。
//int audio_decode_frame(AVCodecContext* aCodecCtx, uint8_t *audio_buf, int buf_size)
//{
//    static  AVPacket   pkt;
//    static  uint8_t*   audio_pkt_data = NULL;
//    static  int        audio_pkt_size = 0;
//    static  AVFrame    frame;
//    int     data_size  = 0;

//    for(;;)
//    {
//        while(audio_pkt_size > 0)
//        {
//            int ret = avcodec_send_packet(aCodecCtx,&pkt);
//            if(ret < 0)
//            {
//                LOG(ERROR,"avcodec_send_packet failed");
//                audio_pkt_size = 0;
//                break;
//            }
//            ret = avcodec_receive_frame(aCodecCtx,&frame);
//            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
//            {
//                //需要更多数据或达到文件末
//                audio_pkt_size = 0;
//                break;
//            }else if(ret < 0)
//            {
//                //解码出错，处理错误
//                audio_pkt_size = 0;
//                break;
//            }
//            data_size = av_samples_get_buffer_size(NULL,
//                                                   aCodecCtx->channels,
//                                                   frame.nb_samples,
//                                                   aCodecCtx->sample_fmt,1);
//            assert(data_size <= buf_size);
//            memcpy(audio_buf,frame.data[0],data_size);
//            audio_pkt_data += data_size;
//            audio_pkt_size -= data_size;
//            //更新音频数据缓冲区指针
//            audio_buf += data_size;
//            if(data_size > 0)
//            {
//                return data_size;
//            }
//        }
//        if(pkt.data)
//            av_packet_unref(&pkt);
//        if(quit)
//        {
//            return -1;
//        }
//        //数据包来源
//        if(packet_queue_get(&audio_q,&pkt,1) < 0)
//        {
//            return -1;
//        }
//        audio_pkt_data = pkt.data;
//        audio_pkt_size = pkt.size;
//    }
//}
//void audio_callback(void* userdata, Uint8 *stream, int len)
//{
//    //LOG(INFO,"audio_callback");
//    AVCodecContext *aCodecCtx = (AVCodecContext*)userdata;
//    int len_l,audio_size;

//    static uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3)/2];
//    static unsigned int audio_buf_size = 0;
//    static unsigned int audio_buf_index = 0;

//    while(len > 0)
//    {
//        if(audio_buf_index >= audio_buf_size)
//        {
//            //we have already sent all our data; get more
//            audio_size = audio_decode_frame(aCodecCtx,audio_buf,sizeof(audio_buf));
//            if(audio_size < 0)
//            {
//                //if error. output silence
//                audio_buf_size - 1024; //arbitrary?
//                memset(audio_buf,0,audio_buf_size);
//            }else
//            {
//                audio_buf_size = audio_size;
//            }
//            audio_buf_index = 0;
//        }
//        len_l = audio_buf_size - audio_buf_index;
//        if(len_l > len)
//        {
//            len_l = len;
//        }
//        memcpy(stream, (uint8_t* )audio_buf + audio_buf_index, len_l);
//        len -= len_l;
//        stream += len_l;
//        audio_buf_index += len_l;
//    }
//}
//void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame)
//{
//  FILE *pFile;
//  char szFilename[32];
//  int  y;

//  // Open file
//  sprintf(szFilename, "frame%d.ppm", iFrame);
//  pFile=fopen(szFilename, "wb");
//  if(pFile==NULL)
//    return;

//  // Write header
//  fprintf(pFile, "P6\n%d %d\n255\n", width, height);

//  // Write pixel data
//  for(y=0; y<height; y++)
//    fwrite(pFrame->data[0]+y*pFrame->linesize[0], 1, width*3, pFile);

//  // Close file
//  fclose(pFile);
//}

//int main(int argc, char *argv[])
//{
//    LOG(INFO,argv[1]);
//    // Initalizing these to NULL prevents segfaults!
//    AVFormatContext   *pFormatCtx = NULL;
//    int               i, videoStream,audioStream;
//    AVCodecContext    *pCodecCtxOrig = NULL;
//    //AVCodecContext    *pCodecCtx = NULL;
//    const AVCodec     *pCodec = NULL;
//    AVFrame           *pFrame = NULL;
//    //AVFrame           *pFrameRGB = NULL;
//    AVPacket          packet;
//    int               frameFinished;
//    //int               numBytes;
//    //uint8_t           *buffer = NULL;
//    //float             aspect_ratio;
//    struct SwsContext *sws_ctx = NULL;

//    AVCodecContext    *aCodecCtx = NULL;
//    AVCodecContext    *aCodecCtxOrig = NULL;
//    const AVCodec     *aCodec = NULL;

//    SDL_Window        *window_;
//    SDL_Renderer      *rendere_;
//    SDL_Texture       *texture_;
//    SDL_AudioSpec     wanted_spec,spec;

//    if(argc < 2)
//    {
//        LOG(ERROR,"Usage test <file>");
//        exit(1);
//    }
//    //make a screen to put our video
//    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
//    {
//        LOG(ERROR,"SDL: Init failed");
//        exit(1);
//    }
//    // Open video file
//    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL)!=0)
//        {
//            LOG(ERROR,"Couldn't open file");
//            return -1;
//        }

//    // Retrieve stream information
//    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
//        {
//            LOG(ERROR,"Couldn't find stream information");
//            return -1;
//        }

//    // Dump information about file onto standard error
//    //av_dump_format(pFormatCtx, 0, argv[1], 0);

//    // Find the first video stream
//    videoStream=-1;
//    audioStream=-1;
//    for(i=0; i<pFormatCtx->nb_streams; i++)
//    {
//      if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && videoStream < 0)
//        {
//          videoStream=i;
//          //LOG(INFO,"videoStream: ",videoStream);
//        }
//      if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_AUDIO && audioStream < 0)
//        {
//          audioStream=i;
//          //LOG(INFO,"audioStream: ",audioStream);
//        }
//    }
//    if(videoStream==-1 || audioStream == -1)
//        {
//            LOG(ERROR,"Didn't find a video/audio stream");
//            return -1;
//        }

//    // Get a pointer to the codec context for the video stream
//    pCodecCtxOrig = avcodec_alloc_context3(NULL);
//    aCodecCtxOrig = avcodec_alloc_context3(NULL);

//    avcodec_parameters_to_context(pCodecCtxOrig,pFormatCtx->streams[videoStream]->codecpar);
//    avcodec_parameters_to_context(aCodecCtxOrig,pFormatCtx->streams[audioStream]->codecpar);
//    // Find the decoder for the video stream
//    pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
//    aCodec = avcodec_find_decoder(aCodecCtxOrig->codec_id);
//    if(pCodec==NULL || aCodec == NULL)
//        {
//          LOG(ERROR,"Unsupported codec!");
//          return -1; // Codec not found
//        }
////    // Copy context
////    pCodecCtxOrig = avcodec_alloc_context3(pCodec);
////    avcodec_parameters_to_context(pCodecCtxOrig, pFormatCtx->streams[videoStream]->codecpar);

//    wanted_spec.freq     =   aCodecCtxOrig->sample_rate;
//    wanted_spec.format   =   AUDIO_S16SYS;
//    wanted_spec.channels =   aCodecCtxOrig->channels;
//    wanted_spec.silence  =   0;
//    wanted_spec.samples  =   SDL_AUDIO_BUFFER_SIZE;
//    wanted_spec.callback =   audio_callback;
//    wanted_spec.userdata =   aCodecCtxOrig;

//    if(SDL_OpenAudio(&wanted_spec,&spec) < 0)
//    {
//        LOG(ERROR,"SDL_OpenAudio failed");
//        return -1;
//    }
//    // Open acodec
//    if(avcodec_open2(aCodecCtxOrig, aCodec, NULL)<0)
//        {
//            LOG(ERROR,"Could not open acodec");
//            return -1;
//        }
//    packet_queue_init(&audio_q);
//    SDL_PauseAudio(0);
//    // Open codec
//    if(avcodec_open2(pCodecCtxOrig, pCodec, NULL)<0)
//        {
//            LOG(ERROR,"Could not open codec");
//            return -1;
//        }
//    // Allocate video frame
//    pFrame=av_frame_alloc();

//    window_ = SDL_CreateWindow("video PLayer",
//                               SDL_WINDOWPOS_CENTERED,
//                               SDL_WINDOWPOS_CENTERED,
//                               pCodecCtxOrig->width,
//                               pCodecCtxOrig->height,
//                               SDL_WINDOW_SHOWN);
//    if(window_ == NULL)
//    {
//        LOG(ERROR,"SDL_CreateWindow failed");
//        return -1;
//    }
//    // 创建渲染器
//    rendere_ = SDL_CreateRenderer(window_, -1, 0);
//    texture_ = SDL_CreateTexture(rendere_,SDL_PIXELFORMAT_YV12,
//                                          SDL_TEXTUREACCESS_STREAMING,
//                                          pCodecCtxOrig->width,
//                                          pCodecCtxOrig->height);
//    // 设置渲染器绘制颜色为黑色
//    SDL_SetRenderDrawColor(rendere_, 0, 0, 0, 255);

//    // 清空渲染器
//    SDL_RenderClear(rendere_);

//    // 更新窗口显示
//    SDL_RenderPresent(rendere_);

//    // initialize SWS context for software scaling
//    sws_ctx = sws_getContext( pCodecCtxOrig->width,
//                              pCodecCtxOrig->height,
//                              pCodecCtxOrig->pix_fmt,
//                              pCodecCtxOrig->width,
//                              pCodecCtxOrig->height,
//                              AV_PIX_FMT_RGB24,
//                              SWS_BILINEAR,
//                              NULL,
//                              NULL,
//                              NULL);
//    // Read frames and save first five frames to disk
//    while(av_read_frame(pFormatCtx, &packet)>=0)
//    {
//      // Is this a packet from the video stream?
//      if(packet.stream_index==videoStream)
//      {
//        // Decode video frame
//          avcodec_send_packet(pCodecCtxOrig,&packet);
//          frameFinished = avcodec_receive_frame(pCodecCtxOrig,pFrame);

//        // Did we get a video frame?
//        if(frameFinished == 0)
//        {
//          // 将解码后的帧数据拷贝到SDL纹理中
//            SDL_UpdateYUVTexture(texture_, NULL, pFrame->data[0], pFrame->linesize[0], pFrame->data[1], pFrame->linesize[1], pFrame->data[2], pFrame->linesize[2]);

//            // 清空渲染器
//            SDL_RenderClear(rendere_);

//            // 将纹理绘制到渲染器
//            SDL_RenderCopy(rendere_, texture_, NULL, NULL);

//            // 更新窗口显示
//            SDL_RenderPresent(rendere_);

//            // 等待一段时间，控制视频帧的显示速率
//            SDL_Delay(24);
//        }
//      }else if(packet.stream_index == audioStream)
//      {
//          packet_queue_put(&audio_q,&packet);
//      }else
//      {
//          // Free the packet that was allocated by av_read_frame
//          av_packet_unref(&packet);
//      }
//    }
//    // Free the YUV frame
//    av_frame_free(&pFrame);

//    // Close the codecs
//    //avcodec_close(pCodecCtx);
//    avcodec_close(pCodecCtxOrig);
//    avcodec_close(aCodecCtx);
//    avcodec_close(aCodecCtxOrig);

//    // Close the video file
//    avformat_close_input(&pFormatCtx);

//    // 销毁纹理、渲染器和窗口
//    SDL_DestroyTexture(texture_);
//    SDL_DestroyRenderer(rendere_);
//    SDL_DestroyWindow(window_);

//    // 退出SDL
//    SDL_Quit();
//    return 0;
//}
