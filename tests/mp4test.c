/* host test: demux an MP4, write Annex-B video + ADTS audio, print tables */
#include <stdio.h>
#include <stdlib.h>
#include "mp4.h"
int main(int argc, char **argv)
{
    Mp4File m;
    if (mp4_open(&m, argv[1])) { printf("open failed: %s\n", m.err); return 1; }
    Mp4Track *v = &m.video, *a = &m.audio;
    printf("video present=%d %dx%d ts=%u n=%u nal=%d sps=%d pps=%d refs=%d dur=%lld\n", v->present, v->width, v->height, v->timescale, v->n, v->nal_len, v->nsps, v->npps, v->max_ref_frames, (long long)v->duration);
    printf("audio present=%d ch=%d rate=%d aot=%d sbr=%d ts=%u n=%u asc=%d\n", a->present, a->channels, a->sample_rate, a->aot, a->sbr, a->timescale, a->n, a->asc_len);
    int nsync = 0;
    for (uint32_t i = 0; i < v->n; i++) nsync += v->s[i].sync;
    printf("sync=%d first cts: ", nsync);
    for (uint32_t i = 0; i < v->n && i < 8; i++) printf("%lld/%lld ", (long long)v->s[i].dts, (long long)v->s[i].cts);
    printf("\n");
    FILE *o = fopen(argv[2], "wb");
    uint8_t *buf = malloc(4 << 20), *ob = malloc(4 << 20);
    for (uint32_t i = 0; i < v->n; i++) {
        mp4_read_sample(&m, v, i, buf);
        int n = mp4_avcc_to_annexb(v, buf, v->s[i].size, ob, 4 << 20, v->s[i].sync);
        fwrite(ob, 1, (size_t)n, o);
    }
    fclose(o);
    o = fopen(argv[3], "wb");
    for (uint32_t i = 0; i < a->n; i++) {
        uint8_t h[7];
        mp4_read_sample(&m, a, i, buf);
        mp4_adts_header(a, a->s[i].size, h);
        fwrite(h, 1, 7, o); fwrite(buf, 1, a->s[i].size, o);
    }
    fclose(o);
    mp4_close(&m);
    return 0;
}
