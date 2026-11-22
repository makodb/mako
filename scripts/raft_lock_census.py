#!/usr/bin/env python3
"""Record where RaftServer acquires each lock, so a refactor can prove it
did not move one.

The mutex demotion made re-entry a hang and the checked mutex makes it an
abort, but neither catches a lock QUIETLY WIDENING or NARROWING -- a body
moved inside or outside a critical section still runs, just with different
atomicity. This census is the before/after diff for that.

Usage:  python3 scripts/raft_lock_census.py > /tmp/before.txt
        ...refactor...
        python3 scripts/raft_lock_census.py | diff /tmp/before.txt -
"""
import re, sys

SRC = 'src/deptran/raft/server.cc'
HDR = 'src/deptran/raft/server.h'
LOCK = re.compile(r'(lock_guard|unique_lock)<[\w:]+>\s+\w+\s*\(\s*([\w:.>\-]*?)\s*\)')

def strip(line):
    return line.split('//')[0]

def census(path, pattern):
    lines = open(path).read().split('\n')
    out = []
    for i, l in enumerate(lines):
        m = pattern.match(l)
        if not m or l.rstrip().endswith(';'):
            continue
        name = m.group(1)
        j = i
        while j < len(lines) and '{' not in lines[j]:
            j += 1
        depth = 0
        events = []
        for k in range(j, len(lines)):
            c = strip(lines[k])
            lk = LOCK.search(c)
            before = depth
            depth += c.count('{') - c.count('}')
            if lk:
                # statements covered by this scope, counted below
                events.append([lk.group(2), before, k, 0])
            else:
                body = c.strip()
                if body and not body.startswith('#'):
                    for e in events:
                        e[3] += 1
            while events and depth < events[-1][1]:
                e = events.pop()
                out.append((name, e[0], e[3]))
            if depth <= 0:
                break
        for e in events:
            out.append((name, e[0], e[3]))
    return out

rows = census(SRC, re.compile(r'^[A-Za-z_][\w:<>,&\* ]*\bRaftServer::(~?\w+)'))
rows += census(HDR, re.compile(r'^\s{2}[\w:<>,\*&\s]+?\b(\w+)\s*\([^;]*\)\s*(?:const\s*)?\{'))

agg = {}
for name, mutex, n in rows:
    agg.setdefault((name, mutex), []).append(n)

print("# method | mutex | critical-section statement counts (sorted)")
for (name, mutex), ns in sorted(agg.items()):
    print(f"{name} | {mutex} | {sorted(ns)}")
print(f"# total acquisitions: {len(rows)}")
