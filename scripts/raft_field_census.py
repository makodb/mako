#!/usr/bin/env python3
"""Step C's done-test (the Raft migration plan (removed; see git history)): does any hand-written C++
under src/deptran name a field of RaftServerBase or RaftCore?

Hand-written means outside the RUSTYCPP GEN regions and the `#if RUSTYCPP_RUST`
source blocks, with comments and string literals stripped. A field is "named"
when it follows `->` or `.` on a receiver, or appears bare inside a member of
`class RaftServer` / `RaftServer::` (implicit this). Same-named fields of other
classes (RaftFrame::commo_, SiteInfo::partition_id_, PaxosServer::mtx_) are
reported by receiver so they can be told apart; the census's verdict counts
only receivers that are the Raft server.

Usage: python3 scripts/raft_field_census.py   (exit 1 if any site remains)
"""
import collections, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RS = os.path.join(ROOT, 'src/deptran/raft/src/server_h.rs')
SERVER_RECEIVERS = re.compile(
    r'^(self|server|svr|svr_|s|this|rep_sched_|raft_server|raft_sched_|leader_server|follower_server'
    r'|frame->svr_|config_->GetServer\(.*\)|GetServer\(.*\)|worker->rep_sched_|it->second->svr_)$')


def fields(struct, rs):
    m = re.search(r'pub struct ' + struct + r' \{(.*?)\n\}', rs, re.S)
    return re.findall(r'^\s+(?:pub(?:\(crate\))? )?([a-z_][a-z0-9_]*)\s*:(?!:)', m.group(1), re.M)


def strip(src):
    blank = lambda m: '\n' * m.group(0).count('\n')
    src = re.sub(r'/\*RUSTYCPP:GEN-BEGIN.*?GEN-END[^*]*\*/', blank, src, flags=re.S)
    src = re.sub(r'#if RUSTYCPP_RUST.*?#endif', blank, src, flags=re.S)
    src = re.sub(r'/\*.*?\*/', blank, src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    src = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', src)
    return src


def main():
    rs = open(RS).read()
    names = set(fields('RaftServerBase', rs)) | set(fields('RaftCore', rs))
    alt = '|'.join(sorted(names, key=len, reverse=True))
    via_receiver = re.compile(r'([A-Za-z_][A-Za-z0-9_]*(?:\([^()]*\))?(?:\.[A-Za-z_][A-Za-z0-9_]*|->[A-Za-z_][A-Za-z0-9_]*)*)\s*(?:->|\.)\s*(' + alt + r')\b')
    bare = re.compile(r'(?<![\w>.])(' + alt + r')\b')
    server_sites, other = [], collections.Counter()
    for root, _, files in os.walk(os.path.join(ROOT, 'src/deptran')):
        if '/raft/src' in root:
            continue
        for fn in files:
            if not fn.endswith(('.cc', '.h', '.hpp', '.cpp')):
                continue
            path = os.path.join(root, fn)
            rel = os.path.relpath(path, ROOT)
            text = strip(open(path).read())
            in_shim = False
            for lineno, line in enumerate(text.split('\n'), 1):
                is_server_file = fn in ('server.h', 'server.cc')
                if is_server_file:
                    opens = re.match(r'^class RaftServer : public RaftServerBase', line) or re.match(r'^[A-Za-z_:<>*& ]*RaftServer::~?\w+\(', line)
                    if opens and not line.rstrip().endswith('}'):
                        in_shim = True
                    elif line.startswith('};') or (line == '}' and in_shim):
                        in_shim = False
                for m in via_receiver.finditer(line):
                    recv, field = m.group(1), m.group(2)
                    # `this` is the Raft server only inside its own carriers.
                    if SERVER_RECEIVERS.match(recv) and (recv != 'this' or is_server_file):
                        server_sites.append((rel, lineno, recv, field))
                    else:
                        other[(rel, recv.split('->')[0].split('.')[0], field)] += 1
                if in_shim:
                    for m in bare.finditer(line):
                        server_sites.append((rel, lineno, '(implicit this)', m.group(1)))
    print(f'fields: RaftServerBase {len(fields("RaftServerBase", rs))}, RaftCore {len(fields("RaftCore", rs))}')
    print(f'hand-written C++ sites naming one through the Raft server: {len(server_sites)}')
    for rel, lineno, recv, field in server_sites:
        print(f'  {rel}:{lineno}: {recv} . {field}')
    if other:
        print('same-named fields of OTHER objects (not counted):')
        for (rel, recv, field), n in sorted(other.items()):
            print(f'  {rel}: {recv}->{field} x{n}')
    bare_wire = bare_wire_declarations()
    if bare_wire:
        print('BARE declarations of a scalar wire struct (indeterminate members):')
        for rel, lineno, text in bare_wire:
            print(f'  {rel}:{lineno}: {text}')
    return 1 if (server_sites or bare_wire) else 0


# The six scalar wire structs src/deptran/raft/messages.hpp generates. Their
# fields carry no per-member `{}` initializer: the transpiler branch srpc
# requires has no value-init mechanism at all, and carrying a fork of it for
# this alone is not worth a transpiler pin nobody else shares.
#
# That is safe only while every C++ site uses the BRACE form. `VoteReply{}`
# value-initializes every member of an aggregate whether or not the members
# have initializers of their own; `VoteReply reply;` leaves them
# indeterminate. The places that would bite are the error paths in
# channel_transport.hpp -- `if (r.is_err()) return VoteReply{};` -- where a
# garbage reply would be read as a real Raft vote or append result.
#
# So the guarantee moves here, from a transpiler attribute to a rule this
# repository owns and can explain.
WIRE_VALUE_STRUCTS = (
    'VoteReq', 'VoteReply', 'AppendEntriesReply',
    'EmptyAppendEntriesReq', 'EmptyAppendEntriesReply', 'InstallSnapshotReply',
)


def bare_wire_declarations():
    """C++ sites default-initializing a wire struct instead of brace-init."""
    names = '|'.join(WIRE_VALUE_STRUCTS)
    # `VoteReply reply;` but not `VoteReply reply{};`, not a parameter
    # (`, VoteReply reply)`), not a forward declaration (`struct VoteReply;`)
    # and not a return type (`VoteReply handle_vote(...)`).
    #
    # The leading alternation is a statement boundary, not a line start, so
    # `{ VoteReply reply; ... }` written on one line is caught too. `(` and
    # `,` are deliberately absent from it: those introduce a parameter, which
    # is initialized by its argument and is not the hazard.
    pattern = re.compile(
        r'(?:^|[{};])\s*(?:const\s+)?(' + names + r')\s+([A-Za-z_]\w*)\s*;')
    found = []
    for root, _, files in os.walk(os.path.join(ROOT, 'src/deptran')):
        for fn in sorted(files):
            if not fn.endswith(('.cc', '.h', '.hpp', '.cpp')):
                continue
            full = os.path.join(root, fn)
            rel = os.path.relpath(full, ROOT)
            with open(full, errors='replace') as source:
                for lineno, line in enumerate(source, 1):
                    stripped = line.split('//')[0]
                    if pattern.search(stripped):
                        found.append((rel, lineno, stripped.strip()))
    return found


if __name__ == '__main__':
    sys.exit(main())
