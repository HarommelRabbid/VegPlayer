/* main.c - VegPlayer: plays a Vegas Pro .veg project in real time on the PS Vita.
 *
 * Threads:  main      UI + sceGxm submission (vita2d context), vsync'd
 *           vp_scene  builds the layer list for the current audio-clock time (core 2)
 *           vp_audio  mixer + sceAudioOut, the master clock (core 1)
 *           vp_vdec / vp_adec0 / vp_adec1   loaders while preparing (hardware H.264 / audio decoders)
 */
#include "common.h"
#include "gpu.h"
#include "media.h"
#include "mixer.h"
#include "scene.h"
#include "veg.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <vita2d.h>
#include "sharptext.h"

unsigned int _newlib_heap_size_user = 112 * 1024 * 1024;

/* ------------------------------------------------------------------ drawing helpers */

static stext_font *g_font;   /* sharptext: glyphs rendered at the size they're drawn (see sharptext.h) */
#define COL_BG      RGBA8(18, 20, 26, 255)
#define COL_PANEL   RGBA8(32, 36, 46, 255)
#define COL_SEL     RGBA8(52, 92, 160, 255)
#define COL_TEXT    RGBA8(230, 232, 238, 255)
#define COL_DIM     RGBA8(140, 146, 160, 255)
#define COL_OK      RGBA8(110, 210, 130, 255)
#define COL_WARN    RGBA8(240, 190, 90, 255)
#define COL_ERR     RGBA8(240, 110, 100, 255)
#define COL_ACCENT  RGBA8(120, 170, 255, 255)

static int is_cjk(unsigned int c) { return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x3000 && c <= 0x30FF); }

static void text(int x, int y, unsigned col, float scale, const char *s) { stext_draw_text(g_font, x, y, col, scale, s); }

/* ------------------------------------------------------------------ button symbols
 * btext() draws hint strings where {X} {O} {S} {T} {L} {R} {START} {SELECT} {UP} {DOWN} {LEFT} {RIGHT} become the
 * console's own button glyphs from sa0:data/font/pvf/psexchar.pvf (a PVF font of PlayStation symbols; character
 * map: github.com/nobodywasishere/vita_Symbols). Without that font the words are drawn instead. */
static stext_font *g_sym;
#define SYM_SCALE 1.15f   /* symbol size relative to the text around it */
#define SYM_DY    1       /* nudge down (pixels at scale 1) to sit on the text's baseline */
typedef struct { const char *tok; const char *glyph; const char *word; unsigned col; } Sym;
static const Sym SYMS[] = {
    /* filled-disc face buttons, in the PlayStation colours */
    { "{X}", "&", "X", RGBA8(124, 178, 232, 255) },
    { "{O}", "%", "O", RGBA8(255, 102, 102, 255) },
    { "{S}", "'", "SQUARE", RGBA8(255, 105, 248, 255) },
    { "{T}", "$", "TRIANGLE", RGBA8(64, 226, 160, 255) },
    { "{L}", "0", "L", 0 }, { "{R}", "1", "R", 0 },
    { "{SELECT}", "4", "SELECT", 0 }, { "{START}", "5", "START", 0 },
    { "{UP}", ",", "UP", 0 }, { "{DOWN}", "-", "DOWN", 0 }, { "{LEFT}", ".", "LEFT", 0 }, { "{RIGHT}", "/", "RIGHT", 0 },
};

static const Sym *sym_at(const char *p)
{
    if (*p != '{') return NULL;
    for (size_t i = 0; i < sizeof SYMS / sizeof SYMS[0]; i++)
        if (!strncmp(p, SYMS[i].tok, strlen(SYMS[i].tok))) return &SYMS[i];
    return NULL;
}

/* draws (draw = 1) or measures a hint string; returns its width */
static int btext_run(int draw, int x, int y, unsigned col, float scale, const char *s)
{
    char run[256];
    int pen = x;
    while (*s) {
        const Sym *k = sym_at(s);
        if (k) {
            if (g_sym) {
                float ss = scale * SYM_SCALE;
                unsigned c = k->col ? k->col : col;
                pen += draw ? stext_draw_text(g_sym, pen, y + (int)lroundf(SYM_DY * scale), c, ss, k->glyph)
                            : stext_text_width(g_sym, ss, k->glyph);
            } else {
                pen += draw ? stext_draw_text(g_font, pen, y, col, scale, k->word) : stext_text_width(g_font, scale, k->word);
            }
            s += strlen(k->tok);
            continue;
        }
        size_t n = 0;
        while (s[n] && n + 1 < sizeof run && !(s[n] == '{' && sym_at(s + n))) { run[n] = s[n]; n++; }
        run[n] = 0;
        pen += draw ? stext_draw_text(g_font, pen, y, col, scale, run) : stext_text_width(g_font, scale, run);
        s += n;
    }
    return pen - x;
}
static void btext(int x, int y, unsigned col, float scale, const char *s) { btext_run(1, x, y, col, scale, s); }
static int btext_width(float scale, const char *s) { return btext_run(0, 0, 0, 0, scale, s); }
static void textf(int x, int y, unsigned col, float scale, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void textf(int x, int y, unsigned col, float scale, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    text(x, y, col, scale, b);
}

/* shorten s to fit `maxw` pixels (keeps the end, where file names are) */
static void fit(char *out, size_t n, const char *s, int maxw, float scale)
{
    snprintf(out, n, "%s", s);
    if (stext_text_width(g_font, scale, out) <= maxw) return;
    const char *p = s;
    while (*p) {
        p++;
        while ((*p & 0xC0) == 0x80) p++;
        snprintf(out, n, "...%s", p);
        if (stext_text_width(g_font, scale, out) <= maxw) return;
    }
}

static void header(const char *title, const char *hint)
{
    vita2d_draw_rectangle(0, 0, SCREEN_W, 40, COL_PANEL);
    text(16, 28, COL_ACCENT, 1.0f, APP_NAME);
    text(130, 28, COL_TEXT, 1.0f, title);
    vita2d_draw_rectangle(0, SCREEN_H - 30, SCREEN_W, 30, COL_PANEL);
    btext(16, SCREEN_H - 9, COL_DIM, 0.8f, hint);
}

/* ------------------------------------------------------------------ input */

static SceCtrlData g_pad;
static unsigned g_held, g_pressed, g_repeat;
static uint64_t g_rep_t[32];

static void input_update(void)
{
    unsigned old = g_held;
    sceCtrlPeekBufferPositive(0, &g_pad, 1);
    g_held = g_pad.buttons;
    if (g_held & SCE_CTRL_L1) g_held |= SCE_CTRL_LTRIGGER;
    if (g_held & SCE_CTRL_R1) g_held |= SCE_CTRL_RTRIGGER;
    g_pressed = g_held & ~old;
    g_repeat = g_pressed;
    uint64_t now = time_us();
    for (int b = 0; b < 32; b++) {
        unsigned m = 1u << b;
        if (g_pressed & m) g_rep_t[b] = now + 380000;
        else if ((g_held & m) && now >= g_rep_t[b]) { g_repeat |= m; g_rep_t[b] = now + 60000; }
    }
}

static int list_nav(int *sel, int n, int page)
{
    int old = *sel;
    if (g_repeat & SCE_CTRL_UP) (*sel)--;
    if (g_repeat & SCE_CTRL_DOWN) (*sel)++;
    if (g_repeat & SCE_CTRL_LTRIGGER) *sel -= page;
    if (g_repeat & SCE_CTRL_RTRIGGER) *sel += page;
    if (*sel < 0) *sel = n > 0 && old == 0 && (g_repeat & SCE_CTRL_UP) ? n - 1 : 0;
    if (*sel >= n) *sel = n > 0 && old == n - 1 && (g_repeat & SCE_CTRL_DOWN) ? 0 : (n > 0 ? n - 1 : 0);
    return *sel != old;
}

/* ------------------------------------------------------------------ states */

enum { ST_BROWSE, ST_PROJECT, ST_TRACKS, ST_OPTIONS, ST_PREPARE, ST_PLAY };
static int g_state = ST_BROWSE, g_tracks_back = ST_PROJECT;
static char g_msg[256];
static uint64_t g_msg_until;

static void flash(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void flash(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    va_end(ap);
    g_msg_until = time_us() + 4000000;
    LOG("%s", g_msg);
}

/* ------------------------------------------------------------------ file browser */

typedef struct { char name[256]; int dir; } Entry;
static Entry *g_ent; static int g_nent, g_entcap;
static char g_cwd[512];
static int g_bsel, g_btop;
static int g_bmode;            /* 0 = open a project, 1 = pick a source */
static int g_pick_media, g_pick_audio;

static const char *DRIVES[] = { "ux0:", "uma0:", "ur0:", "imc0:", "xmc0:", "grw0:", "ud0:", "host0:", NULL };

static int wanted(const char *name)
{
    if (g_bmode == 0) return str_iends(name, ".veg") || str_iends(name, ".vf");
    int k = media_kind_of_file(name);
    return g_pick_audio ? (k & 2) != 0 : (k & 1) != 0;
}

static int cmp_ent(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

static void browse_load(const char *dir)
{
    g_nent = 0; g_bsel = 0; g_btop = 0;
    snprintf(g_cwd, sizeof g_cwd, "%s", dir);
    if (!dir[0]) {
        for (int i = 0; DRIVES[i]; i++) {
            if (!dir_exists(DRIVES[i])) continue;
            if (g_nent == g_entcap) { g_entcap = g_entcap ? g_entcap * 2 : 64; g_ent = realloc(g_ent, sizeof(Entry) * (size_t)g_entcap); }
            snprintf(g_ent[g_nent].name, sizeof g_ent[0].name, "%s", DRIVES[i]);
            g_ent[g_nent++].dir = 1;
        }
        return;
    }
    SceUID d = sceIoDopen(dir);
    if (d < 0) { flash("Can't open %s", dir); return; }
    SceIoDirent e;
    while (sceIoDread(d, &e) > 0) {
        int isdir = SCE_S_ISDIR(e.d_stat.st_mode);
        if (e.d_name[0] == '.' ) continue;
        if (!isdir && !wanted(e.d_name)) continue;
        if (g_nent == g_entcap) { g_entcap = g_entcap ? g_entcap * 2 : 64; Entry *n = realloc(g_ent, sizeof(Entry) * (size_t)g_entcap); if (!n) break; g_ent = n; }
        snprintf(g_ent[g_nent].name, sizeof g_ent[0].name, "%s", e.d_name);
        g_ent[g_nent++].dir = isdir;
    }
    sceIoDclose(d);
    qsort(g_ent, (size_t)g_nent, sizeof(Entry), cmp_ent);
}

static void browse_up(void)
{
    if (!g_cwd[0]) return;
    char *s = strrchr(g_cwd, '/');
    char prev[256] = "";
    if (s) { snprintf(prev, sizeof prev, "%s", s + 1); *s = 0; }
    else { snprintf(prev, sizeof prev, "%s", g_cwd); g_cwd[0] = 0; }
    char d[512];
    snprintf(d, sizeof d, "%s", g_cwd);
    browse_load(d);
    for (int i = 0; i < g_nent; i++) if (!strcmp(g_ent[i].name, prev)) g_bsel = i;
}

static void browse_open(int mode, int media, int audio, const char *start)
{
    g_bmode = mode; g_pick_media = media; g_pick_audio = audio;
    char d[512];
    snprintf(d, sizeof d, "%s", start && start[0] && dir_exists(start) ? start : "ux0:");
    if (d[strlen(d) - 1] == '/') d[strlen(d) - 1] = 0;
    browse_load(d);
    g_state = ST_BROWSE;
}

static void open_project(const char *path)
{
    char err[256];
    flash("Loading %s ...", veg_basename(path));
    if (session_open(path, err, sizeof err) < 0) { flash("Can't read the project: %s", err); return; }
    path_dir(path, g_cfg.last_dir, sizeof g_cfg.last_dir);
    config_save();
    g_state = ST_PROJECT;
}

static void browse_frame(void)
{
    const int rows = 17, top = 52, lh = 26;
    if (g_bsel < g_btop) g_btop = g_bsel;
    if (g_bsel >= g_btop + rows) g_btop = g_bsel - rows + 1;
    char t[600];
    if (g_bmode == 0) snprintf(t, sizeof t, "Open a Vegas project (.veg)");
    else snprintf(t, sizeof t, "Pick the %s for: %s", g_pick_audio ? "SOUND" : "PICTURE", g_s.proj.media[g_pick_media].name);
    header(t, g_bmode == 0 ? "{X} open   {O} up   {L}{R} page   {START} quit"
                           : "{X} choose   {O} up   {T} cancel   {L}{R} page");
    char cw[600];
    fit(cw, sizeof cw, g_cwd[0] ? g_cwd : "(drives)", 920, 0.8f);
    text(20, top + 6, COL_DIM, 0.8f, cw);
    for (int i = 0; i < rows && g_btop + i < g_nent; i++) {
        int k = g_btop + i, y = top + 18 + i * lh;
        if (k == g_bsel) vita2d_draw_rectangle(10, y - 2, SCREEN_W - 20, lh, COL_SEL);
        char nm[300];
        fit(nm, sizeof nm, g_ent[k].name, 860, 0.95f);
        text(24, y + 19, g_ent[k].dir ? COL_ACCENT : COL_TEXT, 0.95f, g_ent[k].dir ? "[ ]" : " - ");
        text(64, y + 19, COL_TEXT, 0.95f, nm);
    }
    if (!g_nent) text(24, top + 50, COL_DIM, 1.0f, g_bmode == 0 ? "No .veg files here" : "No matching files here");

    list_nav(&g_bsel, g_nent, rows);
    if (g_pressed & SCE_CTRL_CROSS && g_nent) {
        Entry *e = &g_ent[g_bsel];
        char p[800];
        if (!g_cwd[0]) snprintf(p, sizeof p, "%s", e->name);
        else snprintf(p, sizeof p, "%s/%s", g_cwd, e->name);
        if (e->dir) browse_load(p);
        else if (g_bmode == 0) open_project(p);
        else {
            MediaRt *m = &g_s.m[g_pick_media];
            VegMedia *vm = &g_s.proj.media[g_pick_media];
            int kind = media_kind_of_file(p);
            if (g_pick_audio) { snprintf(m->afile, sizeof m->afile, "%s", p); m->astate = MS_USER; }
            else {
                snprintf(m->vfile, sizeof m->vfile, "%s", p); m->vstate = MS_USER;
                /* a video file usually also carries the sound: use it unless the user chose something else */
                if (vm->used_audio && (kind & 2) && m->astate != MS_USER) { snprintf(m->afile, sizeof m->afile, "%s", p); m->astate = MS_USER; }
            }
            g_s.prepared = 0;
            session_save_sources();
            session_auto_find_sources();
            g_state = ST_PROJECT;
        }
    }
    if (g_pressed & SCE_CTRL_CIRCLE) browse_up();
    if (g_bmode == 1 && (g_pressed & SCE_CTRL_TRIANGLE)) g_state = ST_PROJECT;
}

/* ------------------------------------------------------------------ project / sources */

typedef struct { int media, audio; } Row;
static Row g_rows[512]; static int g_nrows, g_psel, g_ptop;

static void build_rows(void)
{
    g_nrows = 0;
    for (int i = 0; i < g_s.proj.nmedia && g_nrows < 510; i++) {
        VegMedia *vm = &g_s.proj.media[i];
        if (vm->used_video) g_rows[g_nrows++] = (Row){ i, 0 };
        if (vm->used_audio) g_rows[g_nrows++] = (Row){ i, 1 };
    }
}

static void project_frame(void)
{
    VegProject *p = &g_s.proj;
    build_rows();
    char t[300];
    snprintf(t, sizeof t, "%s", veg_basename(g_s.veg_path));
    header(t, "{X} choose file   {S} skip   {T} tracks   {SELECT} options   {START} play   {O} close");
    textf(20, 66, COL_DIM, 0.85f, "%dx%d  %.3f fps  %d Hz  %d:%05.2f   %d tracks  %d events  %d sources", p->width, p->height, p->fps,
          p->sample_rate, (int)(p->duration / 60), fmod(p->duration, 60), p->ntracks, p->nevents, p->nmedia);
    textf(20, 88, gpu_shaders_ok() ? COL_OK : COL_WARN, 0.8f, "Effects: %s", gpu_status());
    const int rows = 12, top = 100, lh = 30;
    if (g_psel >= g_nrows) g_psel = g_nrows ? g_nrows - 1 : 0;
    if (g_psel < g_ptop) g_ptop = g_psel;
    if (g_psel >= g_ptop + rows) g_ptop = g_psel - rows + 1;
    for (int r = 0; r < rows && g_ptop + r < g_nrows; r++) {
        int k = g_ptop + r, y = top + r * lh;
        Row *row = &g_rows[k];
        VegMedia *vm = &p->media[row->media];
        MediaRt *m = &g_s.m[row->media];
        if (k == g_psel) vita2d_draw_rectangle(10, y, SCREEN_W - 20, lh, COL_SEL);
        text(20, y + 21, row->audio ? COL_WARN : COL_ACCENT, 0.85f, row->audio ? "SOUND" : "PICTURE");
        char nm[300];
        fit(nm, sizeof nm, vm->name, 330, 0.85f);
        text(110, y + 21, COL_TEXT, 0.85f, nm);
        const char *file = row->audio ? m->afile : m->vfile;
        int st = row->audio ? m->astate : m->vstate;
        const char *msg = row->audio ? m->amsg : m->vmsg;
        unsigned col = COL_OK;
        char status[600];
        if (vm->type == VEG_MEDIA_SOLID) snprintf(status, sizeof status, "solid colour (built in)");
        else if (vm->type != VEG_MEDIA_FILE) { snprintf(status, sizeof status, "generator - not supported yet (skipped)"); col = COL_DIM; }
        else if (st == MS_SKIP) { snprintf(status, sizeof status, "skipped"); col = COL_DIM; }
        else if (!file[0]) { snprintf(status, sizeof status, "MISSING - press {X} to choose"); col = COL_ERR; }
        else {
            snprintf(status, sizeof status, "%s%s", st == MS_AUTO ? "(found) " : "", msg[0] ? msg : veg_basename(file));
            if (msg[0] && strstr(msg, ":")) col = strncmp(msg, "audio:", 6) && strncmp(msg, "video:", 6) && strncmp(msg, "picture:", 8) ? COL_OK : COL_ERR;
        }
        if (!file[0] && st != MS_SKIP && vm->type == VEG_MEDIA_FILE) btext(460, y + 21, col, 0.8f, status);   /* has a button symbol */
        else { fit(nm, sizeof nm, status, 470, 0.8f); text(460, y + 21, col, 0.8f, nm); }
        vita2d_draw_rectangle(460, y + 26, (float)(460 * (row->audio ? m->aprog : m->vprog)), 2, COL_ACCENT);
    }
    if (p->unknown_fx[0]) {
        char u[600];
        snprintf(u, sizeof u, "Not played: %s", p->unknown_fx);
        fit(u, sizeof u, u, 920, 0.75f);
        text(20, SCREEN_H - 40, COL_DIM, 0.75f, u);
    }

    list_nav(&g_psel, g_nrows, rows);
    if (g_nrows && (g_pressed & SCE_CTRL_CROSS)) {
        Row *row = &g_rows[g_psel];
        if (p->media[row->media].type == VEG_MEDIA_FILE) {
            MediaRt *m = &g_s.m[row->media];
            const char *cur = row->audio ? m->afile : m->vfile;
            char dir[512];
            if (cur[0]) path_dir(cur, dir, sizeof dir); else snprintf(dir, sizeof dir, "%s", g_s.veg_dir);
            browse_open(1, row->media, row->audio, dir);
        }
    }
    if (g_nrows && (g_pressed & SCE_CTRL_SQUARE)) {
        Row *row = &g_rows[g_psel];
        MediaRt *m = &g_s.m[row->media];
        int *st = row->audio ? &m->astate : &m->vstate;
        if (*st == MS_SKIP) { *st = MS_NONE; session_auto_find_sources(); }
        else *st = MS_SKIP;
        g_s.prepared = 0;
        session_save_sources();
    }
    if (g_pressed & SCE_CTRL_TRIANGLE) { g_tracks_back = ST_PROJECT; g_state = ST_TRACKS; }
    if (g_pressed & SCE_CTRL_SELECT) g_state = ST_OPTIONS;
    if (g_pressed & SCE_CTRL_START) {
        if (g_s.prepared) { g_state = ST_PLAY; scene_start(); audio_seek(0); audio_play(1); }
        else { prepare_start(); g_state = ST_PREPARE; }
    }
    if (g_pressed & SCE_CTRL_CIRCLE) { session_close(); browse_open(0, 0, 0, g_cfg.last_dir); }
}

/* ------------------------------------------------------------------ tracks */

static int g_tsel, g_ttop;

static void tracks_draw_list(int x0, int y0, int w, int rows, int interactive)
{
    VegProject *p = &g_s.proj;
    const int lh = 26;
    if (g_tsel < g_ttop) g_ttop = g_tsel;
    if (g_tsel >= g_ttop + rows) g_ttop = g_tsel - rows + 1;
    for (int r = 0; r < rows && g_ttop + r < p->ntracks; r++) {
        int t = g_ttop + r, y = y0 + r * lh;
        VegTrack *tr = &p->tracks[t];
        if (t == g_tsel && interactive) vita2d_draw_rectangle(x0, y, w, lh, COL_SEL);
        int on = g_s.track_on[t];
        vita2d_draw_rectangle(x0 + 10, y + 6, 14, 14, on ? COL_OK : RGBA8(70, 70, 80, 255));
        textf(x0 + 36, y + 19, on ? COL_TEXT : COL_DIM, 0.85f, "Track %2d   %s   %d events%s", t + 1,
              tr->kind == VEG_KIND_VIDEO ? "VIDEO" : tr->kind == VEG_KIND_AUDIO ? "AUDIO" : "other", tr->nevents,
              tr->has_motion ? "   (track motion)" : "");
    }
}

static void tracks_toggle_kind(int kind)
{
    int any_on = 0;
    for (int t = 0; t < g_s.proj.ntracks; t++) if (g_s.proj.tracks[t].kind == kind && g_s.track_on[t]) any_on = 1;
    for (int t = 0; t < g_s.proj.ntracks; t++) if (g_s.proj.tracks[t].kind == kind) g_s.track_on[t] = !any_on;
}

static void tracks_input(void)
{
    int changed = 0;
    list_nav(&g_tsel, g_s.proj.ntracks, 10);
    if (g_pressed & SCE_CTRL_CROSS) { g_s.track_on[g_tsel] = !g_s.track_on[g_tsel]; changed = 1; }
    if (g_pressed & SCE_CTRL_SQUARE) { tracks_toggle_kind(VEG_KIND_VIDEO); changed = 1; }
    if (g_pressed & SCE_CTRL_TRIANGLE) { tracks_toggle_kind(VEG_KIND_AUDIO); changed = 1; }
    if (changed) {
        session_save_sources();
        if (g_state == ST_PLAY) audio_seek(audio_time());   /* restart voices so re-enabled audio comes back mid-event */
    }
}

static void tracks_frame(void)
{
    header("Tracks (first = front)", "{X} toggle   {S} all video   {T} all audio   {O} back");
    tracks_draw_list(10, 56, SCREEN_W - 20, 17, 1);
    tracks_input();
    if (g_pressed & SCE_CTRL_CIRCLE) g_state = g_tracks_back;
}

/* ------------------------------------------------------------------ options */

static int g_osel;
static const int CACHE_OPTS[] = { 0, 64, 96, 128, 176, 224 };

static void options_frame(void)
{
    header("Options", "{LEFT}{RIGHT} change   {O} back");
    char rows[8][160];
    static const char *STR[] = { "as in the project", "None (resample)", "Classic", "elastique (WSOLA approx.)" };
    static const char *PSM[] = { "as in the project", "resample (speed + pitch)", "Classic (preserve duration)" };
    snprintf(rows[0], 160, "Audio stretch/pitch method:  %s", STR[g_cfg.force_stretch + 1]);
    snprintf(rows[1], 160, "Pitch Shift plug-in mode:  %s", PSM[g_cfg.pitch_plugin_mode + 1]);
    snprintf(rows[2], 160, "Event gain / opacity:  %s", g_cfg.apply_event_level ? "applied" : "ignored");
    snprintf(rows[3], 160, "Show FPS and stats:  %s", g_cfg.show_fps ? "on" : "off");
    snprintf(rows[4], 160, "Track compositing modes (Add, Multiply, Lighten...):  %s", g_cfg.compositing_guess ? "applied" : "ignored");
    if (g_cfg.cache_mb) snprintf(rows[5], 160, "Frame cache budget:  %d MB", g_cfg.cache_mb);
    else snprintf(rows[5], 160, "Frame cache budget:  automatic (176 MB)");
    snprintf(rows[6], 160, "Track Motion with other-shaped sources:  %s", g_cfg.tm_follow_source ? "follows the source (aligned)" : "as in Vegas");
    snprintf(rows[7], 160, "Small-layer frame copies (more memory):  %s", g_cfg.small_copies ? "on" : "off");
    for (int i = 0; i < 8; i++) {
        int y = 56 + i * 32;
        if (i == g_osel) vita2d_draw_rectangle(10, y, SCREEN_W - 20, 32, COL_SEL);
        text(24, y + 23, COL_TEXT, 0.95f, rows[i]);
    }
    text(24, 334, COL_DIM, 0.8f, "The Vegas methods are reverse-engineered: if a project sounds wrong, try forcing them here.");
    text(24, 358, COL_DIM, 0.8f, "Audio changes apply immediately; the cache budget and frame copies at the next prepare.");
    list_nav(&g_osel, 8, 8);
    int d = (g_repeat & SCE_CTRL_RIGHT) ? 1 : (g_repeat & SCE_CTRL_LEFT) ? -1 : (g_pressed & SCE_CTRL_CROSS) ? 1 : 0;
    if (d) {
        switch (g_osel) {
        case 0: g_cfg.force_stretch = (g_cfg.force_stretch + 1 + d + 4) % 4 - 1; break;
        case 1: g_cfg.pitch_plugin_mode = (g_cfg.pitch_plugin_mode + 1 + d + 3) % 3 - 1; break;
        case 2: g_cfg.apply_event_level ^= 1; break;
        case 3: g_cfg.show_fps ^= 1; break;
        case 4: g_cfg.compositing_guess ^= 1; break;
        case 5: {
            int k = 0;
            for (int i = 0; i < 6; i++) if (CACHE_OPTS[i] == g_cfg.cache_mb) k = i;
            k = (k + d + 6) % 6;
            g_cfg.cache_mb = CACHE_OPTS[k];
            g_s.prepared = 0;
            break;
        }
        case 6: g_cfg.tm_follow_source ^= 1; break;
        case 7: g_cfg.small_copies ^= 1; g_s.prepared = 0; break;
        }
        config_save();
        if (g_osel <= 2) mixer_reset();
    }
    if (g_pressed & SCE_CTRL_CIRCLE) g_state = g_s.proj.ntracks ? ST_PROJECT : ST_BROWSE;
}

/* ------------------------------------------------------------------ prepare */

static void prepare_frame(void)
{
    header("Preparing", "{O} cancel");
    text(40, 90, COL_TEXT, 1.0f, g_prep.line);
    vita2d_draw_rectangle(40, 110, SCREEN_W - 80, 18, COL_PANEL);
    vita2d_draw_rectangle(40, 110, (SCREEN_W - 80) * g_prep.progress, 18, COL_ACCENT);
    int y = 160;
    for (int i = 0; i < g_s.proj.nmedia && y < SCREEN_H - 60; i++) {
        VegMedia *vm = &g_s.proj.media[i];
        MediaRt *m = &g_s.m[i];
        if (vm->type != VEG_MEDIA_FILE || (!vm->used_video && !vm->used_audio)) continue;
        char nm[300];
        fit(nm, sizeof nm, vm->name, 400, 0.8f);
        text(40, y, COL_TEXT, 0.8f, nm);
        if (vm->used_video) {
            vita2d_draw_rectangle(460, y - 12, 200 * m->vprog, 6, COL_ACCENT);
            fit(nm, sizeof nm, m->vmsg, 280, 0.7f);
            text(670, y - 4, strncmp(m->vmsg, "video:", 6) && strncmp(m->vmsg, "picture:", 8) ? COL_DIM : COL_ERR, 0.7f, nm);
        }
        if (vm->used_audio) {
            vita2d_draw_rectangle(460, y + 2, 200 * m->aprog, 6, COL_WARN);
            fit(nm, sizeof nm, m->amsg, 280, 0.7f);
            text(670, y + 12, strncmp(m->amsg, "audio:", 6) ? COL_DIM : COL_ERR, 0.7f, nm);
        }
        y += 34;
    }
    if (g_pressed & SCE_CTRL_CIRCLE) g_prep.cancel = 1;
    if (g_prep.done) {
        prepare_finish();
        if (g_prep.cancel) { g_state = ST_PROJECT; flash("Cancelled"); return; }
        LOG("prepared: GPU cache %u KB used of %u KB", (unsigned)(gpu_arena_used() >> 10), (unsigned)(gpu_arena_capacity() >> 10));
        scene_start();
        audio_seek(0);
        audio_play(1);
        g_state = ST_PLAY;
    }
}

/* ------------------------------------------------------------------ player */

static int g_osd = 1, g_panel;
static uint64_t g_osd_until;
static double g_fps;
static int g_frames;
static uint64_t g_fps_t;

static void seek_rel(double d)
{
    double t = audio_time() + d;
    if (t < 0) t = 0;
    if (t > g_s.proj.duration) t = g_s.proj.duration;
    audio_seek(t);
    g_osd_until = time_us() + 2500000;
}

static void play_frame(void)
{
    VegProject *p = &g_s.proj;
    /* viewport: project aspect, fitted to the screen */
    float s = fminf((float)SCREEN_W / p->width, (float)SCREEN_H / p->height);
    float vw = p->width * s, vh = p->height * s, vx = (SCREEN_W - vw) / 2, vy = (SCREEN_H - vh) / 2;
    scene_set_viewport(vx, vy, vw, vh);
    const Scene *sc = scene_acquire();
    vita2d_draw_rectangle(vx, vy, vw, vh, RGBA8(0, 0, 0, 255));
    if (sc) {
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle((int)vx, (int)vy, (int)(vx + vw), (int)(vy + vh));
        for (int i = 0; i < sc->n; i++) gpu_draw_layer(&sc->layer[i]);
        vita2d_disable_clipping();
    }
    double t = audio_time();
    if (t >= p->duration && audio_playing()) { audio_play(0); audio_seek(p->duration); g_osd_until = time_us() + 60000000; }

    /* FPS over the last second */
    g_frames++;
    uint64_t now = time_us();
    if (now - g_fps_t >= 1000000) { g_fps = g_frames * 1e6 / (double)(now - g_fps_t); g_frames = 0; g_fps_t = now; }

    int show = g_osd && (!audio_playing() || now < g_osd_until || g_panel);
    if(audio_playing()) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    if (show) {
        vita2d_draw_rectangle(0, SCREEN_H - 56, SCREEN_W, 56, RGBA8(0, 0, 0, 170));
        float prog = p->duration > 0 ? (float)(t / p->duration) : 0;
        vita2d_draw_rectangle(20, SCREEN_H - 50, SCREEN_W - 40, 6, RGBA8(80, 80, 90, 255));
        vita2d_draw_rectangle(20, SCREEN_H - 50, (SCREEN_W - 40) * fminf(prog, 1), 6, COL_ACCENT);
        textf(20, SCREEN_H - 16, COL_TEXT, 0.85f, "%s  %d:%05.2f / %d:%05.2f", audio_playing() ? "PLAYING" : "PAUSED", (int)(t / 60), fmod(t, 60),
              (int)(p->duration / 60), fmod(p->duration, 60));
        {
            static const char *h = "{X} play/pause  {LEFT}{RIGHT} 1s  {L}{R} 5s  {UP}{DOWN} 10s  {T} tracks  {S} osd  {O} back";
            btext(SCREEN_W - 16 - btext_width(0.75f, h), SCREEN_H - 16, COL_DIM, 0.75f, h);
        }
    }
    if (g_cfg.show_fps) {
        char st[200];
        snprintf(st, sizeof st, "%.1f fps  layers %d", g_fps, sc ? sc->n : 0);
        if (sc && (sc->nlut || sc->nblur)) snprintf(st + strlen(st), sizeof st - strlen(st), " (gradient %d, blur %d)", sc->nlut, sc->nblur);
        if (sc && sc->cpu_fx) snprintf(st + strlen(st), sizeof st - strlen(st), "  cpu-fx %d", sc->cpu_fx);
        snprintf(st + strlen(st), sizeof st - strlen(st), "  voices %d  audio %d%%  build %u us", mixer_active_voices(), audio_load_pct(),
                 sc ? (unsigned)sc->build_us : 0);
        vita2d_draw_rectangle(0, 0, (float)(stext_text_width(g_font, 0.75f, st) + 16), 24, RGBA8(0, 0, 0, 150));
        textf(8, 18, g_fps >= 15 && audio_load_pct() < 80 ? COL_OK : COL_ERR, 0.75f, "%s", st);
    }
    if (g_panel) {
        vita2d_draw_rectangle(480, 40, 470, 400, RGBA8(10, 12, 18, 225));
        btext(496, 64, COL_ACCENT, 0.9f, "Tracks:  {X} toggle  {S} video  {T} audio");
        tracks_draw_list(484, 76, 462, 13, 1);
        tracks_input();
        if (g_pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) g_panel = 0;
        return;
    }
    if (g_pressed & (SCE_CTRL_CROSS | SCE_CTRL_START)) {
        if (!audio_playing() && t >= p->duration - 0.01) audio_seek(0);
        audio_play(!audio_playing());
        g_osd_until = now + 2500000;
    }
    if (g_repeat & SCE_CTRL_LEFT) seek_rel(-1);
    if (g_repeat & SCE_CTRL_RIGHT) seek_rel(1);
    if (g_repeat & SCE_CTRL_LTRIGGER) seek_rel(-5);
    if (g_repeat & SCE_CTRL_RTRIGGER) seek_rel(5);
    if (g_repeat & SCE_CTRL_UP) seek_rel(10);
    if (g_repeat & SCE_CTRL_DOWN) seek_rel(-10);
    if (g_pressed & SCE_CTRL_SELECT) { audio_seek(0); g_osd_until = now + 2500000; }
    if (g_pressed & SCE_CTRL_TRIANGLE) { g_panel = 1; g_pressed = 0; }
    if (g_pressed & SCE_CTRL_SQUARE) g_osd ^= 1;
    if (g_pressed & SCE_CTRL_CIRCLE) {
        audio_play(0);
        scene_stop();
        g_state = ST_PROJECT;
    }
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    log_init();
    config_load();
    LOG(APP_NAME " starting");

    vita2d_init_advanced(4 * 1024 * 1024);   /* bigger per-frame pool: many layers + text */
    vita2d_set_clear_color(COL_BG);
    vita2d_set_vblank_wait(1);
    /* PVF (vector) system fonts: sharp at every size; the PGF ones are the fallback */
    static const vita2d_system_pvf_config pvf[] = {
        { SCE_PVF_LANGUAGE_J, is_cjk },
        { SCE_PVF_DEFAULT_LANGUAGE_CODE, NULL },
    };
    static const vita2d_system_pgf_config pgf[] = {
        { SCE_FONT_LANGUAGE_JAPANESE, is_cjk },
        { SCE_FONT_LANGUAGE_DEFAULT, NULL },
    };
    g_font = stext_load_system_pvf(2, pvf);
    if (!g_font) { LOG("PVF fonts unavailable, using PGF"); g_font = stext_load_system_pgf(2, pgf); }
    if (!g_font) g_font = stext_load_default_pgf();
    g_sym = stext_load_custom_pvf("sa0:data/font/pvf/psexchar.pvf");
    if (!g_sym) LOG("psexchar.pvf not found: button names drawn as words");
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    /* shaders first: the first run compiles them (needs ur0:data/libshacccg.suprx), later runs load the .gxp */
    vita2d_start_drawing();
    vita2d_clear_screen();
    text(40, 280, COL_TEXT, 1.0f, "Preparing GPU shaders...");
    vita2d_end_drawing();
    vita2d_swap_buffers();
    gpu_init();
    audio_start();
    g_fps_t = time_us();

    browse_open(0, 0, 0, g_cfg.last_dir[0] ? g_cfg.last_dir : "ux0:");
    for (;;) {
        input_update();
        vita2d_start_drawing();
        vita2d_clear_screen();
        switch (g_state) {
        case ST_BROWSE:
            browse_frame();
            if (g_bmode == 0 && (g_pressed & SCE_CTRL_START)) goto quit;
            break;
        case ST_PROJECT: project_frame(); break;
        case ST_TRACKS: tracks_frame(); break;
        case ST_OPTIONS: options_frame(); break;
        case ST_PREPARE: prepare_frame(); break;
        case ST_PLAY: play_frame(); break;
        }
        if (time_us() < g_msg_until && g_state != ST_PLAY) {
            vita2d_draw_rectangle(0, SCREEN_H - 64, SCREEN_W, 32, RGBA8(60, 30, 30, 230));
            char m[300];
            fit(m, sizeof m, g_msg, 920, 0.85f);
            text(16, SCREEN_H - 42, COL_TEXT, 0.85f, m);
        }
        vita2d_end_drawing();
        vita2d_wait_rendering_done();   /* the layer/scratch memory of this scene is free for the builder after this */
        vita2d_swap_buffers();
    }
quit:
    audio_play(0);
    scene_stop();
    audio_stop();
    session_close();
    gpu_fini();
    if (g_sym) stext_free(g_sym);
    if (g_font) stext_free(g_font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
