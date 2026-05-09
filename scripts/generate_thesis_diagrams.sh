#!/bin/bash
# Generate thesis architecture/mechanism diagrams from Graphviz DOT sources.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

SRC_DIR="${SRC_DIR:-doc/thesis/figures/diagrams/src}"
OUT_DIR="${OUT_DIR:-doc/thesis/figures/diagrams/generated}"

if ! command -v dot >/dev/null 2>&1; then
    echo "ERROR: Graphviz 'dot' is not installed or not on PATH." >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

for src in "$SRC_DIR"/*.dot; do
    [ -e "$src" ] || continue
    base="$(basename "$src" .dot)"
    dot -Tsvg "$src" -o "$OUT_DIR/$base.svg"
    dot -Tpdf "$src" -o "$OUT_DIR/$base.pdf"
    echo "wrote $OUT_DIR/$base.svg"
    echo "wrote $OUT_DIR/$base.pdf"
done
