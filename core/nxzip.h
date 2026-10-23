// Minimal zip reading and writing for port packs (.nxport) and IPAs: stored and deflated entries, no zip64 (packs and
// IPAs stay well under 4 GB), Unix file modes kept (an IPA's executable must stay executable).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct NxZip NxZip;
typedef struct {
    char *name;
    uint32_t crc, csize, size, local_off, mode;   // mode: Unix st_mode (0 if the zip didn't record one)
    uint16_t method;
} NxZipEntry;

NxZip *nx_zip_open(const char *path);
void nx_zip_close(NxZip *z);
int nx_zip_count(const NxZip *z);
const NxZipEntry *nx_zip_entry(const NxZip *z, int i);
const NxZipEntry *nx_zip_find(const NxZip *z, const char *name);
uint8_t *nx_zip_read(NxZip *z, const NxZipEntry *e);   // malloc'd, e->size bytes; NULL on a bad entry (CRC checked)

typedef struct NxZipWriter NxZipWriter;
NxZipWriter *nx_zip_create(const char *path);
// Adds a file (deflated unless store) or, with data NULL and a name ending in '/', a directory.
int nx_zip_add(NxZipWriter *w, const char *name, const void *data, size_t n, uint32_t mode, int store);
int nx_zip_finish(NxZipWriter *w);   // writes the central directory and closes; 0 = ok
