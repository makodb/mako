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
    def test_discarded_parallel_crate_stays_absent(self) -> None:
        self.assertFalse((REPOSITORY / "crates/srpc").exists())

    def test_retired_inline_carriers_stay_absent(self) -> None:
        retired = (
            "src/rrr/base/callback_wrapper.cpp",
            "src/rrr/rpc/internal_protocol.cpp",
            "src/rrr/misc/stat.cpp",
            "src/rrr/rpc/errors.cpp",
            "src/rrr/rpc/connection_metrics.cpp",
            "src/rrr/rpc/completion_tracker.cpp",
            "src/rrr/misc/rand.cpp",
            "src/rrr/rpc/request_options.cpp",
            "src/rrr/rpc/reconnect_policy.cpp",
            "src/rrr/rpc/circuit_breaker.cpp",
            "src/rrr/rpc/connection_state.cpp",
            "src/rrr/rpc/heartbeat.cpp",
            "src/rrr/base/basetypes.cpp",
            "src/rrr/rpc/request_queue.cpp",
            "src/rrr/rpc/load_balancer.cpp",
            "src/rrr/rpc/utils.cpp",
        )
        self.assertTrue(all(not (REPOSITORY / path).exists() for path in retired))

    def test_source_boundary_census_tracks_the_remaining_scaffolding(self) -> None:
        output = subprocess.check_output(
            [sys.executable, "scripts/rrr_handwritten_census.py"],
            cwd=REPOSITORY,
            text=True,
        )
        self.assertIn(
            "source boundary: 23 hand-authored module units, "
            "SCAFFOLD=1610 noncomment lines (760 DSL fences + 850 other)",
            output,
        )
        self.assertIn(
            "payload census:   dsl=9394  generated=12189 "
            "nonblank/non-// lines",
            output,
        )
        self.assertIn(
            "12 compatibility headers, SCAFFOLD=147 noncomment lines", output
        )
        self.assertIn(
            "terminal C:      3 ABI headers/87 lines; 7 kernels/410 lines",
            output,
        )

    def test_modules_have_only_the_expected_structured_preambles(self) -> None:
        with (REPOSITORY / "src/rrr/module-preambles.toml").open("rb") as stream:
            self.assertEqual(
                tomllib.load(stream),
                {
                    "version": 1,
                    "module": [
                        {
                            "name": "rrr.basetypes",
                            "includes": [
                                {
                                    "path": "misc/srpc_timing.h",
                                    "form": "quote",
                                },
                                {
                                    "path": "rusty/sync/atomic.hpp",
                                    "form": "angle",
                                },
                            ],
                        },
                        {
                            "name": "rrr.connection_metrics",
                            "includes": [
                                {
                                    "path": "rusty/sync/atomic.hpp",
                                    "form": "angle",
                                }
                            ],
                        },
                        {
                            "name": "rrr.completion_tracker",
                            "includes": [
                                {
                                    "path": "rusty/sync/atomic.hpp",
                                    "form": "angle",
                                }
                            ],
                        },
                        {
                            "name": "rrr.rand",
                            "includes": [
                                {
                                    "path": "misc/srpc_rand.h",
                                    "form": "quote",
                                }
                            ],
                        },
                        {
                            "name": "rrr.circuit_breaker",
                            "includes": [
                                {
                                    "path": "misc/srpc_timing.h",
                                    "form": "quote",
                                }
                            ],
                        },
                        {
                            "name": "rrr.utils",
                            "includes": [
                                {
                                    "path": "netdb.h",
                                    "form": "angle",
                                }
                            ],
                        },
                    ],
                },
            )

    def test_utils_sidecars_are_narrow_and_fail_closed_inputs(self) -> None:
        with (REPOSITORY / "src/rrr/rust-type-map.toml").open("rb") as stream:
            self.assertEqual(
                tomllib.load(stream),
                {
                    "LegacyAddrInfo": "addrinfo",
                    "LegacyStdString": "std::string",
                },
            )
        with (REPOSITORY / "src/rrr/cpp-module-index.toml").open("rb") as stream:
            self.assertEqual(
                tomllib.load(stream),
                {
                    "version": 1,
                    "modules": {
                        "rrr::logging": {
                            "namespace": "rrr",
                            "symbols": {
                                "log_line": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "void(int32_t,int32_t,const int8_t*,const std::string&)"
                                    ],
                                }
                            },
                        }
                    },
                },
            )

    def test_manifest_names_the_canonical_rust_sources(self) -> None:
        modules = DRIVER.load_manifest(
            REPOSITORY, REPOSITORY / "src/rrr/rust-modules.toml"
        )
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
                    "rrr.basetypes",
                    "basetypes",
                    "src/rrr/src/basetypes.rs",
                    "src/rrr/src/basetypes.rs",
                ),
                (
                    "rrr.callback_wrapper",
                    "callback_wrapper",
                    "src/rrr/src/callback_wrapper.rs",
                    "src/rrr/src/callback_wrapper.rs",
                ),
                (
                    "rrr.internal_protocol",
                    "internal_protocol",
                    "src/rrr/src/internal_protocol.rs",
                    "src/rrr/src/internal_protocol.rs",
                ),
                (
                    "rrr.stat",
                    "stat",
                    "src/rrr/src/stat.rs",
                    "src/rrr/src/stat.rs",
                ),
                (
                    "rrr.errors",
                    "errors",
                    "src/rrr/src/errors.rs",
                    "src/rrr/src/errors.rs",
                ),
                (
                    "rrr.connection_metrics",
                    "connection_metrics",
                    "src/rrr/src/connection_metrics.rs",
                    "src/rrr/src/connection_metrics.rs",
                ),
                (
                    "rrr.completion_tracker",
                    "completion_tracker",
                    "src/rrr/src/completion_tracker.rs",
                    "src/rrr/src/completion_tracker.rs",
                ),
                (
                    "rrr.rand",
                    "rand",
                    "src/rrr/src/rand.rs",
                    "src/rrr/src/rand.rs",
                ),
                (
                    "rrr.request_options",
                    "request_options",
                    "src/rrr/src/request_options.rs",
                    "src/rrr/src/request_options.rs",
                ),
                (
                    "rrr.reconnect_policy",
                    "reconnect_policy",
                    "src/rrr/src/reconnect_policy.rs",
                    "src/rrr/src/reconnect_policy.rs",
                ),
                (
                    "rrr.circuit_breaker",
                    "circuit_breaker",
                    "src/rrr/src/circuit_breaker.rs",
                    "src/rrr/src/circuit_breaker.rs",
                ),
                (
                    "rrr.connection_state",
                    "connection_state",
                    "src/rrr/src/connection_state.rs",
                    "src/rrr/src/connection_state.rs",
                ),
                (
                    "rrr.heartbeat",
                    "heartbeat",
                    "src/rrr/src/heartbeat.rs",
                    "src/rrr/src/heartbeat.rs",
                ),
                (
                    "rrr.request_queue",
                    "request_queue",
                    "src/rrr/src/request_queue.rs",
                    "src/rrr/src/request_queue.rs",
                ),
                (
                    "rrr.load_balancer",
                    "load_balancer",
                    "src/rrr/src/load_balancer.rs",
                    "src/rrr/src/load_balancer.rs",
                ),
                (
                    "rrr.utils",
                    "utils",
                    "src/rrr/src/utils.rs",
                    "src/rrr/src/utils.rs",
                ),
            ],
        )

    def test_cmake_provider_inventory_matches_the_canonical_manifest(self) -> None:
        modules = DRIVER.load_manifest(
            REPOSITORY, REPOSITORY / "src/rrr/rust-modules.toml"
        )
        cmake = (REPOSITORY / "src/rrr/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        match = re.search(
            r"set\(RRR_GOAL0_CANONICAL_MODULES\n(?P<body>.*?)\n\)",
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
        self.assertIn(
            "${CMAKE_CURRENT_SOURCE_DIR}/src/${_RRR_GOAL0_MODULE}.rs",
            cmake,
        )
        self.assertIn(
            "${RRR_GOAL0_CRATE_CPP_DIR}/rrr.${_RRR_GOAL0_MODULE}.cppm",
            cmake,
        )
        for fragment in (
            'set(RRR_GOAL0_TYPE_MAP\n    ${CMAKE_CURRENT_SOURCE_DIR}/rust-type-map.toml',
            'set(RRR_GOAL0_CPP_MODULE_INDEX\n    ${CMAKE_CURRENT_SOURCE_DIR}/cpp-module-index.toml',
            '--type-map "${RRR_GOAL0_TYPE_MAP}"',
            '--cpp-module-index "${RRR_GOAL0_CPP_MODULE_INDEX}"',
            '"${RRR_GOAL0_TYPE_MAP}"',
            '"${RRR_GOAL0_CPP_MODULE_INDEX}"',
        ):
            self.assertIn(fragment, cmake)

        workflow = (REPOSITORY / ".github/workflows/ci.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn('canonical_input="src/rrr/src/utils.rs"', workflow)
        self.assertIn(
            'utils_generated_path="${GOAL0_BUILD_DIR}/src/rrr/'
            'goal0-crate-cpp/rrr.utils.cppm"',
            workflow,
        )
        self.assertIn(
            'facade_utils_before="$(stat -c %Y "${utils_generated_path}")"',
            workflow,
        )
        self.assertIn(
            'test "$(stat -c %Y "${utils_generated_path}")" -gt '
            '"${facade_utils_before}"',
            workflow,
        )
        self.assertIn('type_map_input="src/rrr/rust-type-map.toml"', workflow)
        self.assertIn(
            'module_index_input="src/rrr/cpp-module-index.toml"', workflow
        )
        self.assertIn(
            'for sidecar_input in "${type_map_input}" "${module_index_input}"',
            workflow,
        )

    def test_checked_in_modules_are_canonical_rust_sources(self) -> None:
        modules = DRIVER.load_manifest(
            REPOSITORY, REPOSITORY / "src/rrr/rust-modules.toml"
        )
        canonical_lines = 0
        for module in modules:
            with self.subTest(cpp_module=module.cpp_module):
                source = module.output.read_text(encoding="utf-8")
                self.assertTrue(
                    source.startswith(
                        f"// Canonical Rust source for the {module.cpp_module} module.\n"
                        "// Compiled directly by rustc and translated by "
                        "rusty-cpp crate mode.\n"
                    )
                )
                self.assertNotIn("@generated", source)
                self.assertNotIn("provenance-input", source)
                canonical_lines += sum(
                    bool(line.strip()) and not line.lstrip().startswith("//")
                    for line in source.splitlines()
                )
        self.assertEqual(canonical_lines, 2333)

    def test_canonical_source_validation_never_normalizes_owned_bytes(self) -> None:
        payload = b"pub fn canonical() {}\n\n"
        self.assertIs(
            DRIVER.validate_canonical_source(payload, "src/rrr/src/example.rs"),
            payload,
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "LF line endings"):
            DRIVER.validate_canonical_source(
                b"pub fn canonical() {}\r\n", "src/rrr/src/example.rs"
            )

    def test_write_never_replaces_a_canonical_source_snapshot(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-canonical-write-") as temporary:
            root = Path(temporary)
            source = root / "src/rrr/src/example.rs"
            source.parent.mkdir(parents=True)
            original = b"pub fn canonical() -> i32 { 1 }\n"
            changed = b"pub fn canonical() -> i32 { 2 }\n"
            source.write_bytes(original)
            generated = [
                DRIVER.GeneratedFile(
                    output_label="src/rrr/src/example.rs",
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
        manifest = REPOSITORY / "src/rrr/rust-modules.toml"
        modules = DRIVER.load_manifest(REPOSITORY, manifest)
        expected_lib = DRIVER.render_lib(
            "src/rrr/rust-modules.toml", manifest, modules
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
                "src/rrr/src/completion_tracker.rs",
                "src/rrr/src/rand.rs",
                "src/rrr/src/reconnect_policy.rs",
                "src/rrr/src/request_options.rs",
                "src/rrr/src/circuit_breaker.rs",
                "src/rrr/src/connection_state.rs",
                "src/rrr/src/heartbeat.rs",
                "src/rrr/src/basetypes.rs",
                "src/rrr/src/request_queue.rs",
                "src/rrr/src/load_balancer.rs",
                "src/rrr/src/utils.rs",
            },
        )

    def test_unsafe_allowances_are_confined_to_the_audited_c_boundaries(self) -> None:
        with (REPOSITORY / "src/rrr/Cargo.toml").open("rb") as stream:
            cargo = tomllib.load(stream)
        self.assertEqual(cargo["lints"]["rust"]["unsafe_code"], "deny")

        unsafe_syntax = re.compile(
            r"#\s*\[\s*allow\s*\(\s*unsafe_code\s*\)\s*\]"
            r"|#\s*\[\s*unsafe\b"
            r"|\bunsafe\s+(?:(?:async|const)\s+)*fn\b"
            r"|\bunsafe\s+(?:extern|impl|trait)\b"
            r"|\bunsafe\s*\{"
        )
        rust_root = REPOSITORY / "src/rrr/src"
        rand_path = rust_root / "rand.rs"
        circuit_path = rust_root / "circuit_breaker.rs"
        basetypes_path = rust_root / "basetypes.rs"
        utils_path = rust_root / "utils.rs"
        for path in sorted(rust_root.rglob("*.rs")):
            if path in {rand_path, circuit_path, basetypes_path, utils_path}:
                continue
            self.assertIsNone(
                unsafe_syntax.search(path.read_text(encoding="utf-8")),
                f"unsafe Rust escaped the audited C boundaries: {path}",
            )

        rust = rand_path.read_text(encoding="utf-8")
        allowed_sections = (
            textwrap.dedent(
                """\
                #[allow(unsafe_code)]
                unsafe extern "C" {
                    fn srpc_rand_raw() -> i32;
                    fn srpc_rand_destroy();
                }
                """
            ).strip(),
            textwrap.dedent(
                """\
                #[allow(unsafe_code)]
                pub fn randgen_rand_raw() -> i32 {
                    unsafe { srpc_rand_raw() }
                }
                """
            ).strip(),
            textwrap.dedent(
                """\
                #[allow(unsafe_code)]
                pub fn randgen_destroy() {
                    unsafe { srpc_rand_destroy(); }
                }
                """
            ).strip(),
        )
        remainder = rust
        for section in allowed_sections:
            self.assertEqual(rust.count(section), 1)
            remainder = remainder.replace(section, "", 1)
        self.assertIsNone(
            unsafe_syntax.search(remainder),
            "rand.rs gained unsafe syntax outside its three exact C-boundary scopes",
        )

        circuit = circuit_path.read_text(encoding="utf-8")
        circuit_sections = (
            textwrap.dedent(
                """\
                #[allow(unsafe_code)]
                unsafe extern "C" {
                    fn srpc_clock_monotonic_us() -> u64;
                }
                """
            ).strip(),
            textwrap.dedent(
                """\
                #[allow(unsafe_code)]
                pub fn current_time_us() -> u64 {
                    unsafe { srpc_clock_monotonic_us() }
                }
                """
            ).strip(),
        )
        remainder = circuit
        for section in circuit_sections:
            self.assertEqual(circuit.count(section), 1)
            remainder = remainder.replace(section, "", 1)
        self.assertIsNone(
            unsafe_syntax.search(remainder),
            "circuit_breaker.rs gained unsafe syntax outside its exact clock boundary",
        )

        basetypes = basetypes_path.read_text(encoding="utf-8")
        self.assertEqual(basetypes.count("#[allow(unsafe_code)]"), 10)
        self.assertEqual(basetypes.count('unsafe extern "C"'), 1)
        self.assertEqual(basetypes.count("pub unsafe fn"), 4)
        self.assertEqual(basetypes.count("unsafe {"), 9)
        self.assertEqual(basetypes.count("/// # Safety"), 4)
        for symbol in (
            "srpc_clock_monotonic_us",
            "srpc_clock_realtime_coarse_us",
            "srpc_gettimeofday_us",
            "srpc_sleep_us",
        ):
            self.assertIn(symbol, basetypes)

        utils = utils_path.read_text(encoding="utf-8")
        self.assertEqual(utils.count("#[allow(unsafe_code)]"), 5)
        self.assertEqual(utils.count('unsafe extern "C"'), 1)
        self.assertEqual(utils.count("pub unsafe fn adopt"), 1)
        self.assertEqual(utils.count("unsafe {"), 5)
        self.assertEqual(utils.count("/// # Safety"), 1)
        self.assertIn("    info_: *mut LegacyAddrInfo,", utils)
        self.assertIn("    owned_: Cell<bool>,", utils)
        self.assertNotIn("pub info_:", utils)
        self.assertNotIn("pub owned_:", utils)
        for symbol in ("freeaddrinfo", "srpc_find_open_port"):
            self.assertIn(symbol, utils)

        facade_manifest = REPOSITORY / "src/rrr/rusty-rustc/Cargo.toml"
        with facade_manifest.open("rb") as stream:
            facade_cargo = tomllib.load(stream)
        self.assertEqual(facade_cargo["package"]["name"], "rusty")
        self.assertEqual(facade_cargo["lib"]["path"], "src/lib.rs")
        self.assertEqual(facade_cargo["lints"]["rust"]["unsafe_code"], "deny")
        self.assertFalse((facade_manifest.parent / "Cargo.lock").exists())
        facade = (facade_manifest.parent / "src/lib.rs").read_text(encoding="utf-8")
        logging_boundary = (
            "#[allow(unsafe_code)]\n"
            "        pub unsafe fn log_line("
            "_level: i32, _line: i32, _file: *const i8, _message: &String) {}"
        )
        self.assertEqual(facade.count(logging_boundary), 1)
        self.assertEqual(facade.count("/// # Safety"), 1)
        facade_remainder = facade.replace(logging_boundary, "", 1)
        self.assertIsNone(
            unsafe_syntax.search(facade_remainder),
            "rustc-only rusty facade gained unsafe Rust outside log_line",
        )
        self.assertIn("inner: Option<Box<F>>", facade)
        self.assertIn("runtime_layout_padding: [u8; 32]", facade)
        self.assertIn("impl<F: ?Sized> Deref for Function<F>", facade)
        self.assertIn("impl<F: ?Sized> DerefMut for Function<F>", facade)
        self.assertIn("impl<A: 'static> Function<dyn FnMut(A)>", facade)

        self.assertEqual(cargo["workspace"]["members"], ["rusty-rustc"])
        self.assertEqual(cargo["dependencies"]["rusty"], {"path": "rusty-rustc"})


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

    def test_manifest_reserves_generated_lib_from_module_ownership(self) -> None:
        self.write_manifest(
            """\
            schema_version = 1
            [[module]]
            cpp_module = "rrr.lib"
            output = "src/rrr/src/lib.rs"
            [[module.input]]
            source = "src/rrr/rpc/example.cpp"
            block_ids = ["example.1"]
            """
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "lib.rs is reserved"):
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
    def test_utils_preamble_is_rejected_from_sibling_children(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-preamble-") as temporary:
            output = Path(temporary)
            (output / "CMakeLists.txt").write_text("# generated\n", encoding="utf-8")
            (output / "rrr.utils.cppm").write_text(
                "// generated\nmodule;\n"
                "#include <netdb.h>\n"
                "#include <cstdint>\n"
                "export module rrr.utils;\n"
                "import rrr.logging;\n",
                encoding="utf-8",
            )
            (output / "rrr.example.cppm").write_text(
                "// generated\nmodule;\n"
                "#include <netdb.h>\n"
                "export module rrr.example;\n",
                encoding="utf-8",
            )
            (output / "rrr.cppm").write_text(
                "export module rrr;\n"
                "namespace rrr {\n"
                "export import rrr.utils;\n"
                "export import rrr.example;\n",
                encoding="utf-8",
            )
            modules = [
                mock.Mock(cpp_module="rrr.utils"),
                mock.Mock(cpp_module="rrr.example"),
            ]
            specs = {
                "rrr.utils": GATE.AbiSpec(frozenset(), frozenset()),
                "rrr.example": GATE.AbiSpec(frozenset(), frozenset()),
            }
            with mock.patch.dict(GATE.ABI_SPECS, specs, clear=True):
                with self.assertRaisesRegex(
                    GATE.GateError,
                    "utils netdb preamble leaked into rrr.example",
                ):
                    GATE.require_cpp_surfaces(Path("/repository"), output, modules)

    def test_placeholder_ratchet_checks_named_module_purview(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-placeholder-") as temporary:
            generated = Path(temporary) / "rrr.example.cppm"
            generated.write_text(
                "module;\n"
                "// Compiler runtime diagnostic: unsupported conversion.\n"
                "export module rrr.example;\n"
                "export int value();\n",
                encoding="utf-8",
            )
            self.assertIn(
                "export module rrr.example;",
                GATE.read_generated(generated, "test module"),
            )

            generated.write_text(
                "module;\n"
                "// Compiler runtime diagnostic: unsupported conversion.\n"
                "export module rrr.example;\n"
                "// TODO: lower this declaration.\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(GATE.GateError, "placeholder marker 'TODO'"):
                GATE.read_generated(generated, "test module")

    def test_generated_children_have_only_their_direct_module_imports(self) -> None:
        GATE.require_exact_module_imports(
            "export module rrr.rand;\nimport rusty;\n",
            "rrr.rand",
            ["rusty"],
        )
        GATE.require_exact_module_imports(
            "export module rrr.request_options;\nimport rrr.rand;\n",
            "rrr.request_options",
            ["rrr.rand"],
        )
        GATE.require_exact_module_imports(
            "export module rrr.reconnect_policy;\nimport rrr.rand;\n",
            "rrr.reconnect_policy",
            ["rrr.rand"],
        )
        GATE.require_exact_module_imports(
            "export module rrr.circuit_breaker;\n",
            "rrr.circuit_breaker",
            [],
        )
        GATE.require_exact_module_imports(
            "export module rrr.basetypes;\n",
            "rrr.basetypes",
            [],
        )
        GATE.require_exact_module_imports(
            "export module rrr.request_queue;\n"
            "import rusty;\n"
            "import rrr.circuit_breaker;\n",
            "rrr.request_queue",
            ["rusty", "rrr.circuit_breaker"],
        )
        GATE.require_exact_module_imports(
            "export module rrr.connection_state;\n",
            "rrr.connection_state",
            [],
        )
        GATE.require_exact_module_imports(
            "export module rrr.heartbeat;\nimport rrr.circuit_breaker;\n",
            "rrr.heartbeat",
            ["rrr.circuit_breaker"],
        )
        GATE.require_exact_module_imports(
            "export module rrr.load_balancer;\n",
            "rrr.load_balancer",
            [],
        )
        with self.assertRaisesRegex(GATE.GateError, "private imports must be exactly"):
            GATE.require_exact_module_imports(
                "export module rrr.rand;\n"
                "import rusty;\n"
                "import rrr.debugging;\n",
                "rrr.rand",
                ["rusty"],
            )
        with self.assertRaisesRegex(GATE.GateError, "exported=\\['rrr.rand'\\]"):
            GATE.require_exact_module_imports(
                "export module rrr.request_options;\n"
                "export import rrr.rand;\n",
                "rrr.request_options",
                ["rrr.rand"],
            )

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
            production = root / "production.a"
            production.touch()
            args = GATE.argparse.Namespace(
                transpiler="transpiler",
                clang="clang++",
                nm="nm",
                production_library=str(production),
                generated_dir=str(generated),
                runtime_library=[],
                runtime_module_root=[],
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
                production=production,
                runtime_libraries=[],
                cxx_flags=["-stdlib=libc++"],
                link_flags=["-lc++abi"],
                prebuilt_module_dirs=[],
            )

    def test_standalone_generation_consumes_the_structured_preamble(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-generate-test-") as temporary:
            root = Path(temporary) / ".." / Path(temporary).name
            args = GATE.argparse.Namespace(
                transpiler="transpiler",
                clang="clang++",
                nm="nm",
                production_library=None,
                generated_dir=None,
                runtime_library=[],
                runtime_module_root=[],
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
            self.assertEqual(
                command[-6:],
                [
                    "--module-preamble",
                    str(root / GATE.MODULE_PREAMBLE),
                    "--type-map",
                    str(root / GATE.TYPE_MAP),
                    "--cpp-module-index",
                    str(root / GATE.CPP_MODULE_INDEX),
                ],
            )
            self.assertEqual(
                command[2], str((root / "src/rrr/Cargo.toml").resolve())
            )
            self.assertTrue(Path(command[2]).is_absolute())

    def test_cmake_crate_invocation_uses_an_absolute_manifest_variable(self) -> None:
        cmake = (REPOSITORY / "src/rrr/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            'get_filename_component(\n'
            '    RRR_GOAL0_CRATE_MANIFEST\n'
            '    "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"\n'
            '    ABSOLUTE\n'
            ')',
            cmake,
        )
        self.assertIn('--crate "${RRR_GOAL0_CRATE_MANIFEST}"', cmake)
        self.assertNotIn(
            '--crate "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"', cmake
        )

    def test_generated_gate_compiles_children_before_partial_root(self) -> None:
        modules = [
            mock.Mock(cpp_module="rrr.basetypes"),
            mock.Mock(cpp_module="rrr.callback_wrapper"),
            mock.Mock(cpp_module="rrr.internal_protocol"),
            mock.Mock(cpp_module="rrr.stat"),
            mock.Mock(cpp_module="rrr.errors"),
            mock.Mock(cpp_module="rrr.connection_metrics"),
            mock.Mock(cpp_module="rrr.completion_tracker"),
            mock.Mock(cpp_module="rrr.rand"),
            mock.Mock(cpp_module="rrr.request_options"),
            mock.Mock(cpp_module="rrr.reconnect_policy"),
            mock.Mock(cpp_module="rrr.circuit_breaker"),
            mock.Mock(cpp_module="rrr.connection_state"),
            mock.Mock(cpp_module="rrr.heartbeat"),
            mock.Mock(cpp_module="rrr.request_queue"),
            mock.Mock(cpp_module="rrr.load_balancer"),
            mock.Mock(cpp_module="rrr.utils"),
        ]

        def symbols_for_module(
            _nm: Path, _root: Path, _path: Path, module_name: str
        ) -> frozenset[tuple[str, str]]:
            return GATE.ABI_SPECS[module_name].symbols

        completion_raw = list(GATE.ABI_SPECS["rrr.completion_tracker"].symbols)
        completion_raw.extend(
            [
                (
                    "T",
                    "rrr::CompletionTracker@rrr.completion_tracker::"
                    "CompletionTracker()",
                ),
                (
                    "T",
                    "rrr::CompletionTracker@rrr.completion_tracker::"
                    "CompletionTracker(rrr::CompletionTrackerConfig@"
                    "rrr.completion_tracker)",
                ),
                ("T", "initializer for module rrr.completion_tracker"),
            ]
        )
        rand_raw = list(GATE.ABI_SPECS["rrr.rand"].symbols)
        rand_raw.append(("T", "initializer for module rrr.rand"))
        request_options_raw = list(
            GATE.ABI_SPECS["rrr.request_options"].symbols
        )
        request_options_raw.append(
            ("T", "initializer for module rrr.request_options")
        )
        reconnect_policy_raw = list(
            GATE.ABI_SPECS["rrr.reconnect_policy"].symbols
        )
        reconnect_policy_raw.append(
            ("T", "initializer for module rrr.reconnect_policy")
        )
        circuit_breaker_raw = list(
            GATE.ABI_SPECS["rrr.circuit_breaker"].symbols
        )
        circuit_breaker_raw.append(
            ("T", "initializer for module rrr.circuit_breaker")
        )
        basetypes_raw = list(GATE.ABI_SPECS["rrr.basetypes"].symbols)
        basetypes_raw.append(("T", "initializer for module rrr.basetypes"))
        request_queue_raw = list(GATE.ABI_SPECS["rrr.request_queue"].symbols)
        request_queue_raw.extend(
            [
                (
                    "T",
                    "rrr::RequestQueue@rrr.request_queue::RequestQueue()",
                ),
                (
                    "T",
                    "rrr::RequestQueue@rrr.request_queue::RequestQueue("
                    "rrr::RequestQueueConfig@rrr.request_queue)",
                ),
                ("T", "initializer for module rrr.request_queue"),
            ]
        )
        exact_raw = {
            name: [
                *GATE.ABI_SPECS[name].symbols,
                ("T", f"initializer for module {name}"),
            ]
            for name in (
                "rrr.connection_state",
                "rrr.heartbeat",
                "rrr.load_balancer",
            )
        }
        utils_raw = list(GATE.ABI_SPECS["rrr.utils"].symbols)
        for symbol in (
            "rrr::AddrInfo@rrr.utils::AddrInfo()",
            "rrr::AddrInfo@rrr.utils::AddrInfo(addrinfo*)",
            "rrr::AddrInfo@rrr.utils::AddrInfo(addrinfo*, rusty::Cell<bool>)",
            "rrr::AddrInfo@rrr.utils::AddrInfo(rrr::AddrInfo@rrr.utils&&)",
            "rrr::AddrInfo@rrr.utils::~AddrInfo()",
        ):
            utils_raw.append(("T", symbol))
        utils_raw.append(("T", "initializer for module rrr.utils"))

        def compiled_object(
            _clang: Path,
            _root: Path,
            _include: Path,
            _source: Path,
            _work: Path,
            module_name: str,
            _cxx_flags: list[str],
            _prebuilt_module_dirs: list[Path],
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
            ), mock.patch.object(
                GATE, "completion_raw_symbols", return_value=completion_raw
            ), mock.patch.object(
                GATE, "rand_raw_symbols", return_value=rand_raw
            ), mock.patch.object(
                GATE,
                "request_options_raw_symbols",
                return_value=request_options_raw,
            ), mock.patch.object(
                GATE,
                "reconnect_policy_raw_symbols",
                return_value=reconnect_policy_raw,
            ), mock.patch.object(
                GATE,
                "circuit_breaker_raw_symbols",
                return_value=circuit_breaker_raw,
            ), mock.patch.object(
                GATE,
                "basetypes_raw_symbols",
                return_value=basetypes_raw,
            ), mock.patch.object(
                GATE,
                "request_queue_raw_symbols",
                return_value=request_queue_raw,
            ), mock.patch.object(
                GATE,
                "utils_raw_symbols",
                return_value=utils_raw,
            ), mock.patch.object(
                GATE,
                "exact_module_raw_symbols",
                side_effect=lambda _nm, _root, _binary, name: exact_raw[name],
            ):
                GATE.check_generated_output(
                    root=Path("/repository"),
                    output=output,
                    modules=modules,
                    clang=Path("/clang++"),
                    nm=Path("/nm"),
                    production=Path("/production.a"),
                    runtime_libraries=[Path("/rusty.a")],
                    cxx_flags=["-stdlib=libc++"],
                    link_flags=["-lc++abi"],
                    prebuilt_module_dirs=[Path("/runtime-modules")],
                )

        compiled_names = [call.args[5] for call in compile_module.call_args_list]
        self.assertEqual(
            compiled_names,
            [
                "rrr.logging",
                "rrr.basetypes",
                "rrr.callback_wrapper",
                "rrr.internal_protocol",
                "rrr.stat",
                "rrr.errors",
                "rrr.connection_metrics",
                "rrr.completion_tracker",
                "rrr.rand",
                "rrr.request_options",
                "rrr.reconnect_policy",
                "rrr.circuit_breaker",
                "rrr.connection_state",
                "rrr.heartbeat",
                "rrr.request_queue",
                "rrr.load_balancer",
                "rrr.utils",
                "rrr",
            ],
        )
        self.assertTrue(
            all(
                call.args[7] == [Path("/runtime-modules")]
                for call in compile_module.call_args_list
            )
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
        self.assertIn(
            "-fprebuilt-module-path=/runtime-modules",
            importer_compile_commands[0],
        )
        link_commands = [
            call.args[0]
            for call in run.call_args_list
            if "-o" in call.args[0]
            and any("importer-" in argument for argument in call.args[0])
        ]
        self.assertEqual(len(link_commands), 2)
        for command in link_commands:
            self.assertIn("-stdlib=libc++", command)
            self.assertIn("/rusty.a", command)
            self.assertIn("-lc++abi", command)
            if GATE.sys.platform.startswith("linux"):
                self.assertIn("-Wl,--start-group", command)
                self.assertIn("-Wl,--end-group", command)

        production_link = next(
            command
            for command in link_commands
            if any(argument == "/production.a" for argument in command)
        )
        logging_interface = next(
            argument
            for argument in production_link
            if argument.endswith("/rrr.logging.o")
        )
        logging_implementation = next(
            argument
            for argument in production_link
            if argument.endswith("/rrr.logging.probe.o")
        )
        self.assertLess(
            production_link.index(logging_interface),
            production_link.index("/production.a"),
        )
        self.assertLess(
            production_link.index(logging_implementation),
            production_link.index("/production.a"),
        )

    def test_completion_raw_symbol_ratchet_pins_all_33_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.completion_tracker"].symbols)
        entries.extend(
            [
                (
                    "T",
                    "rrr::CompletionTracker@rrr.completion_tracker::"
                    "CompletionTracker()",
                ),
                (
                    "T",
                    "rrr::CompletionTracker@rrr.completion_tracker::"
                    "CompletionTracker(rrr::CompletionTrackerConfig@"
                    "rrr.completion_tracker)",
                ),
                ("T", "initializer for module rrr.completion_tracker"),
            ]
        )
        self.assertEqual(len(entries), 33)
        GATE.require_completion_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 33 raw"):
            GATE.require_completion_raw_symbols("test provider", entries[:-1])

    def test_rand_raw_symbol_ratchet_pins_all_13_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.rand"].symbols)
        entries.append(("T", "initializer for module rrr.rand"))
        self.assertEqual(len(entries), 13)
        GATE.require_rand_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 13 raw"):
            GATE.require_rand_raw_symbols("test provider", entries[:-1])

    def test_request_options_raw_symbol_ratchet_pins_all_13_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.request_options"].symbols)
        entries.append(("T", "initializer for module rrr.request_options"))
        self.assertEqual(len(entries), 13)
        GATE.require_request_options_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 13 raw"):
            GATE.require_request_options_raw_symbols(
                "test provider", entries[:-1]
            )

    def test_reconnect_policy_raw_symbol_ratchet_pins_all_12_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.reconnect_policy"].symbols)
        entries.append(("T", "initializer for module rrr.reconnect_policy"))
        self.assertEqual(len(entries), 12)
        GATE.require_reconnect_policy_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 12 raw"):
            GATE.require_reconnect_policy_raw_symbols(
                "test provider", entries[:-1]
            )

    def test_circuit_breaker_raw_symbol_ratchet_pins_all_21_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.circuit_breaker"].symbols)
        entries.append(("T", "initializer for module rrr.circuit_breaker"))
        self.assertEqual(len(entries), 21)
        GATE.require_circuit_breaker_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 21 raw"):
            GATE.require_circuit_breaker_raw_symbols(
                "test provider", entries[:-1]
            )

    def test_exact_raw_symbol_ratchets_include_initializer(self) -> None:
        for module_name, expected_count in (
            ("rrr.connection_state", 14),
            ("rrr.heartbeat", 20),
            ("rrr.load_balancer", 7),
        ):
            with self.subTest(module_name=module_name):
                entries = list(GATE.ABI_SPECS[module_name].symbols)
                entries.append(("T", f"initializer for module {module_name}"))
                self.assertEqual(len(entries), expected_count)
                GATE.require_exact_module_raw_symbols(
                    module_name, "test provider", entries
                )
                with self.assertRaisesRegex(GATE.GateError, "raw strong entries"):
                    GATE.require_exact_module_raw_symbols(
                        module_name, "test provider", entries[:-1]
                    )

    def test_utils_raw_symbol_ratchet_pins_all_17_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.utils"].symbols)
        for symbol in (
            "rrr::AddrInfo@rrr.utils::AddrInfo()",
            "rrr::AddrInfo@rrr.utils::AddrInfo(addrinfo*)",
            "rrr::AddrInfo@rrr.utils::AddrInfo(addrinfo*, rusty::Cell<bool>)",
            "rrr::AddrInfo@rrr.utils::AddrInfo(rrr::AddrInfo@rrr.utils&&)",
            "rrr::AddrInfo@rrr.utils::~AddrInfo()",
        ):
            entries.append(("T", symbol))
        entries.append(("T", "initializer for module rrr.utils"))
        self.assertEqual(len(entries), 17)
        GATE.require_utils_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 17 raw"):
            GATE.require_utils_raw_symbols("test provider", entries[:-1])

    def test_basetypes_raw_symbol_ratchet_pins_all_29_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.basetypes"].symbols)
        entries.append(("T", "initializer for module rrr.basetypes"))
        self.assertEqual(len(entries), 29)
        GATE.require_basetypes_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 29 raw"):
            GATE.require_basetypes_raw_symbols("test provider", entries[:-1])

    def test_request_queue_raw_symbol_ratchet_pins_all_30_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.request_queue"].symbols)
        entries.extend(
            [
                (
                    "T",
                    "rrr::RequestQueue@rrr.request_queue::RequestQueue()",
                ),
                (
                    "T",
                    "rrr::RequestQueue@rrr.request_queue::RequestQueue("
                    "rrr::RequestQueueConfig@rrr.request_queue)",
                ),
                ("T", "initializer for module rrr.request_queue"),
            ]
        )
        self.assertEqual(len(entries), 30)
        GATE.require_request_queue_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 30 raw"):
            GATE.require_request_queue_raw_symbols(
                "test provider", entries[:-1]
            )

    def test_basetypes_cpp_oracle_pins_abort_and_atomic_concurrency(self) -> None:
        source = GATE.importer_source()
        self.assertIn("std::abort();", GATE.ABI_SPECS["rrr.basetypes"].surface)
        self.assertIn("auto concurrent_counter = rrr::Counter::new_(0);", source)
        self.assertIn("for (std::size_t worker = 0; worker < 8; ++worker)", source)
        self.assertIn("concurrent_counter.peek_next() != 80000", source)
        self.assertIn(
            "sparse_wire_digest != UINT64_C(0x6d2ddf1efe2ab0b6)", source
        )

    def test_runtime_module_root_must_exist_and_contain_rusty_pcm(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-runtime-pcm-test-") as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(GATE.GateError, "unavailable"):
                GATE.resolve_prebuilt_module_dirs(root, ["missing"])

            empty = root / "empty"
            empty.mkdir()
            with self.assertRaisesRegex(GATE.GateError, "rusty.pcm"):
                GATE.resolve_prebuilt_module_dirs(root, [str(empty)])

    def test_runtime_module_dirs_are_nested_deduplicated_and_sorted(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-runtime-pcm-test-") as temporary:
            root = Path(temporary)
            first = root / "modules" / "zeta"
            second = root / "modules" / "alpha"
            first.mkdir(parents=True)
            second.mkdir(parents=True)
            (first / "rusty.pcm").touch()
            (first / "rusty-duplicate-dependency.pcm").touch()
            (second / "vec_port.pcm").touch()

            self.assertEqual(
                GATE.resolve_prebuilt_module_dirs(root, ["modules", "modules"]),
                sorted([first.resolve(), second.resolve()]),
            )

    def test_compile_module_passes_every_runtime_bmi_directory(self) -> None:
        with mock.patch.object(GATE, "run") as run:
            result = GATE.compile_module(
                Path("/clang++"),
                Path("/repository"),
                Path("/include"),
                Path("/source"),
                Path("/work"),
                "rrr.completion_tracker",
                ["-stdlib=libc++"],
                [Path("/runtime-z"), Path("/runtime-a")],
            )

        self.assertEqual(result, Path("/work/rrr.completion_tracker.o"))
        self.assertEqual(len(run.call_args_list), 2)
        for call in run.call_args_list:
            command = call.args[0]
            self.assertIn("-std=gnu++23", command)
            self.assertIn("/repository/src/rrr", command)
            self.assertIn("-fprebuilt-module-path=/work", command)
            self.assertIn("-fprebuilt-module-path=/runtime-z", command)
            self.assertIn("-fprebuilt-module-path=/runtime-a", command)

    def test_gate_abi_ratchet_covers_every_manifest_module(self) -> None:
        root = Path("/repository")
        modules = [
            mock.Mock(cpp_module="rrr.basetypes"),
            mock.Mock(cpp_module="rrr.callback_wrapper"),
            mock.Mock(cpp_module="rrr.internal_protocol"),
            mock.Mock(cpp_module="rrr.stat"),
            mock.Mock(cpp_module="rrr.errors"),
            mock.Mock(cpp_module="rrr.connection_metrics"),
            mock.Mock(cpp_module="rrr.completion_tracker"),
            mock.Mock(cpp_module="rrr.rand"),
            mock.Mock(cpp_module="rrr.request_options"),
            mock.Mock(cpp_module="rrr.reconnect_policy"),
            mock.Mock(cpp_module="rrr.circuit_breaker"),
            mock.Mock(cpp_module="rrr.connection_state"),
            mock.Mock(cpp_module="rrr.heartbeat"),
            mock.Mock(cpp_module="rrr.request_queue"),
            mock.Mock(cpp_module="rrr.load_balancer"),
            mock.Mock(cpp_module="rrr.utils"),
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
