#!/usr/bin/env python3
"""Unit and mutation tests for mako_local_fingerprint.py."""

from __future__ import annotations

import importlib.util
from collections import Counter
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "mako_local_fingerprint.py"
SPEC = importlib.util.spec_from_file_location("mako_local_fingerprint", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
fingerprint = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = fingerprint
SPEC.loader.exec_module(fingerprint)


class FingerprintTests(unittest.TestCase):
    def test_native_allocator_parser_is_a_recipe_input(self) -> None:
        self.assertIn(
            "crates/mako-local/build_support/native_allocator.rs",
            fingerprint.RECIPE_FILES,
        )

    def make_archive_inventory(self, root: Path):
        source_root = root / "source"
        build_dir = root / "build"
        compiler = Path("/bin/true")
        source_root.mkdir()
        (build_dir / "generated").mkdir(parents=True)

        members = [
            ("mako", fingerprint.ABI_IMPLEMENTATION),
            # This implementation was outside the former hand-maintained STO
            # list and is the regression sentinel for complete archive scope.
            ("mako", "src/mako/lib/message.cc"),
            # This object-library member is bundled into libmako.a even though
            # Cargo does not link a separate txlog_core_obj archive.
            ("txlog_core_obj", "src/deptran/replication_helper.cc"),
            ("masstree", "src/masstree/compiler.cc"),
            ("cluster", "src/cluster/cluster_config.cc"),
            ("srpc", "src/srpc/base/logging.cpp"),
        ]
        covered_targets = {target for target, _relative in members}
        members.extend(
            (
                target,
                f"third-party/rusty-cpp/transpiled/{target}/{target}.cppm",
            )
            for target in fingerprint.RUST_LINKED_ARCHIVE_TARGETS
            if target not in covered_targets
        )
        database = []
        for target, relative in members:
            source = source_root / relative
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text(f"// {target}:{relative}\n", encoding="utf-8")
            output = f"CMakeFiles/{target}.dir/{relative}.o"
            database.append(
                {
                    "directory": str(build_dir),
                    "file": str(source),
                    "output": output,
                    "arguments": [
                        str(compiler),
                        f"-DARCHIVE={target}",
                        "-o",
                        output,
                        "-c",
                        str(source),
                    ],
                }
            )

        identity = build_dir / fingerprint.GENERATED_IDENTITY
        identity.write_text("// generated identity\n", encoding="utf-8")
        identity_output = "CMakeFiles/mako.dir/generated/mako_local_build_identity.cc.o"
        database.append(
            {
                "directory": str(build_dir),
                "file": str(identity),
                "output": identity_output,
                "arguments": [
                    str(compiler),
                    "-o",
                    identity_output,
                    "-c",
                    str(identity),
                ],
            }
        )
        return source_root, build_dir, database

    def test_native_link_archive_manifest_is_strict_and_ordered(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            manifest = Path(temporary) / "archives.txt"
            manifest.write_text(
                "# consumer before provider\n"
                "mako libmako.a\n"
                "srpc src/srpc/libsrpc.a\n",
                encoding="utf-8",
            )
            self.assertEqual(
                fingerprint.read_native_link_archives(manifest),
                (("mako", "libmako.a"), ("srpc", "src/srpc/libsrpc.a")),
            )

            invalid_cases = (
                ("mako libmako.a extra\n", "exactly"),
                ("mako ../libmako.a\n", "relative"),
                ("mako libmako.a\nmako other.a\n", "repeats library"),
                ("srpc src/srpc/libsrpc.a\n", "must start"),
            )
            for contents, diagnostic in invalid_cases:
                with self.subTest(contents=contents):
                    manifest.write_text(contents, encoding="utf-8")
                    with self.assertRaisesRegex(
                        fingerprint.FingerprintError, diagnostic
                    ):
                        fingerprint.read_native_link_archives(manifest)

    def test_record_order_is_deterministic_and_duplicates_are_rejected(self) -> None:
        first = fingerprint.Record.from_bytes("input", "$SRC/a", b"a")
        second = fingerprint.Record.from_bytes("input", "$SRC/b", b"b")
        self.assertEqual(
            fingerprint.fingerprint_records([first, second]),
            fingerprint.fingerprint_records([second, first]),
        )
        with self.assertRaisesRegex(fingerprint.FingerprintError, "duplicate"):
            fingerprint.fingerprint_records([first, first])

    def test_make_dependency_parser_handles_continuations_and_spaces(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            plain = root / "plain.h"
            spaced = root / "with space.h"
            plain.write_text("plain", encoding="utf-8")
            spaced.write_text("spaced", encoding="utf-8")
            output = f"target: {plain} \\\n {str(spaced).replace(' ', r'\ ')}\n"
            self.assertEqual(
                fingerprint.parse_make_dependencies(output, root),
                [plain.resolve(), spaced.resolve()],
            )

    def test_compile_output_flags_are_removed_without_losing_definitions(self) -> None:
        arguments = (
            "/tool/bin/clang++",
            "-DREAD_MY_WRITES=1",
            "-MD",
            "-MF",
            "output.d",
            "-MTtarget",
            "-o",
            "output.o",
            "@output.o.modmap",
            "source.cc",
        )
        self.assertEqual(
            fingerprint.strip_compile_outputs(arguments, strip_modmap=True),
            ["/tool/bin/clang++", "-DREAD_MY_WRITES=1", "source.cc"],
        )

    def test_predefine_probe_removes_the_absolute_source_argument(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.cc"
            compiler = root / "clang++"
            source.write_text("int x;", encoding="utf-8")
            compiler.write_text("compiler", encoding="utf-8")
            compile_root = fingerprint.CompileRoot(
                "mako",
                "CMakeFiles/mako.dir/source.cc.o",
                source.resolve(),
                root.resolve(),
                (str(compiler), "-march=native", str(source), "-o", "source.o", "-c"),
                compiler.resolve(),
                compiler,
            )
            completed = types.SimpleNamespace(returncode=0, stdout=b"#define X 1\n", stderr=b"")
            with mock.patch.object(fingerprint.subprocess, "run", return_value=completed) as run:
                result = fingerprint.compiler_predefines(compile_root)
            invoked = run.call_args.args[0]
            self.assertNotIn(str(source), invoked)
            self.assertEqual(invoked[-5:], ["-dM", "-E", "-x", "c++", "-"])
            self.assertEqual(result, b"#define X 1\n")

    def test_dependency_scan_jobs_follow_environment_precedence(self) -> None:
        cases = (
            (
                {
                    "MAKO_LOCAL_FINGERPRINT_JOBS": "12",
                    "CI_BUILD_JOBS": "6",
                    "CI_MAKE_JOBS": "3",
                },
                12,
            ),
            ({"CI_BUILD_JOBS": "6", "CI_MAKE_JOBS": "3"}, 6),
            ({"CI_MAKE_JOBS": "3"}, 3),
            ({}, 8),
        )
        for environment, expected in cases:
            with self.subTest(environment=environment):
                with mock.patch.dict(os.environ, environment, clear=True):
                    with mock.patch.object(fingerprint.os, "cpu_count", return_value=64):
                        self.assertEqual(
                            fingerprint.dependency_scan_worker_count(100), expected
                        )

    def test_dependency_scan_jobs_are_clamped_to_cpu_and_task_count(self) -> None:
        with mock.patch.dict(
            os.environ, {"MAKO_LOCAL_FINGERPRINT_JOBS": "100"}, clear=True
        ):
            with mock.patch.object(fingerprint.os, "cpu_count", return_value=6):
                self.assertEqual(fingerprint.dependency_scan_worker_count(20), 6)
                self.assertEqual(fingerprint.dependency_scan_worker_count(4), 4)

    def test_dependency_scan_jobs_reject_invalid_explicit_values(self) -> None:
        for variable in (
            "MAKO_LOCAL_FINGERPRINT_JOBS",
            "CI_BUILD_JOBS",
            "CI_MAKE_JOBS",
        ):
            for value in ("", "0", "-1", "+2", " 3", "four"):
                with self.subTest(variable=variable, value=value):
                    with mock.patch.dict(os.environ, {variable: value}, clear=True):
                        with self.assertRaisesRegex(
                            fingerprint.FingerprintError,
                            rf"{variable} must be a positive integer",
                        ):
                            fingerprint.dependency_scan_worker_count(10)

    def test_complete_linked_archive_inventory_affects_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source_root, build_dir, database = self.make_archive_inventory(Path(temporary))
            roots = fingerprint.select_compile_roots(database, source_root, build_dir)
            self.assertEqual(
                {root.target for root in roots}, set(fingerprint.NATIVE_CLOSURE_TARGETS)
            )
            message = next(
                root for root in roots if root.source.name == "message.cc"
            )
            self.assertEqual(message.target, "mako")
            self.assertFalse(
                any(root.source.name == "mako_local_build_identity.cc" for root in roots)
            )

            inventory_before = fingerprint.fingerprint_records(
                fingerprint.archive_inventory_records(
                    roots,
                    fingerprint.PathNormalizer(
                        source_root,
                        build_dir,
                        roots[0].compiler,
                        roots[0].compiler_spelling,
                    ),
                )
            )
            without_message = [
                entry
                for entry in database
                if Path(str(entry["file"])).name != "message.cc"
            ]
            roots_without_message = fingerprint.select_compile_roots(
                without_message, source_root, build_dir
            )
            inventory_after = fingerprint.fingerprint_records(
                fingerprint.archive_inventory_records(
                    roots_without_message,
                    fingerprint.PathNormalizer(
                        source_root,
                        build_dir,
                        roots_without_message[0].compiler,
                        roots_without_message[0].compiler_spelling,
                    ),
                )
            )
            self.assertNotEqual(inventory_before, inventory_after)

            expected_members = fingerprint.expected_archive_members(
                roots,
                fingerprint.generated_identity_member(database, build_dir),
            )
            self.assertEqual(
                expected_members["libmako.a"],
                Counter(
                    {
                        "mako_local_abi.cc.o": 1,
                        "message.cc.o": 1,
                        "replication_helper.cc.o": 1,
                        "mako_local_build_identity.cc.o": 1,
                    }
                ),
            )

            compiler = roots[0].compiler
            normalizer = fingerprint.PathNormalizer(
                source_root, build_dir, compiler, roots[0].compiler_spelling
            )

            def records():
                result = []
                for root in roots:
                    result.append(
                        fingerprint.Record.from_bytes(
                            "compile-command",
                            root.inventory_name,
                            fingerprint.normalize_command(root, normalizer).encode(),
                        )
                    )
                    result.append(
                        fingerprint.Record.from_bytes(
                            "input", normalizer.path(root.source), root.source.read_bytes()
                        )
                    )
                return result

            before = fingerprint.fingerprint_records(records())
            message.source.write_text("// changed message implementation\n", encoding="utf-8")
            after = fingerprint.fingerprint_records(records())
            self.assertNotEqual(before, after)

    def test_modmap_appearance_does_not_change_fingerprint_records(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "source.cc"
            source.write_text("int value;\n", encoding="utf-8")
            modmap = root / "source.cc.o.modmap"
            compile_root = fingerprint.CompileRoot(
                "mako",
                "CMakeFiles/mako.dir/source.cc.o",
                source,
                root,
                (
                    "/bin/true",
                    f"@{modmap}",
                    "-o",
                    "CMakeFiles/mako.dir/source.cc.o",
                    "-c",
                    str(source),
                ),
                Path("/bin/true").resolve(),
                Path("/bin/true"),
            )
            normalizer = fingerprint.PathNormalizer(
                root, root, compile_root.compiler, compile_root.compiler_spelling
            )

            def records():
                command = fingerprint.Record.from_bytes(
                    "compile-command",
                    compile_root.inventory_name,
                    fingerprint.normalize_command(compile_root, normalizer).encode(),
                )
                responses, dependencies = fingerprint.compile_response_records(
                    compile_root, normalizer
                )
                self.assertEqual(dependencies, set())
                return [command, *responses]

            before = records()
            modmap.write_text("-fmodule-file=std=std.pcm\n", encoding="utf-8")
            after = records()
            self.assertEqual(before, after)
            self.assertEqual(
                fingerprint.fingerprint_records(before),
                fingerprint.fingerprint_records(after),
            )

    def test_linked_archive_inventory_rejects_missing_and_duplicate_members(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source_root, build_dir, database = self.make_archive_inventory(Path(temporary))
            for target in fingerprint.NATIVE_CLOSURE_TARGETS:
                without_target = [
                    entry
                    for entry in database
                    if f"CMakeFiles/{target}.dir/" not in str(entry["output"])
                ]
                with self.subTest(missing_target=target):
                    with self.assertRaisesRegex(
                        fingerprint.FingerprintError, rf"no members.*{target}"
                    ):
                        fingerprint.select_compile_roots(
                            without_target, source_root, build_dir
                        )

            duplicate = database + [dict(database[0])]
            with self.assertRaisesRegex(fingerprint.FingerprintError, "duplicate"):
                fingerprint.select_compile_roots(duplicate, source_root, build_dir)

            without_identity = [
                entry
                for entry in database
                if Path(str(entry["file"])).name != "mako_local_build_identity.cc"
            ]
            with self.assertRaisesRegex(
                fingerprint.FingerprintError, "exactly one excluded generated identity"
            ):
                fingerprint.select_compile_roots(without_identity, source_root, build_dir)

    def test_archive_composition_counts_duplicates_and_rejects_unlisted_members(self) -> None:
        archive = Path("/build/libmako.a")
        expected = Counter({"duplicate.o": 2, "known.o": 1})
        with self.assertRaisesRegex(
            fingerprint.FingerprintError, r"missing: duplicate\.o"
        ):
            fingerprint.compare_archive_members(
                archive, expected, Counter({"duplicate.o": 1, "known.o": 1})
            )

        with self.assertRaisesRegex(
            fingerprint.FingerprintError, r"unexpected: unlisted\.o"
        ):
            fingerprint.compare_archive_members(
                archive,
                expected,
                Counter({"duplicate.o": 2, "known.o": 1, "unlisted.o": 1}),
            )

    def test_archive_content_rejects_same_names_with_different_bytes(self) -> None:
        archive = Path("/build/libmako.a")
        selected = fingerprint.ObjectContent.from_bytes("same.o", b"selected")
        archived = fingerprint.ObjectContent.from_bytes("same.o", b"another build")
        self.assertEqual(selected.basename, archived.basename)
        with self.assertRaisesRegex(
            fingerprint.FingerprintError, r"object content does not match"
        ):
            fingerprint.compare_archive_object_contents(
                archive, Counter({selected: 1}), Counter({archived: 1})
            )

    def test_archive_content_counts_duplicate_object_occurrences(self) -> None:
        archive = Path("/build/libmako.a")
        first = fingerprint.ObjectContent.from_bytes("duplicate.o", b"first")
        second = fingerprint.ObjectContent.from_bytes("duplicate.o", b"second")
        # Basename Counters agree, but one duplicate occurrence came from the
        # wrong build and must still fail the byte-level Counter comparison.
        expected = Counter({first: 1, second: 1})
        actual = Counter({first: 2})

        def basenames(objects):
            result = Counter()
            for item, count in objects.items():
                result[item.basename] += count
            return result

        self.assertEqual(basenames(expected), Counter({"duplicate.o": 2}))
        self.assertEqual(basenames(actual), Counter({"duplicate.o": 2}))
        with self.assertRaisesRegex(
            fingerprint.FingerprintError, r"object content does not match"
        ):
            fingerprint.compare_archive_object_contents(archive, expected, actual)

    def test_archive_content_rejects_missing_selected_build_object(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            missing = Path(temporary) / "missing.o"
            with self.assertRaisesRegex(
                fingerprint.FingerprintError, r"selected build object does not exist"
            ):
                fingerprint.hash_expected_archive_objects(
                    {"libmako.a": [("missing.o", missing)]}
                )

    def test_path_normalization_covers_real_and_symlink_toolchain_paths(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            build = source / "build"
            real_toolchain = root / "cellar" / "llvm"
            lexical_toolchain = root / "opt" / "llvm"
            (real_toolchain / "bin").mkdir(parents=True)
            lexical_toolchain.parent.mkdir(parents=True)
            lexical_toolchain.symlink_to(real_toolchain, target_is_directory=True)
            compiler = real_toolchain / "bin" / "clang++"
            compiler.write_text("compiler", encoding="utf-8")
            source.mkdir()
            build.mkdir()
            normalizer = fingerprint.PathNormalizer(
                source, build, compiler, lexical_toolchain / "bin" / "clang++"
            )
            value = (
                f"-I{source}/include -I{build}/generated "
                f"-resource-dir={lexical_toolchain}/lib/clang"
            )
            self.assertEqual(
                normalizer.text(value),
                "-I$SRC/include -I$BUILD/generated -resource-dir=$TOOLCHAIN/lib/clang",
            )

    def test_content_change_is_rejected_even_with_unchanged_mtime(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "input.hh"
            source.write_bytes(b"before")
            original_times = source.stat()
            before = fingerprint.Record.from_bytes("input", "$SRC/input.hh", source.read_bytes())
            original = fingerprint.Snapshot(root, root, "engine", [before], {source})
            manifest = original.manifest()

            source.write_bytes(b"after!")
            os.utime(source, ns=(original_times.st_atime_ns, original_times.st_mtime_ns))
            after = fingerprint.Record.from_bytes("input", "$SRC/input.hh", source.read_bytes())
            current = fingerprint.Snapshot(root, root, "engine", [after], {source})
            with self.assertRaisesRegex(fingerprint.FingerprintError, "changed input"):
                fingerprint.compare_manifest(manifest, current)

    def test_depfile_has_make_continuations_without_patch_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "identity.cc"
            dependency = root / "with space.hh"
            depfile = root / "identity.d"
            target.write_text("", encoding="utf-8")
            dependency.write_text("", encoding="utf-8")
            fingerprint.write_depfile(depfile, target, [dependency])
            contents = depfile.read_text(encoding="utf-8")
            self.assertIn(" \\\n  ", contents)
            self.assertNotIn("+  ", contents)
            self.assertIn("with\\ space.hh", contents)

    def test_generated_anchor_and_accessors_are_exactly_named(self) -> None:
        record = fingerprint.Record.from_bytes("input", "$SRC/a", b"a")
        snapshot = fingerprint.Snapshot(Path("/src"), Path("/build"), "engine", [record], set())
        digest = snapshot.fingerprint
        cpp = fingerprint.render_cpp(snapshot)
        rust = fingerprint.render_rust(snapshot)
        self.assertIn(f"mako_local_build_anchor_{digest}", cpp)
        self.assertIn(f"mako_local_build_anchor_{digest}", rust)
        for symbol in (
            "mako_local_engine_id",
            "mako_local_build_fingerprint",
            "mako_local_build_fingerprint_size",
        ):
            self.assertIn(symbol, cpp)
            self.assertNotIn(symbol, rust)

    def test_verify_cli_allows_content_gate_without_rust_output(self) -> None:
        args = fingerprint.build_parser().parse_args(
            [
                "verify",
                "--source-root",
                "/source",
                "--build-dir",
                "/build",
                "--manifest",
                "/build/manifest.json",
            ]
        )
        self.assertIsNone(args.rust_out)


if __name__ == "__main__":
    unittest.main()
