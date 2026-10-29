/* vdec.c - H.264 (sceAvcdec) and MPEG-4 Part 2 (sceM4vdec) through the Vita's hardware decoders into the frame cache.
 *
 * sceM4vdec isn't in any SDK header. Found so far (probing on hardware): its codec type is 0x1002, and its decoder
 * size query takes the same {width, height, refs} struct as AVC's (320x240 -> 256 KB, 720x480 -> 1280 KB). The rest
 * of its interface is assumed to mirror sceAvcdec (same wrappers, same module) and every step is logged.
 *
 * Only the frames the project actually shows are kept (Sparta remixes reuse a few seconds of a source thousands
 * of times), so playback never has to seek a decoder: every visible frame is already a GPU texture.
 * Frames come out in display order; each access unit carries its display index as PTS. */
#include "common.h"
#include "media.h"
#include "mp4.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>
#include <psp2/videodec.h>

/* in the SceVideodec stub but not in vita-headers' videodec.h */
int sceAvcdecDecodeStop(const SceAvcdecCtrl *decoder, SceAvcdecArrayPicture *array_picture);
/* MPEG-4 Part 2: prototypes modelled on the AVC twins */
int sceM4vdecQueryDecoderMemSize(uint32_t type, const SceAvcdecQueryDecoderInfo *q, SceAvcdecDecoderInfo *info);
int sceM4vdecCreateDecoder(uint32_t type, SceAvcdecCtrl *ctrl, const SceAvcdecQueryDecoderInfo *q);
int sceM4vdecCreateDecoderInternal(uint32_t type, SceAvcdecCtrl *ctrl, const SceAvcdecQueryDecoderInfo *q);
int sceM4vdecDeleteDecoder(SceAvcdecCtrl *ctrl);
int sceM4vdecDecode(const SceAvcdecCtrl *ctrl, const SceAvcdecAu *au, SceAvcdecArrayPicture *pics);
int sceM4vdecDecodeStop(const SceAvcdecCtrl *ctrl, SceAvcdecArrayPicture *pics);
#define TYPE_M4V 0x1002u

typedef struct {
    const char *name;
    uint32_t type;
    int (*del)(SceAvcdecCtrl *);
    int (*decode)(const SceAvcdecCtrl *, const SceAvcdecAu *, SceAvcdecArrayPicture *);
    int (*stop)(const SceAvcdecCtrl *, SceAvcdecArrayPicture *);
} Codec;
static const Codec AVC = { "H.264", SCE_VIDEODEC_TYPE_HW_AVCDEC, sceAvcdecDeleteDecoder, sceAvcdecDecode, sceAvcdecDecodeStop };
static const Codec M4V = { "MPEG-4", TYPE_M4V, sceM4vdecDeleteDecoder, sceM4vdecDecode, sceM4vdecDecodeStop };

#define ALIGN16(x) (((x) + 15) & ~15)
#define ALIGN_MB(x) (((x) + 0xFFFFF) & ~0xFFFFF)

static int g_vdec_inited;
static int g_max_w, g_max_h, g_max_refs;
/* decoder work memory and the output picture are reserved once, for the largest source, before the frame-cache
 * arenas take what is left of the RAM */
static SceUID g_dec_mem = -1, g_pic_mem = -1;
static void *g_dec_base; static uint8_t *g_pic_base;
static size_t g_dec_size;

static int g_m4v_inited;

/* the MPEG-4 library set-up. sceVideodecInitLibrary refuses type 0x1002 with every struct shape tried (0x80620802,
 * the same answer it gives for nonexistent types: games may only get AVC through it), and the decoder then says
 * "not initialised" (0x8062090C). So the Internal and Nongameapp variants are tried too (prototypes assumed to match
 * the public one), each with a few shapes; every result is logged. */
int sceVideodecInitLibraryInternal(uint32_t type, const void *init);
int sceVideodecInitLibraryNongameapp(uint32_t type, const void *init);
int sceVideodecQueryMemSizeInternal(uint32_t type, const void *init, void *mem);
int sceVideodecQueryMemSizeNongameapp(uint32_t type, const void *init, void *mem);

static void m4v_lib_init(int w, int h)
{
    uint32_t W = (uint32_t)ALIGN16(w), H = (uint32_t)ALIGN16(h);
    uint32_t v[][8] = { { 0x14, W, H, 2, 1 }, { 0x10, W, H, 1 }, { 0x14, W, H, 0, 1 }, { 0x20, W, H, 2, 1 } };
    const int nv = (int)(sizeof v / sizeof *v);
    /* found on hardware: InitLibraryInternal with the AVC-shaped struct (size 0x14) works */
    static const char *names[] = { "InitLibraryInternal", "InitLibrary", "InitLibraryNongameapp" };
    for (int f = 0; f < 3; f++) {
        for (int i = 0; i < nv; i++) {
            int r = f == 1 ? sceVideodecInitLibrary(TYPE_M4V, (void *)v[i])
                  : f == 0 ? sceVideodecInitLibraryInternal(TYPE_M4V, v[i]) : sceVideodecInitLibraryNongameapp(TYPE_M4V, v[i]);
            LOG("vdec: sceVideodec%s(MPEG-4, shape %d: size 0x%X): 0x%08X", names[f], i, (unsigned)v[i][0], (unsigned)r);
            if (r >= 0) { g_m4v_inited = 1; return; }
        }
    }
}

/* TEMPORARY research: find a system library that uses sceM4vdec (its code shows how the decoder is created).
 * Loads each public system module in turn, looks through every loaded module for the M4vdec function NIDs in its
 * import tables, logs where they are found and saves those modules to ux0:data/vegplayer/m4vdec/; then unloads
 * what it loaded. */
static void m4v_sweep(void)
{
    static const uint32_t NIDS[] = { 0x17C6AC9E, 0x4D4CA34E, 0xB4BC325B, 0x624664DB, 0xA8CF1942, 0x7C460D75, 0x87CFD23B, 0xC1CF351F,
                                     0x8F36574D, 0x0EB2E4E7, 0xC67C1A80, 0x84DC8101 };
    /* the media-related ones: MP4 (sceMp4 player), AacEnc, Atrac, VideoExport, AudioCodec, VideoSearch, BEISOBMF,
     * BEMP2SYS, MusicExport, AvPlayer, Mp4Recorder, AvcDec, PhotoExport, ScreenShot */
    static const int IDS[] = { 0x2D, 0x2E, 0x30, 0x32, 0x37, 0x45, 0x47, 0x48, 0x49, 0x4C, 0x51, 0x54, 0x16, 0x1D };
    int loaded[0x56] = { 0 };
    for (size_t i = 0; i < sizeof IDS / sizeof *IDS; i++) {
        int id = IDS[i];
        if (sceSysmoduleIsLoaded((SceSysmoduleModuleId)id) == 0) continue;
        int r = sceSysmoduleLoadModule((SceSysmoduleModuleId)id);
        LOG("m4vsweep: load sysmodule 0x%02X: 0x%08X", id, (unsigned)r);
        if (r >= 0) loaded[id] = 1;
    }
    mkdir_p(APP_DIR "/m4vdec");
    SceUID ids[256];
    SceSize n = 256;
    if (sceKernelGetModuleList(0xFF, ids, &n) >= 0) {
        LOG("m4vsweep: %u modules loaded", (unsigned)n);
        for (SceSize i = 0; i < n; i++) {
            SceKernelModuleInfo mi;
            memset(&mi, 0, sizeof mi);
            mi.size = sizeof mi;
            if (sceKernelGetModuleInfo(ids[i], &mi) < 0) continue;
            mi.module_name[sizeof mi.module_name - 1] = 0;
            if (!strcmp(mi.module_name, "SceAvcodecUser") || !strcmp(mi.module_name, "VegPlayer")) continue;
            int hits = 0;
            for (int k = 0; k < 4; k++) {
                const uint32_t *w = mi.segments[k].vaddr;
                if (!w || !(mi.segments[k].perms & 4)) continue;   /* readable */
                for (size_t j = 0; j < mi.segments[k].memsz / 4; j++)
                    for (size_t q = 0; q < sizeof NIDS / sizeof *NIDS; q++)
                        if (w[j] == NIDS[q]) hits++;
            }
            if (!hits) continue;
            LOG("m4vsweep: %s (%s) has %d M4vdec NID(s)", mi.module_name, mi.path, hits);
            for (int k = 0; k < 4; k++) {
                if (!mi.segments[k].memsz || !mi.segments[k].vaddr) continue;
                char path[200];
                snprintf(path, sizeof path, APP_DIR "/m4vdec/%s_seg%d_%08X.bin", mi.module_name, k, (unsigned)(uintptr_t)mi.segments[k].vaddr);
                FILE *f = fopen(path, "wb");
                if (!f) continue;
                fwrite(mi.segments[k].vaddr, 1, mi.segments[k].memsz, f);
                fclose(f);
                LOG("m4vsweep: saved %s", path);
            }
        }
    }
    for (int id = 0x55; id >= 1; id--) if (loaded[id]) sceSysmoduleUnloadModule((SceSysmoduleModuleId)id);
}

int vdec_global_init(int max_w, int max_h, int max_refs, int with_m4v)
{
    if (g_vdec_inited) return 0;
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);   /* not required on every firmware; harmless */
    SceVideodecQueryInitInfoHwAvcdec init;
    memset(&init, 0, sizeof init);
    init.size = sizeof init;
    init.horizontal = (uint32_t)ALIGN16(max_w);
    init.vertical = (uint32_t)ALIGN16(max_h);
    init.numOfRefFrames = (uint32_t)max_refs;
    init.numOfStreams = 1;
    int r = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (r < 0) { LOG("vdec: sceVideodecInitLibrary(%dx%d, refs %d) failed 0x%08X", max_w, max_h, max_refs, r); return r; }
    g_vdec_inited = 1;
    g_max_w = (int)init.horizontal; g_max_h = (int)init.vertical; g_max_refs = max_refs;

    SceAvcdecQueryDecoderInfo q;
    memset(&q, 0, sizeof q);
    q.horizontal = init.horizontal; q.vertical = init.vertical; q.numOfRefFrames = init.numOfRefFrames;
    SceAvcdecDecoderInfo info;
    memset(&info, 0, sizeof info);
    r = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &info);
    if (r < 0) { LOG("vdec: size query failed 0x%08X", r); vdec_global_term(); return r; }
    g_dec_size = ALIGN_MB(info.frameMemSize);
    g_dec_mem = sceKernelAllocMemBlock("vp_avcdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, g_dec_size, NULL);
    size_t psz = ALIGN_MB((size_t)g_max_w * (size_t)g_max_h * 4);
    g_pic_mem = sceKernelAllocMemBlock("vp_avcpic", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, psz, NULL);
    if (g_dec_mem < 0 || g_pic_mem < 0) {
        LOG("vdec: no physically contiguous memory (%u KB + %u KB)", (unsigned)(g_dec_size >> 10), (unsigned)(psz >> 10));
        vdec_global_term();
        return -1;
    }
    sceKernelGetMemBlockBase(g_dec_mem, &g_dec_base);
    sceKernelGetMemBlockBase(g_pic_mem, (void **)&g_pic_base);
    LOG("vdec: hardware AVC decoder ready (%dx%d, %d refs, %u KB)", g_max_w, g_max_h, max_refs, (unsigned)(g_dec_size >> 10));
    if (with_m4v) { static int swept; if (!swept) { swept = 1; m4v_sweep(); } }
    if (with_m4v && !g_m4v_inited) m4v_lib_init(max_w, max_h);
    return 0;
}

void vdec_global_term(void)
{
    if (g_dec_mem >= 0) sceKernelFreeMemBlock(g_dec_mem);
    if (g_pic_mem >= 0) sceKernelFreeMemBlock(g_pic_mem);
    g_dec_mem = g_pic_mem = -1;
    if (g_m4v_inited) sceVideodecTermLibrary(TYPE_M4V);
    g_m4v_inited = 0;
    if (!g_vdec_inited) return;
    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    g_vdec_inited = 0;
}

typedef struct {
    const Codec *codec;
    SceAvcdecCtrl ctrl;
    SceUID m4v_mem;         /* MPEG-4: its own decoder memory */
    uint8_t *pic;           /* RGBA8888 output picture (physically contiguous, uncached) */
    int pitch, fw, fh;      /* picture buffer pitch / size in pixels */
    int created;
    int w, h, refs;
} Dec;

static void dec_destroy(Dec *d)
{
    if (d->created) d->codec->del(&d->ctrl);
    d->created = 0;
    if (d->m4v_mem > 0) sceKernelFreeMemBlock(d->m4v_mem);
    d->m4v_mem = 0;
}

static int dec_create(Dec *d, const Codec *codec, int w, int h, int refs, char *err, size_t errlen)
{
    memset(d, 0, sizeof *d);
    d->codec = codec;
    if (g_dec_mem < 0) { snprintf(err, errlen, "hardware decoder not initialised"); return -1; }
    SceAvcdecQueryDecoderInfo q;
    memset(&q, 0, sizeof q);
    q.horizontal = (uint32_t)ALIGN16(w);
    q.vertical = (uint32_t)ALIGN16(h);
    q.numOfRefFrames = (uint32_t)refs;
    int r;
    if (codec == &M4V) {
        SceAvcdecDecoderInfo info;
        memset(&info, 0, sizeof info);
        r = sceM4vdecQueryDecoderMemSize(TYPE_M4V, &q, &info);
        if (r < 0) { snprintf(err, errlen, "MPEG-4 decoder size query failed (0x%08X)", r); return -1; }
        size_t sz = ALIGN_MB(info.frameMemSize);
        d->m4v_mem = sceKernelAllocMemBlock("vp_m4vdec", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
        if (d->m4v_mem < 0) { snprintf(err, errlen, "no contiguous memory for the MPEG-4 decoder (0x%08X)", d->m4v_mem); d->m4v_mem = 0; return -1; }
        void *base = NULL;
        sceKernelGetMemBlockBase(d->m4v_mem, &base);
        d->ctrl.frameBuf.pBuf = base;
        d->ctrl.frameBuf.size = (uint32_t)sz;
        /* as with the library init, games may only get the public call refused: try it, then the Internal one, each
         * with the memory block as reserved and with exactly the size the query asked for */
        static int logged;
        for (int a = 0; a < 4; a++) {
            d->ctrl.handle = 0;
            d->ctrl.frameBuf.size = a & 1 ? info.frameMemSize : (uint32_t)sz;
            r = a < 2 ? sceM4vdecCreateDecoder(TYPE_M4V, &d->ctrl, &q) : sceM4vdecCreateDecoderInternal(TYPE_M4V, &d->ctrl, &q);
            if (logged < 8) {
                logged++;
                LOG("vdec: sceM4vdecCreateDecoder%s(%dx%d, buffer %u bytes): 0x%08X, handle 0x%08X", a < 2 ? "" : "Internal", w, h,
                    (unsigned)d->ctrl.frameBuf.size, (unsigned)r, (unsigned)d->ctrl.handle);
            }
            if (r >= 0) break;
        }
    } else {
        d->ctrl.frameBuf.pBuf = g_dec_base;
        d->ctrl.frameBuf.size = (uint32_t)g_dec_size;
        r = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &d->ctrl, &q);
    }
    if (r < 0) { snprintf(err, errlen, "%s decoder creation failed (0x%08X)", codec->name, r); dec_destroy(d); return -1; }
    d->created = 1;
    d->pitch = ALIGN16(w); d->fw = ALIGN16(w); d->fh = ALIGN16(h);
    d->pic = g_pic_base;
    d->w = w; d->h = h; d->refs = refs;
    return 0;
}

static void pic_setup(Dec *d, SceAvcdecPicture *pic, SceAvcdecPicture **pp, SceAvcdecArrayPicture *arr)
{
    memset(pic, 0, sizeof *pic);
    memset(arr, 0, sizeof *arr);
    pic->size = sizeof *pic;
    pic->frame.pixelType = SCE_AVCDEC_PIXELFORMAT_RGBA8888;
    pic->frame.framePitch = (uint32_t)d->pitch;
    pic->frame.frameWidth = (uint32_t)d->fw;
    pic->frame.frameHeight = (uint32_t)d->fh;
    pic->frame.opt.rgba.alpha = 0xFF;
    pic->frame.pPicture[0] = d->pic;
    *pp = pic;
    arr->numOfElm = 1;
    arr->pPicture = pp;
}

typedef struct {
    VideoCache *vc;
    const uint8_t *need;
    int *pending; int npending;     /* display indices fed to the decoder, not yet output */
    int next_slot;
    uint8_t *scratch;
    int src_w, src_h;
    int produced;
    int logged_m4v;
} Ctx;

static void pending_add(Ctx *c, int k)
{
    if (c->npending < 256) c->pending[c->npending++] = k;
}

/* a picture came out: figure out which display frame it is and keep it if needed */
static void got_picture(Ctx *c, Dec *d, const SceAvcdecPicture *pic)
{
    int k = -1, at = -1;
    uint32_t pts = pic->info.pts.lower;
    for (int i = 0; i < c->npending; i++) if ((uint32_t)c->pending[i] == pts && pic->info.pts.upper == 0) { at = i; break; }
    if (at < 0) {   /* no usable PTS: pictures leave in display order, so it's the smallest pending index */
        for (int i = 0; i < c->npending; i++) if (at < 0 || c->pending[i] < c->pending[at]) at = i;
    }
    if (at < 0) return;
    k = c->pending[at];
    c->pending[at] = c->pending[--c->npending];
    VideoCache *vc = c->vc;
    if (k < 0 || k >= vc->nframes || !c->need[k] || vc->slot[k] >= 0 || c->next_slot >= vc->nimg) return;
    int s = c->next_slot++;
    downscale_rgba(d->pic, d->pitch * 4, c->src_w, c->src_h, &vc->img[s], 0, c->scratch);
    if (vc->imgq && vc->imgq[s].data) downscale_rgba(d->pic, d->pitch * 4, c->src_w, c->src_h, &vc->imgq[s], 0, c->scratch);
    vc->slot[k] = s;
    c->produced++;
}

/* end of a run of access units: get the pictures still held for reordering, then start a fresh decoder so the
 * next run can begin at any keyframe */
static void drain(Ctx *c, Dec *d)
{
    if (!d->created) return;
    for (int i = 0; i < 64; i++) {
        SceAvcdecPicture pic, *pp;
        SceAvcdecArrayPicture arr;
        pic_setup(d, &pic, &pp, &arr);
        int r = d->codec->stop(&d->ctrl, &arr);
        if (r < 0 || arr.numOfOutput == 0) break;
        got_picture(c, d, &pic);
    }
    c->npending = 0;
    int w = d->w, h = d->h, refs = d->refs;
    const Codec *codec = d->codec;
    char err[96];
    dec_destroy(d);
    if (dec_create(d, codec, w, h, refs, err, sizeof err) < 0) LOG("vdec: re-create failed: %s", err);
}

int vdec_fill_cache(Mp4File *mf, VideoCache *vc, const uint8_t *need, volatile float *progress, volatile int *cancel, char *err, size_t errlen)
{
    Mp4Track *t = &mf->video;
    const Codec *codec = t->codec == 0x6D703476u ? &M4V : &AVC;   /* 'mp4v' */
    int w = t->width, h = t->height;
    if (w <= 0 || h <= 0) { snprintf(err, errlen, "bad video size"); return -1; }
    if (ALIGN16(w) > g_max_w || ALIGN16(h) > g_max_h) {
        snprintf(err, errlen, "%dx%d is above the hardware decoder's set-up size (%dx%d)", w, h, g_max_w, g_max_h);
        return -1;
    }
    /* display order <-> decode order */
    int n = (int)t->n;
    int *disp_of = malloc(sizeof(int) * (size_t)n), *samp_of = malloc(sizeof(int) * (size_t)n);
    int *pend = malloc(sizeof(int) * 256);
    uint8_t *es = memalign(64, 2u << 20);
    uint8_t *raw = malloc(2u << 20);
    Ctx c;
    memset(&c, 0, sizeof c);
    c.scratch = malloc(downscale_scratch_size(w, h, vc->cache_w, vc->cache_h));
    if (!disp_of || !samp_of || !pend || !es || !raw || !c.scratch) {
        free(disp_of); free(samp_of); free(pend); free(es); free(raw); free(c.scratch);
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    /* display index = rank of cts; vc->pts already holds the sorted times, rebuild the mapping the same way */
    for (int i = 0; i < n; i++) samp_of[i] = i;
    for (int i = 1; i < n; i++) {   /* insertion sort by cts: input is nearly sorted */
        int x = samp_of[i], j = i - 1;
        while (j >= 0 && t->s[samp_of[j]].cts > t->s[x].cts) { samp_of[j + 1] = samp_of[j]; j--; }
        samp_of[j + 1] = x;
    }
    for (int k = 0; k < n; k++) disp_of[samp_of[k]] = k;

    int refs = t->max_ref_frames > 0 ? t->max_ref_frames + 1 : g_max_refs;
    if (refs > g_max_refs) refs = g_max_refs;
    if (codec == &M4V) refs = 2;   /* I/P (and B) pictures: two references */
    Dec d;
    if (dec_create(&d, codec, w, h, refs, err, errlen) < 0) {
        free(disp_of); free(samp_of); free(pend); free(es); free(raw); free(c.scratch);
        return -1;
    }
    c.vc = vc; c.need = need; c.pending = pend; c.src_w = w; c.src_h = h;

    int total_need = 0;
    for (int k = 0; k < n; k++) total_need += need[k] ? 1 : 0;
    int pos = -1;          /* next sample to feed (decode order), -1 = not started */
    int errors = 0, result = 0;
    while (!*cancel) {
        /* next wanted frame */
        int kt = -1;
        for (int k = 0; k < n; k++) if (need[k] && vc->slot[k] < 0 && !(need[k] & 2)) { kt = k; break; }
        if (kt < 0) break;
        int st = samp_of[kt];
        int sync = st;
        while (sync > 0 && !t->s[sync].sync) sync--;
        if (pos < 0 || sync > pos || st < pos - 64) {   /* jump to the GOP that holds it */
            if (pos >= 0) drain(&c, &d);
            pos = sync;
        }
        /* decode until the wanted frame has come out (or the GOP ran out) */
        int fed_target = 0;
        while (pos < n && !*cancel) {
            if (t->s[pos].size > (2u << 20) - 1024) { pos++; continue; }
            if (mp4_read_sample(mf, t, (uint32_t)pos, raw) < 0) { errors++; pos++; continue; }
            int len;
            if (codec == &M4V) {   /* elementary stream as is; the VOS/VOL headers (esds) before each keyframe */
                uint32_t hl = t->s[pos].sync && t->dsi_len > 0 ? (uint32_t)t->dsi_len : 0;
                if (hl + t->s[pos].size > (2u << 20)) { pos++; continue; }
                if (hl) memcpy(es, t->dsi, hl);
                memcpy(es + hl, raw, t->s[pos].size);
                len = (int)(hl + t->s[pos].size);
            } else len = mp4_avcc_to_annexb(t, raw, t->s[pos].size, es, 2u << 20, t->s[pos].sync);
            if (len <= 0) { pos++; continue; }
            SceAvcdecAu au;
            memset(&au, 0, sizeof au);
            au.pts.upper = 0; au.pts.lower = (uint32_t)disp_of[pos];
            au.dts.upper = 0xFFFFFFFF; au.dts.lower = 0xFFFFFFFF;
            au.es.pBuf = es; au.es.size = (uint32_t)len;
            SceAvcdecPicture pic, *pp;
            SceAvcdecArrayPicture arr;
            pic_setup(&d, &pic, &pp, &arr);
            pending_add(&c, disp_of[pos]);
            int r = codec->decode(&d.ctrl, &au, &arr);
            if (codec == &M4V && c.logged_m4v < 3) {
                c.logged_m4v++;
                LOG("vdec: sceM4vdecDecode(sample %d, %d bytes): 0x%08X, %u picture(s) out", pos, len, (unsigned)r, (unsigned)arr.numOfOutput);
            }
            if (r < 0) {
                errors++;
                if (errors < 8) LOG("vdec: %s decode error 0x%08X at sample %d", codec->name, r, pos);
                c.npending--;   /* this AU produced nothing and never will */
            } else if (arr.numOfOutput) got_picture(&c, &d, &pic);
            if (pos == st) fed_target = 1;
            pos++;
            if (vc->slot[kt] >= 0) break;
            /* the target was fed a while ago and still nothing: give up on it (reordering never exceeds 16) */
            if (fed_target && pos > st + 20) break;
            if (pos < n && t->s[pos].sync && pos > st) break;   /* next GOP: drain decides */
        }
        if (vc->slot[kt] < 0) {
            drain(&c, &d);
            pos = -1;
            if (vc->slot[kt] < 0) ((uint8_t *)need)[kt] |= 2;   /* mark as failed so we don't loop */
        }
        if (progress) *progress = total_need ? (float)c.produced / (float)total_need : 1.0f;
        if (errors > 200) { snprintf(err, errlen, "too many decode errors"); result = -1; break; }
    }
    drain(&c, &d);
    dec_destroy(&d);
    LOG("vdec: %s: kept %d of %d needed frames (%d errors)", codec->name, c.produced, total_need, errors);
    free(disp_of); free(samp_of); free(pend); free(es); free(raw); free(c.scratch);
    if (!c.produced && result == 0) { snprintf(err, errlen, "no frames could be decoded"); result = -1; }
    return result;
}
