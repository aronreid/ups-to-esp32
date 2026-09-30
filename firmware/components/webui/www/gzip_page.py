# SPDX-License-Identifier: GPL-3.0-or-later
"""Compress the page for the firmware image: gzip, best compression, no name
or timestamp in the header, so the same page always builds the same bytes."""
import gzip
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = f.read()
with open(dst, "wb") as f:
    f.write(gzip.compress(data, compresslevel=9, mtime=0))
