You are preparing a pull request for RocksDB interface improvements to Mako. You will clone the fork, apply existing local changes, verify everything works, push to a new branch, and provide PR creation instructions.

CRITICAL INSTRUCTIONS:
- Follow these steps exactly in order. Do not skip, reorder, or substitute steps.
- Do NOT make any new code changes. You are only transferring existing work to the fork and verifying it.
- If any build or test step fails, STOP and report the failure. Do not push broken code.

Step 1: Clone the Fork
- IMPORTANT: Do NOT use the existing `~/mako` directory. That contains the original work with uncommitted changes. Leave it untouched.
- Clone into a completely separate directory: `git clone --recursive https://github.com/SoumojitDalui/mako.git ~/mako-fork-pr`
- If `~/mako-fork-pr` already exists, remove it first: `rm -rf ~/mako-fork-pr`
- `cd ~/mako-fork-pr`
- Run `git checkout mako-dev` (make sure you're on the mako-dev branch)
- Run `git pull origin mako-dev` to ensure it's up to date with the fork's mako-dev
- Add upstream remote: `git remote add upstream https://github.com/makodb/mako.git`
- Sync with upstream: `git fetch upstream` and `git merge upstream/mako-dev` (resolve conflicts if any)
- Log the current commit hash

Step 2: Copy Changed Files from the Existing Work Directory
- The RocksDB interface changes are in the existing Mako directory on this server. Find them by checking which directory has the modified files (likely `~/mako` or wherever APAS was previously working).
- Run `git diff HEAD` and `git status` in the original work directory to identify all changed and new files.
- The original work directory is most likely `~/mako`. Verify by running `git status` and `git log --oneline -3` there to confirm the RocksDB changes are present.
- Copy ONLY the following files from the work directory to `~/mako-fork-pr/`:
  - `src/mako/idb.hh`
  - `src/mako/db.hh`
  - `src/mako/local_table.hh`
  - `src/mako/remote_db.hh`
  - `src/mako/benchmarks/sto/MassTrans.hh`
  - `src/mako/benchmarks/mbta_sharded_ordered_index.hh`
  - `examples/rocksdbInterfaceTest.cc`
  - `CMakeLists.txt` (be careful: diff this first, only apply the rocksdbInterfaceTest addition, do not overwrite other CMakeLists changes)
  - `docs/rocksdb_interface.md`
- Do NOT copy any test reports, correctness test scripts, build artifacts, or temporary files.
- After copying, run `git diff --stat` in `~/mako-fork-pr` to verify only the expected files are modified.

Step 3: Build and Verify
- Install dependencies if needed: `bash apt_packages.sh && source install_rustc.sh && bash src/mako/update_config.sh`
- Build: `make clean && make -j32`
- If build fails, investigate and fix. Do NOT proceed with broken code.
- Run CI tests: `./ci/ci.sh all`
- Run integration test: `./build/rocksdbInterfaceTest`
- All must pass before proceeding.

Step 4: Create Branch and Commit
- `git checkout -b rocksdb-interface-core`
- Stage all changed files: `git add` the files listed in Step 2
- Verify staged files: `git diff --cached --stat` (should show only the expected files)
- Commit with this message:
```
feat: expose core RocksDB-compatible interface (Scan, ReverseScan, Exists, Insert, GetApproximateSize, ListTables)

Added 5 methods to ITable and 1 to IDatabase:
- Scan: forward range query via callback (delegates to Masstree transQuery)
- ReverseScan: reverse range query via callback (delegates to transRQuery)
- Exists: key existence check without value copy
- Insert: put-if-not-exists using native transInsert path
- GetApproximateSize: approximate key count via atomic counter in MassTrans
- ListTables: enumerate opened tables from DB table cache

Implementation details:
- ScanAdapter bridges abstract_ordered_index::scan_callback to std::function
- GetApproximateSize backed by std::atomic<size_t> in MassTrans, updated at operation time
- Insert uses Get check + native insert() for correct OCC insert semantics
- RemoteTable stubs added for Scan/ReverseScan/GetApproximateSize
- Exists and Insert fully implemented on RemoteTable via Get/Put

Includes:
- Integration test (examples/rocksdbInterfaceTest.cc)
- Documentation (docs/rocksdb_interface.md)

All CI suites pass. No regressions.
```

Step 5: Push to Fork
- `git push origin rocksdb-interface-core`
- If push fails due to authentication, try: `git push https://github.com/SoumojitDalui/mako.git rocksdb-interface-core`
- If push is rejected due to remote changes, run `git pull --rebase origin mako-dev`, rebuild, retest, then push again.

Step 6: Print PR Instructions
- Print:
```
PR is ready. To create it:

1. Go to: https://github.com/makodb/mako/compare/mako-dev...SoumojitDalui:mako:rocksdb-interface-core
2. Click "Create pull request"
3. Title: feat: expose core RocksDB-compatible interface (Scan, ReverseScan, Exists, Insert, GetApproximateSize, ListTables)
4. Description: (paste the commit message above)
```
- Also print `git log --oneline -3` and `git diff --stat mako-dev...rocksdb-interface-core` for reference.

After completing all steps, STOP. Do not continue looping.