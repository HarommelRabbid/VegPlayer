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

/* one pass does every supported effect: Invert, Black and White, HSL Adjust, Sepia and Brightness/Contrast are
 * all affine colour transforms, folded on the CPU (in chain order) into one 3x4 matrix */
static const char FRAGMENT_SRC[] =
    "float4 main(float2 vTexcoord : TEXCOORD0, uniform sampler2D tex,\n"
    "            uniform float4 uMatR, uniform float4 uMatG, uniform float4 uMatB, uniform float4 uTint) : COLOR\n"
    "{\n"
    "    float4 c = tex2D(tex, vTexcoord);\n"
    "    float4 p = float4(c.rgb, 1.0f);\n"
    "    float3 o = float3(dot(uMatR, p), dot(uMatG, p), dot(uMatB, p));\n"
    "    return float4(saturate(o), c.a) * uTint;\n"
    "}\n";

#define SHADER_VERSION 1

static SceGxmProgram *g_vprog_gxp, *g_fprog_gxp;
static SceGxmShaderPatcherId g_vid, g_fid;
static SceGxmVertexProgram *g_vprog;
static SceGxmFragmentProgram *g_fprog[BLEND_COUNT];
static const SceGxmProgramParameter *g_wvp, *g_umat[3], *g_utint;
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

static int compile_shaders(const char *vpath, const char *fpath)
{
    int r = shark_init(NULL);   /* ur0:data/libshacccg.suprx */
    if (r < 0) {
        snprintf(g_status, sizeof g_status, "no shader compiler (ur0:data/libshacccg.suprx missing, 0x%08X)", r);
        return -1;
    }
    shark_install_log_cb(shark_log);
    shark_set_warnings_level(SHARK_WARN_LOW);
    uint32_t sz = sizeof VERTEX_SRC - 1;
    SceGxmProgram *p = shark_compile_shader(VERTEX_SRC, &sz, SHARK_VERTEX_SHADER);
    if (p) {
        g_vprog_gxp = memalign(16, sz);
        if (g_vprog_gxp) { memcpy(g_vprog_gxp, p, sz); save_gxp(vpath, p, sz); }
    }
    shark_clear_output();
    sz = sizeof FRAGMENT_SRC - 1;
    p = shark_compile_shader(FRAGMENT_SRC, &sz, SHARK_FRAGMENT_SHADER);
    if (p) {
        g_fprog_gxp = memalign(16, sz);
        if (g_fprog_gxp) { memcpy(g_fprog_gxp, p, sz); save_gxp(fpath, p, sz); }
    }
    shark_clear_output();
    shark_end();
    if (!g_vprog_gxp || !g_fprog_gxp) { snprintf(g_status, sizeof g_status, "shader compilation failed (see log)"); return -1; }
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

int gpu_init(void)
{
    ortho(g_ortho, 0, SCREEN_W, SCREEN_H, 0, 0, 1);
    mkdir_p(SHADER_DIR);
    char vpath[128], fpath[128];
    snprintf(vpath, sizeof vpath, SHADER_DIR "/layer_v%d.gxp", SHADER_VERSION);
    snprintf(fpath, sizeof fpath, SHADER_DIR "/layer_f%d.gxp", SHADER_VERSION);
    g_vprog_gxp = load_gxp(vpath);
    g_fprog_gxp = load_gxp(fpath);
    if (!g_vprog_gxp || !g_fprog_gxp) {
        free(g_vprog_gxp); free(g_fprog_gxp);
        g_vprog_gxp = g_fprog_gxp = NULL;
        if (compile_shaders(vpath, fpath) < 0) { LOG("gpu: %s - effects run on the CPU", g_status); return -1; }
        LOG("gpu: shaders compiled and cached in " SHADER_DIR);
    }
    SceGxmShaderPatcher *sp = vita2d_get_shader_patcher();
    if (sceGxmProgramCheck(g_vprog_gxp) < 0 || sceGxmProgramCheck(g_fprog_gxp) < 0 ||
        sceGxmShaderPatcherRegisterProgram(sp, g_vprog_gxp, &g_vid) < 0 ||
        sceGxmShaderPatcherRegisterProgram(sp, g_fprog_gxp, &g_fid) < 0) {
        snprintf(g_status, sizeof g_status, "shader registration failed");
        return -1;
    }
    const SceGxmProgramParameter *pos = sceGxmProgramFindParameterByName(g_vprog_gxp, "aPosition");
    const SceGxmProgramParameter *tc = sceGxmProgramFindParameterByName(g_vprog_gxp, "aTexcoord");
    g_wvp = sceGxmProgramFindParameterByName(g_vprog_gxp, "wvp");
    g_umat[0] = sceGxmProgramFindParameterByName(g_fprog_gxp, "uMatR");
    g_umat[1] = sceGxmProgramFindParameterByName(g_fprog_gxp, "uMatG");
    g_umat[2] = sceGxmProgramFindParameterByName(g_fprog_gxp, "uMatB");
    g_utint = sceGxmProgramFindParameterByName(g_fprog_gxp, "uTint");
    if (!pos || !tc || !g_wvp || !g_umat[0] || !g_umat[1] || !g_umat[2] || !g_utint) {
        snprintf(g_status, sizeof g_status, "shader parameters not found");
        return -1;
    }
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
    static const struct { int src, dst, func; } BL[BLEND_COUNT] = {
        { SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, SCE_GXM_BLEND_FUNC_ADD },   /* normal */
        { SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FUNC_ADD },                   /* add */
        { SCE_GXM_BLEND_FACTOR_SRC_ALPHA, SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT },      /* dst - src */
        { SCE_GXM_BLEND_FACTOR_DST_COLOR, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, SCE_GXM_BLEND_FUNC_ADD },   /* multiply (premultiplied src) */
        { SCE_GXM_BLEND_FACTOR_ONE, SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, SCE_GXM_BLEND_FUNC_ADD },         /* screen (premultiplied src) */
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
        if (sceGxmShaderPatcherCreateFragmentProgram(sp, g_fid, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
                                                     &bi, g_vprog_gxp, &g_fprog[i]) < 0) {
            snprintf(g_status, sizeof g_status, "fragment program creation failed (blend %d)", i);
            return -1;
        }
    }
    g_shaders_ok = 1;
    snprintf(g_status, sizeof g_status, "GPU effects shader ready");
    LOG("gpu: %s", g_status);
    return 0;
}

void gpu_fini(void)
{
    SceGxmShaderPatcher *sp = vita2d_get_shader_patcher();
    if (g_shaders_ok) {
        vita2d_wait_rendering_done();
        for (int i = 0; i < BLEND_COUNT; i++) if (g_fprog[i]) sceGxmShaderPatcherReleaseFragmentProgram(sp, g_fprog[i]);
        if (g_vprog) sceGxmShaderPatcherReleaseVertexProgram(sp, g_vprog);
        sceGxmShaderPatcherUnregisterProgram(sp, g_vid);
        sceGxmShaderPatcherUnregisterProgram(sp, g_fid);
    }
    g_shaders_ok = 0;
}

int gpu_shaders_ok(void) { return g_shaders_ok; }
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

    SceGxmContext *ctx = vita2d_get_context();
    int bl = L->blend >= 0 && L->blend < BLEND_COUNT ? L->blend : BLEND_NORMAL;
    sceGxmSetVertexProgram(ctx, g_vprog);
    sceGxmSetFragmentProgram(ctx, g_fprog[bl]);
    void *vbuf, *fbuf;
    sceGxmReserveVertexDefaultUniformBuffer(ctx, &vbuf);
    sceGxmSetUniformDataF(vbuf, g_wvp, 0, 16, g_ortho);
    sceGxmReserveFragmentDefaultUniformBuffer(ctx, &fbuf);
    sceGxmSetUniformDataF(fbuf, g_umat[0], 0, 4, L->m + 0);
    sceGxmSetUniformDataF(fbuf, g_umat[1], 0, 4, L->m + 4);
    sceGxmSetUniformDataF(fbuf, g_umat[2], 0, 4, L->m + 8);
    sceGxmSetUniformDataF(fbuf, g_utint, 0, 4, L->tint);
    sceGxmSetFragmentTexture(ctx, 0, L->tex);
    sceGxmSetVertexStream(ctx, 0, v);
    sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLE_STRIP, SCE_GXM_INDEX_FORMAT_U16, vita2d_get_linear_indices(), 4);
}
