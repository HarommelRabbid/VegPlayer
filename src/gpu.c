/* gpu.c - see gpu.h */
#include "gpu.h"
#include "common.h"

#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/sysmem.h>
#include <vita2d.h>
#include <vitashark.h>

/* ------------------------------------------------------------------ arenas */

typedef struct { SceUID uid; uint8_t *base; size_t size, used; } Arena;
#define MAX_ARENAS 16
static Arena g_arenas[MAX_ARENAS];
static int g_narenas;
static Mutex g_arena_mtx = { -1 };

static int arena_new(SceKernelMemBlockType type, size_t size)
{
    if (g_narenas >= MAX_ARENAS) return -1;
    size_t align = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256 * 1024 : 4096;
    size = (size + align - 1) & ~(align - 1);
    SceUID uid = sceKernelAllocMemBlock("vp_cache", type, size, NULL);
    if (uid < 0) return -1;
    void *base = NULL;
    if (sceKernelGetMemBlockBase(uid, &base) < 0 || sceGxmMapMemory(base, size, SCE_GXM_MEMORY_ATTRIB_READ | SCE_GXM_MEMORY_ATTRIB_WRITE) < 0) {
        sceKernelFreeMemBlock(uid);
        return -1;
    }
    Arena *a = &g_arenas[g_narenas++];
    a->uid = uid; a->base = base; a->size = size; a->used = 0;
    return 0;
}

size_t gpu_arena_reserve(size_t want)
{
    if (g_arena_mtx.id < 0) mutex_init(&g_arena_mtx, "vp_arena");
    size_t got = 0;
    /* CDRAM in 16 MB blocks while it lasts (leave room for vita2d), then uncached main RAM */
    const size_t chunk = 16u << 20;
    while (got < want && g_narenas < MAX_ARENAS) {
        size_t sz = MIN(chunk, want - got);
        if (arena_new(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sz) < 0) break;
        got += g_arenas[g_narenas - 1].size;
    }
    while (got < want && g_narenas < MAX_ARENAS) {
        size_t sz = MIN(chunk, want - got);
        if (arena_new(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, sz) < 0) {
            if (sz > (4u << 20) && arena_new(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4u << 20) == 0) { got += g_arenas[g_narenas - 1].size; continue; }
            break;
        }
        got += g_arenas[g_narenas - 1].size;
    }
    LOG("gpu: reserved %u KB in %d arenas (wanted %u KB)", (unsigned)(got >> 10), g_narenas, (unsigned)(want >> 10));
    return got;
}

void gpu_arena_release(void)
{
    for (int i = 0; i < g_narenas; i++) {
        sceGxmUnmapMemory(g_arenas[i].base);
        sceKernelFreeMemBlock(g_arenas[i].uid);
    }
    g_narenas = 0;
}

void *gpu_arena_alloc(size_t size, size_t align)
{
    void *r = NULL;
    mutex_lock(&g_arena_mtx);
    for (int i = 0; i < g_narenas && !r; i++) {
        Arena *a = &g_arenas[i];
        size_t off = (a->used + align - 1) & ~(align - 1);
        if (off + size <= a->size) { r = a->base + off; a->used = off + size; }
    }
    mutex_unlock(&g_arena_mtx);
    return r;
}

size_t gpu_arena_capacity(void) { size_t s = 0; for (int i = 0; i < g_narenas; i++) s += g_arenas[i].size; return s; }
size_t gpu_arena_used(void) { size_t s = 0; for (int i = 0; i < g_narenas; i++) s += g_arenas[i].used; return s; }

int gpu_image_wrap(GpuImage *img, void *data, int w, int h, int bpp)
{
    img->data = data; img->w = w; img->h = h; img->bpp = bpp;
    img->stride = (w + 7) & ~7;
    SceGxmTextureFormat fmt = bpp == 2 ? SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB : SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR;
    if (sceGxmTextureInitLinear(&img->tex, data, fmt, (unsigned)w, (unsigned)h, 0) < 0) return -1;
    sceGxmTextureSetMinFilter(&img->tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&img->tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetUAddrMode(&img->tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    sceGxmTextureSetVAddrMode(&img->tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
    return 0;
}

int gpu_image_alloc(GpuImage *img, int w, int h, int bpp)
{
    int stride = (w + 7) & ~7;
    void *d = gpu_arena_alloc((size_t)stride * (size_t)h * (size_t)bpp, 256);
    if (!d) return -1;
    return gpu_image_wrap(img, d, w, h, bpp);
}

/* ------------------------------------------------------------------ shaders */

static const char VERTEX_SRC[] =
    "void main(float3 aPosition, float2 aTexcoord, uniform float4x4 wvp,\n"
    "          float4 out vPosition : POSITION, float2 out vTexcoord : TEXCOORD0)\n"
    "{\n"
    "    vPosition = mul(float4(aPosition, 1.f), wvp);\n"
    "    vTexcoord = aTexcoord;\n"
    "}\n";

/* PASS_MATRIX: every affine colour effect (Invert, Black and White, HSL Adjust, Sepia, Brightness/Contrast,
 * Channel Blend, LAB Adjust, Color Corrector/Balance) folded on the CPU, in chain order, into one 3x4 matrix */
static const char FRAG_MATRIX_SRC[] =
    "float4 main(float2 vTexcoord : TEXCOORD0, uniform sampler2D tex,\n"
    "            uniform float4 uMatR, uniform float4 uMatG, uniform float4 uMatB, uniform float4 uTint) : COLOR\n"
    "{\n"
    "    float4 c = tex2D(tex, vTexcoord);\n"
    "    float4 p = float4(c.rgb, 1.0f);\n"
    "    float3 o = float3(dot(uMatR, p), dot(uMatG, p), dot(uMatB, p));\n"
    "    return float4(saturate(o), c.a) * uTint;\n"
    "}\n";

/* PASS_LUT: effects before the Gradient Map, the map itself (brightness -> 256x1 gradient, mixed by amount),
 * then the effects after it */
static const char FRAG_LUT_SRC[] =
    "float4 main(float2 vTexcoord : TEXCOORD0, uniform sampler2D tex, uniform sampler2D lut,\n"
    "            uniform float4 uMatR, uniform float4 uMatG, uniform float4 uMatB,\n"
    "            uniform float4 uPostR, uniform float4 uPostG, uniform float4 uPostB,\n"
    "            uniform float4 uTint, uniform float4 uLut) : COLOR\n"
    "{\n"
    "    float4 c = tex2D(tex, vTexcoord);\n"
    "    float4 p = float4(c.rgb, 1.0f);\n"
    "    float3 o = saturate(float3(dot(uMatR, p), dot(uMatG, p), dot(uMatB, p)));\n"
    "    float l = dot(o, float3(0.299f, 0.587f, 0.114f));\n"
    "    float3 g = tex2D(lut, float2(l * uLut.y + uLut.z, 0.5f)).rgb;\n"
    "    float4 q = float4(lerp(o, g, uLut.x), 1.0f);\n"
    "    o = float3(dot(uPostR, q), dot(uPostG, q), dot(uPostB, q));\n"
    "    return float4(saturate(o), c.a) * uTint;\n"
    "}\n";

/* PASS_BLUR: 17 taps along a line (Linear/Gaussian Blur: uDir.xy) or towards a centre (Radial Blur:
 * (texcoord - uDir.zw) * uRad.x), weighted by uW0..uW4 (17 used), then the colour matrix */
static const char FRAG_BLUR_SRC[] =
    "float4 main(float2 vTexcoord : TEXCOORD0, uniform sampler2D tex,\n"
    "            uniform float4 uMatR, uniform float4 uMatG, uniform float4 uMatB, uniform float4 uTint,\n"
    "            uniform float4 uDir, uniform float4 uRad, uniform float4 uW0, uniform float4 uW1, uniform float4 uW2,\n"
    "            uniform float4 uW3, uniform float4 uW4) : COLOR\n"
    "{\n"
    "    float2 d = (uDir.xy + (vTexcoord - uDir.zw) * uRad.x) * 0.125f;\n"
    "    float4 c = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
    "    c += tex2D(tex, vTexcoord - 8.0f * d) * uW0.x;\n"
    "    c += tex2D(tex, vTexcoord - 7.0f * d) * uW0.y;\n"
    "    c += tex2D(tex, vTexcoord - 6.0f * d) * uW0.z;\n"
    "    c += tex2D(tex, vTexcoord - 5.0f * d) * uW0.w;\n"
    "    c += tex2D(tex, vTexcoord - 4.0f * d) * uW1.x;\n"
    "    c += tex2D(tex, vTexcoord - 3.0f * d) * uW1.y;\n"
    "    c += tex2D(tex, vTexcoord - 2.0f * d) * uW1.z;\n"
    "    c += tex2D(tex, vTexcoord - d) * uW1.w;\n"
    "    c += tex2D(tex, vTexcoord ) * uW2.x;\n"
    "    c += tex2D(tex, vTexcoord + d) * uW2.y;\n"
    "    c += tex2D(tex, vTexcoord + 2.0f * d) * uW2.z;\n"
    "    c += tex2D(tex, vTexcoord + 3.0f * d) * uW2.w;\n"
    "    c += tex2D(tex, vTexcoord + 4.0f * d) * uW3.x;\n"
    "    c += tex2D(tex, vTexcoord + 5.0f * d) * uW3.y;\n"
    "    c += tex2D(tex, vTexcoord + 6.0f * d) * uW3.z;\n"
    "    c += tex2D(tex, vTexcoord + 7.0f * d) * uW3.w;\n"
    "    c += tex2D(tex, vTexcoord + 8.0f * d) * uW4.x;\n"
    "    float4 p = float4(c.rgb, 1.0f);\n"
    "    float3 o = float3(dot(uMatR, p), dot(uMatG, p), dot(uMatB, p));\n"
    "    return float4(saturate(o), c.a) * uTint;\n"
    "}\n";

#define SHADER_VERSION 3

typedef struct {
    const char *name, *src;
    SceGxmProgram *gxp;
    SceGxmShaderPatcherId id;
    SceGxmFragmentProgram *fp[BLEND_COUNT];
    const SceGxmProgramParameter *mat[3], *post[3], *tint, *lutp, *dir, *rad, *w[5];
    int tex_unit, lut_unit;
    int ok;
} FProg;

static FProg g_fp[PASS_COUNT] = {
    { .name = "layer", .src = FRAG_MATRIX_SRC },
    { .name = "lut", .src = FRAG_LUT_SRC },
    { .name = "blur", .src = FRAG_BLUR_SRC },
};
static SceGxmProgram *g_vprog_gxp;
static SceGxmShaderPatcherId g_vid;
static SceGxmVertexProgram *g_vprog;
static const SceGxmProgramParameter *g_wvp;
static float g_ortho[16];
static int g_shaders_ok;
static char g_status[160] = "not initialised";

static SceGxmProgram *load_gxp(const char *path)
{
    size_t len = 0;
    uint8_t *d = file_read_all(path, &len, 16);
    if (!d) return NULL;
    if (len < 32 || memcmp(d, "GXP", 3) || sceGxmProgramCheck((SceGxmProgram *)d) < 0) { free(d); return NULL; }
    return (SceGxmProgram *)d;
}

static void save_gxp(const char *path, const SceGxmProgram *p, uint32_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(p, 1, size, f);
    fclose(f);
}

static void shark_log(const char *msg, shark_log_level lvl, int line)
{
    LOG("shark[%d] line %d: %s", (int)lvl, line, msg);
}

static SceGxmProgram *compile_one(const char *src, shark_type type, const char *path)
{
    uint32_t sz = (uint32_t)strlen(src);
    SceGxmProgram *p = shark_compile_shader(src, &sz, type), *out = NULL;
    if (p) {
        out = memalign(16, sz);
        if (out) { memcpy(out, p, sz); save_gxp(path, p, sz); }
    }
    shark_clear_output();
    return out;
}

static void gxp_path(char *out, size_t n, const char *name, char kind)
{
    snprintf(out, n, SHADER_DIR "/%s_%c%d.gxp", name, kind, SHADER_VERSION);
}

/* cached .gxp first; the missing ones are compiled with vitaShaRK (ur0:data/libshacccg.suprx) */
static int load_or_compile(void)
{
    char path[160];
    gxp_path(path, sizeof path, "layer", 'v');
    g_vprog_gxp = load_gxp(path);
    int missing = !g_vprog_gxp;
    for (int i = 0; i < PASS_COUNT; i++) {
        gxp_path(path, sizeof path, g_fp[i].name, 'f');
        g_fp[i].gxp = load_gxp(path);
        if (!g_fp[i].gxp) missing = 1;
    }
    if (!missing) return 0;
    int r = shark_init(NULL);
    if (r < 0) {
        snprintf(g_status, sizeof g_status, "no shader compiler (ur0:data/libshacccg.suprx missing, 0x%08X)", r);
        return g_vprog_gxp && g_fp[PASS_MATRIX].gxp ? 0 : -1;   /* an older cache can still cover the basics */
    }
    shark_install_log_cb(shark_log);
    shark_set_warnings_level(SHARK_WARN_LOW);
    if (!g_vprog_gxp) { gxp_path(path, sizeof path, "layer", 'v'); g_vprog_gxp = compile_one(VERTEX_SRC, SHARK_VERTEX_SHADER, path); }
    for (int i = 0; i < PASS_COUNT; i++) {
        if (g_fp[i].gxp) continue;
        gxp_path(path, sizeof path, g_fp[i].name, 'f');
        g_fp[i].gxp = compile_one(g_fp[i].src, SHARK_FRAGMENT_SHADER, path);
        if (!g_fp[i].gxp) LOG("gpu: the %s shader failed to compile", g_fp[i].name);
    }
    shark_end();
    LOG("gpu: shaders compiled and cached in " SHADER_DIR);
    if (!g_vprog_gxp || !g_fp[PASS_MATRIX].gxp) { snprintf(g_status, sizeof g_status, "shader compilation failed (see log)"); return -1; }
    return 0;
}

static void ortho(float *m, float left, float right, float bottom, float top, float n, float f)
{
    memset(m, 0, 16 * sizeof(float));
    m[0x0] = 2.0f / (right - left);  m[0xC] = -(right + left) / (right - left);
    m[0x5] = 2.0f / (top - bottom);  m[0xD] = -(top + bottom) / (top - bottom);
    m[0xA] = -2.0f / (f - n);        m[0xE] = (f + n) / (f - n);
    m[0xF] = 1.0f;
}

static const SceGxmProgramParameter *par(const SceGxmProgram *p, const char *name)
{
    return sceGxmProgramFindParameterByName(p, name);
}

static int unit_of(const SceGxmProgram *p, const char *name)
{
    const SceGxmProgramParameter *q = par(p, name);
    return q ? (int)sceGxmProgramParameterGetResourceIndex(q) : -1;
}

/* register one fragment program and create its blend variants */
static int setup_fprog(FProg *f, SceGxmShaderPatcher *sp)
{
    if (!f->gxp || sceGxmProgramCheck(f->gxp) < 0 || sceGxmShaderPatcherRegisterProgram(sp, f->gxp, &f->id) < 0) return -1;
    static const char *MAT[3] = { "uMatR", "uMatG", "uMatB" }, *POST[3] = { "uPostR", "uPostG", "uPostB" }, *W[5] = { "uW0", "uW1", "uW2", "uW3", "uW4" };
    for (int k = 0; k < 3; k++) { f->mat[k] = par(f->gxp, MAT[k]); f->post[k] = par(f->gxp, POST[k]); }
    for (int k = 0; k < 5; k++) f->w[k] = par(f->gxp, W[k]);
    f->tint = par(f->gxp, "uTint"); f->lutp = par(f->gxp, "uLut"); f->dir = par(f->gxp, "uDir"); f->rad = par(f->gxp, "uRad");
    f->tex_unit = unit_of(f->gxp, "tex");
    f->lut_unit = unit_of(f->gxp, "lut");
    if (!f->mat[0] || !f->mat[1] || !f->mat[2] || !f->tint || f->tex_unit < 0) return -1;
    static const struct { int src, dst, func; } BL[BLEND_COUNT] = {
        { SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, SCE_GXM_BLEND_FUNC_ADD },   /* normal */
        { SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FUNC_ADD },                   /* add */
        { SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT },      /* dst - src */
        { SCE_GXM_BLEND_FACTOR_DST_COLOR, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, SCE_GXM_BLEND_FUNC_ADD },   /* multiply (premultiplied src) */
        { SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, SCE_GXM_BLEND_FUNC_ADD },         /* screen (premultiplied src) */
        { SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FUNC_MAX },                         /* lighten (src towards black) */
        { SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FUNC_MIN },                         /* darken (src towards white) */
        { SCE_GXM_BLEND_FACTOR_ZERO, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, SCE_GXM_BLEND_FUNC_ADD },        /* cut: punch a hole */
    };
    for (int i = 0; i < BLEND_COUNT; i++) {
        SceGxmBlendInfo bi;
        memset(&bi, 0, sizeof bi);
        bi.colorMask = SCE_GXM_COLOR_MASK_ALL;
        bi.colorFunc = (uint8_t)BL[i].func;
        bi.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
        bi.colorSrc = (uint8_t)BL[i].src;
        bi.colorDst = (uint8_t)BL[i].dst;
        bi.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
        bi.alphaDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        if (sceGxmShaderPatcherCreateFragmentProgram(sp, f->id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                                                     &bi, g_vprog_gxp, &f->fp[i]) < 0) {
            LOG("gpu: %s fragment program (blend %d) creation failed", f->name, i);
            return -1;
        }
    }
    f->ok = 1;
    return 0;
}

int gpu_init(void)
{
    ortho(g_ortho, 0, SCREEN_W, SCREEN_H, 0, 0, 1);
    mkdir_p(SHADER_DIR);
    if (load_or_compile() < 0) { LOG("gpu: %s - effects run on the CPU", g_status); return -1; }
    SceGxmShaderPatcher *sp = vita2d_get_shader_patcher();
    if (sceGxmProgramCheck(g_vprog_gxp) < 0 || sceGxmShaderPatcherRegisterProgram(sp, g_vprog_gxp, &g_vid) < 0) {
        snprintf(g_status, sizeof g_status, "shader registration failed");
        return -1;
    }
    const SceGxmProgramParameter *pos = par(g_vprog_gxp, "aPosition"), *tc = par(g_vprog_gxp, "aTexcoord");
    g_wvp = par(g_vprog_gxp, "wvp");
    if (!pos || !tc || !g_wvp) { snprintf(g_status, sizeof g_status, "shader parameters not found"); return -1; }
    SceGxmVertexAttribute attr[2];
    SceGxmVertexStream stream;
    attr[0].streamIndex = 0; attr[0].offset = 0; attr[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32; attr[0].componentCount = 3;
    attr[0].regIndex = (unsigned short)sceGxmProgramParameterGetResourceIndex(pos);
    attr[1].streamIndex = 0; attr[1].offset = 12; attr[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32; attr[1].componentCount = 2;
    attr[1].regIndex = (unsigned short)sceGxmProgramParameterGetResourceIndex(tc);
    stream.stride = sizeof(vita2d_texture_vertex);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (sceGxmShaderPatcherCreateVertexProgram(sp, g_vid, attr, 2, &stream, 1, &g_vprog) < 0) {
        snprintf(g_status, sizeof g_status, "vertex program creation failed");
        return -1;
    }
    if (setup_fprog(&g_fp[PASS_MATRIX], sp) < 0) { snprintf(g_status, sizeof g_status, "effect shader setup failed"); return -1; }
    for (int i = 1; i < PASS_COUNT; i++)
        if (setup_fprog(&g_fp[i], sp) < 0) LOG("gpu: %s shader unavailable (that effect falls back)", g_fp[i].name);
    g_shaders_ok = 1;
    snprintf(g_status, sizeof g_status, "GPU effect shaders ready (gradient map %s, blur %s)",
             g_fp[PASS_LUT].ok ? "yes" : "no", g_fp[PASS_BLUR].ok ? "yes" : "no");
    LOG("gpu: %s", g_status);
    return 0;
}

void gpu_fini(void)
{
    SceGxmShaderPatcher *sp = vita2d_get_shader_patcher();
    if (g_shaders_ok) {
        vita2d_wait_rendering_done();
        for (int k = 0; k < PASS_COUNT; k++) {
            FProg *f = &g_fp[k];
            for (int i = 0; i < BLEND_COUNT; i++) if (f->fp[i]) sceGxmShaderPatcherReleaseFragmentProgram(sp, f->fp[i]);
            if (f->gxp && f->id) sceGxmShaderPatcherUnregisterProgram(sp, f->id);
            f->ok = 0;
        }
        if (g_vprog) sceGxmShaderPatcherReleaseVertexProgram(sp, g_vprog);
        sceGxmShaderPatcherUnregisterProgram(sp, g_vid);
    }
    g_shaders_ok = 0;
}

int gpu_shaders_ok(void) { return g_shaders_ok; }
int gpu_pass_ok(int pass) { return g_shaders_ok && pass >= 0 && pass < PASS_COUNT && g_fp[pass].ok; }
const char *gpu_status(void) { return g_status; }

/* ------------------------------------------------------------------ drawing */

void gpu_draw_layer(const GpuLayer *L)
{
    vita2d_texture_vertex *v = vita2d_pool_memalign(4 * sizeof(vita2d_texture_vertex), sizeof(vita2d_texture_vertex));
    if (!v) return;
    static const float U[4] = { 0, 1, 0, 1 }, V[4] = { 0, 0, 1, 1 };
    for (int i = 0; i < 4; i++) { v[i].x = L->x[i]; v[i].y = L->y[i]; v[i].z = 0.5f; v[i].u = U[i]; v[i].v = V[i]; }

    if (!g_shaders_ok) {
        /* vita2d's own textured program: transforms, opacity and add-blend only (effects were baked on the CPU) */
        vita2d_texture t;
        memset(&t, 0, sizeof t);
        t.gxm_tex = *L->tex;
        unsigned a = (unsigned)CLAMP(L->tint[3] * 255.0f + 0.5f, 0, 255);
        unsigned r = (unsigned)CLAMP(L->tint[0] * 255.0f + 0.5f, 0, 255);
        unsigned g = (unsigned)CLAMP(L->tint[1] * 255.0f + 0.5f, 0, 255);
        unsigned b = (unsigned)CLAMP(L->tint[2] * 255.0f + 0.5f, 0, 255);
        if (L->blend == BLEND_ADD) vita2d_set_blend_mode_add(1);
        vita2d_draw_array_textured(&t, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, v, 4, RGBA8(r, g, b, a));
        if (L->blend == BLEND_ADD) vita2d_set_blend_mode_add(0);
        return;
    }

    int pass = L->pass >= 0 && L->pass < PASS_COUNT && g_fp[L->pass].ok ? L->pass : PASS_MATRIX;
    if (pass == PASS_LUT && !L->lut) pass = PASS_MATRIX;
    const FProg *f = &g_fp[pass];
    SceGxmContext *ctx = vita2d_get_context();
    int bl = L->blend >= 0 && L->blend < BLEND_COUNT ? L->blend : BLEND_NORMAL;
    sceGxmSetVertexProgram(ctx, g_vprog);
    sceGxmSetFragmentProgram(ctx, f->fp[bl]);
    void *vbuf, *fbuf;
    sceGxmReserveVertexDefaultUniformBuffer(ctx, &vbuf);
    sceGxmSetUniformDataF(vbuf, g_wvp, 0, 16, g_ortho);
    sceGxmReserveFragmentDefaultUniformBuffer(ctx, &fbuf);
    for (int k = 0; k < 3; k++) sceGxmSetUniformDataF(fbuf, f->mat[k], 0, 4, L->m + 4 * k);
    sceGxmSetUniformDataF(fbuf, f->tint, 0, 4, L->tint);
    if (pass == PASS_LUT) {
        for (int k = 0; k < 3; k++) if (f->post[k]) sceGxmSetUniformDataF(fbuf, f->post[k], 0, 4, L->m2 + 4 * k);
        if (f->lutp) sceGxmSetUniformDataF(fbuf, f->lutp, 0, 4, L->lutp);
        if (f->lut_unit >= 0) sceGxmSetFragmentTexture(ctx, (unsigned)f->lut_unit, L->lut);
    } else if (pass == PASS_BLUR) {
        if (f->dir) sceGxmSetUniformDataF(fbuf, f->dir, 0, 4, L->dir);
        if (f->rad) sceGxmSetUniformDataF(fbuf, f->rad, 0, 4, L->rad);
        for (int k = 0; k < 5; k++) if (f->w[k]) sceGxmSetUniformDataF(fbuf, f->w[k], 0, 4, L->w + 4 * k);
    }
    sceGxmSetFragmentTexture(ctx, (unsigned)f->tex_unit, L->tex);
    sceGxmSetVertexStream(ctx, 0, v);
    sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, vita2d_get_linear_indices(), 4);
}
