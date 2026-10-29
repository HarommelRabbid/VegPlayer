/* encapi.h - our declarations for the system's hardware H.264 encoder (sceAvcenc / sceVideoenc, in SceAvcodec),
 * the codec engine's memory blocks and the AAC encoder behind the MP4 recorder (SceAacencInternal).
 *
 * None of this is published. The layouts and call order were read from the system's own MP4 recorder
 * (SceLibMp4Recorder), which uses all of it, and the struct sizes and limits from the kernel side (SceAvcodec), which
 * copies and checks each struct. Field names are ours. */
#ifndef ENCAPI_H
#define ENCAPI_H

#include <stdint.h>
#include <psp2/types.h>

#define ENC_TYPE_AVC 0x1002u      /* the encoder library's codec type */

/* sceVideoenc* library set-up: the encoder part (the kernel copies 0x20 bytes, wants size 0x14 and 64..960 x 64..576) */
typedef struct { uint32_t size, width, height, n_encoders, n_unk, rsv[3]; } EncLibInit;
/* memory for sceVideoencInitLibraryWithUnmapMem (the recorder fills words 3 and 4 only) */
typedef struct { uint32_t rsv[3]; void *addr; uint32_t size; uint32_t rsv2[3]; } EncLibMem;
/* sceAvcencQueryEncoderMemSize* result: two memory blocks for the encoder and the size of one frame's stream */
typedef struct { uint32_t mem_a, mem_b, stream_size; } EncMemOut;
/* the encoder object, 0x34 bytes */
typedef struct {
    uint32_t handle;
    void *mem_a; uint32_t mem_a_size;
    void *mem_b; uint32_t mem_b_size;
    void *work;  uint32_t work_size;   /* 0x400 bytes */
    uint32_t rsv[6];
} AvcEncoder;
typedef struct { uint32_t r[6]; } EncParam;   /* 0x18 bytes: the recorder sends { 0, 0, 0, 0x1C, 0, 0 } */
/* a picture (encoder input and colour conversion), 0x30 bytes */
typedef struct {
    uint32_t size, pixelformat, pitch, width, height;
    uint8_t opt[16];               /* RGBA source: opt[1] = conversion coefficients (1 = BT.601) */
    void *buffer, *buffer2;        /* planes (YUV: buffer2 = interleaved chroma; 0 = right after the luma) */
    uint32_t flags;                /* encoder input: 1 = encode this picture */
} EncPicture;
#define ENC_PIXFMT_RGBA 0x00
#define ENC_PIXFMT_YUV420 0x20
/* an output stream slot, 0x28 bytes */
typedef struct {
    uint32_t size;
    uint32_t out[6];               /* filled by the encoder: timestamps */
    void *buf; uint32_t buf_size;  /* in: the buffer and its size; out: bytes used */
    uint8_t n_nal, pic_type, pad[2];
} EncStream;
typedef struct { uint32_t count, max; EncStream **streams; } EncStreamArray;
/* a NAL unit inside an output stream; each is preceded by a 4-byte start code in the buffer */
typedef struct { uint8_t *addr; uint32_t size; uint8_t type, pad[3]; } EncNal;

int sceVideoencQueryMemSize(uint32_t type, const void *dec, const EncLibInit *enc, uint32_t *size);
int sceVideoencInitLibraryWithUnmapMem(uint32_t type, const EncLibMem *mem, const void *dec, const EncLibInit *enc);
int sceVideoencTermLibrary(uint32_t type);
/* the "Internal" calls take the encoder's full 0x50-byte settings (see vexport.c); the plain ones take 0x34 bytes and
 * are held to 720x480 */
int sceAvcencQueryEncoderMemSizeInternal(uint32_t type, const void *full, EncMemOut *out);
int sceAvcencCreateEncoderInternal(uint32_t type, AvcEncoder *enc, const void *full);
int sceAvcencDeleteEncoder(AvcEncoder *enc);
int sceAvcencSetEncoderParameter(AvcEncoder *enc, const EncParam *p);
int sceAvcencEncode(AvcEncoder *enc, const EncPicture *in, EncStreamArray *out);
int sceAvcencEncodeStop(AvcEncoder *enc, EncStreamArray *out);
int sceAvcencGetNalUnit(AvcEncoder *enc, int index, EncNal *nal);
int sceAvcencCsc(EncPicture *dst, const EncPicture *src);

SceUID sceCodecEngineOpenUnmapMemBlock(void *addr, SceSize size);
int sceCodecEngineCloseUnmapMemBlock(SceUID uid);
uintptr_t sceCodecEngineAllocMemoryFromUnmapMemBlock(SceUID uid, SceSize size, SceSize align);
int sceCodecEngineFreeMemoryFromUnmapMemBlock(SceUID uid, uintptr_t addr);

/* AAC encoder (software, runs on the calling thread with a ~37 KB stack frame). The handle is the work memory. */
typedef struct {
    uint32_t channels, sample_rate, bitrate;
    uint32_t r0c, r10;             /* 0 */
    uint32_t r14;                  /* 1 */
    uint32_t r18;                  /* 0 */
    uint32_t r1c;                  /* 1 */
    const uint32_t *p20;           /* -> { 0, 1, 1, 0 } */
    const uint32_t *p24;           /* -> { 2, 2 } */
    uint32_t r28[8];               /* 0 */
    uint32_t r48, r4c, r50;        /* 1, 2, 1 */
    uint32_t r54[11];              /* 0 */
} AacencParam;
uint32_t sceAacencInternalGetWorkSize(int unused);
int sceAacencInternalCreate(void **handle, void *work);
int sceAacencInternalSetParam(void *handle, const AacencParam *p);
/* pcm: 16-bit interleaved, `bytes` long (the recorder sends 1024 frames); out_size: in capacity, out bytes written */
int sceAacencInternalEncode(void *handle, const void *pcm, uint32_t bytes, void *out, uint32_t *out_size, int zero);
int sceAacencInternalFlush(void *handle, void *out, uint32_t *out_size, int zero);
int sceAacencInternalDelete(void *handle);

#endif
