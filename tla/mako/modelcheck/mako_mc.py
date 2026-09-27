#!/usr/bin/env python3
"""Explicit-state BFS model checker for the Mako Verus model.

Independent cross-evidence for the Verus proof in ../src. Every function below
mirrors the spec function of the same name in types.rs, normal.rs,
recovery.rs, behavior.rs, invariants.rs and history.rs, in the same order.

Finite-instance restrictions (explorer only; the transition relation is not
changed):
  * transaction ids are assigned in submit order (id = |txns|). Ids are only
    compared for equality / with the sentinel -1, so this is a symmetry cut;
  * bodies are drawn from a finite set (keys x ops), at most --txns submits;
  * at most --crashes Crash steps (bounded through s.epoch, which only Crash
    increments);
  * tick canonicalization: after every step the live timestamps (every
    invoked, prepared_at of prepared-or-later, acked of committed, and tick)
    are dense-ranked. No guard reads a timestamp and every property compares
    live timestamps with `<` only, so the renaming is a bisimulation. Stutter
    becomes a self-loop and is not enumerated.
Everything else is enumerated exhaustively: every parameter value that can
make a guard true is generated and the guard itself is then evaluated.

Mutations (--mutation NAME) deliberately break one safety-relevant guard to
check that the checker has discriminating power.
"""

import argparse
import collections
import functools
import itertools
import re
import sys
import time
from collections import namedtuple

MUT = set()

# ===========================================================================
# types.rs
# ===========================================================================

READ = ('Read',)


def PUT(v):
    return ('Put', v)


def ADD(d):
    return ('Add', d)


def reads_key(op):
    return op[0] != 'Put'


def writes_key(op):
    return op[0] != 'Read'


def ops_get(body, k):
    for kk, op in body:
        if kk == k:
            return op
    raise KeyError(('body.ops', k))


def ops_dom(body):
    return [k for k, _ in body]


@functools.lru_cache(maxsize=None)
def read_set(body):
    return frozenset(k for k, op in body if reads_key(op))


@functools.lru_cache(maxsize=None)
def write_set(body):
    return frozenset(k for k, op in body if writes_key(op))


def apply_op(old, op):
    if op[0] == 'Read':
        return old
    if op[0] == 'Put':
        return op[1]
    return old + op[1]


Constants = namedtuple('Constants', 'shards threads comp cidx')


def valid_constants(c):
    return (c.shards >= 1 and c.threads >= 1 and c.comp >= 1 and len(c.cidx) == c.shards
            and all(0 <= c.cidx[i] < c.comp for i in range(c.shards)))


def is_shard(c, i):
    return 0 <= i < c.shards


def is_thread(c, t):
    return 0 <= t < c.threads


def is_comp(c, x):
    return 0 <= x < c.comp


def valid_key(k):
    return k >= 0


def owner(c, k):
    return k % c.shards


def group(c, i):
    return c.cidx[i]


@functools.lru_cache(maxsize=None)
def valid_txn(body):
    return len(body) > 0 and all(valid_key(k) for k in ops_dom(body))


@functools.lru_cache(maxsize=None)
def writes_at(c, body, i):
    return any(owner(c, k) == i for k in write_set(body))


def vc_zero(comp):
    return (0,) * comp


def vc_le(a, b, comp):
    return all(a[x] <= b[x] for x in range(comp))


# Wm: ('Fin', v) | ('Inf',)
INF = ('Inf',)


def Fin(v):
    return ('Fin', v)


def wm_le(x, w):
    return True if w[0] == 'Inf' else x <= w[1]


def wm_le_wm(a, b):
    return wm_le(a[1], b) if a[0] == 'Fin' else b[0] == 'Inf'


# Entry: ('Log', txn, epoch, clock) | ('Inf', epoch)
def entry_epoch(e):
    return e[2] if e[0] == 'Log' else e[1]


def entry_wm(e):
    return Fin(e[3]) if e[0] == 'Log' else INF


def is_log_of(e, txn):
    return e[0] == 'Log' and e[1] == txn


# Stream: (durable, pending) tuples of entries
def all_entries(st):
    return st[0] + st[1]


# Sid: (shard, coord, thread)
def valid_sid(c, sid):
    return is_shard(c, sid[0]) and is_shard(c, sid[1]) and is_thread(c, sid[2])


def sid_of(shard, coord, thread):
    return (shard, coord, thread)


@functools.lru_cache(maxsize=None)
def all_valid_sids(c):
    return tuple((i, j, t) for i in range(c.shards) for j in range(c.shards) for t in range(c.threads))


# Version: (txn, epoch, vc, value); ReadRec: (writer, epoch, vc, value)
Version = namedtuple('Version', 'txn epoch vc value')
ReadRec = namedtuple('ReadRec', 'writer epoch vc value')

# Status: 'Running' | 'Prepared' | 'Certified' | 'Committed' | 'AbortedF' | 'AbortedT'
TxnRec = namedtuple('TxnRec', 'body coord thread epoch status reads vc pidx installed invoked prepared_at acked')


class FMap:
    """Immutable finite map (Verus Map with a finite domain)."""
    __slots__ = ('d', 'h')

    def __init__(self, d=None):
        self.d = d if d is not None else {}
        self.h = None

    def __hash__(self):
        if self.h is None:
            self.h = hash(frozenset(self.d.items()))
        return self.h

    def __eq__(self, o):
        return isinstance(o, FMap) and self.d == o.d

    def __getitem__(self, k):
        return self.d[k]

    def __contains__(self, k):
        return k in self.d

    def __len__(self):
        return len(self.d)

    def dom(self):
        return self.d.keys()

    def items(self):
        return self.d.items()

    def values(self):
        return self.d.values()

    def insert(self, k, v):
        d = dict(self.d)
        d[k] = v
        return FMap(d)

    def __repr__(self):
        return '{' + ', '.join('%r: %r' % kv for kv in sorted(self.d.items(), key=lambda kv: repr(kv[0]))) + '}'


def is_aborted(st):
    return st in ('AbortedF', 'AbortedT')


def in_flight(r):
    return r.status in ('Running', 'Prepared')


def is_prepared_or_later(r):
    return r.status in ('Prepared', 'Certified', 'Committed', 'AbortedT')


def read_only(r):
    return len(write_set(r.body)) == 0


def clock_shard(c, r, i):
    return _clock_shard(c, r.body, r.coord, i, 'no_coord_clock' in MUT)


@functools.lru_cache(maxsize=None)
def _clock_shard(c, body, coord, i, no_coord):
    # clock_shard(c, r, i) == !read_only(r) && (writes_at(c, r.body, i) || i == r.coord)
    ro = len(write_set(body)) == 0
    if no_coord:
        return (not ro) and writes_at(c, body, i)
    return (not ro) and (writes_at(c, body, i) or i == coord)


def read_value(r, k):
    return r.reads[k].value if k in r.reads else 0


def write_value(r, k):
    return apply_op(read_value(r, k), ops_get(r.body, k))


State = namedtuple('State', 'epoch shards streams versions locks txns final_wm rolled_back prepared tick')
# shards: tuple of (epoch, counter)


def imax(a, b):
    return a if a >= b else b


def init_state(c):
    return State(epoch=0,
                 shards=tuple((0, 0) for _ in range(c.shards)),
                 streams=FMap({sid: ((), ()) for sid in all_valid_sids(c)}),
                 versions=FMap(), locks=FMap(), txns=FMap(), final_wm=FMap(),
                 rolled_back=frozenset(), prepared=(), tick=0)


def vers(versions, k):
    return versions[k] if k in versions else ()


def top_writer(versions, k):
    vs = vers(versions, k)
    return -1 if len(vs) == 0 else vs[-1].txn


def has_txn(txns, id):
    return id in txns


def stream_wm(es, e):
    for x in reversed(es):
        if entry_epoch(x) == e:
            return entry_wm(x)
    return Fin(0)


def below_wm(streams, c, vc, e):
    return all(wm_le(vc[group(c, sid[0])], stream_wm(streams[sid][0], e)) for sid in all_valid_sids(c))


def fvw_ready(fw, c, e):
    return all((i, e) in fw for i in range(c.shards))


def below_fvw(fw, c, vc, e):
    # Callers guard with fvw_ready; a missing key raises (Verus: unspecified).
    return all(wm_le(vc[group(c, i)], fw[(i, e)]) for i in range(c.shards))


def doomed(fw, c, r):
    return fvw_ready(fw, c, r.epoch) and not below_fvw(fw, c, r.vc, r.epoch)


def doomed_version(fw, c, v):
    return fvw_ready(fw, c, v.epoch) and not below_fvw(fw, c, v.vc, v.epoch)


def no_pending_epoch(st, e):
    return all(entry_epoch(x) != e for x in st[1])


def durable_has_log(st, txn):
    return any(is_log_of(x, txn) for x in st[0])


# ===========================================================================
# normal.rs
# ===========================================================================

def can_submit(s, c, id, body, coord, thread):
    return (id >= 0 and not has_txn(s.txns, id) and valid_txn(body) and is_shard(c, coord)
            and is_thread(c, thread)
            and all(not in_flight(r) for o, r in s.txns.items() if r.coord == coord and r.thread == thread))


def submit(s, c, id, body, coord, thread):
    r = TxnRec(body=body, coord=coord, thread=thread, epoch=s.shards[coord][0], status='Running',
               reads=FMap(), vc=vc_zero(c.comp), pidx=0, installed=frozenset(), invoked=s.tick,
               prepared_at=0, acked=0)
    return s._replace(txns=s.txns.insert(id, r))


def readable_top(s, c, r, k):
    vs = vers(s.versions, k)
    if len(vs) == 0:
        return True
    v = vs[-1]
    if 'read_no_fvw' in MUT:
        return True
    return v.epoch == r.epoch or (v.epoch < r.epoch and fvw_ready(s.final_wm, c, v.epoch)
                                  and below_fvw(s.final_wm, c, v.vc, v.epoch))


def can_read(s, c, id, k):
    if not has_txn(s.txns, id):
        return False
    r = s.txns[id]
    return (r.status == 'Running' and k in read_set(r.body) and k not in r.reads
            and s.shards[owner(c, k)][0] == r.epoch and readable_top(s, c, r, k))


def read_rec(versions, k):
    vs = vers(versions, k)
    if len(vs) == 0:
        return ReadRec(writer=-1, epoch=0, vc=(), value=0)
    v = vs[-1]
    return ReadRec(writer=v.txn, epoch=v.epoch, vc=v.vc, value=v.value)


def read(s, c, id, k):
    r = s.txns[id]
    return s._replace(txns=s.txns.insert(id, r._replace(reads=r.reads.insert(k, read_rec(s.versions, k)))))


def merge_lower_bound(c, r, vc):
    return all(vc_le(rd.vc, vc, c.comp) for k, rd in r.reads.items() if rd.writer != -1 and rd.epoch == r.epoch)


def fetch_lower_bound(s, c, r, vc):
    inc = 0 if 'fetch_no_inc' in MUT else 1
    return all(vc[group(c, i)] >= s.shards[i][1] + inc for i in range(c.shards) if clock_shard(c, r, i))


def exact_component(s, c, r, vc, x):
    inc = 0 if 'fetch_no_inc' in MUT else 1
    if vc[x] == 0:
        return True
    if any(rd.writer != -1 and rd.epoch == r.epoch and rd.vc[x] == vc[x] for k, rd in r.reads.items()):
        return True
    return any(clock_shard(c, r, i) and group(c, i) == x and vc[x] == s.shards[i][1] + inc
               for i in range(c.shards))


def clock_assignment(s, c, r, vc):
    return (len(vc) == c.comp and merge_lower_bound(c, r, vc) and fetch_lower_bound(s, c, r, vc)
            and all(exact_component(s, c, r, vc, x) for x in range(c.comp)))


def validated(s, c, r):
    rs = read_set(r.body)
    ws = write_set(r.body)
    if not all(k in r.reads for k in rs):
        return False
    nolock = 'no_lock_check' not in MUT
    if not all(s.shards[owner(c, k)][0] == r.epoch and (not nolock or k not in s.locks) for k in ws):
        return False
    return all(s.shards[owner(c, k)][0] == r.epoch and (not nolock or k not in s.locks)
               and top_writer(s.versions, k) == r.reads[k].writer for k in rs)


def can_prepare(s, c, id, vc):
    if not has_txn(s.txns, id):
        return False
    r = s.txns[id]
    return r.status == 'Running' and validated(s, c, r) and clock_assignment(s, c, r, vc)


def prepare(s, c, id, vc):
    r = s.txns[id]
    r2 = r._replace(status='Certified' if read_only(r) else 'Prepared', vc=vc, pidx=len(s.prepared),
                    prepared_at=s.tick)
    locks = dict(s.locks.d)
    for k in write_set(r.body):          # union_prefer_right(Map::new(write_set, |k| id))
        locks[k] = id
    shards = tuple((s.shards[i][0], s.shards[i][1] + 1) if clock_shard(c, r, i) else s.shards[i]
                   for i in range(c.shards))
    return s._replace(txns=s.txns.insert(id, r2), locks=FMap(locks), shards=shards,
                      prepared=s.prepared + (id,))


def keys_at(c, body, i):
    return frozenset(k for k in write_set(body) if owner(c, k) == i)


def can_install(s, c, id, i):
    if not has_txn(s.txns, id):
        return False
    r = s.txns[id]
    return (r.status == 'Prepared' and is_shard(c, i) and writes_at(c, r.body, i) and i not in r.installed
            and s.shards[i][0] == r.epoch)


def new_version(r, id, k):
    return Version(txn=id, epoch=r.epoch, vc=r.vc, value=write_value(r, k))


def log_entry(c, r, id, i):
    return ('Log', id, r.epoch, r.vc[group(c, i)])


def append_pending(streams, sid, e):
    st = streams[sid]
    return streams.insert(sid, (st[0], st[1] + (e,)))


def install(s, c, id, i):
    r = s.txns[id]
    keys = keys_at(c, r.body, i)
    sid = sid_of(i, r.coord, r.thread)
    versions = {}
    for k in set(s.versions.dom()) | set(keys):
        versions[k] = vers(s.versions, k) + (new_version(r, id, k),) if k in keys else s.versions[k]
    locks = {k: v for k, v in s.locks.items() if k not in keys}
    shards = list(s.shards)
    shards[i] = (s.shards[i][0], imax(s.shards[i][1], r.vc[group(c, i)]))
    streams = s.streams if i == r.coord else append_pending(s.streams, sid, log_entry(c, r, id, i))
    return s._replace(versions=FMap(versions), locks=FMap(locks), shards=tuple(shards), streams=streams,
                      txns=s.txns.insert(id, r._replace(installed=r.installed | {i})))


def all_installed(c, r):
    return all(i in r.installed for i in range(c.shards) if writes_at(c, r.body, i))


def can_certify(s, c, id):
    if not has_txn(s.txns, id):
        return False
    r = s.txns[id]
    return r.status == 'Prepared' and all_installed(c, r)


def certify(s, c, id):
    r = s.txns[id]
    sid = sid_of(r.coord, r.coord, r.thread)
    shards = list(s.shards)
    shards[r.coord] = (s.shards[r.coord][0], imax(s.shards[r.coord][1], r.vc[group(c, r.coord)]))
    return s._replace(streams=append_pending(s.streams, sid, log_entry(c, r, id, r.coord)),
                      shards=tuple(shards), txns=s.txns.insert(id, r._replace(status='Certified')))


def can_commit(s, c, id):
    if not has_txn(s.txns, id):
        return False
    r = s.txns[id]
    if 'no_below_wm' in MUT:
        return r.status == 'Certified'
    return r.status == 'Certified' and below_wm(s.streams, c, r.vc, r.epoch)


def commit(s, c, id):
    r = s.txns[id]
    return s._replace(txns=s.txns.insert(id, r._replace(status='Committed', acked=s.tick)))


def can_abort(s, id):
    return has_txn(s.txns, id) and s.txns[id].status == 'Running'


def abort(s, id):
    r = s.txns[id]
    return s._replace(txns=s.txns.insert(id, r._replace(status='AbortedF')))


def can_replicate(s, c, sid):
    return valid_sid(c, sid) and len(s.streams[sid][1]) > 0


def replicate(s, c, sid):
    st = s.streams[sid]
    return s._replace(streams=s.streams.insert(sid, (st[0] + (st[1][0],), st[1][1:])))


# ===========================================================================
# recovery.rs
# ===========================================================================

def abort_coordinated(txns, i):
    d = {}
    for id, r in txns.items():
        if 'abort_committed' in MUT and r.coord == i and r.status == 'Committed':
            d[id] = r._replace(status='AbortedT')
        elif r.coord == i and in_flight(r):
            d[id] = r._replace(status='AbortedT' if r.status == 'Prepared' else 'AbortedF')
        else:
            d[id] = r
    return FMap(d)


def can_crash(s, c, i, survive):
    return is_shard(c, i) and all(sid in survive and survive[sid] <= len(s.streams[sid][1])
                                  for sid in all_valid_sids(c) if sid[0] == i)


def crash_streams(s, i, survive):
    d = {}
    for sid, st in s.streams.items():
        if sid[0] == i:
            d[sid] = (st[0] + st[1][:survive[sid]], ())
        else:
            d[sid] = st
    return FMap(d)


def survives_crash(s, streams, i, v):
    if 'crash_keep_all' in MUT:
        return True
    if 'crash_drop_all' in MUT:
        return False
    r = s.txns[v.txn]
    return durable_has_log(streams[sid_of(i, r.coord, r.thread)], v.txn)


def crash_versions(s, c, streams, i):
    d = {}
    for k in s.versions.dom():
        if owner(c, k) == i:
            d[k] = tuple(v for v in s.versions[k] if survives_crash(s, streams, i, v))
        else:
            d[k] = s.versions[k]
    return FMap(d)


def crash_locks(s, c, i):
    return FMap({k: h for k, h in s.locks.items() if owner(c, k) != i and s.txns[h].coord != i})


def crash(s, c, i, survive):
    streams = crash_streams(s, i, survive)
    shards = list(s.shards)
    shards[i] = (s.epoch + 1, 0)
    return s._replace(epoch=s.epoch + 1, shards=tuple(shards), streams=streams,
                      versions=crash_versions(s, c, streams, i), locks=crash_locks(s, c, i),
                      txns=abort_coordinated(s.txns, i))


def can_advance(s, c, i):
    return is_shard(c, i) and s.shards[i][0] < s.epoch


def hung_thread(s, i, t):
    return any(r.coord == i and r.thread == t and r.status == 'Prepared' for id, r in s.txns.items())


def gets_inf(s, i, sid):
    if 'inf_all' in MUT:
        return sid[0] == i
    return sid[0] == i and not (sid[1] == i and hung_thread(s, i, sid[2]))


def append_inf(s, i, e):
    d = {}
    for sid, st in s.streams.items():
        d[sid] = (st[0], st[1] + (('Inf', e),)) if gets_inf(s, i, sid) else st
    return FMap(d)


def advance(s, c, i):
    e = s.shards[i][0]
    shards = list(s.shards)
    shards[i] = (e + 1, 0)
    return s._replace(shards=tuple(shards), streams=append_inf(s, i, e),
                      locks=FMap({k: h for k, h in s.locks.items() if s.txns[h].coord != i}),
                      txns=abort_coordinated(s.txns, i))


def can_close(s, c, i, e, w):
    if not (is_shard(c, i) and s.shards[i][0] > e and (i, e) not in s.final_wm):
        return False
    sids = [sid for sid in all_valid_sids(c) if sid[0] == i]
    nopend = 'close_ignore_pending' not in MUT
    if not all((not nopend or no_pending_epoch(s.streams[sid], e)) and wm_le_wm(w, stream_wm(s.streams[sid][0], e))
               for sid in sids):
        return False
    return any(w == stream_wm(s.streams[sid][0], e) for sid in sids)


def close(s, i, e, w):
    return s._replace(final_wm=s.final_wm.insert((i, e), w))


def can_rollback(s, c, i, e):
    return is_shard(c, i) and fvw_ready(s.final_wm, c, e) and (i, e) not in s.rolled_back


def keep_after_rollback(s, c, e, v):
    if 'rollback_noop' in MUT:
        return True
    return not (v.epoch == e and not below_fvw(s.final_wm, c, v.vc, e))


def rollback(s, c, i, e):
    d = {}
    for k in s.versions.dom():
        if owner(c, k) == i:
            d[k] = tuple(v for v in s.versions[k] if keep_after_rollback(s, c, e, v))
        else:
            d[k] = s.versions[k]
    return s._replace(versions=FMap(d), rolled_back=s.rolled_back | {(i, e)})


# ===========================================================================
# behavior.rs: enabled/apply with a finite enumeration of Action parameters
# ===========================================================================

def apply_tick(s, after):
    return after._replace(tick=s.tick + 1)


def candidate_vcs(s, c, r):
    """All vc with len == comp whose every component satisfies exact_component's
    disjunction (0, a same-epoch read clock, or a fetched counter+1); the guard
    clock_assignment is evaluated on each afterwards."""
    inc = 0 if 'fetch_no_inc' in MUT else 1
    per = []
    for x in range(c.comp):
        vals = {0}
        for k, rd in r.reads.items():
            if rd.writer != -1 and rd.epoch == r.epoch:
                vals.add(rd.vc[x])
        for i in range(c.shards):
            if clock_shard(c, r, i) and group(c, i) == x:
                vals.add(s.shards[i][1] + inc)
        per.append(sorted(vals))
    return [tuple(v) for v in itertools.product(*per)]


def successors(s, c, cfg):
    """Yield (label, next_state) for every enabled action instance."""
    txn_ids = sorted(s.txns.dom())
    # Submit
    if len(s.txns) < cfg.max_txns:
        # symmetry cut: the next id is |txns|; --free-ids tries every unused id < max_txns
        ids = [x for x in range(cfg.max_txns) if x not in s.txns] if cfg.free_ids else [len(s.txns)]
        for id in ids:
            for body in cfg.bodies:
                for coord in range(c.shards):
                    for thread in range(c.threads):
                        if can_submit(s, c, id, body, coord, thread):
                            yield (('Submit', id, body, coord, thread),
                                   apply_tick(s, submit(s, c, id, body, coord, thread)))
    for id in txn_ids:
        r = s.txns[id]
        # Read
        for k in sorted(read_set(r.body)):
            if can_read(s, c, id, k):
                yield (('Read', id, k), apply_tick(s, read(s, c, id, k)))
        # Prepare
        if r.status == 'Running':
            for vc in candidate_vcs(s, c, r):
                if can_prepare(s, c, id, vc):
                    yield (('Prepare', id, vc), apply_tick(s, prepare(s, c, id, vc)))
        # Install
        for i in range(c.shards):
            if can_install(s, c, id, i):
                yield (('Install', id, i), apply_tick(s, install(s, c, id, i)))
        if can_certify(s, c, id):
            yield (('Certify', id), apply_tick(s, certify(s, c, id)))
        if can_commit(s, c, id):
            yield (('Commit', id), apply_tick(s, commit(s, c, id)))
        if can_abort(s, id):
            yield (('Abort', id), apply_tick(s, abort(s, id)))
    # Replicate
    for sid in all_valid_sids(c):
        if can_replicate(s, c, sid):
            yield (('Replicate', sid), apply_tick(s, replicate(s, c, sid)))
    # Crash (bounded number)
    if s.epoch < cfg.max_crashes:
        for i in range(c.shards):
            sids = [sid for sid in all_valid_sids(c) if sid[0] == i]
            for pref in itertools.product(*[range(len(s.streams[sid][1]) + 1) for sid in sids]):
                survive = dict(zip(sids, pref))
                if can_crash(s, c, i, survive):
                    yield (('Crash', i, tuple(pref)), apply_tick(s, crash(s, c, i, survive)))
    # AdvanceEpoch
    for i in range(c.shards):
        if can_advance(s, c, i):
            yield (('AdvanceEpoch', i), apply_tick(s, advance(s, c, i)))
    # CloseEpoch
    for i in range(c.shards):
        for e in range(s.shards[i][0]):
            ws = set(stream_wm(s.streams[sid][0], e) for sid in all_valid_sids(c) if sid[0] == i)
            for w in sorted(ws):
                if can_close(s, c, i, e, w):
                    yield (('CloseEpoch', i, e, w), apply_tick(s, close(s, i, e, w)))
    # Rollback
    epochs = sorted(set(e for (i, e) in s.final_wm.dom()))
    for i in range(c.shards):
        for e in epochs:
            if can_rollback(s, c, i, e):
                yield (('Rollback', i, e), apply_tick(s, rollback(s, c, i, e)))
    # Stutter: tick + 1 only; identical after canonicalization (self-loop).


NO_CANON = [False]


def canon(s):
    """Dense-rank the live timestamps (see module docstring)."""
    if NO_CANON[0]:
        return s
    vals = {s.tick}
    for id, r in s.txns.items():
        vals.add(r.invoked)
        if is_prepared_or_later(r):
            vals.add(r.prepared_at)
        if r.status == 'Committed':
            vals.add(r.acked)
    rank = {v: n for n, v in enumerate(sorted(vals))}
    d = {}
    for id, r in s.txns.items():
        d[id] = r._replace(invoked=rank[r.invoked],
                           prepared_at=rank[r.prepared_at] if is_prepared_or_later(r) else r.prepared_at,
                           acked=rank[r.acked] if r.status == 'Committed' else r.acked)
    return s._replace(txns=FMap(d), tick=rank[s.tick])


# ===========================================================================
# invariants.rs
# ===========================================================================

def txn(txns, id):
    return txns[id]


def coord_sid(r, i):
    return sid_of(i, r.coord, r.thread)


def stream_at(streams, r, i):
    return all_entries(streams[coord_sid(r, i)])


def has_log(es, id):
    return any(is_log_of(x, id) for x in es)


def has_version(versions, k, id):
    return any(v.txn == id for v in vers(versions, k))


def logs_below(es, e, x):
    return all(y[3] < x for y in es if y[0] == 'Log' and y[2] == e)


def no_inf(es, e):
    return not any(y[0] == 'Inf' and y[1] == e for y in es)


def stream_below(es, e, x):
    return logs_below(es, e, x) and no_inf(es, e)


def committed(r):
    return r.status == 'Committed'


def certified_or_committed(r):
    return r.status in ('Certified', 'Committed')


def aborted_prepared(r):
    return r.status == 'AbortedT'


def logged_at(c, r, i):
    return clock_shard(c, r, i) and (certified_or_committed(r) if i == r.coord else i in r.installed)


def pidx_of(txns, id):
    return -1 if id == -1 else txn(txns, id).pidx


def lost(streams, c, r, id):
    return any(logged_at(c, r, i) and not has_log(stream_at(streams, r, i), id) for i in range(c.shards))


def inv_shapes(s, c):
    return (valid_constants(c) and len(s.shards) == c.shards
            and set(s.streams.dom()) == set(all_valid_sids(c))
            and all(s.shards[i][0] <= s.epoch and s.shards[i][1] >= 0 for i in range(c.shards))
            and all(valid_key(k) for k in s.versions.dom()))


def inv_txn(s, c, id):
    r = txn(s.txns, id)
    se = s.shards
    checks = [
        valid_txn(r.body),
        is_shard(c, r.coord),
        is_thread(c, r.thread),
        r.epoch <= se[r.coord][0],
        (not in_flight(r)) or r.epoch == se[r.coord][0],
        (not aborted_prepared(r)) or r.epoch < se[r.coord][0],
        len(r.vc) == c.comp,
        all(r.vc[x] >= 0 for x in range(c.comp)),
        (not is_prepared_or_later(r)) or all(r.vc[group(c, i)] >= 1 for i in range(c.shards) if clock_shard(c, r, i)),
        (not is_prepared_or_later(r)) or all(se[owner(c, k)][0] >= r.epoch for k in write_set(r.body)),
        all(is_shard(c, i) and writes_at(c, r.body, i) for i in r.installed),
        all(k in read_set(r.body) for k in r.reads.dom()),
        r.invoked < s.tick,
        r.status != 'Running' or r.installed == frozenset(),
        (not is_prepared_or_later(r)) or (r.pidx < len(s.prepared) and s.prepared[r.pidx] == id
                                          and r.invoked < r.prepared_at < s.tick
                                          and all(k in r.reads for k in read_set(r.body))),
        r.status != 'Prepared' or not read_only(r),
        (not certified_or_committed(r)) or all_installed(c, r),
        (not committed(r)) or (r.prepared_at < r.acked < s.tick),
    ]
    for n, ok in enumerate(checks):
        if not ok:
            return 'inv_txn[%d] id=%d' % (n, id)
    return None


def inv_txns(s, c):
    for id in s.txns.dom():
        if not id >= 0:
            return 'id<0'
        e = inv_txn(s, c, id)
        if e:
            return e
    return None


def inv_prepared(s, c):
    for j, p in enumerate(s.prepared):
        if not (has_txn(s.txns, p) and txn(s.txns, p).pidx == j and is_prepared_or_later(txn(s.txns, p))):
            return 'inv_prepared[0] j=%d' % j
    for j1 in range(len(s.prepared)):
        for j2 in range(j1 + 1, len(s.prepared)):
            if not txn(s.txns, s.prepared[j1]).prepared_at < txn(s.txns, s.prepared[j2]).prepared_at:
                return 'inv_prepared[1] %d<%d' % (j1, j2)
    return None


def inv_exclusive(s, c):
    for a in s.txns.dom():
        for b in s.txns.dom():
            ra, rb = txn(s.txns, a), txn(s.txns, b)
            if a != b and ra.coord == rb.coord and ra.thread == rb.thread and in_flight(ra) and in_flight(rb):
                return 'inv_exclusive %d %d' % (a, b)
    return None


def inv_lock(s, c, k):
    h = s.locks[k]
    if not has_txn(s.txns, h):
        return False
    r = txn(s.txns, h)
    return (r.status == 'Prepared' and k in write_set(r.body) and owner(c, k) not in r.installed
            and all(pidx_of(s.txns, v.txn) < r.pidx for v in vers(s.versions, k)))


def inv_locks(s, c):
    for k in s.locks.dom():
        if not inv_lock(s, c, k):
            return 'inv_lock k=%d' % k
    for id, r in s.txns.items():
        if r.status == 'Prepared':
            for k in write_set(r.body):
                if owner(c, k) not in r.installed and s.shards[owner(c, k)][0] == r.epoch:
                    if not (k in s.locks and s.locks[k] == id):
                        return 'inv_locks[1] id=%d k=%d' % (id, k)
    return None


def inv_version(s, c, k, v):
    if not has_txn(s.txns, v.txn):
        return False
    r = txn(s.txns, v.txn)
    return (v.epoch == r.epoch and v.vc == r.vc and v.value == write_value(r, k) and k in write_set(r.body)
            and owner(c, k) in r.installed and is_prepared_or_later(r))


def inv_versions(s, c):
    for k in s.versions.dom():
        vs = s.versions[k]
        for v in vs:
            if not inv_version(s, c, k, v):
                return 'inv_version k=%d txn=%d' % (k, v.txn)
        for a in range(len(vs)):
            for b in range(a + 1, len(vs)):
                if not txn(s.txns, vs[a].txn).pidx < txn(s.txns, vs[b].txn).pidx:
                    return 'inv_versions_of order k=%d' % k
    return None


def inv_read_writer(s, c, id, k):
    r = txn(s.txns, id)
    rd = r.reads[k]
    if not has_txn(s.txns, rd.writer):
        return False
    w = txn(s.txns, rd.writer)
    return (rd.writer != id and k in write_set(w.body) and rd.epoch == w.epoch and rd.vc == w.vc
            and rd.value == write_value(w, k) and is_prepared_or_later(w) and owner(c, k) in w.installed
            and w.epoch <= r.epoch
            and (not w.epoch < r.epoch or (fvw_ready(s.final_wm, c, w.epoch)
                                           and below_fvw(s.final_wm, c, w.vc, w.epoch)))
            and (not is_prepared_or_later(r) or w.pidx < r.pidx)
            and (not (is_prepared_or_later(r) and w.epoch == r.epoch) or vc_le(w.vc, r.vc, c.comp)))


def inv_read_order(s, c, id, k, o):
    r = txn(s.txns, id)
    rd = r.reads[k]
    ro = txn(s.txns, o)
    a = (not (owner(c, k) in ro.installed and has_version(s.versions, k, o))) or pidx_of(s.txns, o) <= pidx_of(s.txns, rd.writer)
    b = owner(c, k) in ro.installed or is_aborted(ro.status) or s.shards[owner(c, k)][0] > ro.epoch
    return a and b


def inv_read(s, c, id, k):
    r = txn(s.txns, id)
    rd = r.reads[k]
    if rd.writer == -1 and rd.value != 0:
        return 'inv_read value id=%d k=%d' % (id, k)
    if rd.writer != -1 and not inv_read_writer(s, c, id, k):
        return 'inv_read_writer id=%d k=%d' % (id, k)
    if is_prepared_or_later(r):
        for o, ro in s.txns.items():
            if o != id and is_prepared_or_later(ro) and ro.pidx < r.pidx and k in write_set(ro.body):
                if not inv_read_order(s, c, id, k, o):
                    return 'inv_read_order id=%d k=%d o=%d' % (id, k, o)
    return None


def inv_reads(s, c):
    for id, r in s.txns.items():
        for k in r.reads.dom():
            e = inv_read(s, c, id, k)
            if e:
                return e
    return None


def inv_entry(s, c, sid, e):
    se = s.shards[sid[0]]
    if not entry_epoch(e) <= se[0]:
        return False
    if e[0] == 'Inf' and not entry_epoch(e) < se[0]:
        return False
    if e[0] == 'Log':
        if not has_txn(s.txns, e[1]):
            return False
        r = txn(s.txns, e[1])
        return (r.epoch == e[2] and r.coord == sid[1] and r.thread == sid[2] and e[3] == r.vc[group(c, sid[0])]
                and logged_at(c, r, sid[0]) and (e[2] != se[0] or e[3] <= se[1]))
    return True


def inv_stream_order(es):
    n = len(es)
    for a in range(n):
        for b in range(a + 1, n):
            if not entry_epoch(es[a]) <= entry_epoch(es[b]):
                return 'epochs'
            if es[a][0] == 'Log' and es[b][0] == 'Log':
                if es[a][2] == es[b][2] and not es[a][3] < es[b][3]:
                    return 'clocks'
                if es[a][1] == es[b][1]:
                    return 'dup'
            if es[a][0] == 'Inf' and not entry_epoch(es[b]) > es[a][1]:
                return 'inf-last'
        if es[a][0] == 'Log' and not es[a][3] >= 1:
            return 'clock>=1'
    return None


def inv_streams(s, c):
    for sid in all_valid_sids(c):
        es = all_entries(s.streams[sid])
        e = inv_stream_order(es)
        if e:
            return 'inv_stream_order(%s) sid=%r' % (e, sid)
        for x in es:
            if not inv_entry(s, c, sid, x):
                return 'inv_entry sid=%r entry=%r' % (sid, x)
    return None


def inv_coord_below(s, c, r):
    return (not (r.status == 'Prepared' or aborted_prepared(r))) or \
        stream_below(stream_at(s.streams, r, r.coord), r.epoch, r.vc[group(c, r.coord)])


def inv_uninstalled_below(s, c, r):
    return r.status != 'Prepared' or all(
        logs_below(stream_at(s.streams, r, i), r.epoch, r.vc[group(c, i)])
        for i in range(c.shards) if clock_shard(c, r, i) and i not in r.installed)


def inv_logged(s, c, r, id):
    return all(has_log(stream_at(s.streams, r, i), id)
               or (s.shards[i][0] > r.epoch and stream_below(stream_at(s.streams, r, i), r.epoch, r.vc[group(c, i)]))
               for i in range(c.shards) if logged_at(c, r, i))


def inv_present(s, c, r, id):
    if not (r.status == 'Prepared' or certified_or_committed(r)):
        return True
    for k in write_set(r.body):
        if owner(c, k) in r.installed and not has_version(s.versions, k, id):
            if not ((doomed(s.final_wm, c, r) and (owner(c, k), r.epoch) in s.rolled_back) or lost(s.streams, c, r, id)):
                return False
    return True


def inv_all_logs(s, c):
    for id, r in s.txns.items():
        if not inv_coord_below(s, c, r):
            return 'inv_coord_below id=%d' % id
        if not inv_uninstalled_below(s, c, r):
            return 'inv_uninstalled_below id=%d' % id
        if not inv_logged(s, c, r, id):
            return 'inv_logged id=%d' % id
        if not inv_present(s, c, r, id):
            return 'inv_present id=%d' % id
    return None


def inv_committed(s, c):
    for id, r in s.txns.items():
        if committed(r) and not below_wm(s.streams, c, r.vc, r.epoch):
            return 'inv_committed id=%d' % id
    return None


def inv_final(s, c):
    for (i, e) in s.final_wm.dom():
        w = s.final_wm[(i, e)]
        sids = [sid for sid in all_valid_sids(c) if sid[0] == i]
        ok = (is_shard(c, i) and s.shards[i][0] > e
              and all(no_pending_epoch(s.streams[sid], e) and wm_le_wm(w, stream_wm(s.streams[sid][0], e)) for sid in sids)
              and any(w == stream_wm(s.streams[sid][0], e) for sid in sids))
        if not ok:
            return 'inv_final_of (%d,%d)' % (i, e)
    return None


def inv_rolled_back(s, c):
    for (i, e) in s.rolled_back:
        if not (is_shard(c, i) and fvw_ready(s.final_wm, c, e)):
            return 'inv_rolled_back_of ready (%d,%d)' % (i, e)
        for k in s.versions.dom():
            if owner(c, k) == i:
                for v in s.versions[k]:
                    if v.epoch == e and not below_fvw(s.final_wm, c, v.vc, e):
                        return 'inv_rolled_back_of (%d,%d) k=%d' % (i, e, k)
    return None


INV_CONJUNCTS = [
    ('inv_shapes', lambda s, c: None if inv_shapes(s, c) else 'inv_shapes'),
    ('inv_txns', inv_txns), ('inv_prepared', inv_prepared), ('inv_exclusive', inv_exclusive),
    ('inv_locks', inv_locks), ('inv_versions', inv_versions), ('inv_reads', inv_reads),
    ('inv_streams', inv_streams), ('inv_all_logs', inv_all_logs), ('inv_committed', inv_committed),
    ('inv_final', inv_final), ('inv_rolled_back', inv_rolled_back),
]


# ===========================================================================
# history.rs: the external specification, by brute force over serial orders
# ===========================================================================

def serial_value(txns, k, order):
    val = 0
    for id in order:
        body = txns[id].body
        if k in ops_dom(body):
            val = apply_op(val, ops_get(body, k))
    return val


def serial_witness(s, order):
    t = s.txns
    if len(set(order)) != len(order):
        return False
    if not all(id in t and certified_or_committed(t[id]) for id in order):
        return False
    if not all(id in order for id, r in t.items() if committed(r)):
        return False
    n = len(order)
    for a in range(n):
        if committed(t[order[a]]):
            for b in range(n):
                if t[order[a]].acked < t[order[b]].invoked and not a < b:
                    return False
    for j in range(n):
        r = t[order[j]]
        if committed(r):
            for k in read_set(r.body):
                if r.reads[k].value != serial_value(t, k, order[:j]):
                    return False
    return True


def strictly_serializable(s):
    comm = [id for id, r in s.txns.items() if committed(r)]
    cert = [id for id, r in s.txns.items() if r.status == 'Certified']
    for n in range(len(cert) + 1):
        for extra in itertools.combinations(cert, n):
            for order in itertools.permutations(comm + list(extra)):
                if serial_witness(s, order):
                    return True
    return False


# ---------------------------------------------------------------------------
# Durability, atomicity and rollback-safety statements
# ---------------------------------------------------------------------------

# Statements as in the team's theorem_durability / theorem_atomicity /
# theorem_rollback_safety / theorem_ack_is_final (checked here on reachable
# states, without assuming inv).

def check_durability(s, c):
    for id, r in s.txns.items():
        if committed(r):
            for j in range(c.shards):
                if logged_at(c, r, j) and not durable_has_log(s.streams[coord_sid(r, j)], id):
                    return 'durability: committed %d has no durable entry at shard %d' % (id, j)
            if lost(s.streams, c, r, id):
                return 'durability: committed %d is lost' % id
            if doomed(s.final_wm, c, r):
                return 'durability: committed %d is doomed' % id
            for k in write_set(r.body):
                if not has_version(s.versions, k, id):
                    return 'durability: committed %d lost its version of key %d' % (id, k)
    return None


def check_atomicity(s, c):
    fw = s.final_wm
    for id, r in s.txns.items():
        if not (is_prepared_or_later(r) and fvw_ready(fw, c, r.epoch)):
            continue
        dm = doomed(fw, c, r)
        if aborted_prepared(r) and not dm:
            return 'atomicity(a): aborted-prepared %d is below its epoch FVW' % id
        if certified_or_committed(r) and not dm and not lost(s.streams, c, r, id):
            for k in write_set(r.body):
                if not has_version(s.versions, k, id):
                    return 'atomicity(b): certified %d (not doomed, not lost) misses key %d' % (id, k)
        if dm:
            for k in write_set(r.body):
                if (owner(c, k), r.epoch) in s.rolled_back and has_version(s.versions, k, id):
                    return 'atomicity(c): doomed %d still has key %d after rollback' % (id, k)
    for k in s.versions.dom():
        for v in s.versions[k]:
            r = s.txns[v.txn]
            if is_aborted(r.status) and (owner(c, k), v.epoch) in s.rolled_back:
                return 'atomicity: aborted %d still has a version of key %d after rollback' % (v.txn, k)
    return None


def check_rollback_safety(s, c):
    fw = s.final_wm
    for t1, r1 in s.txns.items():
        if not is_prepared_or_later(r1):
            continue
        for k, rd in r1.reads.items():
            if rd.writer == -1:
                continue
            t0 = rd.writer
            if t0 not in s.txns:
                return 'rollback-safety: %d read key %d from unknown %d' % (t1, k, t0)
            r0 = s.txns[t0]
            ok = (k in write_set(r0.body) and is_prepared_or_later(r0) and r0.epoch <= r1.epoch
                  and (r0.epoch != r1.epoch or vc_le(r0.vc, r1.vc, c.comp))
                  and (not r0.epoch < r1.epoch or not doomed(fw, c, r0))
                  and (not doomed(fw, c, r0) or doomed(fw, c, r1))
                  and (not committed(r1) or (certified_or_committed(r0) and not doomed(fw, c, r0)
                                             and not lost(s.streams, c, r0, t0))))
            if not ok:
                return 'rollback-safety: %d read key %d from %d (status %s, doomed=%s, lost=%s)' % (
                    t1, k, t0, r0.status, doomed(fw, c, r0), lost(s.streams, c, r0, t0))
    return None


def check_ack_final(s, n_raw):
    """theorem_ack_is_final, one step: a committed record is unchanged by any
    transition (compared before the successor's tick canonicalization, so the
    timestamps are in the same numbering)."""
    for id, r in s.txns.items():
        if committed(r) and (id not in n_raw.txns or n_raw.txns[id] != r):
            return 'ack_is_final: committed %d changed' % id
    return None


# ===========================================================================
# Explorer
# ===========================================================================

Cfg = namedtuple('Cfg', 'max_txns bodies max_crashes free_ids')


def make_bodies(keys, ops, max_keys):
    bodies = []
    for n in range(1, max_keys + 1):
        for ks in itertools.combinations(keys, n):
            for os_ in itertools.product(ops, repeat=n):
                bodies.append(tuple(sorted(zip(ks, os_))))
    return bodies


def fmt_state(s):
    out = ['  epoch=%d tick=%d shards=%s prepared=%s' % (s.epoch, s.tick, list(s.shards), list(s.prepared))]
    for id, r in sorted(s.txns.items()):
        out.append('  T%d: %s coord=%d thr=%d ep=%d vc=%s pidx=%d inst=%s reads=%s inv=%d prep=%d ack=%d body=%s' % (
            id, r.status, r.coord, r.thread, r.epoch, r.vc, r.pidx, sorted(r.installed),
            {k: tuple(v) for k, v in r.reads.items()}, r.invoked, r.prepared_at, r.acked, r.body))
    for sid, st in sorted(s.streams.items()):
        if st[0] or st[1]:
            out.append('  stream%s durable=%s pending=%s' % (sid, list(st[0]), list(st[1])))
    for k, vs in sorted(s.versions.items()):
        out.append('  versions[%d]=%s' % (k, [tuple(v) for v in vs]))
    if len(s.locks):
        out.append('  locks=%r' % s.locks)
    if len(s.final_wm):
        out.append('  final_wm=%r' % s.final_wm)
    if s.rolled_back:
        out.append('  rolled_back=%s' % sorted(s.rolled_back))
    return '\n'.join(out)


def check_state(s, c, want_inv):
    errs = []
    if want_inv:
        for name, f in INV_CONJUNCTS:
            e = f(s, c)
            if e:
                errs.append(('invariant', name, e))
    if not strictly_serializable(s):
        errs.append(('spec', 'strictly_serializable', 'no serial witness'))
    for name, f in (('durability', check_durability), ('atomicity', check_atomicity),
                    ('rollback_safety', check_rollback_safety)):
        e = f(s, c)
        if e:
            errs.append(('property', name, e))
    return errs


def explore(c, cfg, max_states, want_inv, stop_on_first, time_limit, progress):
    s0 = canon(init_state(c))
    parent = {s0: None}
    frontier = collections.deque([s0])
    stats = collections.Counter()
    violations = collections.OrderedDict()
    depth = {s0: 0}
    maxd = 0
    t0 = time.time()
    transitions = 0
    exhaustive = True
    while frontier:
        s = frontier.popleft()
        stats['states'] += 1
        # --- properties of s ---
        errs = check_state(s, c, want_inv)
        for kind, name, msg in errs:
            if name not in violations:
                violations[name] = (kind, msg, s)
        # --- coverage statistics ---
        sts = [r.status for r in s.txns.values()]
        if 'Committed' in sts:
            stats['with_committed'] += 1
            if s.epoch > 0:
                stats['with_committed_after_crash'] += 1
            if any(committed(r) and any(rd.writer != -1 for rd in r.reads.values()) for r in s.txns.values()):
                stats['with_committed_reader_of_txn'] += 1
            if any(committed(r) and any(rd.writer != -1 and rd.epoch < r.epoch for rd in r.reads.values())
                   for r in s.txns.values()):
                stats['with_committed_cross_epoch_read'] += 1
            if sum(1 for x in sts if x == 'Committed') >= 2:
                stats['with_2_committed'] += 1
        if s.epoch > 0:
            stats['after_crash'] += 1
        if 'AbortedT' in sts:
            stats['with_aborted_prepared'] += 1
            if any(aborted_prepared(r) and fvw_ready(s.final_wm, c, r.epoch) for r in s.txns.values()):
                stats['with_aborted_prepared_fvw_ready'] += 1
        if s.rolled_back:
            stats['with_rollback'] += 1
        if any(doomed(s.final_wm, c, r) for r in s.txns.values() if r.status in ('Certified', 'AbortedT', 'Prepared')):
            stats['with_doomed_txn'] += 1
        if violations and stop_on_first:
            break
        if len(parent) >= max_states or (time_limit and time.time() - t0 > time_limit):
            exhaustive = False
            continue
        d = depth[s]
        for label, n in successors(s, c, cfg):
            e = check_ack_final(s, n)
            if e and 'ack_is_final' not in violations:
                violations['ack_is_final'] = ('property', e, s)
            n = canon(n)
            transitions += 1
            stats['act_' + label[0]] += 1
            if n not in parent:
                parent[n] = (s, label)
                depth[n] = d + 1
                maxd = max(maxd, d + 1)
                frontier.append(n)
        if progress and stats['states'] % 20000 == 0:
            print('  ... %d states expanded, %d seen, frontier %d, depth %d, %.0fs' % (
                stats['states'], len(parent), len(frontier), maxd, time.time() - t0), file=sys.stderr, flush=True)
    if frontier:
        exhaustive = False
    return dict(states=len(parent), expanded=stats['states'], transitions=transitions, depth=maxd,
                exhaustive=exhaustive and not (violations and stop_on_first), elapsed=time.time() - t0,
                stats=stats, violations=violations, parent=parent)


def trace(parent, s):
    steps = []
    while parent[s] is not None:
        p, label = parent[s]
        steps.append((label, s))
        s = p
    steps.reverse()
    return steps


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--shards', type=int, default=1)
    ap.add_argument('--threads', type=int, default=1)
    ap.add_argument('--comp', type=int, default=None, help='clock length (default: shards, identity cidx)')
    ap.add_argument('--cidx', type=str, default=None, help='comma list shard->component')
    ap.add_argument('--txns', type=int, default=2)
    ap.add_argument('--keys', type=str, default='0,1')
    ap.add_argument('--ops', type=str, default='R,P1,A1', help='from R, P<v>, A<d>')
    ap.add_argument('--max-body-keys', type=int, default=2)
    ap.add_argument('--bodies', type=str, default='',
                    help="explicit body list, e.g. '0A1;1A1;0A1+1A1;0R+1R' (overrides --keys/--ops)")
    ap.add_argument('--crashes', type=int, default=1)
    ap.add_argument('--max-states', type=int, default=5_000_000)
    ap.add_argument('--time-limit', type=float, default=0)
    ap.add_argument('--no-inv', action='store_true', help='skip the 12 invariant conjuncts')
    ap.add_argument('--mutation', type=str, default='', help='comma list: no_below_wm, crash_keep_all, '
                    'crash_drop_all, no_coord_clock, fetch_no_inc, read_no_fvw, no_lock_check, inf_all, '
                    'close_ignore_pending, rollback_noop, abort_committed')
    ap.add_argument('--all-violations', action='store_true', help='keep exploring after the first violation')
    ap.add_argument('--progress', action='store_true')
    ap.add_argument('--no-canon', action='store_true',
                    help='abstraction check: keep raw ticks (finite without Stutter, only larger)')
    ap.add_argument('--free-ids', action='store_true',
                    help='abstraction check: Submit may use any unused id < --txns')
    a = ap.parse_args()

    for m in filter(None, a.mutation.split(',')):
        MUT.add(m)
    comp = a.comp if a.comp is not None else a.shards
    cidx = tuple(int(x) for x in a.cidx.split(',')) if a.cidx else tuple(range(a.shards))
    c = Constants(a.shards, a.threads, comp, cidx)
    assert valid_constants(c), c
    ops = []
    for o in a.ops.split(','):
        ops.append(READ if o == 'R' else PUT(int(o[1:])) if o[0] == 'P' else ADD(int(o[1:])))
    keys = [int(k) for k in a.keys.split(',')]
    bodies = make_bodies(keys, ops, a.max_body_keys)
    if a.bodies:
        bodies = []
        for b in a.bodies.split(';'):
            items = []
            for it in b.split('+'):
                mm = re.fullmatch(r'(\d+)([RPA])(-?\d*)', it)
                assert mm, it
                k, kind, v = int(mm.group(1)), mm.group(2), mm.group(3)
                items.append((k, READ if kind == 'R' else PUT(int(v)) if kind == 'P' else ADD(int(v))))
            assert len(set(k for k, _ in items)) == len(items), b
            bodies.append(tuple(sorted(items)))
    cfg = Cfg(a.txns, bodies, a.crashes, a.free_ids)
    NO_CANON[0] = a.no_canon
    print('config: shards=%d threads=%d comp=%d cidx=%s txns<=%d bodies(%d)=%s crashes<=%d mutation=%s%s%s' % (
        c.shards, c.threads, c.comp, list(c.cidx), a.txns, len(bodies),
        [dict(b) for b in bodies] if a.bodies else ('keys=%s ops=%s' % (keys, a.ops)), a.crashes, sorted(MUT) or 'none',
        ' no-canon' if a.no_canon else '', ' free-ids' if a.free_ids else ''), flush=True)
    res = explore(c, cfg, a.max_states, not a.no_inv, not a.all_violations, a.time_limit, a.progress)
    st = res['stats']
    print('states=%d expanded=%d transitions=%d depth=%d exhaustive=%s elapsed=%.1fs' % (
        res['states'], res['expanded'], res['transitions'], res['depth'], res['exhaustive'], res['elapsed']))
    keys_order = ['with_committed', 'with_2_committed', 'with_committed_after_crash', 'with_committed_reader_of_txn',
                  'with_committed_cross_epoch_read', 'after_crash', 'with_aborted_prepared',
                  'with_aborted_prepared_fvw_ready', 'with_doomed_txn', 'with_rollback']
    print('coverage: ' + ', '.join('%s=%d' % (k, st[k]) for k in keys_order))
    print('actions: ' + ', '.join('%s=%d' % (k[4:], v) for k, v in sorted(st.items()) if k.startswith('act_')))
    if a.no_canon:
        NO_CANON[0] = False
        print('distinct states after tick canonicalization: %d' % len(set(canon(x) for x in res['parent'])))
    if not res['violations']:
        print('RESULT: no violation')
        return 0
    for name, (kind, msg, s) in res['violations'].items():
        print('RESULT: VIOLATION %s %s: %s' % (kind, name, msg))
        steps = trace(res['parent'], s)
        print('trace (%d steps):' % len(steps))
        for n, (label, x) in enumerate(steps):
            print(' %2d. %s' % (n + 1, label))
        print(' final state:')
        print(fmt_state(s))
    return 1


if __name__ == '__main__':
    sys.exit(main())
