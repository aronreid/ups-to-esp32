#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# M0a: capture what a UPS actually reports, using this Mac as the USB host.
#
# Produces a reference dump before any ESP32 work, so that when the board
# enumerates the same UPS, any disagreement is an ESP32 bug rather than an
# unknown. Writes everything to captures/<vid>-<pid>-<timestamp>/.
#
# Usage:  tools/capture-ups.sh            # auto-detect the first UPS found
#         tools/capture-ups.sh 0764 0501  # or name VID and PID explicitly
#
# Note on macOS: the OS binds HID Power Device units itself (that is why a UPS
# shows up in the battery menu). Reading IOKit properties works fine because of
# that, but usbhid-ups has to claim the device via libusb and may need sudo, or
# may fail outright if macOS will not let go. If it does, the IOKit dump alone
# is still enough to drive the parser.

set -uo pipefail

VID="${1:-}"
PID="${2:-}"

# Known USB HID Power Device vendors, for auto-detection.
KNOWN_VENDORS="0764:CyberPower 051d:APC 0463:Eaton-MGE 09ae:TrippLite 0d9f:PowerCOM 06da:Liebert-Phoenixtec 10af:Liebert"

find_ups() {
    local ioreg vid_dec
    ioreg=$(ioreg -p IOUSB -l -w 0 2>/dev/null)
    for entry in $KNOWN_VENDORS; do
        local v="${entry%%:*}" name="${entry##*:}"
        vid_dec=$((16#$v))
        if grep -q "\"idVendor\" = $vid_dec" <<<"$ioreg"; then
            echo "$v" "$name"
            return 0
        fi
    done
    return 1
}

if [[ -z "$VID" ]]; then
    echo "Scanning USB for a known UPS vendor..."
    if read -r VID VENDOR_NAME < <(find_ups); then
        echo "  found: $VENDOR_NAME (VID 0x$VID)"
    else
        echo "No known UPS vendor on USB." >&2
        echo "Plug the UPS in, or pass VID and PID explicitly." >&2
        echo >&2
        echo "Currently attached USB devices:" >&2
        ioreg -p IOUSB -l -w 0 2>/dev/null \
            | grep -E '"USB Product Name"|"idVendor"|"idProduct"' \
            | sed 's/^ *| *//' >&2
        exit 1
    fi
fi

STAMP=$(date +%Y%m%d-%H%M%S)
OUT="captures/${VID}-${PID:-auto}-${STAMP}"
mkdir -p "$OUT"
echo "Writing to $OUT/"

# 1. Everything IOKit knows, including the report descriptor if exposed.
echo "[1/4] IOKit device properties and report descriptor"
ioreg -c IOHIDDevice -l -w 0 > "$OUT/ioreg-hid.txt" 2>&1
ioreg -p IOUSB -l -w 0      > "$OUT/ioreg-usb.txt"  2>&1
system_profiler SPUSBDataType > "$OUT/system_profiler-usb.txt" 2>&1

# Pull the ReportDescriptor property belonging to THIS device out of ioreg.
# A plain grep -A picks up unrelated IOKit noise, so match the property inside
# the device's own block and keep the longest candidate.
python3 - "$OUT" "$VID" <<'EXTRACT'
import re, sys, pathlib
out, vid = pathlib.Path(sys.argv[1]), int(sys.argv[2], 16)
s = (out / "ioreg-hid.txt").read_text(errors="replace")
best = None
for m in re.finditer(r'"ReportDescriptor" = <([0-9a-fA-F]+)>', s):
    ctx = s[max(0, m.start() - 4000):m.start() + 4000]
    if f'"VendorID" = {vid}' in ctx:
        if best is None or len(m.group(1)) > len(best):
            best = m.group(1)
if best:
    (out / "report-descriptor.hex").write_text(best + "\n")
    (out / "report-descriptor.bin").write_bytes(bytes.fromhex(best))
    print(f"      report descriptor: {len(best)//2} bytes -> report-descriptor.bin")
else:
    print("      no ReportDescriptor for this VID in IOKit")
EXTRACT

# 2. NUT's own decode. This is the reference the firmware must reproduce.
echo "[2/4] NUT usbhid-ups debug decode (12s)"
DRV=$(command -v usbhid-ups || echo /opt/homebrew/libexec/nut/usbhid-ups)
if [[ -x "$DRV" ]]; then
    ARGS=(-s m0a -DDDD -x vendorid="$VID")
    [[ -n "$PID" ]] && ARGS+=(-x productid="$PID")
    # Foreground, killed after 12s: long enough for descriptor parse plus a
    # couple of poll cycles.
    ( "$DRV" "${ARGS[@]}" > "$OUT/usbhid-ups-debug.txt" 2>&1 & echo $! > "$OUT/.pid" )
    sleep 12
    kill "$(cat "$OUT/.pid")" 2>/dev/null
    rm -f "$OUT/.pid"
    if grep -qi 'permission\|cannot claim\|failed to open' "$OUT/usbhid-ups-debug.txt"; then
        echo "      could not claim the device -- retry with: sudo $0 $VID $PID"
    else
        echo "      -> usbhid-ups-debug.txt"
    fi
else
    echo "      usbhid-ups not found (brew install nut)" 
fi

# 3. The usage list the firmware's mapping table has to cover.
echo "[3/4] extracting reported usages"
if [[ -s "$OUT/usbhid-ups-debug.txt" ]]; then
    grep -Ei 'Path:|ReportID|Offset|Size|Exponent|battery\.|input\.|output\.|ups\.' \
        "$OUT/usbhid-ups-debug.txt" | sort -u > "$OUT/usages.txt"
    echo "      $(wc -l < "$OUT/usages.txt") lines -> usages.txt"
fi

# 3b. Decode the descriptor. This is the deliverable: report IDs, usages, bit
#     offsets and unit-aware scaling, independent of whether NUT could attach.
if [[ -f "$OUT/report-descriptor.bin" ]]; then
    echo "[3b] decoding descriptor"
    "$(dirname "$0")/decode-hid.py" "$OUT/report-descriptor.bin" \
        > "$OUT/decoded.txt" 2>&1 \
        && echo "      -> decoded.txt ($(grep -c . "$OUT/decoded.txt") lines)"
fi

# 4. Summary for the commit message / docs.
echo "[4/4] summary"
{
    echo "UPS capture $STAMP"
    echo "VID 0x$VID  PID 0x${PID:-unknown}"
    echo
    echo "Host: $(sw_vers -productName) $(sw_vers -productVersion)"
    echo "NUT:  $("$DRV" -h 2>&1 | head -1)"
    echo
    echo "Files:"
    ls -1 "$OUT"
} > "$OUT/SUMMARY.txt"
cat "$OUT/SUMMARY.txt"

echo
echo "Next: compare usages.txt against the HID-to-NUT mapping table in"
echo "docs/firmware.md, and check whether output.voltage needs the CyberPower"
echo "exponent correction in firmware/components/ups_hid/ups_profiles.c."
