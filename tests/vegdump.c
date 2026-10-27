#include <math.h>
/* Host-side check of the .veg reader:  cc -O2 -I../src vegdump.c ../src/veg.c -lm -o vegdump */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "veg.h"

static const char *VFXN[] = { "unknown", "invert", "grayscale", "hsl", "sepia", "bricon", "chanblend", "lab", "colorcorr", "gradient", "linearblur", "radialblur", "noop", "gaussblur" };
static const char *AFXN[] = { "unknown", "pitchshift", "vibrato", "reverb", "noop" };

int main(int argc, char **argv)
{
    for (int a = 1; a < argc; a++) {
        VegProject p;
        char err[256];
        if (veg_load_file(argv[a], &p, err, sizeof err)) { printf("%s: ERROR %s\n", argv[a], err); continue; }
        printf("== %s\n  %dx%d %.3f fps, %d Hz, %.2f s, %d tracks, %d events, %d media\n", argv[a], p.width, p.height,
               p.fps, p.sample_rate, p.duration, p.ntracks, p.nevents, p.nmedia);
        for (int i = 0; i < p.nmedia; i++) {
            VegMedia *m = &p.media[i];
            printf("  media %d id=%u type=%d streams=%d v=%d a=%d '%s'  [%s]", i, m->id, m->type, m->streams,
                   m->used_video, m->used_audio, m->name, m->path);
            if (m->used_video) printf(" vrange %.2f-%.2f", m->use_lo[0], m->use_hi[0]);
            if (m->used_audio) printf(" arange %.2f-%.2f", m->use_lo[1], m->use_hi[1]);
            printf("\n");
        }
        int vfxc[16] = { 0 }, afxc[8] = { 0 }, nostr = 0, meth[4] = { 0 }, pitched = 0, panned = 0, nomedia = 0, xf = 0;
        double minpitch = 0, maxpitch = 0;
        for (int t = 0; t < p.ntracks; t++) {
            VegTrack *tr = &p.tracks[t];
            printf("  track %2d %s events=%4d%s%s", t, tr->kind == VEG_KIND_VIDEO ? "video" : tr->kind == VEG_KIND_AUDIO ? "audio" : "other",
                   tr->nevents, tr->has_motion ? " motion" : "", tr->has_comp ? " comp" : "");
            if (tr->has_comp) printf("(%u,%d)", tr->comp_a, tr->comp_b);
            if (tr->comp_mode != VEG_COMP_SRCALPHA) printf(" [%s]", veg_comp_name(tr->comp_mode));
            for (int k = 0; k < tr->nafx; k++) if (tr->afx[k].type != AFX_NOOP) printf(" [afx %s '%s' dry=%.1f wet=%.1f early=%.1f decay=%.1f pre=%.0f hicut=%.0f]", AFXN[tr->afx[k].type], tr->afx[k].name, tr->afx[k].dry_db, tr->afx[k].wet_db, tr->afx[k].early_db, tr->afx[k].decay_s, tr->afx[k].predelay_ms, tr->afx[k].hicut_hz);
            if (tr->level != 1.0f) {
                if (tr->kind == VEG_KIND_AUDIO) printf(" volume=%.2f dB", tr->level > 0 ? 20 * log10(tr->level) : -INFINITY);
                else printf(" level=%.1f%%", tr->level * 100);
            }
            if (tr->has_motion) printf(" tm=(%.3f,%.3f,%.3f,%.3f)", tr->tm_x, tr->tm_y, tr->tm_w, tr->tm_h);
            for (int q = 0; q < 2; q++) {
                const VegTmFx *fx = q ? &tr->glow : &tr->shadow;
                if (fx->on) printf(" %s=(%.3f,%.3f %.3fx%.3f rgba %.2f,%.2f,%.2f,%.2f blur %.3f)", q ? "glow" : "shadow", fx->x, fx->y, fx->w, fx->h,
                                   fx->rgba[0], fx->rgba[1], fx->rgba[2], fx->rgba[3], fx->blur);
            }
            printf("\n");
        }
        for (int i = 0; i < p.nevents; i++) {
            VegEvent *e = &p.events[i];
            if (e->kind == VEG_KIND_VIDEO && (e->fade_in > 0.001 || e->fade_out > 0.001))
                printf("  video fade: ev%d trk%d %.3f+%.3f in=%.3f(c%d) out=%.3f(c%d) xf=%.3f/%.3f\n", e->seq, e->track, e->start, e->length,
                       e->fade_in, e->fade_in_curve, e->fade_out, e->fade_out_curve, e->xfade_in, e->xfade_out);
        }
        for (int i = 0; i < p.nevents; i++) {
            VegEvent *e = &p.events[i];
            for (int k = 0; k < e->nvfx; k++) vfxc[e->vfx[k].type]++;
            for (int k = 0; k < e->nafx; k++) afxc[e->afx[k].type]++;
            if (e->kind == VEG_KIND_AUDIO) {
                if (!e->has_stretch) nostr++; else meth[e->stretch_method & 3]++;
                if (e->pitch_semis != 0) { pitched++; if (e->pitch_semis < minpitch) minpitch = e->pitch_semis; if (e->pitch_semis > maxpitch) maxpitch = e->pitch_semis; }
            }
            if (e->npan) panned++;
            if (e->media < 0) nomedia++;
            if (e->xfade_in > 0) xf++;
        }
        printf("  vfx:");
        for (int k = 1; k < 14; k++) if (vfxc[k]) printf(" %s=%d", VFXN[k], vfxc[k]);
        printf(" unknown=%d | afx:", vfxc[0]);
        for (int k = 1; k < 5; k++) if (afxc[k]) printf(" %s=%d", AFXN[k], afxc[k]);
        printf(" unknown=%d\n", afxc[0]);
        printf("  audio stretch: none-chunk=%d method0=%d method1=%d method2=%d pitched=%d (%.0f..%.0f)  pan/crop=%d  nomedia=%d crossfades=%d\n",
               nostr, meth[0], meth[1], meth[2], pitched, minpitch, maxpitch, panned, nomedia, xf);
        if (p.unknown_fx[0]) printf("  unknown fx: %s\n", p.unknown_fx);
        /* a few sample events */
        /* VD_T0 / VD_T1 (seconds): list every event starting in that range instead of a few samples */
        double vt0 = getenv("VD_T0") ? atof(getenv("VD_T0")) : -1, vt1 = getenv("VD_T1") ? atof(getenv("VD_T1")) : 1e9;
        int shown = 0;
        for (int i = 0; i < p.nevents && (vt0 >= 0 || shown < 6); i++) {
            VegEvent *e = &p.events[i];
            if (vt0 >= 0 ? (e->start + e->length < vt0 || e->start > vt1) : !(e->nvfx || e->nafx || e->pitch_semis != 0 || e->rate != 1)) continue;
            shown++;
            printf("   ev%d trk%d %s %.3f+%.3f rate=%.3f off=%.3f lvl=%.3f fade=%.3f/%.3f pitch=%.1f m=%d", e->seq, e->track,
                   e->kind ? "V" : "A", e->start, e->length, e->rate, e->offset, e->level, e->fade_in, e->fade_out, e->pitch_semis, e->stretch_method);
            for (int k = 0; k < e->nvfx; k++) printf(" [vfx %s %s amt=%.2f h=%.2f s=%.2f l=%.2f]", VFXN[e->vfx[k].type], e->vfx[k].name, e->vfx[k].amount, e->vfx[k].hue, e->vfx[k].sat, e->vfx[k].lum);
            for (int k = 0; k < e->nafx; k++) printf(" [afx %s '%s' semis=%.2f preserve=%d mode=%d]", AFXN[e->afx[k].type], e->afx[k].name, e->afx[k].semitones, e->afx[k].preserve, e->afx[k].mode);
            if (e->npan) printf(" pan[%d] %dx%d k0=(%.1f,%.1f %.1fx%.1f r%.2f) media=%d", e->npan, e->pan_w, e->pan_h, e->pan[0].cx, e->pan[0].cy, e->pan[0].w, e->pan[0].h, e->pan[0].rot, e->media);
            printf("\n");
        }
        veg_free(&p);
    }
    return 0;
}
