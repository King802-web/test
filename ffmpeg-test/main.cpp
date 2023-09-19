#include "logger.h"
#include <iostream>

#define __STDC_CONSTANT_MACROS
#ifdef _WIN32

extern "C"
{
#include "libavutil/samplefmt.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include "SDL2/SDL.h"
#include "SDL2/SDL_thread.h"
}
#else

#ifdef __cplusplus
extern "C"
{
#include "libavutil/samplefmt.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include "libavutil/pixfmt.h"
#include "libavutil/imgutils.h"
#include "SDL2/SDL.h"
#include "SDL2/SDL_thread.h"
}
#endif
#endif

#define SFM_REFRESH_EVENT (SDL_USEREVENT + 1)
#define SFM_BREAK_EVENT (SDL_USEREVENT + 2)

int thread_exit = 0;
int thread_pause = 0;

int sfp_refresh_thread(void* opaque)
{
    thread_exit = 0;
    thread_pause =  0;

    while(!thread_exit)
    {
        if(!thread_pause)
        {
            SDL_Event event;
            event.type = SFM_REFRESH_EVENT;
            SDL_PushEvent(&event);
        }
        SDL_Delay(40);
    }
    thread_exit = 0;
    thread_pause = 0;
    SDL_Event event;
    event.type = SFM_BREAK_EVENT;
    SDL_PushEvent(&event);
    return 0;
}

#undef main
int main(int argc, char* argv[])
{
    AVFormatContext          *pFormatCtx;
    int                      i,videodex;
    AVCodecContext           *pCodecCtx;
    const AVCodec            *pCodec;
    AVFrame                  *pFrame,*pFrameYUV;
    unsigned char            *out_buffer;
    AVPacket                 *packet;
    //int                      y_size;
    int                      ret,got_picture;
    struct  SwsContext       *img_convert_ctx;

    int                      screen_w = 0,screen_h = 0;
    SDL_Window               *screen;
    SDL_Renderer             *render;
    SDL_Texture              *texture;
    SDL_Rect                 rect;
    SDL_Thread               *video_tid;
    SDL_Event                event;

    //avformat_network_init();
    pFormatCtx = avformat_alloc_context();

    if(avformat_open_input(&pFormatCtx,argv[1],NULL,NULL) != 0)
    {
        LOG(ERROR,"Couldn't open input stream");
        return -1;
    }
    if(avformat_find_stream_info(pFormatCtx,NULL) < 0)
    {
        LOG(ERROR,"Couldn't find stream information");
        return -1;
    }
    videodex = -1;
    for(i = 0; i < pFormatCtx->nb_streams; i++)
    {
        if(pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videodex = i;
            break;
        }
    }
    if(videodex == -1)
    {
        LOG(ERROR,"Didn't find a video stream");
        return -1;
    }

    pCodecCtx = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(pCodecCtx,pFormatCtx->streams[videodex]->codecpar);
    pCodec = avcodec_find_decoder(pCodecCtx->codec_id);

    if(pCodec == NULL)
    {
        LOG(ERROR,"Codec not found.");
        return -1;
    }
    if(avcodec_open2(pCodecCtx,pCodec,NULL) < 0)
    {
        LOG(ERROR,"Couldn't open codec.");
        return -1;
    }
    pFrame = av_frame_alloc();
    pFrameYUV = av_frame_alloc();
    out_buffer =  (unsigned char*)av_malloc(
                    av_image_get_buffer_size(
                    AV_PIX_FMT_YUV420P,
                    pCodecCtx->width,
                    pCodecCtx->height,
                    1));

    av_image_fill_arrays(pFrameYUV->data,
                         pFrameYUV->linesize,
                         out_buffer,
                         AV_PIX_FMT_YUV420P,
                         pCodecCtx->width,
                         pCodecCtx->height,
                         1);
    packet = (AVPacket*)av_malloc(sizeof(AVPacket));
    LOG(INFO,"---------------------file Information-------------------");
    av_dump_format(pFormatCtx,0,argv[1],0);
    LOG(INFO,"--------------------------------------------------------");

    img_convert_ctx = sws_getContext(pCodecCtx->width,
                                     pCodecCtx->height,
                                     pCodecCtx->pix_fmt,
                                     pCodecCtx->width,
                                     pCodecCtx->height,
                                     AV_PIX_FMT_YUV420P,
                                     SWS_BICUBIC,
                                     NULL,NULL,NULL);

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO |SDL_INIT_TIMER))
    {
        LOG(ERROR,"Couldn't initialize SDL ",SDL_GetError());
        return -1;
    }

    screen_w = pCodecCtx->width;
    screen_h = pCodecCtx->height;
    screen = SDL_CreateWindow("video player",
                              SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED,
                              screen_w,
                              screen_h,
                              SDL_WINDOW_OPENGL);
    if(!screen)
    {
        LOG(ERROR,"SDL: couldn't create window - exiting: ",SDL_GetError());
        return -1;
    }
    render = SDL_CreateRenderer(screen,-1,0);
    texture = SDL_CreateTexture(render,
                                SDL_PIXELFORMAT_IYUV,
                                SDL_TEXTUREACCESS_STREAMING,
                                pCodecCtx->width,
                                pCodecCtx->height);

    rect.x = 0;
    rect.y = 0;
    rect.w = screen_w;
    rect.h = screen_h;

    packet = (AVPacket*)av_malloc(sizeof(AVPacket));
    video_tid = SDL_CreateThread(sfp_refresh_thread,NULL,NULL);

    for(;;)
    {
        SDL_WaitEvent(&event);
        if(event.type == SFM_REFRESH_EVENT)
        {
            while(1)
            {
                if(av_read_frame(pFormatCtx,packet) < 0)
                    thread_exit=1;
                if(packet->stream_index == videodex)
                    break;
            }
            ret = avcodec_send_packet(pCodecCtx,packet);
            if(ret < 0)
            {
                LOG(ERROR,"decodec: send packet failed ");
                break;
            }else
            {
                ret = avcodec_receive_frame(pCodecCtx,pFrame);
                if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    LOG(WARN,"need more data or EOF");
                }else if(ret < 0)
                {
                    LOG(ERROR,"decodec: failed");
                }else
                {
                    //解码成功
                    got_picture = 1;
                }
            }
            if(got_picture)
            {
                sws_scale(img_convert_ctx,
                          (const unsigned char* const*)pFrame->data,
                          pFrame->linesize,
                          0,
                          pCodecCtx->height,
                          pFrameYUV->data,
                          pFrameYUV->linesize);

                SDL_UpdateYUVTexture(texture,
                                    &rect,
                                    pFrameYUV->data[0],pFrameYUV->linesize[0],
                                    pFrameYUV->data[1],pFrameYUV->linesize[1],
                                    pFrameYUV->data[2],pFrameYUV->linesize[2]);

                SDL_RenderClear(render);
                SDL_RenderCopy(render,texture,NULL,&rect);
                SDL_RenderPresent(render);
                SDL_Delay(40);
            }
            av_packet_unref(packet);
        }else if(event.type == SDL_KEYDOWN)
        {
            if(event.key.keysym.sym == SDLK_SPACE)
                thread_pause = !thread_pause;
        }else if(event.type == SDL_QUIT)
            thread_exit = 1;
        else if(event.type == SFM_BREAK_EVENT)
            break;
    }
    sws_freeContext(img_convert_ctx);
    SDL_Quit();
    av_frame_free(&pFrameYUV);
    av_frame_free(&pFrame);
    avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);

    return 0;
}



