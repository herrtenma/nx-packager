// Art for Unkai's home screen, taken from the user's own RomFS while the game is built: a game's .unkai-port names the
// textures (wallpapers, the title logo's layers) by RomFS path and texture name; nothing of the game is in the port.
// Paths reach into containers: "Pack/Title.pack/Layout/Title.sblarc/timg/__Combined.bntx" opens the RomFS file
// Pack/Title.pack, then the SARC entries below it (Yaz0 decompressed on the way). Formats: RGBA8, R8, BC1-BC5, BC7.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Reads n bytes at off of a RomFS image; 0 = ok.
typedef int (*NxArtRead)(void *ctx, uint64_t off, void *dst, size_t n);

// The texture `name` (a BNTX texture name; the "^x" / "+x" suffix may be left out) of the BNTX at `path`, as RGBA8
// rows (w * 4 bytes each), the texture's channel mapping applied. malloc'd; NULL with a message in err.
uint8_t *nx_art_texture(NxArtRead rd, void *ctx, const char *path, const char *name, int *w, int *h, char *err, size_t errlen);

// Lists the textures of the BNTX files at or below `path` (a RomFS file, or a container path) to fn.
int nx_art_list(NxArtRead rd, void *ctx, const char *path, void (*fn)(void *fctx, const char *path, const char *name,
                int w, int h, const char *format), void *fctx, char *err, size_t errlen);

// Writes RGBA8 rows as a PNG, cropped to [x, y, cw, ch] when cw > 0. 0 = ok.
int nx_art_png(const char *out_path, const uint8_t *rgba, int w, int h, int x, int y, int cw, int ch);

// Resolves a port file (a .unkai-port zip, or a folder holding port.json) against a RomFS into out_dir: each texture
// it names becomes a PNG there, and out_dir/unkai.json is port.json with an "image" file name added to each of them.
// The port's own files (outlines/...) are copied beside it. 0 = ok.
int nx_art_resolve(const char *port, NxArtRead rd, void *ctx, const char *out_dir, char *err, size_t errlen);
