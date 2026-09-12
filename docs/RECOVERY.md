# Recovering this working tree after a machine reinstall

Written 2026-09-12, before a planned clean reinstall of zoo-003. This file is
committed and pushed on purpose: a recovery plan stored only on the machine
being wiped is no plan at all.

## First: your home directory may not actually be wiped

`/home/users` on zoo-003 is **NFS**, served from `130.245.173.100:/zoohome` —
it is not local disk. Reinstalling this machine's OS does not by itself erase
an NFS export. Before assuming total loss, ask whoever is doing the reinstall
whether `/zoohome` is also being rebuilt. If it is not, everything below
survives untouched.

Do not rely on that. Assume loss and push anyway; treat a surviving home as a
bonus. If the reinstall is a response to a compromise, also consider that files
which survive may not be trustworthy — prefer the pushed git history, which is
content-addressed, over restored working-tree files.

## What IS backed up

Remote `backup` = `git@github.com:yangzeyu1026/raft-rewrite-backup.git`
(private). Verified 2026-09-12 to hold all four branches at these commits:

| branch | commit |
|---|---|
| `codex/raft-vote-site-id` | `3ae8717943dda24ac1bb510ee68ae7240fcd2cc0` |
| `codex/raft-dsl` | `7f52613fe12554b0d01218eae6cb8b2a6bae1815` |
| `codex/raft-memory-only` | `2585a128cde06ddd484e9ef7157087bfc8d10fa2` |
| `codex/raft-stage2-prep` | `f53f3804821631a98d3c146be24720d7bb30e3f8` |

The history is **complete**, not shallow: the clone was unshallowed
(`git fetch --unshallow`) before pushing, so the backup carries full upstream
history rather than a 10-commit boundary. There are no tags and no stashes.

## What is NOT backed up, and what to do about it

### 1. Submodule *contents* — only pointers are pushed

Git records a submodule as a commit id, not its files. After cloning you must
run `git submodule update --init --recursive`, which fetches from these
third-party remotes:

| submodule | url | pinned commit |
|---|---|---|
| `third-party/rusty-cpp` | `https://github.com/shuaimu/rusty-cpp` | `77c3ad5a9ab69190ee361986caf579afa2eae570` |
| `third-party/yaml-cpp` | `https://github.com/jbeder/yaml-cpp/` | `89ff142b991af432b5d7a7cee55282f082a7e629` |
| `third-party/googletest` | `https://github.com/google/googletest.git` | `94be250af7e14c58dcbf476972d2d7141551ff67` |
| `third-party/mako-redis` | `https://github.com/makodb/mako-redis` | `365d9fe9a2a3b073fd64a9ebe83f20afd3a48761` |

**The `rusty-cpp` pin is the fragile one.** It is not on `main` and is not a
tag — it is the tip of the branch `codex/raft-value-init` in someone else's
repository. Verified 2026-09-12: that commit is reachable there both as
`refs/heads/codex/raft-value-init` and as `refs/pull/48/head`. The PR ref makes
it durable against branch deletion, but not against the repository being made
private or removed.

If you want to be safe, mirror it now, while it is still reachable:

```bash
git clone --mirror https://github.com/shuaimu/rusty-cpp /tmp/rusty-cpp.git
# then push that mirror to a private backup repo of your own
```

Note also that `CLAUDE.md` claims the pin is `fa7dd9d9…`; the actual submodule
HEAD and `raft_dsl.sh` both say `77c3ad5a…`. The tree is the truth.

### 2. Build dependencies at `~/.local/mako-deps` (249 MB)

Outside the repository, referenced by no script in it, and therefore not
backed up. Builds need both of these pointing at it, or three test targets fail
to link on `-lrocksdb` / `-llz4`:

```bash
export LD_LIBRARY_PATH=$HOME/.local/mako-deps/usr/lib/x86_64-linux-gnu
export LIBRARY_PATH=$HOME/.local/mako-deps/usr/lib/x86_64-linux-gnu
```

It is a user-prefix unpacking of ~999 library files — the shape you get when
you need system libraries without root. On the rebuilt machine:

- **If you have sudo**, you probably do not need it at all: `./apt_packages.sh`
  installs `librocksdb-dev` and the rest system-wide. Try that first.
- **If you do not have sudo**, you will have to recreate the prefix by
  unpacking the `.deb`s again. There is no script for this in the repo, which
  is the single biggest undocumented step in a rebuild. Consider tarring it up
  before the wipe:
  `tar -czf ~/mako-deps-backup.tar.gz -C ~/.local mako-deps` and copying it off
  the machine.

### 3. Your SSH key

The key that authenticates you to GitHub as `yangzeyu1026` lives in `~/.ssh` on
this machine. After a reinstall you will have a new key and **no access to the
backup repo until you add its public half to GitHub**. Either copy the existing
key off the machine now, or plan to add a new one through the GitHub web UI
(which needs only a browser and your password/2FA, not this machine).

### 4. Performance sweep raw logs

`raft_perf_output/` is gitignored: 2 496 replica log files, ~16 MB. Not backed
up, and not worth backing up — the 664 JSON records that matter are committed
at `docs/performance/raft-baseline-412c225a/records.tar.gz`, and the logs are
only useful for debugging a failed run, of which there were none.

### 5. Docker

`CLAUDE.md` requires tests to run via Docker. Docker is **not installed** on
zoo-003 (`docker info` fails), which is why the harness work ran host-side.
If the rebuilt machine has Docker, prefer the documented Docker path.

## Restore procedure

```bash
# 1. Add your new SSH key to GitHub first, or this fails.
git clone git@github.com:yangzeyu1026/raft-rewrite-backup.git mako
cd mako

# 2. Branches. A clone gives you only the default branch as a local branch;
#    the rest are there as remote-tracking refs.
git branch -a
git checkout codex/raft-vote-site-id

# 3. Submodules (this is the step people forget).
git submodule update --init --recursive

# 4. Re-point at upstream, so you can still pull from the real project.
git remote add origin https://github.com/makodb/mako.git
git remote -v    # backup = your private copy, origin = upstream

# 5. Dependencies: try the system path first.
./apt_packages.sh
#    If rocksdb/lz4 will not link, restore ~/.local/mako-deps and export
#    LD_LIBRARY_PATH and LIBRARY_PATH as above.

# 6. Build.
make -j32

# 7. Sanity-check that the Raft perf baseline still reproduces.
tar -xzf docs/performance/raft-baseline-412c225a/records.tar.gz -C /tmp
python3 scripts/raft_perf/processing.py /tmp/records/rate
```

Note that the clone you get from `backup` is a **normal full clone**, not the
shallow blobless one that was here — pushing required unshallowing it. That is
strictly better; nothing needs undoing.

## Before you wipe: a short checklist

- [ ] `git add -A && git commit` anything uncommitted, then
      `git push backup --all`
- [ ] `git status --porcelain -uall` returns nothing you care about
- [ ] `git ls-remote --heads backup` shows the same commits as `git branch -v`
- [ ] copy `~/.ssh` off the machine, or be ready to enrol a new key
- [ ] `tar -czf ~/mako-deps-backup.tar.gz -C ~/.local mako-deps` and copy it off
- [ ] mirror `shuaimu/rusty-cpp` if you cannot tolerate that pin disappearing
- [ ] anything in `~/jetpack` you rely on — it is a separate read-only tree and
      is **not** part of this repository or this backup
