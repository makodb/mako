from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "generate_srpc_cpp.py"
SPEC = importlib.util.spec_from_file_location("generate_srpc_cpp", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class GeneratorPolicyTest(unittest.TestCase):
    def test_dependency_imports_must_match_exactly(self) -> None:
        manifest = {
            "module": [
                {
                    "rust_module": "crate::base::clock",
                    "module_name": "rrr.clock",
                    "dependencies": [],
                },
                {
                    "rust_module": "crate::rpc::breaker",
                    "module_name": "rrr.breaker",
                    "dependencies": ["crate::base::clock"],
                },
            ]
        }
        entry = manifest["module"][1]
        generator.validate_dependency_imports(
            "export module rrr.breaker;\nimport rrr.clock;\n", entry, manifest
        )

        with self.assertRaisesRegex(
            generator.ProfileError, "missing emitted imports: rrr.clock"
        ):
            generator.validate_dependency_imports(
                "export module rrr.breaker;\n", entry, manifest
            )

        with self.assertRaisesRegex(
            generator.ProfileError, r"unmapped consumer imports.*rrr\.unknown"
        ):
            generator.validate_dependency_imports(
                "export module rrr.breaker;\n"
                "import rrr.clock;\n"
                "import rrr.unknown;\n",
                entry,
                manifest,
            )

    def test_output_integrity_covers_every_non_checksum_byte(self) -> None:
        fields = [("generator-version", "3"), ("source", "owner.rs")]
        data = generator.stamp_output("export module rrr.owner;\n", fields)
        generator.verify_stamped_output(Path("owner.cppm"), data, fields)

        tampered = data.replace(b"rrr.owner", b"rrr.other")
        with self.assertRaisesRegex(generator.ProfileError, "integrity drift"):
            generator.verify_stamped_output(Path("owner.cppm"), tampered, fields)

    def test_cmake_path_inputs_fail_closed(self) -> None:
        for malicious in (
            "/absolute/path",
            "../escape.cpp",
            'module\".cppm',
            "module;injected.cppm",
            "module${VAR}.cppm",
            "module\\windows.cppm",
        ):
            with self.subTest(malicious=malicious):
                with self.assertRaises(generator.ProfileError):
                    generator.validate_profile_path("test path", malicious)

        generator.validate_profile_path("test path", "crates/srpc/generated/x.cppm")

    def test_cmake_manifest_omits_generated_only_units_from_retired_set(self) -> None:
        root = Path("/repo")
        manifest = {
            "module": [
                {
                    "_output": root / "generated/support.cppm",
                    "_legacy_source": None,
                },
                {
                    "_output": root / "generated/errors.cppm",
                    "_legacy_source": root / "src/rrr/errors.cpp",
                },
            ]
        }
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            generator.emit_cmake_manifest(manifest, root)
        rendered = output.getvalue()
        self.assertIn("generated/support.cppm", rendered)
        self.assertIn("generated/errors.cppm", rendered)
        self.assertIn("src/rrr/errors.cpp", rendered)
        retired = rendered.split("set(SRPC_RETIRED_LEGACY_MODULE_FILES", 1)[1]
        self.assertNotIn("support.cppm", retired)


if __name__ == "__main__":
    unittest.main()
