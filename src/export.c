/* export.c - see export.h. The mix is the same as playback (track toggles, sliders, master volume, reverbs), rendered
 * as fast as the CPU allows. WAV: 48 kHz 16-bit stereo. MP3: shine (fixed-point encoder, LGPL, third_party/shine). */
#include "export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "media.h"
#include "mixer.h"
#include "../third_party/shine/layer3.h"

ExportJob g_export;
static int g_thid = -1;

#ifndef EXPORT_DIR
#define EXPORT_DIR "ux0:data/vegplayer/exports"
#endif

const char *export_dir(void) { return EXPORT_DIR; }

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static void wav_header(uint8_t h[44], uint32_t frames)
{
    uint32_t data = frames * 4;
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 2); put32(h + 24, OUT_RATE); put32(h + 28, OUT_RATE * 4);
    put16(h + 32, 4); put16(h + 34, 16); memcpy(h + 36, "data", 4); put32(h + 40, data);
}

static int export_thread(unsigned argc, void *argp)
{
    (void)argc; (void)argp;
    ExportJob *j = &g_export;
    int64_t total = (int64_t)(g_s.proj.duration * OUT_RATE + 0.5);
    const int chunk = 1152;   /* one MP3 frame of samples per channel; any size works for WAV */
    float *mix = malloc(sizeof(float) * chunk * 2);
    int16_t *pcm = malloc(sizeof(int16_t) * chunk * 2);
    FILE *f = fopen(j->path, "wb");
    shine_t sh = NULL;
    if (!mix || !pcm || !f) {
        snprintf(j->msg, sizeof j->msg, f ? "out of memory" : "can't create %s", j->path);
        j->failed = 1;
        goto out;
    }
    setvbuf(f, NULL, _IOFBF, 256 * 1024);
    if (j->fmt == EXPORT_MP3) {
        shine_config_t cfg;
        shine_set_config_mpeg_defaults(&cfg.mpeg);
        cfg.wave.channels = PCM_STEREO;
        cfg.wave.samplerate = OUT_RATE;
        cfg.mpeg.mode = JOINT_STEREO;
        cfg.mpeg.bitr = j->kbps;
        if (shine_check_config(OUT_RATE, j->kbps) < 0 || !(sh = shine_initialise(&cfg))) {
            snprintf(j->msg, sizeof j->msg, "MP3 encoder setup failed (%d kbps)", j->kbps);
            j->failed = 1;
            goto out;
        }
    } else {
        uint8_t h[44];
        wav_header(h, (uint32_t)total);
        fwrite(h, 1, 44, f);
    }
    mixer_project_changed();
    audio_lock();
    mixer_reset();
    int64_t pos = 0;
    while (pos < total && !j->cancel) {
        int n = total - pos < chunk ? (int)(total - pos) : chunk;
        mixer_mix(mix, n, pos);
        if (n < chunk) memset(mix + n * 2, 0, sizeof(float) * (chunk - n) * 2);   /* the last MP3 frame: pad with silence */
        mixer_to_pcm(mix, pcm, chunk * 2);
        int ok;
        if (sh) {
            int written = 0;
            unsigned char *d = shine_encode_buffer_interleaved(sh, pcm, &written);
            ok = written <= 0 || fwrite(d, 1, (size_t)written, f) == (size_t)written;
        } else ok = fwrite(pcm, 4, (size_t)n, f) == (size_t)n;
        if (!ok) { snprintf(j->msg, sizeof j->msg, "write error (memory card full?)"); j->failed = 1; break; }
        pos += n;
        j->progress = (float)pos / (float)(total > 0 ? total : 1);
    }
    mixer_reset();
    audio_unlock();
    if (sh) {
        int written = 0;
        unsigned char *d = shine_flush(sh, &written);
        if (written > 0 && !j->failed) fwrite(d, 1, (size_t)written, f);
    }
out:
    if (sh) shine_close(sh);
    if (f && fclose(f) != 0 && !j->failed) { snprintf(j->msg, sizeof j->msg, "write error (memory card full?)"); j->failed = 1; }
    if ((j->cancel || j->failed) && f) remove(j->path);
    free(mix); free(pcm);
    j->t1 = time_us();
    if (!j->failed && !j->cancel) snprintf(j->msg, sizeof j->msg, "Saved %s", j->path);
    LOG("export: %s (%.1f s)", j->cancel ? "cancelled" : j->msg, (j->t1 - j->t0) / 1e6);
    j->done = 1;
    return 0;
}

int export_start(int fmt, int kbps)
{
    if (g_export.running) return -1;
    memset(&g_export, 0, sizeof g_export);
    g_export.fmt = fmt;
    g_export.kbps = kbps;
    mkdir_p(EXPORT_DIR);
    /* the .veg's name, without the extension */
    const char *b = strrchr(g_s.veg_path, '/');
    b = b ? b + 1 : strchr(g_s.veg_path, ':') ? strchr(g_s.veg_path, ':') + 1 : g_s.veg_path;
    char name[256];
    snprintf(name, sizeof name, "%s", b);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
    const char *ext = fmt == EXPORT_MP3 ? "mp3" : "wav";
    snprintf(g_export.path, sizeof g_export.path, EXPORT_DIR "/%s.%s", name, ext);
    for (int k = 2; k < 100; k++) {   /* don't overwrite an earlier export */
        FILE *t = fopen(g_export.path, "rb");
        if (!t) break;
        fclose(t);
        snprintf(g_export.path, sizeof g_export.path, EXPORT_DIR "/%s (%d).%s", name, k, ext);
    }
    g_export.running = 1;
    g_export.t0 = time_us();
    /* cores 0 and 1: the audio and scene threads aren't busy while exporting */
    g_thid = thread_start("vp_export", export_thread, NULL, 0x10000100 + 16, 128 * 1024, 0x30000);
    if (g_thid < 0) { g_export.running = 0; return -1; }
    return 0;
}

void export_finish(void)
{
    if (g_thid >= 0) { thread_join(g_thid); g_thid = -1; }
    g_export.running = 0;
}
