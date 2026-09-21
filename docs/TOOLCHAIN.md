# Toolchain

Everything needed to build the firmware and regenerate the board on a fresh
machine. **None of this lives in the repo** — which is exactly why it is written
down, because rediscovering it costs an afternoon.

---

## Firmware

**ESP-IDF v5.4**, installed to `~/esp/esp-idf`:

```sh
git clone --branch release/v5.4 --depth 1 --recursive --shallow-submodules \
 https://github.com/espressif/esp-idf.git ~/esp/esp-idf
~/esp/esp-idf/install.sh esp32s3
~/esp/esp-idf/export.sh # every shell, before idf.py
```

**`export.sh` picks its virtualenv from whichever `python3` is first on PATH**,
and errors out if that version does not match the one IDF was installed with.
On this machine IDF was installed under 3.12 while `python3` resolves to 3.9,
so it fails with *"virtual environment... not found"* until the right one is
put in front:

```sh
mkdir -p /tmp/pybin && ln -sf "$(command -v python3.12)" /tmp/pybin/python3
export PATH=/tmp/pybin:$PATH
~/esp/esp-idf/export.sh
```

Check `ls ~/.espressif/python_env/` to see which version yours wants.

Build and flash — **the board target is a build-time choice**, so each target
gets its own build directory rather than reconfiguring one:

```sh
cd firmware

# Rev A (the default; sdkconfig.defaults alone selects it)
idf.py build

# Freenove devkit, for bench work before Rev A boards exist
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.freenove" \
 -B build-freenove build
idf.py -B build-freenove -p /dev/cu.wchusbserial<...> -b 460800 flash monitor
```

`sdkconfig.freenove` is one line (`CONFIG_UPSA_BOARD_FREENOVE=y`) and is
committed: it is a defaults fragment like `sdkconfig.defaults`, not a generated
file. The `build-*/` directories are generated and are not.

**Reading the log without `idf.py monitor`.** The monitor is interactive, which
is awkward from a script. A plain serial read works, but note that opening the
port does not reliably reset the board on this adapter — pulse RTS to get output
from boot:

```python
s = serial.Serial(port, 115200, timeout=0.2)
s.dtr = False; s.rts = False; time.sleep(0.05)
s.rts = True; time.sleep(0.15) # EN low
s.rts = False # EN high -> boots, logs from t=0
```

The target board defaults to **Rev A**. For the Freenove devkit, flip it in
`sdkconfig`:

```sh
sed -i '' 's/^CONFIG_UPSA_BOARD_.*=y/CONFIG_UPSA_BOARD_FREENOVE=y/' sdkconfig
```

On the devkit, flash over the **UART** USB-C port, not the **USB** one — the
latter shares GPIO19/20 with the USB-A host pigtail.

## Hardware

**KiCad 10.** The scripts use both the CLI and KiCad's bundled Python, which is
the only Python that has `pcbnew`.

`tools/kicad_paths.py` finds all of it automatically: the environment first,
then the usual install locations on macOS and Linux, then `PATH`. **On a machine
with KiCad installed normally you do not need to set anything.** If KiCad lives
somewhere unusual, three variables override it:

```sh
export KICAD_CLI=.../kicad-cli
export KICAD_PY=.../python3 # the one with pcbnew
export KICAD_SHARE=.../SharedSupport # holds symbols/ and footprints/
```

A plain `python3 -c "import pcbnew"` will fail. That is expected; use `$KICAD_PY`.

The scripts used to hardcode `/Applications/KiCad/...` in six places with no way
to override it, which meant the repo only worked on one machine. If you find a
new hardcoded path, put it in `kicad_paths.py` rather than adding a seventh.

**Freerouting** for autorouting, plus a JRE. The jar is ~67MB and is **not** in
this repo:

```sh
export FREEROUTING_JAR=/path/to/freerouting.jar
```

`tools/route-pcb.py` looks in `tools/`, `hardware/tools/`, the
christmas-tree-sensor checkout it was originally borrowed from, and `~`. If none
of those has it, it prints where to download it rather than failing on someone
else's home directory. Releases: github.com/freerouting/freerouting.

Dropping the jar in `tools/` is the least surprising option; it is gitignored.

**ngspice** for the auto-reset simulation:

```sh
brew install ngspice
```

**NUT** for capturing UPS descriptors on a host:

```sh
brew install nut
```

## Running things

```sh
tools/test/run-all.sh # 16 checks; see below
tools/build-pcb.sh [passes] [tries] # regenerate the board end to end
tools/gen-bom.py [--with-tht] # hardware/bom/{bom,cpl}.csv, the upload files
tools/capture-ups.sh # capture a UPS's report descriptor on this Mac
tools/host-test/run.sh # firmware HID parser against a real capture
```

**`build-pcb.sh` is 9 steps and routes more than once.** Freerouting is not
deterministic, so it runs the whole generate-place-route-pour cycle several
times, scores each attempt (a floating ground counts 100, an unconnected item
10, a clearance or short 1) and keeps the best. Default 3 attempts; the second
argument changes it. The steps, in order:

| | Step | Why it is separate |
|---|---|---|
| 1 | `gen-pcb.py` | outline, footprints, nets, zones, design rules |
| 2 | `place-pcb.py` | placement — pcbnew, for correct flipping and rotation |
| 3 | `preseed-gnd.py` | ground escape vias **before** the router takes the space |
| 4 | `route-pcb.py` × N | Freerouting, `-mt 1` (its own multithreading is broken) |
| 5 | `join-pads.py` | duplicate pads within a footprint, with a via-assisted fallback |
| 6 | `widen-power.py` | power segments the router or the joiner left at signal width |
| 7 | `stitch-pcb.py` | ground stitching, island bridging, floating-pad rescue |
| 8 | `pour-pcb.py` | fill, then rescue-and-refill until the ground check stops changing |
| 9 | `silk-pcb.py`, `silk-jlc.py` | labels, then JLC's silk minimums and a nudge off pads |

then gerbers, drill, CPL, BOM and DRC.

`run-all.sh` skips the firmware builds if ESP-IDF is not exported, and says so
rather than passing quietly.

## Gotchas that cost time

**`pcbnew.SaveBoard` rewrites the `.kicad_pro`.** A standalone script that
loads a board, changes one thing and saves it will also overwrite the project
file from the board's in-memory settings — which are KiCad's defaults. That
silently reverted every design rule and deleted the Power net class in a script
that only meant to edit component values. It was caught because KiCad's default
edge clearance is *stricter* than this project's, so DRC started failing; a
looser default would have gone unnoticed. `tools/test/test_project.py` now
checks the rules and the net class, so this fails the harness instead of
shipping. If you write a new pcbnew script, run the harness afterwards.

- **macOS claims HID power devices.** `usbhid-ups` cannot take the UPS from the
 OS, so `capture-ups.sh` reads the descriptor from IOKit instead. A failure to
 claim is expected, not a bug.
- **Git here is Apple's** and needs `sudo xcodebuild -license` agreed once.
- **`timeout` does not exist on macOS.** Scripts use ssh's own timeouts or
 Python instead.
- **The board is generated, not hand-edited.** `tools/build-pcb.sh` overwrites
 `ups-adaptor.kicad_pcb`. Edit it in the GUI and your changes are gone on the
 next run — fold them back into the scripts, or stop running the pipeline.
