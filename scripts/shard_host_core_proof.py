#!/usr/bin/env python3
"""The proof gate of crates/shard-host-core. Run by scripts/verify_shard_host_core.sh.

In this order:

1. Source audit. Verus ignores what stands outside `verus!` without a word, and
   plain cargo compiles it all the same. So every item of the crate must be
   inside the macro, nothing may be marked for the verifier to skip or trust,
   no function the shell calls may have a precondition (the shell is not
   verified, so nothing would check it), the shell may not define one of the
   core's names again, and the shell functions that call the core must be the
   ones scripts/shard_host_core_trusted.txt lists. The Cargo library must use
   this source tree. The audit rejects conditional attributes and tests that
   rejection before building the core and shell with plain Cargo.
2. Pins. The crates plain cargo compiles (vstd and the Verus macro crates, the
   archives with the checksums of crates/Cargo.lock) must hold the sources the
   pinned Verus release carries: Verus proves against its own bundled vstd and
   erases with its own bundled macro, cargo builds with the ones from
   crates.io.
3. The proof: Verus over the whole crate with --no-cheating.
4. Negative controls: each deliberately wrong variant of the crate must be
   rejected by Verus for a failed proof obligation in the named function.
   A compile error, a timeout or a resource limit is not a rejection.

The audit is a text check, not a proof. What the gate does not cover is said in
the header of scripts/verify_shard_host_core.sh.
"""
from pathlib import Path
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
CRATE = ROOT / "crates" / "shard-host-core"
SHELL = ROOT / "crates" / "sto-tpcc-ffi" / "src" / "shard_host.rs"
TRUSTED = ROOT / "scripts" / "shard_host_core_trusted.txt"
ENTRY = "crates/shard-host-core/src/lib.rs"
CRATES_VERSION = "0.0.0-2026-08-02-0125"
# Directory in the Verus release -> the package cargo compiles.
RELEASE_SOURCES = {
    "vstd": "vstd",
    "builtin": "verus_builtin",
    "builtin_macros": "verus_builtin_macros",
    "state_machines_macros": "verus_state_machines_macros",
}
# What `cargo publish` adds to a package next to the repository's files. The
# release keeps the repository's Cargo.toml, the package keeps it as .orig.
PACKAGING = {"Cargo.lock", "Cargo.toml.orig", ".cargo_vcs_info.json", "README.md"}
# The functions the shell calls, as module::function.
ENTRY_POINTS = ["status::map_engine", "scan::scan_flags_valid", "scan::scan_arguments", "scan::fixed_scan", "verbs::write_cell", "walk::scan_first"]

VERUS = shutil.which(os.environ.get("VERUS_PATH", "verus"))
if VERUS is None:
    raise SystemExit("Set VERUS_PATH to the pinned Verus executable")
SYSROOT_ARGS = (["--sysroot", os.environ["VERUS_SYSROOT"]]
                if os.environ.get("VERUS_SYSROOT") else [])
VERUS_ARGS = [VERUS, *SYSROOT_ARGS, "--crate-type=lib", "--edition=2021", ENTRY,
              "--no-cheating", "--triggers-mode", "silent"]


def code_of(path):
    """The file without comments. The crate has no string that holds `//`."""
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(), flags=re.S)


# Preserve Rust tokens around comments. In particular, comment markers inside
# a string cannot hide the attribute on the next item. Keep lifetimes intact.
RUST_LITERAL_OR_COMMENT = re.compile(
    r"""//[^\n]*|/\*|(?:br|cr|r)(?P<hashes>\#*)"(?s:.*?)"(?P=hashes)"""
    r"""|"(?:\\.|[^"\\])*"|'(?:\\(?:x[0-9a-fA-F]{2}|u\{[0-9a-fA-F_]+\}|.)|[^'\\\n])'""",
    re.S,
)
BLOCK_DELIMITER = re.compile(r"/\*|\*/")


def core_code(source):
    parts = []
    end = 0
    while match := RUST_LITERAL_OR_COMMENT.search(source, end):
        parts.append(source[end:match.start()])
        end = match.end()
        token = match[0]
        if token == "/*":
            depth = 1
            for delimiter in BLOCK_DELIMITER.finditer(source, end):
                depth += 1 if delimiter[0] == "/*" else -1
                if depth == 0:
                    end = delimiter.end()
                    break
            else:
                raise SystemExit("Unclosed block comment in verified source")
        parts.append(" " if token.startswith(("//", "/*")) else '""')
    return "".join(parts) + source[end:]


def audit_configuration(crate):
    manifest = tomllib.loads((crate / "Cargo.toml").read_text())
    if manifest.get("lib") != {"path": "src/lib.rs"}:
        raise SystemExit("shard-host-core: Cargo must build the verified library at src/lib.rs")
    if (crate / "build.rs").exists() or manifest["package"].get("build", False) is not False:
        raise SystemExit("shard-host-core: build scripts may change the compiled library")
    if "features" in manifest or "target" in manifest:
        raise SystemExit("shard-host-core: conditional Cargo features or dependencies")
    if manifest["dependencies"] != {name: "=" + CRATES_VERSION for name in
                                    ("vstd", "verus_builtin", "verus_builtin_macros")}:
        raise SystemExit("shard-host-core: expected only the three pinned Verus dependencies")


# These are the attributes used in this verified crate. A new attribute
# needs review because it can change which implementation cargo compiles.
DERIVE = r"(?:Clone|Copy|Debug|Eq|PartialEq)"
CORE_ATTRIBUTE = re.compile(
    rf"\#\s*\[\s*derive\s*\(\s*{DERIVE}(?:\s*,\s*{DERIVE})*\s*,?\s*\)\s*\]"
)


def audit_core_tokens(name, code):
    for attr in re.finditer(r"#\s*!?\s*\[", code):
        if name.name == "lib.rs" and code.startswith("#![forbid(unsafe_code)]", attr.start()):
            continue
        if not CORE_ATTRIBUTE.match(code, attr.start()):
            raise SystemExit(f"{name}: unreviewed attribute in verified source")
    if re.search(r"\b(?:cfg|include|include_bytes|include_str|macro_rules)\b", code):
        raise SystemExit(f"{name}: conditional compilation or source inclusion in verified source")


def audit_controls():
    # Apply each attribute to an existing function in memory. These exercise
    # the same source checks as the baseline without changing the checkout.
    path = CRATE / "src" / "status.rs"
    source = path.read_text()
    site = "pub const fn map_engine("
    if source.count(site) != 1:
        raise SystemExit("Configuration audit control site missing or ambiguous")
    for attr in ("#[cfg(verus_keep_ghost)]", "# [ /* guard */ cfg_attr(test, inline) ]"):
        variant = source.replace(site, attr + "\n" + site)
        try:
            audit_core_tokens(path, core_code(variant))
        except SystemExit as error:
            if "unreviewed attribute" not in str(error):
                raise
        else:
            raise SystemExit(f"Configuration audit accepted {attr}")
    print("PASS: source audit rejects cfg and cfg_attr on map_engine")


def audit_sources():
    audit_configuration(CRATE)
    exported = set()
    files = sorted((CRATE / "src").rglob("*.rs"))
    for path in files:
        name = path.relative_to(ROOT)
        code = core_code(path.read_text())
        audit_core_tokens(name, code)
        checked = code.replace("#![forbid(unsafe_code)]", "") if path.name == "lib.rs" else code
        for escape in (r"verifier::", r"\bassume\s*\(", r"\badmit\s*\(", r"\bassume_specification\b",
                       r"\bexternal", r"\brlimit\b", r"\bunsafe"):
            found = re.search(escape, checked)
            if found:
                raise SystemExit(f"{name}: `{found[0]}` is not allowed in the verified crate")
        if path.name == "lib.rs":
            if "#![forbid(unsafe_code)]" not in code:
                raise SystemExit(f"{name}: #![forbid(unsafe_code)] is missing")
            rest = re.sub(r"#!\[forbid\(unsafe_code\)\]|pub mod \w+;|pub use [^;]+;", "", code)
            if rest.strip():
                raise SystemExit(f"{name}: only module declarations and re-exports may stand "
                                 f"outside verus!, found: {rest.strip()[:80]!r}")
            declared = set(re.findall(r"pub mod (\w+);", code))
            present = {p.stem for p in files if p.name != "lib.rs"}
            if declared != present:
                raise SystemExit(f"{name}: modules {sorted(declared)} but files {sorted(present)}")
            continue
        start = code.find("verus! {")
        if start < 0:
            raise SystemExit(f"{name}: no verus! block")
        before = re.sub(r"use [^;]+;", "", code[:start])
        if before.strip():
            raise SystemExit(f"{name}: only imports may stand before verus!, "
                             f"found: {before.strip()[:80]!r}")
        depth, end = 0, None
        for index in range(code.index("{", start), len(code)):
            depth += {"{": 1, "}": -1}.get(code[index], 0)
            if depth == 0:
                end = index
                break
        if end is None or code[end + 1:].strip():
            raise SystemExit(f"{name}: code after the verus! block")
        body = code[start:end]
        for match in re.finditer(r"((?:pub(?:\([^)]*\))?\s+)?(?:open\s+|closed\s+)?"
                                 r"(?:const\s+)?(?:spec\s+|proof\s+)?)fn\s+(\w+)", body):
            qualifiers, function = match[1], match[2]
            if "spec" in qualifiers or "proof" in qualifiers:
                continue
            # Trait contracts end at a semicolon instead of a function body.
            # Both forms are checked for executable preconditions.
            end = re.search(r"[;{]", body[match.end():])
            if end is None:
                raise SystemExit(f"{name}: unterminated function {function}")
            header = body[match.end():match.end() + end.start()]
            engine_method = (path.name == "engine.rs" and end[0] == ";"
                             and function in {"stage", "validate", "finish", "commit"})
            if re.search(r"\brequires\b", header) and not engine_method:
                raise SystemExit(f"{name}: `{function}` runs and has a precondition; the "
                                 "unverified shell could call it wrongly")
            if "pub" in qualifiers:
                exported.add(function)
        exported.update(re.findall(r"pub\s+(?:const|enum|struct|type)\s+(\w+)", body))
    shell = code_of(SHELL)
    for name in sorted(exported):
        if re.search(r"\b(?:fn|const|enum|struct|type|static)\s+" + name + r"\b", shell):
            raise SystemExit(f"{SHELL.relative_to(ROOT)} defines `{name}` again; the verified "
                             "definition must be the only one")
    # The shell functions that call the core are exactly the trusted list. The
    # shell's test module is not part of what runs.
    production = shell.split("#[cfg(test)]\nmod tests", 1)[0]
    functions = [(match.start(), match[1]) for match in re.finditer(r"\bfn\s+(\w+)", production)]
    callers = set()
    for entry in ENTRY_POINTS:
        function = entry.split("::")[1]
        if function not in exported:
            raise SystemExit(f"entry point {entry} is not a public function of the crate")
        calls = [match.start() for match in re.finditer(r"\b" + function + r"\(", production)]
        if not calls:
            raise SystemExit(f"{SHELL.relative_to(ROOT)} does not call `{function}`")
        for call in calls:
            enclosing = [name for start, name in functions if start < call]
            if not enclosing:
                raise SystemExit(f"{SHELL.relative_to(ROOT)}: `{function}` is called outside a function")
            callers.add(f"{SHELL.name}:{enclosing[-1]}")
    trusted = {line.split()[0] for line in TRUSTED.read_text().splitlines()
               if line.strip() and not line.startswith("#")}
    if callers != trusted:
        raise SystemExit(f"{TRUSTED.relative_to(ROOT)} and the shell disagree: calls the core but "
                         f"is not listed {sorted(callers - trusted)}; listed but does not call "
                         f"the core {sorted(trusted - callers)}")
    print(f"Source audit: {len(files)} files, all items inside verus!, no verifier escapes, "
          f"total public entries and explicit engine method contracts, {len(exported)} public names defined once, "
          f"{len(trusted)} trusted shell functions call the core; Verus proof follows")


def check_pins():
    lock = tomllib.loads((ROOT / "crates" / "Cargo.lock").read_text())
    cargo_home = Path(os.environ.get("CARGO_HOME") or Path.home() / ".cargo")
    release = Path(VERUS).resolve().parent
    for directory, package in RELEASE_SOURCES.items():
        locked = [p for p in lock["package"] if p["name"] == package]
        if len(locked) != 1 or locked[0]["version"] != CRATES_VERSION or not locked[0].get(
                "source", "").startswith("registry+"):
            raise SystemExit(f"crates/Cargo.lock: expected exactly {package} {CRATES_VERSION} "
                             f"from crates.io, found {locked}")
        # The archive cargo downloaded, held to the checksum of the lock file.
        archives = sorted(cargo_home.glob(f"registry/cache/*/{package}-{CRATES_VERSION}.crate"))
        archives = [a for a in archives
                    if hashlib.sha256(a.read_bytes()).hexdigest() == locked[0]["checksum"]]
        if not archives:
            raise SystemExit(f"{package}: no archive with the lock file's checksum under "
                             f"{cargo_home}/registry/cache (build the crate with cargo first)")
        prefix = f"{package}-{CRATES_VERSION}/"
        with tarfile.open(archives[0]) as archive:
            theirs = {member.name.removeprefix(prefix): archive.extractfile(member).read()
                      for member in archive.getmembers() if member.isfile()}
        ours = {p.relative_to(release / directory).as_posix(): p.read_bytes()
                for p in (release / directory).rglob("*") if p.is_file()}
        if set(theirs) - PACKAGING != set(ours):
            raise SystemExit(f"{package}: the crates.io package and the Verus release hold "
                             f"different files: {sorted((set(theirs) - PACKAGING) ^ set(ours))[:8]}")
        for relative, content in sorted(ours.items()):
            other = "Cargo.toml.orig" if relative == "Cargo.toml" else relative
            if content != theirs[other]:
                raise SystemExit(f"{package}: {relative} differs between the Verus release and "
                                 "the crates.io package cargo compiles")
        print(f"Pin: {package} {CRATES_VERSION} (crates.io, checksum of crates/Cargo.lock) is "
              f"byte for byte the release's {directory}: {len(ours)} files")


# (what the variant does wrong, file, the text, its replacement, (module, function))
CONTROLS = [
    (
        "a staged write is finished read-only instead of committed",
        "src/verbs.rs",
        "let report = engine.commit();",
        "let report = engine.finish(true);",
        ("verbs", "write_cell"),
    ),
    (
        "an absent deletion bypasses read validation",
        "src/verbs.rs",
        "let validated = engine.validate();",
        "let validated = ENGINE_OK;",
        ("verbs", "write_cell"),
    ),
    (
        "a failed stage returns without closing its attempt",
        "src/verbs.rs",
        "    close(engine, staged)",
        "    Mapped::Shard(SHARD_IO)",
        ("verbs", "write_cell"),
    ),
    (
        "a skipped tombstone hop is reported as end of range",
        "src/walk.rs",
        "Ok(Hop::Resume(key)) => after = Some(key),",
        "Ok(Hop::Resume(key)) => return Ok(None),",
        ("walk", "scan_first"),
    ),
    (
        "a fixed warehouse repeats the cursor row when coordinate and cursor coincide",
        "src/scan.rs",
        "else if cursor_order == 0 {\n            FixedScan::AfterKey",
        "else if cursor_order == 0 {\n            FixedScan::AllKeys",
        ("scan", "fixed_scan"),
    ),
    (
        "a fixed warehouse includes a coordinate equal to the exclusive end",
        "src/scan.rs",
        "if bounded && compare(coordinate, hi) >= 0",
        "if bounded && compare(coordinate, hi) > 0",
        ("scan", "fixed_scan"),
    ),
    (
        "a commit whose report is not definite is answered as an error",
        "src/status.rs",
        "            _ => Mapped::FailStop,\n        },\n        Stage::ReadOnlyFinish",
        "            _ => map_failure(status),\n        },\n        Stage::ReadOnlyFinish",
        ("status", "map_engine"),
    ),
    (
        "a commit is answered OK without a committed report",
        "src/status.rs",
        "Stage::Commit => match (status, disposition) {\n            (ENGINE_OK, COMMITTED) =>",
        "Stage::Commit => match (status, disposition) {\n            (ENGINE_OK, _) =>",
        ("status", "map_engine"),
    ),
    (
        "a read-only finish that did not commit is answered OK",
        "src/status.rs",
        "(ENGINE_OK, _) => Mapped::Shard(SHARD_IO),",
        "(ENGINE_OK, _) => Mapped::Shard(SHARD_OK),",
        ("status", "map_engine"),
    ),
    (
        "a step that failed and was closed is answered OK",
        "src/status.rs",
        "Stage::Closed => map_failure(status),",
        "Stage::Closed => if status == ENGINE_OK { Mapped::Shard(SHARD_OK) } "
        "else { map_failure(status) },",
        ("status", "map_engine"),
    ),
    (
        "COMMITTED_ERROR is answered IO instead of stopping the process",
        "src/status.rs",
        "ENGINE_UNKNOWN | ENGINE_COMMITTED_ERROR => Mapped::FailStop,",
        "ENGINE_UNKNOWN => Mapped::FailStop,",
        ("status", "map_failure"),
    ),
    (
        "an engine failure is answered NOT_FOUND",
        "src/status.rs",
        "_ => Mapped::Shard(SHARD_IO),\n    }\n}",
        "_ => Mapped::Shard(SHARD_NOT_FOUND),\n    }\n}",
        ("status", "map_failure"),
    ),
    (
        "the byte order puts a larger byte first",
        "src/order.rs",
        "mismatch(a@, b@, i as int);\n            }\n            return -1;",
        "mismatch(a@, b@, i as int);\n            }\n            return 1;",
        ("order", "compare"),
    ),
    (
        "a key is ordered with its proper prefix",
        "src/order.rs",
        "shorter(a@, b@);\n        }\n        -1",
        "shorter(a@, b@);\n        }\n        0",
        ("order", "compare"),
    ),
    (
        "a flag above 1 is accepted",
        "src/scan.rs",
        "valid == flags_valid(has_hi, has_after),\n{\n    has_hi <= 1 && has_after <= 1",
        "valid == flags_valid(has_hi, has_after),\n{\n    has_hi <= 1 && has_after <= 2",
        ("scan", "scan_flags_valid"),
    ),
    (
        "the empty range [k, k) is accepted",
        "src/scan.rs",
        "if has_hi == 1 && compare(lo, hi) >= 0 {",
        "if has_hi == 1 && compare(lo, hi) > 0 {",
        ("scan", "scan_arguments"),
    ),
    (
        "a cursor whose coordinate sorts before its key is accepted",
        "src/scan.rs",
        "if has_after == 1 && compare(after_coordinate, after_key) != 0 {",
        "if has_after == 1 && compare(after_coordinate, after_key) > 0 {",
        ("scan", "scan_arguments"),
    ),
    (
        "a cursor equal to the lower bound is delivered again",
        "src/scan.rs",
        "let exclusive = has_after == 1 && compare(after_key, lo) >= 0;",
        "let exclusive = has_after == 1 && compare(after_key, lo) > 0;",
        ("scan", "scan_arguments"),
    ),
    (
        "a cursor exactly at the upper bound is not the end of the range",
        "src/scan.rs",
        "if has_hi == 1 && compare(start, hi) >= 0 {",
        "if has_hi == 1 && compare(start, hi) > 0 {",
        ("scan", "scan_arguments"),
    ),
    (
        "the scan starts at the cursor instead of after it",
        "src/scan.rs",
        "ScanLower::Excluded(after_key)\n    } else {",
        "ScanLower::Included(after_key)\n    } else {",
        ("scan", "scan_arguments"),
    ),
    (
        "the scan loses its upper bound",
        "src/scan.rs",
        "let upper = if has_hi == 1 {\n        Some(hi)",
        "let upper = if has_hi == 1 {\n        None",
        ("scan", "scan_arguments"),
    ),
]

PROOF_FAILURES = ("assertion failed", "postcondition not satisfied",
                  "precondition not satisfied", "invariant not satisfied")
INVALID_RUNS = ("error[E", "Resource limit", "resource limit", "internal compiler error")


def verify():
    try:
        baseline = subprocess.run(VERUS_ARGS, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, timeout=900)
    except subprocess.TimeoutExpired:
        raise SystemExit("Verification of the unchanged crate timed out")
    summary = re.search(r"verification results:: (\d+) verified, (\d+) errors", baseline.stdout)
    if (baseline.returncode != 0 or summary is None or int(summary[1]) == 0
            or int(summary[2]) != 0):
        raise SystemExit(f"Verification of the unchanged crate failed\n{baseline.stdout}")
    print(baseline.stdout, end="", flush=True)


def controls():
    # A rejection is evidence only because the unchanged crate has just passed.
    for name, filename, before, after, (module, function) in CONTROLS:
        with tempfile.TemporaryDirectory(prefix="shard-host-core-control-") as directory:
            dest = Path(directory)
            shutil.copytree(CRATE, dest / "crates" / "shard-host-core")
            path = dest / "crates" / "shard-host-core" / filename
            text = path.read_text()
            if text.count(before) != 1:
                raise SystemExit(f"{name}: mutation site is not unique; no control was run")
            path.write_text(text.replace(before, after))
            try:
                result = subprocess.run(
                    [*VERUS_ARGS, "--verify-only-module", module, "--verify-function", function],
                    cwd=dest, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    timeout=900,
                )
            except subprocess.TimeoutExpired:
                raise SystemExit(f"{name}: timeout is not a semantic proof rejection")
            output = result.stdout
            summary = re.search(r"verification results:: (\d+) verified, (\d+) errors", output)
            if (result.returncode != 1 or summary is None or int(summary[2]) == 0
                    or not any(message in output for message in PROOF_FAILURES)
                    or any(message in output for message in INVALID_RUNS)):
                raise SystemExit(f"{name}: expected a semantic proof rejection\n{output}")
            obligation = next(message for message in PROOF_FAILURES if message in output)
            print(f"PASS: Verus rejects \"{name}\" in {module}::{function} "
                  f"({obligation}; {summary[2]} failed)", flush=True)
    print(f"{len(CONTROLS)} negative controls rejected")


if __name__ == "__main__":
    audit_sources()
    audit_controls()
    # Compile the checked Cargo target only after rejecting alternate entry
    # points and build scripts. This also fetches the archives for check_pins.
    subprocess.run(["cargo", "build", "--locked", "--manifest-path", "crates/Cargo.toml",
                    "--target-dir", str(Path(os.environ.get("BUILD_DIR", "build_docker"))
                                        / "shard-host-core-proof"),
                    "-p", "shard-host-core", "-p", "sto-tpcc-ffi"], cwd=ROOT, check=True)
    check_pins()
    verify()
    controls()
    sys.exit(0)
