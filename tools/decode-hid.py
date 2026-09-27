#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode a USB HID report descriptor, focused on the Power Device classes.

Reference implementation for what firmware/components/ups_hid/hid_parser.c has
to do on the ESP32. Run it against a capture to get the authoritative list of
report IDs, usages, bit offsets and scaling, then check the firmware produces
the same map.

Usage:  tools/decode-hid.py captures/<dir>/report-descriptor.bin
"""
import sys
from collections import defaultdict

PAGE_POWER, PAGE_BATTERY = 0x84, 0x85

POWER = {
    0x01: "iName", 0x02: "PresentStatus", 0x03: "ChangedStatus", 0x04: "UPS",
    0x05: "PowerSupply", 0x10: "BatterySystem", 0x11: "BatterySystemID",
    0x12: "Battery", 0x13: "BatteryID", 0x14: "Charger", 0x15: "ChargerID",
    0x16: "PowerConverter", 0x17: "PowerConverterID", 0x18: "OutletSystem",
    0x19: "OutletSystemID", 0x1A: "Input", 0x1B: "InputID", 0x1C: "Output",
    0x1D: "OutputID", 0x1E: "Flow", 0x1F: "FlowID", 0x20: "Outlet",
    0x21: "OutletID", 0x22: "Gang", 0x23: "GangID", 0x24: "PowerSummary",
    0x25: "PowerSummaryID",
    0x30: "Voltage", 0x31: "Current", 0x32: "Frequency", 0x33: "ApparentPower",
    0x34: "ActivePower", 0x35: "PercentLoad", 0x36: "Temperature",
    0x37: "Humidity", 0x38: "BadCount",
    0x40: "ConfigVoltage", 0x41: "ConfigCurrent", 0x42: "ConfigFrequency",
    0x43: "ConfigApparentPower", 0x44: "ConfigActivePower",
    0x45: "ConfigPercentLoad", 0x46: "ConfigTemperature", 0x47: "ConfigHumidity",
    0x50: "SwitchOnControl", 0x51: "SwitchOffControl", 0x52: "ToggleControl",
    0x53: "LowVoltageTransfer", 0x54: "HighVoltageTransfer",
    0x55: "DelayBeforeReboot", 0x56: "DelayBeforeStartup",
    0x57: "DelayBeforeShutdown", 0x58: "Test", 0x59: "ModuleReset",
    0x5A: "AudibleAlarmControl",
    0x60: "Present", 0x61: "Good", 0x62: "InternalFailure",
    0x63: "VoltageOutOfRange", 0x64: "FrequencyOutOfRange", 0x65: "Overload",
    0x66: "OverCharged", 0x67: "OverTemperature", 0x68: "ShutdownRequested",
    0x69: "ShutdownImminent", 0x6B: "SwitchOnOff", 0x6C: "Switchable",
    0x6D: "Used", 0x6E: "Boost", 0x6F: "Buck", 0x70: "Initialized",
    0x71: "Tested", 0x72: "AwaitingPower", 0x73: "CommunicationLost",
    0xFD: "iManufacturer", 0xFE: "iProduct", 0xFF: "iSerialNumber",
}
BATTERY = {
    0x85: "ManufacturerDate",
    0x01: "SMBBatteryMode", 0x02: "SMBBatteryStatus", 0x03: "SMBAlarmWarning",
    0x04: "SMBChargerMode", 0x05: "SMBChargerStatus",
    0x28: "ManufacturerDate", 0x29: "RemainingCapacityLimit",
    0x2C: "CapacityMode",
    # Corrected against a real APC PresentStatus bitfield: the earlier table
    # here was off by one across 0x42/0x44/0x45, which mislabelled Charging as
    # Discharging -- an inversion that would have reported OB for OL.
    0x42: "BelowRemainingCapacityLimit", 0x43: "RemainingTimeLimitExpired",
    0x44: "Charging", 0x45: "Discharging", 0x46: "FullyCharged",
    0x47: "FullyDischarged", 0x4B: "NeedReplacement",
    0xD0: "ACPresent", 0xD1: "BatteryPresent", 0xDB: "VoltageNotRegulated",
    0x65: "AbsoluteStateOfCharge", 0x66: "RemainingCapacity",
    0x67: "FullChargeCapacity", 0x68: "RunTimeToEmpty",
    0x69: "AverageTimeToEmpty", 0x6A: "AverageTimeToFull",
    0x83: "DesignCapacity", 0x89: "iDeviceChemistry", 0x8B: "ManufacturerData",
    0x8D: "CapacityGranularity1", 0x8E: "CapacityGranularity2",
    0x8F: "iOEMInformation",
}

def unit_offset(unit):
    """Decimal offset between HID base units and conventional SI units.

    HID SI Linear measures in cm and g, not m and kg, so a reported exponent is
    relative to those: offset = 2*length + 3*mass. Gives 7 for volts and watts,
    0 for amps, seconds, hertz and dimensionless. True scale = 10^(exp-offset).
    Must match hid_unit_decimal_offset() in hid_parser.c.
    """
    if unit == 0:
        return 0
    if (unit & 0xF) not in (1, 2):
        return 0
    def nib(sh):
        v = (unit >> sh) & 0xF
        return v - 16 if v > 7 else v
    return 2 * nib(4) + 3 * nib(8)


def name(page, usage):
    t = POWER if page == PAGE_POWER else BATTERY if page == PAGE_BATTERY else {}
    return t.get(usage, f"0x{usage:02x}")

MAIN_TAGS = {0x08: "Input", 0x09: "Output", 0x0B: "Feature",
             0x0A: "Collection", 0x0C: "EndCollection"}

# Collections that disambiguate a value. UPS and PresentStatus wrap everything
# and say nothing, so they are not significant.
SIGNIFICANT_COLL = {
    0x10: "BatterySystem", 0x12: "Battery", 0x14: "Charger",
    0x16: "PowerConverter", 0x18: "OutletSystem", 0x1A: "Input",
    0x1C: "Output", 0x1E: "Flow", 0x20: "Outlet", 0x24: "PowerSummary",
}

# Must stay in step with ups_varmap.c.
VARMAP = {
    (PAGE_POWER, 0x30, 0x1A): "input.voltage",
    (PAGE_POWER, 0x40, 0x1A): "input.voltage.nominal",
    (PAGE_POWER, 0x31, 0x1A): "input.current",
    (PAGE_POWER, 0x32, 0x1A): "input.frequency",
    (PAGE_POWER, 0x53, 0x1A): "input.transfer.low",
    (PAGE_POWER, 0x54, 0x1A): "input.transfer.high",
    (PAGE_POWER, 0x30, 0x1C): "output.voltage",
    (PAGE_POWER, 0x40, 0x1C): "output.voltage.nominal",
    (PAGE_POWER, 0x32, 0x1C): "output.frequency",
    (PAGE_POWER, 0x30, 0x12): "battery.voltage",
    (PAGE_POWER, 0x40, 0x12): "battery.voltage.nominal",
    (PAGE_POWER, 0x36, 0x12): "battery.temperature",
}
VARMAP_ANY = {
    (PAGE_BATTERY, 0x66): "battery.charge",
    (PAGE_BATTERY, 0x29): "battery.charge.low",
    (PAGE_BATTERY, 0x68): "battery.runtime",
    (PAGE_BATTERY, 0x83): "battery.capacity",
    (PAGE_BATTERY, 0x67): "battery.capacity.full",
    (PAGE_BATTERY, 0x89): "battery.type",
    (PAGE_BATTERY, 0x28): "battery.mfr.date",
    (PAGE_POWER, 0x35): "ups.load",
    (PAGE_POWER, 0x34): "ups.realpower",
    (PAGE_POWER, 0x44): "ups.realpower.nominal",
    (PAGE_POWER, 0x33): "ups.power",
    (PAGE_POWER, 0x43): "ups.power.nominal",
    (PAGE_POWER, 0x36): "ups.temperature",
    (PAGE_POWER, 0x57): "ups.delay.shutdown",
    (PAGE_POWER, 0x56): "ups.delay.start",
    (PAGE_POWER, 0x55): "ups.delay.reboot",
    (PAGE_POWER, 0x58): "ups.test.result",
    (PAGE_POWER, 0x5A): "ups.beeper.status",
    (PAGE_POWER, 0xFD): "ups.mfr",
    (PAGE_POWER, 0xFE): "ups.model",
    (PAGE_POWER, 0xFF): "ups.serial",
}


def nut_name(page, usage, coll):
    """NUT variable name, or None when the usage has no standard equivalent."""
    return VARMAP.get((page, usage, coll)) or VARMAP_ANY.get((page, usage))

def decode(buf):
    page = rid = 0
    rsize = rcount = 0
    lmin = lmax = 0
    exp = 0
    unit = 0
    usages = []
    umin = umax = None
    offsets = defaultdict(lambda: defaultdict(int))   # rid -> type -> bits
    rows = []
    stack = []            # (page, usage) per open collection
    i = 0
    while i < len(buf):
        b = buf[i]; i += 1
        if b == 0xFE:
            sz = buf[i]; i += 2 + sz; continue
        size = b & 3
        size = 4 if size == 3 else size
        typ = (b >> 2) & 3
        tag = (b >> 4) & 0xF
        data = int.from_bytes(buf[i:i+size], "little") if size else 0
        sdata = int.from_bytes(buf[i:i+size], "little", signed=True) if size else 0
        i += size

        if typ == 1:      # Global
            if   tag == 0x0: page = data
            elif tag == 0x1: lmin = sdata
            elif tag == 0x2: lmax = sdata
            elif tag == 0x5: exp = data - 16 if data > 7 else data
            elif tag == 0x6: unit = data
            elif tag == 0x7: rsize = data
            elif tag == 0x8: rid = data
            elif tag == 0x9: rcount = data
        elif typ == 2:    # Local
            if   tag == 0x0: usages.append(data)
            elif tag == 0x1: umin = data
            elif tag == 0x2: umax = data
        elif typ == 0:    # Main
            label = MAIN_TAGS.get(tag)
            if label == "Collection":
                stack.append((page, usages[-1] if usages else 0))
                usages, umin, umax = [], None, None
                continue
            if label == "EndCollection":
                if stack:
                    stack.pop()
                usages, umin, umax = [], None, None
                continue
            if label in ("Input", "Output", "Feature"):
                coll = 0
                for pg, u in reversed(stack):
                    if pg == PAGE_POWER and u in SIGNIFICANT_COLL:
                        coll = u
                        break
                ulist = list(usages)
                if umin is not None and umax is not None:
                    ulist += list(range(umin, umax + 1))
                const = bool(data & 1)
                for n in range(rcount):
                    off = offsets[rid][label]
                    u = ulist[n] if n < len(ulist) else (ulist[-1] if ulist else None)
                    if page in (PAGE_POWER, PAGE_BATTERY) and u is not None and not const:
                        rows.append(dict(rid=rid, type=label, off=off, size=rsize,
                                         page=page, usage=u, name=name(page, u),
                                         lmin=lmin, lmax=lmax, exp=exp, unit=unit,
                                         shift=exp - unit_offset(unit),
                                         coll=SIGNIFICANT_COLL.get(coll, "-"),
                                         nut=nut_name(page, u, coll)))
                    offsets[rid][label] += rsize
            usages, umin, umax = [], None, None
    return rows

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    buf = open(sys.argv[1], "rb").read()
    rows = decode(buf)
    print(f"{len(buf)} descriptor bytes, {len(rows)} power/battery fields\n")
    hdr = f"{'RID':>3} {'Type':<8} {'Off':>4} {'Sz':>3} {'Usage':<28} {'LogMax':>7} {'Unit':>9} {'Exp':>4} {'Shift':>6} {'Example':>12}"
    print(hdr); print("-" * len(hdr))
    for r in rows:
        ex = r['lmax'] * (10 ** r['shift'])
        ex = f"{ex:.4g}"
        print(f"{r['rid']:>3} {r['type']:<8} {r['off']:>4} {r['size']:>3} "
              f"{r['name']:<28} {r['lmax']:>7} {hex(r['unit']):>9} {r['exp']:>4} "
              f"{r['shift']:>6} {ex:>12}")
    print()
    feat = sum(1 for r in rows if r["type"] == "Feature")
    inp  = sum(1 for r in rows if r["type"] == "Input")
    print(f"Feature fields: {feat}   Input fields: {inp}")
    print(f"Distinct report IDs: {sorted({r['rid'] for r in rows})}")

    print("\n=== NUT variables this device would publish ===")
    pub = {}
    for r in rows:
        if r["nut"] and r["type"] == "Feature":
            pub.setdefault(r["nut"], r)
    for nm in sorted(pub):
        r = pub[nm]
        print(f"  {nm:<26} report {r['rid']:>3}  {r['coll']:<15} "
              f"max {r['lmax'] * 10 ** r['shift']:.4g}")
    unmapped = sorted({(r['page'], r['usage']) for r in rows if not r['nut']})
    print(f"\n  {len(pub)} NUT variables, {len(unmapped)} usages with no NUT name")
    print("  (unmapped usages still reach /api/raw and MQTT):")
    for pg, u in unmapped:
        print(f"    {'Power' if pg == PAGE_POWER else 'Battery'} 0x{u:02x}  {name(pg, u)}")

if __name__ == "__main__":
    main()
