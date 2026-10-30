/* vexport.c - see vexport.h.
 *
 * SceMp4Rec is barely documented (vita-headers has the prototypes; no homebrew used it before), so this is written
 * defensively: every call's result goes to the log, memory is tried in several kinds, and when the recorder's hardware
 * colour conversion refuses our frame the conversion is done on the CPU.
 *
 * Timing: the recorder stamps samples itself - each video sample lasts 1001/30000 s (29.97 fps), each audio buffer
 * 1024 frames at 48 kHz (4096 bytes, 16-bit stereo). So the timeline is sampled at k * 1001 / 30000 s whatever the
 * project's frame rate, and the sound is fed in 1024-frame blocks, kept just ahead of the picture. */
#include "vexport.h"

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/gxm.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <vita2d.h>

#include "../third_party/sce/mp4rec.h"
#include "encapi.h"
#include "mp4mux.h"

/* What the recorder needs, found by disassembling vs0:sys/external/libSceMp4Rec.suprx (FW 3.6x):
 *  - it imports memset/memcpy/mspace_create/mspace_malloc/... from SceLibc (vs0:sys/external/libc.suprx). SCE-SDK
 *    games always have that loaded; VitaSDK programs don't, so the imports stay unresolved and the first call into
 *    them (mspace_create of the recorder's heap in sceMp4RecCreateRecorder) jumps to address 0. So libc.suprx is
 *    loaded before the recorder.
 *  - the recorder heap (SceMp4RecRecorder.base) must be at least 4 MB (else 0x80108304, invalid buffer size).
 *  - sceMp4RecAddVideoSample wants exactly width * height * 3 / 2 bytes (a 2-plane YUV 4:2:0 frame, pitch = width).
 *  - recordings must be 5 s .. 30 min long.
 *  - the frame struct's last two words are the picture's plane pointers (Y, then the interleaved chroma). */
#define REC_HEAP (4u << 20)
#define REC_MIN_S 5.0
#define REC_MAX_S (30.0 * 60.0)
static SceUID g_libc = -1;
#include "common.h"
#include "media.h"
#include "mixer.h"
#include "scene.h"

const VExportMode VEXPORT_MODES[VEXPORT_NMODES] = {
    { 640, 368, 2000 }, { 640, 368, 1000 }, { 480, 272, 2000 }, { 480, 272, 1000 }, { 368, 208, 2000 }, { 368, 208, 1000 },
    { 960, 544, 6000 }, { 848, 480, 5000 },   /* our own encoder set-up (VEXPORT_DIRECT) */
};

VExport g_vexp;

#define VIDEO_DIR "ux0:data/vegplayer/vexports"   /* where the finished file is moved */
#define REC_DIR "ux0:video"                       /* where the recorder itself saves */
#define NYUV 3                     /* frames handed to the recorder rotate through this many buffers */
#define NAUD 32                    /* mixed sound blocks ready ahead of the recorder (~0.7 s) */
#define AUD_FRAMES 1024

typedef struct { SceUID uid; void *p; size_t size; } Block;

static struct {
    SceMp4RecRecorder rec;
    int created, inited;
    Block base, enc, av, work;
    vita2d_texture *rt;
    int w, h;
    uint8_t *yuv[NYUV];            /* NV12 frames (in `work`) */
    uint8_t *stage;                /* RGBA copy of the render target, for the hardware conversion (in `work`) */
    int16_t *aud[NAUD];            /* audio buffers (in `work`) */
    float *mix;
    int csc_mode;                  /* 0 = try the render target directly, 1 = via `stage`, 2 = CPU conversion */
    int64_t ablocks, ablocks_total;
    char **before; int nbefore;    /* ux0:video files that existed before */
    int logged_add_err;
    int locked;                    /* we hold the mixer (audio_lock) */
    /* pipeline: the main thread renders frame k+1 while vp_mp4v hands frame k to the recorder and vp_mp4a mixes and
     * hands over the sound (the recorder serialises its own calls; mixing and rendering overlap with them) */
    SceUID sem_free, sem_full;     /* free / filled YUV slots */
    SceUID sem_afree, sem_afull;   /* free / mixed sound blocks */
    int64_t mixed;                 /* sound blocks mixed so far (mix thread) */
    int th_v, th_a;
    volatile int queued;           /* frames rendered and queued */
    volatile int vdone;            /* frames the recorder took */
    volatile int quit;             /* stop the threads */
    volatile int err;              /* first recorder error (< 0) */
    volatile int err_video;
    volatile uint64_t t_vadd, t_audio, t_mix;
    /* direct mode: the hardware encoder and the AAC encoder driven by us, our own MP4 writer */
    int direct;
    Block encmem, out, aacw;
    SceUID ub;                     /* codec engine block over encmem */
    uintptr_t m_lib, m_a, m_b;
    int lib, enc_created;
    AvcEncoder avc;
    void *enc_work;
    uint32_t stream_size;
    uint8_t *au;                   /* one access unit, length-prefixed */
    void *aac;                     /* AAC encoder handle */
    uint8_t aacout[8192];
    int aac_adts_logged;
    Mp4Mux *mux;
    char out_path[600];
    int vout;                      /* access units written */
    int64_t aframes;               /* AAC frames written */
    volatile int ablocks_done;     /* direct: everything encoded and flushed */
} X;

static int mem_alloc(Block *b, const char *name, size_t size, unsigned type, size_t gran)
{
    size = (size + gran - 1) & ~(gran - 1);
    SceUID uid = sceKernelAllocMemBlock(name, type, size, NULL);
    if (uid < 0) { LOG("vexport: %s: %u KB of type 0x%08X failed (0x%08X)", name, (unsigned)(size >> 10), type, (unsigned)uid); return -1; }
    void *p = NULL;
    sceKernelGetMemBlockBase(uid, &p);
    b->uid = uid; b->p = p; b->size = size;
    LOG("vexport: %s: %u KB of type 0x%08X at %p", name, (unsigned)(size >> 10), type, p);
    return 0;
}

/* physically contiguous memory: main RAM, then CDRAM */
static int mem_alloc_phys(Block *b, const char *name, size_t size)
{
    if (mem_alloc(b, name, size, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 1u << 20) == 0) return 0;
    if (mem_alloc(b, name, size, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, 1u << 20) == 0) return 0;
    return mem_alloc(b, name, size, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256u << 10);
}

static void mem_free(Block *b)
{
    if (b->uid > 0) sceKernelFreeMemBlock(b->uid);
    memset(b, 0, sizeof *b);
}

/* ------------------------------------------------------------------ finding the saved file */

static void list_videos(const char *dir, char ***out, int *n, int depth)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0) return;
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) {
        if (e.d_name[0] == '.') continue;
        char p[600];
        snprintf(p, sizeof p, "%s/%s", dir, e.d_name);
        if (SCE_S_ISDIR(e.d_stat.st_mode)) { if (depth < 3) list_videos(p, out, n, depth + 1); continue; }
        if (!str_iends(e.d_name, ".mp4")) continue;
        char **nl = realloc(*out, sizeof(char *) * (size_t)(*n + 1));
        if (!nl) break;
        *out = nl;
        (*out)[(*n)++] = strdup(p);
    }
    sceIoDclose(d);
}

static void free_list(char **l, int n)
{
    for (int i = 0; i < n; i++) free(l[i]);
    free(l);
}

/* ------------------------------------------------------------------ colour conversion */

/* RGBA (bytes R, G, B, A) -> the recorder's 2-plane 4:2:0 (Y plane, then interleaved V/U at half size, "YVU420P2"),
 * BT.601 limited range */
static void rgba_to_yvu420(const uint8_t *src, int stride_px, int w, int h, uint8_t *dst)
{
    uint8_t *Y = dst, *UV = dst + w * h;
    for (int y = 0; y < h; y += 2) {
        const uint8_t *r0 = src + (size_t)y * stride_px * 4, *r1 = r0 + stride_px * 4;
        uint8_t *y0 = Y + y * w, *y1 = y0 + w, *uv = UV + (y / 2) * w;
        for (int x = 0; x < w; x += 2) {
            int R = 0, G = 0, B = 0;
            const uint8_t *p[4] = { r0 + x * 4, r0 + x * 4 + 4, r1 + x * 4, r1 + x * 4 + 4 };
            uint8_t *o[4] = { y0 + x, y0 + x + 1, y1 + x, y1 + x + 1 };
            for (int k = 0; k < 4; k++) {
                int r = p[k][0], g = p[k][1], b = p[k][2];
                *o[k] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                R += r; G += g; B += b;
            }
            R = (R + 2) >> 2; G = (G + 2) >> 2; B = (B + 2) >> 2;
            uv[x] = (uint8_t)(((112 * R - 94 * G - 18 * B + 128) >> 8) + 128);
            uv[x + 1] = (uint8_t)(((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128);
        }
    }
}

/* the hardware conversion through the encoder library (direct mode) */
static int csc_direct(uint8_t *dst, void *src, int stride)
{
    EncPicture d, s;
    memset(&d, 0, sizeof d); memset(&s, 0, sizeof s);
    d.size = s.size = sizeof d;
    d.pixelformat = ENC_PIXFMT_YUV420; d.pitch = (uint32_t)X.w; d.width = (uint32_t)X.w; d.height = (uint32_t)X.h;
    d.buffer = dst; d.buffer2 = dst + (size_t)X.w * X.h;
    s.pixelformat = ENC_PIXFMT_RGBA; s.pitch = (uint32_t)stride; s.width = (uint32_t)X.w; s.height = (uint32_t)X.h; s.buffer = src;
    s.opt[1] = 1;   /* BT.601 */
    return sceAvcencCsc(&d, &s);
}

static int convert(uint8_t *dst)
{
    int w = X.w, h = X.h;
    int stride = (int)(vita2d_texture_get_stride(X.rt) / 4);
    uint8_t *src = vita2d_texture_get_datap(X.rt);
    if (X.direct) {
        if (X.csc_mode == 0) {
            int r = csc_direct(dst, src, stride);
            if (r >= 0) return 0;
            LOG("vexport: sceAvcencCsc from the render target: 0x%08X - trying a contiguous copy", (unsigned)r);
            X.csc_mode = 1;
        }
        if (X.csc_mode == 1) {
            memcpy(X.stage, src, (size_t)stride * 4 * h);
            int r = csc_direct(dst, X.stage, stride);
            if (r >= 0) return 0;
            LOG("vexport: sceAvcencCsc from a contiguous copy: 0x%08X - converting on the CPU", (unsigned)r);
            X.csc_mode = 2;
        }
        rgba_to_yvu420(src, stride, w, h, dst);
        return 0;
    }
    SceMp4RecFrame d, s;
    memset(&d, 0, sizeof d); memset(&s, 0, sizeof s);
    d.size = sizeof d; d.pixelformat = SCE_MP4REC_PIXELFORMAT_YUV420_PACKED; d.stride = w; d.width = w; d.height = h; d.buffer = dst;
    d.reserved = (uint32_t)(uintptr_t)(dst + (size_t)w * h);   /* second plane: the interleaved chroma */
    s.size = sizeof s; s.pixelformat = SCE_MP4REC_PIXELFORMAT_A8B8G8R8; s.stride = stride; s.width = w; s.height = h; s.buffer = src;
    s.unk[1] = 1;   /* RGBA option: conversion coefficients ITU-R BT.601 (byte 0 is a constant alpha, unused) */
    if (X.csc_mode == 0) {
        int r = sceMp4RecCsc(&d, &s);
        if (r >= 0) return 0;
        LOG("vexport: sceMp4RecCsc from the render target: 0x%08X - trying a contiguous copy", (unsigned)r);
        X.csc_mode = 1;
    }
    if (X.csc_mode == 1) {
        memcpy(X.stage, src, (size_t)stride * 4 * h);
        s.buffer = X.stage;
        int r = sceMp4RecCsc(&d, &s);
        if (r >= 0) return 0;
        LOG("vexport: sceMp4RecCsc from a contiguous copy: 0x%08X - converting on the CPU", (unsigned)r);
        X.csc_mode = 2;
    }
    rgba_to_yvu420(src, stride, w, h, dst);
    return 0;
}

/* ------------------------------------------------------------------ recorder calls */

/* hand a sample over; if the recorder is still busy with earlier ones, wait (up to ~3 s) */
static int add_sample(int video, void *buf, int size)
{
    int r = 0;
    for (int tries = 0; tries < 1500; tries++) {
        r = video ? sceMp4RecAddVideoSample(&X.rec, buf, size) : sceMp4RecAddAudioSample(&X.rec, buf, size);
        if (r >= 0) return 0;
        if (X.logged_add_err < 6) { X.logged_add_err++; LOG("vexport: sceMp4RecAdd%sSample: 0x%08X (retrying)", video ? "Video" : "Audio", (unsigned)r); }
        sleep_us(2000);
    }
    return r;
}

/* The recorder can't take video and sound from two threads at once in practice: it locks itself for each call and
 * its muxer interleaves by time, so a video-only burst fills its video buffer (0x801083AA). So one feeder thread
 * hands over everything in time order - a frame, then the sound up to that frame's end - while the main thread
 * renders the next frames and a mix thread mixes the sound ahead. */
static int mix_thread(unsigned argc, void *argp)
{
    (void)argc; (void)argp;
    while (!X.quit && X.mixed < X.ablocks_total) {
        SceUInt to = 100000;
        if (sceKernelWaitSema(X.sem_afree, 1, &to) < 0) continue;
        if (X.quit) break;
        uint64_t t = time_us();
        int16_t *pcm = X.aud[X.mixed % NAUD];
        mixer_mix(X.mix, AUD_FRAMES, X.mixed * AUD_FRAMES);
        mixer_to_pcm(X.mix, pcm, AUD_FRAMES * 2);
        X.mixed++;
        X.t_mix += time_us() - t;
        sceKernelSignalSema(X.sem_afull, 1);
    }
    return 0;
}

/* direct mode: one AAC frame (or none, while the encoder fills up) to the file */
static int aac_put(uint32_t n)
{
    if (!n) return 0;
    const uint8_t *p = X.aacout;
    if (n >= 7 && p[0] == 0xFF && (p[1] & 0xF0) == 0xF0) {   /* ADTS: drop the header */
        uint32_t hl = (p[1] & 1) ? 7 : 9;
        if (!X.aac_adts_logged) { X.aac_adts_logged = 1; LOG("vexport: the AAC encoder writes ADTS frames (header %u bytes)", hl); }
        if (n <= hl) return 0;
        p += hl; n -= hl;
    }
    X.aframes++;
    return mux_audio(X.mux, p, n) < 0 ? -1 : 0;
}

static int aac_encode(const int16_t *pcm)
{
    uint32_t n = sizeof X.aacout;
    int r = sceAacencInternalEncode(X.aac, pcm, AUD_FRAMES * 4, X.aacout, &n, 0);
    if (r < 0) return r;
    if (X.ablocks < 3) LOG("vexport: AAC block %lld: %u bytes out (%02X %02X)", (long long)X.ablocks, n, X.aacout[0], X.aacout[1]);
    return aac_put(n);
}

/* hand over the next mixed sound block (waits for the mixer) */
static int feed_audio_block(void)
{
    for (;;) {
        if (X.quit) return 1;
        SceUInt to = 100000;
        if (sceKernelWaitSema(X.sem_afull, 1, &to) >= 0) break;
    }
    uint64_t t = time_us();
    int r = X.direct ? aac_encode(X.aud[X.ablocks % NAUD]) : add_sample(0, X.aud[X.ablocks % NAUD], SCE_MP4REC_AUDIO_BUFFER_SIZE);
    X.t_audio += time_us() - t;
    if (r < 0) return r;
    X.ablocks++;
    sceKernelSignalSema(X.sem_afree, 1);
    return 0;
}

static int feeder_thread(unsigned argc, void *argp)
{
    (void)argc; (void)argp;
    while (!X.quit) {
        int k = X.vdone;
        if (k >= g_vexp.nframes) {   /* the rest of the sound */
            while (!X.quit && X.ablocks < X.ablocks_total) {
                int r = feed_audio_block();
                if (r < 0) { if (!X.err) X.err = r; return 0; }
            }
            break;
        }
        SceUInt to = 100000;
        if (sceKernelWaitSema(X.sem_full, 1, &to) < 0) continue;
        if (X.quit) break;
        uint64_t t = time_us();
        int r = add_sample(1, X.yuv[k % NYUV], X.w * X.h * 3 / 2);
        X.t_vadd += time_us() - t;
        if (r < 0) { if (!X.err) { X.err = r; X.err_video = 1; } break; }
        X.vdone = k + 1;
        sceKernelSignalSema(X.sem_free, 1);
        int64_t until = (int64_t)(k + 1) * 1001 * OUT_RATE;   /* x 30000 */
        while (!X.quit && X.ablocks < X.ablocks_total && X.ablocks * AUD_FRAMES * 30000 < until) {
            r = feed_audio_block();
            if (r < 0) { if (!X.err) X.err = r; return 0; }
        }
    }
    return 0;
}

/* direct mode: the access units that came out of the encoder, to the file */
static int enc_collect(EncStream *st, EncStreamArray *arr)
{
    if (!arr->count) return 0;
    uint32_t len = 0;
    int sync = st->pic_type == 4;
    const uint8_t *sps = NULL, *pps = NULL; uint32_t sl = 0, pl = 0;
    for (int k = 0; k < st->n_nal; k++) {
        EncNal nal; memset(&nal, 0, sizeof nal);
        int r = sceAvcencGetNalUnit(&X.avc, k, &nal);
        if (r < 0) { LOG("vexport: sceAvcencGetNalUnit(%d): 0x%08X", k, (unsigned)r); return r; }
        int type = nal.type & 31;
        if (type == 7) { sps = nal.addr; sl = nal.size; continue; }
        if (type == 8) { pps = nal.addr; pl = nal.size; continue; }
        if (type == 9) continue;   /* access unit delimiter */
        if (type == 5) sync = 1;
        if (len + 4 + nal.size > X.stream_size + 4096) { LOG("vexport: access unit too big"); return -1; }
        X.au[len] = (uint8_t)(nal.size >> 24); X.au[len + 1] = (uint8_t)(nal.size >> 16);
        X.au[len + 2] = (uint8_t)(nal.size >> 8); X.au[len + 3] = (uint8_t)nal.size;
        memcpy(X.au + len + 4, nal.addr, nal.size);
        len += 4 + nal.size;
    }
    if (sps && pps) mux_set_avc_config(X.mux, sps, sl, pps, pl);
    if (!len) return 0;
    X.vout++;
    return mux_video(X.mux, X.au, len, sync) < 0 ? -1 : 0;
}

/* direct mode: encode one frame (yuv) or, with yuv NULL, drain the encoder */
static int enc_frame(uint8_t *yuv)
{
    EncStream st; EncStream *sp = &st;
    EncStreamArray arr;
    for (int rounds = 0; rounds < (yuv ? 1 : 8); rounds++) {
        memset(&st, 0, sizeof st);
        st.size = sizeof st; st.buf = X.out.p; st.buf_size = X.stream_size;
        arr.count = 0; arr.max = 1; arr.streams = &sp;
        int r;
        if (yuv) {
            EncPicture p; memset(&p, 0, sizeof p);
            p.size = sizeof p; p.pixelformat = ENC_PIXFMT_YUV420; p.pitch = (uint32_t)X.w; p.width = (uint32_t)X.w; p.height = (uint32_t)X.h;
            p.buffer = yuv; p.flags = 1;
            r = sceAvcencEncode(&X.avc, &p, &arr);
        } else r = sceAvcencEncodeStop(&X.avc, &arr);
        if (r < 0) { LOG("vexport: sceAvcenc%s: 0x%08X", yuv ? "Encode" : "EncodeStop", (unsigned)r); return r; }
        r = enc_collect(&st, &arr);
        if (r < 0) return r;
        if (!yuv && !arr.count) break;
    }
    return 0;
}

/* direct mode: like feeder_thread, but encoding ourselves. The encoder hands a frame's stream out one call later and
 * may still read the previous picture, so a slot is released one frame late. */
static int direct_thread(unsigned argc, void *argp)
{
    (void)argc; (void)argp;
    int held = 0;
    while (!X.quit) {
        int k = X.vdone;
        if (k >= g_vexp.nframes) {
            int r = enc_frame(NULL);
            if (held) { sceKernelSignalSema(X.sem_free, 1); held = 0; }
            if (r < 0) { if (!X.err) { X.err = r; X.err_video = 1; } return 0; }
            while (!X.quit && X.ablocks < X.ablocks_total) {
                r = feed_audio_block();
                if (r < 0) { if (!X.err) X.err = r; return 0; }
            }
            for (int i = 0; i < 8 && !X.quit; i++) {   /* the AAC encoder's last frames */
                uint32_t n = sizeof X.aacout;
                r = sceAacencInternalFlush(X.aac, X.aacout, &n, 0);
                if (r < 0 || !n) break;
                if (aac_put(n) < 0) { if (!X.err) X.err = -1; return 0; }
            }
            X.ablocks_done = 1;
            break;
        }
        SceUInt to = 100000;
        if (sceKernelWaitSema(X.sem_full, 1, &to) < 0) continue;
        if (X.quit) break;
        uint64_t t = time_us();
        int r = enc_frame(X.yuv[k % NYUV]);
        X.t_vadd += time_us() - t;
        if (r < 0) { if (!X.err) { X.err = r; X.err_video = 1; } break; }
        if (held) sceKernelSignalSema(X.sem_free, 1);
        held = 1;
        X.vdone = k + 1;
        int64_t until = (int64_t)(k + 1) * 1001 * OUT_RATE;   /* x 30000 */
        while (!X.quit && X.ablocks < X.ablocks_total && X.ablocks * AUD_FRAMES * 30000 < until) {
            r = feed_audio_block();
            if (r < 0) { if (!X.err) X.err = r; return 0; }
        }
    }
    return 0;
}

static void threads_stop(void)
{
    X.quit = 1;
    if (X.th_v > 0) { thread_join(X.th_v); X.th_v = 0; }
    if (X.th_a > 0) { thread_join(X.th_a); X.th_a = 0; }
    if (X.sem_afree > 0) { sceKernelDeleteSema(X.sem_afree); X.sem_afree = 0; }
    if (X.sem_afull > 0) { sceKernelDeleteSema(X.sem_afull); X.sem_afull = 0; }
    if (X.sem_full > 0) { sceKernelDeleteSema(X.sem_full); X.sem_full = 0; }
    if (X.sem_free > 0) { sceKernelDeleteSema(X.sem_free); X.sem_free = 0; }
}

static void stats_reset(void);

static void cleanup(void)
{
    threads_stop();
    if (X.inited || X.created) {
        if (X.created) {
            int r = sceMp4RecDeleteRecorder(&X.rec);
            LOG("vexport: sceMp4RecDeleteRecorder: 0x%08X", (unsigned)r);
        }
        X.inited = X.created = 0;
    }
    if (X.mux) { mux_close(X.mux, 1); X.mux = NULL; }
    if (X.enc_created) { LOG("vexport: sceAvcencDeleteEncoder: 0x%08X", (unsigned)sceAvcencDeleteEncoder(&X.avc)); X.enc_created = 0; }
    if (X.lib) { LOG("vexport: sceVideoencTermLibrary: 0x%08X", (unsigned)sceVideoencTermLibrary(ENC_TYPE_AVC)); X.lib = 0; }
    if (X.ub > 0) {
        if (X.m_b) sceCodecEngineFreeMemoryFromUnmapMemBlock(X.ub, X.m_b);
        if (X.m_a) sceCodecEngineFreeMemoryFromUnmapMemBlock(X.ub, X.m_a);
        if (X.m_lib) sceCodecEngineFreeMemoryFromUnmapMemBlock(X.ub, X.m_lib);
        sceCodecEngineCloseUnmapMemBlock(X.ub);
        X.ub = 0; X.m_a = X.m_b = X.m_lib = 0;
    }
    if (X.aac) { sceAacencInternalDelete(X.aac); X.aac = NULL; }
    free(X.enc_work); X.enc_work = NULL;
    free(X.au); X.au = NULL;
    if (X.rt) { vita2d_wait_rendering_done(); vita2d_free_texture(X.rt); X.rt = NULL; }
    g_vexp.preview = NULL;
    mem_free(&X.work); mem_free(&X.enc); mem_free(&X.av); mem_free(&X.base);
    mem_free(&X.encmem); mem_free(&X.out); mem_free(&X.aacw);
    free(X.mix); X.mix = NULL;
    vita2d_set_vblank_wait(1);
}

static void finish(int discard)
{
    threads_stop();
    if (X.direct) {
        if (X.locked) { mixer_reset(); audio_unlock(); X.locked = 0; }
        int r = -1;
        if (X.mux) {
            uint64_t t = time_us();
            /* the AAC encoder starts with 2112 samples of its own (measured: a sound at the project's start comes out
             * 44 ms late, and 6108 blocks in give 6111 frames out) */
            mux_set_audio_edit(X.mux, 2112, (uint64_t)X.ablocks_total * AUD_FRAMES);
            r = mux_close(X.mux, discard || g_vexp.failed);
            X.mux = NULL;
            LOG("vexport: %d video frames, %lld AAC frames written; file %s (%.1f s)", X.vout, (long long)X.aframes,
                discard || g_vexp.failed ? "discarded" : r >= 0 ? "completed" : "FAILED", (time_us() - t) / 1e6);
        }
        cleanup();
        if (!discard && !g_vexp.failed) {
            if (r >= 0) snprintf(g_vexp.msg, sizeof g_vexp.msg, "Saved %s", X.out_path);
            else { g_vexp.failed = 1; snprintf(g_vexp.msg, sizeof g_vexp.msg, "The video file couldn't be completed"); }
        } else if (discard && !g_vexp.failed) snprintf(g_vexp.msg, sizeof g_vexp.msg, "Video export cancelled");
        LOG("vexport: %s (%.1f s, %d frames)", g_vexp.msg, (time_us() - g_vexp.t0) / 1e6, g_vexp.frame);
        g_vexp.running = 0;
        return;
    }
    if (X.inited) {
        SceMp4RecTermParam *tp = calloc(1, sizeof *tp);
        tp->size = sizeof *tp;
        tp->discard = discard;
        uint64_t t = time_us();
        int r = sceMp4RecTerm(&X.rec, tp);
        LOG("vexport: sceMp4RecTerm(discard %d): 0x%08X (%.1f s)", discard, (unsigned)r, (time_us() - t) / 1e6);
        free(tp);
        X.inited = 0;
        if (r < 0 && !discard && !g_vexp.failed) { g_vexp.failed = 1; snprintf(g_vexp.msg, sizeof g_vexp.msg, "The recorder couldn't save the video (0x%08X)", (unsigned)r); }
    }
    if (X.locked) { mixer_reset(); audio_unlock(); X.locked = 0; }
    cleanup();
    if (!discard && !g_vexp.failed) {
        char **after = NULL; int na = 0;
        list_videos(REC_DIR, &after, &na, 0);
        const char *found = NULL;
        for (int i = 0; i < na && !found; i++) {
            int old = 0;
            for (int k = 0; k < X.nbefore && !old; k++) old = !strcmp(after[i], X.before[k]);
            if (!old) found = after[i];
        }
        if (found) {
            /* move it next to the other exports, named after the project */
            char name[256], dst[600];
            const char *b = strrchr(g_s.veg_path, '/');
            snprintf(name, sizeof name, "%s", b ? b + 1 : g_s.veg_path);
            char *dot = strrchr(name, '.');
            if (dot && dot != name) *dot = 0;
            mkdir_p(VIDEO_DIR);
            snprintf(dst, sizeof dst, VIDEO_DIR "/%s.mp4", name);
            for (int k = 2; k < 100 && file_exists(dst); k++) snprintf(dst, sizeof dst, VIDEO_DIR "/%s (%d).mp4", name, k);
            int mr = sceIoRename(found, dst);
            LOG("vexport: move %s -> %s: 0x%08X", found, dst, (unsigned)mr);
            snprintf(g_vexp.msg, sizeof g_vexp.msg, "Saved %s", mr >= 0 ? dst : found);
        } else snprintf(g_vexp.msg, sizeof g_vexp.msg, "Done - the video should be in " REC_DIR);
        free_list(after, na);
    } else if (discard && !g_vexp.failed) snprintf(g_vexp.msg, sizeof g_vexp.msg, "Video export cancelled");
    free_list(X.before, X.nbefore); X.before = NULL; X.nbefore = 0;
    LOG("vexport: %s (%.1f s, %d frames)", g_vexp.msg, (time_us() - g_vexp.t0) / 1e6, g_vexp.frame);
    g_vexp.running = 0;
}

static int fail(const char *what, int code)
{
    g_vexp.failed = 1;
    if (code) snprintf(g_vexp.msg, sizeof g_vexp.msg, "%s (0x%08X)", what, (unsigned)code);
    else snprintf(g_vexp.msg, sizeof g_vexp.msg, "%s", what);
    LOG("vexport: %s", g_vexp.msg);
    return -1;
}

static int recorder_setup(int mode)
{
    /* the recorder object's own memory: its size isn't documented, so grow it until the recorder accepts it */
    static const unsigned BASE_KB[] = { REC_HEAP >> 10, 8192 };
    int r = -1;
    for (int i = 0; i < 2; i++) {
        if (mem_alloc(&X.base, "vp_mp4rec_base", BASE_KB[i] << 10, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4096) < 0) break;
        memset(X.base.p, 0, X.base.size);
        X.rec.size = sizeof X.rec;
        X.rec.base = X.base.p;
        X.rec.base_size = (uint32_t)X.base.size;
        X.rec.unk = 0;
        LOG("vexport: calling sceMp4RecCreateRecorder (%u KB at %p)", BASE_KB[i], X.base.p);
        r = sceMp4RecCreateRecorder(&X.rec);
        LOG("vexport: sceMp4RecCreateRecorder (%u KB): 0x%08X", BASE_KB[i], (unsigned)r);
        if (r >= 0) break;
        mem_free(&X.base);
    }
    if (r < 0) { cleanup(); return fail("The MP4 recorder couldn't be created", r); }
    X.created = 1;

    uint32_t enc_size = 0, av_size = 0;
    r = sceMp4RecQueryPhysicalMemSize(&X.rec, mode, &enc_size, &av_size);
    LOG("vexport: mode %d (%dx%d): encoder %u KB, audio/mux %u KB (0x%08X)", mode, X.w, X.h, enc_size >> 10, av_size >> 10, (unsigned)r);
    if (r < 0) { cleanup(); return fail("The MP4 recorder rejected this mode", r); }
    if ((enc_size && mem_alloc_phys(&X.enc, "vp_mp4rec_enc", enc_size) < 0) || (av_size && mem_alloc_phys(&X.av, "vp_mp4rec_av", av_size) < 0)) {
        cleanup();
        return fail("Not enough memory for the encoder - lower the frame cache budget in Options and prepare again", 0);
    }
    SceMp4RecInitParam ip;
    memset(&ip, 0, sizeof ip);
    ip.size = sizeof ip;
    ip.mode = (uint32_t)mode | 0x10000;   /* option: ITU-R BT.601 colours (what the conversion below produces) */
    ip.encoder_mem = X.enc.p; ip.encoder_size = enc_size;
    ip.av_mem = X.av.p; ip.av_size = av_size;
    ip.affinity = 0x40000;            /* core 2: the scene builder's core, idle while exporting */
    ip.priority = 0x10000100;
    r = sceMp4RecInit(&X.rec, &ip);
    LOG("vexport: sceMp4RecInit: 0x%08X", (unsigned)r);
    if (r < 0) {
        ip.affinity = 0; ip.priority = 160;   /* plain values, in case the recorder wants them so */
        r = sceMp4RecInit(&X.rec, &ip);
        LOG("vexport: sceMp4RecInit (default affinity, priority 160): 0x%08X", (unsigned)r);
    }
    if (r < 0) { cleanup(); return fail("The MP4 recorder couldn't start", r); }
    X.inited = 1;
    return 0;
}

/* our own set-up of the hardware H.264 encoder (sizes the recorder doesn't offer) and of the AAC encoder */
static const uint8_t PRESET4[0x50] = {   /* the kernel's encoder preset 4 (see direct_setup) */
    0x42, 0x1e, 0x01, 0x02, 0x80, 0x02, 0x00, 0x00, 0x80, 0x02, 0x00, 0x00, 0xe0, 0x01, 0x00, 0x00,
    0x80, 0x02, 0x00, 0x00, 0xe0, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x96, 0x98, 0x00,
    0x00, 0x1b, 0xb7, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x84, 0x1e, 0x00, 0x00, 0x9f, 0x24, 0x00, 0x00, 0xc2, 0x01, 0x00, 0x3a, 0x00, 0x00, 0x00,
    0xa4, 0x0f, 0x00, 0x00, 0x80, 0xa9, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static void put32(uint8_t *b, int o, uint32_t v) { memcpy(b + o, &v, 4); }

static int direct_setup(int mode)
{
    int w = X.w, h = X.h, r;
    uint32_t bitrate = (uint32_t)VEXPORT_MODES[mode].kbps * 1000;
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);   /* the encoder's user library lives in the same module */

    /* The encoder's full settings (0x50 bytes), for the "Internal" calls: the plain calls take 0x34 bytes, check them
     * against 720x480 and expand them into this form from one of four built-in presets; the Internal calls only hold
     * the size to the library's (960x576). Bytes: 0 profile (Baseline), 1 level x 10, 3 = 2 (the stream carries
     * SPS/PPS before each keyframe; the recorder's 0 leaves them out), 4 pitch, 8/12 size, 16/20 size checked against
     * the library, 48 bitrate, 52 max bitrate, 56 w*h*3/8, 60 keyframe interval (58), 64/68 frame rate as
     * time_scale / (2 * tick), 72..79 VUI colour description (BT.601). */
    static const uint8_t VUI601[8] = { 0x01, 0x00, 0x02, 0x00, 0x01, 0x06, 0x06, 0x06 };
    uint8_t full[0x50];
    memcpy(full, PRESET4, sizeof full);
    full[1] = 31;   /* level 3.1: 960x544 at 29.97 fps is past level 3.0 */
    full[3] = 2;
    put32(full, 4, (uint32_t)w); put32(full, 8, (uint32_t)w); put32(full, 12, (uint32_t)h);
    put32(full, 16, (uint32_t)w); put32(full, 20, (uint32_t)h); put32(full, 24, 1);
    put32(full, 48, bitrate); put32(full, 52, bitrate / 5 * 6);
    put32(full, 56, (uint32_t)(w * h * 3 / 8));
    put32(full, 64, 1001); put32(full, 68, 60000);   /* tick 1001, time scale 60000: 29.97 frames a second */
    memcpy(full + 72, VUI601, 8);

    EncLibInit li; memset(&li, 0, sizeof li);
    li.size = 0x14; li.width = (uint32_t)w; li.height = (uint32_t)h; li.n_encoders = 1; li.n_unk = 1;
    uint32_t lib_size = 0;
    r = sceVideoencQueryMemSize(ENC_TYPE_AVC, NULL, &li, &lib_size);
    EncMemOut mo; memset(&mo, 0, sizeof mo);
    int r2 = r < 0 ? r : sceAvcencQueryEncoderMemSizeInternal(ENC_TYPE_AVC, full, &mo);
    LOG("vexport: encoder %dx%d %u kbps: library %u KB (0x%08X), encoder %u + %u KB, stream %u KB (0x%08X)", w, h, bitrate / 1000,
        lib_size >> 10, (unsigned)r, mo.mem_a >> 10, mo.mem_b >> 10, mo.stream_size >> 10, (unsigned)r2);
    if (r < 0 || r2 < 0) return fail("The video encoder doesn't take this size", r < 0 ? r : r2);
    const uint32_t A = 0x40000;
    uint32_t total = (((lib_size + A - 1) & ~(A - 1)) + ((mo.mem_a + A - 1) & ~(A - 1)) + ((mo.mem_b + A - 1) & ~(A - 1)) + (1u << 20) + 0xFFFFF) & ~0xFFFFFu;
    if (mem_alloc_phys(&X.encmem, "vp_enc", total) < 0)
        return fail("Not enough memory for the video encoder - lower the frame cache budget in Options and prepare again", 0);
    X.ub = sceCodecEngineOpenUnmapMemBlock(X.encmem.p, (SceSize)X.encmem.size);
    if (X.ub < 0) { r = X.ub; X.ub = 0; cleanup(); return fail("The codec engine refused the encoder memory", r); }
    X.m_lib = sceCodecEngineAllocMemoryFromUnmapMemBlock(X.ub, lib_size, A);
    X.m_a = sceCodecEngineAllocMemoryFromUnmapMemBlock(X.ub, mo.mem_a, A);
    X.m_b = sceCodecEngineAllocMemoryFromUnmapMemBlock(X.ub, mo.mem_b, A);
    if (!X.m_lib || !X.m_a || !X.m_b) { cleanup(); return fail("Couldn't divide the encoder memory", 0); }
    EncLibMem lm; memset(&lm, 0, sizeof lm);
    lm.addr = (void *)X.m_lib; lm.size = lib_size;
    r = sceVideoencInitLibraryWithUnmapMem(ENC_TYPE_AVC, &lm, NULL, &li);
    LOG("vexport: sceVideoencInitLibraryWithUnmapMem: 0x%08X", (unsigned)r);
    if (r < 0) { cleanup(); return fail("The video encoder library couldn't start", r); }
    X.lib = 1;
    X.enc_work = memalign(64, 0x400);
    X.avc.mem_a = (void *)X.m_a; X.avc.mem_a_size = mo.mem_a;
    X.avc.mem_b = (void *)X.m_b; X.avc.mem_b_size = mo.mem_b;
    X.avc.work = X.enc_work; X.avc.work_size = 0x400;
    r = X.enc_work ? sceAvcencCreateEncoderInternal(ENC_TYPE_AVC, &X.avc, full) : -1;
    LOG("vexport: sceAvcencCreateEncoderInternal: 0x%08X", (unsigned)r);
    if (r < 0) { cleanup(); return fail("The video encoder couldn't be created", r); }
    X.enc_created = 1;
    EncParam ep; memset(&ep, 0, sizeof ep); ep.r[3] = 0x1C;
    r = sceAvcencSetEncoderParameter(&X.avc, &ep);
    if (r < 0) LOG("vexport: sceAvcencSetEncoderParameter: 0x%08X", (unsigned)r);
    X.stream_size = mo.stream_size ? mo.stream_size : 1u << 20;
    X.au = malloc(X.stream_size + 4096);
    if (!X.au || mem_alloc(&X.out, "vp_enc_out", X.stream_size, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4096) < 0) {
        cleanup(); return fail("Not enough memory for the encoder output", 0);
    }

    /* AAC, 48 kHz stereo 128 kbps, with the recorder's settings */
    uint32_t aws = sceAacencInternalGetWorkSize(1);
    if (mem_alloc(&X.aacw, "vp_aacenc", aws, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4096) < 0) { cleanup(); return fail("Not enough memory for the sound encoder", 0); }
    memset(X.aacw.p, 0, X.aacw.size);
    r = sceAacencInternalCreate(&X.aac, X.aacw.p);
    if (r < 0) { X.aac = NULL; cleanup(); return fail("The sound encoder couldn't be created", r); }
    static const uint32_t P20[4] = { 0, 1, 1, 0 }, P24[2] = { 2, 2 };
    AacencParam ap; memset(&ap, 0, sizeof ap);
    ap.channels = 2; ap.sample_rate = OUT_RATE; ap.bitrate = 128000;
    ap.r14 = 1; ap.r1c = 1; ap.p20 = P20; ap.p24 = P24; ap.r48 = 1; ap.r4c = 2; ap.r50 = 1;
    r = sceAacencInternalSetParam(X.aac, &ap);
    LOG("vexport: AAC encoder (work %u KB): 0x%08X", aws >> 10, (unsigned)r);
    if (r < 0) { cleanup(); return fail("The sound encoder refused its settings", r); }

    /* the file, named after the project */
    char name[256];
    const char *b = strrchr(g_s.veg_path, '/');
    snprintf(name, sizeof name, "%s", b ? b + 1 : g_s.veg_path);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
    mkdir_p(VIDEO_DIR);
    snprintf(X.out_path, sizeof X.out_path, VIDEO_DIR "/%s.mp4", name);
    for (int k = 2; k < 100 && file_exists(X.out_path); k++) snprintf(X.out_path, sizeof X.out_path, VIDEO_DIR "/%s (%d).mp4", name, k);
    X.mux = mux_open(X.out_path, w, h, 30000, 1001, OUT_RATE, 2);
    if (!X.mux) { cleanup(); return fail("Couldn't create the video file", 0); }
    LOG("vexport: writing %s", X.out_path);
    return 0;
}

int vexport_start(int mode)
{
    if (g_vexp.running || mode < 0 || mode >= VEXPORT_NMODES) return -1;
    memset(&g_vexp, 0, sizeof g_vexp);
    memset(&X, 0, sizeof X);
    g_vexp.mode = mode;
    g_vexp.t0 = time_us();
    X.w = VEXPORT_MODES[mode].w; X.h = VEXPORT_MODES[mode].h;

    if (g_libc < 0) {
        /* the recorder links against SceLibc: it must be there before the recorder is loaded (imports resolve at
         * load time). If the recorder got loaded without it (an earlier version), reload it. */
        /* libc.suprx imports from libfios2.suprx (its file I/O): that goes first, else 0x8002D003 (no such library) */
        static SceUID fios = -1;
        if (fios < 0) {
            fios = sceKernelLoadStartModule("vs0:sys/external/libfios2.suprx", 0, NULL, 0, NULL, NULL);
            LOG("vexport: load SceFios2: 0x%08X", (unsigned)fios);
        }
        g_libc = sceKernelLoadStartModule("vs0:sys/external/libc.suprx", 0, NULL, 0, NULL, NULL);
        LOG("vexport: load SceLibc: 0x%08X", (unsigned)g_libc);
        if (g_libc < 0) return fail("Couldn't load the system's C library (needed by the recorder)", g_libc);
        if (sceSysmoduleIsLoaded(SCE_SYSMODULE_MP4_RECORDER) == 0) {
            int u = sceSysmoduleUnloadModule(SCE_SYSMODULE_MP4_RECORDER);
            LOG("vexport: unload SceMp4Rec (loaded without SceLibc): 0x%08X", (unsigned)u);
        }
    }
    if (sceSysmoduleIsLoaded(SCE_SYSMODULE_MP4_RECORDER) != 0) {
        int r = sceSysmoduleLoadModule(SCE_SYSMODULE_MP4_RECORDER);
        LOG("vexport: load SceMp4Rec module: 0x%08X", (unsigned)r);
        if (r < 0) return fail("Couldn't load the system's MP4 recorder", r);
    }
    SceKernelFreeMemorySizeInfo fm;
    memset(&fm, 0, sizeof fm);
    fm.size = sizeof fm;
    if (sceKernelGetFreeMemorySize(&fm) >= 0)
        LOG("vexport: free memory: main %d KB, CDRAM %d KB, contiguous %d KB", fm.size_user >> 10, fm.size_cdram >> 10, fm.size_phycont >> 10);

    X.direct = mode >= VEXPORT_FIRST_DIRECT;
    if ((X.direct ? direct_setup(mode) : recorder_setup(mode)) < 0) return -1;

    /* frame buffers: NV12 frames, an RGBA staging copy and the audio buffers, in one contiguous block */
    size_t yuv = ((size_t)X.w * X.h * 3 / 2 + 4095) & ~(size_t)4095;
    size_t stage = ((size_t)((X.w + 7) & ~7) * X.h * 4 + 4095) & ~(size_t)4095;
    size_t aud = SCE_MP4REC_AUDIO_BUFFER_SIZE;
    if (mem_alloc_phys(&X.work, "vp_mp4rec_frames", yuv * NYUV + stage + aud * NAUD) < 0) {
        finish(1);
        return fail("Not enough memory for the frame buffers - lower the frame cache budget in Options", 0);
    }
    uint8_t *p = X.work.p;
    for (int i = 0; i < NYUV; i++, p += yuv) X.yuv[i] = p;
    X.stage = p; p += stage;
    for (int i = 0; i < NAUD; i++, p += aud) X.aud[i] = (int16_t *)p;
    X.mix = malloc(sizeof(float) * AUD_FRAMES * 2);
    X.rt = vita2d_create_empty_texture_rendertarget((unsigned)X.w, (unsigned)X.h, SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
    if (!X.rt || !X.mix) { finish(1); return fail("Not enough graphics memory for the video frame", 0); }
    g_vexp.preview = X.rt;

    double dur = g_s.proj.duration;
    if (!X.direct) {
        list_videos(REC_DIR, &X.before, &X.nbefore, 0);
        if (dur > REC_MAX_S) { LOG("vexport: the recorder stops at 30 minutes: exporting the first 30"); dur = REC_MAX_S; }
        if (dur < REC_MIN_S) dur = REC_MIN_S;   /* the recorder's minimum: the rest is silent black */
    }
    g_vexp.nframes = (int)ceil(dur * 30000.0 / 1001.0 - 1e-6);
    if (g_vexp.nframes < 1) g_vexp.nframes = 1;
    X.ablocks_total = (int64_t)ceil(dur * OUT_RATE / AUD_FRAMES - 1e-6);

    /* the picture: the project letterboxed into the recorder's frame */
    VegProject *pr = &g_s.proj;
    float s = fminf((float)X.w / pr->width, (float)X.h / pr->height);
    float vw = pr->width * s, vh = pr->height * s;
    scene_offline_begin();
    scene_set_viewport((X.w - vw) / 2, (X.h - vh) / 2, vw, vh);

    mixer_project_changed();
    audio_lock();
    X.locked = 1;
    mixer_reset();
    vita2d_set_vblank_wait(0);         /* the progress screen needn't wait for the display */
    stats_reset();
    X.sem_free = sceKernelCreateSema("vp_mp4_free", 0, NYUV, NYUV, NULL);
    X.sem_full = sceKernelCreateSema("vp_mp4_full", 0, 0, NYUV, NULL);
    X.sem_afree = sceKernelCreateSema("vp_mp4_afree", 0, NAUD, NAUD, NULL);
    X.sem_afull = sceKernelCreateSema("vp_mp4_afull", 0, 0, NAUD, NULL);
    /* big stacks: the recorder's encoders run on the calling thread (the AAC encoder overflowed 64 KB) */
    X.th_v = thread_start("vp_mp4feed", X.direct ? direct_thread : feeder_thread, NULL, 0x10000100, 1024 * 1024, 0x40000);
    X.th_a = thread_start("vp_mp4mix", mix_thread, NULL, 0x10000100 + 2, 256 * 1024, 0x20000);
    if (X.sem_free < 0 || X.sem_full < 0 || X.sem_afree < 0 || X.sem_afull < 0 || X.th_v < 0 || X.th_a < 0) {
        finish(1);
        return fail("Couldn't start the export threads", 0);
    }
    g_vexp.running = 1;
    LOG("vexport: %d frames, %lld audio blocks, project %dx%d -> %dx%d", g_vexp.nframes, (long long)X.ablocks_total, pr->width, pr->height, X.w, X.h);
    return 0;
}

/* gpu_draw_layer works in screen coordinates (960x544); the render target's viewport maps them onto its pixels */
static void to_screen_space(GpuLayer *L)
{
    float sx = (float)SCREEN_W / X.w, sy = (float)SCREEN_H / X.h;
    for (int i = 0; i < 4; i++) { L->x[i] *= sx; L->y[i] *= sy; }
}

static void render(double T)
{
    vita2d_pool_reset();   /* the GPU is idle here (the last frame waited for it) */
    const Scene *sc = scene_build_now(T);
    scene_prepass(sc);     /* layers' earlier effect stages, into their own render targets */
    vita2d_start_drawing_advanced(X.rt, 0);
    SceGxmContext *ctx = vita2d_get_context();
    sceGxmSetViewport(ctx, X.w * 0.5f, X.w * 0.5f, X.h * 0.5f, -X.h * 0.5f, 0.5f, 0.5f);
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    vita2d_clear_screen();
    static GpuLayer L;
    for (int i = 0; i < sc->n; i++) {
        L = sc->layer[i];
        to_screen_space(&L);
        gpu_draw_layer(&L);
    }
    /* black over the letterbox bars (layers may reach past the picture) */
    float sx = (float)SCREEN_W / X.w, sy = (float)SCREEN_H / X.h;
    float x0 = sc->vx * sx, y0 = sc->vy * sy, x1 = (sc->vx + sc->vw) * sx, y1 = (sc->vy + sc->vh) * sy;
    unsigned k = RGBA8(0, 0, 0, 255);
    if (x0 > 0.01f) { vita2d_draw_rectangle(0, 0, x0, SCREEN_H, k); vita2d_draw_rectangle(x1, 0, SCREEN_W - x1, SCREEN_H, k); }
    if (y0 > 0.01f) { vita2d_draw_rectangle(0, 0, SCREEN_W, y0, k); vita2d_draw_rectangle(0, y1, SCREEN_W, SCREEN_H - y1, k); }
    vita2d_end_drawing();
    vita2d_wait_rendering_done();
}

/* time spent per stage (us), logged every 300 frames: shows where an export's time goes */
static uint64_t g_t_render, g_t_csc, g_t_wait, g_t0;
static int g_t_n;

static void stats_reset(void) { g_t_render = g_t_csc = g_t_wait = 0; g_t_n = 0; g_t0 = 0; }

static void stats_log(void)
{
    if (!g_t_n) return;
    uint64_t wall = time_us() - g_t0;
    LOG("vexport: per frame: %.1f ms overall; render %.1f ms, colour conversion %.1f ms (%s), waiting for a free slot %.1f ms; "
        "feeder: add video %.1f ms, add sound %.1f ms; mixing %.1f ms (%d frames)",
        wall / 1000.0 / g_t_n, g_t_render / 1000.0 / g_t_n, g_t_csc / 1000.0 / g_t_n,
        X.csc_mode == 0 ? "hardware" : X.csc_mode == 1 ? "hardware, copied" : "CPU", g_t_wait / 1000.0 / g_t_n,
        X.t_vadd / 1000.0 / g_t_n, X.t_audio / 1000.0 / g_t_n, X.t_mix / 1000.0 / g_t_n, g_t_n);
    g_t_render = g_t_csc = g_t_wait = 0;
    X.t_vadd = X.t_audio = X.t_mix = 0;
    g_t_n = 0;
    g_t0 = time_us();
}

void vexport_step(void)
{
    if (!g_vexp.running) return;
    if (g_vexp.cancel) { stats_log(); finish(1); return; }
    if (!g_t0) g_t0 = time_us();
    /* render as many frames as fit in ~50 ms, then let the progress screen draw */
    uint64_t start = time_us();
    while (time_us() - start < 50000) {
        if (X.err) {
            if (X.direct) fail(X.err_video ? "The video encoder failed" : "The sound encoder or the file write failed", X.err);
            else fail(X.err_video ? "The recorder stopped taking frames" : "The recorder stopped taking sound", X.err);
            stats_log();
            finish(1);
            return;
        }
        if (X.queued >= g_vexp.nframes) {   /* everything rendered: wait for the threads to hand it all over */
            if (X.vdone >= g_vexp.nframes && X.ablocks >= X.ablocks_total && (!X.direct || X.ablocks_done)) { stats_log(); finish(0); return; }
            sleep_us(2000);
            continue;
        }
        uint64_t t0 = time_us();
        SceUInt to = 2000;
        if (sceKernelWaitSema(X.sem_free, 1, &to) < 0) { g_t_wait += time_us() - t0; continue; }
        uint64_t t1 = time_us();
        int k = X.queued;
        render(k * 1001.0 / 30000.0);
        uint64_t t2 = time_us();
        convert(X.yuv[k % NYUV]);
        uint64_t t3 = time_us();
        X.queued = k + 1;
        sceKernelSignalSema(X.sem_full, 1);
        g_t_wait += t1 - t0; g_t_render += t2 - t1; g_t_csc += t3 - t2;
        g_vexp.frame = k + 1;
        g_vexp.progress = (float)X.vdone / g_vexp.nframes;
        if (++g_t_n == 300) stats_log();
    }
    g_vexp.progress = (float)X.vdone / g_vexp.nframes;
}
