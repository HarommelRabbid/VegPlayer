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
    mkdir_p(PROBE_DIR);
    dump_module("SceAvcodecUser");
    static const uint32_t TYPES[] = { 0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1010, 0x1011, 0x2001, 0x2002, 0x3001, 0x0001,
                                      0x0002, 0x0003, 0x0004 };
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
