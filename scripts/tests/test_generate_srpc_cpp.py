from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "generate_srpc_cpp.py"
SPEC = importlib.util.spec_from_file_location("generate_srpc_cpp", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class GeneratorPolicyTest(unittest.TestCase):
    GROUPED_FIXTURE = Path(__file__).resolve().parent / "fixtures/grouped_units"

    @staticmethod
    def valid_cpp_index_document() -> dict:
        return {
            "version": 1,
            "modules": {
                "rrr::serializable": {
                    "cpp_module": "rrr.serializable",
                    "namespace": "rrr",
                    "symbols": {
                        "Serialize_::serialize": {
                            "kind": "function",
                            "callable_signatures": [
                                "void(uint64_t,BinaryWriteArchive&)"
                            ],
                        }
                    },
                }
            },
        }

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
unit_id = "rrr.owner.interface"
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

    def grouped_fixture_text(self) -> str:
        return (self.GROUPED_FIXTURE / "profile.toml").read_text(encoding="utf-8")

    def assert_grouped_profile_rejected(
        self, manifest_text: str, diagnostic: str
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest_path = root / "profile.toml"
            manifest_path.write_text(manifest_text, encoding="utf-8")
            with self.assertRaisesRegex(generator.ProfileError, diagnostic):
                generator.load_manifest(manifest_path, root)

    def test_grouped_two_unit_fixture_is_canonical_and_topological(self) -> None:
        root = self.GROUPED_FIXTURE
        manifest, _ = generator.load_manifest(root / "profile.toml", root)
        interface, implementation = manifest["module"]

        self.assertEqual(interface["kind"], "interface")
        self.assertEqual(implementation["kind"], "implementation")
        self.assertEqual(interface["module_name"], implementation["module_name"])
        self.assertIs(implementation["_module_interface"], interface)
        self.assertEqual(
            implementation["dependencies"], [interface["rust_module"]]
        )

        # The graph edge is real, but importing one's own named module from an
        # implementation unit would be an illegal C++ self-import.
        generator.validate_dependency_imports(
            "module rrr.epoll_wrapper;\n", implementation, manifest
        )
        with self.assertRaisesRegex(
            generator.ProfileError,
            r"undeclared emitted imports: rrr\.epoll_wrapper",
        ):
            generator.validate_dependency_imports(
                "module rrr.epoll_wrapper;\nimport rrr.epoll_wrapper;\n",
                implementation,
                manifest,
            )

        with tempfile.TemporaryDirectory() as temporary:
            module_map_path = Path(temporary) / "consumer-map.json"
            generator.write_consumer_module_map(manifest, module_map_path)
            module_map = json.loads(module_map_path.read_text(encoding="utf-8"))
        self.assertEqual(
            module_map["module"],
            [
                {
                    "rust_module": interface["rust_module"],
                    "cpp_module": "rrr.epoll_wrapper",
                    "cpp_namespace": "rrr",
                }
            ],
        )

        command = generator.build_transpiler_command(
            transpiler=Path("/tool/rusty-cpp-transpiler"),
            staged_source=Path("/scratch/epoll_linux.rs"),
            raw_output=Path("/scratch/rrr.epoll_wrapper-linux.cpp"),
            module_name=implementation["module_name"],
            unit_kind=implementation["kind"],
            rust_module=implementation["rust_module"],
            cxx_namespace="rrr",
            consumer_module_map=Path("/scratch/consumer-map.json"),
            consumer_rust_module=implementation["rust_module"],
            type_map=None,
            cpp_module_index=None,
        )
        rust_scope = command.index("--consumer-rust-module")
        self.assertEqual(
            command[rust_scope : rust_scope + 2],
            ["--consumer-rust-module", "crate::runtime::epoll_linux"],
        )

        cmake = io.StringIO()
        with contextlib.redirect_stdout(cmake):
            generator.emit_cmake_manifest(manifest, root)
        rendered = cmake.getvalue()
        interfaces = rendered.split(
            "set(SRPC_GENERATED_MODULE_INTERFACE_FILES", 1
        )[1].split("set(SRPC_GENERATED_MODULE_IMPLEMENTATION_FILES", 1)[0]
        implementations = rendered.split(
            "set(SRPC_GENERATED_MODULE_IMPLEMENTATION_FILES", 1
        )[1].split(
            "set(SRPC_RETIRED_LEGACY_MODULE_FILES", 1
        )[0]
        self.assertIn("rrr.epoll_wrapper.cppm", interfaces)
        self.assertNotIn("rrr.epoll_wrapper-linux.cpp", interfaces)
        self.assertIn("rrr.epoll_wrapper-linux.cpp", implementations)
        self.assertNotIn("rrr.epoll_wrapper.cppm", implementations)
        self.assertLess(
            rendered.index("set(SRPC_GENERATED_MODULE_INTERFACE_FILES"),
            rendered.index("set(SRPC_GENERATED_MODULE_IMPLEMENTATION_FILES"),
        )
        self.assertLess(
            rendered.index("reactor/epoll_wrapper.cc"),
            rendered.index("reactor/epoll_platform_linux.cc"),
        )
        for source in (
            "crate/src/runtime/epoll.rs",
            "crate/src/runtime/epoll_linux.rs",
        ):
            self.assertEqual(rendered.count(source), 1)
        for output in (
            "generated/rrr.epoll_wrapper.cppm",
            "generated/rrr.epoll_wrapper-linux.cpp",
        ):
            self.assertEqual(rendered.count(output), 2)

    def test_group_allows_multiple_implementation_units(self) -> None:
        second_implementation = '''
[[module]]
unit_id = "rrr.epoll_wrapper.instrumented"
source = "src/runtime/epoll_instrumented.rs"
rust_module = "crate::runtime::epoll_instrumented"
legacy_source = "src/rrr/reactor/epoll_platform_instrumented.cc"
module_name = "rrr.epoll_wrapper"
output = "rrr.epoll_wrapper-instrumented.cpp"
kind = "implementation"
dependencies = ["crate::runtime::epoll"]
gmf_headers = []
'''
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest_path = root / "profile.toml"
            manifest_path.write_text(
                self.grouped_fixture_text() + second_implementation,
                encoding="utf-8",
            )
            manifest, _ = generator.load_manifest(manifest_path, root)
            self.assertEqual(len(manifest["module"]), 3)
            interface = manifest["module"][0]
            self.assertTrue(
                all(
                    entry["_module_interface"] is interface
                    for entry in manifest["module"][1:]
                )
            )

            module_map_path = root / "consumer-map.json"
            generator.write_consumer_module_map(manifest, module_map_path)
            module_map = json.loads(module_map_path.read_text(encoding="utf-8"))
            self.assertEqual(len(module_map["module"]), 1)

    def test_rrr_cmake_does_not_readd_a_retired_platform_unit(self) -> None:
        cmake = (generator.repo_root() / "src/rrr/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn("set(RRR_PLATFORM_MODULE_SRC", cmake)
        retirement = cmake.index(
            "list(REMOVE_ITEM RRR_PLATFORM_MODULE_SRC\n"
            "    ${SRPC_RETIRED_LEGACY_MODULE_FILES})"
        )
        selected_add = cmake.index(
            "target_sources(rrr PRIVATE ${RRR_PLATFORM_MODULE_SRC})"
        )
        self.assertLess(retirement, selected_add)
        self.assertNotIn(
            "target_sources(rrr PRIVATE "
            "${CMAKE_CURRENT_SOURCE_DIR}/reactor/epoll_platform_linux.cc)",
            cmake,
        )

    def test_rrr_cmake_keeps_implementation_units_out_of_module_file_set(
        self,
    ) -> None:
        cmake = (generator.repo_root() / "src/rrr/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        module_sources = cmake.split("set(RRR_MODULE_SRC", 1)[1].split(
            "list(REMOVE_ITEM RRR_MODULE_SRC", 1
        )[0]
        self.assertIn(
            "${SRPC_GENERATED_MODULE_INTERFACE_FILES}", module_sources
        )
        self.assertNotIn(
            "${SRPC_GENERATED_MODULE_IMPLEMENTATION_FILES}", module_sources
        )

        file_set = cmake.split(
            "FILE_SET rrr_modules TYPE CXX_MODULES", 1
        )[1].split("endif()", 1)[0]
        self.assertIn("FILES ${RRR_MODULE_SRC}", file_set)
        self.assertNotIn(
            "${SRPC_GENERATED_MODULE_IMPLEMENTATION_FILES}", file_set
        )
        self.assertIn(
            "PROPERTIES CXX_SCAN_FOR_MODULES ON",
            cmake,
        )
        private_implementations = (
            "target_sources(rrr PRIVATE\n"
            "            ${SRPC_GENERATED_MODULE_IMPLEMENTATION_FILES}\n"
            "        )"
        )
        self.assertIn(private_implementations, cmake)
        self.assertLess(
            cmake.index("FILE_SET rrr_modules TYPE CXX_MODULES"),
            cmake.index(private_implementations),
        )

    def test_unit_id_is_required_unique_and_filename_safe(self) -> None:
        text = self.grouped_fixture_text()
        self.assert_grouped_profile_rejected(
            text.replace('unit_id = "rrr.epoll_wrapper.interface"\n', "", 1),
            "key 'unit_id' must be a non-empty string",
        )
        for invalid in ("../escape", "unit with space", "unit/child", ".hidden"):
            with self.subTest(invalid=invalid):
                self.assert_grouped_profile_rejected(
                    text.replace(
                        "rrr.epoll_wrapper.linux", invalid, 1
                    ),
                    "invalid unit_id",
                )
        self.assert_grouped_profile_rejected(
            text.replace(
                'unit_id = "rrr.epoll_wrapper.linux"',
                'unit_id = "rrr.epoll_wrapper.interface"',
                1,
            ),
            "duplicate module unit_id",
        )

    def test_module_group_requires_one_interface_before_implementations(self) -> None:
        text = self.grouped_fixture_text()
        no_interface = text.replace(
            'output = "rrr.epoll_wrapper.cppm"\nkind = "interface"',
            'output = "rrr.epoll_wrapper-interface.cpp"\nkind = "implementation"',
            1,
        )
        self.assert_grouped_profile_rejected(no_interface, "exactly one interface")

        two_interfaces = text.replace(
            'output = "rrr.epoll_wrapper-linux.cpp"\nkind = "implementation"',
            'output = "rrr.epoll_wrapper-linux.cppm"\nkind = "interface"',
            1,
        )
        self.assert_grouped_profile_rejected(two_interfaces, "exactly one interface")

        header, first, second = text.split("[[module]]")
        self.assert_grouped_profile_rejected(
            header + "[[module]]" + second + "[[module]]" + first,
            "must follow interface unit",
        )

    def test_implementation_requires_its_interface_and_is_not_an_owner(self) -> None:
        text = self.grouped_fixture_text()
        self.assert_grouped_profile_rejected(
            text.replace(
                'dependencies = ["crate::runtime::epoll"]',
                "dependencies = []",
                1,
            ),
            "must depend on its interface",
        )

        consumer = '''
[[module]]
unit_id = "rrr.consumer.interface"
source = "src/consumer.rs"
rust_module = "crate::consumer"
legacy_source = "src/rrr/consumer.cpp"
module_name = "rrr.consumer"
output = "rrr.consumer.cppm"
kind = "interface"
dependencies = ["crate::runtime::epoll_linux"]
gmf_headers = []
'''
        self.assert_grouped_profile_rejected(
            text + consumer,
            "names implementation unit.*depend on canonical interface",
        )

    def test_grouped_units_keep_per_unit_owners_globally_unique(self) -> None:
        text = self.grouped_fixture_text()
        replacements = (
            (
                'source = "src/runtime/epoll_linux.rs"',
                'source = "src/runtime/epoll.rs"',
                "duplicate module source",
            ),
            (
                'legacy_source = "src/rrr/reactor/epoll_platform_linux.cc"',
                'legacy_source = "src/rrr/reactor/epoll_wrapper.cc"',
                "duplicate module legacy_source",
            ),
        )
        for old, new, diagnostic in replacements:
            with self.subTest(field=diagnostic):
                self.assert_grouped_profile_rejected(
                    text.replace(old, new, 1), diagnostic
                )

        duplicate_rust_module = text.replace(
            'rust_module = "crate::runtime::epoll_linux"',
            'rust_module = "crate::runtime::epoll"',
            1,
        ).replace(
            'dependencies = ["crate::runtime::epoll"]',
            "dependencies = []",
            1,
        )
        self.assert_grouped_profile_rejected(
            duplicate_rust_module, "duplicate module rust_module"
        )

        duplicate_output = '''
[[module]]
unit_id = "rrr.epoll_wrapper.linux_debug"
source = "src/runtime/epoll_linux_debug.rs"
rust_module = "crate::runtime::epoll_linux_debug"
legacy_source = "src/rrr/reactor/epoll_platform_linux_debug.cc"
module_name = "rrr.epoll_wrapper"
output = "rrr.epoll_wrapper-linux.cpp"
kind = "implementation"
dependencies = ["crate::runtime::epoll"]
gmf_headers = []
'''
        self.assert_grouped_profile_rejected(
            text + duplicate_output, "duplicate module output"
        )

    def test_dependency_imports_must_match_exactly(self) -> None:
        manifest = {
            "module": [
                {
                    "unit_id": "rrr.clock.interface",
                    "rust_module": "crate::base::clock",
                    "module_name": "rrr.clock",
                    "kind": "interface",
                    "dependencies": [],
                    "legacy_dependencies": [],
                },
                {
                    "unit_id": "rrr.breaker.interface",
                    "rust_module": "crate::rpc::breaker",
                    "module_name": "rrr.breaker",
                    "kind": "interface",
                    "dependencies": ["crate::base::clock"],
                    "legacy_dependencies": ["rrr.serializable"],
                },
            ]
        }
        entry = manifest["module"][1]
        generator.validate_dependency_imports(
            "export module rrr.breaker;\n"
            "import std;\n"
            "import rusty;\n"
            "import rrr.clock;\n"
            "import rrr.serializable;\n",
            entry,
            manifest,
        )

        with self.assertRaisesRegex(
            generator.ProfileError,
            r"missing emitted imports: rrr\.clock, rrr\.serializable",
        ):
            generator.validate_dependency_imports(
                "export module rrr.breaker;\n", entry, manifest
            )

        with self.assertRaisesRegex(
            generator.ProfileError, r"undeclared emitted imports: rrr\.unknown"
        ):
            generator.validate_dependency_imports(
                "export module rrr.breaker;\n"
                "import rrr.clock;\n"
                "import rrr.serializable;\n"
                "import rrr.unknown;\n",
                entry,
                manifest,
            )

        with self.assertRaisesRegex(
            generator.ProfileError, r"unsupported emitted import.*line 4"
        ):
            generator.validate_dependency_imports(
                "export module rrr.breaker;\n"
                "import rrr.clock;\n"
                "import rrr.serializable;\n"
                "import rrr.injected; import vendor.other;\n",
                entry,
                manifest,
            )

        with self.assertRaisesRegex(
            generator.ProfileError, r"undeclared emitted imports: vendor\.other"
        ):
            generator.validate_dependency_imports(
                "export module rrr.breaker;\n"
                "import rrr.clock;\n"
                "import rrr.serializable;\n"
                "import vendor.other;\n",
                entry,
                manifest,
            )

    def test_hand_slots_reject_unsupported_codegen_diagnostics(self) -> None:
        rendered = (
            "export module rrr.example;\n"
            "// UNSUPPORTED: unresolved circular dependency\n"
            "/* UNSUPPORTED: missing lowering */\n"
        )
        self.assertEqual(
            generator.hand_slots(rendered),
            [
                (2, "// UNSUPPORTED: unresolved circular dependency"),
                (3, "/* UNSUPPORTED: missing lowering */"),
            ],
        )

    def test_legacy_dependencies_and_index_paths_fail_closed(self) -> None:
        invalid_dependencies = (
            'legacy_dependencies = ["rrr.serializable;import evil"]',
            'legacy_dependencies = ["rrr::serializable"]',
            'legacy_dependencies = ["rrr.serializable", "rrr.serializable"]',
            'legacy_dependencies = [7]',
            'legacy_dependencies = "rrr.serializable"',
        )
        for declaration in invalid_dependencies:
            with self.subTest(declaration=declaration):
                with self.assertRaises(generator.ProfileError):
                    self.load_single_module_profile(declaration)

        for path in (
            "../index.toml",
            "/index.json",
            "index.txt",
            "x;evil.json",
            "indexes/index with spaces.json",
            "indexes//index.json",
        ):
            with self.subTest(path=path):
                with self.assertRaises(generator.ProfileError):
                    self.load_single_module_profile(
                        f'cpp_module_index = {json.dumps(path)}'
                    )

    def test_cpp_module_index_schema_fails_closed(self) -> None:
        base = self.valid_cpp_index_document()

        def rejected(document: object) -> None:
            with self.assertRaises(generator.ProfileError):
                generator.validate_cpp_module_index(
                    1,
                    Path("index.json"),
                    (json.dumps(document) + "\n").encode(),
                    ["rrr.serializable"],
                )

        malformed = []
        for key, value in (
            ("rrr::serializable;evil", base["modules"]["rrr::serializable"]),
            ("rrr..serializable", base["modules"]["rrr::serializable"]),
        ):
            document = self.valid_cpp_index_document()
            document["modules"] = {key: value}
            malformed.append(document)

        for field, value in (
            ("cpp_module", "rrr.serializable;import evil"),
            ("namespace", "rrr;evil"),
        ):
            document = self.valid_cpp_index_document()
            document["modules"]["rrr::serializable"][field] = value
            malformed.append(document)

        document = self.valid_cpp_index_document()
        symbol = document["modules"]["rrr::serializable"]["symbols"].pop(
            "Serialize_::serialize"
        )
        document["modules"]["rrr::serializable"]["symbols"][
            "Serialize_::serialize;evil"
        ] = symbol
        malformed.append(document)

        document = self.valid_cpp_index_document()
        document["modules"]["rrr::serializable"]["symbols"][
            "Serialize_::serialize"
        ]["callable_signatures"] = ["void(); import evil"]
        malformed.append(document)

        document = self.valid_cpp_index_document()
        document["modules"]["rrr::serializable"]["unknown"] = "value"
        malformed.append(document)

        for document in malformed:
            with self.subTest(document=document):
                rejected(document)

        with self.assertRaisesRegex(
            generator.ProfileError, "undeclared legacy dependency"
        ):
            generator.validate_cpp_module_index(
                1,
                Path("index.json"),
                (json.dumps(base) + "\n").encode(),
                ["rrr.other"],
            )

        implicit_std = {
            "version": 1,
            "modules": {
                "std": {
                    "cpp_module": "std",
                    "namespace": "std",
                    "symbols": {
                        "string": {"kind": "type", "callable_signatures": []},
                        "vector": {
                            "kind": "type_template",
                            "callable_signatures": [],
                        },
                    },
                }
            },
        }
        normalized, _ = generator.validate_cpp_module_index(
            1,
            Path("std-index.json"),
            (json.dumps(implicit_std) + "\n").encode(),
            [],
        )
        self.assertEqual(normalized["modules"]["std"]["cpp_module"], "std")

        duplicate_json = (
            '{"version":1,"version":1,"modules":{}}\n'.encode()
        )
        with self.assertRaisesRegex(generator.ProfileError, "duplicate JSON key"):
            generator.validate_cpp_module_index(
                1,
                Path("index.json"),
                duplicate_json,
                ["rrr.serializable"],
            )

    def test_cpp_module_index_canonical_bytes_are_deterministic(self) -> None:
        document = self.valid_cpp_index_document()
        json_raw = json.dumps(document, indent=4).encode()
        toml_raw = b'''version = 1

[modules."rrr::serializable"]
namespace = "rrr"
cpp_module = "rrr.serializable"

[modules."rrr::serializable".symbols."Serialize_::serialize"]
callable_signatures = ["void(uint64_t,BinaryWriteArchive&)"]
kind = "function"
'''
        json_index, json_bytes = generator.validate_cpp_module_index(
            1, Path("index.json"), json_raw, ["rrr.serializable"]
        )
        toml_index, toml_bytes = generator.validate_cpp_module_index(
            1, Path("index.toml"), toml_raw, ["rrr.serializable"]
        )
        self.assertEqual(json_index, toml_index)
        self.assertEqual(json_bytes, toml_bytes)
        self.assertNotEqual(
            generator.sha256_bytes(json_raw), generator.sha256_bytes(toml_raw)
        )
        self.assertEqual(
            generator.render_legacy_dependencies(
                ["rrr.threading", "rrr.serializable"]
            ),
            generator.render_legacy_dependencies(
                ["rrr.serializable", "rrr.threading"]
            ),
        )

        with tempfile.TemporaryDirectory() as temporary:
            path = generator.stage_cpp_module_index(
                {
                    "unit_id": "rrr.owner.interface",
                    "module_name": "rrr.owner",
                    "_cpp_module_index_bytes": json_bytes,
                },
                Path(temporary),
            )
            assert path is not None
            self.assertEqual(path.read_bytes(), json_bytes)
            self.assertEqual(path.stat().st_mode & 0o777, 0o400)

    def test_grouped_sidecars_are_keyed_by_unit_id(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            destination = Path(temporary)
            interface = {
                "unit_id": "rrr.shared.interface",
                "module_name": "rrr.shared",
                "type_mappings": {"Alias": "std::string"},
                "_cpp_module_index_bytes": b'{"version":1}\n',
            }
            implementation = {
                "unit_id": "rrr.shared.linux",
                "module_name": "rrr.shared",
                "type_mappings": {"Alias": "vendor::String"},
                "_cpp_module_index_bytes": b'{"version":1,"modules":{}}\n',
            }
            interface_type_map = generator.stage_type_map(interface, destination)
            implementation_type_map = generator.stage_type_map(
                implementation, destination
            )
            interface_index = generator.stage_cpp_module_index(
                interface, destination
            )
            implementation_index = generator.stage_cpp_module_index(
                implementation, destination
            )

            assert interface_type_map is not None
            assert implementation_type_map is not None
            assert interface_index is not None
            assert implementation_index is not None
            self.assertNotEqual(interface_type_map, implementation_type_map)
            self.assertNotEqual(interface_index, implementation_index)
            self.assertEqual(interface_type_map.name, "rrr.shared.interface.toml")
            self.assertEqual(implementation_index.name, "rrr.shared.linux.json")
            self.assertEqual(
                interface_type_map.read_bytes(), b'"Alias" = "std::string"\n'
            )
            self.assertEqual(
                implementation_type_map.read_bytes(),
                b'"Alias" = "vendor::String"\n',
            )

    def test_manifest_loads_external_index_and_rejects_owned_legacy_overlap(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            index_path = root / "indexes" / "owner.toml"
            index_path.parent.mkdir()
            index_path.write_text(
                '''version = 1
[modules."rrr::serializable"]
cpp_module = "rrr.serializable"
namespace = "rrr"
[modules."rrr::serializable".symbols.serialize]
kind = "function"
callable_signatures = ["void(uint64_t)"]
''',
                encoding="utf-8",
            )
            manifest_path = root / "profile.toml"
            manifest_path.write_text(
                f'''schema_version = {generator.SCHEMA_VERSION}
profile = "test"
crate_root = "crate"
output_root = "generated"
cxx_namespace = "rrr"
transpiler_git = "{'a' * 40}"
transpiler_sha256 = "{generator.CANONICAL_TRANSPILER_SHA256}"
[[module]]
unit_id = "rrr.clock.interface"
source = "clock.rs"
rust_module = "crate::clock"
module_name = "rrr.clock"
output = "rrr.clock.cppm"
kind = "interface"
dependencies = []
[[module]]
unit_id = "rrr.owner.interface"
source = "owner.rs"
rust_module = "crate::owner"
module_name = "rrr.owner"
output = "rrr.owner.cppm"
kind = "interface"
dependencies = ["crate::clock"]
legacy_dependencies = ["rrr.serializable"]
cpp_module_index = "indexes/owner.toml"
''',
                encoding="utf-8",
            )
            manifest, _ = generator.load_manifest(manifest_path, root)
            owner = manifest["module"][1]
            self.assertEqual(owner["legacy_dependencies"], ["rrr.serializable"])
            self.assertEqual(owner["_cpp_module_index_path"], index_path)
            self.assertIn(b'"cpp_module":"rrr.serializable"', owner["_cpp_module_index_bytes"])

            manifest_text = manifest_path.read_text(encoding="utf-8").replace(
                'legacy_dependencies = ["rrr.serializable"]',
                'legacy_dependencies = ["rrr.clock", "rrr.serializable"]',
            )
            manifest_path.write_text(manifest_text, encoding="utf-8")
            with self.assertRaisesRegex(
                generator.ProfileError, "manifest-owned dependency as legacy"
            ):
                generator.load_manifest(manifest_path, root)

            # Ownership is global to the manifest, not conditional on the
            # consumer also spelling the corresponding Rust dependency.  A
            # missing dependency edge must not turn an owned module into a
            # valid legacy import.
            manifest_text = manifest_path.read_text(encoding="utf-8").replace(
                'dependencies = ["crate::clock"]', "dependencies = []"
            )
            manifest_path.write_text(manifest_text, encoding="utf-8")
            with self.assertRaisesRegex(
                generator.ProfileError, "manifest-owned dependency as legacy"
            ):
                generator.load_manifest(manifest_path, root)

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
            "path//not-normal.cppm",
            "path/./not-normal.cppm",
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
                    "unit_id": "support-interface",
                    "kind": "interface",
                    "_source": root / "crate/support.rs",
                    "_output": root / "generated/support.cppm",
                    "_legacy_source": None,
                    "_cpp_module_index_path": None,
                },
                {
                    "unit_id": "errors-interface",
                    "kind": "interface",
                    "_source": root / "crate/errors.rs",
                    "_output": root / "generated/errors.cppm",
                    "_legacy_source": root / "src/rrr/errors.cpp",
                    "_cpp_module_index_path": None,
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
            'LegacyCString = "const char*", LegacyStdString = "std::string" }'
        )
        self.assertEqual(
            manifest["module"][0]["type_mappings"],
            {
                "LegacyCString": "const char*",
                "LegacyStdString": "std::string",
                "Zed": "vendor::Zed",
            },
        )
        self.assertEqual(
            generator.render_type_map(manifest["module"][0]["type_mappings"]),
            b'"LegacyCString" = "const char*"\n'
            b'"LegacyStdString" = "std::string"\n'
            b'"Zed" = "vendor::Zed"\n',
        )

    def test_module_type_mappings_fail_closed(self) -> None:
        invalid_entries = (
            'type_mappings = ["LegacyStdString"]',
            'type_mappings = { "bad-path" = "std::string" }',
            'type_mappings = { LegacyStdString = "std::vector<int>" }',
            'type_mappings = { LegacyStdString = "std::string; injected" }',
            'type_mappings = { LegacyPointer = "char*" }',
            'type_mappings = { LegacyPointer = "const void*" }',
            'type_mappings = { LegacyPointer = "const char *" }',
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
            "unit_kind": "interface",
            "rust_module": "crate::rpc::owner",
            "cxx_namespace": "rrr",
            "consumer_module_map": Path("/scratch/module-map.json"),
            "consumer_rust_module": None,
        }
        without_map = generator.build_transpiler_command(
            **common, type_map=None, cpp_module_index=None
        )
        self.assertNotIn("--type-map", without_map)
        self.assertNotIn("--cpp-module-index", without_map)

        map_path = Path("/scratch/maps with spaces/owner.toml")
        with_map = generator.build_transpiler_command(
            **common, type_map=map_path, cpp_module_index=None
        )
        self.assertEqual(with_map[-2:], ["--type-map", str(map_path)])
        self.assertIn(str(common["staged_source"]), with_map)

        index_path = Path("/scratch/indexes with spaces/owner.json")
        with_index = generator.build_transpiler_command(
            **common, type_map=None, cpp_module_index=index_path
        )
        self.assertEqual(
            with_index[-2:], ["--cpp-module-index", str(index_path)]
        )
        self.assertNotIn("--type-map", with_index)

    def test_consumer_rust_module_override_is_implementation_scoped(self) -> None:
        common = {
            "transpiler": Path("/tool/rusty-cpp-transpiler"),
            "staged_source": Path("/scratch/epoll_linux.rs"),
            "raw_output": Path("/scratch/epoll-linux.cpp"),
            "module_name": "rrr.epoll_wrapper",
            "unit_kind": "implementation",
            "rust_module": "crate::runtime::epoll_linux",
            "cxx_namespace": "rrr",
            "consumer_module_map": Path("/scratch/module-map.json"),
            "type_map": None,
            "cpp_module_index": None,
        }
        with self.assertRaisesRegex(
            generator.ProfileError, "requires --consumer-rust-module"
        ):
            generator.build_transpiler_command(
                **common, consumer_rust_module=None
            )
        with self.assertRaisesRegex(
            generator.ProfileError, "must equal manifest rust_module"
        ):
            generator.build_transpiler_command(
                **common,
                consumer_rust_module="crate::runtime::wrong_platform",
            )

        command = generator.build_transpiler_command(
            **common,
            consumer_rust_module="crate::runtime::epoll_linux",
        )
        position = command.index("--consumer-rust-module")
        self.assertEqual(
            command[position : position + 2],
            ["--consumer-rust-module", "crate::runtime::epoll_linux"],
        )

        with self.assertRaisesRegex(
            generator.ProfileError, "must not override its canonical"
        ):
            generator.build_transpiler_command(
                **{
                    **common,
                    "unit_kind": "interface",
                    "rust_module": "crate::runtime::epoll",
                },
                consumer_rust_module="crate::runtime::epoll",
            )

    def test_output_stamp_binds_exact_module_type_map(self) -> None:
        common = {
            "manifest_path": Path("/repo/profile.toml"),
            "manifest_sha": "1" * 64,
            "profile": "test",
            "source_label": "owner.rs",
            "source_sha": "2" * 64,
            "unit_id": "rrr.owner.interface",
            "module_name": "rrr.owner",
            "kind": "interface",
            "transpiler_git": "3" * 40,
            "transpiler_sha256": "4" * 64,
            "root": Path("/repo"),
            "legacy_dependencies_sha": generator.sha256_bytes(
                generator.render_legacy_dependencies([])
            ),
            "cpp_module_index_source_sha": generator.sha256_bytes(b""),
            "cpp_module_index_sha": generator.sha256_bytes(b""),
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

        other_unit_fields = generator.stamp_fields(
            **{**common, "unit_id": "rrr.owner.linux"},
            type_map_sha=first_map_sha,
        )
        with self.assertRaisesRegex(generator.ProfileError, "stale unit-id"):
            generator.verify_stamped_output(
                Path("owner.cppm"), data, other_unit_fields
            )

    def test_output_stamp_binds_legacy_set_and_both_index_byte_streams(self) -> None:
        common = {
            "manifest_path": Path("/repo/profile.toml"),
            "manifest_sha": "1" * 64,
            "profile": "test",
            "source_label": "owner.rs",
            "source_sha": "2" * 64,
            "unit_id": "rrr.owner.interface",
            "module_name": "rrr.owner",
            "kind": "interface",
            "type_map_sha": generator.sha256_bytes(b""),
            "transpiler_git": "3" * 40,
            "transpiler_sha256": "4" * 64,
            "root": Path("/repo"),
        }
        base = generator.stamp_fields(
            **common,
            legacy_dependencies_sha=generator.sha256_bytes(
                generator.render_legacy_dependencies(["rrr.serializable"])
            ),
            cpp_module_index_source_sha=generator.sha256_bytes(b"source-a"),
            cpp_module_index_sha=generator.sha256_bytes(b"canonical-a"),
        )
        data = generator.stamp_output("export module rrr.owner;\n", base)

        changes = (
            (
                "legacy-dependencies-sha256",
                {
                    "legacy_dependencies_sha": generator.sha256_bytes(
                        generator.render_legacy_dependencies(["rrr.threading"])
                    ),
                    "cpp_module_index_source_sha": generator.sha256_bytes(b"source-a"),
                    "cpp_module_index_sha": generator.sha256_bytes(b"canonical-a"),
                },
            ),
            (
                "cpp-module-index-source-sha256",
                {
                    "legacy_dependencies_sha": generator.sha256_bytes(
                        generator.render_legacy_dependencies(["rrr.serializable"])
                    ),
                    "cpp_module_index_source_sha": generator.sha256_bytes(b"source-b"),
                    "cpp_module_index_sha": generator.sha256_bytes(b"canonical-a"),
                },
            ),
            (
                "cpp-module-index-sha256",
                {
                    "legacy_dependencies_sha": generator.sha256_bytes(
                        generator.render_legacy_dependencies(["rrr.serializable"])
                    ),
                    "cpp_module_index_source_sha": generator.sha256_bytes(b"source-a"),
                    "cpp_module_index_sha": generator.sha256_bytes(b"canonical-b"),
                },
            ),
        )
        for stamp_name, values in changes:
            with self.subTest(stamp_name=stamp_name):
                changed = generator.stamp_fields(**common, **values)
                with self.assertRaisesRegex(
                    generator.ProfileError, f"stale {stamp_name}"
                ):
                    generator.verify_stamped_output(
                        Path("owner.cppm"), data, changed
                    )

    def test_check_stamps_recomputes_external_index_bytes_offline(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            crate = root / "crate"
            output_dir = root / "generated"
            indexes = root / "indexes"
            crate.mkdir()
            output_dir.mkdir()
            indexes.mkdir()
            source = crate / "owner.rs"
            source.write_text("pub struct Owner {}\n", encoding="utf-8")
            index_path = indexes / "owner.toml"
            index_path.write_text(
                '''version = 1
[modules."rrr::serializable"]
cpp_module = "rrr.serializable"
namespace = "rrr"
[modules."rrr::serializable".symbols.serialize]
kind = "function"
callable_signatures = ["void(uint64_t)"]
''',
                encoding="utf-8",
            )
            manifest_path = root / "profile.toml"
            manifest_path.write_text(
                f'''schema_version = {generator.SCHEMA_VERSION}
profile = "test"
crate_root = "crate"
output_root = "generated"
cxx_namespace = "rrr"
transpiler_git = "{'a' * 40}"
transpiler_sha256 = "{generator.CANONICAL_TRANSPILER_SHA256}"
[[module]]
unit_id = "rrr.owner.interface"
source = "owner.rs"
rust_module = "crate::owner"
module_name = "rrr.owner"
output = "rrr.owner.cppm"
kind = "interface"
dependencies = []
legacy_dependencies = ["rrr.serializable"]
cpp_module_index = "indexes/owner.toml"
''',
                encoding="utf-8",
            )
            manifest, manifest_raw = generator.load_manifest(manifest_path, root)
            entry = manifest["module"][0]
            fields = generator.stamp_fields(
                manifest_path=manifest_path,
                manifest_sha=generator.sha256_bytes(manifest_raw),
                profile=manifest["profile"],
                source_label="crate/owner.rs",
                source_sha=generator.sha256_bytes(source.read_bytes()),
                unit_id="rrr.owner.interface",
                module_name="rrr.owner",
                kind="interface",
                type_map_sha=generator.sha256_bytes(b""),
                legacy_dependencies_sha=generator.sha256_bytes(
                    generator.render_legacy_dependencies(["rrr.serializable"])
                ),
                cpp_module_index_source_sha=generator.sha256_bytes(
                    entry["_cpp_module_index_raw"]
                ),
                cpp_module_index_sha=generator.sha256_bytes(
                    entry["_cpp_module_index_bytes"]
                ),
                transpiler_git=manifest["transpiler_git"],
                transpiler_sha256=manifest["transpiler_sha256"],
                root=root,
            )
            entry["_output"].write_bytes(
                generator.stamp_output(
                    "export module rrr.owner;\n"
                    "import rrr.serializable;\n"
                    "export struct Owner {};\n",
                    fields,
                )
            )
            with contextlib.redirect_stdout(io.StringIO()):
                generator.check_stamps(
                    manifest, manifest_path, manifest_raw, root
                )

            with index_path.open("a", encoding="utf-8") as handle:
                handle.write("\n# spelling-only source-byte drift\n")
            changed, changed_raw = generator.load_manifest(manifest_path, root)
            with self.assertRaisesRegex(
                generator.ProfileError,
                "stale cpp-module-index-source-sha256",
            ):
                generator.check_stamps(
                    changed, manifest_path, changed_raw, root
                )

    def test_cmake_manifest_tracks_sources_and_index_as_configure_dependencies(self) -> None:
        root = Path("/repo")
        index = root / "crates/srpc/cpp/indexes/owner.toml"
        owner = root / "crates/srpc/src/owner.rs"
        sibling = root / "crates/srpc/src/sibling.rs"
        manifest = {
            "module": [
                {
                    "unit_id": "owner-interface",
                    "kind": "interface",
                    "_source": owner,
                    "_output": root / "generated/owner.cppm",
                    "_legacy_source": root / "src/rrr/owner.cpp",
                    "_cpp_module_index_path": index,
                },
                {
                    "unit_id": "sibling-interface",
                    "kind": "interface",
                    "_source": sibling,
                    "_output": root / "generated/sibling.cppm",
                    "_legacy_source": None,
                    "_cpp_module_index_path": index,
                },
            ]
        }
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            generator.emit_cmake_manifest(manifest, root)
        rendered = output.getvalue()
        self.assertEqual(rendered.count(str(index.relative_to(root))), 1)
        self.assertEqual(rendered.count(str(owner.relative_to(root))), 1)
        self.assertEqual(rendered.count(str(sibling.relative_to(root))), 1)
        self.assertEqual(rendered.count("generated/owner.cppm"), 2)
        self.assertEqual(rendered.count("generated/sibling.cppm"), 2)
        self.assertIn("CMAKE_CONFIGURE_DEPENDS", rendered)
        self.assertIn("${SRPC_CPP_PROFILE_INPUT_FILES}", rendered)

    def test_repository_profile_scopes_type_mappings_and_dependencies(self) -> None:
        root = generator.repo_root()
        manifest_path = root / generator.DEFAULT_MANIFEST
        manifest, _ = generator.load_manifest(manifest_path, root)
        modules = {entry["rust_module"]: entry for entry in manifest["module"]}
        self.assertEqual(len(manifest["module"]), 32)
        self.assertEqual(
            len({entry["unit_id"] for entry in manifest["module"]}),
            32,
        )

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

        legacy_basetypes = modules["crate::base::legacy_basetypes"]
        self.assertEqual(legacy_basetypes["dependencies"], [])
        self.assertEqual(legacy_basetypes["legacy_dependencies"], [])
        self.assertEqual(legacy_basetypes["gmf_headers"], [])
        self.assertEqual(legacy_basetypes["type_mappings"], {})
        self.assertIsNotNone(legacy_basetypes["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"rusty"',
            legacy_basetypes["_cpp_module_index_bytes"],
        )
        generator.validate_dependency_imports(
            "export module rrr.basetypes;\nimport rusty;\n",
            legacy_basetypes,
            manifest,
        )

        inmemory_channel = modules["crate::rpc::inmemory_channel"]
        self.assertEqual(
            inmemory_channel["dependencies"], ["crate::rpc::channel"]
        )
        self.assertEqual(inmemory_channel["legacy_dependencies"], [])
        self.assertEqual(inmemory_channel["gmf_headers"], [])
        self.assertEqual(
            inmemory_channel["type_mappings"],
            {"LegacyStdString": "std::string"},
        )
        self.assertIsNone(inmemory_channel["_cpp_module_index_path"])
        self.assertEqual(inmemory_channel["_cpp_module_index_bytes"], b"")
        generator.validate_dependency_imports(
            "export module rrr.inmemory_channel;\nimport rrr.channel;\n",
            inmemory_channel,
            manifest,
        )

        fiber_channel = modules["crate::rpc::fiber_channel"]
        self.assertEqual(
            fiber_channel["dependencies"], ["crate::rpc::channel"]
        )
        self.assertEqual(fiber_channel["legacy_dependencies"], ["rrr.reactor"])
        self.assertEqual(fiber_channel["gmf_headers"], [])
        self.assertEqual(
            fiber_channel["type_mappings"],
            {
                "LegacyChannelConnectionBase": "rrr::ChannelConnectionBase",
                "LegacyStdDeque": "std::deque",
                "std::marker::PhantomPinned": "rusty::marker::PhantomPinned",
            },
        )
        self.assertIsNotNone(fiber_channel["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"rrr.reactor"',
            fiber_channel["_cpp_module_index_bytes"],
        )
        generator.validate_dependency_imports(
            "export module rrr.fiber_channel;\n"
            "import rrr.channel;\n"
            "import rrr.reactor;\n",
            fiber_channel,
            manifest,
        )

        legacy_future = modules["crate::runtime::legacy_future"]
        self.assertEqual(legacy_future["dependencies"], [])
        self.assertEqual(legacy_future["legacy_dependencies"], ["rrr.reactor"])
        self.assertEqual(legacy_future["gmf_headers"], [])
        self.assertEqual(
            legacy_future["type_mappings"], {"LegacyStdPair": "std::pair"}
        )
        self.assertIsNotNone(legacy_future["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"rrr.reactor"',
            legacy_future["_cpp_module_index_bytes"],
        )
        self.assertIn(
            b'"cpp_module":"std"', legacy_future["_cpp_module_index_bytes"]
        )
        generator.validate_dependency_imports(
            "export module rrr.future;\nimport rrr.reactor;\nimport std;\n",
            legacy_future,
            manifest,
        )

        legacy_fiber = modules["crate::runtime::legacy_fiber"]
        self.assertEqual(
            legacy_fiber["dependencies"], ["crate::base::legacy_basetypes"]
        )
        self.assertEqual(legacy_fiber["legacy_dependencies"], ["rrr.reactor"])
        self.assertEqual(legacy_fiber["gmf_headers"], [])
        self.assertEqual(
            legacy_fiber["type_mappings"], {"LegacyFiber": "rrr::Fiber"}
        )
        self.assertIsNotNone(legacy_fiber["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"rrr.reactor"',
            legacy_fiber["_cpp_module_index_bytes"],
        )
        generator.validate_dependency_imports(
            "export module rrr.fiber;\n"
            "import rrr.basetypes;\n"
            "import rrr.reactor;\n",
            legacy_fiber,
            manifest,
        )

        frame_codec = modules["crate::rpc::frame_codec"]
        self.assertEqual(
            frame_codec["type_mappings"]["LegacyCString"], "const char*"
        )

        serializable_envelope = modules["crate::rpc::serializable_envelope"]
        self.assertEqual(
            serializable_envelope["dependencies"],
            ["crate::base::legacy_basetypes"],
        )
        self.assertEqual(
            serializable_envelope["legacy_dependencies"],
            ["rrr.debugging", "rrr.serializable"],
        )
        self.assertEqual(serializable_envelope["gmf_headers"], [])
        self.assertEqual(serializable_envelope["type_mappings"], {})
        self.assertIsNotNone(serializable_envelope["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"rrr.debugging"',
            serializable_envelope["_cpp_module_index_bytes"],
        )
        self.assertIn(
            b'"cpp_module":"rrr.serializable"',
            serializable_envelope["_cpp_module_index_bytes"],
        )
        self.assertIn(
            b'"cpp_module":"rusty"',
            serializable_envelope["_cpp_module_index_bytes"],
        )
        generator.validate_dependency_imports(
            "export module rrr.serializable_envelope;\n"
            "import rrr.basetypes;\n"
            "import rrr.debugging;\n"
            "import rrr.serializable;\n"
            "import rusty;\n",
            serializable_envelope,
            manifest,
        )

        any_message = modules["crate::rpc::any_message"]
        self.assertEqual(any_message["unit_id"], "rrr.any_message.interface")
        self.assertEqual(any_message["dependencies"], [])
        self.assertEqual(
            any_message["legacy_dependencies"],
            ["rrr.debugging", "rrr.serializable"],
        )
        self.assertEqual(any_message["gmf_headers"], [])
        self.assertEqual(
            any_message["type_mappings"],
            {"LegacyStdString": "std::string"},
        )
        self.assertIsNotNone(any_message["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"rrr.debugging"',
            any_message["_cpp_module_index_bytes"],
        )
        self.assertIn(
            b'"cpp_module":"rrr.serializable"',
            any_message["_cpp_module_index_bytes"],
        )
        self.assertIn(
            b'"cpp_module":"rusty"',
            any_message["_cpp_module_index_bytes"],
        )
        self.assertIn(
            b'"arc_make_default"',
            any_message["_cpp_module_index_bytes"],
        )
        self.assertIn(
            b'"type_id_hash_code"',
            any_message["_cpp_module_index_bytes"],
        )
        generator.validate_dependency_imports(
            "export module rrr.any_message;\n"
            "import rrr.debugging;\n"
            "import rrr.serializable;\n"
            "import rusty;\n",
            any_message,
            manifest,
        )

        load_balancer = modules["crate::rpc::load_balancer"]
        self.assertEqual(load_balancer["dependencies"], [])
        self.assertEqual(load_balancer["legacy_dependencies"], [])
        self.assertEqual(load_balancer["gmf_headers"], [])
        self.assertEqual(load_balancer["type_mappings"], {})
        self.assertIsNotNone(load_balancer["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"rusty"',
            load_balancer["_cpp_module_index_bytes"],
        )
        generator.validate_dependency_imports(
            "export module rrr.load_balancer;\nimport rusty;\n",
            load_balancer,
            manifest,
        )

        misc = modules["crate::base::misc"]
        self.assertEqual(misc["dependencies"], [])
        self.assertEqual(
            misc["type_mappings"], {"LegacyStdString": "std::string"}
        )

        idempotency = modules["crate::rpc::idempotency"]
        self.assertEqual(idempotency["dependencies"], [])
        self.assertEqual(idempotency["legacy_dependencies"], ["rrr.serializable"])
        self.assertIsNotNone(idempotency["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"rrr.serializable"',
            idempotency["_cpp_module_index_bytes"],
        )
        generator.validate_dependency_imports(
            "export module rrr.idempotency;\nimport rrr.serializable;\n",
            idempotency,
            manifest,
        )

        legacy_rand = modules["crate::base::legacy_rand"]
        self.assertEqual(legacy_rand["dependencies"], [])
        self.assertEqual(legacy_rand["legacy_dependencies"], [])
        self.assertIsNotNone(legacy_rand["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"std"', legacy_rand["_cpp_module_index_bytes"]
        )
        generator.validate_dependency_imports(
            "export module rrr.rand;\nimport std;\n",
            legacy_rand,
            manifest,
        )

        legacy_threading = modules["crate::base::legacy_threading"]
        self.assertEqual(legacy_threading["dependencies"], [])
        self.assertEqual(
            legacy_threading["legacy_dependencies"], ["rrr.debugging"]
        )
        self.assertEqual(legacy_threading["gmf_headers"], ["<pthread.h>"])
        self.assertEqual(
            legacy_threading["type_mappings"],
            {
                "LegacyPthreadCond": "pthread_cond_t",
                "LegacyPthreadCondAttr": "pthread_condattr_t",
                "LegacyPthreadMutex": "pthread_mutex_t",
                "LegacyPthreadMutexAttr": "pthread_mutexattr_t",
                "LegacyPthreadSpinlock": "pthread_spinlock_t",
            },
        )
        self.assertIsNotNone(legacy_threading["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"rrr.debugging"',
            legacy_threading["_cpp_module_index_bytes"],
        )
        self.assertIn(
            b'"cpp_module":"rusty"',
            legacy_threading["_cpp_module_index_bytes"],
        )
        generator.validate_dependency_imports(
            "export module rrr.threading;\nimport rrr.debugging;\n",
            legacy_threading,
            manifest,
        )

        legacy_logging = modules["crate::base::legacy_logging"]
        self.assertEqual(legacy_logging["dependencies"], [])
        self.assertEqual(
            legacy_logging["legacy_dependencies"], ["rrr.debugging"]
        )
        self.assertEqual(legacy_logging["gmf_headers"], [])
        self.assertEqual(
            legacy_logging["type_mappings"],
            {
                "LegacyCChar": "std::string::value_type",
                "LegacyStdString": "std::string",
            },
        )
        self.assertIsNotNone(legacy_logging["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"rrr.debugging"',
            legacy_logging["_cpp_module_index_bytes"],
        )
        self.assertIn(
            b'"cpp_module":"std"',
            legacy_logging["_cpp_module_index_bytes"],
        )
        generator.validate_dependency_imports(
            "export module rrr.logging;\n"
            "import rrr.debugging;\n"
            "import std;\n",
            legacy_logging,
            manifest,
        )

        legacy_cpuinfo = modules["crate::base::legacy_cpuinfo"]
        self.assertEqual(
            legacy_cpuinfo["dependencies"], ["crate::base::legacy_logging"]
        )
        self.assertEqual(legacy_cpuinfo["legacy_dependencies"], [])
        self.assertEqual(legacy_cpuinfo["gmf_headers"], [])
        self.assertEqual(
            legacy_cpuinfo["type_mappings"],
            {"LegacyCChar": "std::string::value_type"},
        )
        self.assertIsNotNone(legacy_cpuinfo["_cpp_module_index_path"])
        self.assertNotIn(
            b'"cpp_module":"rrr.logging"',
            legacy_cpuinfo["_cpp_module_index_bytes"],
        )
        self.assertIn(
            b'"cpp_module":"rusty"',
            legacy_cpuinfo["_cpp_module_index_bytes"],
        )
        self.assertIn(
            b'"cpp_module":"std"',
            legacy_cpuinfo["_cpp_module_index_bytes"],
        )
        generator.validate_dependency_imports(
            "export module rrr.cpuinfo;\n"
            "import rrr.logging;\n"
            "import rusty;\n"
            "import std;\n",
            legacy_cpuinfo,
            manifest,
        )

        utils = modules["crate::rpc::utils"]
        self.assertEqual(utils["dependencies"], ["crate::base::legacy_logging"])
        self.assertEqual(utils["legacy_dependencies"], [])
        self.assertEqual(utils["gmf_headers"], ["<netdb.h>"])
        self.assertEqual(
            utils["type_mappings"],
            {
                "LegacyAddrInfo": "addrinfo",
                "LegacyStdString": "std::string",
            },
        )
        self.assertIsNotNone(utils["_cpp_module_index_path"])
        self.assertIn(
            b'"cpp_module":"rusty"', utils["_cpp_module_index_bytes"]
        )
        generator.validate_dependency_imports(
            "export module rrr.utils;\n"
            "import rrr.logging;\n"
            "import rusty;\n",
            utils,
            manifest,
        )

        for entry in modules.values():
            if any(
                entry is indexed
                for indexed in (
                    idempotency,
                    legacy_rand,
                    fiber_channel,
                    legacy_basetypes,
                    legacy_future,
                    legacy_fiber,
                    legacy_threading,
                    legacy_logging,
                    legacy_cpuinfo,
                    serializable_envelope,
                    any_message,
                    utils,
                    load_balancer,
                )
            ):
                continue
            self.assertEqual(entry["legacy_dependencies"], [])
            self.assertIsNone(entry["_cpp_module_index_path"])
            self.assertEqual(entry["_cpp_module_index_bytes"], b"")

        mapped = [
            entry["rust_module"]
            for entry in manifest["module"]
            if entry["type_mappings"]
        ]
        self.assertEqual(
            mapped,
            [
                "crate::rpc::any_message",
                "crate::rpc::frame_codec",
                "crate::rpc::callbacks",
                "crate::rpc::channel",
                "crate::rpc::inmemory_channel",
                "crate::rpc::fiber_channel",
                "crate::runtime::legacy_future",
                "crate::runtime::legacy_fiber",
                "crate::base::misc",
                "crate::base::legacy_threading",
                "crate::base::legacy_logging",
                "crate::base::legacy_cpuinfo",
                "crate::rpc::utils",
            ],
        )


if __name__ == "__main__":
    unittest.main()
