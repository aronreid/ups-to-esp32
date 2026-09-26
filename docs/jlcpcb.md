# Manufacturing
and is enforced by `tools/test/test_jlc.py`, which measures the board rather
than trusting the design rules stored in it.

- <https://jlcpcb.com/capabilities/pcb-capabilities>
- <https://jlcpcb.com/capabilities/pcb-assembly-capabilities>

## Why this is a separate check from DRC

DRC compares the board to the rules **inside** the board. If those rules are
looser than the fab's, DRC passes and the fab either rejects the job or builds
something that fails. Worse, several of JLC's limits have no DRC rule at all:
silkscreen line width and text height are not checked by KiCad's
`min_text_thickness` / `min_text_height`, which apply only to text on copper.

So the harness measures the real geometry and compares it to JLC's published
numbers directly. The two cannot drift apart.

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

**J1's 0.20 mm ring is the one number worth knowing about.** It comes from the
stock USB-C footprint's through-hole shield legs and is above JLC's absolute
minimum but below their recommended 0.25 mm, so registration is tighter than
they like. It will be built. If a batch shows breakout there, the fix is a
footprint with larger shield pads, not a process change.

## Assembly: Economic PCBA

**Every part is on the top side, and has to be.** JLC's capability table gives
Economic PCBA as *"Single sided placement (SMT/Thru-hole)"*; double-sided
placement exists only under Standard PCBA, whose minimum single board is
**70 x 70 mm** — this one is 36 x 58 — and whose setup fees are several times
higher. Rev A's first complete layout had 25 parts on the bottom. It passed DRC,
the ground check and every JLC *fabrication* limit, and could not have been
ordered on the service the whole project is costed around. The reasoning at the
time was that a two-layer board costs the same with parts on one side or two.
True of the bare PCB; not of assembly.

Moving everything to the top cost no board area: it still routes at 36 x 58,
both copper layers still carry tracks, and the bottom is now a nearly unbroken
ground plane. `tools/test/test_bom.py` fails the harness if any placement is on
the bottom.

| Limit | JLC | This board |
|---|---|---|
| Board size | 10 × 10 mm to 470 × 500 mm | 36 × 58 mm |
| Smallest package | 0402 | 0402 |
| Minimum IC pin pitch | 0.40 mm | 1.27 mm (SOIC-16) |
| Edge rails | not required | none |
| Placement sides | **one** | one (top) |

The 0402 passives sit exactly on Economic PCBA's limit. That is supported, not
marginal — but it does mean there is no room to go smaller without moving to the
Standard service.

**Through-hole parts are assembled**, contrary to what this project's notes used
to say. JLC fits them by hand at **$3.50 labour plus $0.0173 a joint**. The
labour fee is once per order, so J3 and J5's 11 joints cost cents on top of it.
Fit them yourself or let JLC; just decide before ordering.

## Ordering

```sh
tools/pack-fab.sh
```

Produces the three uploads and nothing else:

| Tab | File |
|---|---|
| PCB | `hardware/fab/ups-adaptor-gerbers.zip` |
| BOM | `hardware/bom/bom.csv` |
| Placement | `hardware/bom/cpl.csv` |

**All three are committed**, so an order can be placed from any machine, or
straight off GitLab, without KiCad installed. The usual objection to committing
generated artifacts is that a stale one passes every gate that never looks at
it — so `pack-fab.sh` records the SHA-256 of the board it plotted from in
`hardware/fab/SOURCE.sha256`, and `tools/test/test_fab.py` fails the harness if
the board no longer hashes to it. Timestamps would not survive a clone; a hash
does. The same test checks the zip carries every manufacturing layer and no
documentation layer.

**Do not upload `hardware/fab/ups-adaptor-cpl.csv`.** That is KiCad's raw
position export, an intermediate `gen-bom.py` reads, and it carries raw KiCad
angles — it would fit eight parts the wrong way round. It is gitignored for
that reason. The file to upload is `hardware/bom/cpl.csv`.

The zip carries **only** manufacturing layers — copper, mask, silkscreen, paste
and edge cuts, plus the two drill files. `kicad-cli pcb export gerbers` with no
`--layers` also emits Courtyard, Fab, Adhesive, Margin and four `User_*` layers,
which are documentation; a fab importer either ignores them or offers to treat
one as a real layer.

### Placement: centroids, and angles nobody invented

Two separate things go wrong in a CPL, and this board hit both. JLC's assembly
preview showed them ; it renders JLC's own library model at our
angle on top of our gerbers, so "do the model's leads land on the pads" is a
thing a human can see. It is the authority, and it is worth one upload cycle
before paying for anything.

**Positions must be centroids, not anchors.** `kicad-cli pcb export pos` writes
each footprint's *anchor* — the origin its library author chose. A machine puts
the part's *centroid* at the coordinate it is given. Where the two differ, the
part lands off its own pads by that much:

| Part | Anchor vs pad centroid | Why |
|---|---|---|
| **U1** ESP32-S3-WROOM-1 | **3.77 mm** | the anchor is the module body centre, and the antenna end carries no pads |
| **J2** XKB USB-A | **2.49 mm** | body centre vs pads at one end |
| **J1** HRO USB-C | **1.46 mm** | same |

Every other part agrees to better than 0.01 mm. `tools/gen-cpl-pos.py` replaces
the `kicad-cli` export and writes centroids; `test_rotation.py` fails if the
file came from `kicad-cli` instead.

**Angles are KiCad's own, uncorrected.** This project previously applied the
[JLCKicadTools](https://github.com/matthewlai/JLCKicadTools) reel-angle table to
eight parts. The preview showed that table putting **Q2 ninety degrees out**:
the board's SOT-23 pads sit two-down and one-up, and the model's leads came out
left and right. The table is community-maintained and undated — it is vendored
here, and it matches upstream byte for byte, so this is not a transcription
error. It is simply not verifiable by us, and a correction nobody can check is
not safer than no correction; it is the same gamble with more moving parts.

So `gen-bom.py` emits KiCad's angle, `APPLY_VENDOR_ROTATIONS` is off, and any
part the preview actually shows wrong gets an entry in `ROTATION_OVERRIDE` with
the evidence written beside it. `test_rotation.py` fails if the table is
switched back on, if an angle differs from KiCad's without an override, or if an
override has no stated reason.

The eight that ended up overridden, and the one that did not:

| Ref | CPL | Why |
|---|---|---|
| U2 SOIC-16 | **0** | CPL 0 drew it E–W over its E–W pad rows; 90 drew it N–S |
| U3 SOT-223 | **180** | CPL 0 was visibly wrong; 180 matches 3-leads-west, tab-east |
| U4 SOT-23-5 | **270** | CPL 270 put its leads W/E, as the board has them |
| D1, D2 SOT-23-6 | **270** | CPL 270 drew 3 leads W and 3 E; CPL 0 drew them N/S |
| Q1 SOT-23 | **180** | KiCad's 0 is 180 out, as Q2 was at 90 |
| Q2 SOT-23 | **270** | at KiCad's 90 the model was flipped; the board is 2 S, 1 N |
| **J1 USB-C** | **0 — no override** | the table's rule is for this *exact* HRO part, not a family, and it still drew the connector upside down at 180 |

J1 is the point of the whole section. That was the best-evidenced entry in the
vendored table — a rule for one specific part number rather than a package
family — and it was wrong too. The three-lead SOT-23 is wrong the same way,
while SOIC-16, SOT-223 and SOT-23-5/-6 are right, and one `^SOT-23` pattern
covers all four families with a single number.

**The loop before ordering:** upload, open the Component Placements preview,
zoom each non-passive part, and check the model's leads sit on the pads.
Anything that does not, add to `ROTATION_OVERRIDE`, re-run `pack-fab.sh`,
re-upload. It converges in a cycle or two and costs nothing.

### Never leave a line for the matcher to fill

Leaving a passive's LCSC code blank is not safe. JLC's cart resolved them like this:

| Line | What the matcher chose | What it is |
|---|---|---|
| R1,R2,R7,R8 1k | `C606165` RC0100FR-071KL | **01005**, qty 0 |
| R3,R4,R12 10k | `C364373` RC0100FR-0710KL | **01005**, qty 0 |
| R5,R6 4.7k | `C850286` RC0100FR-074K7L | **01005**, qty 0 |
| R9,R10 5.1k | `C2766418` 0105WHJ0512TDE | **01005**, ±5%, qty 0 |
| R11 100k | `C723496` RC0100FR-07100KL | **01005**, qty 0 |
| C1, C3/C6/C9, C8 | nothing | 3 lines "not selected" |

KiCad names the footprint `R_0402_1005Metric`, meaning **imperial 0402 = metric
1005**. JLC read the 0402 as metric, which is imperial **01005** — 0.4 × 0.2 mm,
a quarter the length of the pads, and below Economic PCBA's own 0402 floor. Every
one came back out of stock and Extended.

Nothing in this repository could have caught it. The BOM was right, the
footprints were right, and the substitution happened inside JLC's cart. The only
defence is to pin the code, which `tools/gen-bom.py` now does for every line,
and `tools/test/test_bom.py` fails on a blank one or on a pinned passive whose
size disagrees with its footprint.

## What this check does NOT cover

- **Rotation** — now corrected automatically, but verify the eight corrected
 parts in the assembly preview. See above.
- **Basic vs Extended parts.** JLC charges **$3.07 per unique extended part per
 order**. Their site renders this only in a browser.
- **Stock.** Checked by hand and recorded in `BOM.md`; it moves.

## Known cosmetic warnings

These are KiCad quality warnings, not JLC rule violations, and the board ships
with them:

- **`silk_overlap`** — silkscreen text crossing another part's outline. Readable,
 just untidy. Shuffling it further on a board this dense makes it worse.
- **`silk_over_copper`** — silk crossing a solder-mask opening. Fabs clip this
 automatically; it is what the warning is telling you will happen.
- **`isolated_copper`** — dead pour islands. Island removal is enabled on both
 zones, so they are dropped at fill time.
- **`track_dangling`** — a short stub the autorouter sometimes leaves. The net
 is fully connected; it is a redundant tail. Not present on the current board.
