#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Find KiCad, wherever it is installed.

Every script here used to hardcode /Applications/KiCad/..., which is fine until
the repository is opened on a second machine -- a different install location, or
Linux -- and half the tools fail with a path that does not exist and no way to
say otherwise.

Resolution order, for each of the three things we need:

  1. the environment variable, so anything can be overridden without editing code
  2. a list of well-known install locations for macOS and Linux
  3. whatever is on PATH, for kicad-cli

Import from tools/ or tools/test/; both work.
"""
import os
import shutil

_ENV = {
    "cli":  "KICAD_CLI",
    "py":   "KICAD_PY",
    "share": "KICAD_SHARE",
}

_CANDIDATES = {
    "cli": [
        "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli",
        "/usr/bin/kicad-cli",
        "/usr/local/bin/kicad-cli",
        "/opt/homebrew/bin/kicad-cli",
    ],
    "py": [
        "/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/"
        "Versions/Current/bin/python3",
        "/usr/bin/python3",
    ],
    "share": [
        "/Applications/KiCad/KiCad.app/Contents/SharedSupport",
        "/usr/share/kicad",
        "/usr/local/share/kicad",
        "/opt/homebrew/share/kicad",
    ],
}


def _resolve(kind):
    env = os.environ.get(_ENV[kind])
    if env:
        return env
    for c in _CANDIDATES[kind]:
        if os.path.exists(c):
            return c
    if kind == "cli":
        found = shutil.which("kicad-cli")
        if found:
            return found
    # Nothing found. Return the first candidate so the error names a real path
    # and the message below tells the reader how to fix it.
    return _CANDIDATES[kind][0]


def cli():
    """Path to kicad-cli. Override with KICAD_CLI."""
    return _resolve("cli")


def python():
    """Path to KiCad's bundled python (the one with pcbnew). Override KICAD_PY."""
    return _resolve("py")


def share():
    """KiCad's SharedSupport / share directory. Override with KICAD_SHARE."""
    return _resolve("share")


def symbols():
    return os.path.join(share(), "symbols")


def footprints():
    return os.path.join(share(), "footprints")


def explain():
    return ("KiCad was not found. Set KICAD_CLI, KICAD_PY and KICAD_SHARE, or "
            "install KiCad in a standard location.")
