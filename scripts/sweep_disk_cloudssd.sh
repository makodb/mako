#!/bin/bash
# Wrapper: throttled-disk 4-backend sweep modeling a cloud SSD (AWS gp3-class).
#
#   Bandwidth: 1000 MB/s   (gp3 max, io2 baseline)
#   Latency:   1000 us     (1 ms per fsync; typical for cloud block storage)
#
# Total runtime: ~5 hours.
#
# Usage:
#   nohup bash scripts/sweep_disk_cloudssd.sh > disk_cloudssd.log 2>&1 &

set -e
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_LABEL=cloudssd \
MAKO_PERSIST_BW_MBPS=1000 \
MAKO_PERSIST_LATENCY_US=1000 \
  bash "$REPO/scripts/sweep_disk_throttled.sh"
