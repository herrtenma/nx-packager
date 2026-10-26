# .unkai-port: a game's look in Unkai

Unkai (the home screen, ~/Desktop/unkai: a LiveContainer fork) shows each game with its own art: wallpapers that cycle
behind the home screen and the game's title as a layered 3D emblem. A port ships a `.unkai-port` that **names** that
art inside the game's RomFS; the packager pulls it from the user's own dump while it builds the game
(`core/nxart.m`), so the port file holds no game content.

A `.unkai-port` is a zip (or, while authoring, a folder) with `port.json` and optional files of the port's own
(`outlines/*.svg`, ...) that are copied as they are.

```json
{
  "format": 1,
  "title_id": "01007EF00011E000",
  "versions": ["1.9.0"],
  "name": "The Legend of Zelda: Breath of the Wild",
  "short_name": "Breath of the Wild",
  "accent": "#4FB6C8",
  "cycle_seconds": 20,
  "wallpapers": [
    {"focus": [0.5, 0.55], "tone": "light", "layers": [
      {"file": "Pack/Title.pack/Layout/Title.sblarc/timg/__Combined.bntx", "texture": "TitleBGMainB_00", "depth": 0},
      {"file": "Pack/Title.pack/Layout/Title.sblarc/timg/__Combined.bntx", "texture": "TitleBGMainB_01", "depth": 1}]}
  ],
  "emblem": {"aspect": 1.346, "side": "#6E5420", "layers": [
    {"file": "...", "texture": "Logo3DGameTitle_00", "keep": {"hue": [30, 75], "saturation": [0.05, 0.45], "value": [0.5, 1.1]},
     "tint": ["#F7D77E", "#B07F2A"], "z": 0, "extrude": 10, "side": "#7A5A22"}]},
  "achievements": []
}
```

Any object with `file` + `texture` is an image: `file` is a RomFS path that may continue into containers (Yaz0 and
SARC are opened on the way), `texture` a BNTX texture name (the `^x` suffix may be left out). Optional per image:

| key | meaning |
|---|---|
| `crop` | `[x, y, w, h]` in texels |
| `keep` | keep only pixels in an HSV box: `hue` (degrees), `saturation`, `value` ranges (soft edges; use an upper bound above 1 to include 1), `invert`, `region` `[x, y, w, h]` (outside it is cleared; the image keeps its size). For splitting one logo texture into layers |
| `tint` | `["#top", "#bottom"]`: RGB multiplied by a vertical gradient |

Wallpapers: `layers` back to front, `depth` 0 = background (deeper layers drift further: parallax), `focus` = the
point kept in view while cropping and drifting, `tone` light / dark. Emblem layers: `z` (points in front of the card
plane; negative = behind), `extrude` (thickness in points), `side` (extrusion colour). Formats read: RGBA8, R8,
BC1-BC5, BC7.

The packager writes each image as `artN.png` next to `unkai.json` (port.json with `"image": "artN.png"` added) in
LiveContainer's `Documents/Unkai/Ports/<bundle id>/`, where Unkai reads it.

Authoring on the Mac against a plain RomFS image:

```sh
build/nxpack --tex-list romfs.bin Pack/Title.pack                       # every texture below a path
build/nxpack --tex romfs.bin <path> <texture> out.png                   # one texture as PNG
build/nxpack --unkai <port folder|.unkai-port> romfs.bin <out folder>   # what the phone build will produce
```
