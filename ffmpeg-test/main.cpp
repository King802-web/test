#include "logger.h"
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
