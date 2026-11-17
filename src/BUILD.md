# Building Frame Pull

`index.html` is generated from `src/index.src.html` by filling its placeholders:

- `/*__CORE__*/` → `src/core.js` (up to `// ---- end core ----`)
- `__WASM_B64__` → base64 of `fp.wasm`
- `__CHART_B64__` → base64 of the 640×360 ProRes 422 HQ test chart (Rec.709 tagged, legal range; patches 0/26/128/204/255 and 191,64,64 / 64,160,64 / 64,64,191)
- `__ICC_B64__` → base64 of a compact sRGB ICC profile (`ImageCms.createProfile('sRGB')`)

## fp.wasm

FFmpeg n7.1 (LGPL 2.1), cross-compiled with `zig cc -target wasm32-wasi`:

```
./configure --enable-cross-compile --target-os=none --arch=generic --cpu=generic \
  --disable-asm --disable-inline-asm --disable-pthreads --disable-programs --disable-doc \
  --disable-network --disable-avdevice --disable-swresample --disable-swscale --disable-avfilter \
  --disable-everything --enable-decoder=prores,h264,hevc --enable-demuxer=mov --enable-parser=h264,hevc \
  --disable-debug --disable-runtime-cpudetect --disable-autodetect --disable-iconv --disable-zlib --disable-bzlib --disable-lzma
# then set HAVE_MMAP, HAVE_SYSCONF, HAVE_SYSCTL, HAVE_GETRUSAGE, HAVE_ISATTY to 0 in config.h
make libavformat/libavformat.a libavcodec/libavcodec.a libavutil/libavutil.a
zig cc -target wasm32-wasi -O3 -s -mexec-model=reactor -I. -I<stb> src/fp.c \
  libavformat/libavformat.a libavcodec/libavcodec.a libavutil/libavutil.a -o fp.wasm
```

`fp.c` also includes `stb_image_write.h` (public domain) for the JPEG encoder: quality 95, which makes stb use 4:4:4.

## Colour rules (see the framepull export spec)

Decode Y′CbCr with the file's matrix and range (untagged: BT.709 at ≥720 lines, BT.601 below, legal range), keep R′G′B′ code values, TPDF-dither to 8-bit, label sRGB. No tonal or gamut conversion. Never take frames from a `<video>` element.
