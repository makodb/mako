#!/bin/bash
set -euo pipefail

# Resolve repo root based on this script's location so it works from anywhere.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"

# Generate configurations
cd "${PROJECT}/bash"
python3 convert_ip.py

cd "${PROJECT}/src/mako/config"
python3 generator.py

cd "${PROJECT}/config/1leader_2followers"
python3 generator.py

cd "${PROJECT}"