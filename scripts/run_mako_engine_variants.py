#!/usr/bin/env python3
"""Compare private Rust STO variants using the engine_compare workload."""

import argparse
from pathlib import Path
import re

from run_mako_engine_compare import add_measurement_arguments, run_comparison


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", action="append", required=True, metavar="NAME=PATH")
    add_measurement_arguments(parser)
    args = parser.parse_args()
    binaries = {}
    for variant in args.variant:
        name, separator, path = variant.partition("=")
        if not separator or not re.fullmatch(r"[a-z0-9_-]+", name) or name in binaries:
            parser.error("variants need unique lowercase NAME=PATH pairs")
        binaries[name] = Path(path).resolve(strict=True)
    if len(binaries) < 2:
        parser.error("need at least two Rust STO variants")
    # C ABI measurements use the explicit --c-abi/--sto pair driver, never a
    # Rust variant label. The shared validator enforces the backend per sample.
    run_comparison(parser, args, binaries, {name: "sto" for name in binaries}, "variant")


if __name__ == "__main__":
    main()
