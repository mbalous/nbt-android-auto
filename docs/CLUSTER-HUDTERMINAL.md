# Instrument Cluster (6WB) — Navigation Graphics over MOST `HUDTerminal`

Historical session executable names in this document have been normalized to the current name.

## Key Findings
- The 6WB instrument cluster is **not an NBT display output**. `/etc/system/config/nvkddisplay.conf` only knows the 1280x480 LVDS panel (CID), and the Intel side only exposes `DisplayManager:0`.
- Navigation graphics in the cluster are rendered by the **instrument cluster itself**. The NBT drives it remotely over a graphics terminal on the MOST bus:
  device **0x0101**, FBlock **0x82**, instance **0x00** (there is also `82/02`, presumably the HUD).
- Sender: `NavNBTHighController` (`CGuidanceViewerIcHud` / `CIcHudComm`, logical device `CHUDTerminalLogDev`). Local client `0x2033` receives acknowledgements.
- The 6WB junction view (FPK, "TopFrame 11-13") consists of polygons, lines, circles, and bitmaps (`NBT_RLE32` Bosch / `NBT_RLE16` Conti), plus the moving position arrow ("Carsor"). Rendering is double-buffered: draw into the hidden instance while the other is visible, then swap visibility.

## Functions of FBlock `82/00`
`FktIDs` response, 12-bit start/end pairs:

```
000-003, 200-202, 203-206, 20E-20F, 210-212, 215-216, 218-21A, 21B-221, 226-228, 229-22A, 22B-22C,
247-248, 300-301, C01-C0A
```

Known method names in the controller (`HUDTerminal.*`):
- `CreateInstance`, `ReleaseInstance`, `GetInstanceId`, `TerminalInterface`
- `ImageAddBitmap` (segmented), `ImageGetSize`, `ImageSetTextareaFormat`, `FrameAddItem`
- `PotentiometerSetRange` / `Value` / `Type`
- `ImageRemoveAllItems`, `ImageRemoveItem`
- `ImageAddPolygon`, `ImageSetFillMode` / `FillColor` / `LineWidth` / `LineColor`
- `ImageSetPositionRotation`, `ImageSetRotation`, `ImageAddCircle` (`Diameter`)
- `TextareaSetText`, `KOMBITextareaSetFormat`
- `SwitchVisibility`, `IsVisible`, `SetVisibility`
- `AnimationSetGuidingStep`, `CurrentGuidingStep`

## Observed Acknowledgements from Drive Capture
Captured `ResultAck` (`op=D`, `tx=4`), where payload echoes parameters:

| Fkt | Count | Payload (example) | Meaning |
|---|---|---|---|
| `21D` | 400 | `inst16 item16 type8(02 line/03 fill) n8 n*(x16,y16)` | `ImageAddPolygon` |
| `C06` | 188 | `inst16 x16 y16 0 0 0x0064 0` (x/y updating continuously) | `ImageSetPositionRotation` (Carsor) |
| `227` | 123 | `inst16 0000 00xx 02` (`xx` decreasing: `fc, ec, e5, de ...`) | `PotentiometerSetValue` (distance bar) |
| `210` | 82 | `inst16 00` | `TextareaSetText` (empty string) |
| `220` | 67 | `inst16` | `ImageRemoveAllItems` |
| `204` | 65 | `inst16 00/01` | `SetVisibility` |
| `20E` | 55 | `instA16 instB16` | `SwitchVisibility` (buffer swap) |
| `219` | 41 | `inst16 u8` | Line style / width |
| `21B` | 27 | `inst16 u8` (`04` / `0a`) | Fill / style color |
| `C09` | 16 | `inst16 item16 02 x16 y16 r16 ...` | `ImageAddCircle` |
| `C04`/`C05` | 15 | `inst16 ...` | `ImageGetSize` / `ImageAddBitmap` |

## Confirmed on Vehicle (Parked, Active Route)
- Sending via `nbt_knob --send 0101,82,00,<fkt>,6,4,1,0,FFFF,<hex>`: OpType 6 (`StartResultAck`), `tx=4` (`tx=0` also works), payload = instance (`u16`) + parameters. Acknowledgements go to NBT client `0x2033`.
- `204 [inst16][00|01]` hides/shows an instance: instance `0x17` toggles the entire cluster navigation area. Confirmed.
- `21D [inst][item16][03=fill|02=line][n][n*(x16,y16)]` in instance `0x1F`/`0x20` (junction view) draws over the navigation graphic. Confirmed.
- `C06` (Carsor `0x25`) is acknowledged.
- Instances at route start: `0x17` (root container), `0x18`/`0x19` (`C04`/`C05`), `0x1A`–`0x1C` (`210`/`204`), double-buffered pairs `0x1D`/`0x1E`, `0x1F`/`0x20`, `0x21`/`0x22`, `0x23`/`0x24` (draw, `20E [old][new]`, `220 [old]` to clear), `0x25` Carsor, `0x35` distance bar (`227`).

## Colors, Surfaces, and Factory Layout
- `21B [inst][c]` sets the color/style of subsequent elements. Valid values `0..19` (`>= 20` discards the element). Palette: white, cream, grays, black, red, partly translucent (**no blue/yellow**); colors follow the cluster theme (Comfort/Sport/Eco).
- `219 [inst][c]` does not change fill color; likely line width (NBT uses `0x0A`/`0x0E`/`0x16`).
- Instance `0x1F`/`0x20` is **perspective-projected** (road plane, narrower at the top). The upper region (`y < ~70`) lies behind the horizon and is clipped.
- Polygons in `0x1D`/`0x21`/`0x23` were not visible without matching frame setup; `0x18`/`0x19` are bitmap-only (`RoadIcon`, `ImageAddBitmap`, `NBT_RLE32`), and `0x1A`–`0x1C` are text fields (`TextareaSetText`).
- Note: `nbt_knob --trace` forwards controller keys to the default FIFO. When tracing alongside Android Auto, pass `--fifo /fs/ram3/<dummy>`.

## Function IDs and Parameter Records (from `ctl.dis` `0x8792f8e`–`0x879f366`)
For each function the controller builds a record (`movw $fkt,0x12(%ebx)`) preceded by parameter descriptors (`CHBParaNumber`, `CHBParaStream(max)`, `CHBParaString(max)`, `CHBParaBool`) and a trailing generic `u8, Stream(10)`.

```
C06 u16,i16,i16,i16,i16,i16,u16        Carsor position (confirmed)
C09 u16,u16,u8,i16,i16,u16,i32,i32     ImageAddCircle (21E has same layout)
C07 u16,u16,u8                         ?
C05 u16,u16,u16,u8,u8,u8,u8,Stream(60000)   ImageAddBitmap (acks on inst 0x18/0x19)
C04 u16,u8,Stream,u16,u16              ImageGetSize
C03 u16,u8,u8,u8,u8,u16,u8             ?
22B u16,u16,u8,u16,u8                  FrameAddItem?
220 u16                                ImageRemoveAllItems (confirmed)
21F u16,u16,u16                        ?
21D u16,u16,u8,u8,Stream(400)          ImageAddPolygon (confirmed)
21C/21B/219/218 u16,u8                 Style setters (21B = color, confirmed)
215 u16,i16,i16,i16,u16                ImageSetPositionRotation?
211 u16,u8 x6                          Textarea format?
20E u16,u16                            SwitchVisibility (confirmed)
203 u16,u8                             ?
226 u16,u8,bool                        ?
205 u16,u8,Stream,bool                 IsVisible?
204 u16,bool                           SetVisibility (confirmed)
C08 bool,u8,Stream,u8                  ?
300 Stream(10200)                      Bitmap segment over tx 3?
C01 u16                                ?
247 u8,u8,u8                           ?
C00 u8,u8,String(18)x3                 TerminalInterface (versions)
210 u16,String(255)                    TextareaSetText (NBT clears 0x1A/0x1B/0x1C at route start)
201 u16,String(255),...,u16            ?
200 u16,u8,u8,String(255),...,u16      CreateInstance?
229 u16,u8,i32,i8,i32,i8,i32,i8        PotentiometerSetRange
227 u16,i32,i8                         PotentiometerSetValue (confirmed)
```

Instances: `0x1A`–`0x1C` text fields, `0x18`/`0x19` bitmap surfaces (road sign/icon), `0x1D`–`0x24` drawing surfaces (`0x1F`/`0x20` perspective), `0x25` Carsor, `0x35` distance bar, `0x17` root container.
MOST string format: leading encoding byte (`00` UTF-16, `01` ISO-8859-15, `02` UTF-8 per MOST specification), followed by null-terminated text.

## Bitmaps, Surface Dimensions, and Lane Row

**Surface dimensions** via `C04` `ImageGetSize` (`[inst][01]`, `tx 4`); response is `[inst][01][w u16][h u16]`:

| Instance | Size | Position in cluster (frame test) |
|---|---|---|
| `0x17` | 309 x 330 | Entire nav region down to just above the info row (time/range) |
| `0x18`/`0x19` | 305 x 44 | Top row (`RoadIcon`, bitmap) |
| `0x1D`/`0x1E` | 145 x 44 | Flat strip below the top row, centered |
| `0x1F`/`0x20`, `0x21`/`0x22`, `0x25` | 221 x 158 | Perspective plane in the center |
| `0x23`/`0x24` | 283 x 40 | Flat strip directly above the info row (lane guidance row) |
| `0x35` | 28 x 113 | Distance bar |

Lines drawn directly on a surface boundary are clipped. Color `0x0E` (gray) is invisible on the cluster.

**`ImageAddBitmap` (`C05`)** must be sent on **transfer type 0** (or **tx 1** for larger bitmaps); `tx 4` produces no response.

```
[inst u16][x u16][y u16][filetype 01][01][00][bitmap_id u8][RLE32 data]
```

- Response is `ResultAck [inst][bitmap_id][01]`.
- An invalid 4th byte (filetype) returns `ErrorAck 06 04` (parameter 4 out of range).
- For filetype `<= 0x7F`, the image must match the **exact** surface dimensions (e.g., 305 x 44 for `0x18`/`0x19`). A 49 x 44 image rendered as a striped band.

**`NBT_RLE32`** (from `CGVRLE32Compressor`, `NavigationGuidanceViewer_NV` `0x80f0972`):

- Packets:
  - `[0x80|n][A B G R]` repeats one pixel `n` times (`n = 2..127`).
  - `[n][n * A B G R]` emits `n` literal pixels (`n = 1..127`).
- Row by row with no row separators.
- Zero-padded at the end to a multiple of 8 bytes (at least 1 byte).
- Byte order is **A B G R** (using A R G B turned blue into brown).
- `A = 0` is transparent.

**Bitmap throughput** (measured parked in vehicle with `nbt_cluster --bench` / `--bench2`):

| Channel | Image | Size | Round-trip to ack |
|---|---|---|---|
| `tx 0` (control channel) | Sign 305x44 | 1.4 KB | 250–420 ms |
| `tx 0` | Strip 305x44 | 5.7 KB | 830 ms (~7 KB/s) |
| `tx 0` | Map 221x158 | 7.2 KB | **`ErrorAck 0C 01`** (segmentation error, above ~6 KB) |
| `tx 1` (asynchronous) | Map 221x158 | 7 KB | 360–490 ms (~15–19 KB/s) |
| `tx 1` | Strip 221x158 | 11 KB | 570 ms |

- **Visibility test (`--mapdemo`):** Surfaces `0x21`, `0x1F`, `0x1D`, and `0x23` acknowledge bitmaps with `ResultAck`, but **do not display them**. Polygons in those surfaces are visible. The cluster only renders bitmaps in the `RoadIcon` surfaces `0x18`/`0x19` (top row, 305x44), so a live cluster map is not possible.

**Text:** The large distance font (`0x1B`) has no comma glyph: `5,4` renders as `5 4`, whereas `5.4` renders properly. `nbt_cluster` converts commas to periods.

**Road signs:** Digits and edges are anti-aliased (stroke font, edge pixels pre-blended over black and opaque). Because combined shields are ~5.5 KB, they are sent over `tx 1`. The distance bar only appears below 1 km.

**Implementation in `nbt_cluster`:**

- **Road shields:** Highway markers such as `A 66` render as a blue shield with white digits and `B 40` as a yellow shield with black digits on the left of the top row (`0x18` and `0x19`), stripping the route number from the street text.
- **Lanes:** Rendered in `0x23`. Recommended lanes are filled; non-recommended lanes are drawn as outlines.
- **Test modes:**
  - `--demo` displays static test data.
  - `--frames` draws calibration frames in `0x21`, `0x23`, and `0x1D`.
  - `--bmp VARIANT INST STREET` tests `C04`/`C05` bitmap variants.
