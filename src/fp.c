// Frame Pull ProRes core: libavformat (mov) + libavcodec (prores) behind a tiny API.
// File bytes come from JS via the imported js_read (synchronous, in a worker).
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/pixdesc.h"

#define EXPORT(n) __attribute__((export_name(n)))
__attribute__((import_module("env"), import_name("js_read")))
extern int js_read(double offset, uint8_t *buf, int len);

static double g_size, g_pos;
static AVFormatContext *fmt;
static AVCodecContext *dec;
static AVFrame *frame, *keep;
static AVPacket *pkt;
static int vidx = -1;
static int info[32];

static int rd(void *op, uint8_t *buf, int len) {
  if (g_pos >= g_size) return AVERROR_EOF;
  if (len > g_size - g_pos) len = (int)(g_size - g_pos);
  int n = js_read(g_pos, buf, len);
  if (n <= 0) return AVERROR_EOF;
  g_pos += n; return n;
}
static int64_t sk(void *op, int64_t off, int whence) {
  if (whence == AVSEEK_SIZE) return (int64_t)g_size;
  whence &= ~AVSEEK_FORCE;
  if (whence == SEEK_SET) g_pos = (double)off;
  else if (whence == SEEK_CUR) g_pos += (double)off;
  else if (whence == SEEK_END) g_pos = g_size + (double)off;
  else return -1;
  return (int64_t)g_pos;
}

EXPORT("fp_malloc") void *fp_malloc(int n) { return malloc(n); }
EXPORT("fp_free") void fp_free(void *p) { free(p); }
EXPORT("fp_info") int *fp_info(void) { return info; }

// returns 0 on success, negative on failure
static int64_t start_pts, frame_dur, last_pts; static int have_frame, at_eof;
static AVRational fdur_q;
EXPORT("fp_open") int fp_open(double size) {
  av_log_set_level(AV_LOG_QUIET);
  g_size = size; g_pos = 0;
  int bs = 1 << 20;
  uint8_t *iobuf = av_malloc(bs);
  AVIOContext *io = avio_alloc_context(iobuf, bs, 0, NULL, rd, NULL, sk);
  fmt = avformat_alloc_context(); fmt->pb = io;
  if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) return -1;
  if (avformat_find_stream_info(fmt, NULL) < 0) return -2;
  vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (vidx < 0) return -3;
  AVStream *st = fmt->streams[vidx];
  const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
  if (!c) return -4;
  dec = avcodec_alloc_context3(c);
  avcodec_parameters_to_context(dec, st->codecpar);
  dec->thread_count = 1;
  if (avcodec_open2(dec, c, NULL) < 0) return -5;
  frame = av_frame_alloc(); keep = av_frame_alloc(); pkt = av_packet_alloc();
  AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
  if (!fr.num) return -6;
  fdur_q = av_inv_q(fr);
  frame_dur = av_rescale_q(1, fdur_q, st->time_base); if (frame_dur < 1) frame_dur = 1;
  start_pts = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
  have_frame = 0; at_eof = 0;
  info[0] = st->codecpar->width; info[1] = st->codecpar->height;
  info[2] = avformat_index_get_entries_count(st);
  if (st->duration > 0) { int64_t n = av_rescale_q(st->duration, st->time_base, fdur_q); if (n > 0 && n < info[2]) info[2] = (int)n; }
  info[3] = st->time_base.num; info[4] = st->time_base.den;
  info[5] = fr.num; info[6] = fr.den;
  info[7] = st->codecpar->color_primaries; info[8] = st->codecpar->color_trc;
  info[9] = st->codecpar->color_space; info[10] = st->codecpar->color_range;
  info[11] = st->codecpar->codec_tag;
  info[20] = st->codecpar->codec_id == AV_CODEC_ID_PRORES ? 1 : 0;   // intra-only: cheap random access
  return 0;
}

static void fill_frame_info(void) {
  const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(keep->format);
  info[12] = keep->format; info[13] = d->comp[0].depth; info[14] = d->log2_chroma_w; info[15] = d->log2_chroma_h;
  info[16] = keep->color_range; info[17] = keep->colorspace;
  info[18] = keep->width; info[19] = keep->height;
  if (keep->format == AV_PIX_FMT_YUVJ420P || keep->format == AV_PIX_FMT_YUVJ422P || keep->format == AV_PIX_FMT_YUVJ444P) info[16] = AVCOL_RANGE_JPEG;
}

// decode display frame #n (presentation order): pts = start + n * frame duration
EXPORT("fp_decode") int fp_decode(int n) {
  AVStream *st = fmt->streams[vidx];
  int64_t target = start_pts + av_rescale_q(n, fdur_q, st->time_base), half = frame_dur / 2;
  if (have_frame && llabs(last_pts - target) <= half) return 0;
  int forward = have_frame && target > last_pts && !at_eof && (info[20] ? target - last_pts <= frame_dur + half : target - last_pts <= 60 * frame_dur);
  if (!forward) {
    if (av_seek_frame(fmt, vidx, target, AVSEEK_FLAG_BACKWARD) < 0 && av_seek_frame(fmt, vidx, target, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY) < 0) return -2;
    avcodec_flush_buffers(dec); have_frame = 0; at_eof = 0;
  }
  for (int guard = 0; guard < 4000; guard++) {
    int r = avcodec_receive_frame(dec, frame);
    if (r == 0) {
      int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
      if (pts == AV_NOPTS_VALUE) continue;
      have_frame = 1; last_pts = pts;
      av_frame_unref(keep); av_frame_move_ref(keep, frame);
      if (pts >= target - half) { fill_frame_info(); return 0; }   // first frame at/after target (exact for CFR)
      continue;
    }
    if (r == AVERROR_EOF) { if (have_frame) { fill_frame_info(); return 0; } return -3; }
    if (r != AVERROR(EAGAIN)) return -5;
    if (at_eof) return -3;
    r = av_read_frame(fmt, pkt);
    if (r < 0) { at_eof = 1; avcodec_send_packet(dec, NULL); continue; }
    if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }
    r = avcodec_send_packet(dec, pkt); av_packet_unref(pkt);
    if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_INVALIDDATA) return -4;
  }
  return -6;
}

static inline double px(const uint8_t *p, int ls, int x, int y, int wide) {
  return wide ? ((const uint16_t *)(p + (size_t)y * ls))[x] : (p + (size_t)y * ls)[x];
}
// Convert the decoded frame's rect (x0,y0,w,h) to RGBA8, sampling every `step` pixels, with bilinear chroma.
// matrix: 0=709 1=601 2=2020. full: 1 if full range. lut: 4096 entries, 12-bit RGB code -> 8-bit out.
static uint32_t rng_state = 0x9e3779b9u;
static inline double urand(void) { uint32_t x = rng_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; rng_state = x; return (x >> 8) * (1.0 / 16777216.0); }
static inline uint8_t dith(double v) { double q = v * 255.0 + (urand() - urand()); int i = (int)(q + (q >= 0 ? 0.5 : -0.5)); return i < 0 ? 0 : i > 255 ? 255 : (uint8_t)i; }
// lut == NULL: exports. Code values pass straight through (no tonal change) with TPDF dither to 8-bit.
EXPORT("fp_render") int fp_render(int x0, int y0, int w, int h, int step, int matrix, int full, uint8_t *lut, uint8_t *out) {
  if (!keep->data[0]) return -1;
  rng_state = 0x9e3779b9u;
  double Kr = 0.2126, Kb = 0.0722;
  if (matrix == 1) { Kr = 0.299; Kb = 0.114; } else if (matrix == 2) { Kr = 0.2627; Kb = 0.0593; }
  double Kg = 1 - Kr - Kb;
  int depth = info[13], sw = info[14], shh = info[15], wide = depth > 8;
  int cw = (info[18] + (1 << sw) - 1) >> sw, ch = (info[19] + (1 << shh) - 1) >> shh;
  double mx = (double)((1 << depth) - 1), sc = (double)(1 << (depth - 8));
  double yo = full ? 0 : 16 * sc, yr = full ? mx : 219 * sc, co = 128 * sc, cr = full ? mx : 224 * sc;
  double crR = 2 * (1 - Kr), cbB = 2 * (1 - Kb), cbG = -2 * (1 - Kb) * Kb / Kg, crG = -2 * (1 - Kr) * Kr / Kg;
  int ow = w / step, oh = h / step;
  const uint8_t *Y = keep->data[0], *U = keep->data[1], *V = keep->data[2];
  int l0 = keep->linesize[0], l1 = keep->linesize[1], l2 = keep->linesize[2];
  for (int oy = 0; oy < oh; oy++) {
    int sy = y0 + oy * step;
    double cyf = shh ? (sy + 0.5) / (1 << shh) - 0.5 : sy;      // 4:2:0 chroma sits between rows
    if (cyf < 0) cyf = 0;
    int cy0 = (int)cyf, cy1 = cy0 + 1 < ch ? cy0 + 1 : cy0; double fy = cyf - cy0;
    uint8_t *o = out + (size_t)oy * ow * 4;
    for (int ox = 0; ox < ow; ox++) {
      int sx = x0 + ox * step;
      double cxf = sw ? (double)sx / (1 << sw) : sx;            // co-sited horizontally
      int cx0 = (int)cxf, cx1 = cx0 + 1 < cw ? cx0 + 1 : cx0; double fx = cxf - cx0;
      double u = (px(U, l1, cx0, cy0, wide) * (1 - fx) + px(U, l1, cx1, cy0, wide) * fx) * (1 - fy) + (px(U, l1, cx0, cy1, wide) * (1 - fx) + px(U, l1, cx1, cy1, wide) * fx) * fy;
      double v = (px(V, l2, cx0, cy0, wide) * (1 - fx) + px(V, l2, cx1, cy0, wide) * fx) * (1 - fy) + (px(V, l2, cx0, cy1, wide) * (1 - fx) + px(V, l2, cx1, cy1, wide) * fx) * fy;
      double yy = (px(Y, l0, sx, sy, wide) - yo) / yr, cb = (u - co) / cr, cv = (v - co) / cr;
      double r = yy + crR * cv, g = yy + cbG * cb + crG * cv, b = yy + cbB * cb;
      if (!lut) { o[0] = dith(r); o[1] = dith(g); o[2] = dith(b); o[3] = 255; o += 4; continue; }
      int ri = (int)(r * 4095 + 0.5), gi = (int)(g * 4095 + 0.5), bi = (int)(b * 4095 + 0.5);
      ri = ri < 0 ? 0 : ri > 4095 ? 4095 : ri; gi = gi < 0 ? 0 : gi > 4095 ? 4095 : gi; bi = bi < 0 ? 0 : bi > 4095 ? 4095 : bi;
      o[0] = lut[ri]; o[1] = lut[gi]; o[2] = lut[bi]; o[3] = 255; o += 4;
    }
  }
  return ow * oh;
}


/* ---- packet access for hardware (WebCodecs) decoding of long-GOP files ---- */
EXPORT("fp_extradata") uint8_t *fp_extradata(void) {
  AVCodecParameters *p = fmt->streams[vidx]->codecpar;
  info[21] = p->extradata_size; info[22] = p->codec_id == AV_CODEC_ID_H264 ? 1 : p->codec_id == AV_CODEC_ID_HEVC ? 2 : 0;
  return p->extradata;
}
static int64_t pts_to_index(int64_t pts) {
  AVStream *st = fmt->streams[vidx];
  if (pts == AV_NOPTS_VALUE) return -1;
  return av_rescale_q_rnd(pts - start_pts, st->time_base, fdur_q, AV_ROUND_NEAR_INF);
}
// position the demuxer on the keyframe at or before display frame n
EXPORT("fp_pkt_seek") int fp_pkt_seek(int n) {
  AVStream *st = fmt->streams[vidx];
  int64_t target = start_pts + av_rescale_q(n, fdur_q, st->time_base);
  have_frame = 0; at_eof = 0;
  if (av_seek_frame(fmt, vidx, target, AVSEEK_FLAG_BACKWARD) < 0 && av_seek_frame(fmt, vidx, target, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY) < 0) return -2;
  return 0;
}
// next video packet in decode order: returns data pointer (valid until the next call), 0 at end.
// info[25] size, info[26] keyframe, info[27] display index (from pts), info[28] decode index (from dts)
EXPORT("fp_pkt_next") uint8_t *fp_pkt_next(void) {
  av_packet_unref(pkt);
  for (;;) {
    if (av_read_frame(fmt, pkt) < 0) return 0;
    if (pkt->stream_index == vidx) break;
    av_packet_unref(pkt);
  }
  info[25] = pkt->size; info[26] = (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
  info[27] = (int)pts_to_index(pkt->pts); info[28] = (int)pts_to_index(pkt->dts);
  return pkt->data;
}

/* ---- JPEG encoder: quality 95 => stb uses 4:4:4 (no chroma subsampling) ---- */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
static uint8_t *jbuf; static size_t jlen, jcap;
static void jwrite(void *ctx, void *data, int size) {
  if (jlen + size > jcap) { size_t nc = jcap ? jcap * 2 : (1 << 20); while (nc < jlen + size) nc *= 2; uint8_t *nb = realloc(jbuf, nc); if (!nb) return; jbuf = nb; jcap = nc; }
  memcpy(jbuf + jlen, data, size); jlen += size;
}
EXPORT("fp_jpeg") uint8_t *fp_jpeg(uint8_t *rgba, int w, int h, int quality) {
  jlen = 0;
  if (!stbi_write_jpg_to_func(jwrite, NULL, w, h, 4, rgba, quality)) return 0;
  info[29] = (int)jlen; return jbuf;
}
EXPORT("fp_jpeg_free") void fp_jpeg_free(void) { free(jbuf); jbuf = 0; jlen = jcap = 0; }

/* ---- 16-bit PNG master: full decode precision, no dither, sRGB chunk (spec: no gAMA/cHRM/cICP) ---- */
static uint32_t crc_tab[256];
static uint32_t crc_up(uint32_t c, const uint8_t *b, size_t n) {
  if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1; crc_tab[i] = k; }
  c = ~c; for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ b[i]) & 255] ^ (c >> 8); return ~c;
}
static uint8_t *pbuf; static size_t plen, pcap;
static int pput(const void *d, size_t n) {
  if (plen + n > pcap) { size_t nc = pcap ? pcap : (1 << 22); while (nc < plen + n) nc *= 2; uint8_t *nb = realloc(pbuf, nc); if (!nb) return 0; pbuf = nb; pcap = nc; }
  memcpy(pbuf + plen, d, n); plen += n; return 1;
}
static int pchunk(const char *type, const uint8_t *data, uint32_t n) {
  uint8_t h[8] = { n >> 24, n >> 16, n >> 8, n, type[0], type[1], type[2], type[3] };
  uint32_t c = crc_up(0, h + 4, 4); c = crc_up(c, data, n);
  uint8_t t[4] = { c >> 24, c >> 16, c >> 8, c };
  return pput(h, 8) && (n ? pput(data, n) : 1) && pput(t, 4);
}
EXPORT("fp_png16") uint8_t *fp_png16(int x0, int y0, int w, int h, int matrix, int full) {
  if (!keep->data[0]) return 0;
  double Kr = 0.2126, Kb = 0.0722;
  if (matrix == 1) { Kr = 0.299; Kb = 0.114; } else if (matrix == 2) { Kr = 0.2627; Kb = 0.0593; }
  double Kg = 1 - Kr - Kb;
  int depth = info[13], sw = info[14], shh = info[15], wide = depth > 8;
  int cw = (info[18] + (1 << sw) - 1) >> sw, ch = (info[19] + (1 << shh) - 1) >> shh;
  double mx = (double)((1 << depth) - 1), sc = (double)(1 << (depth - 8));
  double yo = full ? 0 : 16 * sc, yr = full ? mx : 219 * sc, co = 128 * sc, cr = full ? mx : 224 * sc;
  double crR = 2 * (1 - Kr), cbB = 2 * (1 - Kb), cbG = -2 * (1 - Kb) * Kb / Kg, crG = -2 * (1 - Kr) * Kr / Kg;
  const uint8_t *Y = keep->data[0], *U = keep->data[1], *V = keep->data[2];
  int l0 = keep->linesize[0], l1 = keep->linesize[1], l2 = keep->linesize[2];
  size_t stride = (size_t)w * 6 + 1;
  uint8_t *raw = malloc(stride * h), *row = malloc((size_t)w * 6); if (!raw || !row) { free(raw); free(row); return 0; }
  for (int oy = 0; oy < h; oy++) {
    int sy = y0 + oy;
    double cyf = shh ? (sy + 0.5) / (1 << shh) - 0.5 : sy; if (cyf < 0) cyf = 0;
    int cy0 = (int)cyf, cy1 = cy0 + 1 < ch ? cy0 + 1 : cy0; double fy = cyf - cy0;
    for (int ox = 0; ox < w; ox++) {
      int sx = x0 + ox;
      double cxf = sw ? (double)sx / (1 << sw) : sx;
      int cx0 = (int)cxf, cx1 = cx0 + 1 < cw ? cx0 + 1 : cx0; double fx = cxf - cx0;
      double u = (px(U, l1, cx0, cy0, wide) * (1 - fx) + px(U, l1, cx1, cy0, wide) * fx) * (1 - fy) + (px(U, l1, cx0, cy1, wide) * (1 - fx) + px(U, l1, cx1, cy1, wide) * fx) * fy;
      double v = (px(V, l2, cx0, cy0, wide) * (1 - fx) + px(V, l2, cx1, cy0, wide) * fx) * (1 - fy) + (px(V, l2, cx0, cy1, wide) * (1 - fx) + px(V, l2, cx1, cy1, wide) * fx) * fy;
      double yy = (px(Y, l0, sx, sy, wide) - yo) / yr, cb = (u - co) / cr, cv = (v - co) / cr;
      double c3[3] = { yy + crR * cv, yy + cbG * cb + crG * cv, yy + cbB * cb };
      for (int k = 0; k < 3; k++) { double t = c3[k] < 0 ? 0 : c3[k] > 1 ? 1 : c3[k]; unsigned q = (unsigned)(t * 65535.0 + 0.5); row[ox * 6 + k * 2] = q >> 8; row[ox * 6 + k * 2 + 1] = q & 255; }
    }
    uint8_t *o = raw + oy * stride; o[0] = 1;                    // PNG filter: Sub (6 bytes per pixel)
    for (size_t i = 0; i < (size_t)w * 6; i++) o[1 + i] = (uint8_t)(row[i] - (i >= 6 ? row[i - 6] : 0));
  }
  free(row);
  int zlen = 0; uint8_t *z = stbi_zlib_compress(raw, (int)(stride * h), &zlen, 5); free(raw);
  if (!z) return 0;
  plen = 0;
  static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  uint8_t ihdr[13] = { w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 16, 2, 0, 0, 0 }, srgb = 0;
  int ok = pput(sig, 8) && pchunk("IHDR", ihdr, 13) && pchunk("sRGB", &srgb, 1) && pchunk("IDAT", z, zlen) && pchunk("IEND", 0, 0);
  STBIW_FREE(z);
  if (!ok) return 0;
  info[29] = (int)plen; return pbuf;
}
EXPORT("fp_png16_free") void fp_png16_free(void) { free(pbuf); pbuf = 0; plen = pcap = 0; }
