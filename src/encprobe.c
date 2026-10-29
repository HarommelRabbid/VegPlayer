/* encprobe.c - test of the hardware H.264 encoder (sceAvcenc) at sizes the system's MP4 recorder doesn't offer.
 *
 * Nothing about this API is public. The calls and their structs below copy what the system's own recorder
 * (SceLibMp4Recorder) does, read from its code; the kernel side (SceAvcodec) checks the sizes of each struct and
 * limits the encoder library to 64..960 x 64..576. Each step's result goes to the log, and the stream of each
 * size that works is saved as an Annex B .h264 file next to the video exports. */
#include "common.h"
#include "encprobe.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/sysmodule.h>

#define ENC_TYPE   0x1002u        /* the encoder library's codec type (H.264) */
#define OUT_DIR    "ux0:data/vegplayer/vexports"
#define NFRAMES    60

/* sceVideoencInitLibrary* encoder part (the kernel copies 0x20 bytes and wants size 0x14) */
typedef struct { uint32_t size, width, height, n_encoders, n_unk, rsv[3]; } EncLibInit;
/* memory handed to sceVideoencInitLibraryWithUnmapMem (the recorder fills words 3 and 4 only) */
typedef struct { uint32_t rsv[3]; void *addr; uint32_t size; uint32_t rsv2[3]; } EncLibMem;
/* sceAvcencQueryEncoderMemSize / sceAvcencCreateEncoder settings, 0x34 bytes */
typedef struct {
    uint32_t pitch, width, height;
    uint32_t width2, height2;      /* the recorder repeats the size here */
    uint32_t one;
    uint32_t bitrate, max_bitrate; /* bits per second; the recorder's max is 1.2 x the bitrate */
    uint8_t flags;                 /* 0x89 in the recorder */
    uint8_t b21;                   /* 4 above 480 wide, else 2 */
    uint8_t pad22[2];
    uint32_t tick, time_scale;     /* frame rate = time_scale / (2 * tick): 4004 / 240000 = 29.97 */
    uint8_t b2c;                   /* 1 */
    uint8_t colour;                /* 1 BT.601, 2 BT.709 */
    uint8_t pad2e[2];
    uint16_t s30, s32;
} EncInfo;
typedef struct { uint32_t mem_a, mem_b, stream_size; } EncMemOut;
/* the encoder object, 0x34 bytes */
typedef struct {
    uint32_t handle;
    void *mem_a; uint32_t mem_a_size;
    void *mem_b; uint32_t mem_b_size;
    void *work;  uint32_t work_size;   /* 0x400 bytes in the recorder */
    uint32_t rsv[6];
} AvcEncoder;
typedef struct { uint32_t r[6]; } EncParam;   /* 0x18 bytes: the recorder sends { 0, 0, 0, 0x1C, 0, 0 } */
/* a picture (encoder input and colour conversion), 0x30 bytes */
typedef struct {
    uint32_t size, pixelformat, pitch, width, height;
    uint8_t opt[16];
    void *buffer, *buffer2;
    uint32_t flags;                /* 1 = encode this picture; 0x80 = skip (no buffer) */
} EncPicture;
/* an output stream slot, 0x28 bytes */
typedef struct {
    uint32_t size;
    uint32_t out[6];               /* filled by the encoder: timestamps and such */
    void *buf; uint32_t buf_size;  /* in: the buffer and its size; buf_size out: bytes used */
    uint8_t n_nal, pic_type, pad[2];
} EncStream;
typedef struct { uint32_t count, max; EncStream **streams; } EncStreamArray;
typedef struct { uint8_t *addr; uint32_t size; uint8_t type, pad[3]; } EncNal;

int sceVideoencQueryMemSize(uint32_t type, const void *dec, const EncLibInit *enc, uint32_t *size);
int sceVideoencInitLibrary(uint32_t type, const void *dec, const EncLibInit *enc);
int sceVideoencInitLibraryWithUnmapMem(uint32_t type, const EncLibMem *mem, const void *dec, const EncLibInit *enc);
int sceVideoencTermLibrary(uint32_t type);
int sceAvcencQueryEncoderMemSize(uint32_t type, const EncInfo *info, EncMemOut *out);
int sceAvcencCreateEncoder(uint32_t type, AvcEncoder *enc, const EncInfo *info);
int sceAvcencQueryEncoderMemSizeInternal(uint32_t type, const void *info50, EncMemOut *out);
int sceAvcencCreateEncoderInternal(uint32_t type, AvcEncoder *enc, const void *info50);
int sceAvcencDeleteEncoder(AvcEncoder *enc);
int sceAvcencSetEncoderParameter(AvcEncoder *enc, const EncParam *p);
int sceAvcencEncode(AvcEncoder *enc, const EncPicture *in, EncStreamArray *out);
int sceAvcencEncodeStop(AvcEncoder *enc, EncStreamArray *out);
int sceAvcencGetNalUnit(AvcEncoder *enc, int index, EncNal *nal);
int sceAvcencCsc(EncPicture *dst, const EncPicture *src);
SceUID sceCodecEngineOpenUnmapMemBlock(void *addr, SceSize size);
int sceCodecEngineCloseUnmapMemBlock(SceUID uid);
uintptr_t sceCodecEngineAllocMemoryFromUnmapMemBlock(SceUID uid, SceSize size, SceSize align);
int sceCodecEngineFreeMemoryFromUnmapMemBlock(SceUID uid, uintptr_t addr);

/* The encoder's full settings (0x50 bytes), used by the "Internal" calls. The normal calls take the 0x34-byte
 * settings above, check them against a 720x480 limit and expand them into this form, starting from one of four
 * built-in presets; the Internal calls only check the size against the library's (up to 960x576). This is the
 * kernel's preset 4, with the values the expansion would put in from the recorder's settings. */
static const uint8_t PRESET4[0x50] = {
    0x42, 0x1e, 0x01, 0x02, 0x80, 0x02, 0x00, 0x00, 0x80, 0x02, 0x00, 0x00, 0xe0, 0x01, 0x00, 0x00,
    0x80, 0x02, 0x00, 0x00, 0xe0, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x80, 0x96, 0x98, 0x00,
    0x00, 0x1b, 0xb7, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x84, 0x1e, 0x00, 0x00, 0x9f, 0x24, 0x00, 0x00, 0xc2, 0x01, 0x00, 0x3a, 0x00, 0x00, 0x00,
    0xa4, 0x0f, 0x00, 0x00, 0x80, 0xa9, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static void put32(uint8_t *b, int o, uint32_t v) { memcpy(b + o, &v, 4); }

/* byte 0 profile (0x42 Baseline), 1 level x 10, 3 (2 in the preset, 0 with the recorder's flags), 4 pitch, 8/12 size,
 * 16/20 size again (checked against the library), 24 1, 48 bitrate, 52 max bitrate, 56 w*h*3/8, 60 GOP?,
 * 64/68 tick / time scale, 72..79 VUI colour description */
static void build_full(uint8_t *b, int w, int h, int level, int b3, uint32_t bitrate)
{
    static const uint8_t VUI601[8] = { 0x01, 0x00, 0x02, 0x00, 0x01, 0x06, 0x06, 0x06 };
    memcpy(b, PRESET4, sizeof PRESET4);
    b[1] = (uint8_t)level; b[3] = (uint8_t)b3;
    put32(b, 4, (uint32_t)w); put32(b, 8, (uint32_t)w); put32(b, 12, (uint32_t)h);
    put32(b, 16, (uint32_t)w); put32(b, 20, (uint32_t)h); put32(b, 24, 1);
    b[0x24] = 0x10; b[0x28] = 1;
    put32(b, 48, bitrate); put32(b, 52, bitrate / 5 * 6);
    put32(b, 56, (uint32_t)(w * h * 3 / 8));
    put32(b, 64, 4004); put32(b, 68, 240000);
    memcpy(b + 72, VUI601, 8);
}

#define ALIGN(x, a) (((x) + (a) - 1) & ~(uint32_t)((a) - 1))

static SceUID alloc_block(const char *name, uint32_t size, int phys, void **p)
{
    static const struct { unsigned type; uint32_t gran; } PHYS[] = {
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 1u << 20 },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, 1u << 20 },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256u << 10 },
    };
    *p = NULL;
    for (int i = 0; i < (phys ? 3 : 1); i++) {
        unsigned type = phys ? PHYS[i].type : SCE_KERNEL_MEMBLOCK_TYPE_USER_RW;
        uint32_t gran = phys ? PHYS[i].gran : 4096;
        SceUID uid = sceKernelAllocMemBlock(name, type, ALIGN(size, gran), NULL);
        if (uid >= 0) { sceKernelGetMemBlockBase(uid, p); return uid; }
    }
    LOG("enctest: couldn't allocate %u KB for %s", size >> 10, name);
    return -1;
}

static void fill_frame(uint8_t *y, int w, int h, int f)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) y[j * w + i] = (uint8_t)(((i + f * 4) ^ (j + f * 2)) & 0xFF);
    /* a box that moves across, so motion is visible */
    int bx = (f * 12) % (w - 64), by = h / 2 - 32;
    for (int j = 0; j < 64; j++) memset(y + (by + j) * w + bx, 235, 64);
    uint8_t *uv = y + w * h;
    for (int j = 0; j < h / 2; j++)
        for (int i = 0; i < w; i += 2) { uv[j * w + i] = (uint8_t)(64 + i * 128 / w); uv[j * w + i + 1] = (uint8_t)(64 + j * 256 / h); }
}

static int g_types[32];
static void put_nals(AvcEncoder *e, EncStream *s, SceUID fd, int *n_nal, uint32_t *bytes, int verbose)
{
    for (int k = 0; k < s->n_nal; k++) {
        EncNal nal; memset(&nal, 0, sizeof nal);
        int r = sceAvcencGetNalUnit(e, k, &nal);
        if (r < 0) { LOG("enctest:   sceAvcencGetNalUnit(%d): 0x%08X", k, (unsigned)r); return; }
        uint8_t *b = s->buf;
        if (nal.addr < b + 4 || nal.addr + nal.size > b + s->buf_size) {
            LOG("enctest:   NAL %d: type %u, %u bytes at %p - outside the stream buffer %p", k, nal.type, nal.size, (void *)nal.addr, (void *)b);
            return;
        }
        if (verbose)
            LOG("enctest:   NAL %d: type %u, %u bytes at +%d, 4 bytes before: %02X %02X %02X %02X", k, nal.type, nal.size,
                (int)(nal.addr - (uint8_t *)s->buf), nal.addr[-4], nal.addr[-3], nal.addr[-2], nal.addr[-1]);
        static const uint8_t SC[4] = { 0, 0, 0, 1 };
        if (fd >= 0) { sceIoWrite(fd, SC, 4); sceIoWrite(fd, nal.addr, nal.size); }
        (*n_nal)++; *bytes += nal.size; g_types[nal.type & 31]++;
    }
}

/* full >= 0: the Internal calls with the full settings, byte 3 = full */
static int probe(int w, int h, int full)
{
    if (full >= 0) LOG("enctest: ---- %dx%d, full settings, level 3.1, byte 3 = %d ----", w, h, full);
    else LOG("enctest: ---- %dx%d ----", w, h);
    uint8_t fs[0x50];
    int ok = 0, lib = 0, created = 0;
    SceUID blk = -1, ub = -1, outb = -1, frb = -1, rgbb = -1;
    void *blkp = NULL, *outp = NULL, *frp = NULL, *rgbp = NULL, *work = NULL;
    uintptr_t m_lib = 0, m_a = 0, m_b = 0;
    AvcEncoder e; memset(&e, 0, sizeof e);

    EncLibInit li; memset(&li, 0, sizeof li);
    li.size = 0x14; li.width = (uint32_t)w; li.height = (uint32_t)h; li.n_encoders = 1; li.n_unk = 1;
    uint32_t lib_size = 0;
    int r = sceVideoencQueryMemSize(ENC_TYPE, NULL, &li, &lib_size);
    LOG("enctest: sceVideoencQueryMemSize: 0x%08X, %u KB", (unsigned)r, lib_size >> 10);
    if (r < 0) return 0;

    EncInfo in; memset(&in, 0, sizeof in);
    in.pitch = (uint32_t)w; in.width = (uint32_t)w; in.height = (uint32_t)h;
    in.width2 = (uint32_t)w; in.height2 = (uint32_t)h;
    in.one = 1;
    in.bitrate = w * h >= 640 * 368 * 2 ? 6000000 : 2000000; in.max_bitrate = in.bitrate / 5 * 6;
    in.flags = 0x89; in.b21 = w > 480 ? 4 : 2;
    in.tick = 4004; in.time_scale = 240000;
    in.b2c = 1; in.colour = 1;
    EncMemOut mo; memset(&mo, 0, sizeof mo);
    if (full >= 0) {
        build_full(fs, w, h, 31, full, in.bitrate);
        r = sceAvcencQueryEncoderMemSizeInternal(ENC_TYPE, fs, &mo);
    } else r = sceAvcencQueryEncoderMemSize(ENC_TYPE, &in, &mo);
    LOG("enctest: sceAvcencQueryEncoderMemSize: 0x%08X - %u KB, %u KB, stream buffer %u KB", (unsigned)r, mo.mem_a >> 10, mo.mem_b >> 10, mo.stream_size >> 10);
    if (r < 0) return 0;

    /* one physically contiguous block for the library and the encoder, carved up the way the recorder does */
    const uint32_t A = 0x40000;
    uint32_t total = ALIGN(lib_size, A) + ALIGN(mo.mem_a, A) + ALIGN(mo.mem_b, A) + (1u << 20);
    if ((blk = alloc_block("vp_enctest", total, 1, &blkp)) < 0) goto out;
    ub = sceCodecEngineOpenUnmapMemBlock(blkp, ALIGN(total, 1u << 20));
    LOG("enctest: sceCodecEngineOpenUnmapMemBlock(%u KB): 0x%08X", ALIGN(total, 1u << 20) >> 10, (unsigned)ub);
    if (ub >= 0) {
        m_lib = sceCodecEngineAllocMemoryFromUnmapMemBlock(ub, lib_size, A);
        m_a = sceCodecEngineAllocMemoryFromUnmapMemBlock(ub, mo.mem_a, A);
        m_b = sceCodecEngineAllocMemoryFromUnmapMemBlock(ub, mo.mem_b, A);
        LOG("enctest: unmap block allocations: %08X %08X %08X", (unsigned)m_lib, (unsigned)m_a, (unsigned)m_b);
        if (!m_lib || !m_a || !m_b) goto out;
        EncLibMem lm; memset(&lm, 0, sizeof lm);
        lm.addr = (void *)m_lib; lm.size = lib_size;
        r = sceVideoencInitLibraryWithUnmapMem(ENC_TYPE, &lm, NULL, &li);
        LOG("enctest: sceVideoencInitLibraryWithUnmapMem: 0x%08X", (unsigned)r);
    } else {
        /* without the codec engine's block: the plain library call, and the block carved up by hand */
        uintptr_t p = ALIGN((uintptr_t)blkp, A);
        m_a = p; m_b = ALIGN(m_a + mo.mem_a, A);
        r = sceVideoencInitLibrary(ENC_TYPE, NULL, &li);
        LOG("enctest: sceVideoencInitLibrary: 0x%08X", (unsigned)r);
    }
    if (r < 0) goto out;
    lib = 1;

    work = memalign(64, 0x400);
    e.mem_a = (void *)m_a; e.mem_a_size = mo.mem_a;
    e.mem_b = (void *)m_b; e.mem_b_size = mo.mem_b;
    e.work = work; e.work_size = 0x400;
    r = full >= 0 ? sceAvcencCreateEncoderInternal(ENC_TYPE, &e, fs) : sceAvcencCreateEncoder(ENC_TYPE, &e, &in);
    LOG("enctest: sceAvcencCreateEncoder: 0x%08X (handle %08X)", (unsigned)r, (unsigned)e.handle);
    if (r < 0) goto out;
    created = 1;
    EncParam ep; memset(&ep, 0, sizeof ep); ep.r[3] = 0x1C;
    r = sceAvcencSetEncoderParameter(&e, &ep);
    LOG("enctest: sceAvcencSetEncoderParameter: 0x%08X", (unsigned)r);

    uint32_t fsize = (uint32_t)(w * h * 3 / 2);
    if ((outb = alloc_block("vp_enctest_out", mo.stream_size ? mo.stream_size : 1u << 20, 0, &outp)) < 0) goto out;
    if ((frb = alloc_block("vp_enctest_yuv", fsize, 1, &frp)) < 0) goto out;

    /* the hardware colour conversion, RGBA -> YUV 4:2:0, at this size */
    if ((rgbb = alloc_block("vp_enctest_rgba", (uint32_t)(w * h * 4), 1, &rgbp)) >= 0) {
        uint32_t *px = rgbp;
        for (int i = 0; i < w * h; i++) px[i] = 0xFF000000u | (uint32_t)((i % w) * 255 / w) | (uint32_t)(((i / w) * 255 / h) << 8);
        EncPicture d, s; memset(&d, 0, sizeof d); memset(&s, 0, sizeof s);
        d.size = s.size = sizeof d;
        d.pixelformat = 0x20; d.pitch = (uint32_t)w; d.width = (uint32_t)w; d.height = (uint32_t)h; d.buffer = frp; d.buffer2 = (uint8_t *)frp + w * h;
        s.pixelformat = 0; s.pitch = (uint32_t)w; s.width = (uint32_t)w; s.height = (uint32_t)h; s.buffer = rgbp; s.opt[1] = 1;
        r = sceAvcencCsc(&d, &s);
        LOG("enctest: sceAvcencCsc RGBA -> YUV %dx%d: 0x%08X (Y at the middle: %u)", w, h, (unsigned)r, ((uint8_t *)frp)[(h / 2) * w + w / 2]);
    }

    char path[160];
    snprintf(path, sizeof path, OUT_DIR "/enctest_%dx%d%s.h264", w, h, full == 0 ? "_b0" : full > 0 ? "_b2" : "");
    mkdir_p(OUT_DIR);
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);

    memset(g_types, 0, sizeof g_types);
    EncStream st; EncStream *sp = &st;
    EncStreamArray arr;
    int n_out = 0, n_nal = 0, idr = 0; uint32_t bytes = 0;
    uint64_t t_enc = 0;
    for (int f = 0; f <= NFRAMES; f++) {
        memset(&st, 0, sizeof st); st.size = sizeof st; st.buf = outp; st.buf_size = mo.stream_size ? mo.stream_size : 1u << 20;
        arr.count = 0; arr.max = 1; arr.streams = &sp;
        if (f < NFRAMES) {
            fill_frame(frp, w, h, f);
            EncPicture p; memset(&p, 0, sizeof p);
            p.size = sizeof p; p.pixelformat = 0x20; p.pitch = (uint32_t)w; p.width = (uint32_t)w; p.height = (uint32_t)h;
            p.buffer = frp; p.flags = 1;
            uint64_t t1 = time_us();
            r = sceAvcencEncode(&e, &p, &arr);
            t_enc += time_us() - t1;
            if (r < 0 || f < 2) LOG("enctest: frame %d: sceAvcencEncode 0x%08X, %u out", f, (unsigned)r, arr.count);
        } else {
            r = sceAvcencEncodeStop(&e, &arr);
            LOG("enctest: sceAvcencEncodeStop: 0x%08X, %u out", (unsigned)r, arr.count);
        }
        if (r < 0) break;
        if (arr.count) {
            if (n_out < 2)
                LOG("enctest:   stream: %u bytes, %u NALs, picture type %u, out %08X %08X %08X %08X %08X %08X", st.buf_size, st.n_nal, st.pic_type,
                    (unsigned)st.out[0], (unsigned)st.out[1], (unsigned)st.out[2], (unsigned)st.out[3], (unsigned)st.out[4], (unsigned)st.out[5]);
            if (st.pic_type == 4) idr++;
            put_nals(&e, &st, fd, &n_nal, &bytes, n_out < 2);
            n_out++;
        }
    }
    double secs = t_enc / 1e6;
    if (fd >= 0) sceIoClose(fd);
    LOG("enctest: %dx%d: %d of %d frames came out (%d NALs, %u KB, %d of type 4) , encoding took %.2f s = %.1f fps; saved %s",
        w, h, n_out, NFRAMES, n_nal, bytes >> 10, idr, secs, secs > 0 ? n_out / secs : 0.0, path);
    char tl[160]; int tn = 0; tl[0] = 0;
    for (int t = 0; t < 32; t++) if (g_types[t]) tn += snprintf(tl + tn, sizeof tl - (size_t)tn, " %d:%d", t, g_types[t]);
    LOG("enctest: NAL types (type:count):%s", tl);
    ok = n_out > 0;

out:
    if (created) LOG("enctest: sceAvcencDeleteEncoder: 0x%08X", (unsigned)sceAvcencDeleteEncoder(&e));
    if (lib) LOG("enctest: sceVideoencTermLibrary: 0x%08X", (unsigned)sceVideoencTermLibrary(ENC_TYPE));
    if (ub >= 0) {
        if (m_b) sceCodecEngineFreeMemoryFromUnmapMemBlock(ub, m_b);
        if (m_a) sceCodecEngineFreeMemoryFromUnmapMemBlock(ub, m_a);
        if (m_lib) sceCodecEngineFreeMemoryFromUnmapMemBlock(ub, m_lib);
        sceCodecEngineCloseUnmapMemBlock(ub);
    }
    free(work);
    if (rgbb >= 0) sceKernelFreeMemBlock(rgbb);
    if (frb >= 0) sceKernelFreeMemBlock(frb);
    if (outb >= 0) sceKernelFreeMemBlock(outb);
    if (blk >= 0) sceKernelFreeMemBlock(blk);
    return ok;
}

int encprobe_run(char *msg, int msg_size)
{
    LOG("enctest: hardware H.264 encoder test");
    sceSysmoduleLoadModule(SCE_SYSMODULE_AVCDEC);
    static const struct { int w, h, full; } RUNS[] = { { 960, 544, 2 }, { 960, 544, 0 }, { 640, 368, 2 } };
    char res[3][32];
    for (int i = 0; i < 3; i++) {
        int ok = probe(RUNS[i].w, RUNS[i].h, RUNS[i].full);
        snprintf(res[i], sizeof res[i], "%dx%d/%d %s", RUNS[i].w, RUNS[i].h, RUNS[i].full, ok ? "ok" : "failed");
    }
    snprintf(msg, (size_t)msg_size, "Encoder test: %s, %s, %s - details in log.txt", res[0], res[1], res[2]);
    LOG("%s", msg);
    return 0;
}
