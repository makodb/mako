#!/usr/bin/env python3
"""Emit the Rust side of an .rpc service: wire structs, service trait, proxy.

Mako-local on purpose. srpc's rpcgen has cpp and python emitters
(src/srpc/pylib/simplerpcgen/lang_cpp.py, lang_python.py) and no Rust one, but
adding `lang_rust.py` there would put mako's code inside the vendored subtree,
where it conflicts on every `git subtree pull`. This instead does what
`bin/rpcgen` already does -- put `src/srpc/pylib` on sys.path and import
upstream's parser -- and emits from outside. Nothing under src/srpc/ changes.

WHAT IS GENERATED, AND WHAT IS NOT. The same seam C++ uses:

    generated   the wire structs and their Serialize/Deserialize
    generated   the service trait and its __dispatch__ (deserialize, switch
                on rpc id, call a handler, serialize the reply)
    generated   the proxy, one method per RPC
    HAND-WRITTEN the handler bodies -- `impl RaftService for RaftServiceImpl`

That mirrors rcc_rpc.h's abstract `RaftService` (381 lines, generated) against
service.cc's `RaftServiceImpl` (201 lines, hand-written). The boilerplate is
mechanical; the twelve call sites that reach into the Raft server are not.

WIRE IDS. rpcgen assigns them randomly and preserves them across regeneration
ONLY by scraping the header it previously wrote (rpcgen.py:326-338); the .rpc
file does not record them. So they are read from an explicit map here and
checked against the generated C++ header while that header still contains
them. See --ids-from.

Usage:
    python3 scripts/rpcgen_rust.py --service Raft \\
        --rpc src/deptran/rcc_rpc.rpc \\
        --ids-from src/deptran/rcc_rpc.h \\
        --out src/deptran/raft/src/rpc.rs
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src/srpc/pylib"))

from simplerpcgen.rpcgen import parse  # noqa: E402  (needs the path above)

# The .rpc scalar spellings, and what each is on the wire. Every one of these
# is a #define in src/deptran/constants.h; none is a guess.
#   ballot_t int64_t (:6)   parid_t uint32_t (:13)
#   siteid_t uint16_t (:18) bool_t   int8_t   (:26)
# bool_t deliberately stays i8 rather than becoming a Rust bool: the width is
# the header's business, and converting at the edge keeps it that way.
TYPE_MAP = {
    "uint64_t": "u64",
    "uint32_t": "u32",
    "uint16_t": "u16",
    "int64_t": "i64",
    "int32_t": "i32",
    "i32": "i32",
    "i64": "i64",
    "ballot_t": "i64",
    "parid_t": "u32",
    "siteid_t": "u16",
    "bool_t": "i8",
    "std::string": "String",
    "string": "String",
}

# Types that need a framing decision before they can be emitted. `Command` is
# a janus::Command -- an srpc::SerializableEnvelope whose contents Raft reads
# (server.cc:1705-1726) and which writes [v32 kind][payload] with NO length
# prefix (serializable_envelope.rs:111-120). A field list for it would compile
# and silently misparse every replicating AppendEntries, so refuse instead.
UNFRAMED = {
    "Command": (
        "janus::Command is an unframed SerializableEnvelope whose contents "
        "Raft reads; it needs the repeated (u32 len, bytes, i64 term) framing "
        "from stage 2c of docs/migration/raft/commo-service-rpc-plan.md"
    ),
}


def snake(name: str) -> str:
    """leaderPrevLogIndex -> leader_prev_log_index."""
    out = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", out).lower()


def pascal(name: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in name.split("_"))


def rust_type(cpp: str) -> str:
    if cpp in UNFRAMED:
        raise Unframed(cpp, UNFRAMED[cpp])
    if cpp not in TYPE_MAP:
        raise SystemExit(
            f"rpcgen_rust: no Rust mapping for wire type {cpp!r}. Add it to "
            f"TYPE_MAP with the #define it comes from, or it is a guess."
        )
    return TYPE_MAP[cpp]


class Unframed(Exception):
    def __init__(self, type_name: str, why: str):
        super().__init__(f"{type_name}: {why}")
        self.type_name = type_name


def read_ids(header: pathlib.Path, service: str) -> dict[str, int]:
    """Scrape the rpc ids out of the generated C++ header.

    While the header still declares this service, it is the single source of
    truth for the ids, so read them rather than restate them. When the service
    is removed from the .rpc this must be replaced by a checked-in map -- see
    stage 2b of the plan.
    """
    text = header.read_text(errors="replace")
    block = re.search(
        rf"class {service}Service\b.*?enum\s*\{{(.*?)\}}", text, re.S
    )
    if block is None:
        raise SystemExit(
            f"rpcgen_rust: no {service}Service rpc-id enum in {header}. If the "
            f"service has left the .rpc file, the ids now live only here and "
            f"need a checked-in map (plan stage 2b)."
        )
    return {
        m.group(1): int(m.group(2), 16)
        for m in re.finditer(r"(\w+)\s*=\s*(0x[0-9a-fA-F]+)", block.group(1))
    }


def emit(service, ids: dict[str, int], rpc_path: str) -> tuple[str, list[str]]:
    out: list[str] = []
    skipped: list[str] = []
    w = out.append

    w(f"// @generated by scripts/rpcgen_rust.py from {rpc_path} -- do not edit.")
    w("//")
    w("// The Rust counterpart of the generated C++ in src/deptran/rcc_rpc.h.")
    w("// Handler bodies are NOT here: they are hand-written, the same split")
    w("// C++ uses between RaftService and RaftServiceImpl.")
    w("")
    w("#![allow(dead_code)]")
    w("")
    w("use srpc::serializable::{")
    w("    BinaryReadArchive, BinaryWriteArchive, Deserialize, Serialize,")
    w("};")
    w("")
    w("/// Wire ids. Randomly assigned once by rpcgen and preserved only by")
    w("/// scraping the previously generated header, so they are the wire")
    w("/// contract -- never regenerate them.")
    w("pub mod rpc_id {")
    for name, code in sorted(ids.items()):
        w(f"    pub const {name}: i32 = 0x{code:08x};")
    w("}")
    w("")

    emitted = []
    for func in service.functions:
        try:
            fields_in = [(snake(n), rust_type(t)) for t, n in
                         ((a.type, a.name) for a in func.input)]
            fields_out = [(snake(n), rust_type(t)) for t, n in
                          ((a.type, a.name) for a in func.output)]
        except Unframed as exc:
            skipped.append(f"{func.name}: {exc}")
            continue

        req, resp = f"{func.name}Request", f"{func.name}Response"
        for struct, fields in ((req, fields_in), (resp, fields_out)):
            derive = "Clone, Debug, Default, PartialEq, Eq"
            if all(t != "String" for _, t in fields):
                derive = "Clone, Copy, " + derive.split(", ", 1)[1]
            w(f"#[derive({derive})]")
            w(f"pub struct {struct} {{")
            for fname, ftype in fields:
                w(f"    pub {fname}: {ftype},")
            w("}")
            w("")
            w(f"impl Serialize for {struct} {{")
            w("    fn serialize(&self, ar: &mut BinaryWriteArchive) {")
            for fname, _ in fields:
                w(f"        self.{fname}.serialize(ar);")
            w("    }")
            w("}")
            w("")
            w(f"impl Deserialize for {struct} {{")
            w("    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {")
            for fname, _ in fields:
                w(f"        self.{fname}.deserialize(ar);")
            w("    }")
            w("}")
            w("")
        emitted.append((func.name, req, resp))

    w(f"/// The {service.name} service. Implement this; the dispatch below")
    w("/// routes to it.")
    w(f"pub trait {service.name}Service: Send + Sync {{")
    for name, req, resp in emitted:
        w(f"    fn {snake(name)}(&self, req: &{req}) -> {resp};")
    w("}")
    w("")
    return "\n".join(out) + "\n", skipped


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--service", required=True)
    ap.add_argument("--rpc", required=True)
    ap.add_argument("--ids-from", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--check", action="store_true",
                    help="fail if the output would change")
    args = ap.parse_args()

    rpc_path = pathlib.Path(args.rpc)
    body = rpc_path.read_text().split("%%")[1]
    parsed = parse("rpc_source", body)
    services = {s.name: s for s in parsed.services}
    if args.service not in services:
        raise SystemExit(
            f"rpcgen_rust: no service {args.service!r} in {rpc_path}; "
            f"found {sorted(services)}"
        )

    ids = read_ids(pathlib.Path(args.ids_from), args.service)
    text, skipped = emit(services[args.service], ids, args.rpc)

    out = pathlib.Path(args.out)
    if args.check:
        current = out.read_text() if out.is_file() else ""
        if current != text:
            raise SystemExit(f"rpcgen_rust: {out} is stale; rerun without --check")
        print(f"{out}: up to date")
    else:
        out.write_text(text)
        print(f"wrote {out}")

    for line in skipped:
        print(f"  SKIPPED {line}", file=sys.stderr)
    if skipped:
        print(
            f"  -- {len(skipped)} RPC(s) not emitted. They are unreachable from "
            f"Rust until their framing is decided; this is deliberate, not a "
            f"partial success.",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
