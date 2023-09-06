TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

INCLUDEPATH += $$PWD/include

LIBS += $$PWD/lib/ffmpeg/avcodec.lib \
        $$PWD/lib/ffmpeg/avdevice.lib \
        $$PWD/lib/ffmpeg/avfilter.lib \
        $$PWD/lib/ffmpeg/avformat.lib \
        $$PWD/lib/ffmpeg/avutil.lib \
        $$PWD/lib/ffmpeg/postproc.lib \
        $$PWD/lib/ffmpeg/swresample.lib \
        $$PWD/lib/ffmpeg/swscale.lib

INCLUDEPATH += $$PWD/SDL2/include

LIBS += -L$$PWD/lib/SDL2/x64/ -lSDL2

SOURCES += \
    main.cpp

HEADERS += \
    logger.h
