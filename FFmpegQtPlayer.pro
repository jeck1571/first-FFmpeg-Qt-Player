QT += core gui gui-private widgets multimedia network

CONFIG += c++17 windows
TEMPLATE = app
TARGET = FFmpegQtPlayer

SOURCES += \
    main.cpp \
    MainWindow.cpp \
    AiRecognitionService.cpp \
    AiContentDialog.cpp \
    CrashReporter.cpp \
    PlaybackClock.cpp \
    PacketQueue.cpp \
    StreamingPolicy.cpp \
    HlsPlaylist.cpp \
    VideoFramePool.cpp \
    RhiVideoSurface.cpp \
    FFmpegPlayer.cpp \
    VideoCanvas.cpp \
    VideoOverlay.cpp \
    SubtitleTrack.cpp \
    Language.cpp

HEADERS += \
    MainWindow.h \
    AiRecognitionService.h \
    AiContentDialog.h \
    CrashReporter.h \
    PlaybackClock.h \
    PacketQueue.h \
    StreamingPolicy.h \
    HlsPlaylist.h \
    VideoFramePool.h \
    VideoRenderFrame.h \
    RhiVideoSurface.h \
    D3d11ContextSync.h \
    FFmpegPlayer.h \
    VideoCanvas.h \
    VideoOverlay.h \
    SubtitleTrack.h \
    Language.h

RESOURCES += resources.qrc

FFMPEG_ROOT = $$PWD/ffmpeg3.4-x64-debug
INCLUDEPATH += $$FFMPEG_ROOT/include
LIBS += -L$$FFMPEG_ROOT/bin -lavformat -lavcodec -lavutil -lavfilter -lswresample -lswscale
LIBS += -ld3d11 -ldbghelp

QMAKE_CXXFLAGS += /utf-8 /permissive-
DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS

DESTDIR = $$PWD/bin
OBJECTS_DIR = $$PWD/obj
MOC_DIR = $$PWD/obj/moc

ffmpeg_dlls = avcodec-57.dll avdevice-57.dll avfilter-6.dll avformat-57.dll \
    avutil-55.dll swresample-2.dll swscale-4.dll \
    libbz2-1.dll libiconv-2.dll liblzma-5.dll libwinpthread-1.dll zlib1.dll
for(dll, ffmpeg_dlls) {
    QMAKE_POST_LINK += $$quote(cmd /c copy /y \"$$shell_path($$FFMPEG_ROOT/bin/$$dll)\" \"$$shell_path($$DESTDIR)\" >NUL) $$escape_expand(\\n\\t)
}
