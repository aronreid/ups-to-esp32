#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Whole-project test harness. Everything here runs without the board attached;
# the two firmware builds need ESP-IDF exported, and are skipped if it is not.
#
#   tools/test/run-all.sh
#
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
pass=0; fail=0; skip=0

run() {
    local name="$1"; shift
    echo
    echo "── $name"
    if "$@"; then pass=$((pass+1)); else fail=$((fail+1)); fi
}

echo "ups-adaptor test harness"
echo "========================"

run "hardware: auto-reset SPICE"      python3 "$here/test_spice.py"
run "hardware: schematic ERC"         python3 "$here/test_erc.py"
run "hardware: netlist assertions"    python3 "$here/test_netlist.py"
run "hardware: power budget"          python3 "$here/test_power.py"
run "hardware: footprints exist"      python3 "$here/test_footprints.py"
run "hw/fw: pin map + boot state"     python3 "$here/test_pinmap.py"
run "layout: floating grounds"        "${KICAD_PY:-/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3}" "$here/test_ground.py"
run "layout: keepout, pairs, silk"    "${KICAD_PY:-/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3}" "$here/test_layout.py"
run "layout: USB connector pinouts" "${KICAD_PY:-/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3}" "$here/test_connectors.py"
run "hardware: project design rules" python3 "$here/test_project.py"
run "fab: BOM and CPL match the design" python3 "$here/test_bom.py"
run "fab: CPL carries reel angles"    python3 "$here/test_rotation.py"
run "fab: committed gerbers are current" python3 "$here/test_fab.py"
run "fab: JLCPCB rule limits"        "${KICAD_PY:-/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3}" "$here/test_jlc.py"
run "fab: CPL rotation audit"         python3 "$here/check_cpl_rotation.py"
run "firmware: board-target lockstep" python3 "$here/test_board_targets.py"
run "firmware: HID parser on host"    python3 "$here/test_parser.py"
run "firmware: descriptor regression" python3 "$here/test_descriptor.py"
run "firmware: OTA states match the page" python3 "$here/test_ota_states.py"
run "firmware: JSON escaping"          python3 "$here/test_json_escape.py"
run "firmware: OLED shows every LED state" python3 "$here/test_display_parity.py"
run "firmware: release notes abridging" python3 "$here/test_release_notes.py"
run "docs: published schematic matches the design" python3 "$here/test_schematic_docs.py"

echo
echo "── firmware: Rev A builds"
if [[ -n "${IDF_PATH:-}" ]]; then
    # Its own build directory and sdkconfig, generated from the defaults, so
    # whatever the working firmware/sdkconfig says cannot leak into the check.
    if ( cd "$root/firmware" && idf.py -B build-check -D SDKCONFIG=build-check/sdkconfig \
           -D SDKCONFIG_DEFAULTS=sdkconfig.defaults build >/tmp/build-REV_A.log 2>&1 ); then
        echo "  ok   REV_A builds"; pass=$((pass+1))
    else
        echo "  FAIL REV_A (see /tmp/build-REV_A.log)"; fail=$((fail+1))
    fi
else
    echo "  SKIP ESP-IDF not exported (. ~/esp/esp-idf/export.sh)"
    skip=$((skip+1))
fi

echo
echo "========================"
echo "passed $pass   failed $fail   skipped $skip"
[[ $fail == 0 ]]
