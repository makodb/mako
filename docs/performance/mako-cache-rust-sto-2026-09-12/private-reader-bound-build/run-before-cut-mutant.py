#!/usr/bin/env python3
"""Run one source-isolated negative control against the private bounded fence."""

import hashlib
import importlib.util
import os
from pathlib import Path
import sys

if os.environ.get("RUSTUP_TOOLCHAIN") != "1.95.0":
    raise SystemExit("this diagnostic requires explicit Rust 1.95.0")

runner_path = Path("/home/users/shuai/mako/.claude/worktrees/masstree-rocks/.mako-read-bound-crates/mako-cache/mutations/run.py")
spec = importlib.util.spec_from_file_location("bounded_cache_mutations", runner_path)
runner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = runner
spec.loader.exec_module(runner)

anchor = """    fn read_only_scan_bound(&self, order_validation_prefix: impl FnOnce()) -> usize {
        order_validation_prefix();
        // A prefix writer registered its slot before its packed Release RMW.
        // The cut acquires that publication or its release sequence. Loading
        // the monotone registration counter AFTER that cut therefore includes
        // every prefix writer, even though registration itself was Relaxed.
        // An Acquire load before the cut would not establish this guarantee.
        // Slots are never reused and this counter never decreases or resets.
        NEXT_COMMIT_FENCE_THREAD_SLOT.load(Ordering::Acquire)
    }
"""
replacement = """    fn read_only_scan_bound(&self, order_validation_prefix: impl FnOnce()) -> usize {
        // MUTANT: an early bound can omit a writer included by the later cut.
        let registered = NEXT_COMMIT_FENCE_THREAD_SLOT.load(Ordering::Acquire);
        order_validation_prefix();
        registered
    }
"""
runner.MUTATIONS = (
    runner.Mutation(
        name="read-only-registration-bound-before-cut",
        relative_path="src/lib.rs",
        anchor=anchor,
        anchor_sha256=hashlib.sha256(anchor.encode()).hexdigest(),
        replacement=replacement,
        test=runner.TestTarget(
            ("--lib",),
            "tests::commit_fence_scan_bound_includes_registration_completed_inside_cut",
        ),
        breaks="A prefix writer registered during the cut is omitted from the reader's scan.",
    ),
)
raise SystemExit(runner.main())
