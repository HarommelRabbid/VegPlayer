/* host test of the audio engine: one 440 Hz source, several events, rendered to WAV and analysed */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "media.h"
#include "mixer.h"
Session g_s;
Config g_cfg = { -1, -1, 1, 1, 0, 1, 1, 0, 1, "" };
void log_printf(const char *fmt, ...) { (void)fmt; }

int main(void)
{
    VegProject *p = &g_s.proj;
    p->ntracks = 1;
    p->tracks = calloc(1, sizeof(VegTrack));
    p->nevents = 6;
    p->events = calloc(6, sizeof(VegEvent));
    p->nmedia = 1;
    p->media = calloc(1, sizeof(VegMedia));
    g_s.m = calloc(1, sizeof(MediaRt));
    static int on[1] = { 1 };
    g_s.track_on = on;
    /* source: 3 s of 440 Hz at 44100 */
    AudioCache *ac = &g_s.m[0].ac;
    ac->ok = 1; ac->rate = 44100; ac->total = 44100 * 3; ac->nseg = 1;
    ac->seg = calloc(1, sizeof(PcmSeg));
    ac->seg[0].start = 0; ac->seg[0].count = ac->total;
    ac->seg[0].pcm = malloc(sizeof(int16_t) * 2 * ac->total);
    for (int i = 0; i < ac->total; i++) ac->seg[0].pcm[2 * i] = ac->seg[0].pcm[2 * i + 1] = (int16_t)(12000 * sin(2 * M_PI * 440 * i / 44100.0));
    /* events, 1 s each, back to back:
       0: plain  1: élastique +12  2: None rate 2 (resample)  3: élastique rate 0.5 (stretch)  4: pitch shift plugin -12 resample  5: classic +7 */
    for (int i = 0; i < 6; i++) {
        VegEvent *e = &p->events[i];
        e->track = 0; e->kind = VEG_KIND_AUDIO; e->media = 0; e->has_take = 1; e->level = 1;
        e->start = i * 1.0; e->length = 1.0; e->rate = 1.0; e->offset = 0.5; e->has_stretch = 1; e->stretch_method = VEG_STRETCH_ELASTIQUE;
    }
    p->events[1].pitch_semis = 12;
    p->events[2].stretch_method = VEG_STRETCH_NONE; p->events[2].rate = 2.0; p->events[2].offset = 0.25;
    p->events[3].rate = 0.5; p->events[3].offset = 1.0;
    static VegAfx ps = { AFX_PITCH_SHIFT, "Pitch Shift", -12, 0, 0, 0, 0 };
    p->events[4].afx = &ps; p->events[4].nafx = 1;
    p->events[5].stretch_method = VEG_STRETCH_CLASSIC; p->events[5].pitch_semis = 7; p->events[5].stretch_attr = 2;
    p->tracks[0].kind = VEG_KIND_AUDIO; p->tracks[0].level = 1; p->tracks[0].first_event = 0; p->tracks[0].nevents = 6;
    p->duration = 6;
    mixer_init();
    int total = OUT_RATE * 6;
    float *out = malloc(sizeof(float) * 2 * total);
    for (int s = 0; s < total; s += 1024) mixer_mix(out + 2 * s, total - s < 1024 ? total - s : 1024, s);
    FILE *f = fopen(getenv("OUT") ? getenv("OUT") : "mix.raw", "wb");
    fwrite(out, sizeof(float), 2 * (size_t)total, f);
    fclose(f);
    printf("rendered %d frames\n", total);
    return 0;
}
