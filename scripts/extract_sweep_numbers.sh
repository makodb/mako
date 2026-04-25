#!/bin/bash
# Extract per-thread-count summary from a single-Raft sweep dir.
# For each t: final replay_p1, mean_us (across tids+followers), aggregated
# peak backlog across all tids and both followers.
#
# Usage: bash extract_sweep_numbers.sh <sweep_dir>

set -e
dir="${1:?usage: $0 <sweep_dir>}"
logs="$dir/logs"
csv="$dir/results.csv"

printf '%s\n' "threads,replay_p1,batches_per_sec,max_peak_backlog_any_tid,sum_peak_backlog_all,mean_us_avg"

for t in 1 2 3 4 5 6 7 8 9 10 11; do
  # committed rate
  row=$(awk -F, -v t=$t 'NR>1 && $1==t {print $10}' "$csv" | head -1)
  [ -z "$row" ] && continue
  bps=$(awk -v r=$row 'BEGIN { printf "%.1f", r/30.0 }')

  # Backlog: from all APPLY-TIMING lines across both followers and all tids,
  # take the LAST line per (follower, tid) and report max + sum of peak_queue.
  max_peak=0
  sum_peak=0
  mean_sum=0
  mean_count=0
  for pid in p1 p2; do
    f="$logs/t${t}_run1_follower_${pid}.log"
    [ -f "$f" ] || continue
    # last line per tid
    while IFS= read -r last; do
      peak=$(echo "$last" | sed -n 's/.*peak_queue=\([0-9]*\).*/\1/p')
      mean=$(echo "$last" | sed -n 's/.*mean_us=\([0-9]*\).*/\1/p')
      [ -n "$peak" ] && sum_peak=$((sum_peak + peak))
      [ -n "$peak" ] && [ "$peak" -gt "$max_peak" ] && max_peak="$peak"
      [ -n "$mean" ] && mean_sum=$((mean_sum + mean)) && mean_count=$((mean_count + 1))
    done < <(grep "APPLY-TIMING" "$f" | awk '
      {
        tid = ""
        for (i=1;i<=NF;i++) if ($i ~ /tid=/) tid = $i
        line[tid] = $0
      }
      END { for (k in line) print line[k] }
    ')
  done
  mean_avg=0
  if [ "$mean_count" -gt 0 ]; then
    mean_avg=$(awk -v s=$mean_sum -v c=$mean_count 'BEGIN {printf "%d", s/c}')
  fi
  printf '%d,%s,%s,%d,%d,%d\n' "$t" "$row" "$bps" "$max_peak" "$sum_peak" "$mean_avg"
done
