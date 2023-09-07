#include "logger.h"
//#include <stdio.h>
#include <iostream>
#include <string>
#include <filesystem>

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
    // std::filesystem::path currentPath = std::filesystem::current_path();
    // std::string filePath = (currentPath / "test.mp4").string();
    const char* filename = "/home/king/workspace/ffplay_test/test/test-code/test.mp4";
    LOG(INFO,filename);
    // Initalizing these to NULL prevents segfaults!
    AVFormatContext   *pFormatCtx = NULL;
    int               i, videoStream;
    AVCodecContext    *pCodecCtxOrig = NULL;
    AVCodecContext    *pCodecCtx = NULL;
    const AVCodec     *pCodec = NULL;
    AVFrame           *pFrame = NULL;
    //AVFrame           *pFrameRGB = NULL;
    AVPacket          packet;
    int               frameFinished;
    //int               numBytes;
    //uint8_t           *buffer = NULL;
    float             aspect_ratio;
    struct SwsContext *sws_ctx = NULL;

    SDL_Window        *window_;
    SDL_Renderer      *rendere_;
    SDL_Texture       *texture_;

    // Open video file
    if(avformat_open_input(&pFormatCtx, filename, NULL, NULL)!=0)
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
    av_dump_format(pFormatCtx, 0, filename, 0);

    // Find the first video stream
    videoStream=-1;
    for(i=0; i<pFormatCtx->nb_streams; i++)
      if(pFormatCtx->streams[i]->codecpar->codec_type==AVMEDIA_TYPE_VIDEO)
        {
          videoStream=i;
          break;
        }
    if(videoStream==-1)
        {
            LOG(ERROR,"Didn't find a video stream");
            return -1;
        }

    // Get a pointer to the codec context for the video stream
    pCodecCtxOrig = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(pCodecCtxOrig,pFormatCtx->streams[videoStream]->codecpar);
    // Find the decoder for the video stream
    pCodec=avcodec_find_decoder(pCodecCtxOrig->codec_id);
    if(pCodec==NULL)
        {
          LOG(ERROR,"Unsupported codec!");
          return -1; // Codec not found
        }
    // Copy context
    pCodecCtx = avcodec_alloc_context3(pCodec);
    avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoStream]->codecpar);

    // Open codec
    if(avcodec_open2(pCodecCtx, pCodec, NULL)<0)
        {
            LOG(ERROR,"Could not open codec");
            return -1;
        }
    // Allocate video frame
    pFrame=av_frame_alloc();

    //make a screen to put our video
    SDL_Init(SDL_INIT_VIDEO);
    window_ = SDL_CreateWindow("video PLayer",
                               SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED,
                               pCodecCtx->width * 0.25,
                               pCodecCtx->height * 0.25,
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
                                          pCodecCtx->width,
                                          pCodecCtx->height);
    // 设置渲染器绘制颜色为黑色
    SDL_SetRenderDrawColor(rendere_, 0, 0, 0, 255);

    // 清空渲染器
    SDL_RenderClear(rendere_);

    // 更新窗口显示
    SDL_RenderPresent(rendere_);

    // initialize SWS context for software scaling
    sws_ctx = sws_getContext(pCodecCtx->width,
                pCodecCtx->height,
                pCodecCtx->pix_fmt,
                pCodecCtx->width,
                pCodecCtx->height,
                AV_PIX_FMT_RGB24,
                SWS_BILINEAR,
                NULL,
                NULL,
                NULL
                );

    // Read frames and save first five frames to disk
    i=0;
    while(av_read_frame(pFormatCtx, &packet)>=0)
    {
      // Is this a packet from the video stream?
      if(packet.stream_index==videoStream)
      {
        // Decode video frame
          avcodec_send_packet(pCodecCtx,&packet);
          frameFinished = avcodec_receive_frame(pCodecCtx,pFrame);

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
      }

      // Free the packet that was allocated by av_read_frame
      av_packet_unref(&packet);
    }

    // Free the YUV frame
    av_frame_free(&pFrame);

    // Close the codecs
    avcodec_close(pCodecCtx);
    avcodec_close(pCodecCtxOrig);

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