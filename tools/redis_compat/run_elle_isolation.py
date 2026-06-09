#!/usr/bin/env python3
from __future__ import annotations

import os
from pathlib import Path

from harness_common import main_guard, na


def main() -> None:
    elle_jar = os.environ.get("ELLE_JAR", "tools/redis_compat/elle.jar")
    if not Path(elle_jar).exists():
        na(f"missing Elle jar at {elle_jar}")
    history = os.environ.get("MAKO_G4_HISTORY")
    if not history:
        na("missing MAKO_G4_HISTORY history file for Elle analysis")
    na("Elle jar/history hooks are present, but the G4 workload generator is not wired in this checkout")


if __name__ == "__main__":
    main_guard(main)
