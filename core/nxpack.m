// See nxpack.h. The steps of NxBuildIPA:
//   1. the pack: nxport.json, the base app (Payload/), patches, the port's own modules
//   2. the dump: the game must be the pack's title and version; each module the pack needs from the ExeFS is
//      decompressed and must hash to the pack's input_sha256
//   3. each module: the patch's words written into the game's image (or the pack's own module), hashed against
//      output_sha256: the result is exactly the image the port was built and tested with
//   4. the app's executable: module i goes into the Mach-O sections __NX<i>T,__text and __NX<i>D,__data (zero-filled
//      slots of the right size, embed.py --empty), except the hook stubs (the port's code, already there)
//   5. the IPA: Payload/ with the filled executable, zipped. The installer (SideStore, AltStore, LiveContainer...)
//      signs it with the user's own certificate.
#import "nxpack.h"

#include <fcntl.h>
#include <mach-o/loader.h>
#include <unistd.h>

#include "nxart.h"
#include "nxcontent.h"
#include "nxtex.h"
#include "nxzip.h"

static NSString *hex(const uint8_t *p, size_t n) {
    NSMutableString *s = [NSMutableString string];
    for (size_t i = 0; i < n; i++) [s appendFormat:@"%02x", p[i]];
    return s;
}
static NSString *sha256_hex(const uint8_t *p, size_t n) {
    uint8_t h[32];
    nx_sha256(p, n, h);
    return hex(h, 32);
}
static BOOL fail(NSString **error, NSString *msg) {
    if (error) *error = msg;
    return NO;
}

NSDictionary *NxPackManifest(NSString *packPath, NSString **error) {
    NxZip *z = nx_zip_open(packPath.fileSystemRepresentation);
    if (!z) return fail(error, @"That file isn't a port pack (.nxport)."), nil;
    const NxZipEntry *e = nx_zip_find(z, "nxport.json");
    uint8_t *j = e ? nx_zip_read(z, e) : NULL;
    NSDictionary *m = j ? [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytesNoCopy:j length:e->size freeWhenDone:YES] options:0 error:nil] : nil;
    nx_zip_close(z);
    if (![m isKindOfClass:NSDictionary.class] || ![m[@"modules"] isKindOfClass:NSArray.class])
        return fail(error, @"That port pack is damaged (no nxport.json)."), nil;
    if ([m[@"format"] intValue] != 1)
        return fail(error, @"That port pack needs a newer version of the packager."), nil;
    return m;
}

NSString *NxPackBundleID(NSString *packPath) {
    NSDictionary *m = NxPackManifest(packPath, nil);
    NxZip *z = m ? nx_zip_open(packPath.fileSystemRepresentation) : NULL;
    if (!z) return nil;
    const NxZipEntry *e = nx_zip_find(z, [[m[@"app"][@"path"] stringByAppendingPathComponent:@"Info.plist"] UTF8String]);
    uint8_t *p = e ? nx_zip_read(z, e) : NULL;
    NSDictionary *info = p ? [NSPropertyListSerialization propertyListWithData:[NSData dataWithBytesNoCopy:p length:e->size freeWhenDone:YES]
                                                                       options:0 format:NULL error:nil] : nil;
    nx_zip_close(z);
    NSString *b = [info isKindOfClass:NSDictionary.class] ? info[@"CFBundleIdentifier"] : nil;
    return [b isKindOfClass:NSString.class] ? b : nil;
}

NSDictionary *NxDumpInfo(NSString *dumpPath, NSString *keysPath, NSString **error) {
    char err[256] = "";
    NxDump *d = nx_dump_open(dumpPath.fileSystemRepresentation, keysPath.fileSystemRepresentation, err, sizeof err);
    if (!d) return fail(error, @(err)), nil;
    NxGameInfo gi;
    int r = nx_dump_info(d, &gi, err, sizeof err);
    nx_dump_close(d);
    if (r) return fail(error, @(err)), nil;
    static NSString *const kinds[] = {@"base", @"update", @"aoc"};
    return @{@"title_id" : [NSString stringWithFormat:@"%016llX", (unsigned long long)gi.title_id], @"version" : @(gi.version),
             @"base_title_id" : [NSString stringWithFormat:@"%016llX", (unsigned long long)gi.base_title_id],
             @"kind" : kinds[gi.kind], @"display_version" : @(gi.display_version) ?: @"", @"name" : @(gi.name) ?: @"",
             @"format" : @(gi.format)};
}

NSString *NxDumpMismatch(NSDictionary *manifest, NSDictionary *info) {
    NSDictionary *g = manifest[@"game"];
    if (info[@"kind"] && ![info[@"kind"] isEqual:@"base"])
        return [info[@"kind"] isEqual:@"update"] ? @"This is an update, not the game itself." : @"This is add-on content (DLC), not the game itself.";
    if (![[info[@"title_id"] uppercaseString] isEqual:[g[@"title_id"] uppercaseString]])
        return [NSString stringWithFormat:@"This dump is %@ (%@), not %@.", [info[@"name"] length] ? info[@"name"] : @"another game",
                info[@"title_id"], g[@"name"]];
    if ([info[@"version"] intValue] != [g[@"version"] intValue])
        return [NSString stringWithFormat:@"This port needs %@ version %@; the dump has version %@. Dump the game without updates "
                                          @"(the cartridge or the base game).", g[@"name"], g[@"display_version"],
                [info[@"display_version"] length] ? info[@"display_version"] : info[@"version"]];
    NSArray *formats = manifest[@"dump_formats"];   // optional: the dump types the port's app can read at run time
    if ([formats isKindOfClass:NSArray.class] && ![formats containsObject:info[@"format"]])
        return [NSString stringWithFormat:@"This port needs the game as %@; this dump is an .%@.",
                [[formats valueForKey:@"uppercaseString"] componentsJoinedByString:@" or "], info[@"format"]];
    return nil;
}

// Mach-O: a section's file range by segment and section name.
static BOOL macho_section(const uint8_t *b, size_t n, const char *seg, const char *sect, uint64_t *off, uint64_t *size) {
    if (n < sizeof(struct mach_header_64)) return NO;
    const struct mach_header_64 *h = (const void *)b;
    if (h->magic != MH_MAGIC_64) return NO;
    size_t p = sizeof *h;
    for (uint32_t i = 0; i < h->ncmds && p + sizeof(struct load_command) <= n; i++) {
        const struct load_command *lc = (const void *)(b + p);
        if (lc->cmd == LC_SEGMENT_64 && p + sizeof(struct segment_command_64) <= n) {
            const struct segment_command_64 *sc = (const void *)lc;
            if (!strncmp(sc->segname, seg, 16)) {
                const struct section_64 *s = (const void *)(sc + 1);
                for (uint32_t k = 0; k < sc->nsects; k++)
                    if (!strncmp(s[k].sectname, sect, 16) && s[k].offset + (uint64_t)s[k].size <= n) {
                        *off = s[k].offset, *size = s[k].size;
                        return YES;
                    }
            }
        }
        p += lc->cmdsize;
    }
    return NO;
}

// Steps 2-5 with the game's modules from d (the base game's ExeFS, or an update's); progress from f0 to f1.
static BOOL write_ipa(NSString *packPath, NSDictionary *m, NxDump *d, NSString *outPath, void (^step)(NSString *, double), double f0,
                      double f1, NSString **error) {
    NxZip *z = nx_zip_open(packPath.fileSystemRepresentation);
    char err[256] = "";
    if (!z) return fail(error, @"The port pack couldn't be read.");
    NSString *app = m[@"app"][@"path"], *exe = [app stringByAppendingPathComponent:m[@"app"][@"executable"]];
    const NxZipEntry *exe_e = nx_zip_find(z, exe.UTF8String);
    uint8_t *bin = exe_e ? nx_zip_read(z, exe_e) : NULL;
    size_t bin_n = exe_e ? exe_e->size : 0;
    BOOL ok = bin != NULL;
    if (!ok) fail(error, @"The port pack's app is damaged.");

    // 2-4: each module into its slot
    NSArray *mods = m[@"modules"];
    for (NSUInteger i = 0; ok && i < mods.count; i++) {
        NSDictionary *mod = mods[i];
        NSString *name = mod[@"name"], *src = mod[@"source"];
        step([NSString stringWithFormat:@"Building %@", name], f0 + (f1 - f0) * (0.05 + 0.75 * i / mods.count));
        uint8_t *img = NULL;
        size_t img_n = 0;
        if ([src hasPrefix:@"exefs:"]) {
            size_t n;
            uint8_t *nso = nx_dump_exefs_file(d, [src substringFromIndex:6].UTF8String, &n);
            img = nso ? nx_nso_image(nso, n, &img_n, NULL, NULL, err, sizeof err) : NULL;
            free(nso);
            if (!img) { ok = fail(error, [NSString stringWithFormat:@"The game's %@ couldn't be read from the dump.", name]); break; }
            if (![sha256_hex(img, img_n) isEqual:mod[@"input_sha256"]]) {
                ok = fail(error, [NSString stringWithFormat:@"The game's %@ isn't the one this port was made for (the dump may be "
                                                            @"damaged, or modded).", name]);
                free(img);
                break;
            }
            NSArray *layout = mod[@"layout"];   // the port moved parts of the module: [from, to, size] ranges, the rest zero
            if ([layout isKindOfClass:NSArray.class] && layout.count) {
                size_t n = [mod[@"image_size"] unsignedLongLongValue];
                uint8_t *moved = n && n <= (1u << 30) ? calloc(1, n) : NULL;
                for (NSArray *r in layout) {
                    uint64_t from = [r[0] unsignedLongLongValue], to = [r[1] unsignedLongLongValue], len = [r[2] unsignedLongLongValue];
                    if (!moved || from + len > img_n || to + len > n) { free(moved), moved = NULL; break; }
                    memcpy(moved + to, img + from, len);
                }
                free(img);
                img = moved, img_n = n;
                if (!img) { ok = fail(error, @"The port pack's layout is damaged."); break; }
            }
            const NxZipEntry *pe = nx_zip_find(z, [mod[@"patch"] UTF8String]);
            uint8_t *p = pe ? nx_zip_read(z, pe) : NULL;
            uint32_t count = 0;
            if (p && pe->size >= 8 && !memcmp(p, "NXWP", 4)) memcpy(&count, p + 4, 4);
            if (!p || pe->size != 8 + (uint64_t)count * 8) ok = fail(error, @"The port pack's patch is damaged.");
            for (uint32_t k = 0; ok && k < count; k++) {
                uint32_t off, word;
                memcpy(&off, p + 8 + k * 8, 4);
                memcpy(&word, p + 12 + k * 8, 4);
                if (off + 4 > img_n) { ok = fail(error, @"The port pack's patch is damaged."); break; }
                memcpy(img + off, &word, 4);
            }
            free(p);
        } else {
            const NxZipEntry *me = nx_zip_find(z, src.UTF8String);
            img = me ? nx_zip_read(z, me) : NULL;
            img_n = me ? me->size : 0;
            if (!img) ok = fail(error, @"The port pack is damaged (a module is missing).");
        }
        if (ok && (img_n != [mod[@"image_size"] unsignedLongLongValue] || ![sha256_hex(img, img_n) isEqual:mod[@"output_sha256"]]))
            ok = fail(error, [NSString stringWithFormat:@"%@ didn't come out as expected; the port pack may be damaged.", name]);
        // the slot: text section first (hook stubs kept), the rest into the data section
        uint64_t toff = 0, tsize = 0, doff = 0, dsize = 0;
        int slot = [mod[@"slot"] intValue];
        char tseg[17], dseg[17];
        snprintf(tseg, sizeof tseg, "__NX%dT", slot), snprintf(dseg, sizeof dseg, "__NX%dD", slot);
        if (ok && (!macho_section(bin, bin_n, tseg, "__text", &toff, &tsize) || !macho_section(bin, bin_n, dseg, "__data", &doff, &dsize) ||
                   tsize < [mod[@"text_size"] unsignedLongLongValue] || (img_n > tsize && img_n - tsize > dsize)))
            ok = fail(error, @"The port pack's app doesn't have room for the game (it was built for another version).");
        if (ok) {
            NSArray *hooks = mod[@"hooks"];
            uint64_t *hr = malloc(sizeof *hr * (2 * hooks.count + 1));   // [start, end) of each hook stub
            for (NSUInteger k = 0; k < hooks.count; k++)
                hr[2 * k] = [hooks[k][@"offset"] unsignedLongLongValue], hr[2 * k + 1] = hr[2 * k] + [hooks[k][@"size"] unsignedLongLongValue];
            for (uint64_t o = 0; o < tsize && o < img_n; o += 4) {
                BOOL hook = NO;
                for (NSUInteger k = 0; k < hooks.count && !hook; k++) hook = o >= hr[2 * k] && o < hr[2 * k + 1];
                if (!hook) memcpy(bin + toff + o, img + o, MIN(4, img_n - o));
            }
            free(hr);
            if (img_n > tsize) memcpy(bin + doff, img + tsize, img_n - tsize);
        }
        // words of the image the port also runs elsewhere (a hook's displaced instructions, in its trampoline)
        NSDictionary *copies = mod[@"copies"];
        if (ok && [copies isKindOfClass:NSDictionary.class]) {
            NSArray *sect = [copies[@"section"] componentsSeparatedByString:@","];
            uint64_t coff = 0, csize = 0;
            if (sect.count != 2 || !macho_section(bin, bin_n, [sect[0] UTF8String], [sect[1] UTF8String], &coff, &csize))
                ok = fail(error, @"The port pack's app is damaged.");
            for (NSArray *w in ok ? copies[@"words"] : nil) {
                uint64_t from = [w[0] unsignedLongLongValue], to = [w[1] unsignedLongLongValue];
                if (from + 4 > img_n || to + 4 > csize) { ok = fail(error, @"The port pack is damaged."); break; }
                memcpy(bin + coff + to, img + from, 4);
            }
        }
        free(img);
    }

    // 5: the IPA
    if (ok) {
        step(@"Writing the app", f0 + (f1 - f0) * 0.82);
        NSString *tmp = [outPath stringByAppendingString:@".part"];
        NxZipWriter *w = nx_zip_create(tmp.fileSystemRepresentation);
        if (!w) ok = fail(error, @"The app couldn't be written (is there enough free space?).");
        int count = nx_zip_count(z);
        for (int i = 0; ok && i < count; i++) {
            const NxZipEntry *e = nx_zip_entry(z, i);
            if (strncmp(e->name, "Payload/", 8)) continue;
            step(@"Writing the app", f0 + (f1 - f0) * (0.82 + 0.17 * i / count));
            size_t l = strlen(e->name);
            if (e->name[l - 1] == '/') { ok = nx_zip_add(w, e->name, NULL, 0, e->mode ? e->mode : 040755, 1) == 0; continue; }
            BOOL is_exe = e == exe_e;
            uint8_t *data = is_exe ? bin : nx_zip_read(z, e);
            if (!data) { ok = fail(error, @"The port pack is damaged."); break; }
            uint32_t mode = e->mode ? e->mode : is_exe ? 0100755 : 0100644;
            ok = nx_zip_add(w, e->name, data, e->size, mode, 0) == 0;
            if (!is_exe) free(data);
        }
        if (w && nx_zip_finish(w)) ok = NO;
        if (!ok && error && !*error) *error = @"The app couldn't be written (is there enough free space?).";
        [NSFileManager.defaultManager removeItemAtPath:outPath error:nil];
        if (ok && ![NSFileManager.defaultManager moveItemAtPath:tmp toPath:outPath error:nil]) ok = fail(error, @"The app couldn't be saved.");
        if (!ok) [NSFileManager.defaultManager removeItemAtPath:tmp error:nil];
    }
    free(bin);
    nx_zip_close(z);
    return ok;
}

BOOL NxBuildIPA(NSString *packPath, NSString *dumpPath, NSString *keysPath, NSString *outPath,
                void (^progress)(NSString *step, double fraction), NSString **error) {
    void (^step)(NSString *, double) = ^(NSString *s, double f) { if (progress) progress(s, f); };
    step(@"Reading the port pack", 0);
    NSDictionary *m = NxPackManifest(packPath, error);
    if (!m) return NO;
    step(@"Checking your game", 0.02);
    NSDictionary *info = NxDumpInfo(dumpPath, keysPath, error);
    if (!info) return NO;
    NSString *mismatch = NxDumpMismatch(m, info);
    if (mismatch) return fail(error, mismatch);
    char err[256] = "";
    NxDump *d = nx_dump_open(dumpPath.fileSystemRepresentation, keysPath.fileSystemRepresentation, err, sizeof err);
    if (!d) return fail(error, @(err));
    BOOL ok = write_ipa(packPath, m, d, outPath, step, 0, 1, error);
    nx_dump_close(d);
    if (ok) step(@"Done", 1);
    return ok;
}

// ---- ports with game data -------------------------------------------------------------------------------------------
// A pack with "data" (docs/PORT_PACKS.md) also gets the game's files in a folder: its RomFS (base + the update the pack
// names, BC textures as ASTC when asked), a trimmed .xci (the program header and system data the port's runtime reads
// from the cartridge), each DLC's RomFS, and the user's prod.keys. The update's ExeFS gives the code.
static NSString *gb(uint64_t n) { return [NSString stringWithFormat:@"%.1f GB", n / 1e9]; }
static uint64_t free_space(NSString *dir) {
    NSURL *u = [NSURL fileURLWithPath:dir];
    NSNumber *v = nil;
    [u getResourceValue:&v forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil];
    if (!v.longLongValue) [u getResourceValue:&v forKey:NSURLVolumeAvailableCapacityKey error:nil];
    return v.unsignedLongLongValue;
}
static uint64_t file_size(NSString *p) { return [[NSFileManager.defaultManager attributesOfItemAtPath:p error:nil] fileSize]; }

NSDictionary *NxBuildPlan(NSString *packPath, NSArray<NSString *> *dumps, NSString *keysPath, NSString *outDir, NSString **error) {
    NSDictionary *m = NxPackManifest(packPath, error);
    if (!m) return nil;
    NSDictionary *g = m[@"game"], *upd = m[@"update"], *data = m[@"data"];
    NSString *base = nil, *update = nil, *problem = nil;
    NSMutableArray *aoc = [NSMutableArray array], *aocNames = [NSMutableArray array];
    NSMutableArray *notes = [NSMutableArray array];
    for (NSString *path in dumps) {
        NSString *err = nil;
        NSDictionary *i = NxDumpInfo(path, keysPath, &err);
        if (!i) { [notes addObject:[NSString stringWithFormat:@"%@: %@", path.lastPathComponent, err]]; continue; }
        if (![[i[@"base_title_id"] uppercaseString] isEqual:[g[@"title_id"] uppercaseString]]) {
            [notes addObject:[NSString stringWithFormat:@"%@ is for another game (%@).", path.lastPathComponent, i[@"base_title_id"]]];
            continue;
        }
        if ([i[@"kind"] isEqual:@"base"]) {
            NSString *mm = NxDumpMismatch(m, i);
            if (!mm) base = path;
            else if (!base) problem = mm;
        } else if ([i[@"kind"] isEqual:@"update"]) {
            if (upd && [i[@"version"] unsignedIntValue] == [upd[@"version"] unsignedIntValue]) update = path;
            else [notes addObject:[NSString stringWithFormat:@"%@ is update %@; this port needs %@.", path.lastPathComponent,
                                   i[@"display_version"], upd ? upd[@"display_version"] : @"none"]];
        } else if (data[@"aoc"]) [aoc addObject:path], [aocNames addObject:i[@"title_id"]];
    }
    if (!base && !problem)
        problem = [NSString stringWithFormat:@"Your own dump of %@ %@ (.xci) is needed.", g[@"name"], g[@"display_version"]];
    if (!problem && upd && !update)
        problem = [NSString stringWithFormat:@"The %@ update %@ (.nsp, from your own Switch) is needed.", g[@"name"], upd[@"display_version"]];
    // space: the RomFS grows a few % with ASTC (Yaz0 re-compressed); DLC the same; the stub and the app are small
    uint64_t romfs = 0, aocIn = 0;
    if (base && !problem && data[@"romfs"]) {
        char err[256] = "";
        NxDump *b = nx_dump_open(base.fileSystemRepresentation, keysPath.fileSystemRepresentation, err, sizeof err);
        NxDump *u = update ? nx_dump_open(update.fileSystemRepresentation, keysPath.fileSystemRepresentation, err, sizeof err) : NULL;
        NxStorage *st = b && (u || !update) ? nx_romfs_open(b, u, err, sizeof err) : NULL;
        if (st) romfs = nx_storage_size(st);
        else problem = @(err);
        nx_storage_close(st), nx_dump_close(u), nx_dump_close(b);
    }
    for (NSString *a in aoc) aocIn += file_size(a);
    uint64_t out = romfs + romfs / 32 + aocIn + aocIn / 32 + (200ull << 20), have = free_space(outDir);
    // using up the dump: the .xci shrinks as the RomFS is read; the update and DLC go once they are done
    uint64_t outConsume = romfs / 8 + aocIn + aocIn / 32 + (1500ull << 20);
    NSMutableDictionary *plan = [@{@"manifest" : m, @"aoc" : aoc, @"aoc_titles" : aocNames, @"notes" : notes,
                                   @"needed" : @(out), @"needed_consuming" : @(outConsume), @"free" : @(have)} mutableCopy];
    if (base) plan[@"base"] = base;
    if (update) plan[@"update"] = update;
    if (problem) plan[@"problem"] = problem;
    return plan;
}

typedef struct {
    int fd;                        // the .xci, opened to free its space
    uint64_t chunk, nchunks, *last;   // per chunk of the file: the RomFS offset after which it isn't read again
    uint64_t freed;
    char *done;
} Consume;
static void consume_range(void *ctx, uint64_t off, uint64_t len, uint64_t file_off) {
    Consume *c = ctx;
    for (uint64_t k = file_off / c->chunk; k <= (file_off + len - 1) / c->chunk && k < c->nchunks; k++)
        if (c->last[k] == UINT64_MAX || c->last[k] < off + len) c->last[k] = off + len;
}
static void punch(Consume *c, uint64_t k) {
    if (c->done[k]) return;
    c->done[k] = 1;
    struct fpunchhole ph = {.fp_flags = 0, .reserved = 0, .fp_offset = (off_t)(k * c->chunk), .fp_length = (off_t)c->chunk};
    if (!fcntl(c->fd, F_PUNCHHOLE, &ph)) c->freed += c->chunk;
}
static void consume_frontier(void *ctx, uint64_t romfs_off) {
    Consume *c = ctx;
    for (uint64_t k = 0; k < c->nchunks; k++)
        if (!c->done[k] && c->last[k] != UINT64_MAX && c->last[k] <= romfs_off) punch(c, k);
}

// Unkai's art for the port (the pack's "unkai" entry, a .unkai-port) from the game's RomFS into outDir. Only looks: a
// failure leaves the game without its art, the build goes on.
static int storage_read(void *ctx, uint64_t off, void *dst, size_t n) { return nx_storage_read(ctx, off, dst, n); }
static void write_art(NSString *packPath, NSDictionary *m, NxStorage *st, NSString *outDir) {
    NxZip *z = nx_zip_open(packPath.fileSystemRepresentation);
    const NxZipEntry *e = z && [m[@"unkai"] isKindOfClass:NSString.class] ? nx_zip_find(z, [m[@"unkai"] UTF8String]) : NULL;
    uint8_t *p = e ? nx_zip_read(z, e) : NULL;
    NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:@"port.unkai-port"];
    if (p && [[NSData dataWithBytesNoCopy:p length:e->size freeWhenDone:YES] writeToFile:tmp atomically:NO]) {
        char err[256] = "";
        NSString *part = [outDir stringByAppendingString:@".part"];
        [NSFileManager.defaultManager removeItemAtPath:part error:nil];
        if (!nx_art_resolve(tmp.fileSystemRepresentation, storage_read, st, part.fileSystemRepresentation, err, sizeof err)) {
            [NSFileManager.defaultManager removeItemAtPath:outDir error:nil];
            [NSFileManager.defaultManager moveItemAtPath:part toPath:outDir error:nil];
        } else {
            NSLog(@"nxpack: no Unkai art: %s", err);
            [NSFileManager.defaultManager removeItemAtPath:part error:nil];
        }
        [NSFileManager.defaultManager removeItemAtPath:tmp error:nil];
    }
    nx_zip_close(z);
}

typedef struct { void (^step)(NSString *, double); NSString *what; double f0, f1; Consume *consume; } RomfsCtx;
static void romfs_progress(void *ctx, uint64_t done, uint64_t total) {
    RomfsCtx *r = ctx;
    r->step(r->what, r->f0 + (r->f1 - r->f0) * (total ? (double)done / total : 1));
}
static void romfs_frontier(void *ctx, uint64_t off) {
    RomfsCtx *r = ctx;
    if (r->consume) consume_frontier(r->consume, off);
}

BOOL NxBuildPort(NSString *packPath, NSArray<NSString *> *dumps, NSString *keysPath, NSString *ipaPath, NSString *dataDir,
                 NSDictionary *options, void (^progress)(NSString *step, double fraction), NSString **error) {
    void (^step)(NSString *, double) = ^(NSString *s, double f) { if (progress) progress(s, f); };
    step(@"Checking your files", 0);
    NSDictionary *plan = NxBuildPlan(packPath, dumps, keysPath, dataDir.stringByDeletingLastPathComponent, error);
    if (!plan) return NO;
    if (plan[@"problem"]) return fail(error, plan[@"problem"]);
    NSDictionary *m = plan[@"manifest"], *data = m[@"data"];
    BOOL consume = [options[@"consume"] boolValue] && data;
    uint64_t need = [plan[consume ? @"needed_consuming" : @"needed"] unsignedLongLongValue], have = [plan[@"free"] unsignedLongLongValue];
    if (need > have)
        return fail(error, [NSString stringWithFormat:@"Not enough free space: the build needs about %@, %@ is free.%@", gb(need), gb(have),
                            consume || !data ? @"" : @" Turn on \"Free up space while building\" to use up the game dump as it is read."]);
    char err[256] = "";
    NSString *base = plan[@"base"], *update = plan[@"update"];
    NxDump *b = nx_dump_open(base.fileSystemRepresentation, keysPath.fileSystemRepresentation, err, sizeof err);
    NxDump *u = update ? nx_dump_open(update.fileSystemRepresentation, keysPath.fileSystemRepresentation, err, sizeof err) : NULL;
    if (!b || (update && !u)) { nx_dump_close(b), nx_dump_close(u); return fail(error, @(err)); }
    BOOL ok = YES;
    double fa = data ? 0.04 : 0.98;
    // 1. the app (the update's code when there is one)
    ok = write_ipa(packPath, m, u ? u : b, ipaPath, step, 0.01, fa, error);
    NSFileManager *fm = NSFileManager.defaultManager;
    Consume con = {.fd = -1};
    if (ok && data) {
        [fm createDirectoryAtPath:dataDir withIntermediateDirectories:YES attributes:nil error:nil];
        // 2. the trimmed .xci and prod.keys
        NSDictionary *stub = data[@"stub"];
        if (stub) {
            step(@"Writing the small game file", fa);
            uint64_t ids[64];
            int n = 0;
            for (NSString *t in stub[@"system_data"]) if (n < 64) ids[n++] = strtoull(t.UTF8String, NULL, 16);
            NSString *out = [dataDir stringByAppendingPathComponent:stub[@"file"]];
            if (nx_dump_write_stub(b, out.fileSystemRepresentation, ids, n, NULL, err, sizeof err)) ok = fail(error, @(err));
        }
        if (ok && [data[@"keys"] length]) {
            NSString *k = [dataDir stringByAppendingPathComponent:data[@"keys"]];
            [fm removeItemAtPath:k error:nil];
            if (![fm copyItemAtPath:keysPath toPath:k error:nil]) ok = fail(error, @"prod.keys couldn't be copied.");
        }
        // 3. the RomFS (from here on, with consume, the .xci is freed as it is read)
        NSDictionary *rd = data[@"romfs"];
        NxStorage *st = ok && rd ? nx_romfs_open(b, u, err, sizeof err) : NULL;
        if (ok && rd && !st) ok = fail(error, @(err));
        if (ok && st && [options[@"unkai_dir"] length]) {
            step(@"Taking the game's art", fa);
            write_art(packPath, m, st, options[@"unkai_dir"]);
        }
        if (ok && st && consume) {
            con.fd = open(base.fileSystemRepresentation, O_RDWR);
            uint64_t size = file_size(base);
            con.chunk = 16ull << 20, con.nchunks = (size + con.chunk - 1) / con.chunk;
            con.last = malloc(sizeof *con.last * (con.nchunks ? con.nchunks : 1)), con.done = calloc(con.nchunks ? con.nchunks : 1, 1);
            for (uint64_t k = 0; k < con.nchunks; k++) con.last[k] = UINT64_MAX;
            nx_storage_dump_ranges(st, consume_range, &con);
            if (con.fd < 0) ok = fail(error, @"The game dump can't be changed (it must be in the Packager folder to be used up).");
            else for (uint64_t k = 0; k < con.nchunks; k++) if (con.last[k] == UINT64_MAX) punch(&con, k);   // never read again
        }
        if (ok && st) {
            double f0 = fa + 0.01, f1 = [plan[@"aoc"] count] ? 0.9 : 0.99;
            RomfsCtx rc = {step, @"Converting the game data", f0, f1, consume ? &con : NULL};
            NxTexOptions o = {.convert = [rd[@"textures"] isEqual:@"astc"], .quality = 10, .rstb = [rd[@"size_table"] UTF8String],
                              .max_inflight = [options[@"max_inflight"] unsignedLongLongValue], .progress = romfs_progress,
                              .frontier = romfs_frontier, .ctx = &rc};
            NSString *out = [dataDir stringByAppendingPathComponent:rd[@"file"]];
            if (nx_romfs_write(st, out.fileSystemRepresentation, &o, NULL, err, sizeof err)) ok = fail(error, @(err));
        }
        nx_storage_close(st);
        // 4. DLC
        NSArray *aoc = plan[@"aoc"];
        NSString *aocDir = [dataDir stringByAppendingPathComponent:data[@"aoc"][@"dir"] ?: @"aoc"];
        for (NSUInteger i = 0; ok && i < aoc.count; i++) {
            NxDump *a = nx_dump_open([aoc[i] fileSystemRepresentation], keysPath.fileSystemRepresentation, err, sizeof err);
            for (int k = 0; ok && a && k < nx_dump_aoc_count(a); k++) {
                uint64_t tid;
                NxStorage *as = nx_dump_aoc_romfs(a, k, &tid, err, sizeof err);
                if (!as) { ok = fail(error, @(err)); break; }
                [fm createDirectoryAtPath:aocDir withIntermediateDirectories:YES attributes:nil error:nil];
                NSString *out = [aocDir stringByAppendingPathComponent:[NSString stringWithFormat:@"%016llX.romfs", (unsigned long long)tid]];
                RomfsCtx rc = {step, @"Converting the add-on content", 0.9 + 0.09 * i / aoc.count, 0.9 + 0.09 * (i + 1) / aoc.count, NULL};
                NxTexOptions o = {.convert = [data[@"aoc"][@"textures"] isEqual:@"astc"], .quality = 10, .rstb = [data[@"romfs"][@"size_table"] UTF8String],
                                  .max_inflight = [options[@"max_inflight"] unsignedLongLongValue], .progress = romfs_progress, .ctx = &rc};
                if (nx_romfs_write(as, out.fileSystemRepresentation, &o, NULL, err, sizeof err)) ok = fail(error, @(err));
                nx_storage_close(as);
            }
            if (!a) ok = fail(error, @(err));
            nx_dump_close(a);
            if (ok && consume) [fm removeItemAtPath:aoc[i] error:nil];
        }
    }
    nx_dump_close(u), nx_dump_close(b);
    if (con.fd >= 0) close(con.fd);
    free(con.last), free(con.done);
    if (consume && con.last) {   // the dump was partly used up: it can't be used again
        if (ok) [fm removeItemAtPath:base error:nil], [fm removeItemAtPath:update error:nil];
        else if (error && *error) *error = [*error stringByAppendingString:@" The game dump was partly used up and needs to be copied again."];
    }
    if (ok) step(@"Done", 1);
    return ok;
}
