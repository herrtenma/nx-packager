// See nxzip.h. Layout per PKWARE's APPNOTE: local headers + data, central directory, end of central directory record.
#include "nxzip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

struct NxZip {
    FILE *f;
    NxZipEntry *e;
    int n;
};

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

NxZip *nx_zip_open(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseeko(f, 0, SEEK_END);
    off_t size = ftello(f);
    size_t tail = size < 0x10016 ? (size_t)size : 0x10016;   // EOCD (22 bytes) + the longest comment
    uint8_t *t = malloc(tail);
    fseeko(f, size - (off_t)tail, SEEK_SET);
    if (!t || fread(t, 1, tail, f) != tail) { free(t); fclose(f); return NULL; }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--)
        if (u32(t + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(t); fclose(f); return NULL; }
    uint32_t count = u16(t + eocd + 10), cd_size = u32(t + eocd + 12), cd_off = u32(t + eocd + 16);
    free(t);
    uint8_t *cd = malloc(cd_size ? cd_size : 1);
    fseeko(f, cd_off, SEEK_SET);
    if (!cd || fread(cd, 1, cd_size, f) != cd_size) { free(cd); fclose(f); return NULL; }
    NxZip *z = calloc(1, sizeof *z);
    z->f = f;
    z->e = calloc(count ? count : 1, sizeof *z->e);
    for (uint32_t i = 0, p = 0; i < count; i++) {
        if (p + 46 > cd_size || u32(cd + p) != 0x02014b50) break;
        uint16_t nl = u16(cd + p + 28), xl = u16(cd + p + 30), cl = u16(cd + p + 32);
        if (p + 46 + nl > cd_size) break;
        NxZipEntry *e = &z->e[z->n++];
        e->method = u16(cd + p + 10), e->crc = u32(cd + p + 16), e->csize = u32(cd + p + 20), e->size = u32(cd + p + 24);
        e->local_off = u32(cd + p + 42);
        e->mode = u16(cd + p + 4) >> 8 == 3 ? u32(cd + p + 38) >> 16 : 0;   // made by Unix: mode in the upper half
        e->name = strndup((const char *)cd + p + 46, nl);
        p += 46 + nl + xl + cl;
    }
    free(cd);
    return z;
}
void nx_zip_close(NxZip *z) {
    if (!z) return;
    for (int i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e);
    fclose(z->f);
    free(z);
}
int nx_zip_count(const NxZip *z) { return z->n; }
const NxZipEntry *nx_zip_entry(const NxZip *z, int i) { return i >= 0 && i < z->n ? &z->e[i] : NULL; }
const NxZipEntry *nx_zip_find(const NxZip *z, const char *name) {
    for (int i = 0; i < z->n; i++)
        if (!strcmp(z->e[i].name, name)) return &z->e[i];
    return NULL;
}
uint8_t *nx_zip_read(NxZip *z, const NxZipEntry *e) {
    uint8_t h[30];
    fseeko(z->f, e->local_off, SEEK_SET);
    if (fread(h, 1, 30, z->f) != 30 || u32(h) != 0x04034b50) return NULL;
    fseeko(z->f, (off_t)e->local_off + 30 + u16(h + 26) + u16(h + 28), SEEK_SET);
    uint8_t *c = malloc(e->csize ? e->csize : 1), *out = malloc(e->size ? e->size : 1);
    if (!c || !out || fread(c, 1, e->csize, z->f) != e->csize) goto bad;
    if (e->method == 0) {
        if (e->csize != e->size) goto bad;
        memcpy(out, c, e->size);
    } else if (e->method == 8) {
        z_stream s = {0};
        if (inflateInit2(&s, -15) != Z_OK) goto bad;
        s.next_in = c, s.avail_in = e->csize, s.next_out = out, s.avail_out = e->size;
        int r = inflate(&s, Z_FINISH);
        inflateEnd(&s);
        if (r != Z_STREAM_END || s.total_out != e->size) goto bad;
    } else goto bad;
    free(c);
    if ((uint32_t)crc32(0, out, e->size) != e->crc) { free(out); return NULL; }
    return out;
bad:
    free(c);
    free(out);
    return NULL;
}

// ---- writing --------------------------------------------------------------------------------------------------------
struct NxZipWriter {
    FILE *f;
    NxZipEntry *e;
    int n, cap;
    int failed;
};
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v), put16(p + 2, (uint16_t)(v >> 16)); }

NxZipWriter *nx_zip_create(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    NxZipWriter *w = calloc(1, sizeof *w);
    w->f = f;
    return w;
}
int nx_zip_add(NxZipWriter *w, const char *name, const void *data, size_t n, uint32_t mode, int store) {
    if (w->failed) return -1;
    if (n > 0xFFFFFFF0u) return w->failed = -1;
    uint8_t *packed = NULL;
    uint32_t csize = (uint32_t)n;
    uint16_t method = 0;
    if (data && n && !store) {
        z_stream s = {0};
        if (deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return w->failed = -1;
        size_t cap = deflateBound(&s, n);
        packed = malloc(cap);
        s.next_in = (Bytef *)data, s.avail_in = (uInt)n, s.next_out = packed, s.avail_out = (uInt)cap;
        int r = deflate(&s, Z_FINISH);
        deflateEnd(&s);
        if (r != Z_STREAM_END) { free(packed); return w->failed = -1; }
        if (s.total_out < n) csize = (uint32_t)s.total_out, method = 8;
    }
    if (w->n == w->cap) w->cap = w->cap ? w->cap * 2 : 64, w->e = realloc(w->e, w->cap * sizeof *w->e);
    NxZipEntry *e = &w->e[w->n++];
    e->name = strdup(name);
    e->crc = data ? (uint32_t)crc32(0, data, (uInt)n) : 0;
    e->size = (uint32_t)n, e->csize = csize, e->method = method, e->mode = mode;
    e->local_off = (uint32_t)ftello(w->f);
    uint8_t h[30] = {0};
    size_t nl = strlen(name);
    put32(h, 0x04034b50), put16(h + 4, 20), put16(h + 8, method);
    put16(h + 10, 0), put16(h + 12, 0x21);   // DOS time/date: 1980-01-01, reproducible
    put32(h + 14, e->crc), put32(h + 18, csize), put32(h + 22, (uint32_t)n), put16(h + 26, (uint16_t)nl);
    if (fwrite(h, 1, 30, w->f) != 30 || fwrite(name, 1, nl, w->f) != nl ||
        (csize && fwrite(method ? packed : data, 1, csize, w->f) != csize))
        w->failed = -1;
    free(packed);
    return w->failed;
}
int nx_zip_finish(NxZipWriter *w) {
    off_t cd_off = ftello(w->f);
    for (int i = 0; i < w->n && !w->failed; i++) {
        NxZipEntry *e = &w->e[i];
        uint8_t h[46] = {0};
        size_t nl = strlen(e->name);
        put32(h, 0x02014b50), put16(h + 4, 3 << 8 | 20), put16(h + 6, 20), put16(h + 10, e->method);
        put16(h + 12, 0), put16(h + 14, 0x21);
        put32(h + 16, e->crc), put32(h + 20, e->csize), put32(h + 24, e->size), put16(h + 28, (uint16_t)nl);
        put32(h + 38, e->mode << 16 | (e->name[nl - 1] == '/' ? 0x10 : 0)), put32(h + 42, e->local_off);
        if (fwrite(h, 1, 46, w->f) != 46 || fwrite(e->name, 1, nl, w->f) != nl) w->failed = -1;
    }
    off_t cd_end = ftello(w->f);
    uint8_t eocd[22] = {0};
    put32(eocd, 0x06054b50), put16(eocd + 8, (uint16_t)w->n), put16(eocd + 10, (uint16_t)w->n);
    put32(eocd + 12, (uint32_t)(cd_end - cd_off)), put32(eocd + 16, (uint32_t)cd_off);
    if (fwrite(eocd, 1, 22, w->f) != 22) w->failed = -1;
    if (fclose(w->f)) w->failed = -1;
    int r = w->failed;
    for (int i = 0; i < w->n; i++) free(w->e[i].name);
    free(w->e);
    free(w);
    return r;
}
