// Reading a game dump the user owns: XCI or NSP -> NCAs -> ExeFS / meta / control, decrypted with the user's
// prod.keys (and the ticket's title key for NSPs). Layouts per switchbrew and hactool. Nothing is written to disk.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct NxDump NxDump;

// Opens a .xci or .nsp. On failure returns NULL and writes a message for the user into err.
NxDump *nx_dump_open(const char *path, const char *keys_path, char *err, size_t errlen);
void nx_dump_close(NxDump *d);

// What a dump holds: the base game, an update, or add-on content (DLC).
enum { NX_DUMP_BASE, NX_DUMP_UPDATE, NX_DUMP_AOC };
// The game in the dump: its program (the base game's, else an update's), or for DLC the first add-on's meta.
typedef struct {
    int kind;                  // NX_DUMP_*
    uint64_t base_title_id;    // the game's title id (an update's / DLC's own id differs)
    uint64_t title_id;
    uint32_t version;          // title version from the content meta (0 = 1.0.0 for most games)
    char display_version[16];  // from the control data (NACP), e.g. "1.0.0"
    char name[0x201];          // the game's name (first language that has one)
    char format[4];            // "xci" or "nsp"
} NxGameInfo;
int nx_dump_info(NxDump *d, NxGameInfo *info, char *err, size_t errlen);

// A whole ExeFS file ("main", "sdk", "rtld", "subsdk0", "main.npdm"...), malloc'd; NULL if it isn't there.
uint8_t *nx_dump_exefs_file(NxDump *d, const char *name, size_t *size);

// A RomFS as a readable storage: the program's (base + update when given: the update's patch applied), or a DLC's.
typedef struct NxStorage NxStorage;
NxStorage *nx_romfs_open(NxDump *d, NxDump *update, char *err, size_t errlen);
int nx_dump_aoc_count(NxDump *d);
NxStorage *nx_dump_aoc_romfs(NxDump *d, int index, uint64_t *title_id, char *err, size_t errlen);
uint64_t nx_storage_size(const NxStorage *st);
int nx_storage_read(NxStorage *st, uint64_t off, void *dst, size_t n);
void nx_storage_close(NxStorage *st);
// Where the RomFS's bytes lie in the first dump's file: fn(RomFS offset, length, file offset) per range taken from it
// (with an update: the parts of the base game the patch keeps). For freeing a dump's space as it is read.
void nx_storage_dump_ranges(const NxStorage *st, void (*fn)(void *ctx, uint64_t off, uint64_t len, uint64_t file_off), void *ctx);

// The trimmed .xci a port keeps beside its own RomFS file: the program NCA cut to its header and the RomFS's first
// bytes, and of the cartridge's update partition only the system data NCAs with these title ids. 0 = ok.
int nx_dump_write_stub(NxDump *d, const char *out_path, const uint64_t *sysdata, int nsys, uint64_t *out_size, char *err, size_t errlen);

// An NSO file -> its memory image (segments at their memory offsets, decompressed; bss not included), malloc'd.
// Also returns the segment table (file off, mem off, size per text / ro / data) and the bss size if wanted.
uint8_t *nx_nso_image(const uint8_t *nso, size_t n, size_t *image_size, uint32_t seg[3][3], uint32_t *bss,
                      char *err, size_t errlen);

void nx_sha256(const void *p, size_t n, uint8_t out[32]);
