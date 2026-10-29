/* mp4mux.h - a small MP4 writer for one H.264 video track and one AAC audio track.
 * Samples go straight to the file (mdat first), the index (moov) is written at the end. */
#ifndef MP4MUX_H
#define MP4MUX_H

#include <stdint.h>

typedef struct Mp4Mux Mp4Mux;

/* video: width x height, each sample `vdur` units of `vtimescale`; audio: AAC-LC, 1024 frames per sample at `arate`.
 * Returns NULL if the file can't be created. */
Mp4Mux *mux_open(const char *path, int width, int height, uint32_t vtimescale, uint32_t vdur, uint32_t arate, int channels);
/* the stream's parameter sets (needed before mux_close) */
void mux_set_avc_config(Mp4Mux *m, const uint8_t *sps, uint32_t sps_len, const uint8_t *pps, uint32_t pps_len);
/* one access unit, already length-prefixed (4-byte big-endian sizes); sync = keyframe */
int mux_video(Mp4Mux *m, const uint8_t *data, uint32_t len, int sync);
/* one raw AAC frame (no ADTS header) */
int mux_audio(Mp4Mux *m, const uint8_t *data, uint32_t len);
/* writes the index and closes; with discard the file is deleted. Returns 0 if the file was completed. */
int mux_close(Mp4Mux *m, int discard);
/* bytes written so far */
uint64_t mux_size(const Mp4Mux *m);

#endif
