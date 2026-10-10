#!/usr/bin/env python3
"""Generate docs/migration/raft/cpp-rust-correspondence.md from the tree.

WHY GENERATED. Every number in that document rotted within a day of being
typed: server_h.rs drifted 5009 -> 5017, transport.rs 460 -> 647, the kernel
count was wrong three different ways depending on how you counted, and the
"26 out-params" was a grep line-count rather than a count of out-parameters.
A figure a reader cannot refute is worse than no figure, so they are computed
here, with the counting rule stated beside each one.

  python3 scripts/gen_correspondence.py            rewrite the document
  python3 scripts/gen_correspondence.py --check    fail if it is stale
"""
import pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DOC = ROOT / "docs/migration/raft/cpp-rust-correspondence.md"
RS = sorted((ROOT / "src/deptran/raft/shell").glob("*.rs"))


def lines(rel):
    p = ROOT / rel
    return len(p.read_text(errors="replace").split("\n")) - 1 if p.is_file() else 0


def kernels():
    """Distinct raft_* names declared in `extern "C"` blocks under raft/shell.

    The union across files, de-duplicated: five names are declared in two
    modules each, and counting per-file double-counts them."""
    names = set()
    for f in RS:
        for block in re.finditer(r'unsafe extern "C" \{(.*?)\n\}', f.read_text(), re.S):
            names |= set(re.findall(r"\bfn (raft_\w+)", block.group(1)))
    return len(names)


# The runtime seam: raft-rt's kernels, its snapshot store (plan N4) and its
# lab kernels included. The C++ seam of the hybrid and cpp lanes went with
# those lanes (docs/verus/modification-plan.md, Q9).
RT_SEAM = [ROOT / "src/deptran/raft/rt/src" / f
           for f in ("seam.rs", "snapshot.rs", "lab_runtime.rs")]
FACADE = ROOT / "src/rusty-rustc/src/lib.rs"
LANE_KERNELS = ROOT / "src/deptran/raft/lane_kernels.h"
CPP_HOST = [p for p in (ROOT / "src/deptran").rglob("*.cc")
            if "/raft/rt/" not in str(p)]

# Kernels that are C++ only because of where the boundary sits: std time,
# env, a mutex, a thread sleep. Each could become plain Rust in the core (plan
# S1's CORE class); they are listed, not inferred, because "could be Rust" is
# a judgement, and it is the one column here that is.
CORE_CANDIDATES = {
    "raft_mutex_lock", "raft_mutex_unlock", "raft_std_mutex_lock",
    "raft_std_mutex_unlock", "raft_thread_sleep_ms",
    "raft_monotonic_now_us", "raft_monotonic_now_secs", "raft_time_now_us",
    "raft_random_range_us", "raft_env_lookup", "raft_env_snapshots_enabled",
    "raft_election_timeouts", "raft_heartbeat_interval_default",
    "raft_spawn_apply_thread", "raft_apply_thread_join",
}


def lane_kernel_entries():
    """The kernels lane_kernels.h declares, with the class its comment gives:
    each declaration takes the most recent `// LANE:` or `// HOST:` marker."""
    entries, current = {}, None
    for line in LANE_KERNELS.read_text().split("\n"):
        m = re.match(r"\s*//\s*(LANE|HOST):", line)
        if m:
            current = m.group(1)
            continue
        for name in re.findall(r"\b(raft_\w+)\s*\(", line):
            if current and not line.lstrip().startswith("//"):
                entries[name] = current
    return entries


def declared_kernels():
    """raft_* names the core crate and the rustc facade declare as imports,
    and those lane_kernels.h declares for HOST <-> seam calls."""
    names = set(lane_kernel_entries())
    for f in RS + [FACADE]:
        text = f.read_text()
        for block in re.finditer(r'(?:unsafe )?extern "C" \{(.*?)\n\s*\}', text, re.S):
            names |= set(re.findall(r"\bfn (raft_\w+)", block.group(1)))
        # the facade's macro-generated destroy / clone kernels
        names |= set(re.findall(r"=> (raft_\w+)", text))
    return names


def rust_defined(path):
    return set(re.findall(r'extern "C" fn (raft_\w+)', path.read_text()))


def cpp_defined(path):
    """raft_* functions DEFINED (not declared) in a C++ file: a name followed by
    a parameter list and then an opening brace before any ';'."""
    text = path.read_text(errors="replace")
    out = set()
    for m in re.finditer(r"\b(raft_\w+)\s*\(", text):
        head = text[max(0, m.start() - 160):m.start()]
        line_start = head.rfind("\n") + 1
        prefix = head[line_start:]
        if not re.match(r"^\s*(extern \"C\" )?(static )?[\w:<>*&, ]+[\s*&]$", prefix):
            continue
        depth, i = 0, m.end() - 1
        while i < len(text):
            c = text[i]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        tail = text[i + 1:i + 200]
        if re.match(r"\s*(const\s*)?(noexcept\s*)?(override\s*)?\{", tail):
            out.add(m.group(1))
    return out


def classify():
    """Every declared kernel, by where it is defined. The check half: a kernel
    is defined exactly once, by raft-rt's seam or by host C++."""
    declared = declared_kernels()
    rt = set().union(*(rust_defined(p) for p in RT_SEAM))
    marked = lane_kernel_entries()
    host = {}
    for f in CPP_HOST:
        for n in cpp_defined(f):
            host.setdefault(n, []).append(f.relative_to(ROOT / "src/deptran").as_posix())
    rows, problems = [], []
    for n in sorted(declared):
        in_rt, in_h = n in rt, n in host
        if in_rt and not in_h:
            cls = "SEAM"
        elif in_h and not in_rt and len(host[n]) == 1:
            cls = "HOST" + (" (CORE candidate)" if n in CORE_CANDIDATES else "")
        else:
            cls = "?"
            problems.append(f"{n}: rt={in_rt} host={host.get(n)}")
        want = {"LANE": "SEAM", "HOST": "HOST"}.get(marked.get(n))
        if want and not cls.startswith(want):
            problems.append(f"{n}: lane_kernels.h marks it {marked[n]} but it classifies {cls}")
        rows.append((n, cls))
    return rows, problems


def exports(header):
    """Prototypes in a C ABI header -- one per non-comment line ending in ');'."""
    text = (ROOT / header).read_text()
    return len([l for l in text.split("\n")
                if l.rstrip().endswith(");") and not l.lstrip().startswith("//")])


def catch_sites():
    """raft_catch CALL sites. grep counts the template definition too."""
    text = (ROOT / "src/deptran/raft/server.cc").read_text()
    return len(re.findall(r"raft_catch\(", text)) - len(
        re.findall(r"^\w[\w:<>,\s&*]*\braft_catch\(", text, re.M))


def out_params():
    """Methods with a `self` receiver AND a scalar out-pointer parameter."""
    n = 0
    for f in RS + [ROOT / "src/deptran/raft/shell/scheduler_h.rs"]:
        if not f.is_file():
            continue
        for m in re.finditer(r"\bfn \w+\(\s*&(?:mut )?self\b([^)]*)\)", f.read_text(), re.S):
            if re.search(r"\*mut (?:u\d+|i\d+|usize|bool)", m.group(1)):
                n += 1
    return n


PAIRS = [
    ("raft/server.h", "raft/shell/server_h.rs",
     "Rust owns it; the C++ left is kernels and a pointer-holding shim"),
    ("raft/server.cc", "raft/shell/server_cc.rs", ""),
    ("raft/service.cc", "raft/rt/src/service.rs",
     "the Rust serves; the C++ is still compiled, dead behind MAKO_RAFT_LANE_RUST"),
    ("raft/commo.cc", "raft/rt/src/transport.rs",
     "the Rust sends; the C++ is still compiled, dead behind MAKO_RAFT_LANE_RUST"),
    ("communicator.h", "raft/shell/communicator_h.rs",
     "ONE source: the Rust is transpiled into the C++ both engines link"),
]


def body():
    o = []
    w = o.append
    w("# Raft C++ <-> Rust: what corresponds to what")
    w("")
    w("GENERATED by `scripts/gen_correspondence.py` -- do not edit; re-run it.")
    w("The numbers were hand-typed once and every one was wrong within a day, so")
    w("they are computed from the tree with the counting rule stated beside each.")
    w("")
    w("No commit hash here on purpose: embedding one makes this file stale on")
    w("every commit whether or not a number moved, and a `--check` that cries")
    w("wolf gets switched off. `--check` is the freshness guarantee.")
    w("")
    w("## Where each piece lives")
    w("")
    w("| C++ | lines | Rust | lines | state |")
    w("|---|---|---|---|---|")
    for cpp, rs, note in PAIRS:
        w(f"| `{cpp}` | {lines('src/deptran/' + cpp)} | `{rs}` | "
          f"{lines('src/deptran/' + rs)} | {note} |")
    w(f"| `rcc_rpc.h` (Raft slice) | — | `raft/rt/src/rpc.rs` | "
      f"{lines('src/deptran/raft/rt/src/rpc.rs')} | generated from `rcc_rpc.rpc`; "
      f"ids frozen in `raft/rpc_ids.txt` |")
    w("")
    w("`communicator_h.rs` is extracted from the HEADER, not the `.cc` --")
    w("`raft/rust-modules.toml` names `src/deptran/communicator.h` as its source.")
    w("")
    w("## How the boundary is crossed")
    w("")
    w("| direction | mechanism | count |")
    w("|---|---|---|")
    w(f"| C++ → Rust | prototypes in `raft/server_exports.h` | "
      f"{exports('src/deptran/raft/server_exports.h')} |")
    w(f"| C++ → Rust | prototypes in `raft/transport_exports.h` (raft_lane_rust.cc) |"
      f" {exports('src/deptran/raft/transport_exports.h')} |")
    w(f"| Rust → C++ | distinct `raft_*` kernels declared in `extern \"C\"` blocks under "
      f"`raft/shell/` | {kernels()} |")
    w("")
    w("## Counted facts the prose below leans on")
    w("")
    w("| fact | count | counting rule |")
    w("|---|---|---|")
    w(f"| out-parameter methods | {out_params()} | a `self` receiver AND a "
      f"`*mut` scalar parameter. NOT a grep for `*mut u64`, which counts "
      f"kernel declarations and locals too |")
    w(f"| `raft_catch` sites | {catch_sites()} | CALL sites in `raft/server.cc`; "
      f"`grep -c` also counts the template definition |")
    w("")
    w("An export is a method of the Rust server the C++ shim calls. A kernel is")
    w("the other direction -- and most are C++ because of WHERE THE BOUNDARY SITS")
    w("today, not because Rust cannot express them. An earlier revision of this")
    w("file said kernels exist for \"the reactor, threads\"; both were wrong.")
    w("")
    rows, problems = classify()
    counts = {}
    for _, cls in rows:
        counts[cls.split(" ")[0]] = counts.get(cls.split(" ")[0], 0) + 1
    w("Every kernel the core or the rustc facade imports, CLASSIFIED BY WHERE IT")
    w("IS DEFINED (plan S1): **SEAM** -- defined by raft-rt's runtime seam")
    w("(`raft/rt/src/seam.rs`, the snapshot store, the lab kernels); **HOST** --")
    w("defined once in host C++ (Mako's objects: the Command payload, embedder")
    w("callbacks). A HOST kernel marked *CORE candidate* is C++ only by position")
    w("and could become plain Rust in the core. `--check` fails if a kernel is")
    w("defined nowhere or in two places.")
    w("")
    w("| class | count |")
    w("|---|---|")
    for k in sorted(counts):
        w(f"| {k} | {counts[k]} |")
    w("")
    w("| kernel | class |")
    w("|---|---|")
    for n, cls in rows:
        w(f"| `{n}` | {cls} |")
    w("")
    if problems:
        w("**Unclassifiable (fix the definitions):**")
        w("")
        for pr in problems:
            w(f"- `{pr}`")
        w("")
    w("Not kernels at all, despite an earlier revision listing them: rocksdb and")
    w("yaml-cpp. No kernel includes either (the old C++ RocksDB log backend the Rust")
    w("never called was deleted as dead code on 2026-10-10).")
    return o


def rest():
    """Everything after the generated tables is kept verbatim."""
    if not DOC.is_file():
        return []
    text = DOC.read_text().split("\n")
    for i, l in enumerate(text):
        if l.startswith("## How C++ idioms appear"):
            return text[i:]
    return []


def main():
    _, problems = classify()
    out = "\n".join(body() + [""] + rest()).rstrip() + "\n"
    if problems and "--check" in sys.argv:
        sys.exit("kernel classification has unclassifiable kernels:\n  "
                 + "\n  ".join(problems))
    if "--check" in sys.argv:
        if DOC.read_text() != out:
            sys.exit("cpp-rust-correspondence.md is stale; re-run "
                     "scripts/gen_correspondence.py")
        print(f"{DOC.name}: up to date")
        return 0
    DOC.write_text(out)
    print(f"wrote {DOC.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
