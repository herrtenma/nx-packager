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
    // decrypted in place: the 16-byte blocks wholly inside dst there, a partial block at either end through b
    uint8_t *out = dst, b[16];
    while (n) {
        uint64_t blk = at & ~15ull;
        size_t skip = (size_t)(at - blk), m = n;
        if (skip || n < 16) {
            if (pread_all(d, b, 16, s->nca->off + blk)) return -1;
            aes_ctr(s->nca->ctr_key, s->ctr_hi, b, 16, blk);
            m = 16 - skip < n ? 16 - skip : n;
            memcpy(out, b + skip, m);
        } else {
            m = n & ~15ull;
            if (pread_all(d, out, m, s->nca->off + at)) return -1;
            aes_ctr(s->nca->ctr_key, s->ctr_hi, out, m, at);
        }
        at += m, out += m, n -= m;
    }
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

// The NCAs of a title carry the program id of the game they belong to (an update's program and control NCAs have the
// base game's id); only the content meta has the title's own: the base game's ends in 000, an update's in 800, DLC is
// the base + 0x1000 + n. An update's program is told apart by its RomFS section: a patch (crypt type 4, BKTR).
static int is_patch(const Nca *c) { return c->type == NCA_PROGRAM && section_start(c, 1) && fs_header(c, 1)[0x4] == 4; }
static Nca *find_nca(NxDump *d, int type, int patch) {
    for (int i = 0; i < d->nnca; i++)
        if (d->nca[i].type == type && (type != NCA_PROGRAM || is_patch(&d->nca[i]) == patch)) return &d->nca[i];
    return NULL;
}
static Nca *find_meta(NxDump *d, uint32_t low_mask, uint32_t low) {
    for (int i = 0; i < d->nnca; i++)
        if (d->nca[i].type == NCA_META && ((d->nca[i].title_id & 0xFFF) & low_mask) == low) return &d->nca[i];
    return NULL;
}
// The program of the dump: the base game's, or else an update's.
static Nca *program_nca(NxDump *d) {
    Nca *c = find_nca(d, NCA_PROGRAM, 0);
    return c ? c : find_nca(d, NCA_PROGRAM, 1);
}

int nx_dump_info(NxDump *d, NxGameInfo *info, char *err, size_t errlen) {
    memset(info, 0, sizeof *info);
    strcpy(info->format, d->nsp ? "nsp" : "xci");
    Nca *prog = program_nca(d), *meta;
    if (!prog) {   // add-on content only: its title (the first DLC's) and the game it belongs to
        if (!(meta = find_meta(d, 0xFFF, 0)) && !(meta = find_meta(d, 0, 0))) {
            fail(err, errlen, "The dump has no game program in it.");
            return -1;
        }
        info->kind = NX_DUMP_AOC;
        for (int i = 0; i < d->nnca; i++)   // a DLC meta: low bits neither 000 nor 800
            if (d->nca[i].type == NCA_META && (d->nca[i].title_id & 0xFFF) && (d->nca[i].title_id & 0xFFF) != 0x800) meta = &d->nca[i];
        info->title_id = meta->title_id, info->base_title_id = (meta->title_id - 0x1000) & ~0xFFFull;
    } else {
        info->kind = is_patch(prog) ? NX_DUMP_UPDATE : NX_DUMP_BASE;
        info->title_id = info->base_title_id = prog->title_id & ~0xFFFull;
        meta = find_meta(d, 0xFFF, info->kind == NX_DUMP_UPDATE ? 0x800 : 0);
        if (meta) info->title_id = meta->title_id;
    }
    Section s;
    size_t n = 0;
    uint8_t *cnmt = meta && !section_open(d, meta, 0, &s) ? pfs0_file(d, &s, "*.cnmt", &n) : NULL;
    if (cnmt && n >= 0x10) memcpy(&info->version, cnmt + 8, 4);
    free(cnmt);
    if (!prog) return 0;
    // (newer NCAs, e.g. BOTW 1.9.0's control NCA, may LZ4-compress the section: compression info at fs header +0x178,
    // not read here; the name and display version are only for messages)
    Nca *ctl = find_nca(d, NCA_CONTROL, 0);
    static const uint8_t zero[0x28];
    if (ctl && memcmp(fs_header(ctl, 0) + 0x178, zero, sizeof zero)) ctl = NULL;
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
    Nca *prog = program_nca(d);   // an update carries the whole ExeFS of its version
    Section s;
    if (!prog || section_open(d, prog, 0, &s)) return NULL;   // section 0 = ExeFS
    return pfs0_file(d, &s, name, size);
}

// ---- RomFS storages -----------------------------------------------------------------------------------------------
// The program's RomFS (level 5 of section 1's IVFC tree). With an update: the update's section 1 is a patch (BKTR):
// a relocation table maps the virtual section (IVFC tree and RomFS of the new version) to ranges of the base section
// or of the update's own data, and a subsection table gives the update's data its AES-CTR counter per range (crypt
// type 4, "AesCtrEx": the counter's bytes 4-7 = the range's value). Both tables are 0x4000-byte buckets at the end of
// the update's section, read with the section's own counter. Layout per hactool (bktr).
typedef struct { uint64_t virt, phys; uint32_t patch; } Reloc;
typedef struct { uint64_t phys; uint32_t ctr; } Subsec;
struct NxStorage {
    NxDump *d, *upd;
    Section plain;                 // no update: the RomFS level of a section
    Section base, ext;             // with one: the base section whole (from its start), the update's section whole
    Reloc *rel;
    size_t nrel;
    Subsec *sub;
    size_t nsub;
    uint64_t vsize, romfs_off, romfs_size;
};
static uint64_t section_end(const Nca *c, int s) {
    uint32_t end;
    memcpy(&end, c->header + 0x244 + s * 16, 4);
    return (uint64_t)end * 0x200;
}
static int section_whole(NxDump *d, Nca *c, int s, Section *out) {
    if (!c->have_key) {
        if (nca_key(d, c)) return -1;
        c->have_key = 1;
    }
    const uint8_t *fs = fs_header(c, s);
    uint64_t start = section_start(c, s), end = section_end(c, s);
    if (!start || end <= start) return -1;
    memset(out, 0, sizeof *out);
    out->nca = c, out->data_off = start, out->size = end - start, out->encrypted = fs[0x4] >= 3;
    for (int j = 0; j < 8; j++) out->ctr_hi[j] = fs[0x140 + 7 - j];
    return 0;
}
// The update's data at a section offset: AES-CTR with the subsection's counter value.
static int ext_read(NxStorage *st, uint64_t off, uint8_t *dst, size_t n) {
    while (n) {
        size_t lo = 0, hi = st->nsub;   // last subsection starting at or before off
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (st->sub[mid].phys <= off) lo = mid; else hi = mid;
        }
        uint64_t end = lo + 1 < st->nsub ? st->sub[lo + 1].phys : st->ext.size;
        size_t m = end - off < n ? (size_t)(end - off) : n;
        if (!m) return -1;
        Section s = st->ext;
        uint32_t v = st->sub[lo].ctr;
        for (int j = 0; j < 4; j++) s.ctr_hi[7 - j] = (uint8_t)(v >> (8 * j));
        if (section_read(st->upd, &s, off, dst, m)) return -1;
        off += m, dst += m, n -= m;
    }
    return 0;
}
static int virt_read(NxStorage *st, uint64_t off, uint8_t *dst, size_t n) {
    while (n) {
        size_t lo = 0, hi = st->nrel;
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (st->rel[mid].virt <= off) lo = mid; else hi = mid;
        }
        const Reloc *r = &st->rel[lo];
        uint64_t end = lo + 1 < st->nrel ? st->rel[lo + 1].virt : st->vsize;
        size_t m = end - off < n ? (size_t)(end - off) : n;
        if (!m || off < r->virt) return -1;
        uint64_t phys = r->phys + (off - r->virt);
        if (r->patch ? ext_read(st, phys, dst, m) : section_read(st->d, &st->base, phys, dst, m)) return -1;
        off += m, dst += m, n -= m;
    }
    return 0;
}
// The bucket tables: header (u32, u32 buckets, u64 total, offsets) then 0x4000-byte buckets (u32, u32 entries, u64
// end, entries of esz bytes).
static void *bucket_table(NxStorage *st, const uint8_t *hdr, size_t esz, size_t *count) {
    uint64_t off, size;
    uint32_t magic;
    memcpy(&off, hdr, 8), memcpy(&size, hdr + 8, 8), memcpy(&magic, hdr + 16, 4);
    if (magic != 0x52544B42 || size < 0x4000 || size > (256u << 20)) return NULL;   // "BKTR"
    uint8_t *t = malloc(size);
    if (!t || section_read(st->upd, &st->ext, off, t, size)) { free(t); return NULL; }
    uint32_t nb;
    memcpy(&nb, t + 4, 4);
    size_t cap = 0, n = 0;
    uint8_t *out = NULL;
    for (uint32_t b = 0; b < nb && 0x4000 + (b + 1) * 0x4000ull <= size; b++) {
        const uint8_t *bk = t + 0x4000 + b * 0x4000ull;
        uint32_t ne;
        memcpy(&ne, bk + 4, 4);
        if (16 + (uint64_t)ne * esz > 0x4000) break;
        if (n + ne > cap) out = realloc(out, (cap = (n + ne) * 2) * esz);
        memcpy(out + n * esz, bk + 16, (size_t)ne * esz), n += ne;
    }
    free(t);
    *count = n;
    return out;
}
NxStorage *nx_romfs_open(NxDump *d, NxDump *update, char *err, size_t errlen) {
    NxStorage *st = calloc(1, sizeof *st);
    st->d = d, st->upd = update;
    Nca *bp = find_nca(d, NCA_PROGRAM, 0);
    if (!bp) { fail(err, errlen, "The dump has no game program in it."); free(st); return NULL; }
    if (!update) {
        if (section_open(d, bp, 1, &st->plain)) { fail(err, errlen, "The game's data (RomFS) couldn't be read."); free(st); return NULL; }
        st->romfs_size = st->plain.size;
        return st;
    }
    Nca *up = find_nca(update, NCA_PROGRAM, 1);
    if (!up || (up->title_id & ~0xFFFull) != (bp->title_id & ~0xFFFull)) {
        fail(err, errlen, "That update isn't for this game.");
        free(st);
        return NULL;
    }
    const uint8_t *fs = fs_header(up, 1);
    if (section_whole(d, bp, 1, &st->base) || section_whole(update, up, 1, &st->ext) || fs[0x3] != 3) {
        fail(err, errlen, "The update's data couldn't be read (prod.keys may be too old for it).");
        free(st);
        return NULL;
    }
    // the tables are encrypted with the section's own counter
    Reloc *rel = NULL;
    size_t nrel = 0, nsub = 0;
    uint8_t *re = bucket_table(st, fs + 0x100, 0x14, &nrel), *se = bucket_table(st, fs + 0x120, 0x10, &nsub);
    if (re) {
        rel = malloc(sizeof *rel * (nrel ? nrel : 1));
        for (size_t i = 0; i < nrel; i++)
            memcpy(&rel[i].virt, re + i * 0x14, 8), memcpy(&rel[i].phys, re + i * 0x14 + 8, 8), memcpy(&rel[i].patch, re + i * 0x14 + 16, 4);
    }
    if (se) {
        st->sub = malloc(sizeof *st->sub * (nsub ? nsub : 1));
        for (size_t i = 0; i < nsub; i++) memcpy(&st->sub[i].phys, se + i * 0x10, 8), memcpy(&st->sub[i].ctr, se + i * 0x10 + 12, 4);
    }
    free(re), free(se);
    st->rel = rel, st->nrel = nrel, st->nsub = nsub;
    // virtual size: the relocation block header's total (offset 8 of its first 16 bytes)
    uint8_t h16[16];
    uint64_t roff;
    memcpy(&roff, fs + 0x100, 8);
    if (!section_read(update, &st->ext, roff, h16, 16)) memcpy(&st->vsize, h16 + 8, 8);
    memcpy(&st->romfs_off, fs + 0x8 + 0x10 + 5 * 0x18, 8);
    memcpy(&st->romfs_size, fs + 0x8 + 0x10 + 5 * 0x18 + 8, 8);
    if (!rel || !nrel || !st->sub || !nsub || !st->vsize || st->romfs_off + st->romfs_size > st->vsize) {
        fail(err, errlen, "The update's patch tables couldn't be read (the update may be damaged).");
        nx_storage_close(st);
        return NULL;
    }
    return st;
}
uint64_t nx_storage_size(const NxStorage *st) { return st->romfs_size; }
int nx_storage_read(NxStorage *st, uint64_t off, void *dst, size_t n) {
    if (off > st->romfs_size || n > st->romfs_size - off) return -1;
    if (!st->upd) return section_read(st->d, &st->plain, off, dst, n);
    return virt_read(st, st->romfs_off + off, dst, n);
}
void nx_storage_close(NxStorage *st) {
    if (!st) return;
    free(st->rel), free(st->sub), free(st);
}
// Where the RomFS's bytes are in the first dump's file: fn(RomFS offset, length, file offset) for every range read from
// it (with an update, the ranges the patch takes from the base game; the update's own data isn't reported).
void nx_storage_dump_ranges(const NxStorage *st, void (*fn)(void *ctx, uint64_t off, uint64_t len, uint64_t file_off), void *ctx) {
    if (!st->upd) {
        fn(ctx, 0, st->romfs_size, st->plain.nca->off + st->plain.data_off);
        return;
    }
    uint64_t lo = st->romfs_off, hi = st->romfs_off + st->romfs_size;
    for (size_t i = 0; i < st->nrel; i++) {
        if (st->rel[i].patch) continue;
        uint64_t a = st->rel[i].virt, b = i + 1 < st->nrel ? st->rel[i + 1].virt : st->vsize;
        if (b <= lo || a >= hi) continue;
        uint64_t s = a > lo ? a : lo, e = b < hi ? b : hi;
        fn(ctx, s - lo, e - s, st->base.nca->off + st->base.data_off + st->rel[i].phys + (s - a));
    }
}

// ---- add-on content -----------------------------------------------------------------------------------------------
// DLC: data NCAs (content type 4 Data / 5 PublicData) with their own title id; section 0 is the RomFS.
int nx_dump_aoc_count(NxDump *d) {
    int n = 0;
    for (int i = 0; i < d->nnca; i++) n += (d->nca[i].type == 4 || d->nca[i].type == 5) && (d->nca[i].title_id & 0xFFF);
    return n;
}
NxStorage *nx_dump_aoc_romfs(NxDump *d, int index, uint64_t *title_id, char *err, size_t errlen) {
    for (int i = 0, k = 0; i < d->nnca; i++) {
        Nca *c = &d->nca[i];
        if (!((c->type == 4 || c->type == 5) && (c->title_id & 0xFFF)) || k++ != index) continue;
        NxStorage *st = calloc(1, sizeof *st);
        st->d = d;
        if (section_open(d, c, 0, &st->plain)) {
            fail(err, errlen, "The add-on content couldn't be decrypted (its ticket or prod.keys is missing).");
            free(st);
            return NULL;
        }
        st->romfs_size = st->plain.size, *title_id = c->title_id;
        return st;
    }
    fail(err, errlen, "No add-on content in that file.");
    return NULL;
}

// ---- the trimmed .xci -----------------------------------------------------------------------------------------------
// A port that reads the game's RomFS from its own file needs the cartridge only for the program NCA's header (keys,
// the RomFS section, a check of its first bytes) and for system data from the cartridge's update partition (shared
// fonts). The stub: the XCI header; a root HFS0 with "update" (only the wanted system data NCAs, whole) and "secure"
// (the program NCA cut to its header and the RomFS's first 0x200 bytes). Its header is rewritten: sections other
// than the RomFS dropped, the RomFS section placed right after the header with its data level at offset 0, the NCA
// size fixed; then encrypted again with header_key, and the RomFS bytes encrypted again for their new place (AES-CTR,
// same key and counter, new offset). The header's signatures no longer match: the port's runtime doesn't check them.
static void aes_xts_encrypt(const uint8_t *key32, uint8_t *buf, size_t size, uint64_t sector) {
    for (size_t s = 0; s < size; s += 0x200, sector++) {
        uint8_t tweak[16] = {0}, t[16];
        for (int i = 0; i < 8; i++) tweak[15 - i] = (uint8_t)(sector >> (8 * i));
        aes_ecb(0, key32 + 16, tweak, t, 16);
        for (size_t b = 0; b < 0x200; b += 16) {
            uint8_t x[16];
            for (int i = 0; i < 16; i++) x[i] = buf[s + b + i] ^ t[i];
            aes_ecb(0, key32, x, x, 16);
            for (int i = 0; i < 16; i++) buf[s + b + i] = x[i] ^ t[i];
            int carry = t[15] >> 7;
            for (int i = 15; i > 0; i--) t[i] = (uint8_t)(t[i] << 1 | t[i - 1] >> 7);
            t[0] = (uint8_t)(t[0] << 1) ^ (carry ? 0x87 : 0);
        }
    }
}
typedef struct { char name[128]; uint64_t off, size; } Ent;
typedef struct { Ent e[512]; int n; } EntList;
static int ent_add(void *ctx, const char *name, uint64_t off, uint64_t size) {
    EntList *l = ctx;
    if (l->n < 512) snprintf(l->e[l->n].name, sizeof l->e[0].name, "%s", name), l->e[l->n].off = off, l->e[l->n].size = size, l->n++;
    return 0;
}
static int partition_list(NxDump *d, uint64_t off, EntList *out) {
    size_t avail;
    uint8_t *t = read_table(d, off, &avail);
    out->n = 0;
    int r = t ? partition_walk(t, avail, off, ent_add, out) : -1;
    free(t);
    return r;
}
// An HFS0 table for files of the given sizes, packed 0x200-aligned in order; returns its size (0: no room).
static size_t hfs0_table(uint8_t *out, size_t cap, const char *const *names, const uint64_t *sizes, int n) {
    size_t strsz = 0;
    for (int i = 0; i < n; i++) strsz += strlen(names[i]) + 1;
    size_t tsize = (16 + (size_t)n * 0x40 + strsz + 0x1FF) & ~(size_t)0x1FF;   // the files start 0x200-aligned
    if (tsize > cap) return 0;
    strsz = tsize - 16 - (size_t)n * 0x40;
    memset(out, 0, tsize);
    uint32_t n32 = (uint32_t)n, s32 = (uint32_t)strsz;
    memcpy(out, "HFS0", 4), memcpy(out + 4, &n32, 4), memcpy(out + 8, &s32, 4);
    uint64_t pos = 0;
    uint32_t noff = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *e = out + 16 + i * 0x40;
        uint32_t hashed = sizes[i] < 0x200 ? (uint32_t)sizes[i] : 0x200;
        memcpy(e, &pos, 8), memcpy(e + 8, &sizes[i], 8), memcpy(e + 16, &noff, 4), memcpy(e + 20, &hashed, 4);
        strcpy((char *)out + 16 + n * 0x40 + noff, names[i]);
        noff += (uint32_t)strlen(names[i]) + 1;
        pos = (pos + sizes[i] + 0x1FF) & ~0x1FFull;
    }
    return tsize;
}
static int write_all(int fd, const void *p, size_t n) {
    for (size_t done = 0; done < n;) {
        ssize_t w = write(fd, (const uint8_t *)p + done, n - done);
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return 0;
}
static int pad_to(int fd, uint64_t *pos, uint64_t to) {
    static const uint8_t z[0x200];
    while (*pos < to) {
        size_t m = to - *pos < sizeof z ? (size_t)(to - *pos) : sizeof z;
        if (write_all(fd, z, m)) return -1;
        *pos += m;
    }
    return 0;
}
int nx_dump_write_stub(NxDump *d, const char *out_path, const uint64_t *sysdata, int nsys, uint64_t *out_size, char *err, size_t errlen) {
    if (d->nsp) { fail(err, errlen, "The game must be an .xci for this port."); return -1; }
    Nca *prog = find_nca(d, NCA_PROGRAM, 0);
    Section rs;
    uint8_t rom[0x200] = {0};
    if (!prog || section_open(d, prog, 1, &rs) || rs.size < sizeof rom || section_read(d, &rs, 0, rom, sizeof rom) ||
        rom[0] != 0x50 || memcmp(rom + 1, "\0\0\0\0\0\0\0", 7)) {
        fail(err, errlen, "The game's data couldn't be read from the dump.");
        return -1;
    }
    // the program NCA: header rewritten, then the RomFS's first 0x200 bytes at 0xC00
    uint8_t nca[0xE00];
    memcpy(nca, prog->header, 0xC00);
    uint64_t nsize = sizeof nca, zero = 0;
    memcpy(nca + 0x208, &nsize, 8);
    uint8_t fs1[0x200];
    memcpy(fs1, nca + 0x600, 0x200);
    memcpy(fs1 + 0x8 + 0x10 + 5 * 0x18, &zero, 8);   // the data level at the section's start
    memset(nca + 0x240, 0, 0x40), memset(nca + 0x280, 0, 0x80), memset(nca + 0x400, 0, 0x800);
    uint32_t start = 0xC00 / 0x200, end = start + 1;
    memcpy(nca + 0x250, &start, 4), memcpy(nca + 0x254, &end, 4), nca[0x258] = 1;
    memcpy(nca + 0x600, fs1, 0x200);
    nx_sha256(fs1, 0x200, nca + 0x2A0);
    memcpy(nca + 0xC00, rom, 0x200);
    aes_ctr(prog->ctr_key, rs.ctr_hi, nca + 0xC00, 0x200, 0xC00);   // CTR: encrypting = decrypting
    aes_xts_encrypt(d->header_key, nca, 0xC00, 0);
    // the program NCA's name in the secure partition; the system data NCAs of the update partition
    uint8_t xh[0x200];
    uint64_t root = 0, upd_off = 0, sec_off = 0;
    EntList *rootl = calloc(1, sizeof *rootl), *upd = calloc(1, sizeof *upd), *sec = calloc(1, sizeof *sec);
    int rc = -1, fd = -1, nu = 0;
    char part[2048];
    snprintf(part, sizeof part, "%s.part", out_path);
    const char *unames[64], *prog_name = "program.nca";
    uint64_t usizes[64], uoffs[64];
    if (!rootl || !upd || !sec || pread_all(d, xh, sizeof xh, 0)) goto bad;
    memcpy(&root, xh + 0x130, 8);
    if (partition_list(d, root, rootl)) goto bad;
    for (int i = 0; i < rootl->n; i++) {
        if (!strcmp(rootl->e[i].name, "update")) upd_off = rootl->e[i].off;
        if (!strcmp(rootl->e[i].name, "secure")) sec_off = rootl->e[i].off;
    }
    if (!sec_off || partition_list(d, sec_off, sec)) goto bad;
    for (int i = 0; i < sec->n; i++)
        if (sec->e[i].off == prog->off) prog_name = sec->e[i].name;
    if (upd_off && !partition_list(d, upd_off, upd))
        for (int i = 0; i < upd->n && nu < 64; i++) {
            uint8_t h[0xC00];
            uint64_t tid;
            if (upd->e[i].size < sizeof h || pread_all(d, h, sizeof h, upd->e[i].off)) continue;
            aes_xts_decrypt(d->header_key, h, sizeof h, 0);
            if (memcmp(h + 0x200, "NCA3", 4) || h[0x205] == NCA_META) continue;
            memcpy(&tid, h + 0x210, 8);
            for (int k = 0; k < nsys; k++)
                if (tid == sysdata[k]) {
                    unames[nu] = upd->e[i].name, usizes[nu] = upd->e[i].size, uoffs[nu] = upd->e[i].off, nu++;
                    break;
                }
        }
    if (nu < nsys) { fail(err, errlen, "The cartridge's system data (fonts) isn't in this dump."); goto out; }
    {
        uint8_t ut[0x4000], st[0x400], rt[0x400];
        size_t uts = hfs0_table(ut, sizeof ut, unames, usizes, nu);
        const char *sn[1] = {prog_name};
        uint64_t ss[1] = {sizeof nca};
        size_t sts = hfs0_table(st, sizeof st, sn, ss, 1);
        uint64_t upart = uts;
        for (int i = 0; i < nu; i++) upart = ((upart + 0x1FF) & ~0x1FFull) + usizes[i];
        const char *rn[2] = {"update", "secure"};
        uint64_t rsz[2] = {upart, sts + sizeof nca};
        size_t rts = hfs0_table(rt, sizeof rt, rn, rsz, 2);
        if (!uts || !sts || !rts) goto bad;
        uint64_t rootoff = sizeof xh, hsz = rts, pos = 0;
        memcpy(xh + 0x130, &rootoff, 8), memcpy(xh + 0x138, &hsz, 8);
        fd = open(part, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        if (fd < 0 || write_all(fd, xh, sizeof xh) || write_all(fd, rt, rts)) goto wbad;
        pos = sizeof xh + rts;
        uint64_t ubase = pos;   // the update partition
        if (write_all(fd, ut, uts)) goto wbad;
        pos += uts;
        uint8_t *buf = malloc(1 << 20);
        if (!buf) goto wbad;
        for (int i = 0; i < nu; i++) {
            if (pad_to(fd, &pos, ubase + ((pos - ubase + 0x1FF) & ~0x1FFull))) { free(buf); goto wbad; }
            for (uint64_t at = 0; at < usizes[i];) {
                size_t m = usizes[i] - at < (1 << 20) ? (size_t)(usizes[i] - at) : (1 << 20);
                if (pread_all(d, buf, m, uoffs[i] + at) || write_all(fd, buf, m)) { free(buf); goto wbad; }
                at += m, pos += m;
            }
        }
        free(buf);
        if (pad_to(fd, &pos, ubase + ((upart + 0x1FF) & ~0x1FFull))) goto wbad;   // the secure partition
        if (write_all(fd, st, sts) || write_all(fd, nca, sizeof nca)) goto wbad;
        pos += sts + sizeof nca;
        if (fsync(fd)) goto wbad;
        int c = close(fd);
        fd = -1;
        if (c || rename(part, out_path)) goto wbad;
        if (out_size) *out_size = pos;
        rc = 0;
        goto out;
    }
wbad:
    if (fd >= 0) close(fd);
    unlink(part);
    fail(err, errlen, "The small game file couldn't be written (is there enough free space?).");
    goto out;
bad:
    fail(err, errlen, "The cartridge dump's partitions couldn't be read.");
out:
    free(rootl), free(upd), free(sec);
    return rc;
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
