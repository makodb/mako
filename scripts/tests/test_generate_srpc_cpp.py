from __future__ import annotations

import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "generate_srpc_cpp.py"
SPEC = importlib.util.spec_from_file_location("generate_srpc_cpp", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class GeneratorPolicyTest(unittest.TestCase):
    def load_single_module_profile(self, module_extra: str = "") -> dict:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest_path = root / "profile.toml"
            manifest_path.write_text(
                f'''schema_version = {generator.SCHEMA_VERSION}
profile = "test-profile"
crate_root = "crates/srpc"
output_root = "generated"
cxx_namespace = "rrr"
transpiler_git = "{'a' * 40}"
transpiler_sha256 = "{generator.CANONICAL_TRANSPILER_SHA256}"

[[module]]
source = "src/rpc/owner.rs"
rust_module = "crate::rpc::owner"
module_name = "rrr.owner"
output = "rrr.owner.cppm"
kind = "interface"
dependencies = []
gmf_headers = []
{module_extra}
''',
                encoding="utf-8",
            )
            manifest, _ = generator.load_manifest(manifest_path, root)
            return manifest

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
        fields = [
            ("generator-version", str(generator.GENERATOR_VERSION)),
            ("source", "owner.rs"),
        ]
        data = generator.stamp_output("export module rrr.owner;\n", fields)
        generator.verify_stamped_output(Path("owner.cppm"), data, fields)

        tampered = data.replace(b"rrr.owner", b"rrr.other")
        with self.assertRaisesRegex(generator.ProfileError, "integrity drift"):
            generator.verify_stamped_output(Path("owner.cppm"), tampered, fields)

    def test_nested_namespace_export_counts_as_public_interface(self) -> None:
        self.assertTrue(
            generator.has_exported_items(
                "export module rrr.owner;\n"
                "namespace rrr {\n"
                "    export template<typename T> struct Owner;\n"
                "}\n"
            )
        )
        self.assertFalse(
            generator.has_exported_items("export module rrr.owner;\nnamespace rrr {}\n")
        )

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

    def test_module_type_mappings_are_validated_and_sorted(self) -> None:
        manifest = self.load_single_module_profile(
            'type_mappings = { Zed = "vendor::Zed", '
            'LegacyStdString = "std::string" }'
        )
        self.assertEqual(
            manifest["module"][0]["type_mappings"],
            {
                "LegacyStdString": "std::string",
                "Zed": "vendor::Zed",
            },
        )
        self.assertEqual(
            generator.render_type_map(manifest["module"][0]["type_mappings"]),
            b'"LegacyStdString" = "std::string"\n"Zed" = "vendor::Zed"\n',
        )

    def test_module_type_mappings_fail_closed(self) -> None:
        invalid_entries = (
            'type_mappings = ["LegacyStdString"]',
            'type_mappings = { "bad-path" = "std::string" }',
            'type_mappings = { LegacyStdString = "std::vector<int>" }',
            'type_mappings = { LegacyStdString = "std::string; injected" }',
            'type_mappings = { LegacyStdString = 7 }',
        )
        for entry in invalid_entries:
            with self.subTest(entry=entry):
                with self.assertRaises(generator.ProfileError):
                    self.load_single_module_profile(entry)

    def test_type_map_is_scoped_to_one_shell_free_argv(self) -> None:
        common = {
            "transpiler": Path("/tool/rusty-cpp-transpiler"),
            "staged_source": Path("/scratch/source with spaces.rs"),
            "raw_output": Path("/scratch/output.cppm"),
            "module_name": "rrr.owner",
            "cxx_namespace": "rrr",
            "consumer_module_map": Path("/scratch/module-map.json"),
        }
        without_map = generator.build_transpiler_command(**common, type_map=None)
        self.assertNotIn("--type-map", without_map)

        map_path = Path("/scratch/maps with spaces/owner.toml")
        with_map = generator.build_transpiler_command(
            **common, type_map=map_path
        )
        self.assertEqual(with_map[-2:], ["--type-map", str(map_path)])
        self.assertIn(str(common["staged_source"]), with_map)

    def test_output_stamp_binds_exact_module_type_map(self) -> None:
        common = {
            "manifest_path": Path("/repo/profile.toml"),
            "manifest_sha": "1" * 64,
            "profile": "test",
            "source_label": "owner.rs",
            "source_sha": "2" * 64,
            "module_name": "rrr.owner",
            "kind": "interface",
            "transpiler_git": "3" * 40,
            "transpiler_sha256": "4" * 64,
            "root": Path("/repo"),
        }
        first_map_sha = generator.sha256_bytes(
            generator.render_type_map({"Alias": "std::string"})
        )
        second_map_sha = generator.sha256_bytes(
            generator.render_type_map({"Alias": "vendor::String"})
        )
        first_fields = generator.stamp_fields(
            **common, type_map_sha=first_map_sha
        )
        second_fields = generator.stamp_fields(
            **common, type_map_sha=second_map_sha
        )
        data = generator.stamp_output("export module rrr.owner;\n", first_fields)
        generator.verify_stamped_output(Path("owner.cppm"), data, first_fields)
        with self.assertRaisesRegex(generator.ProfileError, "stale type-map-sha256"):
            generator.verify_stamped_output(Path("owner.cppm"), data, second_fields)

    def test_repository_profile_scopes_callbacks_mapping_and_dependencies(self) -> None:
        root = generator.repo_root()
        manifest_path = root / generator.DEFAULT_MANIFEST
        manifest, _ = generator.load_manifest(manifest_path, root)
        modules = {entry["rust_module"]: entry for entry in manifest["module"]}

        callbacks = modules["crate::rpc::callbacks"]
        self.assertEqual(callbacks["dependencies"], ["crate::rpc::errors"])
        self.assertEqual(
            callbacks["type_mappings"],
            {"LegacyStdString": "std::string"},
        )
        generator.validate_dependency_imports(
            "export module rrr.callbacks;\nimport rrr.errors;\n",
            callbacks,
            manifest,
        )
        completion = modules["crate::rpc::completion_tracker"]
        self.assertEqual(completion["dependencies"], [])
        self.assertEqual(completion["type_mappings"], {})

        mapped = [
            entry["rust_module"]
            for entry in manifest["module"]
            if entry["type_mappings"]
        ]
        self.assertEqual(mapped, ["crate::rpc::callbacks"])


if __name__ == "__main__":
    unittest.main()
