// See nxcontent.h. XCI: header "HEAD" at 0x100, root HFS0 at the offset in +0x130, partition "secure" holds the NCAs.
// NSP: a PFS0 holding the NCAs and the tickets (.tik) of title-key encrypted ones. NCA: 0xC00-byte header (AES-XTS
// with header_key), four sections; ExeFS and meta are PFS0 sections, control data is a RomFS section; section data is
// AES-CTR with a key from the key area (key_area_key_*) or the ticket's title key (titlekek_*).
#include "nxcontent.h"

#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { NCA_PROGRAM = 0, NCA_META = 1, NCA_CONTROL = 2 };
enum { MAX_NCAS = 64, MAX_TICKETS = 16 };

typedef struct {
    uint64_t off, size;        // in the dump
    int type;                  // content type (NCA_*), -1 = unreadable
    uint64_t title_id;
    uint8_t header[0xC00];     // decrypted
    uint8_t ctr_key[16];
    int have_key;
} Nca;

typedef struct {               // one section's data (a PFS0 or a RomFS level 5), decrypted on read
    const Nca *nca;
    uint64_t data_off, size;   // within the NCA
    uint8_t ctr_hi[8];
    int encrypted;
} Section;

struct NxDump {
    int fd;
    char keys_path[1024];
    uint8_t header_key[32];
    Nca nca[MAX_NCAS];
    int nnca;
    struct { uint8_t rights_id[16], title_key[16]; } tik[MAX_TICKETS];
    int ntik;
    int nsp;
};

static void fail(char *err, size_t n, const char *fmt, ...) {
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

// ---- keys ---------------------------------------------------------------------------------------------------------
static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static int load_key(const char *path, const char *name, uint8_t *out, size_t len) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int found = -1;
    size_t nl = strlen(name);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, name, nl) || (line[nl] != ' ' && line[nl] != '=')) continue;
        char *p = strchr(line, '=');
        if (!p) continue;
        p++;
        while (*p == ' ') p++;
        size_t i = 0;
        for (; i < len && hexval(p[2 * i]) >= 0 && hexval(p[2 * i + 1]) >= 0; i++)
            out[i] = (uint8_t)(hexval(p[2 * i]) << 4 | hexval(p[2 * i + 1]));
        found = i == len ? 0 : -1;
        break;
    }
    fclose(f);
    return found;
}

// ---- crypto -------------------------------------------------------------------------------------------------------
static void aes_ecb(int decrypt, const uint8_t *key, const uint8_t *in, uint8_t *out, size_t n) {
    size_t moved;
    CCCrypt(decrypt ? kCCDecrypt : kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 16, NULL, in, n, out, n, &moved);
}
// Nintendo's XTS: 0x200-byte sectors, tweak = sector number big-endian.
static void aes_xts_decrypt(const uint8_t *key32, uint8_t *buf, size_t size, uint64_t sector) {
    for (size_t s = 0; s < size; s += 0x200, sector++) {
        uint8_t tweak[16] = {0}, t[16];
        for (int i = 0; i < 8; i++) tweak[15 - i] = (uint8_t)(sector >> (8 * i));
        aes_ecb(0, key32 + 16, tweak, t, 16);
        for (size_t b = 0; b < 0x200; b += 16) {
            uint8_t x[16];
            for (int i = 0; i < 16; i++) x[i] = buf[s + b + i] ^ t[i];
            aes_ecb(1, key32, x, x, 16);
            for (int i = 0; i < 16; i++) buf[s + b + i] = x[i] ^ t[i];
            int carry = t[15] >> 7;   // t *= alpha (little-endian GF(2^128))
            for (int i = 15; i > 0; i--) t[i] = (uint8_t)(t[i] << 1 | t[i - 1] >> 7);
            t[0] = (uint8_t)(t[0] << 1) ^ (carry ? 0x87 : 0);
        }
    }
}
static void aes_ctr(const uint8_t key[16], const uint8_t ctr_hi[8], uint8_t *buf, size_t n, uint64_t nca_offset) {
    uint8_t iv[16];
    memcpy(iv, ctr_hi, 8);
    uint64_t block = nca_offset >> 4;
    for (int i = 0; i < 8; i++) iv[15 - i] = (uint8_t)(block >> (8 * i));
    CCCryptorRef c;
    CCCryptorCreateWithMode(kCCDecrypt, kCCModeCTR, kCCAlgorithmAES, ccNoPadding, iv, key, 16, NULL, 0, 0, kCCModeOptionCTR_BE, &c);
    size_t moved;
    CCCryptorUpdate(c, buf, n, buf, n, &moved);
    CCCryptorRelease(c);
}
void nx_sha256(const void *p, size_t n, uint8_t out[32]) {
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    for (size_t at = 0; at < n; at += 1u << 30) CC_SHA256_Update(&c, (const uint8_t *)p + at, (CC_LONG)(n - at < (1u << 30) ? n - at : (1u << 30)));
    CC_SHA256_Final(out, &c);
}

// ---- reading ------------------------------------------------------------------------------------------------------
static int pread_all(const NxDump *d, void *buf, size_t n, uint64_t off) {
    return pread(d->fd, buf, n, (off_t)off) == (ssize_t)n ? 0 : -1;
}
static int section_read(const NxDump *d, const Section *s, uint64_t off, void *dst, size_t n) {
    if (off > s->size || n > s->size - off) return -1;
    uint64_t at = s->data_off + off;
    if (!s->encrypted) return pread_all(d, dst, n, s->nca->off + at);
    uint64_t start = at & ~15ull, end = (at + n + 15) & ~15ull;
    uint8_t *buf = malloc(end - start);
    if (!buf || pread_all(d, buf, end - start, s->nca->off + start)) { free(buf); return -1; }
    aes_ctr(s->nca->ctr_key, s->ctr_hi, buf, end - start, start);
    memcpy(dst, buf + (at - start), n);
    free(buf);
    return 0;
}

// Partition tables: HFS0 (XCI, entries 0x40 bytes) and PFS0 (NSP / NCA sections, entries 0x18 bytes). Calls fn for
// each file with its absolute offset (relative to `base`) and size; stops when fn returns nonzero.
typedef int (*EntryFn)(void *ctx, const char *name, uint64_t off, uint64_t size);
static int partition_walk(const uint8_t *table, size_t avail, uint64_t base, EntryFn fn, void *ctx) {
    if (avail < 16) return -1;
    int hfs = !memcmp(table, "HFS0", 4);
    if (!hfs && memcmp(table, "PFS0", 4)) return -1;
    uint32_t n, strsz;
    memcpy(&n, table + 4, 4);
    memcpy(&strsz, table + 8, 4);
    size_t esz = hfs ? 0x40 : 0x18, tsize = 16 + (size_t)n * esz + strsz;
    if (tsize > avail) return -1;
    const char *strtab = (const char *)table + 16 + n * esz;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = table + 16 + i * esz;
        uint64_t foff, fsz;
        uint32_t noff;
        memcpy(&foff, e, 8);
        memcpy(&fsz, e + 8, 8);
        memcpy(&noff, e + 16, 4);
        if (noff >= strsz) return -1;
        if (fn(ctx, strtab + noff, base + tsize + foff, fsz)) return 0;
    }
    return 0;
}
static uint8_t *read_table(const NxDump *d, uint64_t off, size_t *avail) {
    uint8_t head[16];
    if (pread_all(d, head, 16, off)) return NULL;
    uint32_t n, strsz;
    memcpy(&n, head + 4, 4);
    memcpy(&strsz, head + 8, 4);
    if (n > 100000 || strsz > (16u << 20)) return NULL;
    *avail = 16 + (size_t)n * (memcmp(head, "HFS0", 4) ? 0x18 : 0x40) + strsz;
    uint8_t *t = malloc(*avail);
    if (t && pread_all(d, t, *avail, off)) { free(t); return NULL; }
    return t;
}

// ---- NCAs ---------------------------------------------------------------------------------------------------------
static const uint8_t *fs_header(const Nca *c, int s) { return c->header + 0x400 + s * 0x200; }
static uint64_t section_start(const Nca *c, int s) {
    uint32_t start;
    memcpy(&start, c->header + 0x240 + s * 16, 4);
    return (uint64_t)start * 0x200;
}
static int nca_key(NxDump *d, Nca *c) {
    const uint8_t *h = c->header;
    int gen = h[0x206] > h[0x220] ? h[0x206] : h[0x220], rev = gen ? gen - 1 : 0;
    static const uint8_t zero[16];
    char kname[64];
    if (memcmp(h + 0x230, zero, 16)) {   // title key: from the ticket with this rights id
        for (int i = 0; i < d->ntik; i++)
            if (!memcmp(d->tik[i].rights_id, h + 0x230, 16)) {
                uint8_t kek[16];
                snprintf(kname, sizeof kname, "titlekek_%02x", rev);
                if (load_key(d->keys_path, kname, kek, 16)) return -1;
                aes_ecb(1, kek, d->tik[i].title_key, c->ctr_key, 16);
                return 0;
            }
        return -1;
    }
    static const char *const kind[] = {"application", "ocean", "system"};
    if (h[0x207] > 2) return -1;
    snprintf(kname, sizeof kname, "key_area_key_%s_%02x", kind[h[0x207]], rev);
    uint8_t kaek[16], keys[64];
    if (load_key(d->keys_path, kname, kaek, 16)) return -1;
    aes_ecb(1, kaek, h + 0x300, keys, 64);
    memcpy(c->ctr_key, keys + 0x20, 16);   // key area slot 2: AES-CTR
    return 0;
}
static int nca_add(void *ctx, const char *name, uint64_t off, uint64_t size) {
    NxDump *d = ctx;
    size_t l = strlen(name);
    if (l < 4 || strcmp(name + l - 4, ".nca") || d->nnca == MAX_NCAS) return 0;
    Nca *c = &d->nca[d->nnca];
    memset(c, 0, sizeof *c);
    c->off = off, c->size = size, c->type = -1;
    if (pread_all(d, c->header, sizeof c->header, off)) return 0;
    aes_xts_decrypt(d->header_key, c->header, sizeof c->header, 0);
    if (memcmp(c->header + 0x200, "NCA3", 4)) return 0;
    c->type = c->header[0x205];
    memcpy(&c->title_id, c->header + 0x210, 8);
    d->nnca++;
    return 0;
}
static int tik_add(void *ctx, const char *name, uint64_t off, uint64_t size) {
    NxDump *d = ctx;
    size_t l = strlen(name);
    if (l < 4 || strcmp(name + l - 4, ".tik") || d->ntik == MAX_TICKETS || size < 0x2C0) return 0;
    uint8_t t[0x2C0];
    if (pread_all(d, t, sizeof t, off)) return 0;
    uint32_t sig;
    memcpy(&sig, t, 4);
    if (sig != 0x10004) return 0;   // RSA-2048 SHA-256: body at 0x140
    memcpy(d->tik[d->ntik].title_key, t + 0x180, 16);
    memcpy(d->tik[d->ntik].rights_id, t + 0x2A0, 16);
    d->ntik++;
    return 0;
}
// Section s of an NCA as a PFS0 or a RomFS (level 5 of its IVFC tree).
static int section_open(NxDump *d, Nca *c, int s, Section *out) {
    if (!c->have_key) {
        if (nca_key(d, c)) return -1;
        c->have_key = 1;
    }
    uint64_t start = section_start(c, s);
    const uint8_t *fs = fs_header(c, s);
    if (!start) return -1;
    memset(out, 0, sizeof *out);
    out->nca = c;
    out->encrypted = fs[0x4] == 3;
    if (fs[0x4] != 1 && fs[0x4] != 3) return -1;   // only plain and AES-CTR sections
    for (int j = 0; j < 8; j++) out->ctr_hi[j] = fs[0x140 + 7 - j];
    if (fs[0x3] == 2) {   // hierarchical SHA-256 (master hash, block size, 2 layers): the PFS0 is layer 1
        uint64_t o, n;
        memcpy(&o, fs + 0x8 + 0x38, 8);
        memcpy(&n, fs + 0x8 + 0x40, 8);
        out->data_off = start + o, out->size = n;
    } else if (fs[0x3] == 3) {   // IVFC: level 5 is the RomFS
        uint64_t o, n;
        memcpy(&o, fs + 0x8 + 0x10 + 5 * 0x18, 8);
        memcpy(&n, fs + 0x8 + 0x10 + 5 * 0x18 + 8, 8);
        out->data_off = start + o, out->size = n;
    } else return -1;
    return 0;
}
// A file from a PFS0 section, malloc'd.
typedef struct { const char *want; uint64_t off, size; int found; } FindCtx;
static int find_entry(void *ctx, const char *name, uint64_t off, uint64_t size) {
    FindCtx *f = ctx;
    size_t wl = strlen(f->want);
    int match = f->want[0] == '*' ? strlen(name) >= wl - 1 && !strcmp(name + strlen(name) - (wl - 1), f->want + 1) : !strcmp(name, f->want);
    if (!match) return 0;
    f->off = off, f->size = size, f->found = 1;
    return 1;
}
static uint8_t *pfs0_file(NxDump *d, const Section *s, const char *name, size_t *size) {
    uint8_t head[16];
    if (section_read(d, s, 0, head, 16) || memcmp(head, "PFS0", 4)) return NULL;
    uint32_t n, strsz;
    memcpy(&n, head + 4, 4);
    memcpy(&strsz, head + 8, 4);
    size_t tsize = 16 + (size_t)n * 0x18 + strsz;
    if (n > 4096 || tsize > s->size) return NULL;
    uint8_t *t = malloc(tsize);
    if (!t || section_read(d, s, 0, t, tsize)) { free(t); return NULL; }
    FindCtx f = {name, 0, 0, 0};
    partition_walk(t, tsize, 0, find_entry, &f);
    free(t);
    if (!f.found) return NULL;
    uint8_t *buf = malloc(f.size ? f.size : 1);
    if (!buf || section_read(d, s, f.off, buf, f.size)) { free(buf); return NULL; }
    *size = f.size;
    return buf;
}
static uint8_t *romfs_file(NxDump *d, const Section *s, const char *fname, size_t *size) {
    uint64_t hdr[10];
    if (section_read(d, s, 0, hdr, sizeof hdr) || hdr[0] != 0x50) return NULL;
    uint64_t fmeta_off = hdr[7], fmeta_size = hdr[8], data_off = hdr[9];
    if (fmeta_size > (64u << 20)) return NULL;
    uint8_t *meta = malloc(fmeta_size);
    if (!meta || section_read(d, s, fmeta_off, meta, fmeta_size)) { free(meta); return NULL; }
    size_t fl = strlen(fname);
    uint8_t *buf = NULL;
    for (uint64_t p = 0; p + 0x20 <= fmeta_size;) {
        uint64_t doff, dsize;
        uint32_t nlen;
        memcpy(&doff, meta + p + 8, 8);
        memcpy(&dsize, meta + p + 16, 8);
        memcpy(&nlen, meta + p + 28, 4);
        if (nlen == fl && p + 0x20 + nlen <= fmeta_size && !memcmp(meta + p + 32, fname, fl)) {
            buf = malloc(dsize ? dsize : 1);
            if (buf && section_read(d, s, data_off + doff, buf, dsize)) free(buf), buf = NULL;
            *size = dsize;
            break;
        }
        p += (0x20 + nlen + 3) & ~3ull;
    }
    free(meta);
    return buf;
}

// ---- the dump -----------------------------------------------------------------------------------------------------
NxDump *nx_dump_open(const char *path, const char *keys_path, char *err, size_t errlen) {
    NxDump *d = calloc(1, sizeof *d);
    snprintf(d->keys_path, sizeof d->keys_path, "%s", keys_path);
    if (load_key(keys_path, "header_key", d->header_key, 32)) {
        fail(err, errlen, "Your prod.keys has no header_key (or couldn't be read).");
        free(d);
        return NULL;
    }
    d->fd = open(path, O_RDONLY);
    if (d->fd < 0) {
        fail(err, errlen, "The game file couldn't be opened.");
        free(d);
        return NULL;
    }
    uint8_t xh[0x200];
    size_t avail;
    if (!pread_all(d, xh, sizeof xh, 0) && !memcmp(xh + 0x100, "HEAD", 4)) {   // XCI
        uint64_t root;
        memcpy(&root, xh + 0x130, 8);
        uint8_t *t = read_table(d, root, &avail);
        FindCtx f = {"secure", 0, 0, 0};
        if (t) partition_walk(t, avail, root, find_entry, &f);
        free(t);
        uint8_t *s = f.found ? read_table(d, f.off, &avail) : NULL;
        if (s) partition_walk(s, avail, f.off, nca_add, d);
        free(s);
    } else if (!memcmp(xh, "PFS0", 4)) {   // NSP: tickets first (title keys), then the NCAs
        d->nsp = 1;
        uint8_t *t = read_table(d, 0, &avail);
        if (t) partition_walk(t, avail, 0, tik_add, d), partition_walk(t, avail, 0, nca_add, d);
        free(t);
    } else {
        fail(err, errlen, "That file isn't a Switch game dump (.xci or .nsp).");
        nx_dump_close(d);
        return NULL;
    }
    if (!d->nnca) {
        fail(err, errlen, "No game content could be decrypted: the dump is damaged or prod.keys doesn't match it.");
        nx_dump_close(d);
        return NULL;
    }
    return d;
}
void nx_dump_close(NxDump *d) {
    if (!d) return;
    if (d->fd >= 0) close(d->fd);
    free(d);
}

// The base game's NCA of a type: its title id ends in 000 (updates end in 800, DLC differ in the upper bits).
static Nca *find_nca(NxDump *d, int type, uint64_t title_id) {
    for (int i = 0; i < d->nnca; i++) {
        Nca *c = &d->nca[i];
        if (c->type != type || (c->title_id & 0xFFF) != 0) continue;
        if (title_id && c->title_id != title_id) continue;
        return c;
    }
    return NULL;
}

int nx_dump_info(NxDump *d, NxGameInfo *info, char *err, size_t errlen) {
    memset(info, 0, sizeof *info);
    Nca *prog = find_nca(d, NCA_PROGRAM, 0);
    if (!prog) { fail(err, errlen, "The dump has no game program in it."); return -1; }
    info->title_id = prog->title_id;
    strcpy(info->format, d->nsp ? "nsp" : "xci");
    Nca *meta = find_nca(d, NCA_META, prog->title_id);
    Section s;
    size_t n;
    uint8_t *cnmt = meta && !section_open(d, meta, 0, &s) ? pfs0_file(d, &s, "*.cnmt", &n) : NULL;
    if (cnmt && n >= 0x10) memcpy(&info->version, cnmt + 8, 4);
    free(cnmt);
    Nca *ctl = find_nca(d, NCA_CONTROL, prog->title_id);
    uint8_t *nacp = ctl && !section_open(d, ctl, 0, &s) ? romfs_file(d, &s, "control.nacp", &n) : NULL;
    if (nacp && n >= 0x4000) {
        memcpy(info->display_version, nacp + 0x3060, 15);
        for (int l = 0; l < 16 && !info->name[0]; l++) memcpy(info->name, nacp + l * 0x300, 0x200);
    }
    free(nacp);
    // The game's code must be there too (cut-down dumps keep only the start of the program).
    uint8_t head[16];
    if (section_open(d, prog, 0, &s) || section_read(d, &s, 0, head, 16) || memcmp(head, "PFS0", 4)) {
        fail(err, errlen, "This dump is incomplete: the game's code isn't in it. Use a full dump of the cartridge.");
        return -1;
    }
    return 0;
}

uint8_t *nx_dump_exefs_file(NxDump *d, const char *name, size_t *size) {
    Nca *prog = find_nca(d, NCA_PROGRAM, 0);
    Section s;
    if (!prog || section_open(d, prog, 0, &s)) return NULL;   // section 0 = ExeFS
    return pfs0_file(d, &s, name, size);
}

// ---- NSO ----------------------------------------------------------------------------------------------------------
// LZ4 block format (no frame).
static int lz4_block(const uint8_t *src, size_t n, uint8_t *dst, size_t cap) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t tok = src[i++];
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { if (i >= n) return -1; b = src[i++]; lit += b; } while (b == 255); }
        if (lit > n - i || lit > cap - o) return -1;
        memcpy(dst + o, src + i, lit), i += lit, o += lit;
        if (i >= n) break;   // the last sequence has literals only
        if (n - i < 2) return -1;
        size_t off = src[i] | src[i + 1] << 8;
        i += 2;
        if (!off || off > o) return -1;
        size_t len = (tok & 15) + 4;
        if ((tok & 15) == 15) { uint8_t b; do { if (i >= n) return -1; b = src[i++]; len += b; } while (b == 255); }
        if (len > cap - o) return -1;
        for (size_t k = 0; k < len; k++, o++) dst[o] = dst[o - off];   // may overlap
    }
    return o == cap ? 0 : -1;
}

uint8_t *nx_nso_image(const uint8_t *nso, size_t n, size_t *image_size, uint32_t seg[3][3], uint32_t *bss, char *err, size_t errlen) {
    if (n < 0x100 || memcmp(nso, "NSO0", 4)) { fail(err, errlen, "not an NSO"); return NULL; }
    uint32_t flags, s[3][3], fsize[3];
    memcpy(&flags, nso + 0xC, 4);
    size_t end = 0;
    for (int i = 0; i < 3; i++) {
        memcpy(s[i], nso + 0x10 + i * 0x10, 12);   // file offset, memory offset, size
        memcpy(&fsize[i], nso + 0x60 + i * 4, 4);
        if (s[i][1] + (size_t)s[i][2] > end) end = s[i][1] + (size_t)s[i][2];
    }
    if (end > (1u << 30)) { fail(err, errlen, "NSO too large"); return NULL; }
    uint8_t *img = calloc(1, end ? end : 1);
    for (int i = 0; i < 3 && img; i++) {
        size_t stored = flags >> i & 1 ? fsize[i] : s[i][2];
        if (s[i][0] > n || stored > n - s[i][0]) { fail(err, errlen, "NSO segment outside the file"); free(img); return NULL; }
        if (flags >> i & 1) {
            if (lz4_block(nso + s[i][0], stored, img + s[i][1], s[i][2])) { fail(err, errlen, "NSO segment doesn't decompress"); free(img); return NULL; }
        } else memcpy(img + s[i][1], nso + s[i][0], s[i][2]);
    }
    if (seg) memcpy(seg, s, sizeof s);
    if (bss) memcpy(bss, nso + 0x3C, 4);
    *image_size = end;
    return img;
}
