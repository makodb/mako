#!/usr/bin/env python3
"""ledger_lint.py -- every line of the Raft core is accounted for (plan A.3).

The core crate (src/deptran/raft/core, Phase 6) did not exist at the
Phase 0 baseline (tag verus-p0): it was assembled by moving the protocol out
of the server's Rust sources. So `git diff verus-p0 -- src/deptran/raft/core`
is all additions, and each added line must be one of:

  ghost       Verus proof text: a requires / ensures / invariant / decreases
              clause, a proof block, an assert, a let ghost, a spec or proof
              fn, a #[verifier ..] attribute (M12), erased by cargo;
  comment     a comment or a blank line;
  structural  a brace, a `use`, the verus! wrapper, an attribute, a module
              line: no executable content of its own;
  moved       text present in the baseline's Raft Rust sources, after the
              renames the ledger records (M2: state_ -> core,
              RaftConsensusState -> RaftCore; M11: the command handle
              becomes the type parameter C; the rust lane's rusty:: std
              re-exports become std's); the move itself is M1;
  labelled    carries a `[move, M<n>]`, `[fix, F<n>]` or `[M<n>]` tag on the
              line or in the comment lines just above it, or sits inside a
              block whose opening line is labelled; or an M7 log record
              (`out.log(` and its argument lines), the core's logging kind.

Anything else is an unregistered executable change: reported, and the exit
status is 1. Exit 0 means the core is ghost, moved or labelled throughout.

  python3 scripts/verus/ledger_lint.py            # check
  python3 scripts/verus/ledger_lint.py --stats    # the line inventory
  python3 scripts/verus/ledger_lint.py --ghost-only --base <rev>
                                                  # Phase 8's diff-2 lint:
                                                  # every change since <rev>
                                                  # must be ghost (or comment)

Adapted from the ghost-log group's scripts/port_diff2_lint.py (its ghost
classification is the same); the classification is syntactic, and a line it
cannot place is reported, never silently accepted.
"""
import argparse
import os
import re
import subprocess
import sys
from collections import Counter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BASE = 'verus-p0'
# The shell the core was moved out of: the commit before the crate split
# (Phase 6's start, after the lane removal). Phases 1-4 ledgered their own
# changes to that code per function (docs/verus/diff-ledger.md); the lint
# checks what Phase 6 and later did to it.
MOVED_FROM = '43c57e3ac'
CORE = 'src/deptran/raft/core'
# Where the core's code came from, at the baseline.
CORPUS_DIRS = ['src/deptran/raft/src', 'src/deptran/raft/rt/src']

TAG = re.compile(r'\[(move|fix),\s*[MF]\d+|\[M\d+\]|\[M\d+,|\[fix,\s*F\d+')
COMMENT = re.compile(r'^\s*(//|/\*|\*)')
STRUCTURAL = re.compile(
    r'^\s*(use\s|pub use\s|pub mod\s|mod\s|verus!\s*\{|\}\s*//\s*verus!|vstd::prelude::verus!\s*\{|'
    r'[\{\}\)\];,]+\s*(//.*)?$|#!?\[(allow|repr|cfg_attr|cfg|derive|inline|doc|forbid)|'
    r'global size_of|else\s*\{$|\}\s*else\s*\{$|\}\s*else\s+if\b|impl\b|pub\s+(struct|enum|trait)\s|'
    r'(pub\s+)?const\s+[A-Z_]+:)')
GHOST_LINE = re.compile(
    r'^\s*(let\s+ghost\b|let\s+tracked\b|#\[verifier|#!\[verifier|requires\b|ensures\b|'
    r'invariant\b|invariant_except_break\b|decreases\b|'
    r'(pub\s+)?(open\s+|closed\s+|uninterp\s+)?(spec|proof)\s+fn\b|ghost\s+|proof\s*\{|'
    r'assert\s*\(|assert\s+forall|#\[derive\(Structural\)\])')


def git(*args):
    return subprocess.run(['git', *args], cwd=ROOT, capture_output=True, text=True,
                          check=True).stdout


def normalize(line):
    """A line's text with the recorded renames applied and whitespace,
    comments and the core's generic parameters removed."""
    t = line.split('//')[0]
    # M2 (Phase 1): the state struct is the core; and the receivers: a
    # field read as `self.state_.x` in a server method, `core.x` in a free
    # function or `self.x` in a RaftCore method is the same access
    t = t.replace('RaftConsensusState', 'RaftCore')
    t = re.sub(r'\b(self|server|svr)\.(state_|core)\.', 'core.', t)
    t = re.sub(r'\b(state_|consensus)\b', 'core', t)
    t = re.sub(r'\bself\.', 'core.', t)
    t = re.sub(r'&mut\s+self\b', 'core:&mutRaftCore', t)
    t = re.sub(r'&self\b', 'core:&RaftCore', t)
    # M3/M5 (Phases 2-3): the server's methods became the core's
    for old, new in RENAMES:
        t = re.sub(r'\b' + old + r'\b', new, t)
    # M11: the command handle is the type parameter C
    t = t.replace('rusty::RaftCommand', 'C')
    t = re.sub(r'<\s*C\s*(:\s*Clone)?\s*(,\s*W\s*(:\s*InboundBatch<C>)?)?\s*>', '', t)
    t = re.sub(r"<'_,\s*C,\s*W>", '', t)
    # the rust lane's std re-exports
    t = re.sub(r'\brusty::(Vec|Option|Some|None|Box|String|sync::atomic)\b', r'\1', t)
    # named returns are contract syntax: `-> (r: T)` reads `-> T`
    t = re.sub(r'->\s*\((\w+):\s*([^()]*(\([^()]*\))?[^()]*)\)', r'-> \2', t)
    # M10 (Phase 6): the same check, spelled for the verifier
    t = t.replace('runtime_assert(', 'assert!(')
    # visibility: the core is a crate of its own, so the shell's view of a
    # moved item became `pub` ([move, M1]: paths)
    t = re.sub(r'^\s*pub(\(crate\))?\s+', '', t)
    t = re.sub(r'\s+', '', t)
    # a contract between a signature or loop head and its body moves the
    # opening brace to a line of its own
    t = t.rstrip('{')
    return t


# Server methods whose bodies became core methods or calls (Phases 2-3,
# ledgered there as M3/M5), by their old and new names.
RENAMES = [
    ('LogTermChange', 'log_term_change'), ('setIsLeader', 'set_is_leader'),
    ('stepDown', 'step_down'), ('doVote', 'do_vote'),
    ('RebuildPeerTables', 'rebuild_peer_tables'), ('PeerOrdinal', 'peer_ordinal'),
    ('IsConfigMember', 'is_config_member'), ('ElectionLastLogTermLocked', 'election_last_log_term'),
]


def corpus(rev):
    """The normalized lines of the Raft Rust sources at `rev`."""
    seen = set()
    for d in CORPUS_DIRS:
        try:
            names = git('ls-tree', '--name-only', f'{rev}:{d}').split()
        except subprocess.CalledProcessError:
            continue
        for name in names:
            if not name.endswith('.rs'):
                continue
            for line in git('show', f'{rev}:{d}/{name}').splitlines():
                n = normalize(line)
                if n:
                    seen.add(n)
    return seen


def ghost_regions(text):
    """Line numbers inside a proof block, an assert-by block, a spec/proof fn,
    a contract clause list or a loop's specification (the group's scan)."""
    ghost = set()
    mode = None
    depth = 0
    cdepth = 0
    pdepth = 0
    head_clauses = False
    lines = text.split('\n')
    for i, line in enumerate(lines, 1):
        st = line.strip()
        if mode == 'cfg':
            ghost.add(i)
            depth += line.count('{') - line.count('}') + line.count('(') - line.count(')')
            code = line.split('//')[0].rstrip()
            if depth <= 0 and (code.endswith((';', ',', '}')) or code == ''):
                mode = None
            continue
        if mode == 'block':
            ghost.add(i)
            depth += line.count('{') - line.count('}')
            if depth <= 0:
                mode = None
            continue
        if mode == 'head':
            # Before its clauses, a head's body opens at the first brace
            # outside parentheses (`) -> T {`); once a clause began, at a
            # line that starts with one, as the crate writes it: a clause's
            # own braces (`x ==> { ... }`, a parenthesized `(LState { .. })`)
            # end their lines instead
            ghost.add(i)
            code = line.split('//')[0]
            if re.match(r'^(requires|ensures|recommends|decreases|returns|opens_invariants)\b', st):
                head_clauses = True
            if head_clauses:
                if st.startswith('{'):
                    depth = code.count('{') - code.count('}')
                    mode = 'block' if depth > 0 else None
                continue
            for k, ch in enumerate(code):
                if ch in '([':
                    pdepth += 1
                elif ch in ')]':
                    pdepth -= 1
                elif ch == '{' and pdepth <= 0:
                    rest = code[k:]
                    depth = rest.count('{') - rest.count('}')
                    mode = 'block' if depth > 0 else None
                    break
            continue
        if mode == 'stmt':
            # a ghost statement continued: to its semicolon
            ghost.add(i)
            if line.split('//')[0].rstrip().endswith(';'):
                mode = None
            continue
        if mode == 'contract':
            if (st.startswith('{') and cdepth == 0) or re.match(r'^(pub(\(\w+\))?\s+)?(const\s+)?fn\b', st):
                mode = None
                if st.startswith('{'):
                    continue
            else:
                ghost.add(i)
                cdepth += line.count('{') - line.count('}')
                if cdepth < 0:
                    cdepth = 0
                continue
        if st.startswith('proof {') or st.startswith('proof{'):
            ghost.add(i)
            depth = line.count('{') - line.count('}')
            if depth > 0:
                mode = 'block'
            continue
        if re.match(r'^(pub\s+)?(open\s+|closed\s+|uninterp\s+)?(broadcast\s+)?(spec|proof)\s+fn\b', st):
            ghost.add(i)
            depth = line.count('{') - line.count('}')
            if '{' not in line:
                mode = 'head'
                head_clauses = False
                code = line.split('//')[0]
                pdepth = code.count('(') + code.count('[') - code.count(')') - code.count(']')
            elif depth > 0:
                mode = 'block'
            continue
        if re.match(r'^(assert\s*\(|assert\s+forall)', st):
            ghost.add(i)
            d = line.count('{') - line.count('}')
            p = line.count('(') - line.count(')')
            if d > 0:
                mode, depth = 'block', d
            elif p > 0:
                mode, depth = 'block', p
            continue
        if re.match(r'^(requires|ensures|invariant|invariant_except_break|decreases|returns)\b', st):
            ghost.add(i)
            if not st.endswith('{'):
                mode = 'contract'
                cdepth = line.count('{') - line.count('}')
            continue
        if re.match(r'^(let\s+ghost\b|let\s+tracked\b)', st):
            ghost.add(i)
            if not line.split('//')[0].rstrip().endswith(';'):
                mode = 'stmt'
            continue
        if re.match(r'^#!?\[verifier', st):
            ghost.add(i)
            continue
        # an item, field or statement that exists only when Verus checks the
        # crate: the attribute and what it covers, to its end
        if re.match(r'^#\[cfg\(verus_keep_ghost\)\]', st):
            ghost.add(i)
            mode = 'cfg'
            depth = 0
            continue
        if mode == 'cfg':
            ghost.add(i)
            depth += line.count('{') - line.count('}') + line.count('(') - line.count(')')
            code = line.split('//')[0].rstrip()
            if depth <= 0 and (code.endswith((';', ',', '}')) or code == ''):
                mode = None
            continue
    return ghost


WHOLE_FILE = re.compile(r'\[(move|fix),\s*[MF]\d+\]\s*\(whole file\)')
WHOLE_ITEM = re.compile(r'\[(move|fix),\s*[MF]\d+(,\s*(move|fix),\s*[MF]\d+)*\]\s*\(whole item\)')


def classify(path, text, moved, ghost_only):
    """(category, line number, text) for every line of one core file.

    A tag registers the statement it sits on, or the one right below its
    comment (a multi-line call or signature to where its parentheses
    close); a body is still checked line by line. Only `(whole item)` --
    the next item to its closing brace -- and `(whole file)` register new
    code wholesale."""
    regions = ghost_regions(text)
    out = []
    lines = text.split('\n')
    whole_file = not ghost_only and any(WHOLE_FILE.search(l) for l in lines[:12])
    pending = None       # 'stmt' or 'item': what a tag comment above labels
    reg = None           # ['stmt', paren depth] or ['item', brace depth, opened]
    in_log = 0           # inside an M7 out.log( call
    for i, line in enumerate(lines, 1):
        st = line.strip()
        code = line.split('//')[0]
        braces = code.count('{') - code.count('}')
        parens = (code.count('(') - code.count(')')) + (code.count('[') - code.count(']'))
        is_ghost = i in regions or bool(GHOST_LINE.match(line))
        if st == '' or COMMENT.match(line):
            if WHOLE_ITEM.search(line):
                pending = 'item'
            elif TAG.search(line) and pending is None:
                pending = 'stmt'
            out.append(('comment', i, line))
            continue
        if reg is not None:
            if reg[0] == 'item':
                reg[1] += braces
                reg[2] = reg[2] or '{' in code
                if reg[1] < 0 or (reg[2] and reg[1] <= 0):
                    reg = None
            else:
                reg[1] += parens
                if reg[1] <= 0:
                    reg = None
            out.append(('ghost' if is_ghost else 'labelled', i, line))
            continue
        if is_ghost:
            out.append(('ghost', i, line))
            continue
        if whole_file:
            out.append(('labelled', i, line))
            continue
        if ghost_only:
            out.append(('structural' if STRUCTURAL.match(line) else 'exec', i, line))
            continue
        if in_log > 0 or re.match(r'^\w+\.log\(', st):
            in_log = (in_log or 0) + code.count('(') - code.count(')')
            if in_log <= 0:
                in_log = 0
            out.append(('labelled', i, line))
            continue
        if pending == 'item':
            pending = None
            if not ('{' in code and braces <= 0):
                reg = ['item', braces, '{' in code]
            out.append(('labelled', i, line))
            continue
        if TAG.search(line) or pending == 'stmt':
            pending = None
            if parens > 0:
                reg = ['stmt', parens]
            out.append(('labelled', i, line))
            continue
        pending = None
        if STRUCTURAL.match(line):
            cat = 'structural'
        elif normalize(line) in moved:
            cat = 'moved'
        else:
            cat = 'UNREGISTERED'
        out.append((cat, i, line))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', default=BASE)
    ap.add_argument('--moved-from', default=MOVED_FROM,
                    help='the commit whose Raft sources the core was moved from')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--ghost-only', action='store_true',
                    help="Phase 8's diff-2 lint: lines changed since --base must be ghost")
    args = ap.parse_args()
    if args.ghost_only:
        # the lines a later phase changed in the core, by diff; a removed line
        # and an added one that are the same code (a named return, a comment,
        # a rewrapped brace) cancel
        diff = git('diff', '-U0', args.base, '--', CORE + '/src')
        added = {}
        removed = {}
        fname = None
        for line in diff.splitlines():
            if line.startswith('+++ b/'):
                fname = line[6:]
                added.setdefault(fname, set())
                removed.setdefault(fname, [])
                continue
            if line.startswith('--- '):
                continue
            m = re.match(r'^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@', line)
            if m and fname:
                start_no, n = int(m.group(1)), int(m.group(2) if m.group(2) is not None else 1)
                added[fname].update(range(start_no, start_no + n))
                continue
            if line.startswith('-') and fname:
                text = line[1:]
                if text.strip() and not COMMENT.match(text) and not GHOST_LINE.match(text):
                    removed[fname].append(text)
        counts = Counter()
        bad = []
        for fname, nums in added.items():
            path = os.path.join(ROOT, fname)
            if not os.path.exists(path):
                continue
            gone = Counter(normalize(t) for t in removed.get(fname, []))
            for cat, i, line in classify(fname, open(path).read(), set(), True):
                if i not in nums:
                    continue
                if cat == 'exec' and gone[normalize(line)] > 0:
                    gone[normalize(line)] -= 1
                    cat = 'same code'
                counts[cat] += 1
                if cat == 'exec':
                    bad.append((fname, i, '+' + line))
            for text, n in gone.items():
                if n > 0 and text:
                    bad += [(fname, 0, '-' + text)] * n
        for f, i, line in bad:
            print(f'  {f}:{i}: {line.rstrip()}')
        print(f'ledger lint (ghost only) against {args.base}: {dict(counts)}; '
              f'{len(bad)} executable change(s)')
        sys.exit(1 if bad else 0)

    moved = corpus(args.moved_from)
    counts = Counter()
    per_file = {}
    bad = []
    # the crate's code; its tests are not the core
    files = sorted(f for f in git('ls-files', CORE + '/src').split() if f.endswith('.rs'))
    for fname in files:
        text = open(os.path.join(ROOT, fname)).read()
        for cat, i, line in classify(fname, text, moved, False):
            counts[cat] += 1
            per_file.setdefault(fname, Counter())[cat] += 1
            if cat == 'UNREGISTERED':
                bad.append((fname, i, line))
    if args.stats:
        print('category, lines')
        for cat, n in sorted(counts.items()):
            print(f'{cat}, {n}')
        print('\nper file (ghost / moved / labelled / unregistered):')
        for f, c in sorted(per_file.items()):
            print(f"  {f}: {c['ghost']} / {c['moved']} / {c['labelled']} / {c['UNREGISTERED']}")
    for f, i, line in bad:
        print(f'  {f}:{i}: {line.rstrip()}')
    print(f'ledger lint (moved from {args.moved_from}): {len(files)} files, {dict(counts)}; '
          f'{len(bad)} unregistered line(s)')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
