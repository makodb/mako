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
import tomllib
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
    def test_connection_metrics_has_the_only_structured_module_preamble(self) -> None:
        with (REPOSITORY / "src/rrr/module-preambles.toml").open("rb") as stream:
            self.assertEqual(
                tomllib.load(stream),
                {
                    "version": 1,
                    "module": [
                        {
                            "name": "rrr.connection_metrics",
                            "includes": [
                                {
                                    "path": "rusty/sync/atomic.hpp",
                                    "form": "angle",
                                }
                            ],
                        }
                    ],
                },
            )

    def test_manifest_names_the_real_module_source_and_output(self) -> None:
        modules = DRIVER.load_manifest(
            REPOSITORY, REPOSITORY / "src/rrr/rust-extraction.toml"
        )
        self.assertEqual(
            [
                (
                    module.cpp_module,
                    module.rust_module,
                    module.output_label,
                    [
                        (source.source_label, source.block_ids)
                        for source in module.inputs
                    ],
                )
                for module in modules
            ],
            [
                (
                    "rrr.callback_wrapper",
                    "callback_wrapper",
                    "src/rrr/src/callback_wrapper.rs",
                    [
                        (
                            "src/rrr/base/callback_wrapper.cpp",
                            ("callback_wrapper.wrapper",),
                        )
                    ],
                ),
                (
                    "rrr.internal_protocol",
                    "internal_protocol",
                    "src/rrr/src/internal_protocol.rs",
                    [
                        (
                            "src/rrr/rpc/internal_protocol.cpp",
                            ("internal_protocol.1",),
                        )
                    ],
                ),
                (
                    "rrr.stat",
                    "stat",
                    "src/rrr/src/stat.rs",
                    [("src/rrr/misc/stat.cpp", ("stat.1",))],
                ),
                (
                    "rrr.errors",
                    "errors",
                    "src/rrr/src/errors.rs",
                    [
                        (
                            "src/rrr/rpc/errors.cpp",
                            (
                                "errors.error_category",
                                "errors.2",
                                "errors.rpc_error",
                                "errors.4",
                                "errors.get_error_category",
                                "errors.category_predicates",
                                "errors.is_retryable_error",
                            ),
                        )
                    ],
                ),
                (
                    "rrr.connection_metrics",
                    "connection_metrics",
                    "src/rrr/src/connection_metrics.rs",
                    [
                        (
                            "src/rrr/rpc/connection_metrics.cpp",
                            (
                                "connection_metrics.usings",
                                "connection_metrics.1",
                            ),
                        )
                    ],
                ),
            ],
        )

    def test_checked_in_payload_is_exactly_the_authored_inline_rust(self) -> None:
        cases = [
            (
                "rrr.callback_wrapper",
                "src/rrr/base/callback_wrapper.cpp",
                ("callback_wrapper.wrapper",),
                "src/rrr/src/callback_wrapper.rs",
            ),
            (
                "rrr.internal_protocol",
                "src/rrr/rpc/internal_protocol.cpp",
                ("internal_protocol.1",),
                "src/rrr/src/internal_protocol.rs",
            ),
            (
                "rrr.stat",
                "src/rrr/misc/stat.cpp",
                ("stat.1",),
                "src/rrr/src/stat.rs",
            ),
            (
                "rrr.errors",
                "src/rrr/rpc/errors.cpp",
                (
                    "errors.error_category",
                    "errors.2",
                    "errors.rpc_error",
                    "errors.4",
                    "errors.get_error_category",
                    "errors.category_predicates",
                    "errors.is_retryable_error",
                ),
                "src/rrr/src/errors.rs",
            ),
            (
                "rrr.connection_metrics",
                "src/rrr/rpc/connection_metrics.cpp",
                (
                    "connection_metrics.usings",
                    "connection_metrics.1",
                ),
                "src/rrr/src/connection_metrics.rs",
            ),
        ]
        for cpp_module, source_label, block_ids, output_label in cases:
            with self.subTest(cpp_module=cpp_module):
                source_path = REPOSITORY / source_label
                output_path = REPOSITORY / output_label
                source_bytes = source_path.read_bytes()
                source = source_bytes.decode("utf-8")
                header, payload = split_generated(output_path.read_bytes())
                expected = "\n\n".join(
                    source_block(source, block_id).rstrip("\n")
                    for block_id in block_ids
                ).encode("utf-8") + b"\n"

                self.assertEqual(payload, expected)
                self.assertIn(
                    f"// provenance-cpp-module: {cpp_module}", header
                )
                self.assertIn(
                    f"// provenance-input[0]-source: {source_label}", header
                )
                self.assertIn(
                    "// provenance-input[0]-block-ids: "
                    + ", ".join(block_ids),
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
                    "// provenance-rust-sha256: "
                    f"{hashlib.sha256(payload).hexdigest()}",
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
                "src/rrr/src/callback_wrapper.rs",
                "src/rrr/src/lib.rs",
                "src/rrr/src/internal_protocol.rs",
                "src/rrr/src/stat.rs",
                "src/rrr/src/errors.rs",
                "src/rrr/src/connection_metrics.rs",
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
    def test_executable_preserves_cxx_driver_symlink_spelling(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-driver-test-") as temporary:
            root = Path(temporary)
            real_driver = root / "clang-22"
            real_driver.write_text("", encoding="utf-8")
            real_driver.chmod(0o755)
            cxx_driver = root / "clang++"
            cxx_driver.symlink_to(real_driver.name)

            self.assertEqual(
                GATE.executable(root, str(cxx_driver), "Clang C++ compiler"),
                cxx_driver,
            )

    def test_symbol_owner_ignores_module_attachments_in_types(self) -> None:
        cases = {
            "rrr::f@rrr.errors(rrr::RpcError@rrr.errors)": "rrr.errors",
            "rrr::AvgStat@rrr.stat::avg() const": "rrr.stat",
            "rrr::value@rrr.internal_protocol": "rrr.internal_protocol",
            (
                "rrr::Facade<rrr::RpcError@rrr.errors>"
                "@rrr.client::call()"
            ): "rrr.client",
            "rrr::Facade<rrr::RpcError@rrr.errors>::call()": None,
            (
                "rrr::RpcError@rrr.errors "
                "rusty::clone<rrr::RpcError@rrr.errors>(int)"
            ): None,
            "rrr::Facade@rrr.types<int> rrr::make@rrr.errors<int>()": "rrr.errors",
            "rrr::operator<@rrr.errors(rrr::RpcError@rrr.errors, rrr::RpcError@rrr.errors)": "rrr.errors",
            "rrr::Widget@rrr.errors::operator bool() const": "rrr.errors",
            "rrr::Widget@rrr.errors::operator rrr::Facade@rrr.types<int>() const": "rrr.errors",
            "rrr::plain(int)": None,
        }
        for symbol, expected in cases.items():
            with self.subTest(symbol=symbol):
                self.assertEqual(GATE.symbol_owner_module(symbol), expected)

    def test_module_symbols_ratchets_only_strong_owned_definitions(self) -> None:
        output = "\n".join(
            [
                "0001 T rrr::f@rrr.errors(rrr::RpcError@rrr.errors)",
                "0002 W rrr::helper@rrr.errors()",
                "0003 t rrr::local@rrr.errors()",
                "0004 T rrr::other@rrr.client(rrr::RpcError@rrr.errors)",
                "0005 W rrr::RpcError@rrr.errors rusty::clone<int>(int)",
            ]
        )
        with mock.patch.object(GATE, "run", return_value=output):
            symbols = GATE.module_symbols(
                Path("/usr/bin/nm"),
                Path("/repository"),
                Path("/repository/librrr.a"),
                "rrr.errors",
            )
        self.assertEqual(
            symbols,
            {("T", "rrr::f@rrr.errors(rrr::RpcError@rrr.errors)")},
        )

    def test_connection_metrics_text_parity_rejects_method_body_drift(self) -> None:
        using_lines = textwrap.dedent(
            """\
            using rusty::sync::atomic::AtomicU64;

            using rusty::sync::atomic::Ordering;
            """
        ).strip()
        declaration = textwrap.dedent(
            """\
            struct ConnectionMetrics;

            struct ConnectionMetrics {
                static ConnectionMetrics new_();
            };
            """
        ).strip()
        bodies = textwrap.dedent(
            """\
            ConnectionMetrics ConnectionMetrics::new_() {
                return ConnectionMetrics{};
            }
            """
        ).strip()
        inline = (
            "/*RUSTYCPP:GEN-BEGIN id=connection_metrics.usings "
            "version=1 rust_sha256=x*/\n"
            f"{using_lines}\n"
            "/*RUSTYCPP:GEN-END id=connection_metrics.usings*/\n"
            "/*RUSTYCPP:GEN-BEGIN id=connection_metrics.1 "
            "version=1 rust_sha256=y*/\n"
            f"{declaration}\n\n{bodies}\n"
            "/*RUSTYCPP:GEN-END id=connection_metrics.1*/\n"
        )
        generated = (
            "export struct ConnectionMetrics;\n\n"
            f"{using_lines}\n\n"
            "export struct ConnectionMetrics {\n"
            "    static ConnectionMetrics new_();\n"
            "};\n\n"
            f"{bodies}\n\n"
            "} // namespace rrr\n"
        )

        with tempfile.TemporaryDirectory(prefix="rrr-parity-test-") as temporary:
            root = Path(temporary)
            source = root / "src/rrr/rpc/connection_metrics.cpp"
            source.parent.mkdir(parents=True)
            source.write_text(inline, encoding="utf-8")
            GATE.require_connection_metrics_text_parity(root, generated)
            with self.assertRaisesRegex(GATE.GateError, "method bodies differ"):
                GATE.require_connection_metrics_text_parity(
                    root, generated.replace("ConnectionMetrics{}", "ConnectionMetrics{1}")
                )

    def test_symbol_census_uses_the_definition_owner_not_parameter_types(self) -> None:
        owned = (
            "rrr::ConnectionMetrics@rrr.connection_metrics::reset() const"
        )
        foreign = (
            "rrr::Client@rrr.client::Client("
            "rrr::ConnectionMetrics@rrr.connection_metrics)"
        )
        nm_output = f"0001 T {owned}\n0002 T {foreign}\n"
        with mock.patch.object(GATE, "run", return_value=nm_output):
            self.assertEqual(
                GATE.module_symbols(
                    Path("/nm"),
                    Path("/repository"),
                    Path("/library.a"),
                    "rrr.connection_metrics",
                ),
                {("T", owned)},
            )

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
            reference_library="librrr.a",
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

    def test_build_tree_output_is_reused_without_second_generation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-reuse-test-") as temporary:
            root = Path(temporary)
            generated = root / "generated"
            generated.mkdir()
            reference = root / "inline.a"
            production = root / "production.a"
            reference.touch()
            production.touch()
            args = GATE.argparse.Namespace(
                transpiler="transpiler",
                clang="clang++",
                nm="nm",
                reference_library=str(reference),
                production_library=str(production),
                generated_dir=str(generated),
                runtime_library=[],
                cxx_flag=["-stdlib=libc++"],
                link_flag=["-lc++abi"],
            )
            modules = [mock.Mock(cpp_module="rrr.internal_protocol")]
            with mock.patch.object(
                GATE, "repository_root", return_value=root
            ), mock.patch.object(
                GATE,
                "executable",
                side_effect=[Path("/transpiler"), Path("/clang++"), Path("/nm")],
            ), mock.patch.object(
                GATE, "verify_pinned_toolchain"
            ), mock.patch.object(
                GATE, "require_extraction_check"
            ), mock.patch.object(
                GATE, "load_owned_modules", return_value=modules
            ), mock.patch.object(
                GATE, "check_generated_output"
            ) as check_output, mock.patch.object(
                GATE, "run"
            ) as run:
                GATE.check(args)

            run.assert_not_called()
            check_output.assert_called_once_with(
                root=root,
                output=generated,
                modules=modules,
                clang=Path("/clang++"),
                nm=Path("/nm"),
                reference=reference,
                production=production,
                runtime_libraries=[],
                cxx_flags=["-stdlib=libc++"],
                link_flags=["-lc++abi"],
            )

    def test_standalone_generation_consumes_the_structured_preamble(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-generate-test-") as temporary:
            root = Path(temporary)
            reference = root / "inline.a"
            reference.touch()
            args = GATE.argparse.Namespace(
                transpiler="transpiler",
                clang="clang++",
                nm="nm",
                reference_library=str(reference),
                production_library=None,
                generated_dir=None,
                runtime_library=[],
                cxx_flag=[],
                link_flag=[],
            )
            modules = [mock.Mock(cpp_module="rrr.internal_protocol")]
            with mock.patch.object(
                GATE, "repository_root", return_value=root
            ), mock.patch.object(
                GATE,
                "executable",
                side_effect=[Path("/transpiler"), Path("/clang++"), Path("/nm")],
            ), mock.patch.object(
                GATE, "verify_pinned_toolchain"
            ), mock.patch.object(
                GATE, "require_extraction_check"
            ), mock.patch.object(
                GATE, "load_owned_modules", return_value=modules
            ), mock.patch.object(
                GATE, "check_generated_output"
            ), mock.patch.object(GATE, "run") as run:
                GATE.check(args)

            command = run.call_args.args[0]
            self.assertEqual(command[-2:], ["--module-preamble", GATE.MODULE_PREAMBLE])

    def test_production_archive_cannot_be_its_own_reference(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-oracle-test-") as temporary:
            root = Path(temporary)
            archive = root / "librrr.a"
            archive.touch()
            args = GATE.argparse.Namespace(
                transpiler="transpiler",
                clang="clang++",
                nm="nm",
                reference_library=str(archive),
                production_library=str(archive),
                generated_dir=str(root),
                runtime_library=[],
                cxx_flag=[],
                link_flag=[],
            )
            with mock.patch.object(
                GATE, "repository_root", return_value=root
            ), mock.patch.object(
                GATE,
                "executable",
                side_effect=[Path("/transpiler"), Path("/clang++"), Path("/nm")],
            ), mock.patch.object(
                GATE, "verify_pinned_toolchain"
            ), mock.patch.object(
                GATE, "require_extraction_check"
            ), mock.patch.object(
                GATE, "load_owned_modules", return_value=[]
            ):
                with self.assertRaisesRegex(
                    GATE.GateError, "must be different artifacts"
                ):
                    GATE.check(args)

    def test_generated_gate_compiles_children_before_partial_root(self) -> None:
        modules = [
            mock.Mock(cpp_module="rrr.callback_wrapper"),
            mock.Mock(cpp_module="rrr.internal_protocol"),
            mock.Mock(cpp_module="rrr.stat"),
            mock.Mock(cpp_module="rrr.errors"),
            mock.Mock(cpp_module="rrr.connection_metrics"),
        ]

        def symbols_for_module(
            _nm: Path, _root: Path, _path: Path, module_name: str
        ) -> frozenset[tuple[str, str]]:
            return GATE.ABI_SPECS[module_name].symbols

        def compiled_object(
            _clang: Path,
            _root: Path,
            _include: Path,
            _source: Path,
            _work: Path,
            module_name: str,
            _cxx_flags: list[str],
        ) -> Path:
            return Path(f"/{module_name}.o")

        with tempfile.TemporaryDirectory(prefix="rrr-gate-children-test-") as temporary:
            output = Path(temporary)
            with mock.patch.object(
                GATE, "require_cpp_surfaces"
            ), mock.patch.object(
                GATE, "require_zero_hand_slots"
            ), mock.patch.object(
                GATE,
                "compile_module",
                side_effect=compiled_object,
            ) as compile_module, mock.patch.object(
                GATE, "run"
            ) as run, mock.patch.object(
                GATE, "module_symbols", side_effect=symbols_for_module
            ):
                GATE.check_generated_output(
                    root=Path("/repository"),
                    output=output,
                    modules=modules,
                    clang=Path("/clang++"),
                    nm=Path("/nm"),
                    reference=Path("/inline.a"),
                    production=Path("/production.a"),
                    runtime_libraries=[Path("/rusty.a")],
                    cxx_flags=["-stdlib=libc++"],
                    link_flags=["-lc++abi"],
                )

        compiled_names = [call.args[-2] for call in compile_module.call_args_list]
        self.assertEqual(
            compiled_names,
            [
                "rrr.callback_wrapper",
                "rrr.internal_protocol",
                "rrr.stat",
                "rrr.errors",
                "rrr.connection_metrics",
                "rrr",
            ],
        )
        importer_compile_commands = [
            call.args[0]
            for call in run.call_args_list
            if "-c" in call.args[0]
            and any(argument.endswith("/importer.cpp") for argument in call.args[0])
        ]
        self.assertEqual(len(importer_compile_commands), 1)
        self.assertIn("-I", importer_compile_commands[0])
        self.assertIn(
            "/repository/third-party/rusty-cpp/include",
            importer_compile_commands[0],
        )
        link_commands = [
            call.args[0]
            for call in run.call_args_list
            if "-o" in call.args[0]
            and any("importer-" in argument for argument in call.args[0])
        ]
        self.assertEqual(len(link_commands), 3)
        for command in link_commands:
            self.assertIn("-stdlib=libc++", command)
            self.assertIn("/rusty.a", command)
            self.assertIn("-lc++abi", command)
            if GATE.sys.platform.startswith("linux"):
                self.assertIn("-Wl,--start-group", command)
                self.assertIn("-Wl,--end-group", command)

    def test_gate_abi_ratchet_covers_every_manifest_module(self) -> None:
        root = Path("/repository")
        modules = [
            mock.Mock(cpp_module="rrr.callback_wrapper"),
            mock.Mock(cpp_module="rrr.internal_protocol"),
            mock.Mock(cpp_module="rrr.stat"),
            mock.Mock(cpp_module="rrr.errors"),
            mock.Mock(cpp_module="rrr.connection_metrics"),
        ]
        with mock.patch.object(
            GATE.extraction, "load_manifest", return_value=modules
        ) as load:
            self.assertEqual(GATE.load_owned_modules(root), modules)
        load.assert_called_once_with(root, root / GATE.EXTRACTION_MANIFEST)

        with mock.patch.object(
            GATE.extraction,
            "load_manifest",
            return_value=[*modules, mock.Mock(cpp_module="rrr.orphan")],
        ):
            with self.assertRaisesRegex(
                GATE.GateError, "missing ABI specification"
            ):
                GATE.load_owned_modules(root)


if __name__ == "__main__":
    unittest.main()
