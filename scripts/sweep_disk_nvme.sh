#!/bin/bash
# Wrapper: throttled-disk 4-backend sweep modeling a datacenter NVMe SSD.
#
#   Bandwidth: 3000 MB/s   (typical PM9A3 / D7-P5520 sustained write)
#   Latency:   100 us      (typical fsync floor on a healthy NVMe)
#
# Total runtime: ~5 hours.
#
# Usage:
#   nohup bash scripts/sweep_disk_nvme.sh > disk_nvme.log 2>&1 &

set -e
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISK_LABEL=nvme \
MAKO_PERSIST_BW_MBPS=3000 \
MAKO_PERSIST_LATENCY_US=100 \
  bash "$REPO/scripts/sweep_disk_throttled.sh"
