#!/usr/bin/env python3
"""Fast structural checks for the standalone Goal-0 repository."""

from __future__ import annotations

from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import tomllib
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUSTY_CPP_PIN = "fa7dd9d9612c0bcec695c3e391ace96b56498e74"
# Every production module is now a canonical Rust provider: the inline
# carrier inventory is EMPTY, and the negative controls below are what
# keep an empty inventory from becoming a vacuous check.
EXPECTED_INLINE_SOURCES: dict[str, str] = {}
EXPECTED_DSL_SOURCES = {
    *EXPECTED_INLINE_SOURCES,
    "reactor/epoll_platform_linux.cc",
}


def cmake_set(text: str, name: str) -> list[str]:
    match = re.search(rf"set\({re.escape(name)}\s+(.*?)\s*\)", text, re.DOTALL)
    if match is None:
        raise AssertionError(f"missing CMake set({name})")
    return [
        token
        for token in re.split(r"\s+", match.group(1).strip())
        if token and not token.startswith("#")
    ]


def source_token(token: str) -> str:
    prefix = "${CMAKE_CURRENT_SOURCE_DIR}/"
    if not token.startswith(prefix):
        raise AssertionError(f"non-repository-relative source token: {token}")
    return token.removeprefix(prefix)


def validate_provider_inventory(cmake: str, manifest: dict[str, object]) -> None:
    modules = manifest["module"]
    assert isinstance(modules, list)
    canonical_names = [
        entry["cpp_module"].removeprefix("srpc.") for entry in modules
    ]
    canonical_sources = [entry["source"] for entry in modules]
    if cmake_set(cmake, "SRPC_GOAL0_CANONICAL_MODULES") != canonical_names:
        raise AssertionError("canonical CMake names differ from manifest order")
    inline_sources = [
        source_token(token) for token in cmake_set(cmake, "SRPC_INLINE_MODULE_SRC")
    ]
    if inline_sources != list(EXPECTED_INLINE_SOURCES):
        raise AssertionError("inline carrier path inventory drifted or duplicated")
    if len(set(inline_sources)) != len(inline_sources):
        raise AssertionError("inline carrier path inventory contains duplicates")
    expected_inline_names = cmake_set(cmake, "SRPC_EXPECTED_INLINE_MODULES")
    if expected_inline_names != list(EXPECTED_INLINE_SOURCES.values()):
        raise AssertionError("inline module-name inventory drifted")
    retired = [
        source_token(token)
        for token in cmake_set(cmake, "SRPC_GOAL0_RETIRED_CARRIER_SRC")
    ]
    if retired != canonical_sources or len(set(retired)) != len(retired):
        raise AssertionError("retired carriers must equal canonical sources")
    if cmake_set(cmake, "SRPC_BORROW_SRC") != ["${SRPC_INLINE_MODULE_SRC}"]:
        raise AssertionError("borrow inventory must derive exactly from inline carriers")


class StandaloneGoal0Tests(unittest.TestCase):
    def test_manifest_sources_are_rust_attached_by_path_declarations(self) -> None:
        with (ROOT / "rust-modules.toml").open("rb") as stream:
            manifest = tomllib.load(stream)
        self.assertEqual(manifest["schema_version"], 2)
        modules = manifest["module"]
        self.assertGreater(len(modules), 0)
        self.assertEqual(
            len({entry["cpp_module"] for entry in modules}), len(modules)
        )
        lib = (ROOT / "src/lib.rs").read_text(encoding="utf-8")
        for entry in modules:
            rust_name = entry["cpp_module"].removeprefix("srpc.")
            source = ROOT / entry["source"]
            # Canonical sources are Rust and are named .rs. They kept .cpp/.cc
            # only until the C++ -> Rust rename lineage was recorded in Git.
            self.assertIn(source.suffix, {".rs"})
            self.assertEqual(source.stem, rust_name)
            self.assertTrue(source.is_file(), source)
            # The module reaches its canonical file through the generated
            # crate index, with nothing in src/ standing in between. Assert
            # both halves: the declaration names the manifest path, and no
            # entry -- file or symlink -- shadows it at the conventional path.
            self.assertIn(
                f'#[path = "../{entry["source"]}"]\npub mod {rust_name};\n', lib
            )
            shadow = ROOT / "src" / f"{rust_name}.rs"
            self.assertFalse(shadow.is_symlink(), shadow)
            self.assertFalse(shadow.exists(), shadow)

        # src/ carries the generated crate index and nothing else.
        self.assertEqual(
            sorted(path.name for path in (ROOT / "src").iterdir()), ["lib.rs"]
        )

        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        validate_provider_inventory(cmake, manifest)
        inventory = re.search(
            r"set\(SRPC_GOAL0_CANONICAL_MODULES\s+(.*?)\n\)",
            cmake,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(inventory)
        assert inventory is not None
        self.assertEqual(
            set(inventory.group(1).split()),
            {
                entry["cpp_module"].removeprefix("srpc.")
                for entry in modules
            },
        )
        for entry in modules:
            rust_name = entry["cpp_module"].removeprefix("srpc.")
            self.assertIn(
                f"set(SRPC_GOAL0_SOURCE_{rust_name} "
                f"${{CMAKE_CURRENT_SOURCE_DIR}}/{entry['source']})",
                cmake,
            )

    def test_duplicate_inline_provider_negative_control_is_rejected(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        with (ROOT / "rust-modules.toml").open("rb") as stream:
            manifest = tomllib.load(stream)
        # The inline inventory is empty, so the drift to reject is a carrier
        # REAPPEARING -- a canonical provider silently re-served from a
        # hand-written inline unit.
        mutated = cmake.replace(
            "set(SRPC_INLINE_MODULE_SRC\n)",
            "set(SRPC_INLINE_MODULE_SRC\n"
            "    ${CMAKE_CURRENT_SOURCE_DIR}/rpc/client.rs\n)",
            1,
        )
        self.assertNotEqual(mutated, cmake)
        with self.assertRaisesRegex(AssertionError, "drifted or duplicated"):
            validate_provider_inventory(mutated, manifest)

    def test_empty_inline_inventory_is_not_a_vacuous_check(self) -> None:
        """An empty expected inventory must still reject a stray module NAME.

        `SRPC_INLINE_MODULE_SRC` and `SRPC_EXPECTED_INLINE_MODULES` are checked
        independently; with both empty it would be easy for the pair to drift
        apart unnoticed.
        """
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        with (ROOT / "rust-modules.toml").open("rb") as stream:
            manifest = tomllib.load(stream)
        self.assertEqual(EXPECTED_INLINE_SOURCES, {})
        validate_provider_inventory(cmake, manifest)
        mutated = cmake.replace(
            "set(SRPC_EXPECTED_INLINE_MODULES\n)",
            "set(SRPC_EXPECTED_INLINE_MODULES\n    client\n)",
            1,
        )
        self.assertNotEqual(mutated, cmake)
        with self.assertRaisesRegex(AssertionError, "inline module-name inventory"):
            validate_provider_inventory(mutated, manifest)

    def test_dsl_census_fails_closed_on_removed_block_or_carrier(self) -> None:
        with tempfile.TemporaryDirectory(prefix="srpc-dsl-contract-") as raw:
            scratch = Path(raw)
            (scratch / "scripts").mkdir()
            shutil.copy2(
                ROOT / "scripts/srpc_dsl_check.sh",
                scratch / "scripts/srpc_dsl_check.sh",
            )
            for relative in EXPECTED_DSL_SOURCES:
                destination = scratch / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, destination)
            transpiler = scratch / "fake-transpiler"
            transpiler.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            transpiler.chmod(0o755)

            def check() -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    ["bash", "scripts/srpc_dsl_check.sh", str(transpiler)],
                    cwd=scratch,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                )

            self.assertEqual(check().returncode, 0)
            carrier = scratch / "reactor/epoll_platform_linux.cc"
            original = carrier.read_text(encoding="utf-8")
            carrier.write_text(
                original.replace("\n#if RUSTYCPP_RUST", "\n#if 0", 1),
                encoding="utf-8",
            )
            removed_block = check()
            self.assertNotEqual(removed_block.returncode, 0)
            self.assertIn("block census mismatch", removed_block.stderr)
            carrier.write_text(original, encoding="utf-8")
            os.unlink(scratch / "reactor/epoll_platform_linux.cc")
            removed_carrier = check()
            self.assertNotEqual(removed_carrier.returncode, 0)
            self.assertIn("carrier census mismatch", removed_carrier.stderr)

    def test_rusty_cpp_is_an_exact_gitlink_dependency(self) -> None:
        fields = subprocess.check_output(
            ["git", "ls-files", "--stage", "--", "third-party/rusty-cpp"],
            cwd=ROOT,
            text=True,
        ).split()
        self.assertGreaterEqual(len(fields), 3)
        self.assertEqual(fields[0], "160000")
        self.assertEqual(fields[1], RUSTY_CPP_PIN)
        with (ROOT / ".gitmodules").open("rb") as stream:
            text = stream.read().decode()
        self.assertIn("https://github.com/shuaimu/rusty-cpp.git", text)

    def test_cmake_and_goal0_scripts_are_repository_relative(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("project(srpc LANGUAGES C CXX ASM)", cmake)
        self.assertIn("SRPC_RUSTY_CPP_DIR", cmake)
        self.assertIn("--dependency-module-root", cmake)
        self.assertIn("--configured-module-map-root", cmake)
        self.assertIn('"${CMAKE_BINARY_DIR}/CMakeFiles"', cmake)
        self.assertNotIn("${CMAKE_SOURCE_DIR}/src/srpc", cmake)
        self.assertNotIn("${CMAKE_SOURCE_DIR}/scripts", cmake)
        self.assertNotIn("MAKO_BREW_LIBCXX_FLAGS", cmake)
        for relative in (
            "scripts/srpc_dsl_check.sh",
            "scripts/extract_srpc_rust.py",
            "scripts/check_srpc_crate_mode.py",
            "scripts/update_file_fingerprint.cmake",
        ):
            self.assertTrue((ROOT / relative).is_file(), relative)

    def test_no_absolute_mako_checkout_dependency(self) -> None:
        tracked = subprocess.check_output(
            ["git", "ls-files"], cwd=ROOT, text=True
        ).splitlines()
        candidates = [
            ROOT / relative
            for relative in tracked
            if relative == "CMakeLists.txt"
            or relative == "Makefile"
            or relative == ".gitmodules"
            or relative.endswith(
                (".cmake", ".py", ".sh", ".toml", ".json", ".yaml", ".yml")
            )
        ]
        absolute_mako = re.compile(
            rb"/(?:home|Users|var/tmp)/[^\x00\r\n\"' ]*mako(?:[-/][^\x00\r\n\"' ]*)?"
        )
        parent_mako = re.compile(
            rb"(?:^|[\s\"'=:(])\.\./(?:\.\./)*mako(?:[/\s\"')]|$)",
            re.MULTILINE,
        )
        for candidate in candidates:
            if not candidate.is_file() or "__pycache__" in candidate.parts:
                continue
            data = candidate.read_bytes()
            self.assertNotIn(b"/home/users/" + b"shuai/mako", data, candidate)
            self.assertNotIn(b"/var/tmp/" + b"mako-", data, candidate)
            self.assertIsNone(absolute_mako.search(data), candidate)
            self.assertIsNone(parent_mako.search(data), candidate)


if __name__ == "__main__":
    unittest.main()
