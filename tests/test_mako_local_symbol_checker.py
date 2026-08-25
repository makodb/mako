#!/usr/bin/env python3
"""Focused positive and negative tests for check_mako_local_symbols.py."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHECKER_PATH = ROOT / "scripts" / "check_mako_local_symbols.py"
SPEC = importlib.util.spec_from_file_location("check_mako_local_symbols", CHECKER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load {CHECKER_PATH}")
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)

FINGERPRINT = "a" * 64
ANCHOR = CHECKER.BUILD_ANCHOR_PREFIX + FINGERPRINT
PUBLIC = ["mako_local_alpha", "mako_local_beta"]


class ParseNmOutputTests(unittest.TestCase):
    def test_normalizes_macho_and_ignores_mangled_names(self) -> None:
        output = """
libmako.a[one.o]:
mako_local_alpha T 0 8
_mako_local_beta T 8 8
_ZN3Foo16mako_local_tableEv W 0 4
mako_local_missing U
"""
        self.assertEqual(
            CHECKER.parse_nm_output(output),
            {
                "mako_local_alpha": ["T"],
                "mako_local_beta": ["T"],
                "mako_local_missing": ["U"],
            },
        )


class ValidateSymbolsTests(unittest.TestCase):
    def good_definitions(self) -> dict[str, list[str]]:
        return {
            "mako_local_alpha": ["T"],
            "mako_local_beta": ["T"],
            ANCHOR: ["T"],
        }

    def test_accepts_exact_surface(self) -> None:
        self.assertEqual(
            CHECKER.validate_symbols(self.good_definitions(), PUBLIC, FINGERPRINT),
            [],
        )

    def test_rejects_missing_public_symbol(self) -> None:
        definitions = self.good_definitions()
        del definitions["mako_local_beta"]
        errors = CHECKER.validate_symbols(definitions, PUBLIC, FINGERPRINT)
        self.assertTrue(any("missing symbols: mako_local_beta" in error for error in errors))

    def test_rejects_unexpected_public_symbol(self) -> None:
        definitions = self.good_definitions()
        definitions["mako_local_gamma"] = ["T"]
        errors = CHECKER.validate_symbols(definitions, PUBLIC, FINGERPRINT)
        self.assertTrue(
            any("unexpected symbols: mako_local_gamma" in error for error in errors)
        )

    def test_rejects_weak_and_duplicate_definitions(self) -> None:
        definitions = self.good_definitions()
        definitions["mako_local_alpha"] = ["W"]
        definitions["mako_local_beta"] = ["T", "T"]
        errors = CHECKER.validate_symbols(definitions, PUBLIC, FINGERPRINT)
        self.assertTrue(any("found: W" in error for error in errors))
        self.assertTrue(any("found: T, T" in error for error in errors))

    def test_rejects_anchor_for_a_different_manifest(self) -> None:
        definitions = self.good_definitions()
        del definitions[ANCHOR]
        definitions[CHECKER.BUILD_ANCHOR_PREFIX + "b" * 64] = ["T"]
        errors = CHECKER.validate_symbols(definitions, PUBLIC, FINGERPRINT)
        self.assertTrue(any("missing symbols" in error for error in errors))
        self.assertTrue(any("unexpected symbols" in error for error in errors))
        self.assertTrue(any("manifest-selected build anchor" in error for error in errors))


class InputFileTests(unittest.TestCase):
    def test_loads_sorted_allowlist_and_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            allowlist = root / "symbols.txt"
            manifest = root / "manifest.json"
            allowlist.write_text(
                "# public ABI\nmako_local_alpha\nmako_local_beta\n",
                encoding="utf-8",
            )
            manifest.write_text(
                json.dumps({"fingerprint": FINGERPRINT}), encoding="utf-8"
            )
            self.assertEqual(CHECKER.load_allowlist(allowlist), PUBLIC)
            self.assertEqual(CHECKER.load_fingerprint(manifest), FINGERPRINT)

    def test_rejects_unsorted_or_anchor_allowlists(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "symbols.txt"
            path.write_text(
                "mako_local_beta\nmako_local_alpha\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(CHECKER.GateError, "sorted"):
                CHECKER.load_allowlist(path)

            path.write_text(ANCHOR + "\n", encoding="utf-8")
            with self.assertRaisesRegex(CHECKER.GateError, "must not be allowlisted"):
                CHECKER.load_allowlist(path)

    def test_rejects_noncanonical_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps({"fingerprint": "A" * 64}), encoding="utf-8")
            with self.assertRaisesRegex(CHECKER.GateError, "lowercase hexadecimal"):
                CHECKER.load_fingerprint(path)


if __name__ == "__main__":
    unittest.main()
