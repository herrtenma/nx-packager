# Port packs (`.nxport`)

A port pack is everything the Packager needs to turn a user's own game dump into one port's app, without containing
any of the game. It is a zip file:

```
nxport.json                the manifest (below)
Payload/<app>.app/         the port's app with EMPTY game slots (no game code; signed ad hoc)
patches/<module>.nxwp      the words the port changes in a game module
modules/<module>.img       modules that are entirely the port's own (e.g. trampolines)
```

## How a port is laid out

The port's executable holds every module of the game it needs in two Mach-O sections, in a fixed order (module
*i* = slot *i*):

- `__NX<i>T,__text` (r-x): the module's memory image from 0 up to its text size rounded up to 16 KB, and
- `__NX<i>D,__data` (rw-): the rest of the image; bss follows as zero fill.

The released app has these sections zero-filled, except for **hook stubs**: 16-byte jumps to the port's own
functions, which the Packager leaves alone. (In the ports built from this runtime, `tools/nxrecomp/embed.py --empty`
generates the sections from the port's `layout.json`.)

A module's *image* is the NSO's segments placed at their memory offsets, decompressed, bss not included: what
`nx_nso_image()` in `core/nxcontent.c` returns.

## `nxport.json`

```json
{
 "format": 1,
 "id": "tomodachi-life",
 "port_name": "Tomodachi",
 "port_version": "0.1.0",
 "homepage": "https://github.com/herrtenma/tomodachi-life-ios",
 "game": {
  "name": "Tomodachi Life: Living the Dream",
  "title_id": "010051F0207B2000",
  "version": 0,
  "display_version": "1.0.0"
 },
 "dump_formats": ["xci"],
 "app": {"path": "Payload/nxrt.app", "executable": "nxrt", "ipa_name": "Tomodachi-0.1.0.ipa"},
 "after_install": "Open the app and choose the same .xci (and prod.keys) once.",
 "modules": [
  {"name": "main", "slot": 1, "source": "exefs:main", "input_sha256": "e5cb…", "patch": "patches/main.nxwp",
   "image_size": 57333188, "text_size": 39642784, "output_sha256": "cb5a…",
   "hooks": [{"offset": 4636800, "size": 16}]},
  {"name": "subsdk9", "slot": 2, "source": "modules/subsdk9.img", "image_size": 1589536, "text_size": 1572864,
   "output_sha256": "…", "hooks": []}
 ]
}
```

| Field | Meaning |
|---|---|
| `format` | 1. A Packager refuses packs of a format it doesn't know. |
| `game.title_id`, `game.version` | The dump's base program must have this title ID and this title version (from its content meta). `display_version` and `name` are for messages. |
| `dump_formats` | Optional: `"xci"`, `"nsp"`. The dump types the port's app can read at run time, if it reads the dump itself. |
| `app` | Where the app is in the pack, its executable's name, and the name of the IPA to write. |
| `after_install` | Shown to the user once the IPA is built. |
| `modules[].source` | `exefs:<file>`: the game's module from the dump's ExeFS. Its image must hash to `input_sha256`; then the `.nxwp` patch is applied. Any other value: a file in the pack used as is. |
| `modules[].output_sha256` | The result must hash to this before it goes into the slot: the app gets exactly the code the port was built and tested with. |
| `modules[].hooks` | Byte ranges of the `__text` section the Packager must not overwrite (the port's stubs). |

## Patch files (`.nxwp`)

```
"NXWP"  u32 count  then count x { u32 offset in the image, u32 new word }     (little-endian)
```

Only the words the port changes: the port's own instructions and data, never a copy of the game's bytes. The ports
built from this runtime generate them with `tools/nxrecomp/mkpack.py`.

## What the Packager does

1. Reads `nxport.json`; checks the dump: title ID, version, format.
2. For each module: extracts and decompresses it from the dump's ExeFS (or takes it from the pack), checks
   `input_sha256`, applies the patch, checks `output_sha256`.
3. Writes each image into its slot of the app's executable (hook ranges kept).
4. Zips `Payload/` into the IPA. The ad-hoc signature is no longer valid; the installer (SideStore, AltStore,
   LiveContainer) signs the app with the user's own certificate.

Nothing is downloaded or uploaded, and nothing of the dump is kept apart from the finished IPA.
