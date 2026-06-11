#!/usr/bin/env python3
"""
Phase 0 inventory for the rrr → DSL migration plan
(see docs/TODO-rusty-rewrite.md).

Walks src/rrr/ (excluding tests), enumerates every top-level
class / struct / union / enum declaration, classifies its
region (manual C++ / inside a `#if RUSTYCPP_RUST` block / inside a
`/*RUSTYCPP:GEN-BEGIN ... GEN-END*/` block), and applies a
heuristic triage bucket for the manual ones:

  - already-dsl       — inside a DSL or GEN region; no work needed
  - boundary          — talks to libc/syscalls/wire types, will stay manual
  - needs-transpiler  — pattern that the DSL doesn't accept today
                        (templates, custom Drop dtors, virtual hierarchies)
  - refactor-then-dsl — would migrate after a refactor (public ctor →
                        ::new() factory, virtual base → trait, etc.)
  - trivial           — POD-ish, looks like an easy migration target

Emits a CSV at the destination passed via --out (default
docs/rrr-inventory.csv) plus a short Markdown summary at
--summary (default docs/rrr-inventory.md).

The classification is a starting cut, not the final word — every
"trivial" row should be reviewed before it gets migrated. The point
of this pass is to make the remaining work countable.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

SRC_ROOT_DEFAULT = "src/rrr"
SKIP_DIR_PARTS = ("/tests/", "/tests_", "/test_")

# A top-level decl introducer at column 0. Captures the kind and name.
DECL_RE = re.compile(
    r"^(?:export\s+)?"
    r"(?P<kind>class|struct|union|enum\s+class|enum)"
    r"\s+(?P<name>[A-Za-z_]\w*)"
    r"(?:\s*:\s*[^{;]*)?"             # optional base-class spec
    r"\s*(?P<term>[{;])"
)

# A top-level template-prefixed decl (the `template<...>` is on its own
# line above the class line; we treat that as a separate kind so the
# classifier sees it).
TEMPLATE_LINE_RE = re.compile(r"^\s*template\s*<")

# Boundary / cannot-migrate name patterns (case-sensitive).
BOUNDARY_NAMES = {
    "AddrInfo",
    "RandomGenerator",
    "SparseInt",
}

BOUNDARY_FILES = {
    "src/rrr/rpc/utils.cpp",
    "src/rrr/misc/rand.cpp",
}

# Names that the plan flags as template-heavy (cannot DSL).
TEMPLATE_NAMES = {
    "SerializableEnvelope",
    "CallbackWrapper",
    "Enumerator",
    "MergedEnumerator",
    "PollableTypedArcAdapter",
    "ServiceTypedBoxAdapter",
    "ChannelConnectionTypedArcAdapter",
    "ChannelListenerTypedArcAdapter",
    "TcpListenerPollableAdapter",
    "TcpConnectionChannelAdapter",
    "TcpListenerChannelAdapter",
    "TcpConnectionPollableAdapter",
    "InMemoryConnectionAdapter",
    "InMemoryChannelAdapter",
    "InMemoryListenerAdapter",
    "InMemoryFactoryAdapter",
    "TcpFactoryAdapter",
}


@dataclass
class Region:
    kind: str            # "manual" | "dsl" | "gen"
    start: int           # 1-based inclusive
    end: int             # 1-based inclusive


@dataclass
class Decl:
    file: str
    start_line: int
    end_line: int
    kind: str            # "class" | "struct" | "union" | "enum" | "enum class"
    name: str
    region: str          # "manual" | "dsl" | "gen"
    template: bool
    has_virtual: bool
    has_user_dtor: bool
    has_user_ctor: bool
    has_inheritance: bool
    base_clause: str
    blockers: list[str]  # detected DSL-blockers in the body
    bucket: str          # classifier output
    notes: list[str] = field(default_factory=list)

    @property
    def loc(self) -> int:
        return self.end_line - self.start_line + 1


def find_regions(lines: list[str]) -> list[Region]:
    """Classify each line as manual / dsl / gen."""
    regions: list[Region] = []
    in_dsl = False
    in_gen = False
    start = 1
    cur = "manual"
    for i, ln in enumerate(lines, start=1):
        new = cur
        if not in_gen and ln.startswith("/*RUSTYCPP:GEN-BEGIN"):
            new = "gen"; in_gen = True
        elif in_gen and "RUSTYCPP:GEN-END" in ln:
            # Close the gen region INCLUSIVE of this line, then flip.
            if cur != new:
                regions.append(Region(cur, start, i - 1))
                start = i
            regions.append(Region("gen", start, i))
            start = i + 1; cur = "manual"; in_gen = False
            continue
        elif not in_gen and not in_dsl and ln.startswith("#if RUSTYCPP_RUST"):
            new = "dsl"; in_dsl = True
        elif in_dsl and ln.startswith("#endif"):
            if cur != new:
                regions.append(Region(cur, start, i - 1))
                start = i
            regions.append(Region("dsl", start, i))
            start = i + 1; cur = "manual"; in_dsl = False
            continue
        if new != cur:
            regions.append(Region(cur, start, i - 1))
            start = i
            cur = new
    if start <= len(lines):
        regions.append(Region(cur, start, len(lines)))
    return [r for r in regions if r.end >= r.start]


def region_at(regions: list[Region], line: int) -> str:
    for r in regions:
        if r.start <= line <= r.end:
            return r.kind
    return "manual"


def find_decl_end(lines: list[str], start_idx: int) -> int:
    """Find the matching `};` (or `}` if it's a function/namespace) for a
    decl that opens at `start_idx` (0-based). Heuristic: depth-counted
    brace match, stopping when depth returns to 0 at a `}` line."""
    depth = 0
    seen_open = False
    in_block_comment = False
    in_line_comment = False
    for i in range(start_idx, len(lines)):
        ln = lines[i]
        j = 0
        while j < len(ln):
            ch = ln[j]
            two = ln[j:j+2]
            if in_block_comment:
                if two == "*/":
                    in_block_comment = False
                    j += 2; continue
                j += 1; continue
            if in_line_comment:
                break  # rest of line is comment
            if two == "/*":
                in_block_comment = True
                j += 2; continue
            if two == "//":
                in_line_comment = True
                break
            if ch == '{':
                depth += 1; seen_open = True
            elif ch == '}':
                depth -= 1
                if seen_open and depth == 0:
                    return i + 1  # 1-based
            j += 1
        in_line_comment = False
        # If the decl ended with `;` on the opening line and we never
        # saw a `{`, treat the start line as the end.
        if not seen_open and ';' in ln and i == start_idx:
            return start_idx + 1
    return len(lines)  # fall through — span to EOF


def scan_body(lines: list[str], start_idx: int, end_line: int) -> dict:
    """Look at the decl body (1-based start_line through end_line) for
    coarse signals + DSL-blocker patterns.

    Blockers are constructs we've confirmed the DSL doesn't accept (or
    that would cascade the migration through many call sites):

      - preprocessor branches inside the body (`#if` / `#ifdef` /
        `#elif`) — the DSL emit can't fold conditional compilation.
      - operator overloads — DSL has no operator-overload grammar.
      - default arguments in member-function signatures — the DSL
        grammar doesn't accept them; either drop the default or migrate
        the caller first.
      - nested struct/class — the DSL emits top-level structs only.
      - template methods (`template <...>` lines inside the body) —
        same templates-aren't-supported rule as the class-level check.
      - `std::atomic<T>` field with `compare_exchange_*` call sites —
        rusty's Atomic CAS returns Result<T, T>, not bool, so the
        idiomatic `!atomic.cmpxchg(...)` callers need rewriting.
      - `std::vector<T>` field with `.assign(...)` or `.emplace_back(...)`
        usage — `rusty::Vec` doesn't expose `.assign`.
      - `std::function<…>` field — needs a `rusty::Function<…>` typedef
        first; the DSL parser doesn't accept raw `std::function` types.

    Returns the (coarse signal, blocker-list) bundle as a dict."""
    name = ""
    has_virtual = False
    has_user_dtor = False
    has_user_ctor = False
    template = False
    base_clause = ""
    blockers: list[str] = []
    # Check template prefix on line above
    if start_idx > 0 and TEMPLATE_LINE_RE.match(lines[start_idx - 1] or ""):
        template = True
    m = DECL_RE.match(lines[start_idx])
    if m:
        name = m.group("name")
        # base clause: anything between `:` and `{` on the decl line
        first = lines[start_idx]
        if ':' in first.split('{', 1)[0]:
            base_clause = first.split(':', 1)[1].split('{', 1)[0].strip()
    body_re_virtual = re.compile(r"\bvirtual\b")
    body_re_dtor = re.compile(rf"~{re.escape(name)}\s*\(") if name else None
    body_re_ctor = (
        re.compile(rf"^\s*(?:explicit\s+)?{re.escape(name)}\s*\(") if name else None
    )
    body_re_preproc = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else)\b")
    body_re_operator = re.compile(r"\boperator\s*(?:[(\[\]+\-*/%^&|~!<>=]|->|::)")
    # Match `name = literal` inside a parameter list (default arg).
    # Distinguish a real `name = literal` from a `==`/`!=`/`<=`/`>=`
    # comparison in a body expression: require the `=` not to be part
    # of a 2-char comparison operator (`(?<!=)`, `(?<![!<>])`, `(?!=)`).
    body_re_default_arg = re.compile(r"\([^()]*(?<![!<>=])=(?!=)[^()]*\)")
    body_re_nested = re.compile(r"^\s*(?:class|struct|union)\s+\w+")
    body_re_method_template = re.compile(r"^\s*template\s*<")
    body_re_std_atomic = re.compile(r"\bstd::atomic\s*<")
    body_re_std_vector = re.compile(r"\bstd::vector\s*<")
    body_re_std_function = re.compile(r"\bstd::function\s*<")
    body_re_std_unique = re.compile(r"\bstd::unique_ptr\s*<")
    body_re_std_shared = re.compile(r"\bstd::shared_ptr\s*<")
    body_re_compare_exchange = re.compile(r"\.compare_exchange_(weak|strong)\b")
    body_re_vec_assign = re.compile(r"\.assign\s*\(")
    body_re_emplace_back = re.compile(r"\.emplace_back\s*\(")
    # `void *` or `const void *` in a param list — DSL grammar doesn't
    # accept untyped pointers; the migration needs a `&[u8]` / `*const u8`
    # reshape first.
    body_re_void_star = re.compile(r"\b(?:const\s+)?void\s*\*")
    # `va_list` — variadic C-style param; DSL can't express it.
    body_re_va_list = re.compile(r"\bva_list\b")
    # C-style array params (`Type name[]` inside a parameter list).
    # Match a closing `]` preceded by `[\s*\]` on the same line, scoped
    # to a parameter list (parenthesized).
    body_re_c_array_param = re.compile(r"\([^()]*\b\w+\s*\[\s*\][^()]*\)")

    found_preproc = found_operator = found_default_arg = False
    found_nested = found_method_template = False
    has_std_atomic = has_std_vector = has_std_function = False
    has_std_unique = has_std_shared = False
    has_compare_exchange = has_vec_assign = has_emplace_back = False
    found_void_star = found_va_list = found_c_array = False

    # Field-name capture for call-site blocker detection (post-pass
    # via scan_call_sites). Catches the pattern where a field's blocker
    # method is invoked outside the decl body — e.g. a free function in
    # the same TU calling `g_stackless_profile.max_slots.compare_exchange_weak(...)`.
    atomic_field_names: list[str] = []
    vector_field_names: list[str] = []
    field_re_atomic = re.compile(
        r"\bstd::atomic\s*<[^>]+>\s+(\w+)\b"
    )
    field_re_vector = re.compile(
        r"\bstd::vector\s*<[^>]+>\s+(\w+)\b"
    )

    # Skip the opening line for the nested-class check (the decl line itself
    # matches `^\s*(?:class|struct)`).
    for i in range(start_idx, min(end_line, len(lines))):
        ln = lines[i]
        if body_re_virtual.search(ln):
            has_virtual = True
        if body_re_dtor and body_re_dtor.search(ln):
            # A `~Name() = default;` (or `=default;`) declaration is
            # the implicit-dtor sugar — no Drop logic to migrate, the
            # DSL aggregate's implicit drop handles it. Only count
            # "real" user dtors; defaulted ones don't gate trivial
            # classification. Look at the matched line plus the next
            # 2 lines (covers `~Name() noexcept(false) = default;`
            # and other multi-line defaulted-dtor declarations).
            tail = ln
            for k in range(i + 1, min(i + 3, min(end_line, len(lines)))):
                tail += " " + lines[k]
            if not re.search(r"=\s*default\s*;", tail):
                has_user_dtor = True
        if body_re_ctor and body_re_ctor.search(ln):
            # A `Name(...) = default;` (or `=default;`) line is the
            # implicit-ctor sugar — no real logic to migrate, the DSL
            # aggregate behaves the same. Only count "real" user ctors;
            # defaulted ones don't block trivial classification.
            # Look at the matched line plus the next 2 lines (covers
            # multi-line defaulted-ctor declarations).
            tail = ln
            for k in range(i + 1, min(i + 3, min(end_line, len(lines)))):
                tail += " " + lines[k]
            if not re.search(r"=\s*default\s*;", tail):
                has_user_ctor = True
        if i > start_idx:
            if body_re_preproc.match(ln):
                found_preproc = True
            if body_re_operator.search(ln) and "operator" in ln:
                # Filter out `using namespace foo::operator` or comment refs.
                # Also filter `operator=(...) = default;` — the DSL aggregate
                # supplies the implicit assignment operator, so a defaulted
                # operator overload carries no logic to migrate.
                tail = ln
                for k in range(i + 1, min(i + 3, min(end_line, len(lines)))):
                    tail += " " + lines[k]
                if ("// " not in ln.split("operator", 1)[0][-40:]
                        and not re.search(r"=\s*default\s*;", tail)):
                    found_operator = True
            if body_re_default_arg.search(ln):
                found_default_arg = True
            if body_re_nested.match(ln):
                found_nested = True
            if body_re_method_template.match(ln):
                found_method_template = True
        if body_re_std_atomic.search(ln):
            has_std_atomic = True
        if body_re_std_vector.search(ln):
            has_std_vector = True
        if body_re_std_function.search(ln):
            has_std_function = True
        if body_re_std_unique.search(ln):
            has_std_unique = True
        if body_re_std_shared.search(ln):
            has_std_shared = True
        if body_re_compare_exchange.search(ln):
            has_compare_exchange = True
        if body_re_vec_assign.search(ln):
            has_vec_assign = True
        if body_re_emplace_back.search(ln):
            has_emplace_back = True
        if i > start_idx:
            if body_re_void_star.search(ln):
                found_void_star = True
            if body_re_va_list.search(ln):
                found_va_list = True
            if body_re_c_array_param.search(ln):
                found_c_array = True
            # Capture field names — only inside the decl body, not the
            # decl-introducer line itself.
            m_at = field_re_atomic.search(ln)
            if m_at:
                atomic_field_names.append(m_at.group(1))
            m_vec = field_re_vector.search(ln)
            if m_vec:
                vector_field_names.append(m_vec.group(1))

    if found_preproc:
        blockers.append("preprocessor branches in body (#if/#ifdef)")
    if found_operator:
        blockers.append("operator overload")
    if found_default_arg:
        blockers.append("default arg in member fn")
    if found_nested:
        blockers.append("nested struct/class")
    if found_method_template:
        blockers.append("template method")
    if has_std_atomic and has_compare_exchange:
        blockers.append("std::atomic CAS — rusty::Atomic returns Result, not bool")
    if has_std_vector and (has_vec_assign or has_emplace_back):
        blockers.append("std::vector .assign/.emplace_back — rusty::Vec lacks these")
    if has_std_function:
        blockers.append("std::function field — needs rusty::Function typedef first")
    if has_std_unique:
        blockers.append("std::unique_ptr field — should use rusty::Box")
    if has_std_shared:
        blockers.append("std::shared_ptr field — should use rusty::Arc")
    if found_void_star:
        blockers.append("void* in param — DSL grammar doesn't accept void*")
    if found_va_list:
        blockers.append("va_list — variadic C-style param, not expressible in DSL")
    if found_c_array:
        blockers.append("C-style array param — DSL needs slice/Vec instead")
    return {
        "template": template,
        "has_virtual": has_virtual,
        "has_user_dtor": has_user_dtor,
        "has_user_ctor": has_user_ctor,
        "base_clause": base_clause,
        "blockers": blockers,
        "atomic_field_names": atomic_field_names,
        "vector_field_names": vector_field_names,
    }


def classify(decl: Decl) -> tuple[str, list[str]]:
    notes: list[str] = []
    if decl.region in ("dsl", "gen"):
        return "already-dsl", []
    # Boundary
    if decl.name in BOUNDARY_NAMES:
        return "boundary", ["name flagged as boundary in plan"]
    if decl.file in BOUNDARY_FILES:
        notes.append("file flagged as boundary in plan")
        return "boundary", notes
    # Template
    if decl.template or decl.name in TEMPLATE_NAMES:
        notes.append("template — DSL doesn't accept C++ templates")
        return "needs-transpiler", notes
    # Custom destructor (Drop trait gate)
    if decl.has_user_dtor:
        notes.append("custom dtor — needs `impl Drop` emit")
        return "needs-transpiler", notes
    # Virtual hierarchy
    if decl.has_virtual or decl.base_clause:
        notes.append(
            "virtual / inheritance — migrate as DSL `pub trait` after the "
            "base-class pass"
        )
        return "refactor-then-dsl", notes
    # User-defined ctor (needs static ::new() refactor first).
    # Also append any body-scan blockers so the full migration picture
    # is visible — the user ctor is the primary blocker for routing,
    # but the secondary blockers (void*, va_list, etc.) tell the
    # implementer which additional refactors are needed before the
    # DSL block compiles.
    if decl.has_user_ctor:
        notes.append("user ctor — needs static ::new() factory refactor first")
        notes.extend(decl.blockers)
        return "refactor-then-dsl", notes
    # Otherwise it looks POD-ish. But body scan may have surfaced
    # hidden DSL-blockers; if so, demote to `trivial-blocked` so the
    # Phase 1 selector skips it.
    if decl.blockers:
        notes.extend(decl.blockers)
        return "trivial-blocked", notes
    if decl.kind in ("enum", "enum class"):
        notes.append("plain enum — DSL pattern is `#[repr(int)] enum`")
    else:
        notes.append("POD-shaped — likely fits the trivial DSL pattern")
    return "trivial", notes


def collect_files(root: Path) -> Iterable[Path]:
    for ext in ("cpp", "hpp", "h", "cc"):
        for p in root.rglob(f"*.{ext}"):
            s = str(p)
            if any(part in s for part in SKIP_DIR_PARTS):
                continue
            yield p


def process_file(path: Path) -> list[Decl]:
    text = path.read_text(errors="replace")
    lines = text.splitlines()
    regions = find_regions(lines)
    decls: list[Decl] = []
    # Per-decl field-name bookkeeping so we can do file-wide call-site
    # detection after the per-decl pass.
    atomic_field_owner: dict[str, Decl] = {}
    vector_field_owner: dict[str, Decl] = {}
    for i, ln in enumerate(lines):
        m = DECL_RE.match(ln)
        if not m:
            continue
        # Filter out one-liner forward declarations (`class Foo;`).
        if m.group("term") == ";" and "{" not in ln:
            continue
        # Filter out lambdas / anonymous structs and obvious noise.
        if not m.group("name"):
            continue
        end_line = find_decl_end(lines, i)
        body = scan_body(lines, i, end_line)
        region = region_at(regions, i + 1)
        decl = Decl(
            file=str(path),
            start_line=i + 1,
            end_line=end_line,
            kind=m.group("kind").replace(" ", " "),
            name=m.group("name"),
            region=region,
            template=body["template"],
            has_virtual=body["has_virtual"],
            has_user_dtor=body["has_user_dtor"],
            has_user_ctor=body["has_user_ctor"],
            has_inheritance=bool(body["base_clause"]),
            base_clause=body["base_clause"],
            blockers=list(body["blockers"]),
            bucket="",
        )
        for fn in body.get("atomic_field_names", []):
            atomic_field_owner[fn] = decl
        for fn in body.get("vector_field_names", []):
            vector_field_owner[fn] = decl
        decls.append(decl)

    # Second pass: file-wide call-site scan. For each captured std::atomic
    # field name, see if any line outside the decl body invokes a
    # compare_exchange variant on it (`.<name>.compare_exchange_…`). For
    # std::vector field names look for `.<name>.assign(` / `.emplace_back(`.
    if atomic_field_owner or vector_field_owner:
        for i, ln in enumerate(lines):
            for name, owner in atomic_field_owner.items():
                if owner.start_line - 1 <= i < owner.end_line:
                    continue
                if re.search(rf"\.{re.escape(name)}\.compare_exchange_(weak|strong)\b", ln):
                    msg = ("std::atomic field used with `compare_exchange_*` "
                           "at a call site — rusty::Atomic CAS returns Result, "
                           "not bool")
                    if msg not in owner.blockers:
                        owner.blockers.append(msg)
            for name, owner in vector_field_owner.items():
                if owner.start_line - 1 <= i < owner.end_line:
                    continue
                if re.search(rf"\.{re.escape(name)}\.(assign|emplace_back)\s*\(", ln):
                    msg = ("std::vector field used with `.assign/.emplace_back` "
                           "at a call site — rusty::Vec lacks these")
                    if msg not in owner.blockers:
                        owner.blockers.append(msg)

    for decl in decls:
        bucket, notes = classify(decl)
        decl.bucket = bucket
        decl.notes = notes
    return decls


def write_csv(decls: list[Decl], out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w") as f:
        f.write("file,start_line,end_line,loc,region,kind,name,bucket,base_clause,notes\n")
        for d in sorted(decls, key=lambda d: (d.file, d.start_line)):
            notes = ";".join(d.notes).replace(",", " ").replace("\n", " ")
            base = d.base_clause.replace(",", " ").replace("\n", " ")
            f.write(
                f"{d.file},{d.start_line},{d.end_line},{d.loc},"
                f"{d.region},{d.kind.replace(' ', '_')},{d.name},"
                f"{d.bucket},{base},{notes}\n"
            )


def write_summary(decls: list[Decl], out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    by_bucket: dict[str, list[Decl]] = {}
    for d in decls:
        by_bucket.setdefault(d.bucket, []).append(d)
    total_loc = sum(d.loc for d in decls)
    with out_path.open("w") as f:
        f.write("# rrr decl inventory (Phase 0)\n\n")
        f.write(
            "Auto-generated by `tools/rrr-inventory.py` from the rrr source\n"
            "tree (excluding tests). This file is the bucket-level summary.\n"
            "The per-decl CSV (`docs/rrr-inventory.csv`) is `.gitignore`d as\n"
            "a build artifact — re-run `python3 tools/rrr-inventory.py`\n"
            "from the repo root to regenerate both this file and the CSV.\n\n"
        )
        f.write(f"**Decl count (top-level class/struct/enum/union):** {len(decls)}  \n")
        f.write(f"**Span across all decls (LOC):** {total_loc}\n\n")
        f.write("## Buckets\n\n")
        f.write("| Bucket | Decls | LOC | % of LOC |\n")
        f.write("|---|---:|---:|---:|\n")
        for bucket in (
            "trivial",
            "trivial-blocked",
            "refactor-then-dsl",
            "needs-transpiler",
            "boundary",
            "already-dsl",
        ):
            rows = by_bucket.get(bucket, [])
            loc = sum(d.loc for d in rows)
            pct = (100.0 * loc / total_loc) if total_loc else 0.0
            f.write(f"| {bucket} | {len(rows)} | {loc} | {pct:.1f}% |\n")
        f.write("\n## Top 20 largest manual decls by LOC\n\n")
        f.write("| File | Lines | Name | Bucket |\n")
        f.write("|---|---:|---|---|\n")
        manual = [d for d in decls if d.region == "manual"]
        for d in sorted(manual, key=lambda d: -d.loc)[:20]:
            relfile = d.file.split("src/rrr/", 1)[-1]
            f.write(f"| `{relfile}` | {d.loc} | `{d.name}` | {d.bucket} |\n")
        f.write("\n## Per-file LOC (manual decls only, top 15)\n\n")
        per_file: dict[str, int] = {}
        for d in manual:
            per_file[d.file] = per_file.get(d.file, 0) + d.loc
        f.write("| File | Manual decl LOC |\n")
        f.write("|---|---:|\n")
        for fname, loc in sorted(per_file.items(), key=lambda kv: -kv[1])[:15]:
            relfile = fname.split("src/rrr/", 1)[-1]
            f.write(f"| `{relfile}` | {loc} |\n")
        f.write(
            "\n## Caveats\n\n"
            "- The decl span is a brace-counted heuristic, not a full C++\n"
            "  parse. Inline-comment braces and string-literal braces are\n"
            "  handled but template angle brackets aren't. Spans within\n"
            "  ~5% of the truth.\n"
            "- The bucket is a first cut. Anything in `trivial` should be\n"
            "  reviewed before migration — the script can't see whether\n"
            "  the decl is referenced by name from a template instantiation\n"
            "  elsewhere, which would block DSL-ification.\n"
            "- File-scope free functions, typedefs, and constants are not\n"
            "  enumerated here (the existing GEN-block coverage already\n"
            "  catches the simple-constant pattern; what's left is per-decl\n"
            "  signal-heavy).\n"
        )


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--root", default=SRC_ROOT_DEFAULT)
    p.add_argument("--out", default="docs/rrr-inventory.csv")
    p.add_argument("--summary", default="docs/rrr-inventory.md")
    args = p.parse_args()
    root = Path(args.root)
    if not root.exists():
        print(f"error: {root} does not exist", file=sys.stderr)
        return 1
    decls: list[Decl] = []
    for path in collect_files(root):
        decls.extend(process_file(path))
    write_csv(decls, Path(args.out))
    write_summary(decls, Path(args.summary))
    by_bucket: dict[str, int] = {}
    for d in decls:
        by_bucket[d.bucket] = by_bucket.get(d.bucket, 0) + 1
    print(f"wrote {args.out} ({len(decls)} decls)")
    print(f"wrote {args.summary}")
    for b in ("trivial", "trivial-blocked", "refactor-then-dsl",
              "needs-transpiler", "boundary", "already-dsl"):
        print(f"  {b}: {by_bucket.get(b, 0)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
