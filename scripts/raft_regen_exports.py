#!/usr/bin/env python3
"""Regenerate the three generated surfaces of the Raft C ABI from the one
signature table in scripts/raft_gen_exports.py:

  - the `extern "C"` exports, spliced between the GENERATED EXPORTS markers in
    src/deptran/raft/shell/server_cc.rs (canonical Rust; nothing else there is
    touched),
  - src/deptran/raft/server_exports.h, the prototypes hand-written C++ uses,
  - the pointer-holding shim `class RaftServer` in src/deptran/raft/server.h.

Run after changing a method's signature in server_h.rs or the generator's
lists; then `bash scripts/raft_dsl.sh --check` and `cargo clippy` in
src/deptran/raft. See the Raft migration plan (removed; see git history), F1 and F2.6.
"""
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GEN = REPO / "scripts" / "raft_gen_exports.py"
RS = REPO / "src" / "deptran" / "raft" / "shell" / "server_cc.rs"
HEADER = REPO / "src" / "deptran" / "raft" / "server_exports.h"
SERVER_H = REPO / "src" / "deptran" / "raft" / "server.h"
BEGIN = "// --- GENERATED EXPORTS BEGIN (scripts/raft_gen_exports.py; do not edit by hand) ---\n"
END = "// --- GENERATED EXPORTS END ---\n"


def run(*args: str) -> str:
    return subprocess.run([sys.executable, str(GEN), *args], capture_output=True, text=True, check=True).stdout


def main() -> int:
    rs = RS.read_text()
    start = rs.index(BEGIN) + len(BEGIN)
    end = rs.index(END)
    RS.write_text(rs[:start] + run() + "\n" + rs[end:])
    HEADER.write_text(run("--header"))
    h = SERVER_H.read_text()
    a = h.index("class RaftServer : public RaftSpecific {")
    b = h.index("\n};\n", a) + len("\n};\n")
    out = h[:a] + run("--shim") + "\n" + h[b:]
    # the splice must not accumulate blank lines from one run to the next
    out = re.sub(r"\n{3,}(\} // namespace janus)", r"\n\n\1", out)
    SERVER_H.write_text(out)
    print("regenerated: exports (server_cc.rs), server_exports.h, the RaftServer shim (server.h)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
