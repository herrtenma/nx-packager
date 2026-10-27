// See nxtex.h. Conversions:
//   BC1 (8 B / 4x4)       -> ASTC 6x6 (16 B / 6x6: 3.56 bpp), or 8x5 / 8x6 / 8x8 when 6x6 does not fit BC1's space
//                            (small textures: GOB padding)
//   BC4 unorm (8 B / 4x4) -> the same, luminance (every BC4 channel mapping reads only R)
//   BC3 (16 B / 4x4)      -> ASTC 4x4 (same block grid and size)
//   BC5 unorm (16 B)      -> ASTC 4x4, normal-map mode: x in RGB, y in A; channel mapping Green -> Alpha (every BC5
//                            mapping reads only R and G)
//   snorm BC4/BC5, 3D textures: unchanged.
// The texture data keeps its place and size inside the BNTX (ASTC needs the same or less, the rest is zero), so no
// offset in any file moves; only the BRTI fields format, block height (textureLayout bits 0-2), channel mapping and mip
// offsets change. Layout: Tegra block-linear. Containers: Yaz0 (re-compressed, header bytes 8-15 kept: the data
// alignment), SARC (rebuilt, each file keeps the alignment its old position had). A resource whose decompressed size
// grows gets its resource size table entry raised. The output RomFS keeps every file in its old order; the size table
// itself goes last (it can only be written once every file is converted).
#include "nxtex.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <TargetConditionals.h>
#include <sys/mman.h>
#include <sys/qos.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <zlib.h>

#define BCDEC_IMPLEMENTATION
#include "../third_party/bcdec.h"

int nx_astc_encode(int kind, int bw, int bh, const uint8_t *rgba, int w, int h, uint8_t *out, size_t out_len, float quality);
void nx_astc_thread_done(void);

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }

// File data (contents, decompressed and converted copies) is in bnew / bdel buffers: a big one is its own mapping, so
// freeing it gives the memory back at once (malloc keeps freed large blocks dirty for reuse: ~300 MB on DLC Pack 1).
// g_live counts their bytes: the in-flight cap is on it (a file's working set is several times its size).
enum { BMAP = 1 << 20 };
static _Atomic size_t g_live;
static void *bnew(size_t n, int zero) {
    uint8_t *h;
    if (n < BMAP) {
        if (!(h = zero ? calloc(n + 16, 1) : malloc(n + 16))) return NULL;
    } else if ((h = mmap(NULL, n + 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)) == MAP_FAILED) return NULL;
    memcpy(h, &n, sizeof n);
    g_live += n;
    return h + 16;
}
static void bdel(void *p) {
    if (!p) return;
    uint8_t *h = (uint8_t *)p - 16;
    size_t n;
    memcpy(&n, h, sizeof n);
    g_live -= n;
    if (n < BMAP) free(h);
    else munmap(h, n + 16);
}

// ---- Yaz0 -------------------------------------------------------------------------------------------------------
static uint8_t *yaz0_dec(const uint8_t *s, size_t n, size_t *out_n) {
    if (n < 16 || memcmp(s, "Yaz0", 4)) return NULL;
    size_t dn = be32(s + 4), si = 16, di = 0;
    uint8_t *d = bnew(dn, 0);
    if (!d) return NULL;
    while (di < dn && si < n) {
        uint8_t code = s[si++];
        for (int b = 0; b < 8 && di < dn; b++, code <<= 1) {
            if (code & 0x80) {
                if (si >= n) goto bad;
                d[di++] = s[si++];
            } else {
                if (si + 2 > n) goto bad;
                size_t dist = ((s[si] & 15) << 8 | s[si + 1]) + 1, len = s[si] >> 4;
                si += 2;
                if (!len) {
                    if (si >= n) goto bad;
                    len = s[si++] + 0x12;
                } else len += 2;
                if (dist > di) goto bad;
                for (size_t k = 0; k < len && di < dn; k++, di++) d[di] = d[di - dist];
            }
        }
    }
    if (di != dn) goto bad;
    *out_n = dn;
    return d;
bad:
    bdel(d);
    return NULL;
}

// Hash chains over the 4 KB window, one step of lazy matching. hdr8 = the original header's bytes 8-15.
enum { YW = 4096, YMAX = 0x111, YHASH = 1 << 15, YSTEPS = 48 };
static uint32_t yhash(const uint8_t *p) { return ((uint32_t)p[0] << 16 | p[1] << 8 | p[2]) * 2654435761u >> 17; }
typedef struct { int32_t head[YHASH]; int64_t prev[YW]; } YState;
static size_t ymatch(const YState *y, const uint8_t *s, size_t n, size_t p, size_t *dist) {
    if (p + 3 > n) return 0;
    size_t best = 0, maxl = n - p < YMAX ? n - p : YMAX;
    int64_t c = y->head[yhash(s + p)];
    int64_t last = (int64_t)p;
    for (int st = 0; c >= 0 && c < last && (int64_t)p - c <= YW && st < YSTEPS; st++) {
        if (s[c + best] == s[p + best] && s[c] == s[p]) {
            size_t l = 0;
            while (l < maxl && s[c + l] == s[p + l]) l++;
            if (l > best) {
                best = l, *dist = p - (size_t)c;
                if (l == maxl) break;
            }
        }
        last = c;
        c = y->prev[c & (YW - 1)];
    }
    return best >= 3 ? best : 0;
}
static void yinsert(YState *y, const uint8_t *s, size_t n, size_t p) {
    if (p + 3 > n) return;
    uint32_t h = yhash(s + p);
    y->prev[p & (YW - 1)] = y->head[h];
    y->head[h] = (int32_t)p;
}
static uint8_t *yaz0_enc(const uint8_t *s, size_t n, const uint8_t *hdr8, size_t *on) {
    uint8_t *o = bnew(16 + n + n / 8 + 16, 0);
    YState *y = malloc(sizeof *y);
    if (!o || !y) { bdel(o), free(y); return NULL; }
    for (int i = 0; i < YHASH; i++) y->head[i] = -1;
    memcpy(o, "Yaz0", 4), put_be32(o + 4, (uint32_t)n), memcpy(o + 8, hdr8, 8);
    size_t oi = 16, p = 0;
    while (p < n) {
        size_t cp = oi++;
        uint8_t code = 0;
        for (int b = 0; b < 8 && p < n; b++) {
            size_t d = 0, l = ymatch(y, s, n, p, &d);
            if (l && l < YMAX) {   // lazy: a longer match one byte later wins
                size_t d2 = 0;
                yinsert(y, s, n, p);
                size_t l2 = ymatch(y, s, n, p + 1, &d2);
                if (l2 > l + 1) l = 0;
                else {
                    if (l >= 0x12) o[oi++] = (uint8_t)((d - 1) >> 8), o[oi++] = (uint8_t)(d - 1), o[oi++] = (uint8_t)(l - 0x12);
                    else o[oi++] = (uint8_t)((l - 2) << 4 | (d - 1) >> 8), o[oi++] = (uint8_t)(d - 1);
                    for (size_t k = 1; k < l; k++) yinsert(y, s, n, p + k);
                    p += l;
                    continue;
                }
                code |= 0x80 >> b;
                o[oi++] = s[p++];
                continue;
            }
            if (l) {
                o[oi++] = (uint8_t)((d - 1) >> 8), o[oi++] = (uint8_t)(d - 1), o[oi++] = (uint8_t)(l - 0x12);
                for (size_t k = 0; k < l; k++) yinsert(y, s, n, p + k);
                p += l;
                continue;
            }
            code |= 0x80 >> b;
            yinsert(y, s, n, p);
            o[oi++] = s[p++];
        }
        o[cp] = code;
    }
    free(y);
    *on = oi;
    return o;
}

// ---- block-linear (Tegra X1) ----------------------------------------------------------------------------------------
// Block height of a mip level: halved while the level is at most half as tall (in GOBs of 8 rows).
static int mip_gob_height(int gh, uint32_t height_blocks) {
    while (gh > 1 && height_blocks <= (uint32_t)(gh >> 1) * 8) gh >>= 1;
    return gh;
}
// Bytes of one surface (one level of one layer): 64-byte-wide GOB columns, rows padded to whole blocks of gh GOBs.
static uint64_t surface_size(uint32_t wb, uint32_t hb, int bpb, int gh) {
    uint64_t width_gobs = ((uint64_t)wb * bpb + 63) / 64, rows = (hb + 8 * gh - 1) / (8 * gh);
    return rows * width_gobs * 512 * gh;
}
static uint64_t bl_addr(uint32_t x_bytes, uint32_t y, uint64_t width_gobs, int gh) {
    return (uint64_t)(y / (8 * gh)) * 512 * gh * width_gobs + (uint64_t)((y % (8 * gh)) / 8) * 512 + ((y % 8) / 2) * 64 +
           (y % 2) * 16 + (uint64_t)(x_bytes / 64) * 512 * gh + ((x_bytes % 64) / 32) * 256 + ((x_bytes % 32) / 16) * 32 +
           (x_bytes % 16);
}
static void deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, int bpb, int gh) {
    uint64_t wg = ((uint64_t)wb * bpb + 63) / 64;
    for (uint32_t y = 0; y < hb; y++)
        for (uint32_t x = 0; x < wb; x++) memcpy(dst + ((uint64_t)y * wb + x) * bpb, src + bl_addr(x * bpb, y, wg, gh), bpb);
}
static void swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, int bpb, int gh) {
    uint64_t wg = ((uint64_t)wb * bpb + 63) / 64;
    for (uint32_t y = 0; y < hb; y++)
        for (uint32_t x = 0; x < wb; x++) memcpy(dst + bl_addr(x * bpb, y, wg, gh), src + ((uint64_t)y * wb + x) * bpb, bpb);
}
// Level 0 of a block-linear surface to linear blocks (core/nxart.m); 0 = ok, -1 = src_len too short.
int nx_bl_deswizzle(const uint8_t *src, size_t src_len, uint8_t *dst, uint32_t wb, uint32_t hb, int bpb, int gh) {
    gh = mip_gob_height(gh, hb);
    if (surface_size(wb, hb, bpb, gh) > src_len) return -1;
    deswizzle(src, dst, wb, hb, bpb, gh);
    return 0;
}
// Bytes of one layer (all levels), and the offset of each level in it.
static uint64_t layer_layout(uint32_t w, uint32_t h, int levels, int bw, int bh, int bpb, int gh, uint64_t *level_off) {
    uint64_t off = 0;
    for (int l = 0; l < levels; l++) {
        uint32_t mw = w >> l ? w >> l : 1, mh = h >> l ? h >> l : 1, wb = (mw + bw - 1) / bw, hb = (mh + bh - 1) / bh;
        if (level_off) level_off[l] = off;
        off += surface_size(wb, hb, bpb, mip_gob_height(gh, hb));
    }
    return off;
}

// ---- conversion state -----------------------------------------------------------------------------------------------
typedef struct { char *name; uint64_t old_n, new_n; } Note;   // a resource that changed size (for the RSTB)
typedef struct {
    pthread_mutex_t lock;
    NxTexStats *st;
    float quality;
    Note *notes;
    size_t nnotes, cap;
} Conv;

// RSTB name: the resource as loaded (Yaz0 'sbfres' -> 'bfres')
static void rstb_note(Conv *cv, const char *name, int yaz0, size_t old_n, size_t new_n) {
    if (old_n == new_n || !name[0]) return;
    char nm[512];
    snprintf(nm, sizeof nm, "%s", name);
    char *dot = strrchr(nm, '.');
    if (yaz0 && dot && dot[1] == 's') memmove(dot + 1, dot + 2, strlen(dot + 2) + 1);
    pthread_mutex_lock(&cv->lock);
    if (cv->nnotes == cv->cap) cv->notes = realloc(cv->notes, (cv->cap = cv->cap ? cv->cap * 2 : 256) * sizeof *cv->notes);
    cv->notes[cv->nnotes++] = (Note){strdup(nm), old_n, new_n};
    pthread_mutex_unlock(&cv->lock);
}

// ---- BNTX -----------------------------------------------------------------------------------------------------------
// BRTI: 0x16 mips, 0x1c format (type << 8 | variant), 0x24 w, 0x28 h, 0x2c depth, 0x30 layers, 0x34 layout (bits 0-2:
// log2 block height in GOBs), 0x50 data size, 0x58 channel mapping (bytes r g b a: 0 zero, 1 one, 2-5 R G B A),
// 0x70 -> mip offsets (u64, absolute in the BNTX).
// Converts one texture in place; 1 = converted.
static int brti_convert(uint8_t *b, size_t n, uint8_t *r, Conv *cv) {
    uint32_t fmt, size, layout;
    uint16_t mips;
    int32_t w, h, depth, layers;
    int64_t ptrs;
    memcpy(&mips, r + 0x16, 2), memcpy(&fmt, r + 0x1c, 4), memcpy(&w, r + 0x24, 4), memcpy(&h, r + 0x28, 4);
    memcpy(&depth, r + 0x2c, 4), memcpy(&layers, r + 0x30, 4), memcpy(&layout, r + 0x34, 4), memcpy(&size, r + 0x50, 4);
    memcpy(&ptrs, r + 0x70, 8);
    int type = fmt >> 8 & 255, var = fmt & 255, which, kind, dbw, sbpb;
    if (type == 0x1a && (var == 1 || var == 6)) which = 0, kind = var == 6, dbw = 6, sbpb = 8;
    else if (type == 0x1d && var == 1) which = 1, kind = 0, dbw = 6, sbpb = 8;
    else if (type == 0x1c && (var == 1 || var == 6)) which = 2, kind = var == 6, dbw = 4, sbpb = 16;
    else if (type == 0x1e && var == 1) which = 3, kind = 2, dbw = 4, sbpb = 16;
    else return 0;
    if (depth > 1 || w <= 0 || h <= 0 || mips < 1 || mips > 16 || ptrs < 0 || ptrs + 8 * (int64_t)mips > (int64_t)n) goto skipped;
    if (layers < 1) layers = 1;
    int64_t base;
    memcpy(&base, b + ptrs, 8);
    if (base < 0 || base + (int64_t)size > (int64_t)n) goto skipped;
    int sgh = 1 << (layout & 7);
    uint64_t soff[16], doff[16];
    uint64_t slayer = layer_layout(w, h, mips, 4, 4, sbpb, sgh, soff);
    if (layers > 1) slayer = (slayer + 512ull * sgh - 1) / (512ull * sgh) * (512ull * sgh);
    if (slayer * layers > size) goto skipped;
    // Footprint: the best one that fits the texture's space (8-byte BC blocks: 6x6, else 8x5 / 8x6 / 8x8; a GOB row
    // holds half as many 16-byte blocks, so up to ~256x256 6x6 needs ~10% more than BC1). Block height: the one with
    // the least padding (the BNTX carries it).
    static const int foot[4][3] = {{6, 6, 0x31}, {8, 5, 0x32}, {8, 6, 0x33}, {8, 8, 0x34}};   // w, h, BNTX type
    int dbh = dbw, ntype = 0x2d, dgh = 0;
    uint64_t dlayer = ~0ull;
    for (int fi = 0; fi < (dbw == 4 ? 1 : 4); fi++) {
        int fw = dbw == 4 ? 4 : foot[fi][0], fh = dbw == 4 ? 4 : foot[fi][1];
        for (int g = 16; g >= 1; g >>= 1) {
            uint64_t s = layer_layout(w, h, mips, fw, fh, 16, g, NULL);
            if (layers > 1) s = (s + 512ull * g - 1) / (512ull * g) * (512ull * g);
            if (s < dlayer || (fi && dlayer * layers > size)) dlayer = s, dgh = g, dbw = fw, dbh = fh, ntype = dbw == 4 && fh == 4 ? 0x2d : foot[fi][2];
        }
        if (dlayer * layers <= size) break;
    }
    layer_layout(w, h, mips, dbw, dbh, 16, dgh, doff);
    if (dlayer * layers > size) goto skipped;

    uint8_t *data = b + base, *out = bnew(size, 1);
    uint8_t *lin = bnew((size_t)((w + 3) / 4) * ((h + 3) / 4) * 16, 0), *rgba = bnew((size_t)(w + 3) * (h + 3) * 4, 0);
    uint8_t *astc = bnew((size_t)((w + 3) / 4 + 1) * ((h + 3) / 4 + 1) * 16, 0);   // 4x4 is the most blocks
    if (!out || !lin || !rgba || !astc) { bdel(out), bdel(lin), bdel(rgba), bdel(astc); goto skipped; }
    for (int L = 0; L < layers; L++)
        for (int l = 0; l < mips; l++) {
            uint32_t mw = w >> l ? w >> l : 1, mh = h >> l ? h >> l : 1;
            uint32_t wb = (mw + 3) / 4, hb = (mh + 3) / 4, pw = wb * 4;
            deswizzle(data + L * slayer + soff[l], lin, wb, hb, sbpb, mip_gob_height(sgh, hb));
            // decode into a block-padded RGBA image (pw wide), then hand astcenc the real mw x mh part
            uint8_t blk[64];
            for (uint32_t by = 0; by < hb; by++)
                for (uint32_t bx = 0; bx < wb; bx++) {
                    const uint8_t *s = lin + ((size_t)by * wb + bx) * sbpb;
                    if (which == 0) bcdec_bc1(s, blk, 16);
                    else if (which == 2) bcdec_bc3(s, blk, 16);
                    else if (which == 1) {
                        uint8_t v[16];
                        bcdec_bc4(s, v, 4);
                        for (int k = 0; k < 16; k++) blk[4 * k] = blk[4 * k + 1] = blk[4 * k + 2] = v[k], blk[4 * k + 3] = 255;
                    } else {
                        uint8_t rg[32];
                        bcdec_bc5(s, rg, 8);
                        for (int k = 0; k < 16; k++)
                            blk[4 * k] = blk[4 * k + 1] = blk[4 * k + 2] = rg[2 * k], blk[4 * k + 3] = rg[2 * k + 1];
                    }
                    for (int yy = 0; yy < 4; yy++) memcpy(rgba + (((size_t)by * 4 + yy) * pw + bx * 4) * 4, blk + yy * 16, 16);
                }
            if (pw != mw)   // rows of mw texels
                for (uint32_t y = 1; y < mh; y++) memmove(rgba + (size_t)y * mw * 4, rgba + (size_t)y * pw * 4, (size_t)mw * 4);
            uint32_t dwb = (mw + dbw - 1) / dbw, dhb = (mh + dbh - 1) / dbh;
            if (nx_astc_encode(kind, dbw, dbh, rgba, mw, mh, astc, (size_t)dwb * dhb * 16, cv->quality)) {
                bdel(out), bdel(lin), bdel(rgba), bdel(astc);
                goto skipped;
            }
            swizzle(astc, out + L * dlayer + doff[l], dwb, dhb, 16, mip_gob_height(dgh, dhb));
        }
    memcpy(data, out, size);
    bdel(out), bdel(lin), bdel(rgba), bdel(astc);
    uint32_t nf = (uint32_t)ntype << 8 | (which == 0 || which == 2 ? (uint32_t)var : 1);
    memcpy(r + 0x1c, &nf, 4);
    int lg = 0;
    while ((1 << lg) < dgh) lg++;
    layout = (layout & ~7u) | (uint32_t)lg;
    memcpy(r + 0x34, &layout, 4);
    for (int l = 0; l < mips; l++) {
        int64_t v = base + (int64_t)doff[l];
        memcpy(b + ptrs + 8 * l, &v, 8);
    }
    if (which == 3)
        for (int c = 0; c < 4; c++)
            if (r[0x58 + c] == 3) r[0x58 + c] = 5;   // Green -> Alpha
    pthread_mutex_lock(&cv->lock);
    cv->st->textures[which]++, cv->st->bytes[which] += size;
    pthread_mutex_unlock(&cv->lock);
    return 1;
skipped:
    pthread_mutex_lock(&cv->lock);
    cv->st->skipped++;
    pthread_mutex_unlock(&cv->lock);
    return 0;
}

static int bntx_convert(uint8_t *b, size_t n, Conv *cv) {
    uint32_t cnt;
    int64_t info;
    memcpy(&cnt, b + 0x24, 4), memcpy(&info, b + 0x28, 8);
    int done = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        int64_t p;
        if (info < 0 || info + 8 * (i + 1) > (int64_t)n) break;
        memcpy(&p, b + info + 8 * i, 8);
        if (p < 0 || p + 0xa0 > (int64_t)n || memcmp(b + p, "BRTI", 4)) break;
        done += brti_convert(b, n, b + p, cv);
    }
    return done;
}

// ---- containers ---------------------------------------------------------------------------------------------------
// Takes ownership of buf; returns the (possibly new) buffer, *changed set when anything was converted.
static uint8_t *process(uint8_t *buf, size_t *pn, const char *name, int depth, Conv *cv, int *changed) {
    size_t n = *pn;
    *changed = 0;
    if (depth > 8 || n < 16) return buf;
    if (!memcmp(buf, "Yaz0", 4)) {
        size_t dn;
        uint8_t *d = yaz0_dec(buf, n, &dn);
        if (!d) return buf;
        size_t dn2 = dn;
        int ch;
        d = process(d, &dn2, name, depth + 1, cv, &ch);
        if (!ch) { bdel(d); return buf; }
        size_t en;
        uint8_t *e = yaz0_enc(d, dn2, buf + 8, &en);
        bdel(d);
        if (!e) return buf;
        rstb_note(cv, name, 1, dn, dn2);
        bdel(buf);
        *pn = en, *changed = 1;
        return e;
    }
    if (!memcmp(buf, "SARC", 4) && n >= 0x20) {
        uint16_t hl, count;
        uint32_t doff;
        memcpy(&hl, buf + 4, 2), memcpy(&doff, buf + 12, 4);
        if (hl + 12u > n) return buf;
        memcpy(&count, buf + hl + 6, 2);
        uint8_t *nodes = buf + hl + 12, *names = nodes + 16 * count + 8;
        if ((size_t)(nodes - buf) + 16ull * count > n || doff > n) return buf;
        uint8_t **nb = calloc(count ? count : 1, sizeof *nb);
        size_t *nn = calloc(count ? count : 1, sizeof *nn);
        int any = 0;
        for (int i = 0; i < count; i++) {
            uint32_t attr, s, e;
            memcpy(&attr, nodes + 16 * i + 4, 4), memcpy(&s, nodes + 16 * i + 8, 4), memcpy(&e, nodes + 16 * i + 12, 4);
            if (s > e || doff + (uint64_t)e > n) continue;
            char nm[512] = "";
            if (attr >> 24 && names + (attr & 0xffffff) * 4 < buf + n)
                snprintf(nm, sizeof nm, "%s", (const char *)names + (attr & 0xffffff) * 4);
            size_t en = e - s;
            uint8_t *eb = bnew(en, 0);
            memcpy(eb, buf + doff + s, en);
            int ch;
            eb = process(eb, &en, nm, depth + 1, cv, &ch);
            if (ch) {
                nb[i] = eb, nn[i] = en, any = 1;
                if (memcmp(eb, "Yaz0", 4)) rstb_note(cv, nm, 0, e - s, en);
            } else bdel(eb);
        }
        if (!any) { free(nb), free(nn); return buf; }
        // rebuild: files in their old order, each at the alignment its old position had (max 0x2000)
        int *ord = malloc(sizeof(int) * (count ? count : 1));
        for (int i = 0; i < count; i++) ord[i] = i;
        for (int i = 1; i < count; i++)   // insertion sort by old start
            for (int k = i; k > 0; k--) {
                uint32_t a, c;
                memcpy(&a, nodes + 16 * ord[k - 1] + 8, 4), memcpy(&c, nodes + 16 * ord[k] + 8, 4);
                if (a <= c) break;
                int t = ord[k]; ord[k] = ord[k - 1], ord[k - 1] = t;
            }
        size_t cap = doff + 0x2000;
        for (int i = 0; i < count; i++) {
            uint32_t s, e;
            memcpy(&s, nodes + 16 * i + 8, 4), memcpy(&e, nodes + 16 * i + 12, 4);
            cap += (nb[i] ? nn[i] : e - s) + 0x2000;
        }
        uint8_t *o = bnew(cap, 1);
        memcpy(o, buf, doff);
        uint8_t *onodes = o + hl + 12;
        size_t pos = doff;
        for (int k = 0; k < count; k++) {
            int i = ord[k];
            uint32_t s, e;
            memcpy(&s, nodes + 16 * i + 8, 4), memcpy(&e, nodes + 16 * i + 12, 4);
            uint64_t abs_old = doff + (uint64_t)s, al = abs_old ? abs_old & -abs_old : 0x2000;
            if (al > 0x2000) al = 0x2000;
            pos = (pos + al - 1) & ~(al - 1);
            size_t len = nb[i] ? nn[i] : e - s;
            memcpy(o + pos, nb[i] ? nb[i] : buf + doff + s, len);
            uint32_t ns = (uint32_t)(pos - doff), ne = (uint32_t)(pos - doff + len);
            memcpy(onodes + 16 * i + 8, &ns, 4), memcpy(onodes + 16 * i + 12, &ne, 4);
            pos += len;
            bdel(nb[i]);
        }
        uint32_t total = (uint32_t)pos;
        memcpy(o + 8, &total, 4);
        free(nb), free(nn), free(ord), bdel(buf);
        *pn = pos, *changed = 1;
        return o;
    }
    // FRES / BNTX / anything else: embedded BNTX
    int done = 0;
    for (size_t o = 0; o + 0x40 <= n; o += 8)
        if (!memcmp(buf + o, "BNTX", 4) && !memcmp(buf + o + 0x20, "NX  ", 4)) {
            uint32_t sz;
            memcpy(&sz, buf + o + 0x1c, 4);
            size_t len = sz && o + sz <= n ? sz : n - o;
            done += bntx_convert(buf + o, len, cv);
            if (sz) o += (sz - 8) & ~7ull;
        }
    *changed = done > 0;
    return buf;
}

// RSTB (little-endian): "RSTB", u32 crc entries, u32 name entries; {u32 crc32, u32 size}...; {char[128], u32 size}...
// Each grown resource's entry is raised by its growth (rounded up to 32).
static uint8_t *rstb_patch(uint8_t *yaz, size_t *pn, Conv *cv) {
    size_t n;
    uint8_t *t = yaz0_dec(yaz, *pn, &n);
    if (!t || n < 12 || memcmp(t, "RSTB", 4)) { bdel(t); return NULL; }
    uint32_t ncrc, nname;
    memcpy(&ncrc, t + 4, 4), memcpy(&nname, t + 8, 4);
    if (12 + 8ull * ncrc + 132ull * nname > n) { bdel(t); return NULL; }
    uint8_t *crcs = t + 12, *nms = crcs + 8ull * ncrc;
    for (size_t k = 0; k < cv->nnotes; k++) {
        const Note *e = &cv->notes[k];
        if (e->new_n <= e->old_n) continue;
        uint32_t grow = (uint32_t)((e->new_n - e->old_n + 31) & ~31ull);
        uint8_t *val = NULL;
        for (uint32_t i = 0; i < nname && !val; i++)
            if (!strncmp((char *)nms + 132ull * i, e->name, 128)) val = nms + 132ull * i + 128;
        uint32_t crc = (uint32_t)crc32(0, (const Bytef *)e->name, (uInt)strlen(e->name));
        for (uint32_t i = 0; i < ncrc && !val; i++)
            if (!memcmp(crcs + 8ull * i, &crc, 4)) val = crcs + 8ull * i + 4;
        if (!val) { cv->st->rstb_missing++; continue; }
        uint32_t v;
        memcpy(&v, val, 4), v += grow, memcpy(val, &v, 4);
        cv->st->rstb_raised++;
    }
    size_t en;
    uint8_t *o = yaz0_enc(t, n, yaz + 8, &en);
    bdel(t);
    if (o) *pn = en;
    return o;
}

// ---- the RomFS pass -------------------------------------------------------------------------------------------------
typedef struct { const char *path; uint64_t off, size, meta_at; } RFile;
typedef struct {
    NxStorage *in;
    const NxTexOptions *o;
    Conv cv;
    uint64_t h[10];
    uint8_t *tab[4];   // dir hash, dir meta, file hash, file meta
    RFile *files;
    int nfiles, *ord;  // ord: file indices in data order
    int rstb;          // file index of the size table, -1 none
    pthread_mutex_t rlock;   // picking and reading (in data order)
    pthread_mutex_t mlock;   // results, memory in flight
    pthread_cond_t cond;
    int next_read;
    uint64_t inflight, cap;
    uint8_t **out;
    size_t *outn;
    char *ready;
    int failed;
    char err[256];
} Pass;

static int rd(NxStorage *in, uint64_t off, void *p, size_t n) { return nx_storage_read(in, off, p, n); }

static void set_failed(Pass *p, const char *msg) {
    pthread_mutex_lock(&p->mlock);
    if (!p->failed) p->failed = 1, snprintf(p->err, sizeof p->err, "%s", msg);
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->mlock);
}

static int candidate(const uint8_t *b, uint64_t n) {
    return n >= 16 && (!memcmp(b, "Yaz0", 4) || !memcmp(b, "SARC", 4) || !memcmp(b, "FRES", 4) || !memcmp(b, "BNTX", 4));
}

static void *worker(void *arg) {
    Pass *p = arg;
#if TARGET_OS_IPHONE
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // keeps the workers on the performance cores
#endif
    for (;;) {
        pthread_mutex_lock(&p->rlock);
        pthread_mutex_lock(&p->mlock);
        int k = p->next_read;
        while (!p->failed && k < p->nfiles && p->inflight && g_live + p->files[p->ord[k]].size > p->cap)
            pthread_cond_wait(&p->cond, &p->mlock);
        if (p->failed || k >= p->nfiles) {
            pthread_mutex_unlock(&p->mlock), pthread_mutex_unlock(&p->rlock);
            nx_astc_thread_done();
            return NULL;
        }
        const RFile *f = &p->files[p->ord[k]];
        p->next_read++, p->inflight += f->size;
        pthread_mutex_unlock(&p->mlock);
        uint8_t *b = bnew(f->size, 0);
        int bad = !b || rd(p->in, p->h[9] + f->off, b, f->size);
        if (!bad && p->o->frontier)   // every byte below the next file's start is read now
            p->o->frontier(p->o->ctx, k + 1 < p->nfiles ? p->h[9] + p->files[p->ord[k + 1]].off : p->h[9] + nx_storage_size(p->in));
        pthread_mutex_unlock(&p->rlock);
        if (bad) {
            bdel(b);
            set_failed(p, "The game's data couldn't be read (the dump may be damaged or was removed).");
            nx_astc_thread_done();
            return NULL;
        }
        size_t n = f->size;
        if (p->o->convert && p->ord[k] != p->rstb && candidate(b, n)) {
            int ch;
            b = process(b, &n, f->path, 0, &p->cv, &ch);
            if (!memcmp(b, "SARC", 4) && ch) rstb_note(&p->cv, f->path, 0, f->size, n);
            if (ch) {
                pthread_mutex_lock(&p->cv.lock);
                p->cv.st->containers_changed++;
                pthread_mutex_unlock(&p->cv.lock);
            }
        }
        pthread_mutex_lock(&p->mlock);
        p->out[k] = b, p->outn[k] = n, p->ready[k] = 1;
        pthread_cond_broadcast(&p->cond);
        pthread_mutex_unlock(&p->mlock);
    }
}

static int by_offset(void *ctx, const void *a, const void *b) {
    const RFile *f = ctx;
    uint64_t x = f[*(const int *)a].off, y = f[*(const int *)b].off;
    return x < y ? -1 : x > y;
}

static int wr(int fd, const void *p, size_t n, uint64_t off) {
    for (size_t done = 0; done < n;) {
        ssize_t w = pwrite(fd, (const uint8_t *)p + done, n - done, (off_t)(off + done));
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return 0;
}

int nx_romfs_write(NxStorage *in, const char *out_path, const NxTexOptions *o, NxTexStats *stats, char *err, size_t errlen) {
    NxTexStats dummy;
    if (!stats) stats = &dummy;
    memset(stats, 0, sizeof *stats);
    Pass p;
    memset(&p, 0, sizeof p);
    p.in = in, p.o = o, p.rstb = -1;
    p.cv.st = stats, p.cv.quality = o->quality > 0 ? o->quality : 10.0f;
    pthread_mutex_init(&p.cv.lock, NULL), pthread_mutex_init(&p.rlock, NULL), pthread_mutex_init(&p.mlock, NULL);
    pthread_cond_init(&p.cond, NULL);
    int rc = -1, fd = -1;
    char part[2048];
    snprintf(part, sizeof part, "%s.part", out_path);
    uint8_t *head = NULL;
    // header and tables
    if (rd(in, 0, p.h, sizeof p.h) || p.h[0] != 0x50) { snprintf(err, errlen, "The game's data isn't a RomFS."); goto done; }
    for (int i = 0; i < 4; i++) {
        uint64_t off = p.h[1 + 2 * i], sz = p.h[2 + 2 * i];
        if (sz > (256u << 20) || !(p.tab[i] = malloc(sz ? sz : 1)) || rd(in, off, p.tab[i], sz)) {
            snprintf(err, errlen, "The game's data couldn't be read.");
            goto done;
        }
    }
    {   // the file list with paths (dir meta: parent u32, sibling, child, file, hash, name length u32 at 20, name at 24)
        uint64_t dms = p.h[4], fms = p.h[8];
        int cap = 1024;
        p.files = malloc(sizeof(RFile) * cap);
        for (uint64_t q = 0; q + 0x20 <= fms;) {
            uint32_t parent, nl;
            uint64_t off, size;
            memcpy(&parent, p.tab[3] + q, 4), memcpy(&off, p.tab[3] + q + 8, 8), memcpy(&size, p.tab[3] + q + 16, 8);
            memcpy(&nl, p.tab[3] + q + 28, 4);
            if (q + 0x20 + nl > fms) break;
            char path[1024] = "", tmp[1024];
            for (uint32_t d = parent; d && d + 0x18 <= dms;) {
                uint32_t up, dl;
                memcpy(&up, p.tab[1] + d, 4), memcpy(&dl, p.tab[1] + d + 20, 4);
                snprintf(tmp, sizeof tmp, "%.*s%s%s", (int)dl, p.tab[1] + d + 24, path[0] ? "/" : "", path);
                memcpy(path, tmp, sizeof path);
                if (up == d) break;
                d = up;
            }
            if (p.nfiles == cap) p.files = realloc(p.files, sizeof(RFile) * (cap *= 2));
            snprintf(tmp, sizeof tmp, "%s%s%.*s", path, path[0] ? "/" : "", (int)nl, p.tab[3] + q + 32);
            if (o->rstb && !strcmp(tmp, o->rstb)) p.rstb = p.nfiles;
            p.files[p.nfiles++] = (RFile){strdup(tmp), off, size, q};
            q += (0x20 + nl + 3) & ~3ull;
        }
    }
    stats->files = (uint64_t)p.nfiles;
    p.ord = malloc(sizeof(int) * (p.nfiles ? p.nfiles : 1));
    for (int i = 0; i < p.nfiles; i++) p.ord[i] = i;
    qsort_r(p.ord, p.nfiles, sizeof *p.ord, p.files, by_offset);
    p.out = calloc(p.nfiles ? p.nfiles : 1, sizeof *p.out);
    p.outn = calloc(p.nfiles ? p.nfiles : 1, sizeof *p.outn);
    p.ready = calloc(p.nfiles ? p.nfiles : 1, 1);
    p.cap = o->max_inflight ? o->max_inflight : 256ull << 20;

    fd = open(part, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) { snprintf(err, errlen, "The game data file couldn't be created (is there enough free space?)."); goto done; }
    uint64_t data = p.h[9];
    head = calloc(data ? data : 1, 1);
    if (!head || rd(in, 0, head, data) || wr(fd, head, data, 0)) { snprintf(err, errlen, "The game data couldn't be written."); goto done; }

    int nt = o->threads;
    if (nt <= 0) {
        size_t len = sizeof nt;
#if TARGET_OS_IPHONE
        // the phone's efficiency cores are many times slower at ASTC encoding than its performance cores, and the
        // output is written in file order: a big file on an E-core held the whole build (A15: 2 P + 4 E)
        if (sysctlbyname("hw.perflevel0.logicalcpu", &nt, &len, NULL, 0) || nt <= 0)
#endif
        if (sysctlbyname("hw.ncpu", &nt, &len, NULL, 0) || nt <= 0) nt = 4;
    }
    if (!o->convert) nt = 1;
    if (nt > 32) nt = 32;
    pthread_t th[32];
    for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, &p);
    // this thread writes, in data order
    uint64_t pos = 0, done_bytes = 0, total = nx_storage_size(in);
    for (int k = 0; k < p.nfiles; k++) {
        pthread_mutex_lock(&p.mlock);
        while (!p.ready[k] && !p.failed) pthread_cond_wait(&p.cond, &p.mlock);
        int ready = p.ready[k];
        pthread_mutex_unlock(&p.mlock);
        if (!ready) break;
        const RFile *f = &p.files[p.ord[k]];
        if (p.ord[k] == p.rstb) {   // written last (after every file's growth is known); keep it
            done_bytes += f->size;
            continue;
        }
        static const uint8_t z[16];
        uint64_t al = (pos + 15) & ~15ull;
        if ((al > pos && wr(fd, z, al - pos, data + pos)) || wr(fd, p.out[k], p.outn[k], data + al)) {
            set_failed(&p, "The game data couldn't be written (is there enough free space?).");
            break;
        }
        pos = al;
        uint64_t sz = p.outn[k];
        memcpy(p.tab[3] + f->meta_at + 8, &pos, 8), memcpy(p.tab[3] + f->meta_at + 16, &sz, 8);
        pos += sz, done_bytes += f->size;
        pthread_mutex_lock(&p.mlock);
        bdel(p.out[k]), p.out[k] = NULL, p.inflight -= f->size;
        pthread_cond_broadcast(&p.cond);
        pthread_mutex_unlock(&p.mlock);
        if (o->progress) o->progress(o->ctx, done_bytes, total);
    }
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    if (p.failed) {
        snprintf(err, errlen, "%s", p.err);
        goto done;
    }
    if (p.rstb >= 0) {   // the size table, raised for every resource that grew, at the end of the data
        int k = 0;
        while (p.ord[k] != p.rstb) k++;
        const RFile *f = &p.files[p.rstb];
        size_t n = p.outn[k];
        uint8_t *t = o->convert ? rstb_patch(p.out[k], &n, &p.cv) : p.out[k];
        if (!t) { snprintf(err, errlen, "The game's resource size table isn't one this packager understands."); goto done; }
        if (t != p.out[k]) bdel(p.out[k]);
        p.out[k] = t;
        uint64_t al = (pos + 15) & ~15ull, sz = n;
        static const uint8_t z[16];
        if ((al > pos && wr(fd, z, al - pos, data + pos)) || wr(fd, t, n, data + al)) {
            snprintf(err, errlen, "The game data couldn't be written (is there enough free space?).");
            goto done;
        }
        memcpy(p.tab[3] + f->meta_at + 8, &al, 8), memcpy(p.tab[3] + f->meta_at + 16, &sz, 8);
        pos = al + n;
    }
    {   // tables after the data, as in the original: dir hash, dir meta, file hash, file meta
        uint64_t at = data + pos;
        for (int i = 0; i < 4; i++) {
            uint64_t al = (at + 3) & ~3ull;
            static const uint8_t z[4];
            if (al > at && wr(fd, z, al - at, at)) goto wfail;
            at = al;
            p.h[1 + 2 * i] = at;
            if (wr(fd, p.tab[i], p.h[2 + 2 * i], at)) goto wfail;
            at += p.h[2 + 2 * i];
        }
        if (wr(fd, p.h, sizeof p.h, 0) || fsync(fd)) goto wfail;
        stats->out_size = at;
    }
    if (close(fd)) { fd = -1; goto wfail; }
    fd = -1;
    if (rename(part, out_path)) goto wfail;
    rc = 0;
    goto done;
wfail:
    snprintf(err, errlen, "The game data couldn't be written (is there enough free space?).");
done:
    if (fd >= 0) close(fd);
    if (rc) unlink(part);
    free(head);
    for (int i = 0; i < 4; i++) free(p.tab[i]);
    for (int i = 0; i < p.nfiles; i++) free((void *)p.files[i].path), bdel(p.out ? p.out[i] : NULL);
    free(p.files), free(p.ord), free(p.out), free(p.outn), free(p.ready);
    for (size_t i = 0; i < p.cv.nnotes; i++) free(p.cv.notes[i].name);
    free(p.cv.notes);
    return rc;
}
