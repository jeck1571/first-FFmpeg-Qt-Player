# Third-Party Notices

## FFmpeg

This project is distributed with a prebuilt FFmpeg 3.4 x64 SDK in
`ffmpeg3.4-x64-debug/`.

Running the included `ffmpeg.exe -L` reports the LGPL 2.1 or later license
terms. The application dynamically links the FFmpeg shared libraries:

- `avformat-57.dll`
- `avcodec-57.dll`
- `avutil-55.dll`
- `swresample-2.dll`
- `swscale-4.dll`

FFmpeg is free software. If this project is redistributed, keep the FFmpeg
license notice and satisfy the applicable LGPL requirements, including the
requirements for relinking and corresponding source availability. Obtain the
exact matching FFmpeg source from the official FFmpeg source archive for the
version and build configuration you redistribute.

FFmpeg project:

- https://ffmpeg.org/
- https://ffmpeg.org/legal.html

## Qt

This application uses Qt 6 under the license selected by the user who builds
or redistributes it. Qt is available under commercial and open-source license
options. Review the Qt license applicable to your distribution.

Qt project:

- https://www.qt.io/
- https://www.qt.io/licensing/

## MinGW runtime DLLs

The bundled FFmpeg build depends on:

- `libbz2-1.dll`
- `libiconv-2.dll`
- `liblzma-5.dll`
- `libwinpthread-1.dll`
- `zlib1.dll`

These libraries remain subject to their own licenses. Keep their notices and
source-availability obligations when redistributing binaries.

## Sample media

`sample.mp4` is an 8-second test clip used for playback and automation
verification. Do not assume it is suitable as production content. Replace it
with appropriately licensed media in a commercial distribution.

## Project scope

This is an independent FFmpeg + Qt implementation inspired by the desktop
feature set of `vlc-re`. It is not the VLC source code, is not endorsed by the
VideoLAN project, and does not contain libVLC as a runtime dependency.
