#!/usr/bin/env python3
from __future__ import annotations

from harness_common import main_guard, na, require_env


def main() -> None:
    require_env("MAKO_G3_START_CMD")
    require_env("MAKO_G3_KILL_CMD")
    require_env("MAKO_G3_RECOVER_CMD")
    na("G3 command hooks are present, but replicated makoCon fault workflow is not wired in this checkout")


if __name__ == "__main__":
    main_guard(main)
