#!/usr/bin/env python3
"""Fail-closed negative controls for Goal-0 ownership and C++ ABI gates."""

from __future__ import annotations

from collections import Counter
import contextlib
import hashlib
import io
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import tomllib
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]


def load_script(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


DRIVER = load_script("extract_srpc_rust", "scripts/extract_srpc_rust.py")
GATE = load_script("check_srpc_crate_mode", "scripts/check_srpc_crate_mode.py")


class GateStaticContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.modules = DRIVER.load_manifest(ROOT, ROOT / "rust-modules.toml")

    def test_placeholder_ratchet_allows_only_the_benign_cycle_diagnostic(
        self,
    ) -> None:
        """The UNSUPPORTED allowlist must stay one exact compiler comment.

        rusty-cpp emits an informational by-value-cycle diagnostic that does
        not drop code (srpc.tcp_channel carries it for TcpListener and still
        compiles to its full ratcheted ABI), so that one anchored form is
        allowed. Everything else -- including hand-attention slots and any
        other wording of the same diagnostic -- must still fail.
        """
        head = "module;\n#include <cstdint>\nexport module srpc.probe;\n\n"
        allowed = (
            "// UNSUPPORTED: unsupported by-value circular type dependency "
            "in scope <crate>: [TcpListener]; cycle path: "
            "TcpListener -> TcpListener",
            "// UNSUPPORTED: unsupported by-value circular type dependency "
            "in scope <crate>: [A, B]",
        )
        rejected = (
            "// TODO(interface_traits): implement",
            "// TODO: finish this",
            "// UNSUPPORTED: trait objects are not lowered yet",
            "// skipped: emitter bailed out",
            "// UNSUPPORTED: by-value cycle dropped [TcpListener]",
            "int x; // UNSUPPORTED: unsupported by-value circular type "
            "dependency in scope <crate>: [X]",
        )
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory) / "probe.cppm"
            for body in allowed:
                with self.subTest(allowed=body):
                    probe.write_text(head + body + "\n", encoding="utf-8")
                    GATE.read_generated(probe, "probe")
            for body in rejected:
                with self.subTest(rejected=body):
                    probe.write_text(head + body + "\n", encoding="utf-8")
                    with self.assertRaises(GATE.GateError):
                        GATE.read_generated(probe, "probe")

    def test_every_manifest_module_has_all_exhaustive_ratchets(self) -> None:
        manifest = {module.cpp_module for module in self.modules}
        self.assertEqual(set(GATE.ABI_SPECS), manifest)
        self.assertEqual(set(GATE.EXPECTED_IMPORTS), manifest)
        self.assertEqual(set(GATE.EXPECTED_GENERATED_MODULE_SHA256), manifest)
        self.assertEqual(set(GATE.IMPORTER_USE_MARKERS), manifest)
        self.assertEqual(
            sum(len(spec.symbols) for spec in GATE.ABI_SPECS.values()), 1961
        )
        self.assertEqual(GATE.EXPECTED_TOTAL_PROVIDER_SYMBOLS, 1961)
        GATE.require_importer_coverage(self.modules)

    def test_platform_implementation_symbols_are_exhaustive(self) -> None:
        """Only declared modules may gain symbols outside the crate.

        A module implementation unit that CMake compiles but the crate does
        not (srpc.epoll_wrapper's platform unit) is the sole reason production
        may hold symbols the generated object lacks. Keep that allowlist
        pinned so a new out-of-crate definition cannot slip in unreviewed.
        """
        manifest = {module.cpp_module for module in self.modules}
        self.assertLessEqual(set(GATE.PLATFORM_IMPL_SYMBOLS), manifest)
        self.assertEqual(set(GATE.PLATFORM_IMPL_SYMBOLS), {"srpc.epoll_wrapper"})
        self.assertEqual(
            sum(len(s) for s in GATE.PLATFORM_IMPL_SYMBOLS.values()),
            GATE.EXPECTED_TOTAL_PLATFORM_SYMBOLS,
        )
        # The platform unit must not redefine anything the crate already owns.
        for name, symbols in GATE.PLATFORM_IMPL_SYMBOLS.items():
            self.assertFalse(symbols & set(GATE.ABI_SPECS[name].symbols))

    def test_each_promoted_module_has_surface_and_raw_abi_ratchets(self) -> None:
        expected = {
            "srpc.channel": (13, 20),
            # Factory-only construction: Epoll's public ctor became the static
            # `Epoll::new_()` factory. A ctor emits two raw ABI entries (C1/C2)
            # that demangle to one name, a factory emits one, so the raw count
            # drops by one while the unique count is unchanged: 26 -> 25.
            "srpc.epoll_wrapper": (22, 25),
            "srpc.pollable_proxy": (4, 7),
            "srpc.callbacks": (27, 28),
            "srpc.inmemory_channel": (77, 84),
            # Factory-only construction: FiberChannel's explicit ctor became
            # the static `FiberChannel::new_()` factory; one ctor, so one fewer
            # raw entry (20 -> 19), unique count unchanged.
            "srpc.fiber_channel": (17, 19),
            "srpc.threading": (17, 18),
            "srpc.debugging": (9, 10),
            "srpc.any_message": (10, 11),
        }
        for module, (unique_count, raw_count) in expected.items():
            with self.subTest(module=module):
                spec = GATE.ABI_SPECS[module]
                self.assertGreaterEqual(len(spec.surface), 4)
                self.assertEqual(len(spec.symbols), unique_count)
                raw = Counter(spec.symbols)
                raw.update(GATE.RAW_ABI_ALIASES.get(module, ()))
                raw[("T", f"initializer for module {module}")] += 1
                self.assertEqual(sum(raw.values()), raw_count)
                GATE.require_all_module_raw_symbols(
                    module, "fixture", list(raw.elements())
                )


class GateContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory(prefix="srpc-gate-fixture-")
        cls.generated = Path(cls.temporary.name) / "generated"
        transpiler = ROOT / GATE.DEFAULT_TRANSPILER
        if not transpiler.is_file():
            raise unittest.SkipTest(f"transpiler fixture unavailable: {transpiler}")
        flat_import_namespace = DRIVER.load_flat_import_namespace(
            ROOT, ROOT / "rust-modules.toml"
        )
        flat_import_arguments = (
            ["--flat-import-namespace", flat_import_namespace]
            if flat_import_namespace is not None
            else []
        )
        subprocess.run(
            [
                str(transpiler),
                "--crate",
                str(ROOT / "Cargo.toml"),
                "--output-dir",
                str(cls.generated),
                "--cxx-namespace",
                "srpc",
                *flat_import_arguments,
                "--module-preamble",
                str(ROOT / "module-preambles.toml"),
                "--type-map",
                str(ROOT / "rust-type-map.toml"),
                "--cpp-module-index",
                str(ROOT / "cpp-module-index.toml"),
            ],
            cwd=ROOT,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        cls.modules = GATE.load_owned_modules(ROOT)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def copied_output(self) -> tuple[tempfile.TemporaryDirectory[str], Path]:
        temporary = tempfile.TemporaryDirectory(prefix="srpc-gate-negative-")
        output = Path(temporary.name) / "generated"
        shutil.copytree(self.generated, output)
        return temporary, output

    def test_every_manifest_module_has_all_exhaustive_ratchets(self) -> None:
        manifest = {module.cpp_module for module in self.modules}
        self.assertEqual(set(GATE.ABI_SPECS), manifest)
        self.assertEqual(set(GATE.EXPECTED_IMPORTS), manifest)
        self.assertEqual(set(GATE.EXPECTED_GENERATED_MODULE_SHA256), manifest)
        self.assertEqual(set(GATE.IMPORTER_USE_MARKERS), manifest)
        self.assertEqual(sum(len(spec.symbols) for spec in GATE.ABI_SPECS.values()), 1961)
        GATE.require_importer_coverage(self.modules)
        GATE.require_cpp_surfaces(ROOT, self.generated, self.modules)

    def test_manifest_module_without_abi_spec_is_rejected(self) -> None:
        reduced = dict(GATE.ABI_SPECS)
        reduced.pop("srpc.misc")
        with mock.patch.object(GATE, "ABI_SPECS", reduced):
            with self.assertRaisesRegex(GATE.GateError, "missing ABI specification"):
                GATE.load_owned_modules(ROOT)

    def test_combined_importer_must_import_and_use_every_owner(self) -> None:
        source = GATE.importer_source()
        without_import = source.replace("import srpc.misc;\n", "", 1)
        with mock.patch.object(GATE, "importer_source", return_value=without_import):
            with self.assertRaisesRegex(GATE.GateError, "directly import"):
                GATE.require_importer_coverage(self.modules)
        without_use = source.replace("srpc::OneTimeJob", "srpc::RemovedJob")
        with mock.patch.object(GATE, "importer_source", return_value=without_use):
            with self.assertRaisesRegex(GATE.GateError, "lacks concrete"):
                GATE.require_importer_coverage(self.modules)

    def test_generated_digest_drift_is_advisory_not_fatal(self) -> None:
        """Byte-identity was repealed as an acceptance criterion.

        This test used to require that a digest change fail the gate. The
        project's rule is now "builds + tests pass + equivalent public
        function surface"; byte drift explicitly does not count, so the digest
        map is retained for reporting only. Assert the new contract directly:
        drift is computed and reported, and it does not raise.
        """
        temporary, output = self.copied_output()
        try:
            child = output / "srpc.future.cppm"
            child.write_text(
                child.read_text(encoding="utf-8").replace(
                    "namespace srpc {", "namespace srpc {\nexport struct Surprise;", 1
                ),
                encoding="utf-8",
            )
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                GATE.require_cpp_surfaces(ROOT, output, self.modules)
            report = stdout.getvalue()
            self.assertIn("ADVISORY", report)
            self.assertIn("srpc.future", report)
        finally:
            temporary.cleanup()

    def test_root_reexports_are_all_and_only_ordered_manifest_children(self) -> None:
        temporary, output = self.copied_output()
        try:
            root = output / "srpc.cppm"
            root.write_text(
                root.read_text(encoding="utf-8").replace(
                    "export module srpc;",
                    "export module srpc;\nexport import rusty;",
                    1,
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(GATE.GateError, "root imports must be exactly"):
                GATE.require_cpp_surfaces(ROOT, output, self.modules)
        finally:
            temporary.cleanup()

    def test_direct_imports_are_exact_for_previously_unchecked_child(self) -> None:
        temporary, output = self.copied_output()
        try:
            child = output / "srpc.completion_tracker.cppm"
            # The needle must be the module's CURRENT import line (the
            # port-narrowed graph imports std_port, not the rusty umbrella)
            # or the corruption is a no-op and this negative control goes
            # vacuous.
            child.write_text(
                child.read_text(encoding="utf-8").replace(
                    "import std_port;",
                    "import std_port;\nexport import srpc.logging;",
                    1,
                ),
                encoding="utf-8",
            )
            digest = hashlib.sha256(child.read_bytes()).hexdigest()
            with mock.patch.dict(
                GATE.EXPECTED_GENERATED_MODULE_SHA256,
                {"srpc.completion_tracker": digest},
            ):
                with self.assertRaisesRegex(GATE.GateError, "private imports"):
                    GATE.require_cpp_surfaces(ROOT, output, self.modules)
        finally:
            temporary.cleanup()

    def test_preamble_leakage_checks_do_not_bypass_enumerated_siblings(self) -> None:
        cases = (
            ("srpc.connection_state", "#include <netdb.h>"),
            ("srpc.heartbeat", "#include <rusty/io.hpp>"),
            ("srpc.future", '#include "base/rustc_markers.hpp"'),
        )
        for module, include in cases:
            with self.subTest(module=module, include=include):
                temporary, output = self.copied_output()
                try:
                    child = output / f"{module}.cppm"
                    child.write_text(
                        child.read_text(encoding="utf-8").replace(
                            "#include <cstdint>", f"{include}\n#include <cstdint>", 1
                        ),
                        encoding="utf-8",
                    )
                    digest = hashlib.sha256(child.read_bytes()).hexdigest()
                    with mock.patch.dict(
                        GATE.EXPECTED_GENERATED_MODULE_SHA256, {module: digest}
                    ):
                        with self.assertRaisesRegex(GATE.GateError, "leaked"):
                            GATE.require_cpp_surfaces(ROOT, output, self.modules)
                finally:
                    temporary.cleanup()

    def test_all_promoted_modules_pin_unique_and_raw_counts(self) -> None:
        expected = {
            "srpc.serializable_envelope": (0, 1),
            "srpc.future": (0, 1),
            "srpc.logging": (7, 8),
            # Factory-only construction: IdempotencyCache's two public ctors
            # became `new_()` / `with_config()`; two ctors, so two fewer raw
            # entries (39 -> 37), unique count unchanged.
            "srpc.idempotency": (36, 37),
            "srpc.fiber": (8, 9),
            "srpc.misc": (18, 23),
            "srpc.channel": (13, 20),
            # Factory-only construction: Epoll's public ctor became the static
            # `Epoll::new_()` factory. A ctor emits two raw ABI entries (C1/C2)
            # that demangle to one name, a factory emits one, so the raw count
            # drops by one while the unique count is unchanged: 26 -> 25.
            "srpc.epoll_wrapper": (22, 25),
            "srpc.pollable_proxy": (4, 7),
            "srpc.callbacks": (27, 28),
            "srpc.inmemory_channel": (77, 84),
            # Factory-only construction: FiberChannel's explicit ctor became
            # the static `FiberChannel::new_()` factory; one ctor, so one fewer
            # raw entry (20 -> 19), unique count unchanged.
            "srpc.fiber_channel": (17, 19),
            "srpc.threading": (17, 18),
            "srpc.debugging": (9, 10),
            "srpc.any_message": (10, 11),
        }
        for module, (unique_count, raw_count) in expected.items():
            with self.subTest(module=module):
                spec = GATE.ABI_SPECS[module]
                self.assertEqual(len(spec.symbols), unique_count)
                raw = Counter(spec.symbols)
                raw.update(GATE.RAW_ABI_ALIASES.get(module, ()))
                raw[("T", f"initializer for module {module}")] += 1
                self.assertEqual(sum(raw.values()), raw_count)
                GATE.require_all_module_raw_symbols(module, "fixture", list(raw.elements()))
                with self.assertRaisesRegex(GATE.GateError, "raw ABI"):
                    GATE.require_all_module_raw_symbols(
                        module, "fixture", list(raw.elements())[1:]
                    )

    def test_placeholder_is_rejected_only_in_named_module_purview(self) -> None:
        with tempfile.TemporaryDirectory(prefix="generated-placeholder-") as raw:
            path = Path(raw) / "srpc.fixture.cppm"
            path.write_text(
                "module;\n// TODO: compiler support preamble\n"
                "export module srpc.fixture;\nexport struct Ready {};\n",
                encoding="utf-8",
            )
            self.assertIn("Ready", GATE.read_generated(path, "fixture"))
            path.write_text(
                "module;\nexport module srpc.fixture;\n"
                "// TODO: missing generated declaration\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(GATE.GateError, "placeholder marker"):
                GATE.read_generated(path, "fixture")

    def test_symbol_ownership_uses_entity_not_parameter_attachment(self) -> None:
        owner = "srpc::run@srpc.future(srpc::Arg@srpc.logging)"
        parameter_only = "srpc::run(srpc::Arg@srpc.logging)"
        self.assertEqual(GATE.symbol_owner_module(owner), "srpc.future")
        self.assertIsNone(GATE.symbol_owner_module(parameter_only))

        nm_output = "\n".join(
            (
                f"0000000000000000 T {owner}",
                f"0000000000000010 T {parameter_only}",
                "0000000000000020 W srpc::weak@srpc.future()",
            )
        )
        with mock.patch.object(GATE, "run", return_value=nm_output):
            self.assertEqual(
                GATE.module_symbols(Path("nm"), ROOT, Path("object.o"), "srpc.future"),
                {("T", owner)},
            )

    def test_build_info_is_exact_one_line_clean_pin(self) -> None:
        exact = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout=(
                '{"git_hash":"'
                + GATE.REQUIRED_RUSTY_CPP_COMMIT
                + '","git_dirty":false}\n'
            ),
            stderr="",
        )
        with mock.patch.object(GATE.subprocess, "run", return_value=exact):
            GATE.verify_transpiler_build_info(ROOT, Path("transpiler"))

        invalid = (
            exact.stdout + "extra\n",
            exact.stdout.replace("false", "true"),
            exact.stdout.replace(GATE.REQUIRED_RUSTY_CPP_COMMIT, "0" * 40),
            exact.stdout.replace("}", ',"extra":1}'),
        )
        for stdout in invalid:
            with self.subTest(stdout=stdout):
                result = subprocess.CompletedProcess(
                    args=[], returncode=0, stdout=stdout, stderr=""
                )
                with mock.patch.object(GATE.subprocess, "run", return_value=result):
                    with self.assertRaises(GATE.GateError):
                        GATE.verify_transpiler_build_info(ROOT, Path("transpiler"))

    def test_generated_lane_uses_exact_configured_dependency_closure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="generated-bmi-map-") as raw:
            work = Path(raw)
            fresh = work / "srpc.future.pcm"
            fresh.touch()
            configured = {
                "srpc.future": Path("/production/srpc.future.pcm"),
                "srpc.logging": Path("/production/srpc.logging.pcm"),
                "srpc.reactor": Path("/production/srpc.reactor.pcm"),
                "rusty": Path("/runtime/rusty.pcm"),
            }
            actual = GATE.generated_lane_module_map(
                configured,
                work,
                dependency_names={"srpc.future", "srpc.reactor", "rusty"},
            )
            self.assertEqual(actual["srpc.future"], configured["srpc.future"])
            self.assertNotIn("srpc.logging", actual)
            self.assertEqual(actual["srpc.reactor"], configured["srpc.reactor"])
            self.assertEqual(actual["rusty"], configured["rusty"])
            self.assertNotIn(
                "srpc.future",
                GATE.generated_lane_module_map(
                    configured,
                    work,
                    dependency_names={"srpc.future"},
                    exclude="srpc.future",
                ),
            )


class ExtractionContractTests(unittest.TestCase):
    def test_checked_in_manifest_is_unique_and_physical(self) -> None:
        modules = DRIVER.load_manifest(ROOT, ROOT / "rust-modules.toml")
        self.assertEqual(len(modules), 37)
        self.assertEqual(len({module.cpp_module for module in modules}), 37)
        self.assertTrue(all(module.canonical_source_label for module in modules))

    def test_duplicate_ownership_and_path_escape_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="manifest-negative-", dir=ROOT) as raw:
            directory = Path(raw)
            manifest = directory / "manifest.toml"
            entry = textwrap.dedent(
                """
                [[module]]
                cpp_module = "srpc.basetypes"
                source = "base/basetypes.rs"
                """
            )
            manifest.write_text("schema_version = 2\n" + entry + entry, encoding="utf-8")
            with self.assertRaisesRegex(DRIVER.ExtractionError, "duplicate cpp_module"):
                DRIVER.load_manifest(ROOT, manifest)
            manifest.write_text(
                textwrap.dedent(
                    """
                    schema_version = 2
                    [[module]]
                    cpp_module = "srpc.escape"
                    source = "../escape.cpp"
                    """
                ),
                encoding="utf-8",
            )
            with self.assertRaisesRegex(DRIVER.ExtractionError, "inside the repository"):
                DRIVER.load_manifest(ROOT, manifest)

    def test_manifest_symlink_and_orphan_rust_source_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="manifest-symlink-", dir=ROOT) as raw:
            link = Path(raw) / "manifest.toml"
            link.symlink_to(ROOT / "rust-modules.toml")
            with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
                DRIVER.load_manifest(ROOT, link)

        modules = DRIVER.load_manifest(ROOT, ROOT / "rust-modules.toml")
        generated = [
            DRIVER.GeneratedFile(
                output_label="src/lib.rs",
                output=ROOT / "src/lib.rs",
                content=(ROOT / "src/lib.rs").read_bytes(),
            ),
            *(
                DRIVER.GeneratedFile(
                    output_label=module.output_label,
                    output=module.output,
                    content=module.output.read_bytes(),
                    writable=False,
                )
                for module in modules
            ),
        ]
        orphan = ROOT / "src/goal0_contract_orphan.rs"
        try:
            orphan.write_text("// negative fixture\n", encoding="utf-8")
            with self.assertRaisesRegex(DRIVER.ExtractionError, "orphan Rust source"):
                DRIVER.validate_census(ROOT, generated, allow_missing=False)
        finally:
            orphan.unlink(missing_ok=True)

        # Every module reaches its canonical bytes through the `#[path]` in the
        # generated lib.rs, so a symlink under src/ is a second, unowned route
        # to a source. The census fails on it rather than skipping it, or the
        # retired discovery-shim layer could grow back one file at a time.
        shim = ROOT / "src/goal0_contract_shim.rs"
        try:
            shim.symlink_to(ROOT / "base/basetypes.rs")
            with self.assertRaisesRegex(
                DRIVER.ExtractionError, "census rejects symlinks"
            ):
                DRIVER.validate_census(ROOT, generated, allow_missing=False)
        finally:
            shim.unlink(missing_ok=True)

    def test_canonical_source_is_byte_stable_and_strict(self) -> None:
        canonical = b"pub fn ready() {}\n"
        self.assertIs(
            DRIVER.validate_canonical_source(canonical, "src/ready.rs"), canonical
        )
        for invalid in (
            b"pub fn ready() {}\r\n",
            b"pub fn ready() {}",
            b"\n",
            b"pub fn ready() {\x00}\n",
            b"\xff\n",
        ):
            with self.subTest(invalid=invalid):
                with self.assertRaises(DRIVER.ExtractionError):
                    DRIVER.validate_canonical_source(invalid, "src/ready.rs")

    def test_check_mode_reports_drift_without_rewriting(self) -> None:
        with tempfile.TemporaryDirectory(prefix="extraction-check-") as raw:
            root = Path(raw)
            output = root / "src/lib.rs"
            output.parent.mkdir()
            output.write_bytes(b"old\n")
            generated = [
                DRIVER.GeneratedFile(
                    output_label="src/lib.rs", output=output, content=b"new\n"
                )
            ]
            with self.assertRaisesRegex(DRIVER.ExtractionError, "stale"):
                DRIVER.apply_mode(root, generated, "check")
            self.assertEqual(output.read_bytes(), b"old\n")

    def test_extractor_build_info_contract_matches_gate(self) -> None:
        exact = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout=(
                '{"git_hash":"'
                + DRIVER.REQUIRED_RUSTY_CPP_COMMIT
                + '","git_dirty":false}\n'
            ),
            stderr="",
        )
        with mock.patch.object(DRIVER.subprocess, "run", return_value=exact):
            DRIVER.verify_transpiler_build_info(ROOT, Path("transpiler"))
        dirty = subprocess.CompletedProcess(
            args=[],
            returncode=0,
            stdout=exact.stdout.replace("false", "true"),
            stderr="",
        )
        with mock.patch.object(DRIVER.subprocess, "run", return_value=dirty):
            with self.assertRaisesRegex(DRIVER.ExtractionError, "git_dirty=false"):
                DRIVER.verify_transpiler_build_info(ROOT, Path("transpiler"))


if __name__ == "__main__":
    unittest.main()
