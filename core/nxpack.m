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

#include <mach-o/loader.h>

#include "nxcontent.h"
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

NSDictionary *NxDumpInfo(NSString *dumpPath, NSString *keysPath, NSString **error) {
    char err[256] = "";
    NxDump *d = nx_dump_open(dumpPath.fileSystemRepresentation, keysPath.fileSystemRepresentation, err, sizeof err);
    if (!d) return fail(error, @(err)), nil;
    NxGameInfo gi;
    int r = nx_dump_info(d, &gi, err, sizeof err);
    nx_dump_close(d);
    if (r) return fail(error, @(err)), nil;
    return @{@"title_id" : [NSString stringWithFormat:@"%016llX", (unsigned long long)gi.title_id], @"version" : @(gi.version),
             @"display_version" : @(gi.display_version), @"name" : @(gi.name), @"format" : @(gi.format)};
}

NSString *NxDumpMismatch(NSDictionary *manifest, NSDictionary *info) {
    NSDictionary *g = manifest[@"game"];
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

    NxZip *z = nx_zip_open(packPath.fileSystemRepresentation);
    char err[256] = "";
    NxDump *d = nx_dump_open(dumpPath.fileSystemRepresentation, keysPath.fileSystemRepresentation, err, sizeof err);
    if (!z || !d) {
        nx_zip_close(z), nx_dump_close(d);
        return fail(error, d ? @"The port pack couldn't be read." : @(err));
    }
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
        step([NSString stringWithFormat:@"Building %@", name], 0.05 + 0.75 * i / mods.count);
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
        uint64_t toff, tsize, doff, dsize;
        int slot = [mod[@"slot"] intValue];
        char tseg[17], dseg[17];
        snprintf(tseg, sizeof tseg, "__NX%dT", slot), snprintf(dseg, sizeof dseg, "__NX%dD", slot);
        if (ok && (!macho_section(bin, bin_n, tseg, "__text", &toff, &tsize) || !macho_section(bin, bin_n, dseg, "__data", &doff, &dsize) ||
                   tsize < [mod[@"text_size"] unsignedLongLongValue] || (img_n > tsize && img_n - tsize > dsize)))
            ok = fail(error, @"The port pack's app doesn't have room for the game (it was built for another version).");
        if (ok) {
            NSArray *hooks = mod[@"hooks"];
            for (uint64_t o = 0; o < tsize && o < img_n; o += 4) {
                BOOL hook = NO;
                for (NSDictionary *h in hooks)
                    hook |= o >= [h[@"offset"] unsignedLongLongValue] && o < [h[@"offset"] unsignedLongLongValue] + [h[@"size"] unsignedLongLongValue];
                if (!hook) memcpy(bin + toff + o, img + o, MIN(4, img_n - o));
            }
            if (img_n > tsize) memcpy(bin + doff, img + tsize, img_n - tsize);
        }
        free(img);
    }
    nx_dump_close(d);

    // 5: the IPA
    if (ok) {
        step(@"Writing the app", 0.82);
        NSString *tmp = [outPath stringByAppendingString:@".part"];
        NxZipWriter *w = nx_zip_create(tmp.fileSystemRepresentation);
        if (!w) ok = fail(error, @"The app couldn't be written (is there enough free space?).");
        int count = nx_zip_count(z);
        for (int i = 0; ok && i < count; i++) {
            const NxZipEntry *e = nx_zip_entry(z, i);
            if (strncmp(e->name, "Payload/", 8)) continue;
            step(@"Writing the app", 0.82 + 0.17 * i / count);
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
    if (ok) step(@"Done", 1);
    return ok;
}
