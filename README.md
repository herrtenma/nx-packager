# Packager

Builds native iOS ports of Switch games **on your iPhone**, from your own game dump and your own keys.

Ports made with this runtime don't ship any of the game: each one is released as a **port pack** (`.nxport`), its app
with the game's code left out, plus the small patch that adapts that code to iOS. The Packager reads your dump, checks
that it is exactly the version the port was made for, rebuilds the port's code from it and writes a ready-to-install
`.ipa`. Everything happens on your device; nothing is downloaded or uploaded.

> Not affiliated with Nintendo. The Packager contains no game content, keys or Nintendo code, and it is only useful
> with dumps and keys you made from your own console and games.

## Ports

| Port | Game (exact version needed) | Pack |
|---|---|---|
| [Tomodachi](https://github.com/herrtenma/tomodachi-life-ios) | Tomodachi Life: Living the Dream 1.0.0 (`010051F0207B2000`), `.xci` | `tomodachi.nxport` from its releases |

## Use

1. Install the Packager's `.ipa` (from [releases](../../releases)) with SideStore, AltStore or LiveContainer, and open
   it once: it makes its folder and shows where it is.
2. Put three files in that folder with the Files app:
   - inside LiveContainer: **Files › LiveContainer › Packager**
   - installed on its own (SideStore, AltStore): **Files › On My iPhone › Packager**

   | File | What |
   |---|---|
   | `<port>.nxport` | the port pack, from the port's releases |
   | your game dump | `.xci` (or `.nsp` if the port allows it); several dumps can sit there, the one that fits the pack is used |
   | `prod.keys` | dumped from your own Switch |
3. Back in the Packager (it looks again whenever you return to it, or tap **Check again**), each file gets a tick;
   tap **Build**. The finished `.ipa` is saved in the same folder.
4. Install that `.ipa` with SideStore, AltStore or LiveContainer, and follow the port's own instructions.

On a Mac, the same thing from the command line:

```sh
make                      # build/nxpack
build/nxpack --info tomodachi.nxport game.xci prod.keys      # check a dump against a pack
build/nxpack tomodachi.nxport game.xci prod.keys Tomodachi.ipa
```

## For port developers

The pack format is in [docs/PORT_PACKS.md](docs/PORT_PACKS.md).

Homeport, a launcher that merges the Packager into LiveContainer, is planned in [docs/LAUNCHER.md](docs/LAUNCHER.md),
with a UI prototype in [design/homeport.html](design/homeport.html).

## Source

| | |
|---|---|
| `core/nxcontent.c` | Reading `.xci` and `.nsp` dumps: HFS0 / PFS0, NCA decryption (key area and title keys), ExeFS, RomFS, NSO decompression (LZ4), game version from the content meta and control data |
| `core/nxzip.c` | Zip reading and writing (packs, IPAs) |
| `core/nxpack.m` | The build: checks, patches, slot filling, IPA |
| `cli/` | The Mac command-line tool |
| `ios/` | The iPhone app |

## License

[GNU General Public License v3.0](LICENSE). Container and crypto layouts follow the documentation of
[switchbrew](https://switchbrew.org) and [hactool](https://github.com/SciresM/hactool). Uses Apple's CommonCrypto and
zlib.
