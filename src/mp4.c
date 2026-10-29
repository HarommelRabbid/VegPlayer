/* mp4.c - minimal ISO-BMFF demuxer (see mp4.h). Handles 32/64-bit boxes, stts/ctts/stss/stsc/stsz/stz2/stco/co64,
 * avcC, esds (also inside QuickTime 'wave'), and the first edit list entry. Fragmented MP4 is not supported. */
#include "mp4.h"

#include <stdlib.h>
#include <string.h>

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

static int fseek64(FILE *f, uint64_t off)
{
#if defined(__vita__) || defined(_WIN32)
    return fseek(f, (long)off, SEEK_SET);   /* newlib on Vita: long is 32-bit; files > 2 GB aren't expected here */
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}

/* Read a whole box payload into memory (for the small table boxes). */
static uint8_t *read_payload(Mp4File *m, uint64_t off, uint64_t len)
{
    if (len > 64u * 1024 * 1024) return NULL;
    uint8_t *b = malloc(len ? (size_t)len : 1);
    if (!b) return NULL;
    if (fseek64(m->f, off) || fread(b, 1, (size_t)len, m->f) != len) { free(b); return NULL; }
    return b;
}

typedef struct {
    uint32_t handler;
    uint32_t timescale; int64_t duration;
    int64_t elst_media_time; int64_t elst_empty; int has_elst;
    uint32_t movie_ts;          /* mvhd timescale: an empty edit's duration is in it */
    uint8_t *stts, *ctts, *stss, *stsc, *stsz, *stz2, *stco, *co64, *stsd;
    uint64_t stts_n, ctts_n, stss_n, stsc_n, stsz_n, stz2_n, stco_n, co64_n, stsd_n;
} TrakTmp;

static void trak_free(TrakTmp *t)
{
    free(t->stts); free(t->ctts); free(t->stss); free(t->stsc); free(t->stsz); free(t->stz2); free(t->stco); free(t->co64); free(t->stsd);
}

/* ------------------------------------------------------------------ codec config */

typedef struct { const uint8_t *p; int len, pos; } Bits;
static int getbit(Bits *b) { if (b->pos >= b->len * 8) return 0; int v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1; b->pos++; return v; }
static uint32_t getbits(Bits *b, int n) { uint32_t v = 0; while (n--) v = v << 1 | (uint32_t)getbit(b); return v; }
static uint32_t ue(Bits *b) { int z = 0; while (!getbit(b) && z < 32) z++; return z ? ((1u << z) - 1 + getbits(b, z)) : 0; }
static int32_t se(Bits *b) { uint32_t k = ue(b); return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2); }

int h264_sps_max_refs(const uint8_t *nal, int len)
{
    /* strip emulation prevention bytes */
    uint8_t rb[256];
    int n = 0;
    for (int i = 1; i < len && n < (int)sizeof rb; i++) {
        if (i + 2 < len && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) { rb[n++] = 0; if (n < (int)sizeof rb) rb[n++] = 0; i += 2; continue; }
        rb[n++] = nal[i];
    }
    Bits b = { rb, n, 0 };
    int profile = (int)getbits(&b, 8);
    getbits(&b, 16);    /* constraint flags + level */
    ue(&b);             /* sps id */
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        uint32_t cf = ue(&b);
        if (cf == 3) getbit(&b);
        ue(&b); ue(&b);  /* bit depths */
        getbit(&b);      /* qpprime */
        if (getbit(&b)) {    /* scaling matrices */
            for (int i = 0; i < (cf != 3 ? 8 : 12); i++) {
                if (!getbit(&b)) continue;
                int size = i < 6 ? 16 : 64, last = 8, next = 8;
                for (int j = 0; j < size; j++) {
                    if (next) { int32_t d = se(&b); next = (last + d + 256) % 256; }
                    last = next ? next : last;
                }
            }
        }
    }
    ue(&b);   /* log2_max_frame_num */
    uint32_t poc = ue(&b);
    if (poc == 0) ue(&b);
    else if (poc == 1) {
        getbit(&b); se(&b); se(&b);
        uint32_t k = ue(&b);
        for (uint32_t i = 0; i < k && i < 256; i++) se(&b);
    }
    uint32_t refs = ue(&b);
    return refs <= 16 ? (int)refs : 0;
}

static void parse_avcc(Mp4Track *t, const uint8_t *p, uint32_t len)
{
    if (len < 7) return;
    t->nal_len = (p[4] & 3) + 1;
    int nsps = p[5] & 31;
    uint32_t q = 6;
    for (int i = 0; i < nsps && q + 2 <= len; i++) {
        uint16_t l = be16(p + q); q += 2;
        if (q + l > len) return;
        if (t->nsps < 4) {
            t->sps[t->nsps] = malloc(l);
            if (t->sps[t->nsps]) { memcpy(t->sps[t->nsps], p + q, l); t->sps_len[t->nsps] = l; t->nsps++; }
        }
        q += l;
    }
    if (q >= len) return;
    int npps = p[q++];
    for (int i = 0; i < npps && q + 2 <= len; i++) {
        uint16_t l = be16(p + q); q += 2;
        if (q + l > len) return;
        if (t->npps < 4) {
            t->pps[t->npps] = malloc(l);
            if (t->pps[t->npps]) { memcpy(t->pps[t->npps], p + q, l); t->pps_len[t->npps] = l; t->npps++; }
        }
        q += l;
    }
    if (t->nsps) t->max_ref_frames = h264_sps_max_refs(t->sps[0], t->sps_len[0]);
}

static uint32_t desc_len(const uint8_t *p, uint32_t len, uint32_t *q)
{
    uint32_t v = 0;
    for (int i = 0; i < 4 && *q < len; i++) {
        uint8_t c = p[(*q)++];
        v = v << 7 | (c & 0x7F);
        if (!(c & 0x80)) break;
    }
    return v;
}

static const int AAC_RATES[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

static void parse_asc(Mp4Track *t, const uint8_t *p, int len)
{
    if (len < 2) return;
    memcpy(t->asc, p, (size_t)(len > 16 ? 16 : len));
    t->asc_len = len > 16 ? 16 : len;
    Bits b = { p, len, 0 };
    int aot = (int)getbits(&b, 5);
    if (aot == 31) aot = 32 + (int)getbits(&b, 6);
    int sfi = (int)getbits(&b, 4);
    int rate = sfi == 15 ? (int)getbits(&b, 24) : (sfi < 13 ? AAC_RATES[sfi] : 0);
    int ch = (int)getbits(&b, 4);
    t->aot = aot;
    if (aot == 5 || aot == 29) {
        t->sbr = 1;
        int esfi = (int)getbits(&b, 4);
        int erate = esfi == 15 ? (int)getbits(&b, 24) : (esfi < 13 ? AAC_RATES[esfi] : 0);
        aot = (int)getbits(&b, 5);
        t->aot = aot;
        if (erate) rate = erate;
    }
    if (rate) t->sample_rate = rate;
    if (ch >= 1 && ch <= 7) t->channels = ch == 7 ? 8 : ch;
}

static void parse_esds(Mp4Track *t, const uint8_t *p, uint32_t len)
{
    uint32_t q = 4;   /* version/flags */
    while (q + 2 <= len) {
        uint8_t tag = p[q++];
        uint32_t l = desc_len(p, len, &q);
        if (tag == 0x03) {           /* ES_Descriptor */
            if (q + 3 > len) return;
            uint8_t fl = p[q + 2];
            q += 3;
            if (fl & 0x80) q += 2;
            if (fl & 0x40) { if (q < len) q += 1u + p[q]; }
            if (fl & 0x20) q += 2;
            continue;                /* descend into its children */
        } else if (tag == 0x04) {    /* DecoderConfigDescriptor */
            if (q < len) t->oti = p[q];
            q += 13;
            continue;
        } else if (tag == 0x05) {    /* DecoderSpecificInfo: AudioSpecificConfig, or MPEG-4 Visual VOS/VOL headers */
            if (q + l > len) return;
            if (t->codec == FOURCC('m', 'p', '4', 'v')) {
                free(t->dsi);
                t->dsi = malloc(l ? l : 1);
                if (t->dsi) { memcpy(t->dsi, p + q, l); t->dsi_len = (int)l; }
            } else parse_asc(t, p + q, (int)l);
            return;
        }
        q += l;
    }
}

/* scan child boxes of a sample entry for avcC / esds (esds may sit inside a 'wave' box) */
static void scan_entry_children(Mp4Track *t, const uint8_t *p, uint32_t len)
{
    uint32_t q = 0;
    while (q + 8 <= len) {
        uint32_t sz = be32(p + q), ty = be32(p + q + 4);
        if (sz < 8 || q + sz > len) break;
        if (ty == FOURCC('a', 'v', 'c', 'C')) parse_avcc(t, p + q + 8, sz - 8);
        else if (ty == FOURCC('e', 's', 'd', 's')) parse_esds(t, p + q + 8, sz - 8);
        else if (ty == FOURCC('w', 'a', 'v', 'e')) scan_entry_children(t, p + q + 8, sz - 8);
        else if (ty == FOURCC('p', 'c', 'm', 'C') && sz >= 14) {   /* ISO 23003-5: format flags (bit 0 little-endian), bits */
            t->pcm_le = p[q + 12] & 1;
            t->pcm_bits = p[q + 13];
        } else if (ty == FOURCC('e', 'n', 'd', 'a') && sz >= 10) t->pcm_le = be16(p + q + 8) != 0;   /* QuickTime */
        q += sz;
    }
}

static int parse_stsd(Mp4Track *t, const uint8_t *p, uint64_t len, int video)
{
    if (len < 16) return -1;
    uint32_t esz = be32(p + 8), fmt = be32(p + 12);
    if (esz < 16 || 8 + (uint64_t)esz > len) return -1;
    const uint8_t *e = p + 16;        /* after the entry's size + type */
    uint32_t el = esz - 8;
    t->codec = fmt;
    if (video) {
        int avc = fmt == FOURCC('a', 'v', 'c', '1') || fmt == FOURCC('a', 'v', 'c', '3');
        if (!avc && fmt != FOURCC('m', 'p', '4', 'v')) return -2;
        if (el < 78) return -1;
        t->width = be16(e + 24); t->height = be16(e + 26);
        scan_entry_children(t, e + 78, el - 78);
        if (avc) { if (!t->nal_len || !t->nsps || !t->npps) return -3; }
        else if (t->oti && t->oti != 0x20) return -2;   /* only MPEG-4 Visual (not MPEG-1/2 in an mp4v entry) */
    } else {
        int pcm = 1;
        switch (fmt) {
        case FOURCC('m', 'p', '4', 'a'): pcm = 0; break;
        case FOURCC('i', 'p', 'c', 'm'): case FOURCC('f', 'p', 'c', 'm'): case FOURCC('s', 'o', 'w', 't'): case FOURCC('t', 'w', 'o', 's'): case FOURCC('l', 'p', 'c', 'm'):
        case FOURCC('i', 'n', '2', '4'): case FOURCC('i', 'n', '3', '2'): case FOURCC('f', 'l', '3', '2'): case FOURCC('f', 'l', '6', '4'):
        case FOURCC('r', 'a', 'w', ' '): break;
        default: return -2;
        }
        if (el < 28) return -1;
        uint16_t ver = be16(e + 8);
        t->channels = be16(e + 16);
        t->sample_rate = (int)(be32(e + 24) >> 16);
        if (pcm) {
            t->pcm_bits = be16(e + 18);
            t->pcm_le = fmt == FOURCC('s', 'o', 'w', 't');
            if (fmt == FOURCC('i', 'n', '2', '4')) t->pcm_bits = 24;
            if (fmt == FOURCC('i', 'n', '3', '2') || fmt == FOURCC('f', 'l', '3', '2')) t->pcm_bits = 32;
            if (fmt == FOURCC('f', 'l', '6', '4')) t->pcm_bits = 64;
            if (fmt == FOURCC('r', 'a', 'w', ' ')) { t->pcm_bits = 8; t->pcm_unsigned = 1; }
            t->pcm_float = fmt == FOURCC('f', 'l', '3', '2') || fmt == FOURCC('f', 'l', '6', '4') || fmt == FOURCC('f', 'p', 'c', 'm');
            if (ver == 2 && el >= 64) {   /* QuickTime sound description v2 (lpcm) */
                uint64_t rb = be64(e + 32);
                double r;
                memcpy(&r, &rb, 8);
                t->sample_rate = (int)(r + 0.5);
                t->channels = (int)be32(e + 40);
                t->pcm_bits = (int)be32(e + 48);
                uint32_t fl = be32(e + 52);
                t->pcm_float = fl & 1;
                t->pcm_le = !(fl & 2);
                t->pcm_unsigned = !t->pcm_float && !(fl & 4) && t->pcm_bits == 8;
            }
        }
        uint32_t skip = 28 + (ver == 1 ? 16 : ver == 2 ? 36 : 0);
        if (skip < el) scan_entry_children(t, e + skip, el - skip);   /* esds, or pcmC / enda */
        if (!t->sample_rate || !t->channels) return -1;
        if (pcm) {
            if (t->pcm_bits != 8 && t->pcm_bits != 16 && t->pcm_bits != 24 && t->pcm_bits != 32 && t->pcm_bits != 64) return -2;
            if ((t->pcm_bits == 64 && !t->pcm_float) || (t->pcm_float && t->pcm_bits != 32 && t->pcm_bits != 64)) return -2;
            if (t->pcm_bits == 8 && fmt != FOURCC('t', 'w', 'o', 's') && fmt != FOURCC('s', 'o', 'w', 't') && fmt != FOURCC('i', 'p', 'c', 'm') && fmt != FOURCC('f', 'p', 'c', 'm'))
                t->pcm_unsigned = 1;
            t->pcm_bpf = t->channels * t->pcm_bits / 8;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ sample table */

/* edit list -> shift (in track timescale units `ts`) to add to every time: a leading empty edit delays the track
 * (its duration is in the movie timescale), a media start time > 0 skips that much of the media */
static int64_t edit_shift(const TrakTmp *k, double ts)
{
    double sh = 0, kts = k->timescale ? k->timescale : 1;
    if (k->elst_empty > 0 && k->movie_ts) sh += (double)k->elst_empty * ts / k->movie_ts;
    if (k->has_elst && k->elst_media_time > 0) sh -= (double)k->elst_media_time * ts / kts;
    return (int64_t)(sh >= 0 ? sh + 0.5 : sh - 0.5);
}

/* PCM: one entry per chunk (a chunk is a contiguous run of frames), so hours of audio don't need a table entry
 * per frame. QuickTime stores a "sample size" of 1 for PCM: the real frame size is the format's. */
static int build_pcm_chunks(Mp4Track *t, TrakTmp *k)
{
    uint32_t fixed = 0, nsz = 0;
    if (k->stsz && k->stsz_n >= 12) { fixed = be32(k->stsz + 4); nsz = be32(k->stsz + 8); }
    else return -1;
    uint64_t nchunks = 0;
    if (k->stco && k->stco_n >= 8) nchunks = be32(k->stco + 4);
    else if (k->co64 && k->co64_n >= 8) nchunks = be32(k->co64 + 4);
    else return -1;
    uint32_t nsc = k->stsc && k->stsc_n >= 8 ? be32(k->stsc + 4) : 0;
    if (!nsc || 8 + 12ull * nsc > k->stsc_n || !nchunks || nchunks > 10000000) return -1;
    t->s = calloc((size_t)nchunks, sizeof(Mp4Sample));
    if (!t->s) return -1;
    uint32_t c_out = 0, si = 0;
    int64_t frames = 0;
    for (uint32_t e = 0; e < nsc; e++) {
        uint32_t first = be32(k->stsc + 8 + 12 * e), per = be32(k->stsc + 12 + 12 * e);
        uint32_t last = e + 1 < nsc ? be32(k->stsc + 8 + 12 * (e + 1)) : (uint32_t)nchunks + 1;
        for (uint32_t c = first; c < last; c++) {
            if (c == 0 || c > nchunks) return -1;
            uint64_t off = k->stco ? be32(k->stco + 8 + 4 * (c - 1)) : be64(k->co64 + 8 + 8 * (c - 1));
            uint64_t bytes = 0;
            if (fixed) bytes = (uint64_t)per * (fixed < (uint32_t)t->pcm_bpf ? (uint32_t)t->pcm_bpf : fixed);
            else for (uint32_t j = 0; j < per && si + j < nsz && 12 + 4ull * (si + j + 1) <= k->stsz_n; j++) bytes += be32(k->stsz + 12 + 4 * (si + j));
            si += per;
            bytes -= bytes % (uint32_t)t->pcm_bpf;
            if (!bytes || bytes > 0xFFFFFFFFu) continue;
            Mp4Sample *sm = &t->s[c_out++];
            sm->offset = off; sm->size = (uint32_t)bytes; sm->dts = sm->cts = frames; sm->sync = 1;
            frames += (int64_t)(bytes / (uint32_t)t->pcm_bpf);
        }
    }
    t->n = c_out;
    if (!t->n) return -1;
    t->timescale = (uint32_t)t->sample_rate;   /* dts are in frames */
    t->duration = frames;
    int64_t sh = edit_shift(k, t->sample_rate);   /* negative: the decoder skips the first -sh frames */
    for (uint32_t i = 0; i < t->n; i++) { t->s[i].dts += sh; t->s[i].cts += sh; }
    t->duration += sh;
    return 0;
}

static int build_samples(Mp4Track *t, TrakTmp *k)
{
    if (t->pcm_bpf) return build_pcm_chunks(t, k);
    uint64_t n = 0;
    uint32_t fixed = 0;
    int fs = 0;
    if (k->stsz && k->stsz_n >= 12) { fixed = be32(k->stsz + 4); n = be32(k->stsz + 8); }
    else if (k->stz2 && k->stz2_n >= 12) { fs = k->stz2[7]; n = be32(k->stz2 + 8); }
    else return -1;
    if (n == 0 || n > 4000000) return -1;
    t->s = calloc((size_t)n, sizeof(Mp4Sample));
    if (!t->s) return -1;
    t->n = (uint32_t)n;
    /* sizes */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sz = fixed;
        if (k->stsz && !fixed) { if (12 + 4ull * (i + 1) > k->stsz_n) return -1; sz = be32(k->stsz + 12 + 4 * i); }
        else if (k->stz2) {
            if (fs == 4) { uint8_t v = k->stz2[12 + i / 2]; sz = (i & 1) ? (v & 15u) : (uint32_t)(v >> 4); }
            else if (fs == 8) sz = k->stz2[12 + i];
            else if (fs == 16) sz = be16(k->stz2 + 12 + 2 * i);
            else return -1;
        }
        t->s[i].size = sz;
    }
    /* offsets: stsc + stco/co64 */
    uint64_t nchunks = 0;
    if (k->stco && k->stco_n >= 8) nchunks = be32(k->stco + 4);
    else if (k->co64 && k->co64_n >= 8) nchunks = be32(k->co64 + 4);
    else return -1;
    uint32_t nsc = k->stsc && k->stsc_n >= 8 ? be32(k->stsc + 4) : 0;
    if (!nsc || 8 + 12ull * nsc > k->stsc_n) return -1;
    uint32_t si = 0;
    for (uint32_t e = 0; e < nsc && si < n; e++) {
        uint32_t first = be32(k->stsc + 8 + 12 * e), per = be32(k->stsc + 12 + 12 * e);
        uint32_t last = e + 1 < nsc ? be32(k->stsc + 8 + 12 * (e + 1)) : (uint32_t)nchunks + 1;
        for (uint32_t c = first; c < last && si < n; c++) {
            if (c == 0 || c > nchunks) return -1;
            uint64_t off = k->stco ? be32(k->stco + 8 + 4 * (c - 1)) : be64(k->co64 + 8 + 8 * (c - 1));
            for (uint32_t j = 0; j < per && si < n; j++) { t->s[si].offset = off; off += t->s[si].size; si++; }
        }
    }
    if (si < n) t->n = si;
    /* timing */
    uint32_t nst = k->stts && k->stts_n >= 8 ? be32(k->stts + 4) : 0;
    int64_t dts = 0;
    si = 0;
    for (uint32_t e = 0; e < nst && 8 + 8ull * (e + 1) <= k->stts_n; e++) {
        uint32_t cnt = be32(k->stts + 8 + 8 * e), d = be32(k->stts + 12 + 8 * e);
        for (uint32_t j = 0; j < cnt && si < t->n; j++) { t->s[si].dts = dts; t->s[si].cts = dts; dts += d; si++; }
    }
    for (; si < t->n; si++) { t->s[si].dts = dts; t->s[si].cts = dts; }
    if (k->ctts && k->ctts_n >= 8) {
        int ver = k->ctts[0];
        uint32_t nc = be32(k->ctts + 4);
        si = 0;
        for (uint32_t e = 0; e < nc && 8 + 8ull * (e + 1) <= k->ctts_n; e++) {
            uint32_t cnt = be32(k->ctts + 8 + 8 * e);
            uint32_t raw = be32(k->ctts + 12 + 8 * e);
            int64_t off = ver ? (int64_t)(int32_t)raw : (int64_t)raw;
            if (!ver && raw > 0x7fffffffu) off = (int64_t)(int32_t)raw;
            for (uint32_t j = 0; j < cnt && si < t->n; j++) { t->s[si].cts = t->s[si].dts + off; si++; }
        }
    }
    /* edit list: the first presented sample at 0 (media start skipped), after any leading empty edit */
    int64_t sh = edit_shift(k, k->timescale ? k->timescale : 1000);
    if (sh) for (uint32_t i = 0; i < t->n; i++) { t->s[i].cts += sh; t->s[i].dts += sh; }
    /* sync samples */
    if (k->stss && k->stss_n >= 8) {
        uint32_t ns = be32(k->stss + 4);
        for (uint32_t e = 0; e < ns && 8 + 4ull * (e + 1) <= k->stss_n; e++) {
            uint32_t idx = be32(k->stss + 8 + 4 * e);
            if (idx >= 1 && idx <= t->n) t->s[idx - 1].sync = 1;
        }
    } else for (uint32_t i = 0; i < t->n; i++) t->s[i].sync = 1;
    if (t->n) t->s[0].sync = 1;
    t->timescale = k->timescale ? k->timescale : 1000;
    t->duration = k->duration ? k->duration : dts;
    return 0;
}

/* ------------------------------------------------------------------ box walking */

typedef struct { uint64_t off, size, hdr; uint32_t type; } Box;

static int read_box(Mp4File *m, uint64_t off, uint64_t end, Box *b)
{
    uint8_t h[16];
    if (off + 8 > end || fseek64(m->f, off) || fread(h, 1, 8, m->f) != 8) return 0;
    uint64_t sz = be32(h);
    b->type = be32(h + 4);
    b->hdr = 8;
    if (sz == 1) {
        if (fread(h + 8, 1, 8, m->f) != 8) return 0;
        sz = be64(h + 8);
        b->hdr = 16;
    } else if (sz == 0) sz = end - off;
    if (sz < b->hdr || off + sz > end) return 0;
    b->off = off; b->size = sz;
    return 1;
}

static int walk_trak(Mp4File *m, uint64_t off, uint64_t end, TrakTmp *k)
{
    Box b;
    while (read_box(m, off, end, &b)) {
        uint64_t po = b.off + b.hdr, pl = b.size - b.hdr;
        switch (b.type) {
        case FOURCC('m', 'd', 'i', 'a'): case FOURCC('m', 'i', 'n', 'f'): case FOURCC('s', 't', 'b', 'l'): case FOURCC('e', 'd', 't', 's'):
            if (walk_trak(m, po, po + pl, k) < 0) return -1;
            break;
        case FOURCC('m', 'd', 'h', 'd'): {
            uint8_t *p = read_payload(m, po, pl < 32 ? pl : 32);
            if (!p) return -1;
            if (p[0] == 1 && pl >= 32) { k->timescale = be32(p + 20); k->duration = (int64_t)be64(p + 24); }
            else if (pl >= 20) { k->timescale = be32(p + 12); k->duration = be32(p + 16); }
            free(p);
            break;
        }
        case FOURCC('h', 'd', 'l', 'r'): {
            uint8_t *p = read_payload(m, po, pl < 12 ? pl : 12);
            if (!p) return -1;
            if (pl >= 12 && !k->handler) k->handler = be32(p + 8);   /* mdia's hdlr comes first; MOV adds a 'dhlr' one in minf */
            free(p);
            break;
        }
        case FOURCC('e', 'l', 's', 't'): {
            uint8_t *p = read_payload(m, po, pl);
            if (!p) return -1;
            uint32_t ne = pl >= 8 ? be32(p + 4) : 0;
            int v1 = p[0] == 1;
            uint32_t es = v1 ? 20 : 12;
            for (uint32_t e = 0; e < ne && 8 + (uint64_t)es * (e + 1) <= pl; e++) {
                const uint8_t *q = p + 8 + es * e;
                int64_t mt = v1 ? (int64_t)be64(q + 8) : (int64_t)(int32_t)be32(q + 4);
                if (mt == -1) { k->elst_empty += v1 ? (int64_t)be64(q) : be32(q); continue; }
                k->elst_media_time = mt; k->has_elst = 1;
                break;
            }
            free(p);
            break;
        }
#define TABLE(cc, field) case cc: free(k->field); k->field = read_payload(m, po, pl); if (!k->field) return -1; k->field##_n = pl; break;
        TABLE(FOURCC('s', 't', 't', 's'), stts)
        TABLE(FOURCC('c', 't', 't', 's'), ctts)
        TABLE(FOURCC('s', 't', 's', 's'), stss)
        TABLE(FOURCC('s', 't', 's', 'c'), stsc)
        TABLE(FOURCC('s', 't', 's', 'z'), stsz)
        TABLE(FOURCC('s', 't', 'z', '2'), stz2)
        TABLE(FOURCC('s', 't', 'c', 'o'), stco)
        TABLE(FOURCC('c', 'o', '6', '4'), co64)
        TABLE(FOURCC('s', 't', 's', 'd'), stsd)
#undef TABLE
        default: break;
        }
        off = b.off + b.size;
    }
    return 0;
}

static int walk_moov(Mp4File *m, uint64_t off, uint64_t end)
{
    Box b;
    while (read_box(m, off, end, &b)) {
        if (b.type == FOURCC('t', 'r', 'a', 'k')) {
            TrakTmp k;
            memset(&k, 0, sizeof k);
            k.movie_ts = m->movie_ts;
            if (walk_trak(m, b.off + b.hdr, b.off + b.size, &k) < 0) { trak_free(&k); snprintf(m->err, sizeof m->err, "out of memory"); return -1; }
            Mp4Track *t = NULL;
            int video = 0;
            if (k.handler == FOURCC('v', 'i', 'd', 'e') && !m->video.present) { t = &m->video; video = 1; }
            else if (k.handler == FOURCC('s', 'o', 'u', 'n') && !m->audio.present) t = &m->audio;
            if (t && k.stsd) {
                memset(t, 0, sizeof *t);
                int r = parse_stsd(t, k.stsd, k.stsd_n, video);
                if (r == 0 && build_samples(t, &k) == 0) t->present = 1;
                else {
                    if (r == -2) snprintf(m->err, sizeof m->err, "%s codec '%c%c%c%c' isn't supported", video ? "video" : "audio",
                                          (int)(t->codec >> 24), (int)(t->codec >> 16 & 255), (int)(t->codec >> 8 & 255), (int)(t->codec & 255));
                    free(t->s);
                    for (int i = 0; i < t->nsps; i++) free(t->sps[i]);
                    for (int i = 0; i < t->npps; i++) free(t->pps[i]);
                    free(t->dsi);
                    memset(t, 0, sizeof *t);
                }
            }
            trak_free(&k);
        } else if (b.type == FOURCC('m', 'v', 'h', 'd')) {
            uint8_t *p = read_payload(m, b.off + b.hdr, b.size - b.hdr < 24 ? b.size - b.hdr : 24);
            if (p && b.size - b.hdr >= 24) m->movie_ts = be32(p + (p[0] == 1 ? 20 : 12));
            free(p);
        } else if (b.type == FOURCC('m', 'v', 'e', 'x')) {
            snprintf(m->err, sizeof m->err, "fragmented MP4 isn't supported (re-mux it)");
        }
        off = b.off + b.size;
    }
    return 0;
}

int mp4_open(Mp4File *m, const char *path)
{
    memset(m, 0, sizeof *m);
    m->f = fopen(path, "rb");
    if (!m->f) { snprintf(m->err, sizeof m->err, "can't open file"); return -1; }
    fseek(m->f, 0, SEEK_END);
    m->fsize = (uint64_t)ftell(m->f);
    Box b;
    uint64_t off = 0;
    int found = 0;
    while (read_box(m, off, m->fsize, &b)) {
        if (b.type == FOURCC('m', 'o', 'o', 'v')) {
            found = 1;
            if (walk_moov(m, b.off + b.hdr, b.off + b.size) < 0) { mp4_close(m); return -1; }
        }
        off = b.off + b.size;
    }
    if (!found) { snprintf(m->err, sizeof m->err, "no 'moov' box: not an MP4/MOV file"); fclose(m->f); m->f = NULL; return -1; }
    if (!m->video.present && !m->audio.present) {
        if (!m->err[0]) snprintf(m->err, sizeof m->err, "no H.264 video or AAC audio track");
        mp4_close(m);
        return -1;
    }
    return 0;
}

void mp4_close(Mp4File *m)
{
    Mp4Track *ts[2] = { &m->video, &m->audio };
    for (int i = 0; i < 2; i++) {
        free(ts[i]->s);
        for (int k = 0; k < ts[i]->nsps; k++) free(ts[i]->sps[k]);
        for (int k = 0; k < ts[i]->npps; k++) free(ts[i]->pps[k]);
        free(ts[i]->dsi);
        memset(ts[i], 0, sizeof *ts[i]);
    }
    if (m->f) fclose(m->f);
    m->f = NULL;
}

int mp4_seek(Mp4File *m, uint64_t off) { return fseek64(m->f, off); }

int mp4_read_sample(Mp4File *m, const Mp4Track *t, uint32_t i, uint8_t *buf)
{
    if (i >= t->n) return -1;
    if (fseek64(m->f, t->s[i].offset)) return -1;
    return fread(buf, 1, t->s[i].size, m->f) == t->s[i].size ? 0 : -1;
}

int mp4_avcc_to_annexb(const Mp4Track *t, const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t cap, int with_params)
{
    uint32_t o = 0;
#define PUT_SC() do { if (o + 4 > cap) return -1; out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 1; } while (0)
    if (with_params) {
        for (int i = 0; i < t->nsps; i++) { PUT_SC(); if (o + t->sps_len[i] > cap) return -1; memcpy(out + o, t->sps[i], t->sps_len[i]); o += t->sps_len[i]; }
        for (int i = 0; i < t->npps; i++) { PUT_SC(); if (o + t->pps_len[i] > cap) return -1; memcpy(out + o, t->pps[i], t->pps_len[i]); o += t->pps_len[i]; }
    }
    uint32_t q = 0;
    int nl = t->nal_len;
    while (q + (uint32_t)nl <= inlen) {
        uint32_t l = 0;
        for (int k = 0; k < nl; k++) l = l << 8 | in[q + k];
        q += (uint32_t)nl;
        if (l == 0 || q + l > inlen) break;
        PUT_SC();
        if (o + l > cap) return -1;
        memcpy(out + o, in + q, l);
        o += l;
        q += l;
    }
#undef PUT_SC
    return (int)o;
}

void mp4_adts_header(const Mp4Track *t, uint32_t len, uint8_t h[7])
{
    int sfi = 4;
    int rate = t->sample_rate;
    if (t->sbr && rate > 24000) rate /= 2;       /* ADTS carries the core rate */
    for (int i = 0; i < 13; i++) if (AAC_RATES[i] == rate) sfi = i;
    int profile = (t->aot >= 1 && t->aot <= 4 ? t->aot : 2) - 1;
    int ch = t->channels > 0 && t->channels <= 7 ? t->channels : 2;
    uint32_t fl = len + 7;
    h[0] = 0xFF;
    h[1] = 0xF1;
    h[2] = (uint8_t)(profile << 6 | sfi << 2 | (ch >> 2 & 1));
    h[3] = (uint8_t)((ch & 3) << 6 | (fl >> 11 & 3));
    h[4] = (uint8_t)(fl >> 3 & 0xFF);
    h[5] = (uint8_t)((fl & 7) << 5 | 0x1F);
    h[6] = 0xFC;
}
