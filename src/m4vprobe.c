/* m4vprobe.c - TEMPORARY research helper for the Vita's hardware MPEG-4 Part 2 decoder (sceM4vdec), which no SDK
 * header describes. When a project with an MPEG-4 (mp4v) source is prepared, once per run:
 *  - the user-side wrapper module that exports sceM4vdec* / sceAvcdec* (SceAvcodecUser, os0:us/avcodec_us.suprx) is
 *    written to ux0:data/vegplayer/m4vdec/ for disassembly (its sceAvcdec* twins, whose structs are known, are in it);
 *  - harmless size queries are made with a range of codec type numbers and logged ("m4vprobe:" lines), to find the
 *    type the MPEG-4 decoder answers to. Queries only: no decoder is created. */
#include "common.h"

#include <stdio.h>
#include <string.h>

#include <psp2/kernel/modulemgr.h>

#define PROBE_DIR APP_DIR "/m4vdec"

/* in the SceVideodec stub; prototypes written from the AVC twins (codec type, in struct, out struct) */
int sceVideodecQueryMemSize(uint32_t type, const void *init_info, void *mem_info);
int sceM4vdecQueryDecoderMemSize(uint32_t type, const void *query, void *info);

static void dump_module(const char *want)
{
    SceUID ids[256];
    SceSize n = 256;
    if (sceKernelGetModuleList(0xFF, ids, &n) < 0) return;
    for (SceSize i = 0; i < n; i++) {
        SceKernelModuleInfo mi;
        memset(&mi, 0, sizeof mi);
        mi.size = sizeof mi;
        if (sceKernelGetModuleInfo(ids[i], &mi) < 0) continue;
        mi.module_name[sizeof mi.module_name - 1] = 0;
        if (strcmp(mi.module_name, want)) continue;
        for (int k = 0; k < 4; k++) {
            if (!mi.segments[k].memsz || !mi.segments[k].vaddr) continue;
            char path[200];
            snprintf(path, sizeof path, PROBE_DIR "/%s_seg%d_%08X.bin", want, k, (unsigned)(uintptr_t)mi.segments[k].vaddr);
            FILE *f = fopen(path, "wb");
            if (!f) continue;
            fwrite(mi.segments[k].vaddr, 1, mi.segments[k].memsz, f);
            fclose(f);
            LOG("m4vprobe: dumped %s (%s, 0x%X bytes)", path, mi.path, (unsigned)mi.segments[k].memsz);
        }
        return;
    }
    LOG("m4vprobe: module %s not loaded", want);
}

void m4v_probe(int w, int h)
{
    static int done;
    if (done) return;
    done = 1;
    /* the MPEG-4 type is 0x1002 (its decoder size query answers); find the library init struct it accepts */
    {
        uint32_t W = (uint32_t)((w + 15) & ~15), H = (uint32_t)((h + 15) & ~15);
        static const char *what[] = { "size 0x10 {w,h,streams}", "size 0x10 {w,h,refs}", "size 0x14 {w,h,0,1}", "size 0x14 {w,h,1,1}",
                                      "size 0x0C {w,h}", "size 0x18 {w,h,2,1,0}", "size 0x14 {w,h,2,1} (AVC)", "size 0x20 {w,h,2,1}" };
        uint32_t v[8][8] = { { 0x10, W, H, 1 }, { 0x10, W, H, 2 }, { 0x14, W, H, 0, 1 }, { 0x14, W, H, 1, 1 }, { 0x0C, W, H },
                             { 0x18, W, H, 2, 1, 0 }, { 0x14, W, H, 2, 1 }, { 0x20, W, H, 2, 1 } };
        for (int i = 0; i < 8; i++) {
            uint32_t mem[8] = { 0 };
            int r = sceVideodecQueryMemSize(0x1002, v[i], mem);
            LOG("m4vprobe: VideodecQueryMemSize(0x1002, %s): 0x%08X (mem %u)", what[i], (unsigned)r, (unsigned)mem[0]);
        }
        for (uint32_t refs = 0; refs < 4; refs++) {
            uint32_t q[8] = { W, H, refs }, info[8] = { 0 };
            int r = sceM4vdecQueryDecoderMemSize(0x1002, q, info);
            LOG("m4vprobe: M4vdecQueryDecoderMemSize(0x1002, %ux%u, third word %u): 0x%08X -> %u", (unsigned)W, (unsigned)H, (unsigned)refs,
                (unsigned)r, (unsigned)info[0]);
        }
        uint32_t q2[8] = { 720, 480, 2 }, info2[8] = { 0 };
        int r = sceM4vdecQueryDecoderMemSize(0x1002, q2, info2);
        LOG("m4vprobe: M4vdecQueryDecoderMemSize(0x1002, 720x480): 0x%08X -> %u", (unsigned)r, (unsigned)info2[0]);
    }
    static const uint32_t TYPES[] = { 0x1001, 0x1002 };
    for (size_t i = 0; i < sizeof TYPES / sizeof *TYPES; i++) {
        uint32_t init[8] = { 0x14, (uint32_t)((w + 15) & ~15), (uint32_t)((h + 15) & ~15), 2, 1 };   /* AVC init layout */
        uint32_t mem[8] = { 0 };
        int r1 = sceVideodecQueryMemSize(TYPES[i], init, mem);
        uint32_t q[8] = { (uint32_t)((w + 15) & ~15), (uint32_t)((h + 15) & ~15), 2 };                   /* AVC query layout */
        uint32_t info[8] = { 0 };
        int r2 = sceM4vdecQueryDecoderMemSize(TYPES[i], q, info);
        LOG("m4vprobe: type 0x%04X: VideodecQueryMemSize 0x%08X (mem %u)  M4vdecQueryDecoderMemSize 0x%08X (out %08X %08X %08X %08X)",
            (unsigned)TYPES[i], (unsigned)r1, (unsigned)mem[0], (unsigned)r2, (unsigned)info[0], (unsigned)info[1], (unsigned)info[2],
            (unsigned)info[3]);
    }
}
