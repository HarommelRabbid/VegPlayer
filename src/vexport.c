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
#include <psp2/sysmodule.h>
#include <vita2d.h>

#include "../third_party/sce/mp4rec.h"

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
};

VExport g_vexp;

#define VIDEO_DIR "ux0:data/vegplayer/vexports"   /* where the finished file is moved */
#define REC_DIR "ux0:video"                       /* where the recorder itself saves */
#define NYUV 3                     /* frames handed to the recorder rotate through this many buffers */
#define NAUD 4
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

static int convert(uint8_t *dst)
{
    int w = X.w, h = X.h;
    int stride = (int)(vita2d_texture_get_stride(X.rt) / 4);
    uint8_t *src = vita2d_texture_get_datap(X.rt);
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

static int add_audio_block(void)
{
    int16_t *pcm = X.aud[X.ablocks % NAUD];
    mixer_mix(X.mix, AUD_FRAMES, X.ablocks * AUD_FRAMES);
    mixer_to_pcm(X.mix, pcm, AUD_FRAMES * 2);
    int r = add_sample(0, pcm, SCE_MP4REC_AUDIO_BUFFER_SIZE);
    if (r < 0) return r;
    X.ablocks++;
    return 0;
}

static void cleanup(void)
{
    if (X.inited || X.created) {
        if (X.created) {
            int r = sceMp4RecDeleteRecorder(&X.rec);
            LOG("vexport: sceMp4RecDeleteRecorder: 0x%08X", (unsigned)r);
        }
        X.inited = X.created = 0;
    }
    if (X.rt) { vita2d_wait_rendering_done(); vita2d_free_texture(X.rt); X.rt = NULL; }
    g_vexp.preview = NULL;
    mem_free(&X.work); mem_free(&X.enc); mem_free(&X.av); mem_free(&X.base);
    free(X.mix); X.mix = NULL;
    vita2d_set_vblank_wait(1);
}

static void finish(int discard)
{
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

    list_videos(REC_DIR, &X.before, &X.nbefore, 0);
    double dur = g_s.proj.duration;
    if (dur > REC_MAX_S) { LOG("vexport: the recorder stops at 30 minutes: exporting the first 30"); dur = REC_MAX_S; }
    if (dur < REC_MIN_S) dur = REC_MIN_S;   /* the recorder's minimum: the rest is silent black */
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
    vita2d_start_drawing_advanced(X.rt, 0);
    SceGxmContext *ctx = vita2d_get_context();
    sceGxmSetViewport(ctx, X.w * 0.5f, X.w * 0.5f, X.h * 0.5f, -X.h * 0.5f, 0.5f, 0.5f);
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    vita2d_clear_screen();
    const Scene *sc = scene_build_now(T);
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

void vexport_step(void)
{
    if (!g_vexp.running) return;
    if (g_vexp.cancel) { finish(1); return; }
    if (g_vexp.frame >= g_vexp.nframes) {
        while (X.ablocks < X.ablocks_total)
            if (add_audio_block() < 0) { fail("The recorder stopped taking sound", 0); finish(1); return; }
        finish(0);
        return;
    }
    int k = g_vexp.frame;
    render(k * 1001.0 / 30000.0);
    uint8_t *f = X.yuv[k % NYUV];
    convert(f);
    int r = add_sample(1, f, X.w * X.h * 3 / 2);
    if (r < 0) { fail("The recorder stopped taking frames", r); finish(1); return; }
    /* sound up to the end of this frame */
    int64_t until = (int64_t)(k + 1) * 1001 * OUT_RATE;   /* x 30000 */
    while (X.ablocks < X.ablocks_total && X.ablocks * AUD_FRAMES * 30000 < until)
        if ((r = add_audio_block()) < 0) { fail("The recorder stopped taking sound", r); finish(1); return; }
    g_vexp.frame = k + 1;
    g_vexp.progress = (float)g_vexp.frame / g_vexp.nframes;
}
