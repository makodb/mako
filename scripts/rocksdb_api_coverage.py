#!/usr/bin/env python3
"""Regenerate docs/rocksdb-api-coverage.md.

Diffs the RocksDB C API this build links against
(/usr/include/rocksdb/c.h, override with ROCKSDB_C_HEADER) against what
crates/mrx-ffi/include/mrxdb_rocksdb_compat.h maps.

Run it after a RocksDB upgrade, or after adding to the compat header.
The prose at the top of the doc is preserved: only the generated lists
below the `<!-- GENERATED -->` marker are replaced, because the prose is
the part that says WHICH gaps are deliberate and which are structural,
and that is not derivable from a header diff.
"""

import os
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = Path(os.environ.get("ROCKSDB_C_HEADER", "/usr/include/rocksdb/c.h"))
COMPAT = ROOT / "crates/mrx-ffi/include/mrxdb_rocksdb_compat.h"
DOC = ROOT / "docs/rocksdb-api-coverage.md"
MARKER = "<!-- GENERATED: everything below is rebuilt by scripts/rocksdb_api_coverage.py -->"

# Longest prefix wins, so order matters.
PREFIXES = [
    ("rocksdb_transactiondb_options_", "transactions"),
    ("rocksdb_optimistictransaction_options_", "transactions"),
    ("rocksdb_transaction_options_", "transactions"),
    ("rocksdb_optimistictransactiondb_", "transactions"),
    ("rocksdb_optimistictransaction_", "transactions"),
    ("rocksdb_transactiondb_", "transactions"),
    ("rocksdb_transaction_", "transactions"),
    ("rocksdb_backup_engine_", "backup / restore / checkpoint"),
    ("rocksdb_restore_options_", "backup / restore / checkpoint"),
    ("rocksdb_checkpoint_", "backup / restore / checkpoint"),
    ("rocksdb_block_based_options_", "table & cache tuning"),
    ("rocksdb_cuckoo_options_", "table & cache tuning"),
    ("rocksdb_plain_table_options_", "table & cache tuning"),
    ("rocksdb_lru_cache_options_", "table & cache tuning"),
    ("rocksdb_hyper_clock_cache_options_", "table & cache tuning"),
    ("rocksdb_write_buffer_manager_", "table & cache tuning"),
    ("rocksdb_cache_", "table & cache tuning"),
    ("rocksdb_filterpolicy_", "pluggable behaviour"),
    ("rocksdb_mergeoperator_", "pluggable behaviour"),
    ("rocksdb_comparator_", "pluggable behaviour"),
    ("rocksdb_slicetransform_", "pluggable behaviour"),
    ("rocksdb_compactionfilter", "pluggable behaviour"),
    ("rocksdb_ratelimiter_", "environment / IO"),
    ("rocksdb_logger_", "environment / IO"),
    ("rocksdb_env_", "environment / IO"),
    ("rocksdb_sstfilewriter_", "bulk load / files"),
    ("rocksdb_ingestexternalfileoptions_", "bulk load / files"),
    ("rocksdb_livefiles_", "bulk load / files"),
    ("rocksdb_statistics_", "stats & introspection"),
    ("rocksdb_perfcontext_", "stats & introspection"),
    ("rocksdb_memory_", "stats & introspection"),
    ("rocksdb_column_family_", "column families"),
    ("rocksdb_writebatch_wi_", "write batch (indexed)"),
    ("rocksdb_writebatch_", "write batch (plain)"),
    ("rocksdb_readoptions_", "read/write/flush options"),
    ("rocksdb_writeoptions_", "read/write/flush options"),
    ("rocksdb_flushoptions_", "read/write/flush options"),
    ("rocksdb_compactoptions_", "read/write/flush options"),
    ("rocksdb_universal_compaction_options_", "DB options"),
    ("rocksdb_fifo_compaction_options_", "DB options"),
    ("rocksdb_options_", "DB options"),
    ("rocksdb_iter_", "iterators"),
    ("rocksdb_wal_", "WAL / replication"),
    ("rocksdb_snapshot", "snapshots"),
]


def group_of(name):
    for prefix, group in PREFIXES:
        if name.startswith(prefix):
            return group
    return "core DB operations"


def main():
    if not HEADER.exists():
        sys.exit(f"no RocksDB C header at {HEADER}; set ROCKSDB_C_HEADER")

    declared = set(
        re.findall(
            r"^extern ROCKSDB_LIBRARY_API [^(\n]*\b(rocksdb_[a-z0-9_]+)\(",
            HEADER.read_text(),
            re.M,
        )
    )
    compat = COMPAT.read_text()
    mapped = set(re.findall(r"^#define\s+(rocksdb_[a-z0-9_]+)", compat, re.M))
    mapped |= set(
        re.findall(r"^static inline [^(\n]*\b(rocksdb_[a-z0-9_]+)\(", compat, re.M)
    )

    supported = sorted(declared & mapped)
    missing = sorted(declared - mapped)

    by = defaultdict(list)
    for name in missing:
        by[group_of(name)].append(name)

    out = [MARKER, ""]
    out.append("| | count |")
    out.append("|---|---|")
    out.append(f"| RocksDB C functions | **{len(declared)}** |")
    out.append(f"| Supported here | **{len(supported)}** |")
    out.append(f"| Not supported | **{len(missing)}** |")
    out.append("")
    out.append("## Supported\n")
    out.extend(f"- `{n}`" for n in supported)
    for group in sorted(by, key=lambda g: (-len(by[g]), g)):
        out.append(f"\n## Not supported: {group} — {len(by[group])}\n")
        out.extend(f"- `{n}`" for n in by[group])
    generated = "\n".join(out) + "\n"

    if DOC.exists() and MARKER in DOC.read_text():
        prose = DOC.read_text().split(MARKER)[0]
    else:
        prose = DOC.read_text() if DOC.exists() else ""
    DOC.write_text(prose.rstrip() + "\n\n" + generated)
    print(f"{DOC}: {len(supported)} supported, {len(missing)} missing")


if __name__ == "__main__":
    main()
