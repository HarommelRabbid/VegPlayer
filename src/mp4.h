/* mp4.h - minimal ISO-BMFF (MP4/MOV/M4A) demuxer: sample tables of the first video track (H.264, or MPEG-4 Part 2)
 * and the first audio track (AAC, or uncompressed PCM), enough to feed the decoders. Portable C (stdio only). */
#ifndef MP4_H
#define MP4_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint64_t offset;   /* file offset */
    uint32_t size;
    int64_t dts, cts;  /* in track timescale (cts = dts + composition offset) */
    uint8_t sync;      /* keyframe */
} Mp4Sample;

typedef struct {
    int present;
    uint32_t timescale;
    int64_t duration;          /* track duration in timescale units */
    Mp4Sample *s; uint32_t n;
    /* video (avc1/avc3) */
    int width, height;
    int nal_len;               /* NAL length field size (1..4) */
    uint8_t *sps[4]; uint16_t sps_len[4]; int nsps;
    uint8_t *pps[4]; uint16_t pps_len[4]; int npps;
    int max_ref_frames;        /* from the SPS, 0 if unknown */
    /* audio (mp4a) */
    int channels, sample_rate; /* from the AudioSpecificConfig when present */
    int aot;                   /* audio object type (2 = AAC-LC, 5 = SBR, 29 = PS) */
    int sbr;
    uint8_t asc[16]; int asc_len;
    uint32_t codec;            /* sample entry fourcc */
    /* MPEG-4 Part 2 video (mp4v): the esds DecoderSpecificInfo (VOS/VOL headers) and object type */
    uint8_t oti;
    uint8_t *dsi; int dsi_len;
    /* uncompressed PCM audio (ipcm, fpcm, sowt, twos, lpcm, in24, in32, fl32, fl64, raw): samples are whole chunks then -
     * s[i].size bytes at s[i].offset, s[i].dts = first frame of the chunk */
    int pcm_bits, pcm_le, pcm_float, pcm_unsigned;
    int pcm_bpf;               /* bytes per frame (all channels); 0 = not PCM */
} Mp4Track;

typedef struct Mp4File {
    FILE *f;
    uint64_t fsize;
    Mp4Track video, audio;
    uint32_t movie_ts;         /* mvhd timescale */
    char err[160];
} Mp4File;

int mp4_open(Mp4File *m, const char *path);   /* 0 on success */
void mp4_close(Mp4File *m);
int mp4_seek(Mp4File *m, uint64_t off);       /* position m->f (PCM chunks are read straight from the file) */
/* read sample i of track t into buf (must hold s[i].size bytes) */
int mp4_read_sample(Mp4File *m, const Mp4Track *t, uint32_t i, uint8_t *buf);
/* convert one AVCC sample (length-prefixed NALs) to an Annex-B byte stream; optionally prepend SPS/PPS.
 * Returns bytes written or -1 if out too small. */
int mp4_avcc_to_annexb(const Mp4Track *t, const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outcap, int with_params);
/* write a 7-byte ADTS header for one AAC frame of payload `len` */
void mp4_adts_header(const Mp4Track *t, uint32_t len, uint8_t hdr[7]);
/* parse an H.264 SPS (without the NAL header byte stripped: pass the whole NAL) for max_num_ref_frames */
int h264_sps_max_refs(const uint8_t *nal, int len);

#endif
