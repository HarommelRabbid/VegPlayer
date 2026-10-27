/* adec.c - audio sources into PCM segments.
 *   AT9            hardware (SceAtrac -> SceAudiodec AT9)
 *   MP3            hardware (SceAudiodec MP3); dr_mp3 only if the hardware refuses a stream (e.g. Layer I/II)
 *   AAC (MP4/M4A)  hardware (SceAudiodec AAC), raw access units, ADTS as a fallback
 *   WAV            PCM 8/16/24/32-bit, float
 *   OGG Vorbis     stb_vorbis
 *   AT3 / AT3plus  detected and refused: their decoder (SceAudiocodec) has no stubs in VitaSDK - see README
 * Everything is kept as 16-bit stereo at the source rate; only the time ranges the project uses are stored. */
#include "common.h"
#include "media.h"
#include "mp4.h"

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/atrac.h>
#include <psp2/audiodec.h>
#include <psp2/sysmodule.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "../third_party/dr_mp3.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb_vorbis.c"

enum { AF_WAV = 1, AF_AT9, AF_AT3, AF_MP3, AF_OGG, AF_AAC };

struct AudioSrc {
    int fmt;
    char path[512];
    int rate, ch;
    int64_t total;
    /* WAV */
    FILE *f; int64_t data_off, data_len; int bits, is_float, block;
    /* whole-file formats */
    uint8_t *buf; size_t len;
    /* MP3 */
    size_t mp3_start; int mp3_ver, mp3_layer; int64_t mp3_skip;
    /* MP4 */
    Mp4File mp4; int mp4_open;
};

/* ------------------------------------------------------------------ PCM sink */

typedef struct {
    AudioCache *ac;
    const int64_t (*r)[2]; int nr;
    int64_t pos, end;
    int cur;   /* first range not finished */
} Sink;

static int sink_init(Sink *s, AudioCache *ac, const int64_t (*r)[2], int nr, int64_t start_pos)
{
    memset(s, 0, sizeof *s);
    s->ac = ac; s->r = r; s->nr = nr; s->pos = start_pos;
    ac->seg = calloc((size_t)(nr ? nr : 1), sizeof(PcmSeg));
    if (!ac->seg) return -1;
    ac->nseg = nr;
    for (int i = 0; i < nr; i++) {
        ac->seg[i].start = r[i][0];
        ac->seg[i].count = r[i][1] - r[i][0];
        ac->seg[i].pcm = calloc((size_t)ac->seg[i].count * 2, sizeof(int16_t));   /* zeroed: undecodable parts stay silent */
        if (!ac->seg[i].pcm) return -1;
        if (r[i][1] > s->end) s->end = r[i][1];
    }
    return 0;
}

/* push `frames` frames of `ch`-channel 16-bit audio that start at the current source position */
static void sink_put(Sink *s, const int16_t *pcm, int frames, int ch)
{
    int64_t a = s->pos, b = s->pos + frames;
    for (int i = s->cur; i < s->nr; i++) {
        int64_t lo = s->r[i][0] > a ? s->r[i][0] : a, hi = s->r[i][1] < b ? s->r[i][1] : b;
        if (s->r[i][0] >= b) break;
        if (hi <= lo) continue;
        int16_t *dst = s->ac->seg[i].pcm + (lo - s->r[i][0]) * 2;
        const int16_t *src = pcm + (lo - a) * ch;
        int64_t n = hi - lo;
        if (ch == 2) memcpy(dst, src, (size_t)n * 4);
        else if (ch == 1) for (int64_t k = 0; k < n; k++) { dst[2 * k] = dst[2 * k + 1] = src[k]; }
        else for (int64_t k = 0; k < n; k++) { dst[2 * k] = src[k * ch]; dst[2 * k + 1] = src[k * ch + 1]; }
    }
    while (s->cur < s->nr && s->r[s->cur][1] <= b) s->cur++;
    s->pos = b;
}

static int sink_done(const Sink *s) { return s->pos >= s->end; }

/* ------------------------------------------------------------------ hardware decoder libraries */

static int g_mp3_lib, g_aac_lib, g_atrac_mod;
static Mutex g_at9_mtx = { -1 };   /* one SceAtrac decoder group at a time */

void adec_global_init(void)
{
    SceAudiodecInitParam ip;
    memset(&ip, 0, sizeof ip);
    ip.size = sizeof ip.mp3;
    ip.mp3.size = sizeof ip.mp3;
    ip.mp3.totalStreams = 2;
    int r = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_MP3, &ip);
    g_mp3_lib = r >= 0;
    if (r < 0) LOG("adec: MP3 hardware decoder init failed 0x%08X", r);
    memset(&ip, 0, sizeof ip);
    ip.size = sizeof ip.aac;
    ip.aac.size = sizeof ip.aac;
    ip.aac.totalStreams = 2;
    r = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &ip);
    g_aac_lib = r >= 0;
    if (r < 0) LOG("adec: AAC hardware decoder init failed 0x%08X", r);
    if (g_at9_mtx.id < 0) mutex_init(&g_at9_mtx, "vp_at9");
    g_atrac_mod = sceSysmoduleLoadModule(SCE_SYSMODULE_ATRAC) >= 0;
    if (!g_atrac_mod) LOG("adec: SceAtrac module failed to load");
}

void adec_global_term(void)
{
    if (g_mp3_lib) sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_MP3);
    if (g_aac_lib) sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);
    g_mp3_lib = g_aac_lib = 0;
}

/* ------------------------------------------------------------------ helpers */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static const uint8_t AT9_GUID[16] = { 0xD2, 0x42, 0xE1, 0x47, 0xBA, 0x36, 0x8D, 0x4D, 0x88, 0xFC, 0x61, 0x65, 0x4F, 0x8C, 0x83, 0x6C };
static const uint8_t AT3P_GUID[16] = { 0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62 };

/* ------------------------------------------------------------------ MP3 frame scanning */

static const int MP3_BR[2][16] = {   /* layer III kbps: [mpeg1][idx], [mpeg2/2.5][idx] */
    { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },
    { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 } };
static const int MP3_SR[4][3] = { { 11025, 12000, 8000 }, { 0, 0, 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 } };

/* length of the layer III frame at p, or 0 */
static int mp3_frame(const uint8_t *p, size_t avail, int *ver, int *rate, int *ch, int *spf, int *layer)
{
    if (avail < 4 || p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;
    int v = (p[1] >> 3) & 3, l = (p[1] >> 1) & 3, bi = p[2] >> 4, si = (p[2] >> 2) & 3, pad = (p[2] >> 1) & 1;
    if (v == 1 || l == 0 || bi == 0 || bi == 15 || si == 3) return 0;
    *layer = 4 - l;
    int sr = MP3_SR[v][si];
    if (*layer != 3) {   /* layer I/II: compute the length so the scan can walk them, decode will go to software */
        static const int BR1[16] = { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 };
        static const int BR2[16] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 };
        int br = (*layer == 1 ? BR1[bi] : BR2[bi]) * 1000;
        *ver = v; *rate = sr; *ch = ((p[3] >> 6) == 3) ? 1 : 2;
        *spf = *layer == 1 ? 384 : 1152;
        return *layer == 1 ? (12 * br / sr + pad) * 4 : 144 * br / sr + pad;
    }
    int br = MP3_BR[v == 3 ? 0 : 1][bi] * 1000;
    *ver = v; *rate = sr; *ch = ((p[3] >> 6) == 3) ? 1 : 2;
    *spf = v == 3 ? 1152 : 576;
    return (v == 3 ? 144 : 72) * br / sr + pad;
}

static int mp3_probe(AudioSrc *a)
{
    size_t q = 0;
    if (a->len >= 10 && !memcmp(a->buf, "ID3", 3))
        q = 10 + ((size_t)(a->buf[6] & 127) << 21 | (size_t)(a->buf[7] & 127) << 14 | (size_t)(a->buf[8] & 127) << 7 | (a->buf[9] & 127));
    /* find the first frame (two consecutive headers) */
    int ver = 0, rate = 0, ch = 0, spf = 0, layer = 0;
    for (; q + 4 < a->len; q++) {
        int l = mp3_frame(a->buf + q, a->len - q, &ver, &rate, &ch, &spf, &layer);
        int v2, r2, c2, s2, l2;
        if (l > 0 && q + (size_t)l + 4 <= a->len && mp3_frame(a->buf + q + l, a->len - q - l, &v2, &r2, &c2, &s2, &l2) > 0) break;
    }
    if (q + 4 >= a->len) return -1;
    a->mp3_start = q; a->rate = rate; a->ch = ch; a->mp3_ver = ver; a->mp3_layer = layer;
    /* Xing/Info frame with a LAME tag: skip it and the encoder delay, like gapless decoders do */
    int fl = mp3_frame(a->buf + q, a->len - q, &ver, &rate, &ch, &spf, &layer);
    size_t side = ver == 3 ? (ch == 1 ? 17 : 32) : (ch == 1 ? 9 : 17);
    const uint8_t *x = a->buf + q + 4 + side;
    a->mp3_skip = 0;
    if (q + 4 + side + 120 < a->len && (!memcmp(x, "Xing", 4) || !memcmp(x, "Info", 4))) {
        uint32_t flags = (uint32_t)x[4] << 24 | (uint32_t)x[5] << 16 | (uint32_t)x[6] << 8 | x[7];
        const uint8_t *y = x + 8;
        if (flags & 1) y += 4;
        if (flags & 2) y += 4;
        if (flags & 4) y += 100;
        if (flags & 8) y += 4;
        if ((size_t)(y - a->buf) + 24 < a->len && !memcmp(y, "LAME", 4)) {
            int enc_delay = (y[21] << 4) | (y[22] >> 4);
            a->mp3_skip = enc_delay + 529;
        }
        a->mp3_start = q + (size_t)fl;   /* the Info frame itself is silence */
    }
    /* count frames */
    int64_t frames = 0;
    size_t p = a->mp3_start;
    while (p + 4 <= a->len) {
        int l = mp3_frame(a->buf + p, a->len - p, &ver, &rate, &ch, &spf, &layer);
        if (l <= 0) { p++; continue; }
        frames += spf;
        p += (size_t)l;
    }
    a->total = frames - a->mp3_skip;
    if (a->total < 0) a->total = 0;
    return 0;
}

/* ------------------------------------------------------------------ open */

static int wav_probe(AudioSrc *a, char *err, size_t errlen)
{
    uint8_t h[12];
    if (fread(h, 1, 12, a->f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { snprintf(err, errlen, "not a WAV file"); return -1; }
    int have_fmt = 0, tag = 0;
    uint8_t fmt[64];
    memset(fmt, 0, sizeof fmt);
    int64_t pos = 12;
    for (;;) {
        uint8_t ck[8];
        if (fseek(a->f, (long)pos, SEEK_SET) || fread(ck, 1, 8, a->f) != 8) break;
        uint32_t sz = le32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            size_t n = sz < sizeof fmt ? sz : sizeof fmt;
            if (fread(fmt, 1, n, a->f) != n) break;
            have_fmt = 1;
        } else if (!memcmp(ck, "data", 4)) {
            a->data_off = pos + 8; a->data_len = sz;
            break;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (!have_fmt || !a->data_off) { snprintf(err, errlen, "WAV without fmt/data chunk"); return -1; }
    tag = le16(fmt);
    a->ch = le16(fmt + 2); a->rate = (int)le32(fmt + 4); a->block = le16(fmt + 12); a->bits = le16(fmt + 14);
    if (tag == 0xFFFE) {
        if (!memcmp(fmt + 24, AT9_GUID, 16)) { a->fmt = AF_AT9; return 0; }
        if (!memcmp(fmt + 24, AT3P_GUID, 16)) { a->fmt = AF_AT3; return 0; }
        tag = le16(fmt + 24);
    }
    if (tag == 0x0270) { a->fmt = AF_AT3; return 0; }
    if (tag != 1 && tag != 3) { snprintf(err, errlen, "WAV format 0x%04X isn't supported", tag); return -1; }
    a->is_float = tag == 3;
    if (a->ch < 1 || a->ch > 8 || !a->block || (a->is_float ? (a->bits != 32 && a->bits != 64) : (a->bits != 8 && a->bits != 16 && a->bits != 24 && a->bits != 32))) {
        snprintf(err, errlen, "unsupported WAV layout (%d ch, %d bit)", a->ch, a->bits);
        return -1;
    }
    a->total = a->data_len / a->block;
    a->fmt = AF_WAV;
    return 0;
}

int adec_open(const char *path, AudioSrc **out, char *err, size_t errlen)
{
    AudioSrc *a = calloc(1, sizeof *a);
    if (!a) { snprintf(err, errlen, "out of memory"); return -1; }
    snprintf(a->path, sizeof a->path, "%s", path);
    FILE *f = fopen(path, "rb");
    if (!f) { free(a); snprintf(err, errlen, "can't open the file"); return -1; }
    uint8_t m[16] = { 0 };
    size_t got = fread(m, 1, 16, f);
    fseek(f, 0, SEEK_SET);
    if (got >= 12 && !memcmp(m, "RIFF", 4) && !memcmp(m + 8, "WAVE", 4)) {
        a->f = f;
        if (wav_probe(a, err, errlen) < 0) goto fail;
        if (a->fmt == AF_AT3) {
            snprintf(err, errlen, "ATRAC3/ATRAC3plus: no hardware decoder reachable with VitaSDK (convert to AT9)");
            goto fail;
        }
        if (a->fmt == AF_AT9) {
            fclose(f); a->f = NULL;
            a->buf = file_read_all(path, &a->len, SCE_ATRAC_ALIGNMENT_SIZE);
            if (!a->buf) { snprintf(err, errlen, "can't read the AT9 file"); goto fail; }
            if (!g_atrac_mod) { snprintf(err, errlen, "SceAtrac module unavailable"); goto fail; }
            /* the AT9 'fact' chunk holds the sample count; SceAtrac also reports it once a handle exists */
            a->total = 0;
            for (size_t q = 12; q + 8 < a->len;) {
                uint32_t sz = le32(a->buf + q + 4);
                if (!memcmp(a->buf + q, "fact", 4) && sz >= 4) { a->total = le32(a->buf + q + 8); break; }
                q += 8 + sz + (sz & 1);
            }
        }
    } else if ((got >= 3 && !memcmp(m, "ea3", 3)) || (got >= 3 && !memcmp(m, "EA3", 3))) {
        snprintf(err, errlen, "OMA/AA3 (ATRAC3): no hardware decoder reachable with VitaSDK (convert to AT9)");
        fclose(f);
        free(a);
        return -1;
    } else if (got >= 4 && !memcmp(m, "OggS", 4)) {
        fclose(f); f = NULL;
        a->buf = file_read_all(path, &a->len, 0);
        if (!a->buf) { snprintf(err, errlen, "can't read the file"); goto fail; }
        int e = 0;
        stb_vorbis *v = stb_vorbis_open_memory(a->buf, (int)a->len, &e, NULL);
        if (!v) { snprintf(err, errlen, "not an Ogg Vorbis stream (%d)", e); goto fail; }
        stb_vorbis_info vi = stb_vorbis_get_info(v);
        a->rate = (int)vi.sample_rate; a->ch = vi.channels;
        a->total = stb_vorbis_stream_length_in_samples(v);
        stb_vorbis_close(v);
        a->fmt = AF_OGG;
    } else if (got >= 8 && (!memcmp(m + 4, "ftyp", 4) || !memcmp(m + 4, "moov", 4) || !memcmp(m + 4, "mdat", 4) || !memcmp(m + 4, "wide", 4) || !memcmp(m + 4, "free", 4))) {
        fclose(f); f = NULL;
        if (mp4_open(&a->mp4, path) < 0) { snprintf(err, errlen, "%s", a->mp4.err); goto fail; }
        a->mp4_open = 1;
        Mp4Track *t = &a->mp4.audio;
        if (!t->present) { snprintf(err, errlen, "%s", a->mp4.err[0] ? a->mp4.err : "the file has no AAC audio track"); goto fail; }
        a->rate = t->sample_rate; a->ch = t->channels > 2 ? 2 : t->channels;
        a->total = (int64_t)((double)t->duration * a->rate / (t->timescale ? t->timescale : 1) + 0.5);
        a->fmt = AF_AAC;
    } else {
        fclose(f); f = NULL;
        a->buf = file_read_all(path, &a->len, 0);
        if (!a->buf) { snprintf(err, errlen, "can't read the file"); goto fail; }
        if (mp3_probe(a) < 0) { snprintf(err, errlen, "unknown audio format"); goto fail; }
        a->fmt = AF_MP3;
    }
    if (a->rate < 4000 || a->rate > 192000 || a->ch < 1) { snprintf(err, errlen, "bad audio parameters (%d Hz, %d ch)", a->rate, a->ch); goto fail; }
    *out = a;
    return 0;
fail:
    adec_close(a);
    return -1;
}

void adec_info(AudioSrc *a, int *rate, int64_t *total)
{
    *rate = a->rate;
    *total = a->total;
}

const char *adec_codec_name(AudioSrc *a)
{
    switch (a->fmt) {
    case AF_WAV: return "WAV";
    case AF_AT9: return "AT9 (hardware)";
    case AF_MP3: return "MP3";
    case AF_OGG: return "Ogg Vorbis";
    case AF_AAC: return "AAC (hardware)";
    default: return "?";
    }
}

void adec_close(AudioSrc *a)
{
    if (!a) return;
    if (a->f) fclose(a->f);
    if (a->mp4_open) mp4_close(&a->mp4);
    free(a->buf);
    free(a);
}

/* ------------------------------------------------------------------ decoders */

static int dec_wav(AudioSrc *a, Sink *s, volatile float *prog, volatile int *cancel)
{
    const int CH = 4096;
    uint8_t *raw = malloc((size_t)CH * (size_t)a->block);
    int16_t *pcm = malloc((size_t)CH * 2 * sizeof(int16_t));
    if (!raw || !pcm) { free(raw); free(pcm); return -1; }
    fseek(a->f, (long)a->data_off, SEEK_SET);
    int64_t left = a->total;
    while (left > 0 && !sink_done(s) && !*cancel) {
        int n = (int)(left < CH ? left : CH);
        size_t got = fread(raw, (size_t)a->block, (size_t)n, a->f);
        if (got == 0) break;
        n = (int)got;
        int bps = a->bits / 8;
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < 2; c++) {
                int cc = c < a->ch ? c : 0;
                const uint8_t *p = raw + (size_t)i * a->block + (size_t)cc * bps;
                int v;
                if (a->is_float) {
                    double d;
                    if (a->bits == 32) { float fl; uint32_t u = le32(p); memcpy(&fl, &u, 4); d = fl; }
                    else { uint64_t u = (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; memcpy(&d, &u, 8); }
                    d = d > 1 ? 1 : d < -1 ? -1 : d;
                    v = (int)(d * 32767.0);
                } else if (a->bits == 8) v = ((int)p[0] - 128) << 8;
                else if (a->bits == 16) v = (int16_t)le16(p);
                else if (a->bits == 24) v = (int16_t)(p[1] | p[2] << 8);
                else v = (int16_t)le16(p + 2);
                pcm[i * 2 + c] = (int16_t)v;
            }
        }
        sink_put(s, pcm, n, 2);
        left -= n;
        if (prog) *prog = (float)s->pos / (float)(s->end ? s->end : 1);
    }
    free(raw); free(pcm);
    return 0;
}

static int dec_ogg(AudioSrc *a, Sink *s, volatile float *prog, volatile int *cancel)
{
    int e = 0;
    stb_vorbis *v = stb_vorbis_open_memory(a->buf, (int)a->len, &e, NULL);
    if (!v) return -1;
    int16_t buf[4096 * 2];
    while (!sink_done(s) && !*cancel) {
        int n = stb_vorbis_get_samples_short_interleaved(v, 2, buf, 4096 * 2);
        if (n <= 0) break;
        sink_put(s, buf, n, 2);
        if (prog) *prog = (float)s->pos / (float)(s->end ? s->end : 1);
    }
    stb_vorbis_close(v);
    return 0;
}

static int dec_mp3_sw(AudioSrc *a, Sink *s, volatile float *prog, volatile int *cancel)
{
    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, a->buf, a->len, NULL)) return -1;
    int16_t buf[4096 * 2];
    int ch = (int)mp3.channels;
    int16_t *tmp = ch > 2 ? malloc(sizeof(int16_t) * 4096 * (size_t)ch) : NULL;
    while (!sink_done(s) && !*cancel) {
        drmp3_uint64 n = drmp3_read_pcm_frames_s16(&mp3, 4096 / (ch > 2 ? 1 : 1), ch <= 2 ? buf : tmp);
        if (n == 0) break;
        sink_put(s, ch <= 2 ? buf : tmp, (int)n, ch);
        if (prog) *prog = (float)s->pos / (float)(s->end ? s->end : 1);
    }
    free(tmp);
    drmp3_uninit(&mp3);
    return 0;
}

static int dec_mp3_hw(AudioSrc *a, Sink *s, volatile float *prog, volatile int *cancel)
{
    if (!g_mp3_lib || a->mp3_layer != 3) return -1;
    SceAudiodecInfo info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info.mp3;
    info.mp3.size = sizeof info.mp3;
    info.mp3.ch = (SceUInt32)a->ch;
    info.mp3.version = (SceUInt32)a->mp3_ver;
    SceAudiodecCtrl ctrl;
    memset(&ctrl, 0, sizeof ctrl);
    ctrl.size = sizeof ctrl;
    ctrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
    ctrl.pInfo = &info;
    int r = sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_MP3);
    if (r < 0) { LOG("adec: MP3 decoder creation failed 0x%08X", r); return -1; }
    uint8_t *es = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(ctrl.maxEsSize + 16));
    int16_t *pcm = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(ctrl.maxPcmSize));
    if (!es || !pcm) { free(es); free(pcm); sceAudiodecDeleteDecoder(&ctrl); return -1; }
    size_t p = a->mp3_start;
    int64_t skip = a->mp3_skip;
    int errors = 0, ok_frames = 0;
    s->pos = 0;
    while (p + 4 <= a->len && !sink_done(s) && !*cancel) {
        int ver, rate, ch, spf, layer;
        int l = mp3_frame(a->buf + p, a->len - p, &ver, &rate, &ch, &spf, &layer);
        if (l <= 0 || p + (size_t)l > a->len) { p++; continue; }
        size_t n = (size_t)l < ctrl.maxEsSize ? (size_t)l : ctrl.maxEsSize;
        memcpy(es, a->buf + p, n);
        ctrl.pEs = es;
        ctrl.pPcm = pcm;
        r = sceAudiodecDecode(&ctrl);
        int frames;
        if (r < 0) {
            if (++errors > 32 && !ok_frames) break;   /* the hardware doesn't like this stream: software fallback */
            frames = spf;
            memset(pcm, 0, (size_t)spf * (size_t)a->ch * 2);
        } else {
            ok_frames++;
            frames = (int)(ctrl.outputPcmSize / (2u * (unsigned)a->ch));
        }
        p += (size_t)l;
        const int16_t *src = pcm;
        if (skip > 0) {
            int k = (int)(skip < frames ? skip : frames);
            skip -= k; src += k * a->ch; frames -= k;
        }
        if (frames > 0) sink_put(s, src, frames, a->ch);
        if (prog) *prog = (float)s->pos / (float)(s->end ? s->end : 1);
    }
    sceAudiodecDeleteDecoder(&ctrl);
    free(es); free(pcm);
    if (!ok_frames) return -1;
    if (errors) LOG("adec: %d MP3 frames failed to decode in %s", errors, a->path);
    return 0;
}

static int dec_at9(AudioSrc *a, Sink *s, volatile float *prog, volatile int *cancel, char *err, size_t errlen)
{
    SceAtracDecoderGroup grp;
    memset(&grp, 0, sizeof grp);
    grp.size = sizeof grp;
    grp.wordLength = SCE_ATRAC_WORD_LENGTH_16BITS;
    grp.totalCh = (SceUInt32)(a->ch > 2 ? a->ch : 2);
    int msz = sceAtracQueryDecoderGroupMemSize(SCE_ATRAC_TYPE_AT9, &grp);
    if (msz < 0) { snprintf(err, errlen, "AT9 decoder size query failed (0x%08X)", msz); return -1; }
    void *work = memalign(SCE_ATRAC_ALIGNMENT_SIZE, SCE_ATRAC_ROUND_UP((unsigned)msz));
    if (!work) { snprintf(err, errlen, "out of memory"); return -1; }
    int r = sceAtracCreateDecoderGroup(SCE_ATRAC_TYPE_AT9, &grp, work, 1);
    if (r < 0) { free(work); snprintf(err, errlen, "AT9 decoder creation failed (0x%08X)", r); return -1; }
    /* the buffer size must be a multiple of SCE_ATRAC_ALIGNMENT_SIZE (else 0x80630005 INVALID_ALIGNMENT):
     * file_read_all allocated it rounded up and zeroed the tail; the read size is the real file size */
    int h = sceAtracSetDataAndAcquireHandle(a->buf, (SceUInt32)a->len, (SceUInt32)SCE_ATRAC_ROUND_UP(a->len));
    int result = 0;
    if (h < 0) { snprintf(err, errlen, "AT9 data rejected (0x%08X)", h); result = -1; }
    else {
        SceAtracContentInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.size = sizeof ci;
        if (sceAtracGetContentInfo(h, &ci) >= 0) {
            if (ci.samplingRate) a->rate = (int)ci.samplingRate;
            if (ci.channel) a->ch = (int)ci.channel;
        }
        sceAtracSetLoopNum(h, 0);
        int16_t *pcm = memalign(SCE_ATRAC_ALIGNMENT_SIZE, SCE_ATRAC_ROUND_UP(SCE_ATRAC_MAX_OUTPUT_SAMPLES * 2u * (unsigned)a->ch));
        if (!pcm) result = -1;
        while (pcm && !sink_done(s) && !*cancel) {
            SceUInt32 n = 0, st = 0;
            r = sceAtracDecode(h, pcm, &n, &st);
            if (r < 0) { if (r != (int)SCE_ATRAC_ERROR_ALL_DATA_WAS_DECODED) LOG("adec: sceAtracDecode 0x%08X", r); break; }
            if (n) sink_put(s, pcm, (int)n, a->ch);
            if (prog) *prog = (float)s->pos / (float)(s->end ? s->end : 1);
            if (st & SCE_ATRAC_DECODER_STATUS_ALL_DATA_WAS_DECODED) break;
        }
        free(pcm);
        sceAtracReleaseHandle(h);
    }
    sceAtracDeleteDecoderGroup(SCE_ATRAC_TYPE_AT9, 1);
    free(work);
    return result;
}

static int dec_aac(AudioSrc *a, Sink *s, volatile float *prog, volatile int *cancel, char *err, size_t errlen)
{
    if (!g_aac_lib) { snprintf(err, errlen, "AAC hardware decoder unavailable"); return -1; }
    Mp4Track *t = &a->mp4.audio;
    int tch = t->channels > 2 ? 2 : t->channels;
    int core_rate = t->sbr && t->sample_rate > 24000 ? t->sample_rate / 2 : t->sample_rate;
    uint8_t *raw = malloc(8192);
    int16_t *pcm = NULL;
    uint8_t *es = NULL;
    int result = -1;
    for (int adts = 0; adts < 2 && result < 0; adts++) {
        SceAudiodecInfo info;
        memset(&info, 0, sizeof info);
        info.size = sizeof info.aac;
        info.aac.size = sizeof info.aac;
        info.aac.isAdts = (SceUInt32)adts;
        info.aac.ch = (SceUInt32)tch;
        info.aac.samplingRate = (SceUInt32)core_rate;
        info.aac.isSbr = (SceUInt32)t->sbr;
        SceAudiodecCtrl ctrl;
        memset(&ctrl, 0, sizeof ctrl);
        ctrl.size = sizeof ctrl;
        ctrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
        ctrl.pInfo = &info;
        int r = sceAudiodecCreateDecoder(&ctrl, SCE_AUDIODEC_TYPE_AAC);
        if (r < 0) { LOG("adec: AAC decoder (adts=%d) creation failed 0x%08X", adts, r); continue; }
        free(es); free(pcm);
        es = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(8192));
        pcm = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, SCE_AUDIODEC_ROUND_UP(ctrl.maxPcmSize > 16384 ? ctrl.maxPcmSize : 16384));
        if (!raw || !es || !pcm) { sceAudiodecDeleteDecoder(&ctrl); break; }
        /* the first sample's dts is negative by the edit list's priming delay: those samples get dropped */
        double scale = (double)a->rate / (double)(t->timescale ? t->timescale : 1);
        s->pos = (int64_t)floor((double)t->s[0].dts * scale + 0.5);
        int ok = 0, bad = 0;
        for (uint32_t i = 0; i < t->n && !sink_done(s) && !*cancel; i++) {
            uint32_t sz = t->s[i].size;
            if (sz > 8192 - 8 || mp4_read_sample(&a->mp4, t, i, raw) < 0) { bad++; continue; }
            uint32_t len = sz;
            if (adts) { mp4_adts_header(t, sz, es); memcpy(es + 7, raw, sz); len += 7; }
            else memcpy(es, raw, sz);
            ctrl.pEs = es;
            ctrl.inputEsSize = len;
            ctrl.pPcm = pcm;
            r = sceAudiodecDecode(&ctrl);
            int frames;
            int64_t nominal = i + 1 < t->n ? (int64_t)((double)(t->s[i + 1].dts - t->s[i].dts) * scale + 0.5) : 1024 * (t->sbr ? 2 : 1);
            if (r < 0) {
                if (!ok && ++bad > 4) break;   /* this mode doesn't work: try the other */
                frames = (int)nominal;
                if (frames > 4096) frames = 4096;
                memset(pcm, 0, (size_t)frames * (size_t)tch * 2);
            } else {
                ok++;
                frames = (int)(ctrl.outputPcmSize / (2u * (unsigned)tch));
            }
            sink_put(s, pcm, frames, tch);
            if (prog) *prog = (float)s->pos / (float)(s->end ? s->end : 1);
        }
        sceAudiodecDeleteDecoder(&ctrl);
        if (ok) result = 0;
        else LOG("adec: AAC (adts=%d) produced nothing", adts);
    }
    free(raw); free(es); free(pcm);
    if (result < 0) snprintf(err, errlen, "the AAC hardware decoder rejected this stream");
    return result;
}

int adec_decode(AudioSrc *a, AudioCache *ac, const int64_t (*ranges)[2], int nr, volatile float *prog, volatile int *cancel, char *err, size_t errlen)
{
    Sink s;
    memset(ac, 0, sizeof *ac);
    ac->rate = a->rate;
    ac->total = a->total;
    if (sink_init(&s, ac, ranges, nr, 0) < 0) { snprintf(err, errlen, "out of memory for decoded audio"); return -1; }
    int r = -1;
    switch (a->fmt) {
    case AF_WAV: r = dec_wav(a, &s, prog, cancel); break;
    case AF_OGG: r = dec_ogg(a, &s, prog, cancel); break;
    case AF_AT9: mutex_lock(&g_at9_mtx); r = dec_at9(a, &s, prog, cancel, err, errlen); mutex_unlock(&g_at9_mtx); break;
    case AF_AAC: r = dec_aac(a, &s, prog, cancel, err, errlen); break;
    case AF_MP3:
        r = dec_mp3_hw(a, &s, prog, cancel);
        if (r < 0) {
            LOG("adec: %s: using the software MP3 decoder", a->path);
            for (int i = 0; i < ac->nseg; i++) memset(ac->seg[i].pcm, 0, (size_t)ac->seg[i].count * 4);
            s.pos = 0; s.cur = 0;
            r = dec_mp3_sw(a, &s, prog, cancel);
        }
        if (r < 0) snprintf(err, errlen, "MP3 decoding failed");
        break;
    default: snprintf(err, errlen, "unsupported audio format"); break;
    }
    if (r == 0) ac->ok = 1;
    if (prog) *prog = 1.0f;
    return r;
}
