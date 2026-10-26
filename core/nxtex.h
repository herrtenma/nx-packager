// A game's RomFS rewritten for the iPhone: BC textures (BNTX, inside Yaz0 / SARC / BFRES containers) become ASTC, which
// the iPhone's GPU samples natively (BC would be decoded to RGBA8 at run time: several times the memory). Everything
// else is copied as is. One streamed pass: files are read in RomFS order, converted on worker threads and written in
// order to a new RomFS image.
#pragma once
#include <stdint.h>

#include "nxcontent.h"

typedef struct {
    int convert;              // 0: copy only (the RomFS as it is)
    int threads;              // workers; 0 = one per CPU core
    float quality;            // astcenc effort, 0..100 (10 = fast)
    uint64_t max_inflight;    // file data held in memory at once (working copies included); 0 = 256 MB
    const char *rstb;         // the game's resource size table (BOTW: "System/Resource/ResourceSizeTable.product.srsizetable"):
                              // entries of resources that grew are raised (the game refuses a resource over its entry)
    void (*progress)(void *ctx, uint64_t done, uint64_t total);   // RomFS bytes read so far
    void (*frontier)(void *ctx, uint64_t romfs_off);   // every byte of the input RomFS below this has been read
    void *ctx;
} NxTexOptions;

typedef struct {
    uint64_t files, containers_changed;
    uint64_t textures[4], bytes[4];   // converted: BC1, BC4, BC3, BC5
    uint64_t skipped;                 // BC textures left as they were (no fitting ASTC layout, 3D)
    uint64_t rstb_raised, rstb_missing;
    uint64_t out_size;
} NxTexStats;

// Writes the RomFS of `in` to out_path (created; written to out_path.part, renamed when complete). 0 = ok.
int nx_romfs_write(NxStorage *in, const char *out_path, const NxTexOptions *o, NxTexStats *stats, char *err, size_t errlen);
