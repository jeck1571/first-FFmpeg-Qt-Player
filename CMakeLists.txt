cmake_minimum_required(VERSION 3.24)

project(FFmpegQtPlayer VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui GuiPrivate Widgets Multimedia Network Test)

set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg3.4-x64-debug")

qt_add_executable(FFmpegQtPlayer WIN32
    main.cpp
    MainWindow.h
    MainWindow.cpp
    AiRecognitionService.h
    AiRecognitionService.cpp
    AiContentDialog.h
    AiContentDialog.cpp
    CrashReporter.h
    CrashReporter.cpp
    PlaybackClock.h
    PlaybackClock.cpp
    PacketQueue.h
    PacketQueue.cpp
    StreamingPolicy.h
    StreamingPolicy.cpp
    HlsPlaylist.h
    HlsPlaylist.cpp
    VideoFramePool.h
    VideoFramePool.cpp
    VideoRenderFrame.h
    RhiVideoSurface.h
    RhiVideoSurface.cpp
    FFmpegPlayer.h
    FFmpegPlayer.cpp
    VideoCanvas.h
    VideoCanvas.cpp
    VideoOverlay.h
    VideoOverlay.cpp
    SubtitleTrack.h
    SubtitleTrack.cpp
    Language.h
    Language.cpp
)

target_include_directories(FFmpegQtPlayer PRIVATE
    "${FFMPEG_ROOT}/include"
)

target_link_directories(FFmpegQtPlayer PRIVATE
    "${FFMPEG_ROOT}/bin"
)

target_link_libraries(FFmpegQtPlayer PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Network
    d3d11
    dbghelp
    avformat
    avcodec
    avutil
    avfilter
    swresample
    swscale
)

qt_add_resources(FFmpegQtPlayer "player_shaders"
    PREFIX "/"
    FILES
        "shader_assets/video.vert.qsb"
        "shader_assets/video.frag.qsb"
)

target_compile_definitions(FFmpegQtPlayer PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(FFmpegQtPlayer PRIVATE
        /utf-8
        /permissive-
        /W3
    )
endif()

set(FFMPEG_RUNTIME_DLLS
    avcodec-57.dll
    avdevice-57.dll
    avfilter-6.dll
    avformat-57.dll
    avutil-55.dll
    swresample-2.dll
    swscale-4.dll
)

# This prebuilt FFmpeg SDK was produced with MinGW and therefore also needs
# its compression, iconv, and pthread support DLLs at runtime.
set(FFMPEG_RUNTIME_AUX_DLLS
    libbz2-1.dll
    libiconv-2.dll
    liblzma-5.dll
    libwinpthread-1.dll
    zlib1.dll
)

foreach(FFMPEG_DLL IN LISTS FFMPEG_RUNTIME_DLLS FFMPEG_RUNTIME_AUX_DLLS)
    add_custom_command(TARGET FFmpegQtPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_ROOT}/bin/${FFMPEG_DLL}"
            "$<TARGET_FILE_DIR:FFmpegQtPlayer>/${FFMPEG_DLL}"
    )
endforeach()

get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
get_filename_component(QT_BINARY_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)

# qtbase_zh_CN.qm localizes the standard Qt dialogs and buttons (OK, Cancel,
# message boxes) when the UI language is Chinese.
find_file(QTBASE_ZH_QM
    NAMES qtbase_zh_CN.qm
    HINTS "${QT_BINARY_DIR}/../translations"
)
if(QTBASE_ZH_QM)
    add_custom_command(TARGET FFmpegQtPlayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:FFmpegQtPlayer>/translations"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QTBASE_ZH_QM}"
            "$<TARGET_FILE_DIR:FFmpegQtPlayer>/translations/qtbase_zh_CN.qm"
    )
endif()

find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BINARY_DIR}")
if(WIN32 AND WINDEPLOYQT_EXECUTABLE)
    add_custom_command(TARGET FFmpegQtPlayer POST_BUILD
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            --no-translations
            "$<TARGET_FILE:FFmpegQtPlayer>"
    )
endif()

include(CTest)
if(BUILD_TESTING AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    qt_add_executable(PlaybackCoreTests
        tests/PlaybackCoreTests.cpp
        PlaybackClock.h
        PlaybackClock.cpp
        PacketQueue.h
        PacketQueue.cpp
        StreamingPolicy.h
        StreamingPolicy.cpp
        HlsPlaylist.h
        HlsPlaylist.cpp
        VideoFramePool.h
        VideoFramePool.cpp
    )
    target_include_directories(PlaybackCoreTests PRIVATE
        "${FFMPEG_ROOT}/include"
    )
    target_link_directories(PlaybackCoreTests PRIVATE
        "${FFMPEG_ROOT}/bin"
    )
    target_link_libraries(PlaybackCoreTests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Test
        avformat
        avcodec
        avutil
    )
    if(MSVC)
        target_compile_options(PlaybackCoreTests PRIVATE
            /utf-8
            /permissive-
            /W3
        )
    endif()
    target_compile_definitions(PlaybackCoreTests PRIVATE
        FFMPEG_PLAYER_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    if(WIN32 AND WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET PlaybackCoreTests POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                "$<TARGET_FILE:PlaybackCoreTests>"
        )
    endif()
    add_test(NAME PlaybackCoreTests COMMAND PlaybackCoreTests)

    qt_add_executable(CrashReporterSmoke
        tests/CrashReporterSmoke.cpp
        CrashReporter.h
        CrashReporter.cpp
    )
    target_link_libraries(CrashReporterSmoke PRIVATE
        Qt6::Core
        dbghelp
    )
    if(MSVC)
        target_compile_options(CrashReporterSmoke PRIVATE
            /utf-8
            /permissive-
            /W3
        )
    endif()
    add_test(NAME CrashReporterSmoke COMMAND CrashReporterSmoke)
    set_tests_properties(CrashReporterSmoke PROPERTIES TIMEOUT 20)

    qt_add_executable(PlayerSmoke
        tests/PlayerSmoke.cpp
        FFmpegPlayer.h
        FFmpegPlayer.cpp
        PlaybackClock.h
        PlaybackClock.cpp
        PacketQueue.h
        PacketQueue.cpp
        StreamingPolicy.h
        StreamingPolicy.cpp
        HlsPlaylist.h
        HlsPlaylist.cpp
        VideoFramePool.h
        VideoFramePool.cpp
        VideoRenderFrame.h
        RhiVideoSurface.h
        RhiVideoSurface.cpp
        VideoCanvas.h
        VideoCanvas.cpp
        VideoOverlay.h
        VideoOverlay.cpp
    )
    target_include_directories(PlayerSmoke PRIVATE
        "${FFMPEG_ROOT}/include"
    )
    target_link_directories(PlayerSmoke PRIVATE
        "${FFMPEG_ROOT}/bin"
    )
    target_link_libraries(PlayerSmoke PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Widgets
        Qt6::Multimedia
        Qt6::Network
        d3d11
        avformat
        avcodec
        avutil
        avfilter
        swresample
        swscale
    )
    if(MSVC)
        target_compile_options(PlayerSmoke PRIVATE
            /utf-8
            /permissive-
            /W3
        )
    endif()
    if(WIN32 AND WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET PlayerSmoke POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                "$<TARGET_FILE:PlayerSmoke>"
        )
    endif()
    add_test(
        NAME PlayerSmoke
        COMMAND PlayerSmoke "${CMAKE_CURRENT_SOURCE_DIR}/sample.mp4"
    )
    set_tests_properties(PlayerSmoke PROPERTIES TIMEOUT 20)

    qt_add_executable(NetworkRecoverySmoke
        tests/NetworkRecoverySmoke.cpp
        FFmpegPlayer.h
        FFmpegPlayer.cpp
        PlaybackClock.h
        PlaybackClock.cpp
        PacketQueue.h
        PacketQueue.cpp
        StreamingPolicy.h
        StreamingPolicy.cpp
        HlsPlaylist.h
        HlsPlaylist.cpp
        VideoFramePool.h
        VideoFramePool.cpp
        VideoRenderFrame.h
        RhiVideoSurface.h
        RhiVideoSurface.cpp
        VideoCanvas.h
        VideoCanvas.cpp
        VideoOverlay.h
        VideoOverlay.cpp
    )
    target_include_directories(NetworkRecoverySmoke PRIVATE
        "${FFMPEG_ROOT}/include"
    )
    target_link_directories(NetworkRecoverySmoke PRIVATE
        "${FFMPEG_ROOT}/bin"
    )
    target_link_libraries(NetworkRecoverySmoke PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Widgets
        Qt6::Multimedia
        Qt6::Network
        d3d11
        avformat
        avcodec
        avutil
        avfilter
        swresample
        swscale
    )
    if(MSVC)
        target_compile_options(NetworkRecoverySmoke PRIVATE
            /utf-8
            /permissive-
            /W3
        )
    endif()
    if(WIN32 AND WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET NetworkRecoverySmoke POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                "$<TARGET_FILE:NetworkRecoverySmoke>"
        )
    endif()
    add_test(
        NAME NetworkRecoverySmoke
        COMMAND NetworkRecoverySmoke "${CMAKE_CURRENT_SOURCE_DIR}/sample.mp4"
    )
    set_tests_properties(NetworkRecoverySmoke PROPERTIES TIMEOUT 25)

    qt_add_executable(TrackSwitchSmoke
        tests/TrackSwitchSmoke.cpp
        FFmpegPlayer.h
        FFmpegPlayer.cpp
        PlaybackClock.h
        PlaybackClock.cpp
        PacketQueue.h
        PacketQueue.cpp
        StreamingPolicy.h
        StreamingPolicy.cpp
        HlsPlaylist.h
        HlsPlaylist.cpp
        VideoFramePool.h
        VideoFramePool.cpp
        VideoRenderFrame.h
        RhiVideoSurface.h
        RhiVideoSurface.cpp
        VideoCanvas.h
        VideoCanvas.cpp
        VideoOverlay.h
        VideoOverlay.cpp
    )
    target_include_directories(TrackSwitchSmoke PRIVATE
        "${FFMPEG_ROOT}/include"
    )
    target_link_directories(TrackSwitchSmoke PRIVATE
        "${FFMPEG_ROOT}/bin"
    )
    target_link_libraries(TrackSwitchSmoke PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Widgets
        Qt6::Multimedia
        Qt6::Network
        d3d11
        avformat
        avcodec
        avutil
        avfilter
        swresample
        swscale
    )
    if(MSVC)
        target_compile_options(TrackSwitchSmoke PRIVATE
            /utf-8
            /permissive-
            /W3
        )
    endif()
    if(WIN32 AND WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET TrackSwitchSmoke POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                "$<TARGET_FILE:TrackSwitchSmoke>"
        )
    endif()
    add_test(
        NAME TrackSwitchSmoke
        COMMAND TrackSwitchSmoke
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/track-switch.mp4"
    )
    set_tests_properties(TrackSwitchSmoke PROPERTIES TIMEOUT 15)

    qt_add_executable(GpuRenderSmoke
        tests/GpuRenderSmoke.cpp
        FFmpegPlayer.h
        FFmpegPlayer.cpp
        PlaybackClock.h
        PlaybackClock.cpp
        PacketQueue.h
        PacketQueue.cpp
        StreamingPolicy.h
        StreamingPolicy.cpp
        HlsPlaylist.h
        HlsPlaylist.cpp
        VideoFramePool.h
        VideoFramePool.cpp
        VideoRenderFrame.h
        RhiVideoSurface.h
        RhiVideoSurface.cpp
        VideoCanvas.h
        VideoCanvas.cpp
        VideoOverlay.h
        VideoOverlay.cpp
    )
    qt_add_resources(GpuRenderSmoke "gpu_smoke_shaders"
        PREFIX "/"
        FILES
            "shader_assets/video.vert.qsb"
            "shader_assets/video.frag.qsb"
    )
    target_include_directories(GpuRenderSmoke PRIVATE
        "${FFMPEG_ROOT}/include"
    )
    target_link_directories(GpuRenderSmoke PRIVATE
        "${FFMPEG_ROOT}/bin"
    )
    target_link_libraries(GpuRenderSmoke PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Widgets
        Qt6::Multimedia
        Qt6::Network
        d3d11
        avformat
        avcodec
        avutil
        avfilter
        swresample
        swscale
    )
    if(MSVC)
        target_compile_options(GpuRenderSmoke PRIVATE
            /utf-8
            /permissive-
            /W3
        )
    endif()
    if(WIN32 AND WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET GpuRenderSmoke POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                "$<TARGET_FILE:GpuRenderSmoke>"
        )
    endif()
    add_test(
        NAME GpuRenderSmoke
        COMMAND GpuRenderSmoke "${CMAKE_CURRENT_SOURCE_DIR}/sample.mp4"
    )
    set_tests_properties(GpuRenderSmoke PROPERTIES
        TIMEOUT 25
        LABELS "gpu"
    )

    qt_add_executable(PlayerBench
        tests/PlayerBench.cpp
        FFmpegPlayer.h
        FFmpegPlayer.cpp
        PlaybackClock.h
        PlaybackClock.cpp
        PacketQueue.h
        PacketQueue.cpp
        StreamingPolicy.h
        StreamingPolicy.cpp
        HlsPlaylist.h
        HlsPlaylist.cpp
        VideoFramePool.h
        VideoFramePool.cpp
        VideoRenderFrame.h
        RhiVideoSurface.h
        RhiVideoSurface.cpp
        VideoCanvas.h
        VideoCanvas.cpp
        VideoOverlay.h
        VideoOverlay.cpp
    )
    target_include_directories(PlayerBench PRIVATE
        "${FFMPEG_ROOT}/include"
    )
    target_link_directories(PlayerBench PRIVATE
        "${FFMPEG_ROOT}/bin"
    )
    target_link_libraries(PlayerBench PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Widgets
        Qt6::Multimedia
        Qt6::Network
        avformat
        avcodec
        avutil
        avfilter
        swresample
        swscale
    )
    if(WIN32)
        target_link_libraries(PlayerBench PRIVATE psapi)
    endif()
    if(MSVC)
        target_compile_options(PlayerBench PRIVATE
            /utf-8
            /permissive-
            /W3
        )
    endif()
    if(WIN32 AND WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET PlayerBench POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --no-translations
                "$<TARGET_FILE:PlayerBench>"
        )
    endif()

    qt_add_executable(AiRecognitionServiceSmoke
        tests/AiRecognitionServiceSmoke.cpp
        AiRecognitionService.h
        AiRecognitionService.cpp
    )
    target_link_libraries(AiRecognitionServiceSmoke PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
    )
    if(MSVC)
        target_compile_options(AiRecognitionServiceSmoke PRIVATE
            /utf-8
            /permissive-
            /W3
        )
    endif()
endif()

