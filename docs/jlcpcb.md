# Manufacturing

Rev A is made on JLCPCB's two-layer process and assembled with Economic PCBA.
The limits below are JLC's published numbers, measured against the board
itself rather than against the design rules stored in it:

- <https://jlcpcb.com/capabilities/pcb-capabilities>
- <https://jlcpcb.com/capabilities/pcb-assembly-capabilities>

## Why this is a separate check from DRC

DRC compares the board to the rules **inside** the board. If those rules are
looser than the fab's, DRC passes and the fab either rejects the job or
builds something that fails. Several of JLC's limits have no DRC rule at all:
silkscreen line width and text height are not checked by KiCad's
`min_text_thickness` and `min_text_height`, which apply only to text on
copper. So the geometry is measured directly against JLC's numbers.

## Board process: 2-layer FR-4, 1 oz

| Limit | JLC | This board |
|---|---|---|
| Track width | 0.10 mm | **0.20 mm** |
| Track clearance | 0.10 mm | **0.20 mm** (board rule) |
| Via hole | 0.15 mm | **0.30 mm** |
| Via pad | 0.25 mm | **0.60 mm** |
| Via annular ring | 0.05 mm | **0.15 mm** |
| Plated-hole annular ring | 0.18 mm absolute, 0.25 recommended | **0.20 mm** (J1 shield leg) |
| Via hole to via hole | 0.20 mm | **0.52 mm** |
| Plated hole to plated hole | 0.45 mm | **0.51 mm** |
| Copper to board edge | 0.20 mm | **0.90 mm** |
| Silkscreen line width | 0.15 mm | **0.15 mm** |
| Silkscreen text height | 1.00 mm | **1.00 mm** |

Everything has margin except the two silkscreen rows and J1's shield leg.

**J1's 0.20 mm ring is the one number worth knowing about.** It comes from
the stock USB-C footprint's through-hole shield legs and is above JLC's
absolute minimum but below their recommended 0.25 mm, so registration is
tighter than they like. It builds. If a batch shows breakout there, the fix is
a footprint with larger shield pads, not a process change.

Order settings: JLC's defaults are right for this board. 2 layers, 1.6 mm
FR-4, 1 oz copper, HASL, green with white silkscreen, tented vias. Tented
matters: U1's thermal vias have no back mask opening and the back silkscreen
artwork prints over them. JLC prints their order number somewhere of their
choosing; the back of this board is artwork, so either pay to remove the
number or accept it.

## Assembly: Economic PCBA

**Every part is on the top side, and has to be.** JLC's capability table
gives Economic PCBA as *"Single sided placement (SMT/Thru-hole)"*;
double-sided placement exists only under Standard PCBA, whose minimum single
board is **70 x 70 mm** (this one is 36 x 58) and whose setup fees are several
times higher. A two-layer board costs the same with parts on one side or two;
assembly does not. Both copper layers carry tracks and the bottom is a nearly
unbroken ground plane. When ordering, set the assembly side to **top only**.

| Limit | JLC | This board |
|---|---|---|
| Board size | 10 x 10 mm to 470 x 500 mm | 36 x 58 mm |
| Smallest package | 0402 | 0402 |
| Minimum IC pin pitch | 0.40 mm | 1.27 mm (SOIC-16) |
| Edge rails | not required | none |
| Placement sides | **one** | one (top) |

The 0402 passives sit exactly on Economic PCBA's limit. That is supported, not
marginal. It does mean there is no room to go smaller without moving to the
Standard service.

**Through-hole parts are assembled**, by hand, at a labour fee per order plus
a few cents a joint. J3 and J5 are the only ones, 11 joints, and they are not
in the placement file. Fit them yourself or ask JLC to; decide before ordering.

## Ordering

Three uploads, from `hardware/`:

| Tab | File |
|---|---|
| PCB | `hardware/fab/ups-adaptor-gerbers.zip` |
| BOM | `hardware/bom/bom.csv` |
| Placement | `hardware/bom/cpl.csv` |

`hardware/fab/ups-adaptor-jlcpcb-order.zip` holds all three plus a
`README.txt` with the order settings. All three are committed, so an order can
be placed from any machine without KiCad installed. `hardware/fab/SOURCE.sha256`
records the SHA-256 of the `.kicad_pcb` the outputs were plotted from; if the
board no longer hashes to it, the outputs are stale and must be regenerated
([`hardware.md`](hardware.md), *Opening and editing the design*).

The gerber zip carries **only** manufacturing layers: copper, mask, silkscreen,
paste and edge cuts, plus the job file and the two drill files. Courtyard,
fab, adhesive, margin and user layers are documentation, and a fab importer
either ignores them or offers to treat one as a real layer.

**Do not upload KiCad's raw position export as the placement file.** It
carries anchors instead of centroids and KiCad's own angles, and it would fit
seven parts the wrong way round and misplace three. The file to upload is `hardware/bom/cpl.csv`.

### Placement: centroids, and angles the preview decided

Two separate things go wrong in a placement file. JLC's assembly preview
shows both: it renders JLC's own library model at the file's angle on top of
the gerbers, so "do the model's leads land on the pads" is a thing a human can
see. It is the authority, and worth one upload cycle before paying.

**Positions are centroids, not anchors.** `kicad-cli pcb export pos` writes
each footprint's *anchor*, the origin its library author chose. A machine puts
the part's *centroid* at the coordinate it is given. Where the two differ, the
part lands off its own pads by that much:

| Part | Anchor vs pad centroid | Why |
|---|---|---|
| **U1** ESP32-S3-WROOM-1 | **3.77 mm** | the anchor is the module body centre, and the antenna end carries no pads |
| **J2** XKB USB-A | **2.49 mm** | body centre vs pads at one end |
| **J1** HRO USB-C | **1.46 mm** | same |

Every other part agrees to better than 0.01 mm. At 3.77 mm every one of the
module's castellated pads misses. `cpl.csv` carries pad centroids.

**Angles are KiCad's own, except where the preview showed them wrong.** The
community reel-angle table that is often applied to KiCad exports is not used:
it drew Q2 ninety degrees out and J1 upside down, and its rule for J1 is for
that exact part number. `cpl.csv` therefore carries KiCad's angle for every
part except these seven, each decided by looking at the preview:

| Ref | CPL | What the preview shows |
|---|---|---|
| U2 SOIC-16 | **0** | body runs east-west, 8 pads north and 8 south |
| U3 SOT-223 | **180** | 3 leads west, tab east |
| U4 SOT-23-5 | **270** | 3 leads west, 2 east |
| D1, D2 SOT-23-6 | **270** | 3 leads west, 3 east |
| Q1 SOT-23 | **180** | 2 leads west, 1 east |
| Q2 SOT-23 | **270** | 2 leads south, 1 north |
| **J1 USB-C**, U1 | **KiCad's angle** | no correction |

**The loop before paying:** upload, open the Component Placements preview,
zoom each non-passive part, and check the model's leads sit on the pads.
Anything that does not, fix its angle in `cpl.csv`, re-upload. It converges in
a cycle or two and costs nothing.

**Polarity on silk:** a dot marks pin 1. D3 and D4 instead carry a bar across
the cathode end (pin 1 is the cathode); on a diode a dot would read as the
capacitor convention and mean the opposite. Check D1 to D4 against the render.

### Never leave a line for the matcher to fill

Every line in `bom.csv` carries an LCSC code, including commodity passives,
because a blank code is not safe. JLC's cart resolves a blank 0402 line like
this:

| Line | What the matcher chooses | What it is |
|---|---|---|
| R1,R2,R7,R8 1k | `C606165` RC0100FR-071KL | **01005**, out of stock |
| R3,R4,R12 10k | `C364373` RC0100FR-0710KL | **01005**, out of stock |
| R5,R6 4.7k | `C850286` RC0100FR-074K7L | **01005**, out of stock |
| R9,R10 5.1k | `C2766418` 0105WHJ0512TDE | **01005**, ±5%, out of stock |
| R11 100k | `C723496` RC0100FR-07100KL | **01005**, out of stock |
| C1, C3/C6/C9, C8 | nothing | lines "not selected" |

KiCad names the footprint `R_0402_1005Metric`, meaning imperial 0402 = metric
1005. JLC reads the 0402 as metric, which is imperial **01005**: 0.4 x 0.2 mm,
a quarter the length of the pads, and below Economic PCBA's own 0402 floor.
The substitution happens inside JLC's cart, where nothing in the design files
can see it. The only defence is to pin the code, and to check that each pinned
passive's size agrees with its footprint. [`BOM.md`](BOM.md) lists the codes.

## Not covered by the files

- **Rotation.** Verify the seven corrected parts in the assembly preview. See
  above.
- **Basic vs Extended parts.** JLC charges about $3 per unique Extended part
  per order. Their site shows this only in a browser.
- **Stock.** It moves; check every line at order time.

## Known cosmetic warnings

These are KiCad quality warnings, not JLC rule violations, and the board ships
with them:

- **`silk_overlap`**: silkscreen text crossing another part's outline.
  Readable, just untidy.
- **`silk_over_copper`**: silk crossing a solder-mask opening. Fabs clip this
  automatically; it is what the warning is telling you will happen.
- **`isolated_copper`**: dead pour islands. Island removal is enabled on both
  zones, so they are dropped at fill time.
