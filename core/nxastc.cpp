// C entry to astc-encoder (third_party/astc-encoder, ARM-software 5.2.0, Apache 2.0): one context per worker thread,
// kind and block size, made on first use and freed by nx_astc_thread_done.
#include <stdint.h>
#include <stdlib.h>

#include "astcenc.h"

enum { KINDS = 3, BLOCKS = 13 };
static thread_local astcenc_context *g_ctx[KINDS][BLOCKS];

extern "C" {
// kind: 0 color, 1 color sRGB, 2 luminance+alpha normal map (input r = x, a = y). Returns 0 on success; out = blocks
// in row order.
int nx_astc_encode(int kind, int bw, int bh, const uint8_t *rgba, int w, int h, uint8_t *out, size_t out_len, float quality) {
    int bi = bw < 4 || bw > 12 || bh < 4 || bh > 12 ? -1 : bw == bh ? bw - 4 : bw == 8 ? 8 + bh - 5 : -1;   // 4x4..12x12, 8x5, 8x6
    if (kind < 0 || kind >= KINDS || bi < 0 || bi >= BLOCKS) return -1;
    astcenc_context *&ctx = g_ctx[kind][bi];
    if (!ctx) {
        astcenc_config cfg;
        astcenc_profile prof = kind == 1 ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
        unsigned flags = kind == 2 ? ASTCENC_FLG_MAP_NORMAL : 0;
        if (astcenc_config_init(prof, bw, bh, 1, quality, flags, &cfg) != ASTCENC_SUCCESS) return -2;
        if (astcenc_context_alloc(&cfg, 1, &ctx) != ASTCENC_SUCCESS) return -3;
    }
    astcenc_image img;
    void *slices[1] = {(void *)rgba};
    img.dim_x = w, img.dim_y = h, img.dim_z = 1, img.data_type = ASTCENC_TYPE_U8, img.data = slices;
    astcenc_swizzle sw = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    if (kind == 2) sw = {ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_R, ASTCENC_SWZ_A};
    astcenc_error e = astcenc_compress_image(ctx, &img, &sw, out, out_len, 0);
    astcenc_compress_reset(ctx);
    return e == ASTCENC_SUCCESS ? 0 : -4;
}
void nx_astc_thread_done(void) {
    for (int k = 0; k < KINDS; k++)
        for (int b = 0; b < BLOCKS; b++)
            if (g_ctx[k][b]) astcenc_context_free(g_ctx[k][b]), g_ctx[k][b] = nullptr;
}
}
