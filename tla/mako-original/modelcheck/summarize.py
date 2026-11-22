#!/usr/bin/env python3
"""Summarize mc/results/*.log and mc/results/mut/*.log into two tables."""
import glob, os, re, sys
root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'results')

def parse(f):
    t = open(f).read()
    m = re.search(r'^states=(\d+) expanded=\d+ transitions=(\d+) depth=(\d+) exhaustive=(\w+) elapsed=([\d.]+)s', t, re.M)
    cov = dict(re.findall(r'(\w+)=(\d+)', (re.search(r'^coverage: (.*)$', t, re.M) or [None, ''])[1]))
    res = re.findall(r'^RESULT: (.*)$', t, re.M)
    dc = re.search(r'^distinct states after tick canonicalization: (\d+)', t, re.M)
    return m, cov, res, dc

print('UNMUTATED / ABSTRACTION RUNS')
for f in sorted(glob.glob(os.path.join(root, '*.log'))):
    m, cov, res, dc = parse(f)
    n = os.path.basename(f)[:-4]
    if not m:
        print('%-26s (running or killed)' % n); continue
    print('%-26s states=%-9s trans=%-9s depth=%-3s exhaustive=%-5s %6.0fs committed=%s 2comm=%s comm_after_crash=%s comm_reader=%s xepoch_read=%s abprep_fvw=%s rollback=%s%s :: %s' % (
        n, m.group(1), m.group(2), m.group(3), m.group(4), float(m.group(5)), cov.get('with_committed'),
        cov.get('with_2_committed'), cov.get('with_committed_after_crash'), cov.get('with_committed_reader_of_txn'),
        cov.get('with_committed_cross_epoch_read'), cov.get('with_aborted_prepared_fvw_ready'), cov.get('with_rollback'),
        (' canon_images=%s' % dc.group(1)) if dc else '', ' | '.join(res)))
print()
print('MUTATIONS (I = invariant conjunct, P = durability/atomicity/rollback-safety/ack, S = strictly_serializable)')
rows = {}
for f in sorted(glob.glob(os.path.join(root, 'mut', '*.log'))):
    m, cov, res, dc = parse(f)
    n = os.path.basename(f)[:-4]
    if not m:
        print('%-50s (running)' % n); continue
    tags = []
    for r in res:
        if r.startswith('VIOLATION invariant'):
            tags.append('I:' + r.split()[2].rstrip(':'))
        elif r.startswith('VIOLATION property'):
            tags.append('P:' + r.split()[2].rstrip(':'))
        elif r.startswith('VIOLATION spec'):
            tags.append('S')
    print('%-50s states=%-8s exhaustive=%-5s %s' % (n, m.group(1), m.group(4), ' '.join(tags) or 'NOT CAUGHT'))
