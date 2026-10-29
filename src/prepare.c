/* prepare.c - session (project + chosen sources) and the loader that fills the frame caches and PCM buffers.
 * Loading runs on worker threads: one for the hardware H.264 decoder, two for audio, while the UI keeps drawing. */
#include "common.h"
#include "pfs.h"
#include "media.h"
#include "mp4.h"
#include "veg.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/dirent.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

Session g_s;
PrepareState g_prep;

/* ------------------------------------------------------------------ file kinds */

static const char *IMG_EXT[] = { ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".tga", ".psd", ".pnm", ".ppm", ".pgm", ".hdr", ".pic", NULL };
static const char *VID_EXT[] = { ".mp4", ".m4v", ".mov", NULL };
static const char *AUD_EXT[] = { ".wav", ".mp3", ".ogg", ".at9", ".at3", ".m4a", ".oma", ".aa3", NULL };

int media_kind_of_file(const char *path)
{
    for (int i = 0; IMG_EXT[i]; i++) if (str_iends(path, IMG_EXT[i])) return 1;
    for (int i = 0; VID_EXT[i]; i++) if (str_iends(path, VID_EXT[i])) return 3;
    for (int i = 0; AUD_EXT[i]; i++) if (str_iends(path, AUD_EXT[i])) return 2;
    return 0;
}

/* a picture source: a picture file, or an MP3 (its cover art) */
static int is_image(const char *path) { return media_kind_of_file(path) == 1 || str_iends(path, ".mp3"); }

/* ------------------------------------------------------------------ session */

static void sidecar_path(char *out, size_t n) { snprintf(out, n, "%s.vegplayer", g_s.veg_path); }

static void load_sidecar(void)
{
    char p[600];
    sidecar_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        char kind = line[0];
        char *eq = strchr(line, '=');
        if (!eq || (kind != 'v' && kind != 'a' && kind != 't')) continue;
        *eq++ = 0;
        unsigned id = (unsigned)strtoul(line + 1, NULL, 10);
        if (kind == 't') {   /* track toggles */
            if (id < (unsigned)g_s.proj.ntracks) g_s.track_on[id] = atoi(eq) != 0;
            continue;
        }
        for (int i = 0; i < g_s.proj.nmedia; i++) {
            if (g_s.proj.media[i].id != id) continue;
            MediaRt *m = &g_s.m[i];
            if (!strcmp(eq, "*skip*")) { if (kind == 'v') m->vstate = MS_SKIP; else m->astate = MS_SKIP; }
            else if (file_exists(eq)) {
                if (kind == 'v') { snprintf(m->vfile, sizeof m->vfile, "%s", eq); m->vstate = MS_USER; }
                else { snprintf(m->afile, sizeof m->afile, "%s", eq); m->astate = MS_USER; }
            }
        }
    }
    fclose(f);
}

void session_save_sources(void)
{
    char p[600];
    sidecar_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < g_s.proj.nmedia; i++) {
        MediaRt *m = &g_s.m[i];
        unsigned id = g_s.proj.media[i].id;
        if (m->vstate == MS_SKIP) fprintf(f, "v%u=*skip*\n", id); else if (m->vstate == MS_USER) fprintf(f, "v%u=%s\n", id, m->vfile);
        if (m->astate == MS_SKIP) fprintf(f, "a%u=*skip*\n", id); else if (m->astate == MS_USER) fprintf(f, "a%u=%s\n", id, m->afile);
    }
    for (int t = 0; t < g_s.proj.ntracks; t++) if (!g_s.track_on[t]) fprintf(f, "t%d=0\n", t);
    fclose(f);
}

/* case-insensitive search for `name` in dir (and one level of sub-folders) */
static int find_in_dir(const char *dir, const char *name, char *out, size_t outsz, int depth)
{
    SceUID d = sceIoDopen(dir);
    if (d < 0) return 0;
    SceIoDirent e;
    int found = 0;
    char sub[16][256];
    int nsub = 0;
    while (!found && sceIoDread(d, &e) > 0) {
        if (SCE_S_ISDIR(e.d_stat.st_mode)) {
            if (depth > 0 && nsub < 16 && e.d_name[0] != '.') snprintf(sub[nsub++], 256, "%s", e.d_name);
            continue;
        }
        if (str_ieq(e.d_name, name)) { snprintf(out, outsz, "%s/%s", dir, e.d_name); found = 1; }
    }
    sceIoDclose(d);
    for (int i = 0; i < nsub && !found; i++) {
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, sub[i]);
        found = find_in_dir(p, name, out, outsz, depth - 1);
    }
    return found;
}

static int find_source(const char *name, char *out, size_t outsz)
{
    if (find_in_dir(g_s.veg_dir, name, out, outsz, 1)) return 1;
    if (find_in_dir(APP_DIR "/media", name, out, outsz, 1)) return 1;
    return 0;
}

void session_auto_find_sources(void)
{
    static const char *ALT_AUDIO[] = { ".at9", ".wav", ".mp3", ".ogg", ".m4a", NULL };
    for (int i = 0; i < g_s.proj.nmedia; i++) {
        VegMedia *vm = &g_s.proj.media[i];
        MediaRt *m = &g_s.m[i];
        if (vm->type != VEG_MEDIA_FILE) continue;
        char found[512];
        int have = 0;
        if ((vm->used_video || vm->used_audio) && (m->vstate == MS_NONE || m->astate == MS_NONE)) have = find_source(vm->name, found, sizeof found);
        int kind = have ? media_kind_of_file(found) : 0;
        if (vm->used_video && m->vstate == MS_NONE && have && (kind & 1)) { snprintf(m->vfile, sizeof m->vfile, "%s", found); m->vstate = MS_AUTO; }
        if (vm->used_audio && m->astate == MS_NONE) {
            if (have && (kind & 2)) { snprintf(m->afile, sizeof m->afile, "%s", found); m->astate = MS_AUTO; }
            else {   /* the same name with an audio extension (e.g. sound converted to AT9) */
                char stem[256];
                snprintf(stem, sizeof stem, "%s", vm->name);
                char *dot = strrchr(stem, '.');
                if (dot) *dot = 0;
                for (int k = 0; ALT_AUDIO[k]; k++) {
                    char nm[300];
                    snprintf(nm, sizeof nm, "%s%s", stem, ALT_AUDIO[k]);
                    if (find_source(nm, found, sizeof found)) { snprintf(m->afile, sizeof m->afile, "%s", found); m->astate = MS_AUTO; break; }
                }
            }
        }
    }
}

int session_open(const char *veg_path, char *err, size_t errlen)
{
    session_close();
    if (veg_load_file(veg_path, &g_s.proj, err, errlen) < 0) return -1;
    snprintf(g_s.veg_path, sizeof g_s.veg_path, "%s", veg_path);
    path_dir(veg_path, g_s.veg_dir, sizeof g_s.veg_dir);
    g_s.m = calloc((size_t)(g_s.proj.nmedia ? g_s.proj.nmedia : 1), sizeof(MediaRt));
    g_s.track_on = calloc((size_t)(g_s.proj.ntracks ? g_s.proj.ntracks : 1), sizeof(int));
    if (!g_s.m || !g_s.track_on) { snprintf(err, errlen, "out of memory"); session_close(); return -1; }
    for (int t = 0; t < g_s.proj.ntracks; t++) { g_s.track_on[t] = 1; g_s.proj.tracks[t].level_veg = g_s.proj.tracks[t].level; }
    g_s.master = 1.0f;
    load_sidecar();
    session_auto_find_sources();
    LOG("project %s: %dx%d %.3f fps, %d tracks, %d events, %d media", veg_path, g_s.proj.width, g_s.proj.height, g_s.proj.fps,
        g_s.proj.ntracks, g_s.proj.nevents, g_s.proj.nmedia);
    return 0;
}

void session_free_caches(void)
{
    for (int i = 0; i < g_s.proj.nmedia && g_s.m; i++) {
        MediaRt *m = &g_s.m[i];
        free(m->vc.pts); free(m->vc.slot); free(m->vc.img); free(m->vc.imgq);
        memset(&m->vc, 0, sizeof m->vc);
        for (int k = 0; k < m->ac.nseg; k++) free(m->ac.seg[k].pcm);
        free(m->ac.seg);
        memset(&m->ac, 0, sizeof m->ac);
        m->vmsg[0] = m->amsg[0] = 0;
    }
    gpu_arena_release();
    memset(&g_s.white, 0, sizeof g_s.white);
    g_s.prepared = 0;
}

void session_close(void)
{
    if (g_prep.running) { g_prep.cancel = 1; while (!g_prep.done) sleep_us(10000); prepare_finish(); }
    session_free_caches();
    free(g_s.m);
    free((void *)g_s.track_on);
    veg_free(&g_s.proj);
    memset(&g_s, 0, sizeof g_s);
}

/* ------------------------------------------------------------------ what each source has to provide */

double event_audio_speed(const VegEvent *e);   /* mixer.c */

typedef struct { double a, b; } Rng;

static int cmp_rng(const void *x, const void *y)
{
    const Rng *p = x, *q = y;
    return p->a < q->a ? -1 : p->a > q->a;
}

/* add [a,b) (seconds) to the list, wrapping around `dur` like Vegas loops a short source */
static void add_range(Rng **r, int *n, int *cap, double a, double b, double dur)
{
    if (b <= a) return;
    if (dur > 0 && b - a >= dur) { a = 0; b = dur; }
    else if (dur > 0 && (a >= dur || b > dur)) {
        double a2 = fmod(a, dur), b2 = a2 + (b - a);
        if (a2 < 0) { a2 += dur; b2 += dur; }
        if (b2 > dur) { add_range(r, n, cap, a2, dur, 0); add_range(r, n, cap, 0, b2 - dur, 0); return; }
        a = a2; b = b2;
    }
    if (a < 0) a = 0;
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        Rng *nr = realloc(*r, sizeof(Rng) * (size_t)*cap);
        if (!nr) return;
        *r = nr;
    }
    (*r)[(*n)++] = (Rng){ a, b };
}

static int merge_ranges(Rng *r, int n, double gap)
{
    if (!n) return 0;
    qsort(r, (size_t)n, sizeof(Rng), cmp_rng);
    int w = 0;
    for (int i = 1; i < n; i++) {
        if (r[i].a <= r[w].b + gap) { if (r[i].b > r[w].b) r[w].b = r[i].b; }
        else r[++w] = r[i];
    }
    return w + 1;
}

/* ------------------------------------------------------------------ jobs */

typedef struct {
    int media;
    Mp4File mp4; int mp4_ok;
    uint8_t *need;            /* per display frame */
    int nneed;
    int cw, ch, bpp;
} VJob;

static VJob *g_vjobs; static int g_nvjobs;
static int g_ajob_next; static Mutex g_ajob_mtx = { -1 };
static int g_threads[3] = { -1, -1, -1 };
static volatile int g_workers_left;

static void set_line(const char *fmt, const char *name)
{
    snprintf(g_prep.line, sizeof g_prep.line, fmt, name);
}

static int video_worker(unsigned argc, void *argp)
{
    (void)argc; (void)argp;
    for (int j = 0; j < g_nvjobs && !g_prep.cancel; j++) {
        VJob *vj = &g_vjobs[j];
        MediaRt *m = &g_s.m[vj->media];
        VegMedia *vm = &g_s.proj.media[vj->media];
        set_line("Decoding pictures of %s", vm->name);
        char err[128] = "";
        if (vj->mp4_ok) {
            int m4v = vj->mp4.video.codec == 0x6D703476u;   /* 'mp4v': MPEG-4 Part 2, decoded in software */
            int ok = m4v ? vdec_fill_cache_sw(&vj->mp4, &m->vc, vj->need, &m->vprog, &g_prep.cancel, err, sizeof err)
                         : vdec_fill_cache(&vj->mp4, &m->vc, vj->need, &m->vprog, &g_prep.cancel, err, sizeof err);
            if (ok == 0) {
                /* frames that failed: point them at the nearest decoded neighbour */
                VideoCache *vc = &m->vc;
                int last = -1;
                for (int k = 0; k < vc->nframes; k++) { if (vc->slot[k] >= 0) last = vc->slot[k]; else if (vj->need[k] && last >= 0) vc->slot[k] = last; }
                last = -1;
                for (int k = vc->nframes - 1; k >= 0; k--) { if (vc->slot[k] >= 0) last = vc->slot[k]; else if (vj->need[k] && last >= 0) vc->slot[k] = last; }
                vc->ok = 1;
                snprintf(m->vmsg, sizeof m->vmsg, "%s %dx%d -> %d frames cached at %dx%d", m4v ? "MPEG-4" : "H.264", vc->src_w, vc->src_h, vc->nimg, vc->cache_w, vc->cache_h);
            } else {
                snprintf(m->vmsg, sizeof m->vmsg, "video: %s", err);
                LOG("%s: %s", vm->name, err);
            }
            mp4_close(&vj->mp4);
            vj->mp4_ok = 0;
        }
        free(vj->need);
        vj->need = NULL;
        m->vprog = 1.0f;
    }
    vdec_global_term();
    __atomic_sub_fetch(&g_workers_left, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static int audio_job(int i)
{
    MediaRt *m = &g_s.m[i];
    VegMedia *vm = &g_s.proj.media[i];
    char err[128] = "";
    AudioSrc *src = NULL;
    set_line("Decoding sound of %s", vm->name);
    if (adec_open(m->afile, &src, err, sizeof err) < 0) {
        snprintf(m->amsg, sizeof m->amsg, "audio: %s", err);
        LOG("%s: %s", m->afile, err);
        return -1;
    }
    int rate;
    int64_t total;
    adec_info(src, &rate, &total);
    double dur = total > 0 ? (double)total / rate : 0;
    /* source time ranges the events read */
    Rng *r = NULL;
    int n = 0, cap = 0;
    VegProject *p = &g_s.proj;
    for (int e = 0; e < p->nevents; e++) {
        VegEvent *ev = &p->events[e];
        if (ev->kind != VEG_KIND_AUDIO || ev->media != i) continue;
        double s0 = ev->offset * ev->rate, len = ev->length * event_audio_speed(ev);
        double grain = 0.15 * (1.0 + fabs(ev->pitch_semis) / 12.0);   /* granular pitch shifting reads a little around */
        add_range(&r, &n, &cap, s0 - 0.05, s0 + len + grain + 0.05, dur);
    }
    n = merge_ranges(r, n, 0.5);
    double covered = 0;
    for (int k = 0; k < n; k++) covered += r[k].b - r[k].a;
    int64_t (*fr)[2] = malloc(sizeof(int64_t[2]) * (size_t)(n ? n : 1));
    int nf = 0;
    if (fr && dur > 0 && covered > 0.7 * dur) { fr[0][0] = 0; fr[0][1] = total; nf = 1; }
    else if (fr) {
        for (int k = 0; k < n; k++) {
            int64_t a = (int64_t)floor(r[k].a * rate), b = (int64_t)ceil(r[k].b * rate);
            if (total > 0 && b > total) b = total;
            if (b > a) { fr[nf][0] = a; fr[nf][1] = b; nf++; }
        }
    }
    free(r);
    int res = fr ? adec_decode(src, &m->ac, (const int64_t (*)[2])fr, nf, &m->aprog, &g_prep.cancel, err, sizeof err) : -1;
    if (res == 0) {
        double kept = 0;
        for (int k = 0; k < nf; k++) kept += (double)(fr[k][1] - fr[k][0]) / rate;
        snprintf(m->amsg, sizeof m->amsg, "%s %d Hz, %.1f s kept of %.1f s", adec_codec_name(src), m->ac.rate, kept, dur);
    } else {
        snprintf(m->amsg, sizeof m->amsg, "audio: %s", err[0] ? err : "decode failed");
        for (int k = 0; k < m->ac.nseg; k++) free(m->ac.seg[k].pcm);
        free(m->ac.seg);
        memset(&m->ac, 0, sizeof m->ac);
    }
    free(fr);
    adec_close(src);
    m->aprog = 1.0f;
    return res;
}

static int audio_worker(unsigned argc, void *argp)
{
    (void)argc; (void)argp;
    for (;;) {
        if (g_prep.cancel) break;
        mutex_lock(&g_ajob_mtx);
        int i = g_ajob_next;
        while (i < g_s.proj.nmedia) {
            MediaRt *m = &g_s.m[i];
            if (g_s.proj.media[i].used_audio && m->afile[0] && m->astate != MS_SKIP) break;
            i++;
        }
        g_ajob_next = i + 1;
        mutex_unlock(&g_ajob_mtx);
        if (i >= g_s.proj.nmedia) break;
        audio_job(i);
    }
    __atomic_sub_fetch(&g_workers_left, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static int monitor_thread(unsigned argc, void *argp)
{
    (void)argc; (void)argp;
    while (g_workers_left > 0) {
        float sum = 0;
        int cnt = 0;
        for (int i = 0; i < g_s.proj.nmedia; i++) {
            VegMedia *vm = &g_s.proj.media[i];
            if (vm->used_video && g_s.m[i].vfile[0]) { sum += g_s.m[i].vprog; cnt++; }
            if (vm->used_audio && g_s.m[i].afile[0]) { sum += g_s.m[i].aprog; cnt++; }
        }
        g_prep.progress = cnt ? sum / cnt : 1.0f;
        sleep_us(50000);
    }
    adec_global_term();
    g_prep.progress = 1.0f;
    g_prep.done = 1;
    return 0;
}

/* ------------------------------------------------------------------ start: probe, budget, allocate, spawn */

/* sources inside an official (encrypted) game/app: mount that title first. One title at a time (the first found). */
static void prepare_mount_titles(void)
{
    for (int i = 0; i < g_s.proj.nmedia; i++) {
        MediaRt *m = &g_s.m[i];
        const char *f[2] = { m->vstate != MS_SKIP ? m->vfile : "", m->astate != MS_SKIP ? m->afile : "" };
        for (int k = 0; k < 2; k++) {
            if (!f[k][0]) continue;
            int r = pfs_ensure_for(f[k]);
            if (r > 0) { LOG("prepare: sources from %s (decrypted)", pfs_mounted_title()); return; }
            if (r < 0) LOG("prepare: can't decrypt %s (0x%08X)", f[k], (unsigned)r);
        }
    }
}

int prepare_start(void)
{
    VegProject *p = &g_s.proj;
    prepare_mount_titles();
    session_free_caches();
    for (int i = 0; i < p->nmedia; i++) g_s.m[i].vc.want_q = g_cfg.small_copies;
    memset(&g_prep, 0, sizeof g_prep);
    g_prep.running = 1;
    snprintf(g_prep.line, sizeof g_prep.line, "Reading sources...");
    if (g_ajob_mtx.id < 0) mutex_init(&g_ajob_mtx, "vp_ajob");

    free(g_vjobs);
    g_vjobs = calloc((size_t)(p->nmedia ? p->nmedia : 1), sizeof(VJob));
    g_nvjobs = 0;
    int max_w = 0, max_h = 0, max_refs = 2, navc = 0;
    double need_bytes4 = 0;     /* bytes at full (screen-capped) size, RGBA */
    int max_cw[64] = { 0 };
    (void)max_cw;

    /* 1. probe the video files: frame times and which frames the events show */
    for (int i = 0; i < p->nmedia; i++) {
        VegMedia *vm = &p->media[i];
        MediaRt *m = &g_s.m[i];
        m->vprog = m->aprog = 0;
        m->vmsg[0] = m->amsg[0] = 0;
        if (!vm->used_video || !m->vfile[0] || m->vstate == MS_SKIP) continue;
        if (is_image(m->vfile)) continue;   /* pictures are loaded below, after the budget */
        VJob *vj = &g_vjobs[g_nvjobs];
        memset(vj, 0, sizeof *vj);
        vj->media = i;
        if (mp4_open(&vj->mp4, m->vfile) < 0 || !vj->mp4.video.present) {
            snprintf(m->vmsg, sizeof m->vmsg, "video: %s", vj->mp4.err[0] ? vj->mp4.err : "no H.264 or MPEG-4 video track");
            if (vj->mp4.f) mp4_close(&vj->mp4);
            continue;
        }
        vj->mp4_ok = 1;
        Mp4Track *t = &vj->mp4.video;
        if (t->codec == 0x6D703476u) { void m4v_probe(int w, int h); m4v_probe(t->width, t->height); }   /* TEMPORARY research */
        VideoCache *vc = &m->vc;
        vc->nframes = (int)t->n;
        vc->pts = malloc(sizeof(float) * t->n);
        vc->slot = malloc(sizeof(int) * t->n);
        vj->need = calloc(t->n, 1);
        if (!vc->pts || !vc->slot || !vj->need) { snprintf(m->vmsg, sizeof m->vmsg, "out of memory"); mp4_close(&vj->mp4); vj->mp4_ok = 0; continue; }
        /* presentation times in display order */
        double *ct = malloc(sizeof(double) * t->n);
        for (uint32_t k = 0; k < t->n; k++) ct[k] = (double)t->s[k].cts / t->timescale;
        for (uint32_t k = 1; k < t->n; k++) { double x = ct[k]; int j = (int)k - 1; while (j >= 0 && ct[j] > x) { ct[j + 1] = ct[j]; j--; } ct[j + 1] = x; }
        for (uint32_t k = 0; k < t->n; k++) { vc->pts[k] = (float)ct[k]; vc->slot[k] = -1; }
        double last_d = t->n > 1 ? ct[t->n - 1] - ct[t->n - 2] : 1.0 / 30;
        vc->duration = ct[t->n - 1] + last_d;
        free(ct);
        vc->src_w = t->width; vc->src_h = t->height;
        /* frames used */
        Rng *r = NULL;
        int n = 0, cap = 0;
        for (int e = 0; e < p->nevents; e++) {
            VegEvent *ev = &p->events[e];
            if (ev->kind != VEG_KIND_VIDEO || ev->media != i) continue;
            add_range(&r, &n, &cap, ev->offset * ev->rate, (ev->offset + ev->length) * ev->rate + 1e-4, vc->duration);
        }
        n = merge_ranges(r, n, 0);
        for (int q = 0; q < n; q++) {
            /* the frame on screen at r.a is the last one starting at or before it */
            int lo = 0, hi = vc->nframes - 1, first = 0;
            while (lo <= hi) { int mid = (lo + hi) / 2; if (vc->pts[mid] <= r[q].a) { first = mid; lo = mid + 1; } else hi = mid - 1; }
            for (int k = first; k < vc->nframes && vc->pts[k] < r[q].b; k++) vj->need[k] = 1;
        }
        free(r);
        for (int k = 0; k < vc->nframes; k++) vj->nneed += vj->need[k];
        /* cache size: never larger than the screen */
        double s = MIN(1.0, MIN((double)SCREEN_W / t->width, (double)SCREEN_H / t->height));
        vj->cw = MAX(16, (int)(t->width * s)); vj->ch = MAX(16, (int)(t->height * s));
        need_bytes4 += (double)vj->nneed * (((vj->cw + 7) & ~7) * vj->ch + (g_s.m[vj->media].vc.want_q ? ((QUARTER(vj->cw) + 7) & ~7) * QUARTER(vj->ch) : 0)) * 4;
        if (t->codec != 0x6D703476u) {   /* the hardware H.264 decoder is sized for the H.264 sources only */
            max_w = MAX(max_w, t->width); max_h = MAX(max_h, t->height);
            if (t->max_ref_frames + 1 > max_refs) max_refs = t->max_ref_frames + 1;
            navc++;
        }
        LOG("%s: %dx%d, %u frames, %.2f s, %d needed, refs %d", vm->name, t->width, t->height, t->n, vc->duration, vj->nneed, t->max_ref_frames);
        g_nvjobs++;
    }
    if (max_refs > 16) max_refs = 16;

    /* 2. the hardware decoder's memory first (physically contiguous), then the frame-cache arenas */
    if (navc && vdec_global_init(max_w, max_h, max_refs) < 0) {
        for (int j = 0; j < g_nvjobs; j++) {
            if (g_vjobs[j].mp4.video.codec == 0x6D703476u) continue;
            snprintf(g_s.m[g_vjobs[j].media].vmsg, sizeof g_s.m[0].vmsg, "video: hardware decoder unavailable (%dx%d, see log)", max_w, max_h);
            mp4_close(&g_vjobs[j].mp4); g_vjobs[j].mp4_ok = 0;
            g_vjobs[j].nneed = 0;
        }
    }
    size_t want = (size_t)(g_cfg.cache_mb > 0 ? g_cfg.cache_mb : 176) << 20;
    size_t images_reserve = 16u << 20;
    /* keep some RAM for everything else (thread stacks, file buffers, audio decoder work areas) */
    SceUID headroom = sceKernelAllocMemBlock("vp_headroom", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 24u << 20, NULL);
    size_t got = gpu_arena_reserve(want);
    if (headroom >= 0) sceKernelFreeMemBlock(headroom);
    double avail = (double)got - (double)images_reserve - 65536;
    if (avail < (4 << 20)) avail = 4 << 20;
    int bpp = 4;
    double scale = 1.0;
    if (need_bytes4 > avail) {
        bpp = 2;
        if (need_bytes4 / 2 > avail) scale = sqrt(avail / (need_bytes4 / 2)) * 0.97;
    }
    if (scale < 0.25) scale = 0.25;   /* below that, keep every other frame instead */
    LOG("cache: %.1f MB needed at full size, %.1f MB available -> %d bytes/pixel, scale %.2f", need_bytes4 / 1048576.0, avail / 1048576.0, bpp, scale);
    for (int j = 0; j < g_nvjobs; j++) {
        VJob *vj = &g_vjobs[j];
        VideoCache *vc = &g_s.m[vj->media].vc;
        vj->cw = MAX(16, (int)(vj->cw * scale)); vj->ch = MAX(16, (int)(vj->ch * scale));
        vc->cache_w = vj->cw; vc->cache_h = vj->ch; vc->bpp = bpp;
        size_t per = ((size_t)((vj->cw + 7) & ~7) * (size_t)vj->ch + (vc->want_q ? (size_t)((QUARTER(vj->cw) + 7) & ~7) * (size_t)QUARTER(vj->ch) : 0)) * (size_t)bpp;
        size_t left = gpu_arena_capacity() - gpu_arena_used();
        left = left > images_reserve ? left - images_reserve : 0;
        int fit = (int)(left / (per + 256));
        int keep = vj->nneed;
        if (keep > fit) {   /* still too much: thin out the needed frames evenly */
            int step = (keep + fit - 1) / MAX(fit, 1), c = 0;
            for (int k = 0; k < vc->nframes; k++) if (vj->need[k]) { if (c++ % step) vj->need[k] = 0; }
            keep = 0;
            for (int k = 0; k < vc->nframes; k++) keep += vj->need[k];
            LOG("cache: %s thinned to %d frames", g_s.proj.media[vj->media].name, keep);
        }
        vc->img = calloc((size_t)(keep ? keep : 1), sizeof(GpuImage));
        vc->imgq = vc->want_q ? calloc((size_t)(keep ? keep : 1), sizeof(GpuImage)) : NULL;
        vc->nimg = 0;
        for (int k = 0; k < keep && vc->img; k++) {
            if (gpu_image_alloc(&vc->img[k], vj->cw, vj->ch, bpp) < 0) break;
            if (vc->imgq && gpu_image_alloc(&vc->imgq[k], QUARTER(vj->cw), QUARTER(vj->ch), bpp) < 0) memset(&vc->imgq[k], 0, sizeof(GpuImage));
            vc->nimg++;
        }
    }
    /* white pixel for solid colours */
    if (gpu_image_alloc(&g_s.white, 1, 1, 4) == 0) memset(g_s.white.data, 0xFF, 32);

    /* 3. pictures (quick, on this thread) */
    for (int i = 0; i < p->nmedia; i++) {
        VegMedia *vm = &p->media[i];
        MediaRt *m = &g_s.m[i];
        if (!vm->used_video || !m->vfile[0] || m->vstate == MS_SKIP || !is_image(m->vfile)) continue;
        char err[128];
        if (image_load_to_cache(m->vfile, &m->vc, 1024, 1024, err, sizeof err) < 0) snprintf(m->vmsg, sizeof m->vmsg, "picture: %s", err);
        else snprintf(m->vmsg, sizeof m->vmsg, "picture %dx%d", m->vc.src_w, m->vc.src_h);
        m->vprog = 1.0f;
    }

    /* 4. decoders and threads */
    adec_global_init();
    g_ajob_next = 0;
    g_workers_left = 3;
    g_threads[0] = thread_start("vp_vdec", video_worker, NULL, 0x10000100, 256 * 1024, 0x10000);   /* core 0 */
    g_threads[1] = thread_start("vp_adec0", audio_worker, NULL, 0x10000100, 256 * 1024, 0x20000);  /* core 1 */
    g_threads[2] = thread_start("vp_adec1", audio_worker, NULL, 0x10000100, 256 * 1024, 0x40000);  /* core 2 */
    for (int k = 0; k < 3; k++) if (g_threads[k] < 0) __atomic_sub_fetch(&g_workers_left, 1, __ATOMIC_SEQ_CST);
    if (thread_start("vp_prepmon", monitor_thread, NULL, 0x10000100, 16 * 1024, 0) < 0) {
        while (g_workers_left > 0) sleep_us(20000);
        adec_global_term();
        g_prep.done = 1;
    }
    return 0;
}

void prepare_finish(void)
{
    for (int k = 0; k < 3; k++) { thread_join(g_threads[k]); g_threads[k] = -1; }
    for (int j = 0; j < g_nvjobs; j++) { if (g_vjobs[j].mp4_ok) mp4_close(&g_vjobs[j].mp4); free(g_vjobs[j].need); }
    free(g_vjobs);
    g_vjobs = NULL; g_nvjobs = 0;
    g_prep.running = 0;
    g_s.prepared = !g_prep.cancel;
}
