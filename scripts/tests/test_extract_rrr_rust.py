#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
DRIVER_PATH = REPOSITORY / "scripts/extract_rrr_rust.py"
SPEC = importlib.util.spec_from_file_location("extract_rrr_rust", DRIVER_PATH)
assert SPEC is not None and SPEC.loader is not None
DRIVER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = DRIVER
SPEC.loader.exec_module(DRIVER)

GATE_PATH = REPOSITORY / "scripts/check_rrr_crate_mode.py"
GATE_SPEC = importlib.util.spec_from_file_location("check_rrr_crate_mode", GATE_PATH)
assert GATE_SPEC is not None and GATE_SPEC.loader is not None
GATE = importlib.util.module_from_spec(GATE_SPEC)
sys.modules[GATE_SPEC.name] = GATE
GATE_SPEC.loader.exec_module(GATE)


def source_block(source: str, block_id: str) -> str:
    marker = f"/*RUSTYCPP:GEN-BEGIN id={block_id} "
    marker_at = source.index(marker)
    prefix = source[:marker_at]
    end = prefix.rfind("#endif")
    start_directive = prefix.rfind("#if RUSTYCPP_RUST", 0, end)
    if end < 0 or start_directive < 0:
        raise AssertionError(f"cannot locate source block {block_id}")
    start = prefix.index("\n", start_directive) + 1
    return prefix[start:end].strip("\n") + "\n"


def split_generated(data: bytes) -> tuple[list[str], bytes]:
    header, payload = data.split(b"//\n", 1)
    return header.decode("utf-8").splitlines(), payload


def generated_by_label(
    generated: list[object], output_label: str
) -> object:
    return next(item for item in generated if item.output_label == output_label)


def subprocess_result(
    returncode: int, stdout: str, stderr: str
) -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(
        args=["rusty-cpp-transpiler", "--build-info"],
        returncode=returncode,
        stdout=stdout,
        stderr=stderr,
    )


class CheckedInCanaryTests(unittest.TestCase):
    def test_manifest_names_the_real_module_source_and_output(self) -> None:
        modules = DRIVER.load_manifest(
            REPOSITORY, REPOSITORY / "src/rrr/rust-extraction.toml"
        )
        self.assertEqual(len(modules), 1)
        module = modules[0]
        self.assertEqual(module.cpp_module, "rrr.internal_protocol")
        self.assertEqual(module.rust_module, "internal_protocol")
        self.assertEqual(module.output_label, "src/rrr/src/internal_protocol.rs")
        self.assertEqual(len(module.inputs), 1)
        self.assertEqual(
            module.inputs[0].source_label,
            "src/rrr/rpc/internal_protocol.cpp",
        )
        self.assertEqual(module.inputs[0].block_ids, ("internal_protocol.1",))

    def test_checked_in_payload_is_exactly_the_authored_inline_rust(self) -> None:
        source_path = REPOSITORY / "src/rrr/rpc/internal_protocol.cpp"
        output_path = REPOSITORY / "src/rrr/src/internal_protocol.rs"
        source_bytes = source_path.read_bytes()
        source = source_bytes.decode("utf-8")
        header, payload = split_generated(output_path.read_bytes())
        expected = source_block(source, "internal_protocol.1").encode("utf-8")

        self.assertEqual(payload, expected)
        self.assertIn("// provenance-cpp-module: rrr.internal_protocol", header)
        self.assertIn(
            "// provenance-input[0]-source: src/rrr/rpc/internal_protocol.cpp",
            header,
        )
        self.assertIn(
            "// provenance-input[0]-block-ids: internal_protocol.1",
            header,
        )
        self.assertIn(
            "// provenance-input[0]-source-sha256: "
            f"{hashlib.sha256(source_bytes).hexdigest()}",
            header,
        )
        self.assertIn(
            "// provenance-input[0]-rust-sha256: "
            f"{hashlib.sha256(payload).hexdigest()}",
            header,
        )
        self.assertIn(
            f"// provenance-rust-sha256: {hashlib.sha256(payload).hexdigest()}",
            header,
        )

    def test_lib_is_manifest_generated_and_census_has_no_orphans(self) -> None:
        manifest = REPOSITORY / "src/rrr/rust-extraction.toml"
        modules = DRIVER.load_manifest(REPOSITORY, manifest)
        expected_lib = DRIVER.render_lib(
            "src/rrr/rust-extraction.toml", manifest, modules
        )
        self.assertEqual(
            (REPOSITORY / "src/rrr/src/lib.rs").read_bytes(),
            expected_lib,
        )
        self.assertEqual(
            DRIVER.rust_source_census(REPOSITORY),
            {
                "src/rrr/src/lib.rs",
                "src/rrr/src/internal_protocol.rs",
            },
        )


class DriverBehaviorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="rrr-extractor-test-")
        self.root = Path(self.temporary.name)
        source_root = self.root / "src/rrr/rpc"
        source_root.mkdir(parents=True)
        self.interface = source_root / "example.cpp"
        self.interface.write_text(
            textwrap.dedent(
                """\
                module;
                export module rrr.example;

                #if RUSTYCPP_RUST
                const FIRST: i32 = 7;
                #endif
                /*RUSTYCPP:GEN-BEGIN id=example.1 version=1 rust_sha256=unused*/
                generated C++ 1
                /*RUSTYCPP:GEN-END id=example.1*/

                #if RUSTYCPP_RUST
                const SECOND: i32 = 11;
                #endif
                /*RUSTYCPP:GEN-BEGIN id=example.2 version=1 rust_sha256=unused*/
                generated C++ 2
                /*RUSTYCPP:GEN-END id=example.2*/
                """
            ),
            encoding="utf-8",
        )
        self.implementation = source_root / "example_impl.cc"
        self.implementation.write_text(
            textwrap.dedent(
                """\
                module rrr.example;

                #if RUSTYCPP_RUST
                fn implementation() -> i32 { 13 }
                #endif
                /*RUSTYCPP:GEN-BEGIN id=example.impl version=1 rust_sha256=unused*/
                generated C++ implementation
                /*RUSTYCPP:GEN-END id=example.impl*/
                """
            ),
            encoding="utf-8",
        )
        self.manifest = self.root / "src/rrr/rust-extraction.toml"
        self.write_manifest(
            """\
            schema_version = 1

            [[module]]
            cpp_module = "rrr.example"
            output = "src/rrr/src/example.rs"

            [[module.input]]
            source = "src/rrr/rpc/example.cpp"
            block_ids = ["example.2", "example.1"]

            [[module.input]]
            source = "src/rrr/rpc/example_impl.cc"
            block_ids = ["example.impl"]
            """
        )
        self.log = self.root / "argv.json"
        self.fake = self.root / "fake-inline-rust"
        self.fake.write_text(
            textwrap.dedent(
                """\
                #!/usr/bin/env python3
                import json
                import os
                from pathlib import Path
                import sys

                args = sys.argv[1:]
                log = Path(os.environ["FAKE_ARGV_LOG"])
                history = json.loads(log.read_text()) if log.exists() else []
                history.append(args)
                log.write_text(json.dumps(history))
                if args[0] != "inline-rust":
                    raise SystemExit(9)
                output = Path(args[args.index("--emit-rust") + 1])
                source = Path(args[args.index("--files") + 1]).read_text()
                block_ids = [
                    args[index + 1]
                    for index, value in enumerate(args)
                    if value == "--block-id"
                ]
                payloads = []
                for block_id in block_ids:
                    marker_at = source.index(
                        f"/*RUSTYCPP:GEN-BEGIN id={block_id} "
                    )
                    prefix = source[:marker_at]
                    end = prefix.rfind("#endif")
                    directive = prefix.rfind("#if RUSTYCPP_RUST", 0, end)
                    start = prefix.index("\\n", directive) + 1
                    payloads.append(prefix[start:end].strip("\\n"))
                output.write_text("\\n\\n".join(payloads) + "\\n")
                """
            ),
            encoding="utf-8",
        )
        self.fake.chmod(0o755)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_manifest(self, contents: str) -> None:
        self.manifest.parent.mkdir(parents=True, exist_ok=True)
        self.manifest.write_text(textwrap.dedent(contents), encoding="utf-8")

    def generate(self) -> list[object]:
        modules = DRIVER.load_manifest(self.root, self.manifest)
        executable = DRIVER.resolve_transpiler(self.root, str(self.fake))
        with mock.patch.dict(os.environ, {"FAKE_ARGV_LOG": str(self.log)}):
            return DRIVER.generate_all(
                self.root,
                modules,
                executable,
                "src/rrr/rust-extraction.toml",
                self.manifest,
            )

    def test_write_and_check_are_deterministic_and_use_one_call_per_source(self) -> None:
        generated = self.generate()
        DRIVER.apply_mode(self.root, generated, "write")
        first = {
            item.output_label: item.output.read_bytes()
            for item in generated
        }

        DRIVER.apply_mode(self.root, self.generate(), "check")
        self.assertEqual(
            {item.output_label: item.output.read_bytes() for item in generated},
            first,
        )

        history = json.loads(self.log.read_text(encoding="utf-8"))
        self.assertEqual(len(history), 4)
        for offset in (0, 2):
            self.assertEqual(history[offset][0:2], ["inline-rust", "--emit-rust"])
            self.assertEqual(
                history[offset][3:],
                [
                    "--block-id",
                    "example.2",
                    "--block-id",
                    "example.1",
                    "--files",
                    "src/rrr/rpc/example.cpp",
                ],
            )
            self.assertEqual(
                history[offset + 1][3:],
                [
                    "--block-id",
                    "example.impl",
                    "--files",
                    "src/rrr/rpc/example_impl.cc",
                ],
            )

    def test_two_sources_are_concatenated_in_manifest_order(self) -> None:
        generated = self.generate()
        module = generated_by_label(generated, "src/rrr/src/example.rs")
        header, payload = split_generated(module.content)
        self.assertEqual(
            payload,
            b"const SECOND: i32 = 11;\n\n"
            b"const FIRST: i32 = 7;\n\n"
            b"fn implementation() -> i32 { 13 }\n",
        )
        self.assertIn(
            "// provenance-input[0]-block-ids: example.2, example.1",
            header,
        )
        self.assertIn(
            "// provenance-input[1]-block-ids: example.impl",
            header,
        )
        history = json.loads(self.log.read_text(encoding="utf-8"))
        self.assertEqual(len(history), 2)

    def test_check_detects_drift_without_rewriting(self) -> None:
        generated = self.generate()
        DRIVER.apply_mode(self.root, generated, "write")
        output = self.root / "src/rrr/src/example.rs"
        output.write_text("tampered\n", encoding="utf-8")

        with self.assertRaisesRegex(DRIVER.ExtractionError, "stale"):
            DRIVER.apply_mode(self.root, generated, "check")
        self.assertEqual(output.read_text(encoding="utf-8"), "tampered\n")

    def test_check_and_write_reject_orphan_rust_sources(self) -> None:
        generated = self.generate()
        DRIVER.apply_mode(self.root, generated, "write")
        orphan = self.root / "src/rrr/src/orphan.rs"
        orphan.write_text("parallel implementation\n", encoding="utf-8")
        for mode in ("check", "write"):
            with self.subTest(mode=mode):
                with self.assertRaisesRegex(DRIVER.ExtractionError, "orphan"):
                    DRIVER.apply_mode(self.root, generated, mode)
        self.assertEqual(orphan.read_text(), "parallel implementation\n")

    def test_check_rejects_stale_and_missing_generated_lib(self) -> None:
        generated = self.generate()
        DRIVER.apply_mode(self.root, generated, "write")
        lib = generated_by_label(generated, "src/rrr/src/lib.rs")
        lib.output.write_text("stale lib\n", encoding="utf-8")
        with self.assertRaisesRegex(DRIVER.ExtractionError, "stale"):
            DRIVER.apply_mode(self.root, generated, "check")
        DRIVER.apply_mode(self.root, generated, "write")
        lib.output.unlink()
        with self.assertRaisesRegex(DRIVER.ExtractionError, "missing"):
            DRIVER.apply_mode(self.root, generated, "check")

    def test_output_symlink_is_rejected_at_load_and_before_write(self) -> None:
        generated = self.generate()
        output = self.root / "src/rrr/src/example.rs"
        output.parent.mkdir(parents=True)
        victim = self.root / "src/rrr/victim.rs"
        victim.write_text("do not overwrite\n", encoding="utf-8")
        output.symlink_to("../victim.rs")

        with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
            DRIVER.load_manifest(self.root, self.manifest)
        with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
            DRIVER.apply_mode(self.root, generated, "write")
        self.assertEqual(victim.read_text(encoding="utf-8"), "do not overwrite\n")

    def test_output_parent_symlink_is_rejected_at_load_and_before_census(self) -> None:
        generated = self.generate()
        victim = self.root / "src/rrr/generated-victim"
        victim.mkdir(parents=True)
        marker = victim / "marker"
        marker.write_text("do not touch\n", encoding="utf-8")
        (self.root / "src/rrr/src").symlink_to(
            victim, target_is_directory=True
        )

        with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
            DRIVER.load_manifest(self.root, self.manifest)
        with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
            DRIVER.apply_mode(self.root, generated, "write")
        self.assertEqual(marker.read_text(encoding="utf-8"), "do not touch\n")
        self.assertEqual(sorted(path.name for path in victim.iterdir()), ["marker"])

    def test_manifest_rejects_empty_input_and_block_ids(self) -> None:
        cases = [
            ("input = []", "input must be a non-empty"),
            (
                "[[module.input]]\n"
                "source = \"src/rrr/rpc/example.cpp\"\n"
                "block_ids = []",
                "block_ids must be a non-empty",
            ),
            (
                "[[module.input]]\n"
                "source = \"src/rrr/rpc/example.cpp\"\n"
                "block_ids = [\"example.1\", \"example.1\"]",
                "contains duplicate",
            ),
        ]
        for input_body, diagnostic in cases:
            with self.subTest(input_body=input_body):
                self.write_manifest(
                    f"""\
                    schema_version = 1
                    [[module]]
                    cpp_module = "rrr.example"
                    output = "src/rrr/src/example.rs"
                    {input_body}
                    """
                )
                with self.assertRaisesRegex(DRIVER.ExtractionError, diagnostic):
                    DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_module_source_and_output_mismatches(self) -> None:
        cases = [
            (
                "rrr.other",
                "src/rrr/src/other.rs",
                "src/rrr/rpc/example.cpp",
                "example.1",
                "interface source .* must contain exactly",
            ),
            (
                "rrr.example",
                "src/rrr/src/wrong.rs",
                "src/rrr/rpc/example.cpp",
                "example.1",
                "output does not match cpp_module",
            ),
            (
                "rrr.example",
                "src/rrr/src/example.rs",
                "src/rrr/rpc/example_impl.cc",
                "example.impl",
                "interface source .* must contain exactly",
            ),
        ]
        for cpp_module, output, source, block_id, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                self.write_manifest(
                    f"""\
                    schema_version = 1
                    [[module]]
                    cpp_module = "{cpp_module}"
                    output = "{output}"
                    [[module.input]]
                    source = "{source}"
                    block_ids = ["{block_id}"]
                    """
                )
                with self.assertRaisesRegex(DRIVER.ExtractionError, diagnostic):
                    DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_restricts_sources_to_real_production_roots(self) -> None:
        outside = self.root / "src/rrr/tests/example.cpp"
        outside.parent.mkdir(parents=True)
        outside.write_bytes(self.interface.read_bytes())
        self.write_manifest(
            """\
            schema_version = 1
            [[module]]
            cpp_module = "rrr.example"
            output = "src/rrr/src/example.rs"
            [[module.input]]
            source = "src/rrr/tests/example.cpp"
            block_ids = ["example.1"]
            """
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "approved production"):
            DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_source_file_and_parent_symlinks(self) -> None:
        source_link = self.root / "src/rrr/rpc/source_link.cpp"
        source_link.symlink_to("example.cpp")
        parent_link = self.root / "src/rrr/base"
        parent_link.symlink_to("rpc", target_is_directory=True)
        cases = [
            "src/rrr/rpc/source_link.cpp",
            "src/rrr/base/example.cpp",
        ]
        for source in cases:
            with self.subTest(source=source):
                self.write_manifest(
                    f"""\
                    schema_version = 1
                    [[module]]
                    cpp_module = "rrr.example"
                    output = "src/rrr/src/example.rs"
                    [[module.input]]
                    source = "{source}"
                    block_ids = ["example.1"]
                    """
                )
                with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
                    DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_wrong_implementation_module(self) -> None:
        wrong = self.root / "src/rrr/rpc/wrong_impl.cc"
        wrong.write_text("module rrr.other;\n", encoding="utf-8")
        self.write_manifest(
            """\
            schema_version = 1
            [[module]]
            cpp_module = "rrr.example"
            output = "src/rrr/src/example.rs"
            [[module.input]]
            source = "src/rrr/rpc/example.cpp"
            block_ids = ["example.1"]
            [[module.input]]
            source = "src/rrr/rpc/wrong_impl.cc"
            block_ids = ["wrong.1"]
            """
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "implementation source"):
            DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_duplicate_module_source_and_block_ownership(self) -> None:
        other = self.root / "src/rrr/rpc/other.cpp"
        other.write_text("export module rrr.other;\n", encoding="utf-8")
        cases = [
            (
                "rrr.example",
                "src/rrr/src/example.rs",
                "src/rrr/rpc/other.cpp",
                "other.1",
                "duplicate cpp_module ownership",
            ),
            (
                "rrr.other",
                "src/rrr/src/other.rs",
                "src/rrr/rpc/example.cpp",
                "other.1",
                "duplicate source ownership",
            ),
            (
                "rrr.other",
                "src/rrr/src/other.rs",
                "src/rrr/rpc/other.cpp",
                "example.1",
                "block ID .* already owned",
            ),
        ]
        for cpp_module, output, source, block_id, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                self.write_manifest(
                    f"""\
                    schema_version = 1
                    [[module]]
                    cpp_module = "rrr.example"
                    output = "src/rrr/src/example.rs"
                    [[module.input]]
                    source = "src/rrr/rpc/example.cpp"
                    block_ids = ["example.1"]
                    [[module]]
                    cpp_module = "{cpp_module}"
                    output = "{output}"
                    [[module.input]]
                    source = "{source}"
                    block_ids = ["{block_id}"]
                    """
                )
                with self.assertRaisesRegex(DRIVER.ExtractionError, diagnostic):
                    DRIVER.load_manifest(self.root, self.manifest)

    def test_toolchain_verification_fails_closed_on_git_drift(self) -> None:
        required = DRIVER.REQUIRED_RUSTY_CPP_COMMIT
        gitlink = f"160000 {required} 0 third-party/rusty-cpp"
        cases = [
            (["160000 deadbeef 0 third-party/rusty-cpp"], [], "gitlink pin"),
            ([gitlink, "deadbeef"], [], "submodule HEAD"),
            ([gitlink, required, " M transpiler/src/main.rs"], [], "local changes"),
        ]
        for git_results, _, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                with mock.patch.object(
                    DRIVER, "git_output", side_effect=git_results
                ):
                    with self.assertRaisesRegex(
                        DRIVER.ExtractionError, diagnostic
                    ):
                        DRIVER.verify_pinned_toolchain(self.root, self.fake)

    def test_toolchain_verification_requires_exact_clean_build_info(self) -> None:
        required = DRIVER.REQUIRED_RUSTY_CPP_COMMIT
        gitlink = f"160000 {required} 0 third-party/rusty-cpp"
        cases = [
            (
                subprocess_result(2, "", "unsupported"),
                "build-info failed",
            ),
            (subprocess_result(0, "", ""), "exactly one JSON line"),
            (subprocess_result(0, "not-json\n", ""), "invalid JSON"),
            (subprocess_result(0, "{}\n", ""), "keys must be exactly"),
            (
                subprocess_result(
                    0,
                    json.dumps({"git_hash": "0" * 40, "git_dirty": False})
                    + "\n",
                    "",
                ),
                "build commit mismatch",
            ),
            (
                subprocess_result(
                    0,
                    json.dumps({"git_hash": required, "git_dirty": True}) + "\n",
                    "",
                ),
                "git_dirty=false",
            ),
            (
                subprocess_result(
                    0,
                    json.dumps({"git_hash": required, "git_dirty": "false"})
                    + "\n",
                    "",
                ),
                "git_dirty=false",
            ),
        ]
        for completed, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                with mock.patch.object(
                    DRIVER,
                    "git_output",
                    side_effect=[gitlink, required, ""],
                ), mock.patch.object(
                    DRIVER.subprocess, "run", return_value=completed
                ):
                    with self.assertRaisesRegex(
                        DRIVER.ExtractionError, diagnostic
                    ):
                        DRIVER.verify_pinned_toolchain(self.root, self.fake)

        good = subprocess_result(
            0,
            json.dumps({"git_hash": required, "git_dirty": False}) + "\n",
            "",
        )
        with mock.patch.object(
            DRIVER,
            "git_output",
            side_effect=[gitlink, required, ""],
        ), mock.patch.object(DRIVER.subprocess, "run", return_value=good) as run:
            DRIVER.verify_pinned_toolchain(self.root, self.fake)
        run.assert_called_once_with(
            [str(self.fake), "--build-info"],
            cwd=self.root,
            text=True,
            stdout=DRIVER.subprocess.PIPE,
            stderr=DRIVER.subprocess.PIPE,
            check=False,
        )


class CrateModeGateTests(unittest.TestCase):
    def test_gate_rechecks_extracted_rust_with_the_same_transpiler(self) -> None:
        root = Path("/repository")
        transpiler = Path("/tools/rusty-cpp-transpiler")
        with mock.patch.object(GATE, "run") as run:
            GATE.require_extraction_check(root, transpiler)
        run.assert_called_once_with(
            [
                sys.executable,
                GATE.EXTRACTION_DRIVER,
                "--check",
                "--transpiler",
                str(transpiler),
            ],
            root,
        )

    def test_extraction_drift_stops_gate_before_translation_setup(self) -> None:
        root = Path("/repository")
        transpiler = Path("/tools/rusty-cpp-transpiler")
        args = mock.Mock(
            transpiler=str(transpiler),
            clang="clang++",
            nm="nm",
            reference_object="reference.o",
        )
        with mock.patch.object(
            GATE, "repository_root", return_value=root
        ), mock.patch.object(
            GATE, "executable", return_value=transpiler
        ) as executable, mock.patch.object(
            GATE, "verify_pinned_toolchain"
        ) as verify, mock.patch.object(
            GATE,
            "require_extraction_check",
            side_effect=GATE.GateError("generated Rust output is stale"),
        ) as extraction:
            with self.assertRaisesRegex(GATE.GateError, "stale"):
                GATE.check(args)
        executable.assert_called_once_with(
            root, str(transpiler), "rusty-cpp transpiler"
        )
        verify.assert_called_once_with(root, transpiler)
        extraction.assert_called_once_with(root, transpiler)

    def test_gate_requires_exact_clean_transpiler_build_info(self) -> None:
        required = GATE.REQUIRED_RUSTY_CPP_COMMIT
        gitlink = f"160000 {required} 0 third-party/rusty-cpp"
        dirty = subprocess_result(
            0,
            json.dumps({"git_hash": required, "git_dirty": True}) + "\n",
            "",
        )
        with mock.patch.object(
            GATE,
            "git_output",
            side_effect=[gitlink, required, ""],
        ), mock.patch.object(GATE.subprocess, "run", return_value=dirty):
            with self.assertRaisesRegex(GATE.GateError, "git_dirty=false"):
                GATE.verify_pinned_toolchain(Path("/repository"), Path("/tool"))

        clean = subprocess_result(
            0,
            json.dumps({"git_hash": required, "git_dirty": False}) + "\n",
            "",
        )
        with mock.patch.object(
            GATE,
            "git_output",
            side_effect=[gitlink, required, ""],
        ), mock.patch.object(GATE.subprocess, "run", return_value=clean):
            GATE.verify_pinned_toolchain(Path("/repository"), Path("/tool"))


if __name__ == "__main__":
    unittest.main()
