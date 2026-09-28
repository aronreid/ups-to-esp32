#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Compile and run the descriptor parser on a host machine, no ESP-IDF needed.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
cap="${1:-$(ls -d "$root"/captures/*/ 2>/dev/null | head -1)}"
bin="$cap/report-descriptor.bin"
[[ -f "$bin" ]] || { echo "no report-descriptor.bin in $cap" >&2; exit 1; }

# Derive VID/PID from the capture directory name (e.g. 051d-0002-2026...) so
# the profile lookup is exercised with the device's real identity.
base="$(basename "${cap%/}")"
vid="${base%%-*}"; rest="${base#*-}"; pid="${rest%%-*}"

out=$(mktemp -d)
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 \
   -I"$here/stub" \
   -I"$root/firmware/components/ups_hid/include" \
   "$here/test_parser.c" \
   "$root/firmware/components/ups_hid/hid_parser.c" \
   "$root/firmware/components/ups_hid/ups_varmap.c" \
   "$root/firmware/components/ups_hid/ups_profiles.c" \
   -lm -o "$out/test_parser"
"$out/test_parser" "$bin" "$vid" "$pid"
