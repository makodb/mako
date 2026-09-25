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
# Types Rust must not parse, carried as opaque bytes instead.
#
# janus::Command is an srpc::SerializableEnvelope: it writes
# [v32 kind][payload] with NO length prefix (serializable_envelope.rs:111-120)
# and can only be delimited by dispatching through SerializableRegistry, which
# is C++-owned. So Rust cannot decode it and cannot skip it either --
# generically.
#
# It does not have to. When every field BEFORE an opaque one is fixed-width and
# every field AFTER it is too, the opaque field's extent is arithmetic: it runs
# from a constant offset to `len - (width of the trailing fields)`. Rust copies
# that range verbatim, C++ keeps writing and reading the envelope exactly as it
# does today, and the wire does not change at all.
#
# That is the case for AppendEntries: `cmd` sits at a fixed offset 50 with only
# `leaderNextLogTerm` (8 bytes) after it. If a variable-length field is ever
# added after an opaque one, this stops holding and the emitter says so.
OPAQUE = {
    "Command": "janus::Command, an unframed SerializableEnvelope owned by C++",
}

# Wire widths, for computing an opaque field's extent. Only fixed-width types
# belong here; a type absent from this map cannot bound an opaque field.
WIDTH = {
    "uint64_t": 8, "uint32_t": 4, "uint16_t": 2,
    "int64_t": 8, "int32_t": 4,
    "ballot_t": 8, "parid_t": 4, "siteid_t": 2, "bool_t": 1,
}


def snake(name: str) -> str:
    """leaderPrevLogIndex -> leader_prev_log_index."""
    out = re.sub(r"(.)([A-Z][a-z]+)", r"\1_\2", name)
    return re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", out).lower()


def pascal(name: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in name.split("_"))


def rust_type(cpp: str) -> str:
    if cpp in OPAQUE:
        return "Vec<u8>"
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
    w("use srpc::client::{AsyncReplyCallback, Client};")
    w("use srpc::serializable::{")
    w("    make_source_proxy_buffer, BinaryReadArchive, BinaryWriteArchive,")
    w("    BufferSource, Deserialize, Serialize,")
    w("};")
    w("use srpc::server::{")
    w("    reject_malformed_request, Request, WeakServerConnection,")
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
        raw_in = [(a.type, a.name) for a in func.input]
        opaque_at = [i for i, (t, _) in enumerate(raw_in) if t in OPAQUE]
        layout = None
        if opaque_at:
            if len(opaque_at) > 1:
                skipped.append(
                    f"{func.name}: {len(opaque_at)} opaque fields; only one "
                    f"can be bounded by arithmetic"
                )
                continue
            at = opaque_at[0]
            before = [t for t, _ in raw_in[:at]]
            after = [t for t, _ in raw_in[at + 1:]]
            unbounded = [t for t in before + after if t not in WIDTH]
            if unbounded:
                skipped.append(
                    f"{func.name}: opaque {raw_in[at][0]} is not bounded -- "
                    f"variable-width field(s) {sorted(set(unbounded))} sit "
                    f"beside it, so its extent is not arithmetic"
                )
                continue
            layout = {
                "index": at,
                "name": snake(raw_in[at][1]),
                "prefix": sum(WIDTH[t] for t in before),
                "suffix": sum(WIDTH[t] for t in after),
                "after": [(snake(n), rust_type(t), WIDTH[t])
                          for t, n in raw_in[at + 1:]],
            }

        try:
            fields_in = [(snake(n), rust_type(t)) for t, n in raw_in]
            fields_out = [(snake(n), rust_type(t)) for t, n in
                          ((a.type, a.name) for a in func.output)]
        except SystemExit:
            raise

        req, resp = f"{func.name}Request", f"{func.name}Response"
        opaque_note = layout
        for struct, fields in ((req, fields_in), (resp, fields_out)):
            derive = "Clone, Debug, Default, PartialEq, Eq"
            # Copy only when every field is a scalar. String and Vec<u8> (an
            # opaque payload) both own heap storage.
            is_copy = all(t not in ("String", "Vec<u8>") for _, t in fields)
            if is_copy:
                derive = "Clone, Copy, " + derive.split(", ", 1)[1]
            if struct == req:
                req_is_copy = is_copy
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
            if struct == req and opaque_note is not None:
                # No streaming Deserialize: an opaque field's end is only
                # knowable from the whole frame. See from_body below.
                w(f"// {struct} has an opaque field ({opaque_note['name']}), so it")
                w("// decodes from the whole frame rather than a streaming archive.")
                w("")
                continue
            w(f"impl Deserialize for {struct} {{")
            w("    fn deserialize(&mut self, ar: &mut BinaryReadArchive) {")
            for fname, _ in fields:
                w(f"        self.{fname}.deserialize(ar);")
            w("    }")
            w("}")
            w("")

        if opaque_note is not None:
            pre, suf = opaque_note["prefix"], opaque_note["suffix"]
            w(f"impl {req} {{")
            w("    /// Decode from the whole request frame.")
            w("    ///")
            w(f"    /// `{opaque_note['name']}` is opaque to Rust -- C++ owns its")
            w("    /// encoding and it carries no length -- but its extent is")
            w(f"    /// arithmetic: it runs from byte {pre} to `len - {suf}`,")
            w("    /// because every field around it is fixed-width. The bytes")
            w("    /// are copied verbatim and handed back to C++ untouched.")
            w("    pub fn from_body(body: &[u8]) -> Option<Self> {")
            w(f"        if body.len() < {pre} + {suf} {{")
            w("            return None;")
            w("        }")
            w("        let mut src = BufferSource::new(body.as_ptr(), body.len());")
            w("        let mut ar = BinaryReadArchive::new(unsafe {")
            w("            make_source_proxy_buffer(&raw mut src)")
            w("        });")
            w("        let mut out = Self::default();")
            for fname, _ in fields_in[:opaque_note["index"]]:
                w(f"        out.{fname}.deserialize(&mut ar);")
            w("        if ar.failed() {")
            w("            return None;")
            w("        }")
            w(f"        out.{opaque_note['name']} =")
            w(f"            body[{pre}..body.len() - {suf}].to_vec();")
            w(f"        let mut tail = body.len() - {suf};")
            for fname, ftype, width in opaque_note["after"]:
                w(f"        out.{fname} = {ftype}::from_le_bytes(")
                w(f"            body[tail..tail + {width}].try_into().ok()?,")
                w("        );")
                w(f"        tail += {width};")
            w("        let _ = tail;")
            w("        Some(out)")
            w("    }")
            w("}")
            w("")
        emitted.append((func.name, req, resp, opaque_note, req_is_copy))

    # The handler trait. `Result<Resp, i32>` mirrors the C++ signature
    # `rusty::Result<RpcVoteResponse, srpc::i32>`: an Err is replied as a bare
    # error code with no body, exactly as the generated C++ wrapper does.
    w(f"/// The {service.name} service. Implement this; `dispatch` below routes")
    w("/// to it. An `Err(code)` is replied as that code with no body.")
    w(f"pub trait {service.name}Handler: Send + Sync {{")
    for name, req, resp, _, _ in emitted:
        w(f"    fn {snake(name)}(&self, req: &{req}) -> Result<{resp}, i32>;")
    w("}")
    w("")

    # Dispatch. Deliberately does NOT spawn a fiber, unlike the generated C++
    # wrapper's `Fiber::create_run`: srpc's Rust server already chooses
    # between dispatching inline and spawning one before it calls
    # __dispatch__ (rpc/server.rs:1478-1505), so spawning here would nest a
    # second fiber per request and put an Rc<Fiber> -- thread-bound -- in code
    # that must stay Send.
    w(f"/// Route one request to `handler`. Call this from `Service::__dispatch__`.")
    w(f"pub fn dispatch<H: {service.name}Handler>(")
    w("    handler: &H,")
    w("    rpc_id: i32,")
    w("    req: &Request,")
    w("    weak_sconn: &WeakServerConnection,")
    w(") {")
    w("    match rpc_id {")
    for name, req_t, resp_t, opaque, _ in emitted:
        w(f"        rpc_id::{name.upper()} => {{")
        if opaque is not None:
            # Decoded from the whole frame: the opaque field's end is only
            # knowable there, not from a streaming archive.
            w(f"            let Some(typed) = {req_t}::from_body(&req.body) else {{")
            w("                reject_malformed_request(req, weak_sconn);")
            w("                return;")
            w("            };")
        else:
            w(f"            let mut typed = {req_t}::default();")
            w("            let mut ar = BinaryReadArchive::new(unsafe {")
            w("                make_source_proxy_buffer(&req.src as *const _ as *mut _)")
            w("            });")
            w("            typed.deserialize(&mut ar);")
            w("            if ar.failed() {")
            w("                reject_malformed_request(req, weak_sconn);")
            w("                return;")
            w("            }")
        w(f"            reply_with(weak_sconn, req, handler.{snake(name)}(&typed));")
        w("        }")
    w("        // Unknown id: ignore, matching the generated C++ dispatch.")
    w("        _ => {}")
    w("    }")
    w("}")
    w("")
    w("/// Serialize a handler result back to the caller. Err(code) replies")
    w("/// that code with no body; Ok(resp) replies 0 with the fields.")
    w("fn reply_with<R: Serialize + 'static>(")
    w("    weak_sconn: &WeakServerConnection,")
    w("    req: &Request,")
    w("    result: Result<R, i32>,")
    w(") {")
    w("    let Some(sconn) = weak_sconn.upgrade() else { return };")
    w("    match result {")
    w("        Err(code) => sconn.reply(req, code, None),")
    w("        Ok(resp) => sconn.reply(")
    w("            req,")
    w("            0,")
    w("            Some(Box::new(move |ar: &mut BinaryWriteArchive| {")
    w("                resp.serialize(ar);")
    w("            })),")
    w("        ),")
    w("    }")
    w("}")
    w("")

    # The proxy: one method per RPC, mirroring RaftProxy's async_* calls.
    w(f"/// Client side. One method per RPC, mirroring {service.name}Proxy in")
    w("/// the generated C++ header.")
    w(f"pub struct {service.name}Proxy<'a> {{")
    w("    pub client: &'a Client,")
    w("}")
    w("")
    w(f"impl<'a> {service.name}Proxy<'a> {{")
    for name, req_t, resp_t, _, is_copy in emitted:
        w(f"    pub fn {snake(name)}_async(")
        w("        &self,")
        w(f"        req: &{req_t},")
        w("        on_reply: AsyncReplyCallback,")
        w("    ) -> Result<(), i32> {")
        # `.clone()` on a Copy type is a clippy error, and the crate builds
        # with -D warnings, so pick the right one per struct.
        w("        let payload = *req;" if is_copy else "        let payload = req.clone();")
        w(f"        self.client.request_async(")
        w(f"            rpc_id::{name.upper()},")
        w("            move |ar: &mut BinaryWriteArchive| payload.serialize(ar),")
        w("            on_reply,")
        w("        )")
        w("    }")
        w("")
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
