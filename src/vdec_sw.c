/* vdec_sw.c - MPEG-4 Part 2 video (the 'mp4v' sample entry: Simple / Advanced Simple Profile, e.g. PSP videos) in
 * software, with FFmpeg's mpeg4 decoder (third_party/ffmpeg: a minimal LGPL build with only that decoder). The Vita's
 * hardware has an MPEG-4 decoder too (sceM4vdec), but its interface isn't documented.
 *
 * Same strategy as the H.264 path (vdec.c): only the frames the project shows are kept, decoding jumps to the
 * keyframe before each run of wanted frames. Packets carry their display index as PTS. */
#include "common.h"
#include "media.h"
#include "mp4.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>

/* planar 4:2:0 -> RGBA bytes, BT.601; limited range (16-235) unless the stream says full range (PSP videos do) */
static void yuv420_to_rgba(const AVFrame *f, int w, int h, uint8_t *dst)
{
    int full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    int ky = full ? 256 : 298, y0 = full ? 0 : 16;
    int krv = full ? 359 : 409, kgu = full ? 88 : 100, kgv = full ? 183 : 208, kbu = full ? 454 : 516;
    for (int y = 0; y < h; y++) {
        const uint8_t *Y = f->data[0] + (size_t)y * f->linesize[0];
        const uint8_t *U = f->data[1] + (size_t)(y >> 1) * f->linesize[1];
        const uint8_t *V = f->data[2] + (size_t)(y >> 1) * f->linesize[2];
        uint8_t *o = dst + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            int c = ky * ((int)Y[x] - y0), d = (int)U[x >> 1] - 128, e = (int)V[x >> 1] - 128;
            int r = (c + krv * e + 128) >> 8, g = (c - kgu * d - kgv * e + 128) >> 8, b = (c + kbu * d + 128) >> 8;
            o[0] = (uint8_t)CLAMP(r, 0, 255); o[1] = (uint8_t)CLAMP(g, 0, 255); o[2] = (uint8_t)CLAMP(b, 0, 255); o[3] = 255;
            o += 4;
        }
    }
}

/* FFmpeg's own warnings and errors go to the log */
static void av_log_to_ours(void *avcl, int level, const char *fmt, va_list vl)
{
    (void)avcl;
    if (level > AV_LOG_WARNING) return;
    char b[256];
    vsnprintf(b, sizeof b, fmt, vl);
    size_t n = strlen(b);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    LOG("ffmpeg: %s", b);
}

typedef struct {
    VideoCache *vc;
    const uint8_t *need;
    uint8_t *rgba, *scratch;
    int w, h;
    int produced;
    int bad_format;
    int got, rej_range, rej_need, rej_have, rej_full, logged;   /* diagnostics */
} SwCtx;

static void keep_frame(SwCtx *c, const AVFrame *f)
{
    int64_t k = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    VideoCache *vc = c->vc;
    c->got++;
    if (c->logged < 4) {
        c->logged++;
        LOG("vdec_sw: frame out: pts %lld, best effort %lld, format %d, %dx%d, range %d", (long long)f->pts,
            (long long)f->best_effort_timestamp, f->format, f->width, f->height, (int)f->color_range);
    }
    if (k < 0 || k >= vc->nframes) { c->rej_range++; return; }
    if (!c->need[k]) { c->rej_need++; return; }
    if (vc->slot[k] >= 0) { c->rej_have++; return; }
    int s = c->produced;   /* cache slots are filled in order */
    if (s >= vc->nimg) { c->rej_full++; return; }
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
        if (!c->bad_format++) LOG("vdec_sw: unexpected pixel format %d", f->format);
        return;
    }
    yuv420_to_rgba(f, c->w, c->h, c->rgba);
    downscale_rgba(c->rgba, c->w * 4, c->w, c->h, &vc->img[s], 0, c->scratch);
    if (vc->imgq && vc->imgq[s].data) downscale_rgba(c->rgba, c->w * 4, c->w, c->h, &vc->imgq[s], 0, c->scratch);
    vc->slot[k] = s;
    c->produced++;
}

static void receive_all(AVCodecContext *ctx, AVFrame *fr, SwCtx *c)
{
    static int logged;
    int r;
    while ((r = avcodec_receive_frame(ctx, fr)) == 0) {
        keep_frame(c, fr);
        av_frame_unref(fr);
    }
    if (r != AVERROR(EAGAIN) && r != AVERROR_EOF && logged++ < 8) LOG("vdec_sw: receive_frame: %d", r);
}

/* the frames still held back for reordering, then a clean decoder for the next run */
static void sw_drain(AVCodecContext *ctx, AVFrame *fr, SwCtx *c)
{
    avcodec_send_packet(ctx, NULL);
    receive_all(ctx, fr, c);
    avcodec_flush_buffers(ctx);
}

int vdec_fill_cache_sw(Mp4File *mf, VideoCache *vc, const uint8_t *need, volatile float *progress, volatile int *cancel, char *err, size_t errlen)
{
    Mp4Track *t = &mf->video;
    int w = t->width, h = t->height;
    if (w <= 0 || h <= 0 || w > 1920 || h > 1088) { snprintf(err, errlen, "bad video size %dx%d", w, h); return -1; }
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
    if (!codec) { snprintf(err, errlen, "MPEG-4 decoder missing"); return -1; }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVFrame *fr = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int n = (int)t->n;
    int *disp_of = malloc(sizeof(int) * (size_t)n), *samp_of = malloc(sizeof(int) * (size_t)n);
    SwCtx c;
    memset(&c, 0, sizeof c);
    c.vc = vc; c.need = need; c.w = w; c.h = h;
    c.rgba = malloc((size_t)w * h * 4);
    c.scratch = malloc(downscale_scratch_size(w, h, vc->cache_w, vc->cache_h));
    int result = -1;
    if (!ctx || !fr || !pkt || !disp_of || !samp_of || !c.rgba || !c.scratch) { snprintf(err, errlen, "out of memory"); goto out; }
    if (t->dsi_len > 0) {
        ctx->extradata = av_mallocz((size_t)t->dsi_len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!ctx->extradata) { snprintf(err, errlen, "out of memory"); goto out; }
        memcpy(ctx->extradata, t->dsi, (size_t)t->dsi_len);
        ctx->extradata_size = t->dsi_len;
    }
    av_log_set_callback(av_log_to_ours);
    ctx->width = w; ctx->height = h;
    ctx->thread_count = 1;
    int r = avcodec_open2(ctx, codec, NULL);
    if (r < 0) { snprintf(err, errlen, "MPEG-4 decoder setup failed (%d)", r); goto out; }

    /* display index = rank of cts */
    for (int i = 0; i < n; i++) samp_of[i] = i;
    for (int i = 1; i < n; i++) {
        int x = samp_of[i], j = i - 1;
        while (j >= 0 && t->s[samp_of[j]].cts > t->s[x].cts) { samp_of[j + 1] = samp_of[j]; j--; }
        samp_of[j + 1] = x;
    }
    for (int k = 0; k < n; k++) disp_of[samp_of[k]] = k;

    int total_need = 0;
    for (int k = 0; k < n; k++) total_need += need[k] ? 1 : 0;
    uint32_t cap = 0;
    uint8_t *buf = NULL;
    int pos = -1, errors = 0;
    result = 0;
    while (!*cancel) {
        int kt = -1;
        for (int k = 0; k < n; k++) if (need[k] && vc->slot[k] < 0 && !(need[k] & 2)) { kt = k; break; }
        if (kt < 0) break;
        int st = samp_of[kt], sync = st;
        while (sync > 0 && !t->s[sync].sync) sync--;
        if (pos < 0 || sync > pos || st < pos - 64) {   /* jump to the keyframe before it */
            if (pos >= 0) sw_drain(ctx, fr, &c);
            pos = sync;
        }
        int fed_target = 0;
        while (pos < n && !*cancel) {
            uint32_t sz = t->s[pos].size;
            if (sz + AV_INPUT_BUFFER_PADDING_SIZE > cap) {
                uint8_t *nb = av_realloc(buf, sz + AV_INPUT_BUFFER_PADDING_SIZE);
                if (!nb) { errors++; pos++; continue; }
                buf = nb; cap = sz + AV_INPUT_BUFFER_PADDING_SIZE;
            }
            if (mp4_read_sample(mf, t, (uint32_t)pos, buf) < 0) { errors++; pos++; continue; }
            memset(buf + sz, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            pkt->data = buf; pkt->size = (int)sz;
            pkt->pts = disp_of[pos]; pkt->dts = AV_NOPTS_VALUE;
            pkt->flags = t->s[pos].sync ? AV_PKT_FLAG_KEY : 0;
            r = avcodec_send_packet(ctx, pkt);
            if (r == AVERROR(EAGAIN)) { receive_all(ctx, fr, &c); r = avcodec_send_packet(ctx, pkt); }
            if (r < 0 && errors++ < 8) LOG("vdec_sw: decode error %d at sample %d", r, pos);
            receive_all(ctx, fr, &c);
            if (pos == st) fed_target = 1;
            pos++;
            if (vc->slot[kt] >= 0) break;
            if (fed_target && pos > st + 4) break;   /* reordering is at most one B-frame run */
            if (pos < n && t->s[pos].sync && pos > st) break;
        }
        if (vc->slot[kt] < 0) {
            sw_drain(ctx, fr, &c);
            pos = -1;
            if (vc->slot[kt] < 0) ((uint8_t *)need)[kt] |= 2;
        }
        if (progress) *progress = total_need ? (float)c.produced / (float)total_need : 1.0f;
        if (errors > 200) { snprintf(err, errlen, "too many decode errors"); result = -1; break; }
    }
    sw_drain(ctx, fr, &c);
    av_free(buf);
    LOG("vdec_sw: MPEG-4 %dx%d: kept %d of %d needed frames (%d errors); %d frames out (not kept: %d out of range, %d not needed, "
        "%d already, %d cache full); cache %d slots", w, h, c.produced, total_need, errors, c.got, c.rej_range, c.rej_need, c.rej_have,
        c.rej_full, vc->nimg);
    if (!c.produced && result == 0) { snprintf(err, errlen, "no frames could be decoded"); result = -1; }
out:
    if (ctx) avcodec_free_context(&ctx);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    free(disp_of); free(samp_of); free(c.rgba); free(c.scratch);
    return result;
}
