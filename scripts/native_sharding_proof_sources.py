#!/usr/bin/env python3
"""Coverage/escape audit, NOT a proof; Verus runs on the actual crate afterward."""
from pathlib import Path
import re
import tomllib

ROOT = Path(__file__).resolve().parents[1]
CLUSTER = ROOT / "src/cluster"
PIN = "b677dd5a766f25f56e9aa1e32621aa4e53304b47"
manifest = tomllib.loads((CLUSTER / "Cargo.toml").read_text())
lock = tomllib.loads((CLUSTER / "Cargo.lock").read_text())
if manifest["dependencies"]["vstd"]["rev"] != PIN:
    raise SystemExit("Native vstd manifest pin drift")
for package in lock["package"]:
    source = package.get("source", "")
    if "verus-lang/verus" in source and not source.endswith("#" + PIN):
        raise SystemExit("Native Verus lockfile pin drift")

# Follow Rust's file-module declarations, including #[path] modules. Inline
# modules do not introduce files. This is deliberately only a coverage audit:
# no count or source-text result is presented as a verified safety property.
seen = set()
def visit(path):
    path = path.resolve()
    if path in seen:
        return
    seen.add(path)
    text = path.read_text()
    # Strip comments so examples and method names such as Gateway::admit do
    # not masquerade as proof escapes. Strings remain for #[path] parsing.
    code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    if re.search(r"(?<!fn )(?<![.\w])\b(?:assume|admit)\s*\(", code):
        raise SystemExit(f"Unproved assumption in {path.relative_to(ROOT)}")
    if re.search(r"\b(?:external_body|rlimit)\b", code):
        raise SystemExit(f"Verifier escape in {path.relative_to(ROOT)}")
    pattern = r'((?:#\[[^\]]*\]\s*)*)(?:pub(?:\([^)]*\))?\s+)?mod\s+(\w+)\s*;'
    for match in re.finditer(pattern, code):
        attribute = re.search(r'path\s*=\s*"([^"]+)"', match[1])
        child = path.parent / (attribute[1] if attribute else match[2] + ".rs")
        if not child.exists() and not attribute:
            child = path.parent / match[2] / "mod.rs"
        visit(child)

visit(CLUSTER / manifest["lib"]["path"])
orphans = sorted(path.name for path in CLUSTER.glob("*.rs")
                 if path.resolve() not in seen and not path.name.endswith("_verify.rs"))
if orphans:
    raise SystemExit("Production crate omits native modules: " + ", ".join(orphans))
print(f"Native source coverage audit: {len(seen)} reachable files; Verus proof follows")
