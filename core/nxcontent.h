// Reading a game dump the user owns: XCI or NSP -> NCAs -> ExeFS / meta / control, decrypted with the user's
// prod.keys (and the ticket's title key for NSPs). Layouts per switchbrew and hactool. Nothing is written to disk.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct NxDump NxDump;

// Opens a .xci or .nsp. On failure returns NULL and writes a message for the user into err.
NxDump *nx_dump_open(const char *path, const char *keys_path, char *err, size_t errlen);
void nx_dump_close(NxDump *d);

// The game in the dump (its base program; updates and DLC in an NSP are ignored).
typedef struct {
    uint64_t title_id;
    uint32_t version;          // title version from the content meta (0 = 1.0.0 for most games)
    char display_version[16];  // from the control data (NACP), e.g. "1.0.0"
    char name[0x201];          // the game's name (first language that has one)
    char format[4];            // "xci" or "nsp"
} NxGameInfo;
int nx_dump_info(NxDump *d, NxGameInfo *info, char *err, size_t errlen);

// A whole ExeFS file ("main", "sdk", "rtld", "subsdk0", "main.npdm"...), malloc'd; NULL if it isn't there.
uint8_t *nx_dump_exefs_file(NxDump *d, const char *name, size_t *size);

// An NSO file -> its memory image (segments at their memory offsets, decompressed; bss not included), malloc'd.
// Also returns the segment table (file off, mem off, size per text / ro / data) and the bss size if wanted.
uint8_t *nx_nso_image(const uint8_t *nso, size_t n, size_t *image_size, uint32_t seg[3][3], uint32_t *bss,
                      char *err, size_t errlen);

void nx_sha256(const void *p, size_t n, uint8_t out[32]);
