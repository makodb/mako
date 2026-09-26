#!/usr/bin/env python3
"""Reconstruct the leader's poll-thread timeline from an strace -f -tt -T trace."""
import re, sys, collections, statistics as st
line_re = re.compile(r'^(\d+) (\d\d):(\d\d):(\d\d)\.(\d{6}) (.*)$')
call_re = re.compile(r'^(\w+)\((.*)\) += (-?\d+|\?)(?: [A-Z]+ \(.*\))? <(\d+\.\d+)>$')
unf_re = re.compile(r'^(\w+)\((.*) <unfinished \.\.\.>$')
res_re = re.compile(r'^<\.\.\. (\w+) resumed>(.*) = (-?\d+|\?)(?: [A-Z]+ \(.*\))? <(\d+\.\d+)>$')
WRITES = {'write', 'sendto', 'sendmsg', 'writev'}
READS = {'read', 'recvfrom', 'recvmsg', 'readv'}

def parse(path):
    ev, pend = [], {}
    for line in open(path, errors='replace'):
        m = line_re.match(line.rstrip('\n'))
        if not m: continue
        tid = int(m.group(1)); t = int(m.group(2))*3600 + int(m.group(3))*60 + int(m.group(4)) + int(m.group(5))/1e6
        rest = m.group(6)
        mc = call_re.match(rest)
        if mc:
            ev.append((t, t+float(mc.group(4)), tid, mc.group(1), mc.group(2), mc.group(3))); continue
        mu = unf_re.match(rest)
        if mu:
            pend[tid] = (t, mu.group(1), mu.group(2)); continue
        mr = res_re.match(rest)
        if mr and tid in pend:
            t0, name, args = pend.pop(tid)
            ev.append((t0, t0+float(mr.group(4)), tid, name, args+mr.group(2), mr.group(3)))
    ev.sort(); return ev

def fd_of(args):
    m = re.match(r'\s*(\d+)', args); return int(m.group(1)) if m else -1
def pct(xs, p):
    if not xs: return float('nan')
    xs = sorted(xs); return xs[min(len(xs)-1, int(round(p/100*(len(xs)-1))))]
def us(x): return '%6.0f' % (x*1e6)
def summ(label, xs):
    if not xs: print('  %-46s (none)' % label); return
    print('  %-46s n=%6d  p50 %s  p90 %s  p99 %s  max %s us' % (label, len(xs), us(pct(xs,50)), us(pct(xs,90)), us(pct(xs,99)), us(max(xs))))

def analyze(path):
    ev = parse(path)
    print('=== %s: %d syscalls ===' % (path.split('/')[-1], len(ev)))
    by_tid = collections.defaultdict(collections.Counter)
    for t0, t1, tid, name, args, ret in ev: by_tid[tid][name] += 1
    ep = [e for e in ev if e[3] == 'epoll_wait']
    poll_tid = collections.Counter(e[2] for e in ep).most_common(1)[0][0]
    print('  threads:', ' | '.join('%d:%s' % (tid, ','.join('%s=%d' % kv for kv in c.most_common(3))) for tid, c in sorted(by_tid.items())))
    t_start = ep[0][0] + 4.0; t_end = t_start + 8.0     # steady state: skip election + warmup
    win = [e for e in ev if t_start <= e[0] < t_end]
    # fd roles
    client_fds, accepted = set(), set()
    for t0, t1, tid, name, args, ret in ev:
        if name == 'connect': client_fds.add(fd_of(args))
        if name in ('accept4', 'accept') and ret not in ('-1', '?'): accepted.add(int(ret))
    print('  client (outbound) fds: %s   accepted fds: %s' % (sorted(client_fds), sorted(accepted)))
    # poll thread loop shape
    pe = [e for e in win if e[2] == poll_tid and e[3] == 'epoll_wait']
    durs = [e[1]-e[0] for e in pe]; timeouts = sum(1 for e in pe if e[5] == '0')
    print('  poll thread %d: %d epoll_wait in 8 s (%.0f/s), %d (%.0f%%) returned by timeout' % (poll_tid, len(pe), len(pe)/8, timeouts, 100*timeouts/max(1,len(pe))))
    summ('epoll_wait duration', durs)
    gaps = [pe[i+1][0] - pe[i][1] for i in range(len(pe)-1)]
    summ('work between epoll_waits (loop body)', gaps)
    # who writes/reads the sockets
    w = [e for e in win if e[3] in WRITES and fd_of(e[4]) in client_fds]
    r = [e for e in win if e[3] in READS and fd_of(e[4]) in client_fds and e[5] not in ('-1','?','0')]
    print('  writes on client fds: %d (%.0f/s) by tids %s; bytes p50 %s' % (len(w), len(w)/8, sorted(set(e[2] for e in w)), pct([int(e[5]) for e in w if e[5] not in ('-1','?')],50) if w else '-'))
    print('  reads  on client fds: %d (%.0f/s) by tids %s' % (len(r), len(r)/8, sorted(set(e[2] for e in r))))
    # per-fd round trips: first write after quiet -> first non-empty read
    rtts, rounds = [], []
    for fd in sorted(client_fds):
        seq = sorted([e for e in win if fd_of(e[4]) == fd and (e[3] in WRITES or (e[3] in READS and e[5] not in ('-1','?','0')))])
        outstanding = None; last_round = None
        for t0, t1, tid, name, args, ret in seq:
            if name in WRITES:
                if outstanding is None:
                    outstanding = t0
                    if last_round is not None: rounds.append(t0 - last_round)
                    last_round = t0
            elif outstanding is not None:
                rtts.append(t1 - outstanding); outstanding = None
    summ('RPC round trip on a follower fd (write->reply read)', rtts)
    summ('interval between rounds on a follower fd', rounds)
    # reply arrival -> how long until the poll thread's epoll_wait had returned before that read (wake latency of the reply)
    sleeps = [e for e in win if e[3] in ('nanosleep', 'clock_nanosleep')]
    by = collections.Counter(e[2] for e in sleeps)
    print('  sleeps: ' + ', '.join('tid %d: %d (%.0f/s, mean %.0f us)' % (tid, n, n/8, 1e6*st.mean([e[1]-e[0] for e in sleeps if e[2]==tid])) for tid, n in by.most_common(4)))
    # writes issued by the poll thread: delay from the epoll_wait that preceded them
    pw = [e for e in w if e[2] == poll_tid]
    lat = []
    j = 0
    for e in pw:
        while j < len(pe) and pe[j][1] <= e[0]: j += 1
        if j > 0: lat.append(e[0] - pe[j-1][1])
    summ('epoll_wait return -> AppendEntries write', lat)
    # reads by the poll thread: time from the epoll_wait return that delivered them
    pr = [e for e in r if e[2] == poll_tid]
    lat2 = []; j = 0
    for e in pr:
        while j < len(pe) and pe[j][1] <= e[0]: j += 1
        if j > 0: lat2.append(e[0] - pe[j-1][1])
    summ('epoll_wait return -> reply read', lat2)


def analyze_follower(path):
    ev = parse(path)
    print('=== FOLLOWER %s: %d syscalls ===' % (path.split('/')[-1], len(ev)))
    ep = [e for e in ev if e[3] == 'epoll_wait']
    poll_tid = collections.Counter(e[2] for e in ep).most_common(1)[0][0]
    t_start = ep[0][0] + 4.0; t_end = t_start + 8.0
    win = [e for e in ev if t_start <= e[0] < t_end]
    accepted = set(int(e[5]) for e in ev if e[3] in ('accept4','accept') and e[5] not in ('-1','?'))
    pe = [e for e in win if e[2] == poll_tid and e[3] == 'epoll_wait']
    print('  poll thread %d: %d epoll_wait in 8 s, %d%% by timeout; accepted fds %s' % (poll_tid, len(pe), 100*sum(1 for e in pe if e[5]=='0')//max(1,len(pe)), sorted(accepted)))
    summ('epoll_wait duration', [e[1]-e[0] for e in pe])
    summ('work between epoll_waits (loop body)', [pe[i+1][0]-pe[i][1] for i in range(len(pe)-1)])
    svc, same_iter, deferred = [], 0, 0
    for fd in sorted(accepted):
        seq = sorted([e for e in win if fd_of(e[4]) == fd and ((e[3] in READS and e[5] not in ('-1','?','0')) or e[3] in WRITES)])
        pending = None
        for t0, t1, tid, name, args, ret in seq:
            if name in READS:
                if pending is None: pending = t1
            elif pending is not None:
                svc.append(t0 - pending)
                # was there an epoll_wait between the request read and the reply write?
                if any(pending <= x[0] < t0 for x in pe): deferred += 1
                else: same_iter += 1
                pending = None
    summ('request read -> reply write (service time)', svc)
    print('  replies written in the same poll iteration as the read: %d; after another epoll_wait: %d' % (same_iter, deferred))
    rd = [e for e in win if e[3] in READS and fd_of(e[4]) in accepted and e[5] not in ('-1','?','0')]
    print('  request reads: %d (%.0f/s), bytes p50 %s' % (len(rd), len(rd)/8, pct([int(e[5]) for e in rd],50) if rd else '-'))
    sleeps = [e for e in win if e[3] in ('nanosleep','clock_nanosleep')]
    by = collections.Counter(e[2] for e in sleeps)
    print('  sleeps: ' + ', '.join('tid %d: %d (%.0f/s, mean %.0f us)' % (tid, n, n/8, 1e6*st.mean([e[1]-e[0] for e in sleeps if e[2]==tid])) for tid, n in by.most_common(3)))

for p in sys.argv[1:]:
    (analyze_follower if p.endswith('.p1') else analyze)(p); print()

