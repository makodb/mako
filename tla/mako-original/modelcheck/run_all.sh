#!/usr/bin/env bash
# Reproduce every model-checking run reported for the Mako model.
# Usage: mc/run_all.sh [results-dir]   (runs in parallel; ~1-2 h wall on a loaded 64-core box)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
out="${1:-$here/results}"
mkdir -p "$out/mut"
mc="python3 $here/mako_mc.py"
B3='0A1+1A1;0A1;1A1'
B4='0A1;0R+1R;0P1+1A1;1A1'
MAXJ="${MAXJ:-20}"
run() {
  local name="$1"; shift
  while [ "$(jobs -rp | wc -l)" -ge "$MAXJ" ]; do wait -n; done
  $mc "$@" > "$out/$name.log" 2>&1 &
}
# --- unmutated model ---
run s1t1_x1             --shards 1 --txns 1
run s1t1_x2             --shards 1 --txns 2
run s1t2_x2             --shards 1 --threads 2 --txns 2
run s2c2_b3_x2          --shards 2 --comp 2 --txns 2 --bodies "$B3"
run s2c1_b3_x2          --shards 2 --comp 1 --cidx 0,0 --txns 2 --bodies "$B3"
run s1t1_b4_x3          --shards 1 --txns 3 --bodies "$B4"
run s2c2_all_x2_nocrash --shards 2 --comp 2 --txns 2 --crashes 0
run s2c2_b3_x3_nocrash  --shards 2 --comp 2 --txns 3 --bodies "$B3" --crashes 0
run s2c1_b3_x3_nocrash  --shards 2 --comp 1 --cidx 0,0 --txns 3 --bodies "$B3" --crashes 0
run s1t1_x2_crash2       --shards 1 --txns 2 --crashes 2
run s1t2_b2_x3_nocrash  --shards 1 --threads 2 --txns 3 --bodies '0A1;1A1' --crashes 0
# cross-epoch reads (Lemma 8 rule of readable_top) exercised to a commit across shards
run s2c2_b2r_x2          --shards 2 --comp 2 --txns 2 --bodies '0A1;0R' --crashes 1
run s2c1_b2r_x2          --shards 2 --comp 1 --cidx 0,0 --txns 2 --bodies '0A1;0R' --crashes 1
# --- abstraction cross-checks (tick canonicalization, id symmetry cut) ---
run abs_s1t1_x2_nocanon     --shards 1 --txns 2 --no-canon
run abs_s1t1_x2_freeids     --shards 1 --txns 2 --free-ids
run abs_s2c2_b3_x2_nocanon  --shards 2 --comp 2 --txns 2 --bodies "$B3" --no-canon
for m in no_below_wm fetch_no_inc crash_drop_all; do
  run mut/abs_s1t1_x2_nocanon_freeids_$m --shards 1 --txns 2 --no-canon --free-ids --mutation "$m" --all-violations
done
# --- mutations (each must be caught somewhere) ---
for m in no_below_wm crash_keep_all crash_drop_all no_coord_clock fetch_no_inc read_no_fvw no_lock_check \
         inf_all close_ignore_pending rollback_noop crash_keep_all,rollback_noop abort_committed; do
  n=${m/,/+}
  run mut/s1t1_x2_$n    --shards 1 --txns 2 --mutation "$m" --all-violations
  run mut/s2c2_b3_x2_$n --shards 2 --comp 2 --txns 2 --bodies "$B3" --mutation "$m" --all-violations
  run mut/s2c1_b3_x2_$n --shards 2 --comp 1 --cidx 0,0 --txns 2 --bodies "$B3" --mutation "$m" --all-violations
done
for m in no_lock_check read_no_fvw inf_all close_ignore_pending no_coord_clock; do
  run mut/s1t2_x2_$m --shards 1 --threads 2 --txns 2 --mutation "$m" --all-violations
done
run mut/s2c2_b2r_x2_read_no_fvw --shards 2 --comp 2 --txns 2 --bodies '0A1;0R' --crashes 1 --mutation read_no_fvw --all-violations
run mut/s2c1_b2r_x2_read_no_fvw --shards 2 --comp 1 --cidx 0,0 --txns 2 --bodies '0A1;0R' --crashes 1 --mutation read_no_fvw --all-violations
run mut/s1t2_b2_x3_nocrash_no_lock_check --shards 1 --threads 2 --txns 3 --bodies '0A1;1A1' --crashes 0 \
    --mutation no_lock_check --all-violations
wait
# --- the two large runs (about 10-20 GB each), after the batch above ---
run s2c2_all_x2 --shards 2 --comp 2 --txns 2 --crashes 1 --max-states 14000000 --progress
run s2c1_all_x2 --shards 2 --comp 1 --cidx 0,0 --txns 2 --crashes 1 --max-states 14000000 --progress
wait
