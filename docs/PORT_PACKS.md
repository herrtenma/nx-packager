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
| `modules[].layout` | Optional: `[[from, to, size], ...]`. The port moved parts of the module (e.g. a segment): the image becomes `image_size` zero bytes with each range of the game's image copied from `from` to `to`, before the patch is applied. |
| `modules[].hooks` | Byte ranges of the `__text` section the Packager must not overwrite (the port's stubs). |
| `modules[].copies` | Optional: `{"section": "__TEXT,__nxorig", "words": [[from, to], ...]}`. Words of the finished image the port's app also runs elsewhere (e.g. the instructions a hook stub displaced, which the hook's trampoline runs before jumping back): each 4-byte word at image offset `from` is copied to offset `to` of that Mach-O section, which the shared app has zero-filled. |

## Ports that need the game's data (`update`, `data`)

A port whose runtime reads the game's files itself (instead of the user choosing the `.xci` in the app) gets them from
the Packager too, as a folder next to the IPA: the RomFS of the exact game version the port was made for, a trimmed
`.xci`, the DLC the user owns, and their `prod.keys`. Optional manifest sections:

```json
 "update": {"version": 1114112, "display_version": "1.9.0"},
 "data": {
  "folder": "botw",
  "romfs": {"file": "romfs.bin", "textures": "astc", "size_table": "System/Resource/ResourceSizeTable.product.srsizetable"},
  "stub": {"file": "game.xci", "system_data": ["0100000000000810", "0100000000000811", "0100000000000812",
                                               "0100000000000813", "0100000000000814"]},
  "aoc": {"dir": "aoc", "textures": "astc"},
  "keys": "prod.keys"
 }
```

| Field | Meaning |
|---|---|
| `update` | The update the port needs (`.nsp` from the user's Switch, title id = the game's + 0x800): its version. Modules with `exefs:` sources then come from the update's ExeFS, and the RomFS is the base game's with the update's patch applied. |
| `data.folder` | The folder's name (the port's own Documents folder on the phone). |
| `data.romfs` | Writes the game's RomFS (base + update) as one plain image, `file`. `textures: "astc"`: BC textures (BNTX in Yaz0 / SARC / BFRES) become ASTC, which iPhone GPUs sample natively; `size_table`: the game's resource size table, raised for every resource that grew (Breath of the Wild). |
| `data.stub` | A trimmed `.xci`: the cartridge's program NCA cut to its header and the RomFS's first bytes (header rewritten and encrypted again with the user's `header_key`), and of the cartridge's update partition only the system data NCAs listed (e.g. the shared fonts). Tens of MB instead of the whole cartridge, for a runtime that opens the `.xci` only for these. |
| `data.aoc` | Each DLC's RomFS as `<dir>/<title id>.romfs` (the DLC `.nsp` files the user put beside the dump), textures converted the same way. |
| `data.keys` | Copies the user's `prod.keys` into the folder under this name. |
| `unkai` | Optional: the name of a `.unkai-port` file in the pack (docs/UNKAI_PORT.md). Unkai's packager resolves it against the game's RomFS (base + update, before any texture conversion) into Unkai's `Ports/<bundle id>/`. Missing art never fails a build. |

Free space: about the RomFS's size plus the DLC's (the dumps stay). With the build option **consume** (the app's "Free up
space while building"), the `.xci` is used up as it is read: each 16 MB of it is released (APFS hole punching) once
nothing later reads it, and the dumps are deleted at the end, so the build needs only a few GB beyond the finished folder.
A build that fails then leaves a partly emptied dump that has to be copied again.

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
3. Writes each image into its slot of the app's executable (hook ranges kept), and the module's `copies`.
4. Zips `Payload/` into the IPA. The ad-hoc signature is no longer valid; the installer (SideStore, AltStore,
   LiveContainer) signs the app with the user's own certificate.

Nothing is downloaded or uploaded, and nothing of the dump is kept apart from the finished IPA.
