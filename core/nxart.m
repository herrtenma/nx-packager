// See nxart.h. RomFS: the level-3 image (header of ten u64: directory / file hash and meta tables, data offset);
// SARC: SFAT nodes with SFNT names; BNTX: BRTI blocks (see nxtex.c for their fields), level 0 of layer 0 only.
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <math.h>

#include "nxart.h"
#include "nxzip.h"

#include "../third_party/bcdec.h"

int nx_bl_deswizzle(const uint8_t *src, size_t src_len, uint8_t *dst, uint32_t wb, uint32_t hb, int bpb, int gh);

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); goto fail; } while (0)

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

// ---- RomFS ----------------------------------------------------------------------------------------------------------
// Finds the file that starts `path` (the part after it is left in *rest: a path inside the file). 0 = found.
static int romfs_find(NxArtRead rd, void *ctx, const char *path, uint64_t *off, uint64_t *size, const char **rest) {
    uint8_t hdr[0x50];
    if (rd(ctx, 0, hdr, sizeof hdr)) return -1;
    uint64_t dmo = rd64(hdr + 0x18), dms = rd64(hdr + 0x20), fmo = rd64(hdr + 0x38), fms = rd64(hdr + 0x40), data = rd64(hdr + 0x48);
    if (dms > 64 << 20 || fms > 64 << 20) return -1;
    uint8_t *dm = malloc(dms), *fm = malloc(fms);
    int r = -1;
    if (!dm || !fm || rd(ctx, dmo, dm, dms) || rd(ctx, fmo, fm, fms)) goto done;
    uint32_t dir = 0;
    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        // a file of this directory?
        for (uint32_t f = rd32(dm + dir + 12); f != 0xffffffff && f + 0x20 <= fms;) {
            uint32_t nl = rd32(fm + f + 0x1c);
            if (nl == len && f + 0x20 + nl <= fms && !memcmp(fm + f + 0x20, p, len)) {
                *off = data + rd64(fm + f + 8), *size = rd64(fm + f + 0x10), *rest = slash ? slash + 1 : p + len;
                r = 0;
                goto done;
            }
            f = rd32(fm + f + 4);
        }
        // a subdirectory?
        uint32_t c = rd32(dm + dir + 8);
        while (c != 0xffffffff && c + 0x18 <= dms) {
            uint32_t nl = rd32(dm + c + 0x14);
            if (nl == len && c + 0x18 + nl <= dms && !memcmp(dm + c + 0x18, p, len)) break;
            c = rd32(dm + c + 4);
        }
        if (c == 0xffffffff || c + 0x18 > dms || !slash) goto done;
        dir = c, p = slash + 1;
    }
done:
    free(dm), free(fm);
    return r;
}

// ---- containers -----------------------------------------------------------------------------------------------------
static uint8_t *yaz0(const uint8_t *s, size_t n, size_t *on) {
    if (n < 16) return NULL;
    uint32_t size = (uint32_t)s[4] << 24 | s[5] << 16 | s[6] << 8 | s[7];
    uint8_t *o = malloc(size ? size : 1);
    size_t i = 16, k = 0;
    while (o && k < size && i < n) {
        uint8_t code = s[i++];
        for (int b = 0; b < 8 && k < size; b++) {
            if (code & (0x80 >> b)) {
                if (i >= n) goto bad;
                o[k++] = s[i++];
                continue;
            }
            if (i + 1 >= n) goto bad;
            size_t dist = ((s[i] & 15) << 8 | s[i + 1]) + 1, cnt = s[i] >> 4;
            i += 2;
            if (!cnt) { if (i >= n) goto bad; cnt = s[i++] + 0x12; } else cnt += 2;
            if (dist > k || k + cnt > size) goto bad;
            for (size_t j = 0; j < cnt; j++, k++) o[k] = o[k - dist];
        }
    }
    if (!o || k != size) goto bad;
    *on = size;
    return o;
bad:
    free(o);
    return NULL;
}

// Each SARC entry: fn(name, data, size); fn returns nonzero to stop. Little-endian SARCs only (all Switch games).
static void sarc_each(const uint8_t *b, size_t n, int (^fn)(const char *name, const uint8_t *d, size_t dn)) {
    if (n < 0x14 || memcmp(b, "SARC", 4) || b[6] != 0xff) return;
    uint16_t hl;
    memcpy(&hl, b + 4, 2);
    uint32_t data = rd32(b + 0xc);
    if (hl + 12 > n || memcmp(b + hl, "SFAT", 4)) return;
    uint16_t nodes;
    memcpy(&nodes, b + hl + 6, 2);
    size_t names = hl + 12 + (size_t)nodes * 16 + 8;
    if (names > n) return;
    for (uint16_t i = 0; i < nodes; i++) {
        const uint8_t *e = b + hl + 12 + i * 16;
        uint32_t attr = rd32(e + 4), s = rd32(e + 8), en = rd32(e + 12);
        if (!(attr & 0x01000000) || data + (size_t)en > n || s > en) continue;
        size_t no = names + (size_t)(attr & 0xffff) * 4;
        if (no >= n || !memchr(b + no, 0, n - no)) continue;
        if (fn((const char *)b + no, b + data + s, en - s)) return;
    }
}

// The file at `path` inside a container buffer (path "" = the buffer itself), decompressed: malloc'd copy or NULL.
static uint8_t *inner_file(const uint8_t *b, size_t n, const char *path, size_t *on) {
    uint8_t *dec = NULL;
    if (n >= 4 && !memcmp(b, "Yaz0", 4)) {
        if (!(dec = yaz0(b, n, &n))) return NULL;
        b = dec;
    }
    uint8_t *r = NULL;
    if (!*path) {
        if (dec) r = dec, *on = n, dec = NULL;
        else if ((r = malloc(n ? n : 1))) memcpy(r, b, n), *on = n;
    } else {
        // the SARC entry whose name is the longest prefix of path (entry names hold '/')
        __block const uint8_t *hit = NULL;
        __block size_t hn = 0, hl = 0;
        sarc_each(b, n, ^int(const char *name, const uint8_t *d, size_t dn) {
            size_t l = strlen(name);
            if (l > hl && !strncmp(path, name, l) && (path[l] == 0 || path[l] == '/')) hit = d, hn = dn, hl = l;
            return 0;
        });
        if (hit) r = inner_file(hit, hn, path[hl] ? path + hl + 1 : "", on);
    }
    free(dec);
    return r;
}

static uint8_t *open_path(NxArtRead rd, void *ctx, const char *path, size_t *n, char *err, size_t errlen) {
    uint64_t off, size;
    const char *rest;
    uint8_t *raw = NULL, *r = NULL;
    if (romfs_find(rd, ctx, path, &off, &size, &rest)) FAIL("%s: not in the RomFS", path);
    if (size > 512 << 20 || !(raw = malloc(size ? size : 1)) || rd(ctx, off, raw, size)) FAIL("%s: read failed", path);
    if (!(r = inner_file(raw, size, rest, n))) FAIL("%s: \"%s\" not found inside it", path, rest);
fail:
    free(raw);
    return r;
}

// ---- BNTX -----------------------------------------------------------------------------------------------------------
typedef struct { int type, bw, bpb; const char *name; } Fmt;
static const Fmt *fmt_of(uint32_t f) {
    static const Fmt fmts[] = {{0x02, 1, 1, "R8"}, {0x0b, 1, 4, "RGBA8"}, {0x1a, 4, 8, "BC1"}, {0x1b, 4, 16, "BC2"},
                               {0x1c, 4, 16, "BC3"}, {0x1d, 4, 8, "BC4"}, {0x1e, 4, 16, "BC5"}, {0x20, 4, 16, "BC7"}};
    for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++)
        if (fmts[i].type == (int)(f >> 8 & 255)) return &fmts[i];
    return NULL;
}

static void brti_each(const uint8_t *b, size_t n, int (^fn)(const uint8_t *r, const char *name)) {
    if (n < 0x30 || memcmp(b, "BNTX", 4)) return;
    uint32_t cnt = rd32(b + 0x24);
    uint64_t info = rd64(b + 0x28);
    for (uint32_t i = 0; i < cnt && info + 8 * (i + 1) <= n; i++) {
        uint64_t p = rd64(b + info + 8 * i);
        if (p + 0xa0 > n || memcmp(b + p, "BRTI", 4)) break;
        uint64_t no = rd64(b + p + 0x60);
        if (no + 2 > n) continue;
        uint16_t nl;
        memcpy(&nl, b + no, 2);
        if (no + 2 + nl > n) continue;
        char name[256];
        snprintf(name, sizeof name, "%.*s", (int)nl, b + no + 2);
        if (fn(b + p, name)) return;
    }
}

static int name_matches(const char *have, const char *want) {
    size_t l = strlen(want);
    return !strcmp(have, want) || (!strncmp(have, want, l) && (have[l] == '^' || have[l] == '+') && strlen(have) == l + 2);
}

static uint8_t *brti_decode(const uint8_t *b, size_t n, const uint8_t *r, int *ow, int *oh, char *err, size_t errlen) {
    uint32_t f = rd32(r + 0x1c), w = rd32(r + 0x24), h = rd32(r + 0x28), layout = rd32(r + 0x34), size = rd32(r + 0x50);
    uint64_t ptrs = rd64(r + 0x70), base;
    const Fmt *ft = fmt_of(f);
    uint8_t *lin = NULL, *rgba = NULL;
    if (!ft) FAIL("texture format %#x is not supported", f);
    if (!w || !h || w > 8192 || h > 8192 || ptrs + 8 > n || (base = rd64(b + ptrs)) + size > n) FAIL("bad texture header");
    uint32_t wb = (w + ft->bw - 1) / ft->bw, hb = (h + ft->bw - 1) / ft->bw;
    lin = malloc((size_t)wb * hb * ft->bpb);
    rgba = calloc((size_t)wb * ft->bw * hb * ft->bw, 4);
    if (!lin || !rgba) FAIL("out of memory");
    if (nx_bl_deswizzle(b + base, size, lin, wb, hb, ft->bpb, 1 << (layout & 7))) FAIL("texture data too short");
    size_t pw = (size_t)wb * ft->bw;   // padded width
    for (uint32_t by = 0; by < hb; by++)
        for (uint32_t bx = 0; bx < wb; bx++) {
            const uint8_t *s = lin + ((size_t)by * wb + bx) * ft->bpb;
            uint8_t *d = rgba + ((size_t)by * ft->bw * pw + (size_t)bx * ft->bw) * 4;
            int pitch = (int)pw * 4;
            uint8_t one[16 * 2];
            switch (ft->type) {
            case 0x02: d[0] = s[0], d[3] = 255; break;
            case 0x0b: memcpy(d, s, 4); break;
            case 0x1a: bcdec_bc1(s, d, pitch); break;
            case 0x1b: bcdec_bc2(s, d, pitch); break;
            case 0x1c: bcdec_bc3(s, d, pitch); break;
            case 0x20: bcdec_bc7(s, d, pitch); break;
            case 0x1d:
            case 0x1e:
                if (ft->type == 0x1d) bcdec_bc4(s, one, 4);
                else bcdec_bc5(s, one, 8);
                for (int k = 0; k < 16; k++) {
                    uint8_t *px = d + ((size_t)(k / 4) * pw + k % 4) * 4;
                    px[0] = ft->type == 0x1d ? one[k] : one[2 * k], px[1] = ft->type == 0x1d ? 0 : one[2 * k + 1], px[2] = 0, px[3] = 255;
                }
                break;
            }
        }
    // channel mapping (0 zero, 1 one, 2-5 R G B A), rows to w texels
    const uint8_t *map = r + 0x58;
    uint8_t *out = malloc((size_t)w * h * 4);
    if (!out) FAIL("out of memory");
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *s = rgba + ((size_t)y * pw + x) * 4;
            uint8_t *d = out + ((size_t)y * w + x) * 4;
            for (int c = 0; c < 4; c++) d[c] = map[c] == 0 ? 0 : map[c] == 1 ? 255 : map[c] <= 5 ? s[map[c] - 2] : s[c];
        }
    free(lin), free(rgba);
    *ow = (int)w, *oh = (int)h;
    return out;
fail:
    free(lin), free(rgba);
    return NULL;
}

uint8_t *nx_art_texture(NxArtRead rd, void *ctx, const char *path, const char *name, int *w, int *h, char *err, size_t errlen) {
    size_t n;
    uint8_t *b = open_path(rd, ctx, path, &n, err, errlen);
    if (!b) return NULL;
    __block const uint8_t *hit = NULL;
    brti_each(b, n, ^int(const uint8_t *r, const char *nm) {
        if (name_matches(nm, name)) hit = r;
        return hit != NULL;
    });
    uint8_t *out = NULL;
    if (!hit) snprintf(err, errlen, "%s: no texture \"%s\"", path, name);
    else out = brti_decode(b, n, hit, w, h, err, errlen);
    free(b);
    return out;
}

static void list_in(const uint8_t *b, size_t n, const char *path, void (^fn)(const char *, const uint8_t *, size_t)) {
    uint8_t *dec = NULL;
    if (n >= 4 && !memcmp(b, "Yaz0", 4)) {
        if (!(dec = yaz0(b, n, &n))) return;
        b = dec;
    }
    if (n >= 4 && !memcmp(b, "SARC", 4))
        sarc_each(b, n, ^int(const char *name, const uint8_t *d, size_t dn) {
            char sub[1024];
            snprintf(sub, sizeof sub, "%s/%s", path, name);
            list_in(d, dn, sub, fn);
            return 0;
        });
    else if (n >= 4 && !memcmp(b, "BNTX", 4)) fn(path, b, n);
    free(dec);
}

int nx_art_list(NxArtRead rd, void *ctx, const char *path, void (*fn)(void *, const char *, const char *, int, int, const char *),
                void *fctx, char *err, size_t errlen) {
    size_t n;
    uint8_t *b = open_path(rd, ctx, path, &n, err, errlen);
    if (!b) return -1;
    list_in(b, n, path, ^(const char *p, const uint8_t *bn, size_t bnn) {
        brti_each(bn, bnn, ^int(const uint8_t *r, const char *name) {
            const Fmt *ft = fmt_of(rd32(r + 0x1c));
            char other[16];
            snprintf(other, sizeof other, "%#x", rd32(r + 0x1c));
            fn(fctx, p, name, (int)rd32(r + 0x24), (int)rd32(r + 0x28), ft ? ft->name : other);
            return 0;
        });
    });
    free(b);
    return 0;
}

// ---- PNG ------------------------------------------------------------------------------------------------------------
int nx_art_png(const char *out_path, const uint8_t *rgba, int w, int h, int x, int y, int cw, int ch) {
    if (cw <= 0 || ch <= 0) x = 0, y = 0, cw = w, ch = h;
    if (x < 0 || y < 0 || x + cw > w || y + ch > h) return -1;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, rgba + ((size_t)y * w + x) * 4, (size_t)w * 4 * (ch - 1) + (size_t)cw * 4, NULL);
    CGImageRef img = CGImageCreate(cw, ch, 8, 32, (size_t)w * 4, cs, kCGBitmapByteOrderDefault | kCGImageAlphaLast, dp, NULL, false,
                                   kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)out_path, strlen(out_path), false);
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    int r = -1;
    if (dst && img) {
        CGImageDestinationAddImage(dst, img, NULL);
        r = CGImageDestinationFinalize(dst) ? 0 : -1;
    }
    if (dst) CFRelease(dst);
    if (url) CFRelease(url);
    CGImageRelease(img), CGDataProviderRelease(dp), CGColorSpaceRelease(cs);
    return r;
}

// ---- port files -----------------------------------------------------------------------------------------------------
static int hexcolor(id v, float rgb[3]) {
    unsigned c;
    if (![v isKindOfClass:NSString.class] || sscanf([v UTF8String], "#%6x", &c) != 1) return -1;
    rgb[0] = (c >> 16 & 255) / 255.f, rgb[1] = (c >> 8 & 255) / 255.f, rgb[2] = (c & 255) / 255.f;
    return 0;
}
static float smooth(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}
// "keep": {"hue": [from, to] (degrees), "saturation": [min, max], "value": [min, max], "invert": bool, "region": [x, y,
// w, h]}: only the pixels in that HSV box (or outside it; soft edges) and inside the region (the image keeps its size); for splitting one logo texture into layers. "tint": ["#top", "#bottom"]:
// RGB multiplied by a vertical gradient.
static float range_k(NSArray *r, float v, float soft) {
    if (![r isKindOfClass:NSArray.class] || r.count != 2) return 1;
    float lo = [r[0] floatValue], hi = [r[1] floatValue];
    return smooth(lo - soft, lo + soft, v) * (1 - smooth(hi - soft, hi + soft, v));
}
static void shape(NSDictionary *d, uint8_t *rgba, int w, int h) {
    NSDictionary *keep = [d[@"keep"] isKindOfClass:NSDictionary.class] ? d[@"keep"] : nil;
    NSArray *tint = [d[@"tint"] isKindOfClass:NSArray.class] ? d[@"tint"] : nil;
    float top[3], bot[3];
    if (tint && (tint.count != 2 || hexcolor(tint[0], top) || hexcolor(tint[1], bot))) tint = nil;
    if (!keep && !tint) return;
    BOOL invert = [keep[@"invert"] boolValue];
    NSArray *region = [keep[@"region"] isKindOfClass:NSArray.class] && [keep[@"region"] count] == 4 ? keep[@"region"] : nil;
    int rx0 = [region[0] intValue], ry0 = [region[1] intValue], rx1 = rx0 + [region[2] intValue], ry1 = ry0 + [region[3] intValue];
    for (int y = 0; y < h; y++) {
        float t = h > 1 ? (float)y / (h - 1) : 0;
        for (int x = 0; x < w; x++) {
            uint8_t *p = rgba + ((size_t)y * w + x) * 4;
            if (keep) {
                float r = p[0] / 255.f, g = p[1] / 255.f, b = p[2] / 255.f;
                float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), c = mx - mn, hue = 0;
                if (c > 0) hue = mx == r ? 60 * fmodf((g - b) / c + 6, 6) : mx == g ? 60 * ((b - r) / c + 2) : 60 * ((r - g) / c + 4);
                float k = range_k(keep[@"hue"], hue, 5) * range_k(keep[@"saturation"], mx > 0 ? c / mx : 0, 0.04f) *
                          range_k(keep[@"value"], mx, 0.04f);
                if (invert) k = 1 - k;
                if (region && (x < rx0 || x >= rx1 || y < ry0 || y >= ry1)) k = 0;
                p[3] = (uint8_t)(p[3] * k + 0.5f);
            }
            if (tint)
                for (int c = 0; c < 3; c++) p[c] = (uint8_t)(p[c] * (top[c] + (bot[c] - top[c]) * t) + 0.5f);
        }
    }
}

// Every dictionary in the port with "file" + "texture" gets its texture written as <prefix><n>.png and "image" set.
static int resolve_node(id node, NxArtRead rd, void *ctx, NSString *dir, int *count, char *err, size_t errlen) {
    if ([node isKindOfClass:NSArray.class]) {
        for (id v in node)
            if (resolve_node(v, rd, ctx, dir, count, err, errlen)) return -1;
        return 0;
    }
    if (![node isKindOfClass:NSMutableDictionary.class]) return 0;
    NSMutableDictionary *d = node;
    for (NSString *k in d.allKeys)
        if (resolve_node(d[k], rd, ctx, dir, count, err, errlen)) return -1;
    NSString *file = d[@"file"], *tex = d[@"texture"];
    if (![file isKindOfClass:NSString.class] || ![tex isKindOfClass:NSString.class]) return 0;
    int w, h, x = 0, y = 0, cw = 0, ch = 0;
    uint8_t *rgba = nx_art_texture(rd, ctx, file.UTF8String, tex.UTF8String, &w, &h, err, errlen);
    if (!rgba) return -1;
    shape(d, rgba, w, h);
    NSArray *crop = d[@"crop"];
    if ([crop isKindOfClass:NSArray.class] && crop.count == 4)
        x = [crop[0] intValue], y = [crop[1] intValue], cw = [crop[2] intValue], ch = [crop[3] intValue];
    NSString *name = [NSString stringWithFormat:@"art%d.png", (*count)++];
    int r = nx_art_png([dir stringByAppendingPathComponent:name].fileSystemRepresentation, rgba, w, h, x, y, cw, ch);
    free(rgba);
    if (r) {
        snprintf(err, errlen, "%s %s: crop outside the texture or PNG write failed", file.UTF8String, tex.UTF8String);
        return -1;
    }
    d[@"image"] = name;
    return 0;
}

int nx_art_resolve(const char *port, NxArtRead rd, void *ctx, const char *out_dir, char *err, size_t errlen) {
    @autoreleasepool {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *dir = @(out_dir), *src = @(port);
        [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        NSData *json = nil;
        BOOL isdir = NO;
        if ([fm fileExistsAtPath:src isDirectory:&isdir] && isdir) {
            json = [NSData dataWithContentsOfFile:[src stringByAppendingPathComponent:@"port.json"]];
            for (NSString *f in [fm subpathsAtPath:src]) {   // the port's own files (outlines, ...)
                NSString *from = [src stringByAppendingPathComponent:f], *to = [dir stringByAppendingPathComponent:f];
                if ([f isEqualToString:@"port.json"] || ([fm fileExistsAtPath:from isDirectory:&isdir] && isdir)) continue;
                [fm createDirectoryAtPath:to.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
                [fm removeItemAtPath:to error:nil];
                [fm copyItemAtPath:from toPath:to error:nil];
            }
        } else {
            NxZip *z = nx_zip_open(port);
            if (!z) { snprintf(err, errlen, "%s: not a port file", port); return -1; }
            for (int i = 0; i < nx_zip_count(z); i++) {
                const NxZipEntry *e = nx_zip_entry(z, i);
                size_t l = strlen(e->name);
                if (!l || e->name[l - 1] == '/' || strstr(e->name, "..")) continue;
                uint8_t *b = nx_zip_read(z, e);
                if (!b) continue;
                NSData *data = [NSData dataWithBytesNoCopy:b length:e->size freeWhenDone:YES];
                if (!strcmp(e->name, "port.json")) { json = data; continue; }
                NSString *to = [dir stringByAppendingPathComponent:@(e->name)];
                [fm createDirectoryAtPath:to.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
                [data writeToFile:to atomically:YES];
            }
            nx_zip_close(z);
        }
        id root = json ? [NSJSONSerialization JSONObjectWithData:json options:NSJSONReadingMutableContainers error:nil] : nil;
        if (![root isKindOfClass:NSMutableDictionary.class]) { snprintf(err, errlen, "%s: no readable port.json", port); return -1; }
        int count = 0;
        if (resolve_node(root, rd, ctx, dir, &count, err, errlen)) return -1;
        NSData *out = [NSJSONSerialization dataWithJSONObject:root options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nil];
        if (![out writeToFile:[dir stringByAppendingPathComponent:@"unkai.json"] atomically:YES]) {
            snprintf(err, errlen, "%s: could not write unkai.json", out_dir);
            return -1;
        }
        return 0;
    }
}
