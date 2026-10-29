A minimal static build of FFmpeg 9.0.2 (libavcodec + libavutil, LGPL 2.1 or later) with only the MPEG-4 Part 2
video decoder, used by `src/vdec_sw.c` for `mp4v` sources (the Vita's own MPEG-4 decoder, sceM4vdec, has no public
interface). Source: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz, unmodified, configured with:

```
./configure --enable-cross-compile --cross-prefix=arm-vita-eabi- --disable-shared --enable-static \
  --disable-runtime-cpudetect --disable-programs --disable-doc --disable-network --disable-everything \
  --enable-decoder=mpeg4 --enable-parser=mpeg4video --enable-small --disable-debug --arch=armv7-a --cpu=cortex-a9 \
  --target-os=none --disable-pthreads --disable-iconv --disable-bzlib --disable-lzma --disable-zlib \
  --disable-avformat --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
  --extra-cflags="-std=gnu11 -O2 -D_BSD_SOURCE -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types"
make && make install
```

Built without threads: it is only used from one loader thread at a time. Rebuild the same way to update or relink.
