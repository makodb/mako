#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import textwrap
import tomllib
import unittest
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
# `src/srpc` is the vendored srpc tree. Every crate-relative label the
# extraction driver owns resolves against this directory, exactly as it
# resolves against the repository root in standalone srpc.
CRATE = REPOSITORY / "src/srpc"
DRIVER_PATH = REPOSITORY / "scripts/extract_srpc_rust.py"
SPEC = importlib.util.spec_from_file_location("extract_srpc_rust", DRIVER_PATH)
assert SPEC is not None and SPEC.loader is not None
DRIVER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = DRIVER
SPEC.loader.exec_module(DRIVER)

GATE_PATH = REPOSITORY / "scripts/check_srpc_crate_mode.py"
GATE_SPEC = importlib.util.spec_from_file_location("check_srpc_crate_mode", GATE_PATH)
assert GATE_SPEC is not None and GATE_SPEC.loader is not None
GATE = importlib.util.module_from_spec(GATE_SPEC)
sys.modules[GATE_SPEC.name] = GATE
GATE_SPEC.loader.exec_module(GATE)


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
    def test_discarded_parallel_crate_stays_absent(self) -> None:
        self.assertFalse((REPOSITORY / "crates/srpc").exists())

    def test_retired_inline_carriers_stay_absent(self) -> None:
        retired = (
            "src/srpc/base/callback_wrapper.cpp",
            "src/srpc/rpc/internal_protocol.cpp",
            "src/srpc/misc/stat.cpp",
            "src/srpc/rpc/errors.cpp",
            "src/srpc/rpc/connection_metrics.cpp",
            "src/srpc/rpc/completion_tracker.cpp",
            "src/srpc/misc/rand.cpp",
            "src/srpc/rpc/request_options.cpp",
            "src/srpc/rpc/reconnect_policy.cpp",
            "src/srpc/rpc/circuit_breaker.cpp",
            "src/srpc/rpc/connection_state.cpp",
            "src/srpc/rpc/heartbeat.cpp",
            "src/srpc/base/basetypes.cpp",
            "src/srpc/rpc/request_queue.cpp",
            "src/srpc/rpc/load_balancer.cpp",
            "src/srpc/rpc/utils.cpp",
            "src/srpc/rpc/frame_codec.cpp",
        )
        self.assertTrue(all(not (REPOSITORY / path).exists() for path in retired))


    def test_manifest_names_the_canonical_rust_sources(self) -> None:
        modules = DRIVER.load_manifest(CRATE, CRATE / "rust-modules.toml")
        self.assertEqual(
            [
                (
                    module.cpp_module,
                    module.rust_module,
                    module.output_label,
                    module.canonical_source_label,
                )
                for module in modules
            ],
            [
                (
                    "srpc.basetypes",
                    "basetypes",
                    "base/basetypes.rs",
                    "base/basetypes.rs",
                ),
                (
                    "srpc.callback_wrapper",
                    "callback_wrapper",
                    "base/callback_wrapper.rs",
                    "base/callback_wrapper.rs",
                ),
                (
                    "srpc.internal_protocol",
                    "internal_protocol",
                    "rpc/internal_protocol.rs",
                    "rpc/internal_protocol.rs",
                ),
                (
                    "srpc.stat",
                    "stat",
                    "misc/stat.rs",
                    "misc/stat.rs",
                ),
                (
                    "srpc.errors",
                    "errors",
                    "rpc/errors.rs",
                    "rpc/errors.rs",
                ),
                (
                    "srpc.connection_metrics",
                    "connection_metrics",
                    "rpc/connection_metrics.rs",
                    "rpc/connection_metrics.rs",
                ),
                (
                    "srpc.completion_tracker",
                    "completion_tracker",
                    "rpc/completion_tracker.rs",
                    "rpc/completion_tracker.rs",
                ),
                (
                    "srpc.rand",
                    "rand",
                    "misc/rand.rs",
                    "misc/rand.rs",
                ),
                (
                    "srpc.request_options",
                    "request_options",
                    "rpc/request_options.rs",
                    "rpc/request_options.rs",
                ),
                (
                    "srpc.reconnect_policy",
                    "reconnect_policy",
                    "rpc/reconnect_policy.rs",
                    "rpc/reconnect_policy.rs",
                ),
                (
                    "srpc.circuit_breaker",
                    "circuit_breaker",
                    "rpc/circuit_breaker.rs",
                    "rpc/circuit_breaker.rs",
                ),
                (
                    "srpc.connection_state",
                    "connection_state",
                    "rpc/connection_state.rs",
                    "rpc/connection_state.rs",
                ),
                (
                    "srpc.heartbeat",
                    "heartbeat",
                    "rpc/heartbeat.rs",
                    "rpc/heartbeat.rs",
                ),
                (
                    "srpc.request_queue",
                    "request_queue",
                    "rpc/request_queue.rs",
                    "rpc/request_queue.rs",
                ),
                (
                    "srpc.load_balancer",
                    "load_balancer",
                    "rpc/load_balancer.rs",
                    "rpc/load_balancer.rs",
                ),
                (
                    "srpc.utils",
                    "utils",
                    "rpc/utils.rs",
                    "rpc/utils.rs",
                ),
                (
                    "srpc.frame_codec",
                    "frame_codec",
                    "rpc/frame_codec.rs",
                    "rpc/frame_codec.rs",
                ),
                (
                    "srpc.serializable",
                    "serializable",
                    "misc/serializable.rs",
                    "misc/serializable.rs",
                ),
                (
                    "srpc.serializable_envelope",
                    "serializable_envelope",
                    "misc/serializable_envelope.rs",
                    "misc/serializable_envelope.rs",
                ),
                (
                    "srpc.future",
                    "future",
                    "reactor/future.rs",
                    "reactor/future.rs",
                ),
                (
                    "srpc.logging",
                    "logging",
                    "base/logging.rs",
                    "base/logging.rs",
                ),
                (
                    "srpc.idempotency",
                    "idempotency",
                    "rpc/idempotency.rs",
                    "rpc/idempotency.rs",
                ),
                (
                    "srpc.fiber",
                    "fiber",
                    "reactor/fiber.rs",
                    "reactor/fiber.rs",
                ),
                (
                    "srpc.misc",
                    "misc",
                    "base/misc.rs",
                    "base/misc.rs",
                ),
                (
                    "srpc.channel",
                    "channel",
                    "rpc/channel.rs",
                    "rpc/channel.rs",
                ),
                (
                    "srpc.epoll_wrapper",
                    "epoll_wrapper",
                    "reactor/epoll_wrapper.rs",
                    "reactor/epoll_wrapper.rs",
                ),
                (
                    "srpc.pollable_proxy",
                    "pollable_proxy",
                    "rpc/pollable_proxy.rs",
                    "rpc/pollable_proxy.rs",
                ),
                (
                    "srpc.callbacks",
                    "callbacks",
                    "rpc/callbacks.rs",
                    "rpc/callbacks.rs",
                ),
                (
                    "srpc.inmemory_channel",
                    "inmemory_channel",
                    "rpc/inmemory_channel.rs",
                    "rpc/inmemory_channel.rs",
                ),
                (
                    "srpc.fiber_channel",
                    "fiber_channel",
                    "rpc/fiber_channel.rs",
                    "rpc/fiber_channel.rs",
                ),
                (
                    "srpc.threading",
                    "threading",
                    "base/threading.rs",
                    "base/threading.rs",
                ),
                (
                    "srpc.debugging",
                    "debugging",
                    "base/debugging.rs",
                    "base/debugging.rs",
                ),
                (
                    "srpc.any_message",
                    "any_message",
                    "misc/any_message.rs",
                    "misc/any_message.rs",
                ),
                (
                    "srpc.tcp_channel",
                    "tcp_channel",
                    "rpc/tcp_channel.rs",
                    "rpc/tcp_channel.rs",
                ),
                (
                    "srpc.reactor",
                    "reactor",
                    "reactor/reactor.rs",
                    "reactor/reactor.rs",
                ),
                (
                    "srpc.server",
                    "server",
                    "rpc/server.rs",
                    "rpc/server.rs",
                ),
                (
                    "srpc.client",
                    "client",
                    "rpc/client.rs",
                    "rpc/client.rs",
                ),
            ],
        )

    def test_cmake_provider_inventory_matches_the_canonical_manifest(self) -> None:
        modules = DRIVER.load_manifest(CRATE, CRATE / "rust-modules.toml")
        cmake = (REPOSITORY / "src/srpc-cmake/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        match = re.search(
            r"set\(SRPC_GOAL0_CANONICAL_MODULES\n(?P<body>.*?)\n\)",
            cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        assert match is not None
        cmake_modules = tuple(
            line.strip()
            for line in match.group("body").splitlines()
            if line.strip()
        )
        manifest_modules = tuple(module.rust_module for module in modules)
        self.assertEqual(cmake_modules, manifest_modules)
        self.assertEqual(len(cmake_modules), len(set(cmake_modules)))
        # Canonical sources mirror the C++ layout now, so CMake carries the
        # relative paths explicitly instead of deriving `src/<name>.rs`. The
        # list must equal the manifest's, in manifest order.
        relpath_match = re.search(
            r"set\(SRPC_GOAL0_CANONICAL_SOURCE_RELPATH\n(?P<body>.*?)\n\)",
            cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(relpath_match)
        assert relpath_match is not None
        cmake_relpaths = tuple(
            line.strip()
            for line in relpath_match.group("body").splitlines()
            if line.strip()
        )
        self.assertEqual(
            cmake_relpaths,
            tuple(
                module.canonical_source_label for module in modules
            ),
        )
        self.assertIn(
            "${SRPC_SOURCE_DIR}/${_SRPC_GOAL0_RELPATH}",
            cmake,
        )
        self.assertIn(
            "${SRPC_GOAL0_CRATE_CPP_DIR}/srpc.${_SRPC_GOAL0_MODULE}.cppm",
            cmake,
        )
        for fragment in (
            'set(SRPC_GOAL0_TYPE_MAP\n    ${SRPC_SOURCE_DIR}/rust-type-map.toml',
            'set(SRPC_GOAL0_CPP_MODULE_INDEX\n    ${SRPC_SOURCE_DIR}/cpp-module-index.toml',
            '--type-map "${SRPC_GOAL0_TYPE_MAP}"',
            '--cpp-module-index "${SRPC_GOAL0_CPP_MODULE_INDEX}"',
            '"${SRPC_GOAL0_TYPE_MAP}"',
            '"${SRPC_GOAL0_CPP_MODULE_INDEX}"',
        ):
            self.assertIn(fragment, cmake)

    def test_flat_import_namespace_is_declared_and_passed_to_crate_mode(
        self,
    ) -> None:
        """The manifest key and the emitter flag must agree.

        The seventeen canonical sources carry NO per-item
        `cpp_import_namespace` marker: every private
        `use crate::<child>::<Name leaves>;` gets its contract from the
        crate-level namespace instead. That inference only happens when the
        emitter is actually invoked with `--flat-import-namespace`, so a
        manifest key with no flag (or a flag with no key) would silently
        change what the generated providers mean.
        """

        manifest = CRATE / "rust-modules.toml"
        self.assertEqual(
            DRIVER.load_flat_import_namespace(CRATE, manifest), "srpc"
        )
        cmake = (REPOSITORY / "src/srpc-cmake/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn("--flat-import-namespace srpc", cmake)
        gate = (REPOSITORY / "scripts/check_srpc_crate_mode.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("extraction.load_flat_import_namespace(", gate)
        self.assertIn('["--flat-import-namespace", flat_import_namespace]', gate)
        self.assertIn("*flat_import_arguments,", gate)
        for source in (
            module.output
            for module in DRIVER.load_manifest(CRATE, manifest)
        ):
            # The invariant is "no per-item MARKER", i.e. no
            # `#[cfg_attr(any(), cpp_import_namespace(...))]` attribute. Scan
            # code only: `channel.rs` explains the emitter's leaf contract in
            # a doc comment, and prose naming the mechanism is not a marker.
            code = "\n".join(
                line
                for line in source.read_text(encoding="utf-8").splitlines()
                if not line.lstrip().startswith("//")
            )
            self.assertNotIn(
                "cpp_import_namespace",
                code,
                msg=f"{source} still carries a per-item marker",
            )

    def test_manifest_rejects_an_unknown_top_level_key(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / "rust-modules.toml"
            manifest.write_text(
                'schema_version = 2\nstray = "x"\n'
                '[[module]]\ncpp_module = "srpc.example"\n'
                'source = "src/srpc/src/example.rs"\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                DRIVER.ExtractionError, "manifest keys must be exactly"
            ):
                DRIVER.load_manifest(root, manifest)

    def test_manifest_rejects_a_non_namespace_flat_import_value(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / "rust-modules.toml"
            manifest.write_text(
                'schema_version = 2\nflat_import_namespace = "not a ns"\n'
                '[[module]]\ncpp_module = "srpc.example"\n'
                'source = "src/srpc/src/example.rs"\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                DRIVER.ExtractionError,
                "flat_import_namespace must be a C\\+\\+ namespace path",
            ):
                DRIVER.load_manifest(root, manifest)

        workflow = (REPOSITORY / ".github/workflows/ci.yml").read_text(
            encoding="utf-8"
        )
        # Pin the INVARIANT, not the literal path. This used to hardcode
        # `canonical_input="src/srpc/src/frame_codec.rs"`; the srpc graft moved
        # canonical Rust to its layout-mirroring paths and the stale name made
        # the CI step `touch` a nonexistent file -- which CREATES it, planting
        # an unmanifested .rs in the crate and killing the next build with no
        # output. Assert instead that whatever path the step names is a real
        # canonical module, so a future move fails here with a clear reason.
        canonical_match = re.search(
            r'canonical_input="([^"]+)"', workflow
        )
        self.assertIsNotNone(
            canonical_match, "ci.yml no longer defines canonical_input"
        )
        canonical_rel = canonical_match.group(1)
        self.assertTrue(
            canonical_rel.startswith("src/srpc/")
            and canonical_rel.endswith(".rs"),
            f"canonical_input is not a canonical Rust source: {canonical_rel}",
        )
        self.assertTrue(
            (REPOSITORY / canonical_rel).is_file(),
            f"canonical_input does not exist: {canonical_rel}",
        )
        # These used to pin the `<name>_generated_path=` / `..._before=$(stat
        # -c %Y ...)` / `test ... -gt ...` machinery of the determinism step.
        # That machinery is gone: it asserted outputs were REWRITTEN, which the
        # emitters deliberately do not do for byte-identical content, so it
        # failed CI on a correct build. The coverage it was protecting -- that
        # the facade and sidecar sub-checks still watch utils and frame_codec --
        # is now pinned directly on the disposition assertions that replaced it.
        self.assertIn(
            "scripts/ci/assert_crate_codegen.sh", workflow
        )
        for generated in ("srpc.utils.cppm", "srpc.frame_codec.cppm"):
            self.assertIn(generated, workflow)
        # This used to pin `test_rpc_tcp_channel`, one of the nineteen
        # `src/srpc/tests/` binaries the Goal-0 job named. 4a06ef0e stopped
        # building that corpus (it is srpc's, and runs in srpc's CI), so the
        # canary was pinning a target that no longer exists. What is worth
        # pinning is the property that made the breakage survivable-but-silent:
        # `ctest -R` exits 0 when its pattern matches nothing, so without this
        # flag a stale name list goes green having run zero tests. See
        # GoalZeroConsumerSelectionTests for the list-agreement checks.
        self.assertIn("--no-tests=error", workflow)
        self.assertIn('type_map_input="src/srpc/rust-type-map.toml"', workflow)
        self.assertIn(
            'module_index_input="src/srpc/cpp-module-index.toml"', workflow
        )
        self.assertIn(
            'for sidecar_input in "${type_map_input}" "${module_index_input}"',
            workflow,
        )

    def test_checked_in_modules_are_canonical_rust_sources(self) -> None:
        modules = DRIVER.load_manifest(CRATE, CRATE / "rust-modules.toml")
        # `src/srpc` is now srpc's tree byte for byte, so the two-line
        # "// Canonical Rust source for the srpc.X module." banner is gone from
        # the seventeen sources srpc never carried it on. Nothing is dropped:
        # the banner only claimed ownership and location, and BOTH facts are
        # now enforced structurally and more tightly --
        #   * the driver already pins the file to an approved production root
        #     with a basename equal to the module (validate_production_source_path),
        #   * and the generated crate index must reach exactly this file
        #     through a `#[path]` attribute, checked here.
        # The "not a generated artifact" half of the banner's job stays as the
        # explicit marker assertions.
        library = (REPOSITORY / "src/srpc/src/lib.rs").read_text(encoding="utf-8")
        canonical_lines = 0
        for module in modules:
            with self.subTest(cpp_module=module.cpp_module):
                source = module.output.read_text(encoding="utf-8")
                self.assertIsNotNone(module.canonical_source_label)
                value = DRIVER.module_path_attribute_value(
                    module.canonical_source_label
                )
                self.assertIn(
                    f'#[path = "{value}"]\npub mod {module.rust_module};',
                    library,
                )
                self.assertEqual(
                    (REPOSITORY / "src/srpc/src" / value).resolve(),
                    module.output.resolve(),
                )
                self.assertNotIn("@generated", source)
                self.assertNotIn("provenance-input", source)
                canonical_lines += sum(
                    bool(line.strip()) and not line.lstrip().startswith("//")
                    for line in source.splitlines()
                )
        self.assertEqual(len(modules), 37)

    def test_toolchain_pin_matches_the_vendored_gate(self) -> None:
        upstream = (CRATE / "scripts/extract_srpc_rust.py").read_text()
        self.assertIn(f'REQUIRED_RUSTY_CPP_COMMIT = "{DRIVER.REQUIRED_RUSTY_CPP_COMMIT}"', upstream)
        self.assertEqual(GATE.REQUIRED_RUSTY_CPP_COMMIT, DRIVER.REQUIRED_RUSTY_CPP_COMMIT)
        self.assertEqual(len(GATE.load_owned_modules(REPOSITORY)), 37)


    def test_canonical_source_validation_never_normalizes_owned_bytes(self) -> None:
        payload = b"pub fn canonical() {}\n\n"
        self.assertIs(
            DRIVER.validate_canonical_source(payload, "src/srpc/src/example.rs"),
            payload,
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "LF line endings"):
            DRIVER.validate_canonical_source(
                b"pub fn canonical() {}\r\n", "src/srpc/src/example.rs"
            )

    def test_write_never_replaces_a_canonical_source_snapshot(self) -> None:
        with tempfile.TemporaryDirectory(prefix="srpc-canonical-write-") as temporary:
            root = Path(temporary)
            # A canonical source lives at its layout-mirroring path, not in
            # src/. src/ holds only the generated crate index, and the census
            # rejects anything else that appears there.
            source = root / "rpc/example.rs"
            source.parent.mkdir(parents=True)
            (root / "src").mkdir(parents=True)
            original = b"pub fn canonical() -> i32 { 1 }\n"
            changed = b"pub fn canonical() -> i32 { 2 }\n"
            source.write_bytes(original)
            generated = [
                DRIVER.GeneratedFile(
                    output_label="rpc/example.rs",
                    output=source,
                    content=original,
                    writable=False,
                )
            ]
            source.write_bytes(changed)
            with self.assertRaisesRegex(
                DRIVER.ExtractionError, "refusing to overwrite"
            ):
                DRIVER.apply_mode(root, generated, "write")
            self.assertEqual(source.read_bytes(), changed)

    def test_lib_is_manifest_generated_and_census_has_no_orphans(self) -> None:
        manifest = CRATE / "rust-modules.toml"
        modules = DRIVER.load_manifest(CRATE, manifest)
        expected_lib = DRIVER.render_lib("rust-modules.toml", manifest, modules)
        self.assertEqual(
            (REPOSITORY / "src/srpc/src/lib.rs").read_bytes(),
            expected_lib,
        )
        self.assertEqual(
            DRIVER.rust_source_census(CRATE),
            {
                "src/lib.rs",
            },
        )


class DriverBehaviorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="srpc-extractor-test-")
        self.root = Path(self.temporary.name)
        source_root = self.root / "rpc"
        source_root.mkdir(parents=True)
        self.interface = source_root / "example.cpp"
        self.interface.write_text(
            textwrap.dedent(
                """\
                module;
                export module srpc.example;

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
                module srpc.example;

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
        self.manifest = self.root / "rust-extraction.toml"
        self.write_manifest(
            """\
            schema_version = 1

            [[module]]
            cpp_module = "srpc.example"
            output = "src/example.rs"

            [[module.input]]
            source = "rpc/example.cpp"
            block_ids = ["example.2", "example.1"]

            [[module.input]]
            source = "rpc/example_impl.cc"
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
                "rust-extraction.toml",
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
                    "rpc/example.cpp",
                ],
            )
            self.assertEqual(
                history[offset + 1][3:],
                [
                    "--block-id",
                    "example.impl",
                    "--files",
                    "rpc/example_impl.cc",
                ],
            )

    def test_two_sources_are_concatenated_in_manifest_order(self) -> None:
        generated = self.generate()
        module = generated_by_label(generated, "src/example.rs")
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
        output = self.root / "src/example.rs"
        output.write_text("tampered\n", encoding="utf-8")

        with self.assertRaisesRegex(DRIVER.ExtractionError, "stale"):
            DRIVER.apply_mode(self.root, generated, "check")
        self.assertEqual(output.read_text(encoding="utf-8"), "tampered\n")

    def test_check_and_write_reject_orphan_rust_sources(self) -> None:
        generated = self.generate()
        DRIVER.apply_mode(self.root, generated, "write")
        orphan = self.root / "src/orphan.rs"
        orphan.write_text("parallel implementation\n", encoding="utf-8")
        for mode in ("check", "write"):
            with self.subTest(mode=mode):
                with self.assertRaisesRegex(DRIVER.ExtractionError, "orphan"):
                    DRIVER.apply_mode(self.root, generated, mode)
        self.assertEqual(orphan.read_text(), "parallel implementation\n")

    def test_check_rejects_stale_and_missing_generated_lib(self) -> None:
        generated = self.generate()
        DRIVER.apply_mode(self.root, generated, "write")
        lib = generated_by_label(generated, "src/lib.rs")
        lib.output.write_text("stale lib\n", encoding="utf-8")
        with self.assertRaisesRegex(DRIVER.ExtractionError, "stale"):
            DRIVER.apply_mode(self.root, generated, "check")
        DRIVER.apply_mode(self.root, generated, "write")
        lib.output.unlink()
        with self.assertRaisesRegex(DRIVER.ExtractionError, "missing"):
            DRIVER.apply_mode(self.root, generated, "check")

    def test_output_symlink_is_rejected_at_load_and_before_write(self) -> None:
        generated = self.generate()
        output = self.root / "src/example.rs"
        output.parent.mkdir(parents=True)
        victim = self.root / "victim.rs"
        victim.write_text("do not overwrite\n", encoding="utf-8")
        output.symlink_to("../victim.rs")

        with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
            DRIVER.load_manifest(self.root, self.manifest)
        with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
            DRIVER.apply_mode(self.root, generated, "write")
        self.assertEqual(victim.read_text(encoding="utf-8"), "do not overwrite\n")

    def test_manifest_file_and_parent_symlinks_are_rejected_before_read(self) -> None:
        file_link = self.root / "manifest-link.toml"
        file_link.symlink_to(self.manifest)
        parent_link = self.root / "manifest-parent-link"
        parent_link.symlink_to(self.manifest.parent, target_is_directory=True)

        for manifest in (file_link, parent_link / self.manifest.name):
            with self.subTest(manifest=manifest):
                with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
                    DRIVER.load_manifest(self.root, manifest)

    def test_output_parent_symlink_is_rejected_at_load_and_before_census(self) -> None:
        generated = self.generate()
        victim = self.root / "generated-victim"
        victim.mkdir(parents=True)
        marker = victim / "marker"
        marker.write_text("do not touch\n", encoding="utf-8")
        (self.root / "src").symlink_to(
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
                "source = \"rpc/example.cpp\"\n"
                "block_ids = []",
                "block_ids must be a non-empty",
            ),
            (
                "[[module.input]]\n"
                "source = \"rpc/example.cpp\"\n"
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
                    cpp_module = "srpc.example"
                    output = "src/example.rs"
                    {input_body}
                    """
                )
                with self.assertRaisesRegex(DRIVER.ExtractionError, diagnostic):
                    DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_reserves_generated_lib_from_module_ownership(self) -> None:
        self.write_manifest(
            """\
            schema_version = 1
            [[module]]
            cpp_module = "srpc.lib"
            output = "src/lib.rs"
            [[module.input]]
            source = "rpc/example.cpp"
            block_ids = ["example.1"]
            """
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "lib.rs is reserved"):
            DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_module_source_and_output_mismatches(self) -> None:
        cases = [
            (
                "srpc.other",
                "src/other.rs",
                "rpc/example.cpp",
                "example.1",
                "interface source .* must contain exactly",
            ),
            (
                "srpc.example",
                "src/wrong.rs",
                "rpc/example.cpp",
                "example.1",
                "output does not match cpp_module",
            ),
            (
                "srpc.example",
                "src/example.rs",
                "rpc/example_impl.cc",
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
        outside = self.root / "tests/example.cpp"
        outside.parent.mkdir(parents=True)
        outside.write_bytes(self.interface.read_bytes())
        self.write_manifest(
            """\
            schema_version = 1
            [[module]]
            cpp_module = "srpc.example"
            output = "src/example.rs"
            [[module.input]]
            source = "tests/example.cpp"
            block_ids = ["example.1"]
            """
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "approved production"):
            DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_source_file_and_parent_symlinks(self) -> None:
        source_link = self.root / "rpc/source_link.cpp"
        source_link.symlink_to("example.cpp")
        parent_link = self.root / "base"
        parent_link.symlink_to("rpc", target_is_directory=True)
        cases = [
            "rpc/source_link.cpp",
            "base/example.cpp",
        ]
        for source in cases:
            with self.subTest(source=source):
                self.write_manifest(
                    f"""\
                    schema_version = 1
                    [[module]]
                    cpp_module = "srpc.example"
                    output = "src/example.rs"
                    [[module.input]]
                    source = "{source}"
                    block_ids = ["example.1"]
                    """
                )
                with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
                    DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_wrong_implementation_module(self) -> None:
        wrong = self.root / "rpc/wrong_impl.cc"
        wrong.write_text("module srpc.other;\n", encoding="utf-8")
        self.write_manifest(
            """\
            schema_version = 1
            [[module]]
            cpp_module = "srpc.example"
            output = "src/example.rs"
            [[module.input]]
            source = "rpc/example.cpp"
            block_ids = ["example.1"]
            [[module.input]]
            source = "rpc/wrong_impl.cc"
            block_ids = ["wrong.1"]
            """
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "implementation source"):
            DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_duplicate_module_source_and_block_ownership(self) -> None:
        other = self.root / "rpc/other.cpp"
        other.write_text("export module srpc.other;\n", encoding="utf-8")
        cases = [
            (
                "srpc.example",
                "src/example.rs",
                "rpc/other.cpp",
                "other.1",
                "duplicate cpp_module ownership",
            ),
            (
                "srpc.other",
                "src/other.rs",
                "rpc/example.cpp",
                "other.1",
                "duplicate source ownership",
            ),
            (
                "srpc.other",
                "src/other.rs",
                "rpc/other.cpp",
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
                    cpp_module = "srpc.example"
                    output = "src/example.rs"
                    [[module.input]]
                    source = "rpc/example.cpp"
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
        # The Verus erasure coupling runs after build-info and has its own
        # test (test_srpc_crate_mode.py); keep it out of these cases.
        for completed, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                with mock.patch.object(
                    DRIVER,
                    "git_output",
                    side_effect=[gitlink, required, ""],
                ), mock.patch.object(
                    DRIVER.subprocess, "run", return_value=completed
                ), mock.patch.object(DRIVER, "verify_verus_erasure_coupling"):
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
        ), mock.patch.object(DRIVER.subprocess, "run", return_value=good) as run, \
                mock.patch.object(DRIVER, "verify_verus_erasure_coupling") as coupling:
            DRIVER.verify_pinned_toolchain(self.root, self.fake)
        run.assert_called_once_with(
            [str(self.fake), "--build-info"],
            cwd=self.root,
            text=True,
            stdout=DRIVER.subprocess.PIPE,
            stderr=DRIVER.subprocess.PIPE,
            check=False,
        )
        # Cargo.lock is crate content: the coupling reads the vendored tree's.
        coupling.assert_called_once_with(DRIVER.crate_root(self.root), self.fake)


class GoalZeroConsumerSelectionTests(unittest.TestCase):
    """The Goal-0 job builds a list of consumer targets and then runs a ctest
    pattern naming the same tests. Both lists were left naming `src/srpc/tests/`
    binaries after 4a06ef0e deleted them, which failed the job with
    `ninja: error: unknown target 'test_timer'`. Pin the two properties that
    turn that class of drift into a local failure instead of a red CI run.
    """

    @staticmethod
    def goal_zero_steps() -> tuple[str, str]:
        workflow = (REPOSITORY / ".github/workflows/ci.yml").read_text(
            encoding="utf-8"
        )
        build = re.search(r"--target ([^\n]*?)\n\s*-- -k 0", workflow)
        assert build is not None, "Goal-0 build step lost its --target list"
        selection = re.search(r"-R '\^\((.*?)\)\$'", workflow)
        assert selection is not None, "Goal-0 ctest step lost its -R pattern"
        return build.group(1), selection.group(1)

    def test_built_targets_and_selected_tests_agree(self) -> None:
        built, selected = self.goal_zero_steps()
        targets = [
            target
            for target in built.split()
            if target != "srpc_goal0_dual_compile"
        ]
        self.assertTrue(targets, "Goal-0 builds no consumer targets")
        self.assertEqual(sorted(targets), sorted(selected.split("|")))

    def test_selected_tests_are_targets_this_repository_defines(self) -> None:
        """The exact check the broken workflow would have failed: every name
        must be an `add_executable` in mako's own CMakeLists."""
        _, selected = self.goal_zero_steps()
        cmake = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")
        defined = set(re.findall(r"add_executable\(\s*([A-Za-z0-9_]+)", cmake))
        for name in selected.split("|"):
            with self.subTest(target=name):
                self.assertIn(name, defined)


class ForeignOwnedCheckoutTests(unittest.TestCase):
    """The pin attestation runs in a container whose checkout belongs to a
    different uid than the process, so bare `git` answers "detected dubious
    ownership" instead of reading the repository. `GIT_TEST_ASSUME_DIFFERENT_-
    OWNER` reproduces exactly that condition without needing a second uid.
    """

    # Build the fixture with the knob explicitly OFF. These tests must *own*
    # the ownership condition, not inherit it: if the variable is already set
    # in the environment (e.g. someone reproducing a CI failure by exporting
    # it for a whole gate run), bare git would disown even the scratch repo
    # this setUp just created and every test here would error in setUp instead
    # of testing anything.
    NATIVE_ENV = {
        key: value
        for key, value in os.environ.items()
        if key != "GIT_TEST_ASSUME_DIFFERENT_OWNER"
    }

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="srpc-ownership-")
        self.addCleanup(temporary.cleanup)
        self.repository = Path(temporary.name) / "checkout"
        self.repository.mkdir()
        for arguments in (
            ["init", "--quiet", "."],
            ["config", "user.name", "gate"],
            ["config", "user.email", "gate@example.invalid"],
            ["commit", "--quiet", "--allow-empty", "-m", "seed"],
        ):
            subprocess.run(
                ["git", *arguments],
                cwd=self.repository,
                env=self.NATIVE_ENV,
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        self.head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.repository,
            env=self.NATIVE_ENV,
            text=True,
            stdout=subprocess.PIPE,
            check=True,
        ).stdout.strip()

    def test_bare_git_really_is_refused(self) -> None:
        """Guard the guard: if this ever stops failing, the tests below stop
        proving anything, because they would pass without the exception."""
        completed = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.repository,
            env=dict(os.environ, GIT_TEST_ASSUME_DIFFERENT_OWNER="1"),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("dubious ownership", completed.stderr)

    def test_git_output_reads_a_foreign_owned_checkout(self) -> None:
        with mock.patch.dict(
            os.environ, {"GIT_TEST_ASSUME_DIFFERENT_OWNER": "1"}
        ):
            for module in (DRIVER, GATE):
                with self.subTest(module=module.__name__):
                    self.assertEqual(
                        module.git_output(
                            self.repository, ["rev-parse", "HEAD"], "probe"
                        ),
                        self.head,
                    )

    def test_ownership_exception_names_only_the_inspected_directory(self) -> None:
        flags = DRIVER.ownership_exception(self.repository)
        self.assertEqual(flags[::2], ["-c"] * (len(flags) // 2))
        self.assertEqual(
            {flag.removeprefix("safe.directory=") for flag in flags[1::2]},
            {str(self.repository), str(self.repository.resolve())},
        )
        # A blanket "trust everything" opt-out would also silence genuine
        # ownership problems in unrelated repositories.
        self.assertNotIn("safe.directory=*", flags)

    def test_repository_scripts_that_shell_out_to_git_survive(self) -> None:
        """Every script the source gate runs must tolerate a foreign-owned
        checkout, not just the pin attestation. `srpc_handwritten_census.py`
        did not, and CI died with `git ls-files ... exit status 128` once the
        attestation stopped failing first and stopped masking it."""
        if not (REPOSITORY / ".git").exists():
            self.skipTest("not a git checkout")
        completed = subprocess.run(
            [sys.executable, "scripts/srpc_handwritten_census.py"],
            cwd=REPOSITORY,
            env=dict(os.environ, GIT_TEST_ASSUME_DIFFERENT_OWNER="1"),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(
            completed.returncode, 0, msg=completed.stdout + completed.stderr
        )
        self.assertIn("source boundary:", completed.stdout)

    def test_pin_attestation_still_fails_closed_on_a_foreign_checkout(self) -> None:
        """Relaxing git's ownership heuristic must not relax the pin itself:
        against the real repository, a wrong required commit is still caught."""
        if not (REPOSITORY / ".git").exists():
            self.skipTest("not a git checkout")
        cases = (
            (DRIVER, DRIVER.ExtractionError),
            (GATE, GATE.GateError),
        )
        with mock.patch.dict(
            os.environ, {"GIT_TEST_ASSUME_DIFFERENT_OWNER": "1"}
        ):
            for module, failure in cases:
                with self.subTest(module=module.__name__):
                    with mock.patch.object(
                        module, "REQUIRED_RUSTY_CPP_COMMIT", "0" * 40
                    ):
                        with self.assertRaisesRegex(
                            failure, "gitlink pin mismatch"
                        ):
                            module.verify_pinned_toolchain(
                                REPOSITORY, Path("/nonexistent-transpiler")
                            )


class CrateCodegenAssertionTests(unittest.TestCase):
    def test_ci_invalidation_probes_name_existing_inputs(self) -> None:
        workflow = (REPOSITORY / ".github/workflows/ci.yml").read_text()
        inputs = dict(re.findall(
            r'(canonical_input|crate_manifest_input|type_map_input|module_index_input)="([^"$]+)"',
            workflow,
        ))
        self.assertEqual(set(inputs), {
            "canonical_input", "crate_manifest_input", "type_map_input", "module_index_input",
        })
        for name, relative in inputs.items():
            with self.subTest(input=name):
                self.assertTrue((REPOSITORY / relative).is_file(), relative)
        self.assertNotIn("rusty-rustc/", workflow)

    """`scripts/ci/assert_crate_codegen.sh` replaced the CI step's output-mtime
    assertions, which were measuring "the file was rewritten" -- only true by
    accident, since the emitters skip writing byte-identical output. These
    tests pin the replacement, and in particular that it is not weaker: it must
    still FAIL when regeneration genuinely does not happen.
    """

    SCRIPT = REPOSITORY / "scripts/ci/assert_crate_codegen.sh"

    # A faithful sample of what `cmake --build --target srpc_goal0_crate_codegen`
    # prints when it really regenerates (trimmed to the lines that matter).
    REGENERATED = """\
[1/4] Building rusty-cpp-transpiler...
   Compiling rusty-cpp-transpiler v0.1.0 (/w/third-party/rusty-cpp/transpiler)
    Finished `release` profile [optimized] target(s) in 6m 23s
[2/4] Fingerprinting the Goal-0 rusty-cpp emitter
[3/4] Generating Goal-0 srpc crate C++ child modules
Transpiling crate 'srpc' (38 source files)
  src/frame_codec.rs → srpc.frame_codec.cppm (module: srpc.frame_codec)
  src/utils.rs → srpc.utils.cppm (module: srpc.utils)
  src/request_queue.rs → srpc.request_queue.cppm (module: srpc.request_queue)
  src/load_balancer.rs → srpc.load_balancer.cppm (module: srpc.load_balancer)
Done: 38 files transpiled, 0 errors
"""

    # What a no-op build prints: ninja has nothing to do, so the codegen edge
    # never fires and none of the generator's own output appears.
    NOT_REGENERATED = "ninja: no work to do.\n"

    def assert_script(self, stdin: str, *generated: str):
        # Invoked through `bash` on purpose: this repo has core.fileMode=false,
        # so a `chmod +x` in a worktree is silently NOT recorded and the script
        # lands in git as 100644. Relying on the exec bit made CI die with
        # PermissionError. The mode is now 100755 as well, but not depended on.
        return subprocess.run(
            ["bash", str(self.SCRIPT), *generated],
            input=stdin,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_passes_when_regeneration_reached_every_named_output(self) -> None:
        completed = self.assert_script(
            self.REGENERATED,
            "srpc.request_queue.cppm",
            "srpc.load_balancer.cppm",
            "srpc.utils.cppm",
            "srpc.frame_codec.cppm",
        )
        self.assertEqual(
            completed.returncode, 0, msg=completed.stdout + completed.stderr
        )

    def test_fails_when_regeneration_did_not_happen(self) -> None:
        """THE point of the gate. If this ever passes, stale generated C++
        ships silently and the whole step is decoration."""
        completed = self.assert_script(
            self.NOT_REGENERATED, "srpc.frame_codec.cppm"
        )
        self.assertEqual(completed.returncode, 1)
        self.assertIn("crate generation did not re-run", completed.stderr)

    def test_fails_when_the_generator_never_reached_the_output(self) -> None:
        """Regeneration ran but skipped the file we care about -- exactly the
        drift an mtime check cannot distinguish from success."""
        completed = self.assert_script(
            self.REGENERATED, "srpc.serializable.cppm"
        )
        self.assertEqual(completed.returncode, 1)
        self.assertIn(
            "generator did not report srpc.serializable.cppm", completed.stderr
        )

    def test_fails_when_generation_did_not_finish_cleanly(self) -> None:
        broken = self.REGENERATED.replace(
            "Done: 38 files transpiled, 0 errors",
            "Done: 38 files transpiled, 2 errors",
        )
        completed = self.assert_script(broken, "srpc.frame_codec.cppm")
        self.assertEqual(completed.returncode, 1)
        self.assertIn("did not finish cleanly", completed.stderr)

    def test_workflow_uses_the_script_and_no_output_mtime_assertions(self) -> None:
        workflow = (REPOSITORY / ".github/workflows/ci.yml").read_text(
            encoding="utf-8"
        )
        step = workflow.split(
            "Verify emitter and canonical Rust invalidate crate generation"
        )[1].split("Run focused production consumers")[0]
        self.assertIn("scripts/ci/assert_crate_codegen.sh", step)
        # The bug class: comparing generated-output timestamps. Nothing in the
        # step may go back to it. Scan code only -- the comments explain the
        # old `stat -c %Y` assertions and naming them is the point.
        code = "\n".join(
            line
            for line in step.splitlines()
            if not line.lstrip().startswith("#")
        )
        self.assertNotIn("stat -c %Y", code)
        # The steady check -- "a no-op build must NOT regenerate" -- is the one
        # assertion that was always correct, and must survive.
        self.assertIn(
            "crate generation reran without an emitter or source change", step
        )


if __name__ == "__main__":
    unittest.main()
