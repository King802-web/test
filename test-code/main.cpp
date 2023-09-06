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
}

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
    std::filesystem::path currentPath = std::filesystem::current_path();
    std::string filePath = (currentPath / "test.mp4").string();
    //LOG(INFO,filePath);
  // Initalizing these to NULL prevents segfaults!
  AVFormatContext   *pFormatCtx = NULL;
  int               i, videoStream;
  AVCodecContext    *pCodecCtxOrig = NULL;
  AVCodecContext    *pCodecCtx = NULL;
  const AVCodec     *pCodec = NULL;
  AVFrame           *pFrame = NULL;
  AVFrame           *pFrameRGB = NULL;
  AVPacket          packet;
  int               frameFinished;
  int               numBytes;
  uint8_t           *buffer = NULL;
  struct SwsContext *sws_ctx = NULL;

//   if(argc < 2)
//       {
//         LOG(ERROR,"No imput file");
//         return -1;
//       }
  // Open video file
  if(avformat_open_input(&pFormatCtx, filePath.c_str(), NULL, NULL)!=0)
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
  av_dump_format(pFormatCtx, 0, filePath.c_str(), 0);

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

  // Allocate an AVFrame structure
  pFrameRGB=av_frame_alloc();
  if(pFrameRGB==NULL)
      {
          LOG(ERROR,"Allocate an AVFrame structure failed");
          return -1;
      }

  // Determine required buffer size and allocate buffer
  numBytes=av_image_get_buffer_size(AV_PIX_FMT_RGB24, pCodecCtx->width,
                  pCodecCtx->height,1);
  buffer=(uint8_t *)av_malloc(numBytes * sizeof(uint8_t));

  // Assign appropriate parts of buffer to image planes in pFrameRGB
  // Note that pFrameRGB is an AVFrame, but AVFrame is a superset
  // of AVPicture
  av_image_fill_arrays(pFrameRGB->data,pFrameRGB->linesize,buffer,
                       AV_PIX_FMT_RGB24,pCodecCtx->width,pCodecCtx->height,1);

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
          // Convert the image from its native format to RGB
          sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data,
          pFrame->linesize, 0, pCodecCtx->height,
          pFrameRGB->data, pFrameRGB->linesize);

         // Save the frame to disk
          if(++i<=5)
            SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height,i);
      }
    }

    // Free the packet that was allocated by av_read_frame
    av_packet_unref(&packet);
  }

  // Free the RGB image
  av_free(buffer);
  av_frame_free(&pFrameRGB);

  // Free the YUV frame
  av_frame_free(&pFrame);

  // Close the codecs
  avcodec_close(pCodecCtx);
  avcodec_close(pCodecCtxOrig);

  // Close the video file
  avformat_close_input(&pFormatCtx);

  return 0;
}