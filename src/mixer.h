/* mixer.h - real-time audio engine. One voice per playing audio event:
 *   source read (looping, cubic interpolation) -> time-stretch / pitch -> vibrato -> level, fades, crossfades.
 * Pitch handling follows the project:
 *   method None      : pitch follows the playback rate (plain resampling)
 *   method Classic   : granular overlap-add, grain size from the Classic mode (A01..A19)
 *   method élastique : WSOLA (grains aligned by cross-correlation) - an approximation of zplane élastique
 *   Pitch Shift FX   : "resample" multiplies speed and pitch; "preserve duration" shifts pitch only (Classic). */
#ifndef MIXER_H
#define MIXER_H

#include <stdint.h>

void mixer_init(void);
void mixer_reset(void);                                   /* drop all voices (after a seek or track change) */
/* mix `frames` stereo frames starting at timeline sample `start` (OUT_RATE units) into out (interleaved float) */
void mixer_mix(float *out, int frames, int64_t start);
int mixer_active_voices(void);


/* Vita output thread: the audio clock drives the picture */
int audio_start(void);
void audio_stop(void);
void audio_play(int on);
int audio_playing(void);
void audio_seek(double seconds);
double audio_time(void);          /* timeline position of what is being heard now */

#endif
