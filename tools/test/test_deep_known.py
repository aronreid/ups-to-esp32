#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The web page's DEEP_KNOWN table must agree with docs/tested-ups.md.

The page tells people, before they press Deep test, what their UPS model does
with it -- disables the button on one, warns about frozen reports on another.
Those are claims about hardware, and the evidence lives in tested-ups.md. So
every model in the table must be in that document, and what the page says
(`quick`: "deep" runs the quick test; `hides`: a real run-down whose reports
may freeze) must match what the document measured. An entry added to the page
without the measurement written up fails here.
"""
import pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
page = (ROOT / "firmware/components/webui/www/index.html").read_text()
doc = (ROOT / "docs/tested-ups.md").read_text()

m = re.search(r"const DEEP_KNOWN=\[(.*?)\n\];", page, re.S)
if not m:
    sys.exit("FAIL: DEEP_KNOWN not found in index.html")
entries = re.findall(r"model:/(.*?)/i,\s*deep:'(\w+)'", m.group(1))
if not entries:
    sys.exit("FAIL: DEEP_KNOWN has no entries the test can read")

# What the tested-ups deep-test row must say for each kind of entry.
EXPECT = {"quick": r"runs the quick test instead", "hides": r"works, but hides"}
row = next((l for l in doc.splitlines() if l.startswith("| `test.battery.start.deep`")), None)
if row is None:
    sys.exit("FAIL: tested-ups.md has no test.battery.start.deep row")
header = next(l for l in doc.splitlines() if l.startswith("| Command |"))
cols = [c.strip() for c in header.strip("|").split("|")]
cells = [c.strip() for c in row.strip("|").split("|")]

fails = 0
for model_re, kind in entries:
    model = model_re.replace("\\", "")
    if kind not in EXPECT:
        print(f"FAIL: {model}: unknown kind '{kind}'"); fails += 1; continue
    # Match on the model number ("EC850LCD", "1000G"): the page's pattern and
    # the document's column header name the maker differently.
    code = [w for w in model.split() if re.search(r"\d", w)][-1]
    col = next((i for i, c in enumerate(cols) if code.lower() in c.lower()), None)
    if col is None:
        print(f"FAIL: {model}: no column in tested-ups.md's command table"); fails += 1; continue
    if not re.search(EXPECT[kind], cells[col], re.I):
        print(f"FAIL: {model}: page says '{kind}', tested-ups.md says: {cells[col]}"); fails += 1; continue
    print(f"ok   {model}: '{kind}' matches the measured result")
sys.exit(1 if fails else 0)
