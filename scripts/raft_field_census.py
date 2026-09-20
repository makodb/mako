#!/usr/bin/env python3
"""Step C's done-test (docs/migration/raft/plan.md): does any hand-written C++
under src/deptran name a field of RaftServerBase or RaftConsensusState?

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
    names = set(fields('RaftServerBase', rs)) | set(fields('RaftConsensusState', rs))
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
    print(f'fields: RaftServerBase {len(fields("RaftServerBase", rs))}, RaftConsensusState {len(fields("RaftConsensusState", rs))}')
    print(f'hand-written C++ sites naming one through the Raft server: {len(server_sites)}')
    for rel, lineno, recv, field in server_sites:
        print(f'  {rel}:{lineno}: {recv} . {field}')
    if other:
        print('same-named fields of OTHER objects (not counted):')
        for (rel, recv, field), n in sorted(other.items()):
            print(f'  {rel}: {recv}->{field} x{n}')
    return 1 if server_sites else 0


if __name__ == '__main__':
    sys.exit(main())
