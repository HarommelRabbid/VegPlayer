/* mp4mux.c - see mp4mux.h */
#include "mp4mux.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>

#define WBUF (512u << 10)

typedef struct {
    uint32_t *size; uint32_t n, cap;           /* sample sizes */
    uint32_t *chunk_off; uint32_t *chunk_n;    /* chunks: file offset and sample count */
    uint32_t nchunks, ccap, ncap;
    uint32_t *sync; uint32_t nsync, scap;      /* keyframes (1-based sample numbers) */
} Track;

struct Mp4Mux {
    SceUID fd;
    char path[512];
    uint8_t *buf; uint32_t blen;
    uint64_t off;                              /* file offset of the next byte */
    uint64_t mdat_at;
    int last;                                  /* track of the last sample (-1 none) */
    int err;
    int w, h, channels;
    uint32_t vts, vdur, arate;
    Track t[2];                                /* 0 video, 1 audio */
    uint8_t *sps, *pps; uint32_t sps_len, pps_len;
    uint32_t priming; uint64_t asamples;
};

static int grow(void **p, uint32_t *cap, uint32_t need, size_t el)
{
    if (need <= *cap) return 0;
    uint32_t c = *cap ? *cap * 2 : 1024;
    while (c < need) c *= 2;
    void *q = realloc(*p, c * el);
    if (!q) return -1;
    *p = q; *cap = c;
    return 0;
}

static int flush_buf(Mp4Mux *m)
{
    if (!m->blen || m->err) return m->err;
    int r = sceIoWrite(m->fd, m->buf, m->blen);
    if (r != (int)m->blen) { LOG("mp4mux: write failed (0x%08X)", (unsigned)r); m->err = -1; }
    m->blen = 0;
    return m->err;
}

static int put(Mp4Mux *m, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    while (len && !m->err) {
        uint32_t k = WBUF - m->blen;
        if (k > len) k = len;
        memcpy(m->buf + m->blen, p, k);
        m->blen += k; p += k; len -= k; m->off += k;
        if (m->blen == WBUF) flush_buf(m);
    }
    return m->err;
}

static void be32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v; }

Mp4Mux *mux_open(const char *path, int width, int height, uint32_t vtimescale, uint32_t vdur, uint32_t arate, int channels)
{
    Mp4Mux *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->buf = malloc(WBUF);
    m->fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (!m->buf || m->fd < 0) { LOG("mp4mux: can't create %s (0x%08X)", path, (unsigned)m->fd); if (m->fd >= 0) sceIoClose(m->fd); free(m->buf); free(m); return NULL; }
    snprintf(m->path, sizeof m->path, "%s", path);
    m->w = width; m->h = height; m->vts = vtimescale; m->vdur = vdur; m->arate = arate; m->channels = channels;
    m->last = -1;
    static const uint8_t FTYP[] = { 0, 0, 0, 0x1C, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 2, 0,
                                    'i', 's', 'o', 'm', 'i', 's', 'o', '2', 'm', 'p', '4', '1' };
    put(m, FTYP, sizeof FTYP);
    m->mdat_at = m->off;
    static const uint8_t MDAT[] = { 0, 0, 0, 0, 'm', 'd', 'a', 't' };   /* size patched at the end */
    put(m, MDAT, sizeof MDAT);
    return m;
}

void mux_set_avc_config(Mp4Mux *m, const uint8_t *sps, uint32_t sps_len, const uint8_t *pps, uint32_t pps_len)
{
    if (m->sps) return;
    m->sps = malloc(sps_len); m->pps = malloc(pps_len);
    if (!m->sps || !m->pps) return;
    memcpy(m->sps, sps, sps_len); m->sps_len = sps_len;
    memcpy(m->pps, pps, pps_len); m->pps_len = pps_len;
}

static int add_sample(Mp4Mux *m, int ti, const uint8_t *data, uint32_t len, int sync)
{
    Track *t = &m->t[ti];
    if (m->err) return m->err;
    if (m->off + len > 0xFFFFFFF0ull) { LOG("mp4mux: the file would pass 4 GB"); return m->err = -1; }
    if (grow((void **)&t->size, &t->cap, t->n + 1, 4)) return m->err = -1;
    if (m->last != ti || !t->nchunks) {
        if (grow((void **)&t->chunk_off, &t->ccap, t->nchunks + 1, 4) || grow((void **)&t->chunk_n, &t->ncap, t->nchunks + 1, 4)) return m->err = -1;
        t->chunk_off[t->nchunks] = (uint32_t)m->off;
        t->chunk_n[t->nchunks] = 0;
        t->nchunks++;
    }
    t->chunk_n[t->nchunks - 1]++;
    t->size[t->n++] = len;
    if (sync) {
        if (grow((void **)&t->sync, &t->scap, t->nsync + 1, 4)) return m->err = -1;
        t->sync[t->nsync++] = t->n;
    }
    m->last = ti;
    return put(m, data, len);
}

void mux_set_audio_edit(Mp4Mux *m, uint32_t priming, uint64_t samples) { m->priming = priming; m->asamples = samples; }

int mux_video(Mp4Mux *m, const uint8_t *data, uint32_t len, int sync) { return add_sample(m, 0, data, len, sync); }
int mux_audio(Mp4Mux *m, const uint8_t *data, uint32_t len) { return add_sample(m, 1, data, len, 0); }
uint64_t mux_size(const Mp4Mux *m) { return m->off; }

/* ------------------------------------------------------------------ the index */

typedef struct { uint8_t *p; uint32_t n, cap; int err; uint32_t stack[16]; int depth; } Box;

static void b_raw(Box *b, const void *d, uint32_t len)
{
    if (b->err) return;
    if (grow((void **)&b->p, &b->cap, b->n + len, 1)) { b->err = 1; return; }
    memcpy(b->p + b->n, d, len); b->n += len;
}
static void b_u8(Box *b, uint32_t v) { uint8_t x = (uint8_t)v; b_raw(b, &x, 1); }
static void b_u16(Box *b, uint32_t v) { uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v }; b_raw(b, x, 2); }
static void b_u32(Box *b, uint32_t v) { uint8_t x[4]; be32(x, v); b_raw(b, x, 4); }
static void b_zero(Box *b, uint32_t n) { while (n--) b_u8(b, 0); }
static void b_open(Box *b, const char *type) { b->stack[b->depth++] = b->n; b_u32(b, 0); b_raw(b, type, 4); }
static void b_full(Box *b, const char *type, uint32_t vf) { b_open(b, type); b_u32(b, vf); }
static void b_close(Box *b) { uint32_t at = b->stack[--b->depth]; if (!b->err) be32(b->p + at, b->n - at); }

static void b_matrix(Box *b)
{
    static const uint32_t M[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 };
    for (int i = 0; i < 9; i++) b_u32(b, M[i]);
}

/* duration in the movie timescale (1000) */
static uint32_t to_ms(uint64_t units, uint32_t ts) { return (uint32_t)((units * 1000 + ts / 2) / ts); }

static void b_stbl_common(Box *b, const Track *t, uint32_t dur)
{
    b_full(b, "stts", 0); b_u32(b, t->n ? 1 : 0); if (t->n) { b_u32(b, t->n); b_u32(b, dur); } b_close(b);
    /* stsc: runs of chunks with the same sample count */
    uint32_t runs = 0;
    for (uint32_t i = 0; i < t->nchunks; i++) if (!i || t->chunk_n[i] != t->chunk_n[i - 1]) runs++;
    b_full(b, "stsc", 0); b_u32(b, runs);
    for (uint32_t i = 0; i < t->nchunks; i++)
        if (!i || t->chunk_n[i] != t->chunk_n[i - 1]) { b_u32(b, i + 1); b_u32(b, t->chunk_n[i]); b_u32(b, 1); }
    b_close(b);
    b_full(b, "stsz", 0); b_u32(b, 0); b_u32(b, t->n); for (uint32_t i = 0; i < t->n; i++) b_u32(b, t->size[i]); b_close(b);
    b_full(b, "stco", 0); b_u32(b, t->nchunks); for (uint32_t i = 0; i < t->nchunks; i++) b_u32(b, t->chunk_off[i]); b_close(b);
}

static void b_dinf(Box *b)
{
    b_open(b, "dinf"); b_full(b, "dref", 0); b_u32(b, 1); b_full(b, "url ", 1); b_close(b); b_close(b); b_close(b);
}

static void b_hdlr(Box *b, const char *type, const char *name)
{
    b_full(b, "hdlr", 0); b_u32(b, 0); b_raw(b, type, 4); b_zero(b, 12); b_raw(b, name, (uint32_t)strlen(name) + 1); b_close(b);
}

static void b_tkhd(Box *b, int id, uint32_t dur_ms, int audio, int w, int h)
{
    b_full(b, "tkhd", 3); b_u32(b, 0); b_u32(b, 0); b_u32(b, (uint32_t)id); b_u32(b, 0); b_u32(b, dur_ms);
    b_zero(b, 8); b_u16(b, 0); b_u16(b, 0); b_u16(b, audio ? 0x100 : 0); b_u16(b, 0);
    b_matrix(b); b_u32(b, (uint32_t)w << 16); b_u32(b, (uint32_t)h << 16);
    b_close(b);
}

static void b_mdhd(Box *b, uint32_t ts, uint32_t dur)
{
    b_full(b, "mdhd", 0); b_u32(b, 0); b_u32(b, 0); b_u32(b, ts); b_u32(b, dur); b_u16(b, 0x55C4 /* und */); b_u16(b, 0); b_close(b);
}

/* MPEG-4 descriptor with a 4-byte length */
static void b_desc(Box *b, uint8_t tag, uint32_t len)
{
    b_u8(b, tag); b_u8(b, 0x80 | ((len >> 21) & 0x7F)); b_u8(b, 0x80 | ((len >> 14) & 0x7F)); b_u8(b, 0x80 | ((len >> 7) & 0x7F)); b_u8(b, len & 0x7F);
}

static int sr_index(uint32_t r)
{
    static const uint32_t R[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    for (int i = 0; i < 13; i++) if (R[i] == r) return i;
    return 3;
}

int mux_close(Mp4Mux *m, int discard)
{
    if (!m) return -1;
    int ok = 0;
    if (!discard && !m->err && m->sps && m->t[0].n) {
        flush_buf(m);
        uint64_t mdat_end = m->off;
        Track *v = &m->t[0], *a = &m->t[1];
        uint64_t vdur = (uint64_t)v->n * m->vdur, adur = (uint64_t)a->n * 1024;
        uint32_t vms = to_ms(vdur, m->vts), ams = to_ms(adur > m->priming ? adur - m->priming : adur, m->arate);
        Box b; memset(&b, 0, sizeof b);
        b_open(&b, "moov");
        b_full(&b, "mvhd", 0); b_u32(&b, 0); b_u32(&b, 0); b_u32(&b, 1000); b_u32(&b, vms > ams ? vms : ams);
        b_u32(&b, 0x10000); b_u16(&b, 0x100); b_zero(&b, 10); b_matrix(&b); b_zero(&b, 24); b_u32(&b, a->n ? 3 : 2); b_close(&b);

        /* video */
        b_open(&b, "trak");
        b_tkhd(&b, 1, vms, 0, m->w, m->h);
        b_open(&b, "mdia"); b_mdhd(&b, m->vts, (uint32_t)vdur); b_hdlr(&b, "vide", "VideoHandler");
        b_open(&b, "minf");
        b_full(&b, "vmhd", 1); b_zero(&b, 8); b_close(&b);
        b_dinf(&b);
        b_open(&b, "stbl");
        b_full(&b, "stsd", 0); b_u32(&b, 1);
        b_open(&b, "avc1"); b_zero(&b, 6); b_u16(&b, 1); b_zero(&b, 16); b_u16(&b, (uint32_t)m->w); b_u16(&b, (uint32_t)m->h);
        b_u32(&b, 0x480000); b_u32(&b, 0x480000); b_u32(&b, 0); b_u16(&b, 1); b_zero(&b, 32); b_u16(&b, 0x18); b_u16(&b, 0xFFFF);
        b_open(&b, "avcC"); b_u8(&b, 1); b_u8(&b, m->sps[1]); b_u8(&b, m->sps[2]); b_u8(&b, m->sps[3]); b_u8(&b, 0xFF);
        b_u8(&b, 0xE1); b_u16(&b, m->sps_len); b_raw(&b, m->sps, m->sps_len);
        b_u8(&b, 1); b_u16(&b, m->pps_len); b_raw(&b, m->pps, m->pps_len); b_close(&b);
        b_open(&b, "colr"); b_raw(&b, "nclx", 4); b_u16(&b, 6); b_u16(&b, 6); b_u16(&b, 6); b_u8(&b, 0); b_close(&b);   /* BT.601, limited range */
        b_close(&b);   /* avc1 */
        b_close(&b);   /* stsd */
        b_stbl_common(&b, v, m->vdur);
        if (v->nsync && v->nsync < v->n) { b_full(&b, "stss", 0); b_u32(&b, v->nsync); for (uint32_t i = 0; i < v->nsync; i++) b_u32(&b, v->sync[i]); b_close(&b); }
        b_close(&b); b_close(&b); b_close(&b); b_close(&b);   /* stbl minf mdia trak */

        if (a->n) {
            uint64_t avalid = adur > m->priming ? adur - m->priming : 0;
            if (m->asamples && m->asamples < avalid) avalid = m->asamples;
            uint32_t ems = m->priming ? to_ms(avalid, m->arate) : ams;
            b_open(&b, "trak");
            b_tkhd(&b, 2, ems, 1, 0, 0);
            if (m->priming) {
                b_open(&b, "edts"); b_full(&b, "elst", 0); b_u32(&b, 1);
                b_u32(&b, ems); b_u32(&b, m->priming); b_u32(&b, 0x10000);
                b_close(&b); b_close(&b);
            }
            b_open(&b, "mdia"); b_mdhd(&b, m->arate, (uint32_t)adur); b_hdlr(&b, "soun", "SoundHandler");
            b_open(&b, "minf");
            b_full(&b, "smhd", 0); b_u32(&b, 0); b_close(&b);
            b_dinf(&b);
            b_open(&b, "stbl");
            b_full(&b, "stsd", 0); b_u32(&b, 1);
            b_open(&b, "mp4a"); b_zero(&b, 6); b_u16(&b, 1); b_zero(&b, 8); b_u16(&b, (uint32_t)m->channels); b_u16(&b, 16);
            b_u32(&b, 0); b_u32(&b, m->arate << 16);
            uint32_t maxs = 0; uint64_t tot = 0;
            for (uint32_t i = 0; i < a->n; i++) { if (a->size[i] > maxs) maxs = a->size[i]; tot += a->size[i]; }
            uint32_t avg = adur ? (uint32_t)(tot * 8 * m->arate / adur) : 0;
            b_full(&b, "esds", 0);
            b_desc(&b, 0x03, 3 + 5 + 13 + 5 + 2 + 5 + 1); b_u16(&b, 2); b_u8(&b, 0);
            b_desc(&b, 0x04, 13 + 5 + 2); b_u8(&b, 0x40); b_u8(&b, 0x15); b_u8(&b, 0); b_u16(&b, 0); b_u32(&b, maxs * 8); b_u32(&b, avg);
            int si = sr_index(m->arate);
            b_desc(&b, 0x05, 2); b_u8(&b, (2u << 3) | (uint32_t)(si >> 1)); b_u8(&b, (uint32_t)((si & 1) << 7) | ((uint32_t)m->channels << 3));
            b_desc(&b, 0x06, 1); b_u8(&b, 2);
            b_close(&b);   /* esds */
            b_close(&b);   /* mp4a */
            b_close(&b);   /* stsd */
            b_stbl_common(&b, a, 1024);
            b_close(&b); b_close(&b); b_close(&b); b_close(&b);
        }
        b_close(&b);   /* moov */
        if (!b.err) {
            put(m, b.p, b.n);
            flush_buf(m);
            uint8_t sz[4]; be32(sz, (uint32_t)(mdat_end - m->mdat_at));
            if (!m->err && sceIoLseek(m->fd, (SceOff)m->mdat_at, SCE_SEEK_SET) >= 0 && sceIoWrite(m->fd, sz, 4) == 4) ok = 1;
        } else LOG("mp4mux: out of memory for the index");
        free(b.p);
    } else if (!discard) LOG("mp4mux: nothing to save (error %d, parameter sets %s, %u frames)", m->err, m->sps ? "yes" : "no", m->t[0].n);
    sceIoClose(m->fd);
    if (!ok) sceIoRemove(m->path);
    for (int i = 0; i < 2; i++) { free(m->t[i].size); free(m->t[i].chunk_off); free(m->t[i].chunk_n); free(m->t[i].sync); }
    free(m->sps); free(m->pps); free(m->buf); free(m);
    return ok ? 0 : -1;
}
