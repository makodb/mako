"""Shared utilities for reading .res result files.

The new build produces verbose log output (6–120 MB per .res file), but
summary statistics (throughput, latency percentiles, CPU) are always in the
last ~100 lines.  This module provides ``read_res_tail`` which reads only the
tail of a file, avoiding the O(GB) full-file reads that would otherwise be
needed.
"""

import os

# Upper bound: skip files larger than 2 GB (likely corrupt / infinite loop).
MAX_RES_FILE_SIZE = 2_000_000_000

# How many bytes from the end of the file to read.  100 KB is enough for
# ~1000 lines of summary output while keeping I/O fast.
TAIL_BYTES = 100_000


def read_res_tail(filepath, tail_bytes=None):
    """Return the last *tail_bytes* of *filepath* as a list of lines.

    Returns an empty list when the file is missing, unreadable, or
    exceeds ``MAX_RES_FILE_SIZE``.
    """
    if tail_bytes is None:
        tail_bytes = TAIL_BYTES
    try:
        size = os.path.getsize(filepath)
        if size > MAX_RES_FILE_SIZE:
            return []
        with open(filepath, 'rb') as f:
            if size > tail_bytes:
                f.seek(-tail_bytes, os.SEEK_END)
                # Skip partial first line
                f.readline()
            data = f.read()
        return data.decode('utf-8', errors='replace').splitlines()
    except (FileNotFoundError, IOError, OSError):
        return []


def read_res_full(filepath):
    """Read the full .res file, returning lines.

    Only use this when you need content from the beginning of the file
    (e.g., timestamps for wall-time measurement).  For summary stats,
    prefer ``read_res_tail``.

    Returns an empty list when the file is missing or exceeds the size limit.
    """
    try:
        size = os.path.getsize(filepath)
        if size > MAX_RES_FILE_SIZE:
            return []
        with open(filepath, 'r', errors='replace') as f:
            return f.readlines()
    except (FileNotFoundError, IOError, OSError):
        return []
