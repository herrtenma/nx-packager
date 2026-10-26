// nxpack: the packager on a Mac (the same core as the iPhone app).
//   nxpack <port.nxport> <game.xci|.nsp> <prod.keys> <out.ipa>
//   nxpack --info <port.nxport> [<game.xci|.nsp> <prod.keys>]
#import <Foundation/Foundation.h>
#import "nxpack.h"

#include "nxcontent.h"

// nxpack --romfs-compare <game.xci> <update.nsp|-> <prod.keys> <reference romfs.bin>: the RomFS the packager reads
// (base + update) against a known-good image, streamed (for testing the update path).
static int romfs_compare(const char *base, const char *upd, const char *keys, const char *ref) {
    char err[256] = "";
    NxDump *b = nx_dump_open(base, keys, err, sizeof err), *u = strcmp(upd, "-") ? nx_dump_open(upd, keys, err, sizeof err) : NULL;
    NxStorage *st = b && (u || !strcmp(upd, "-")) ? nx_romfs_open(b, u, err, sizeof err) : NULL;
    if (!st) { fprintf(stderr, "%s\n", err); return 1; }
    FILE *r = fopen(ref, "rb");
    if (!r) { perror(ref); return 1; }
    fseeko(r, 0, SEEK_END);
    uint64_t size = nx_storage_size(st), rsize = (uint64_t)ftello(r);
    fseeko(r, 0, SEEK_SET);
    printf("RomFS %llu bytes, reference %llu\n", (unsigned long long)size, (unsigned long long)rsize);
    size_t chunk = 16 << 20;
    uint8_t *a = malloc(chunk), *c = malloc(chunk);
    for (uint64_t off = 0; off < size; off += chunk) {
        size_t n = size - off < chunk ? (size_t)(size - off) : chunk;
        if (nx_storage_read(st, off, a, n)) { printf("read failed at %#llx\n", (unsigned long long)off); return 1; }
        if (fread(c, 1, n, r) != n || memcmp(a, c, n)) {
            size_t k = 0;
            while (k < n && a[k] == c[k]) k++;
            printf("differs at %#llx\n", (unsigned long long)(off + k));
            return 1;
        }
        if (off % (1ull << 30) == 0) fprintf(stderr, "  %.0f GB ok\n", off / 1e9);
    }
    printf("identical\n");
    return size == rsize ? 0 : 1;
}

#include <time.h>
#include "nxtex.h"
#include "nxart.h"
#include <fcntl.h>
#include <unistd.h>

// nxpack --romfs <game.xci> <update.nsp|-> <prod.keys> <out romfs.bin> [copy]: the RomFS with BC textures as ASTC
// (copy: unchanged), as the full build writes it.
static void romfs_progress(void *ctx, uint64_t done, uint64_t total) {
    static uint64_t last;
    if (done - last >= (1ull << 30) || done == total) last = done, fprintf(stderr, "  %.1f / %.1f GB\n", done / 1e9, total / 1e9);
}
static int romfs_write(const char *base, const char *upd, const char *keys, const char *out, int copy) {
    char err[256] = "";
    NxDump *b = nx_dump_open(base, keys, err, sizeof err), *u = strcmp(upd, "-") ? nx_dump_open(upd, keys, err, sizeof err) : NULL;
    NxStorage *st = b && (u || !strcmp(upd, "-")) ? nx_romfs_open(b, u, err, sizeof err) : NULL;
    if (!st) { fprintf(stderr, "%s\n", err); return 1; }
    NxTexOptions o = {.convert = !copy, .quality = 10, .rstb = "System/Resource/ResourceSizeTable.product.srsizetable",
                      .progress = romfs_progress};
    if (getenv("NXPACK_INFLIGHT_MB")) o.max_inflight = strtoull(getenv("NXPACK_INFLIGHT_MB"), NULL, 10) << 20;   // tuning
    if (getenv("NXPACK_THREADS")) o.threads = atoi(getenv("NXPACK_THREADS"));
    NxTexStats s;
    time_t t0 = time(NULL);
    if (nx_romfs_write(st, out, &o, &s, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    printf("%llu files, %llu containers changed; BC1 %llu, BC4 %llu, BC3 %llu, BC5 %llu textures; %llu skipped; RSTB %llu raised, "
           "%llu missing; %.2f GiB in %lds\n", s.files, s.containers_changed, s.textures[0], s.textures[1], s.textures[2], s.textures[3],
           s.skipped, s.rstb_raised, s.rstb_missing, s.out_size / 1073741824.0, (long)(time(NULL) - t0));
    return 0;
}

// nxpack --stub <game.xci> <prod.keys> <out.xci> <system data title id>...: the trimmed .xci.
static int stub_write(int argc, char **argv) {
    char err[256] = "";
    NxDump *d = nx_dump_open(argv[2], argv[3], err, sizeof err);
    if (!d) { fprintf(stderr, "%s\n", err); return 1; }
    uint64_t ids[64], size = 0;
    int n = 0;
    for (int i = 5; i < argc && n < 64; i++) ids[n++] = strtoull(argv[i], NULL, 16);
    if (nx_dump_write_stub(d, argv[4], ids, n, &size, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    printf("%s: %.1f MB\n", argv[4], size / 1048576.0);
    return 0;
}

// nxpack --aoc <dlc.nsp> <prod.keys> <out dir> [copy]: each DLC's RomFS as <out dir>/<title id>.romfs (textures as ASTC).
static int aoc_write(const char *nsp, const char *keys, const char *dir, int copy) {
    char err[256] = "";
    NxDump *d = nx_dump_open(nsp, keys, err, sizeof err);
    if (!d) { fprintf(stderr, "%s\n", err); return 1; }
    for (int k = 0; k < nx_dump_aoc_count(d); k++) {
        uint64_t tid;
        NxStorage *st = nx_dump_aoc_romfs(d, k, &tid, err, sizeof err);
        if (!st) { fprintf(stderr, "%s\n", err); return 1; }
        char out[2048];
        snprintf(out, sizeof out, "%s/%016llX.romfs", dir, (unsigned long long)tid);
        NxTexOptions o = {.convert = !copy, .quality = 10, .rstb = "System/Resource/ResourceSizeTable.product.srsizetable"};
        if (getenv("NXPACK_INFLIGHT_MB")) o.max_inflight = strtoull(getenv("NXPACK_INFLIGHT_MB"), NULL, 10) << 20;   // tuning
        if (getenv("NXPACK_THREADS")) o.threads = atoi(getenv("NXPACK_THREADS"));
        NxTexStats s;
        if (nx_romfs_write(st, out, &o, &s, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
        printf("%s: %llu files, %llu textures, RSTB %llu raised / %llu missing, %.1f MB\n", out, s.files,
               s.textures[0] + s.textures[1] + s.textures[2] + s.textures[3], s.rstb_raised, s.rstb_missing, s.out_size / 1048576.0);
        nx_storage_close(st);
    }
    return 0;
}

// Unkai art from a plain RomFS image (romfs.bin): nxpack --tex-list <romfs.bin> <path>, --tex <romfs.bin> <path> <texture>
// <out.png>, --unkai <port.unkai-port|folder> <romfs.bin> <out folder>. (The phone build reads the dump's RomFS instead.)
static int file_read(void *ctx, uint64_t off, void *dst, size_t n) { return pread(*(int *)ctx, dst, n, (off_t)off) == (ssize_t)n ? 0 : -1; }
static void tex_print(void *ctx, const char *path, const char *name, int w, int h, const char *format) {
    printf("%5dx%-5d %-6s %s  %s\n", w, h, format, name, path);
}
static int art_main(int argc, char **argv) {
    char err[512] = "";
    int fd = open(argv[!strcmp(argv[1], "--unkai") ? 3 : 2], O_RDONLY);
    if (fd < 0) { perror("romfs"); return 1; }
    int r = 0;
    if (!strcmp(argv[1], "--tex-list")) r = nx_art_list(file_read, &fd, argv[3], tex_print, NULL, err, sizeof err);
    else if (!strcmp(argv[1], "--tex")) {
        int w, h;
        uint8_t *rgba = nx_art_texture(file_read, &fd, argv[3], argv[4], &w, &h, err, sizeof err);
        r = rgba ? nx_art_png(argv[5], rgba, w, h, 0, 0, 0, 0) : -1;
        if (rgba) printf("%s: %dx%d\n", argv[5], w, h);
        free(rgba);
    } else r = nx_art_resolve(argv[2], file_read, &fd, argv[4], err, sizeof err);
    close(fd);
    if (r) fprintf(stderr, "%s\n", err[0] ? err : "failed");
    return r ? 1 : 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *err = nil;
        if ((argc == 5 || argc == 6) && !strcmp(argv[1], "--aoc")) return aoc_write(argv[2], argv[3], argv[4], argc == 6);
        // nxpack --build <port.nxport> <prod.keys> <out.ipa> <game folder> [--consume] <dump>...: a port with game data
        if (argc >= 7 && !strcmp(argv[1], "--build")) {
            int i = 6, consume = !strcmp(argv[6], "--consume");
            NSMutableArray *dumps = [NSMutableArray array];
            for (i += consume; i < argc; i++) [dumps addObject:@(argv[i])];
            NSDictionary *plan = NxBuildPlan(@(argv[2]), dumps, @(argv[3]), @(argv[5]).stringByDeletingLastPathComponent, &err);
            if (!plan) { fprintf(stderr, "%s\n", err.UTF8String); return 1; }
            for (NSString *n in plan[@"notes"]) fprintf(stderr, "note: %s\n", n.UTF8String);
            fprintf(stderr, "base %s, update %s, %lu DLC; needs %.1f GB (%.1f GB using up the dump), %.1f GB free\n",
                    [plan[@"base"] lastPathComponent].UTF8String ?: "-", [plan[@"update"] lastPathComponent].UTF8String ?: "-",
                    (unsigned long)[plan[@"aoc"] count], [plan[@"needed"] doubleValue] / 1e9, [plan[@"needed_consuming"] doubleValue] / 1e9,
                    [plan[@"free"] doubleValue] / 1e9);
            __block NSString *last = nil;
            __block int lastPct = -1;
            BOOL ok = NxBuildPort(@(argv[2]), dumps, @(argv[3]), @(argv[4]), @(argv[5]), @{@"consume" : @(consume)}, ^(NSString *step, double f) {
                if (![step isEqual:last] || (int)(f * 100) != lastPct) fprintf(stderr, "%3.0f%% %s\n", f * 100, step.UTF8String), last = step, lastPct = (int)(f * 100);
            }, &err);
            if (!ok) { fprintf(stderr, "error: %s\n", err.UTF8String); return 1; }
            printf("%s\n%s\n", argv[4], argv[5]);
            return 0;
        }
        if ((argc == 4 && !strcmp(argv[1], "--tex-list")) || (argc == 6 && !strcmp(argv[1], "--tex")) ||
            (argc == 5 && !strcmp(argv[1], "--unkai")))
            return art_main(argc, argv);
        if (argc >= 5 && !strcmp(argv[1], "--stub")) return stub_write(argc, argv);
        if (argc == 6 && !strcmp(argv[1], "--romfs-compare")) return romfs_compare(argv[2], argv[3], argv[4], argv[5]);
        if ((argc == 6 || argc == 7) && !strcmp(argv[1], "--romfs"))
            return romfs_write(argv[2], argv[3], argv[4], argv[5], argc == 7 && !strcmp(argv[6], "copy"));
        if (argc >= 3 && !strcmp(argv[1], "--info")) {
            NSDictionary *m = NxPackManifest(@(argv[2]), &err);
            if (!m) { fprintf(stderr, "%s\n", err.UTF8String); return 1; }
            NSDictionary *g = m[@"game"];
            printf("%s %s: needs %s %s (title %s, version %d)\n", [m[@"port_name"] UTF8String], [m[@"port_version"] UTF8String],
                   [g[@"name"] UTF8String], [g[@"display_version"] UTF8String], [g[@"title_id"] UTF8String], [g[@"version"] intValue]);
            if (argc >= 5) {
                NSDictionary *i = NxDumpInfo(@(argv[3]), @(argv[4]), &err);
                if (!i) { fprintf(stderr, "%s\n", err.UTF8String); return 1; }
                NSString *mm = NxDumpMismatch(m, i);
                printf("dump: %s %s (title %s, version %d): %s\n", [i[@"name"] UTF8String], [i[@"display_version"] UTF8String],
                       [i[@"title_id"] UTF8String], [i[@"version"] intValue], mm ? mm.UTF8String : "OK");
            }
            return 0;
        }
        if (argc != 5) {
            fprintf(stderr, "usage: nxpack <port.nxport> <game.xci|.nsp> <prod.keys> <out.ipa>\n       nxpack --info <port.nxport> [<game> <prod.keys>]\n");
            return 2;
        }
        __block NSString *last = nil;
        BOOL ok = NxBuildIPA(@(argv[1]), @(argv[2]), @(argv[3]), @(argv[4]), ^(NSString *step, double f) {
            if (![step isEqual:last]) fprintf(stderr, "%3.0f%% %s\n", f * 100, step.UTF8String), last = step;
        }, &err);
        if (!ok) { fprintf(stderr, "error: %s\n", err.UTF8String); return 1; }
        printf("%s\n", argv[4]);
        return 0;
    }
}
