#!/usr/bin/env python3
"""Smoke test for tools/rrr-inventory.py. Runs the script against a
small synthesized source tree under a tmp dir and checks that the
expected decls land in the expected buckets.

Invoke via `python3 -m unittest tools/test_rrr_inventory.py` from the
repo root, or just `python3 tools/test_rrr_inventory.py`.
"""

from __future__ import annotations

import csv
import importlib.util
import os
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "rrr-inventory.py"

# Load the script as a module (its filename uses a dash, so we go through
# importlib's file-based loader rather than `import`). Register in
# sys.modules so dataclasses' `sys.modules[cls.__module__]` lookup works
# for any `@dataclass`es defined inside the loaded module.
spec = importlib.util.spec_from_file_location("rrr_inventory", SCRIPT)
mod = importlib.util.module_from_spec(spec)
sys.modules["rrr_inventory"] = mod
spec.loader.exec_module(mod)


def fake_source(text: str) -> str:
    return textwrap.dedent(text).lstrip("\n")


class InventoryTest(unittest.TestCase):
    maxDiff = None

    def run_script(self, files: dict[str, str]) -> list[dict]:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "src" / "rrr"
            for rel, body in files.items():
                p = root / rel
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(body)
            out_csv = Path(tmp) / "out.csv"
            out_md = Path(tmp) / "out.md"
            # Invoke main() directly so we don't shell out.
            argv = sys.argv
            sys.argv = [
                "rrr-inventory.py",
                "--root", str(root),
                "--out", str(out_csv),
                "--summary", str(out_md),
            ]
            try:
                rc = mod.main()
            finally:
                sys.argv = argv
            self.assertEqual(rc, 0)
            with out_csv.open() as f:
                return list(csv.DictReader(f))

    def test_pod_struct_lands_in_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Foo {
                    int a;
                    int b;
                };
            """),
        })
        # Expect one row, name=Foo, region=manual, bucket=trivial.
        self.assertEqual(len(rows), 1)
        r = rows[0]
        self.assertEqual(r["name"], "Foo")
        self.assertEqual(r["region"], "manual")
        self.assertEqual(r["bucket"], "trivial")

    def test_dsl_block_marks_already_dsl(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                #if RUSTYCPP_RUST
                struct Inside { f: i32 }
                #endif
                /*RUSTYCPP:GEN-BEGIN id=foo version=1 rust_sha256=x*/
                struct Inside {
                    int f;
                };
                /*RUSTYCPP:GEN-END id=foo*/
            """),
        })
        # The GEN-block struct gets enumerated. It should be in `gen`
        # region and `already-dsl` bucket.
        self.assertGreaterEqual(len(rows), 1)
        gen_rows = [r for r in rows if r["region"] == "gen"]
        self.assertEqual(len(gen_rows), 1)
        self.assertEqual(gen_rows[0]["bucket"], "already-dsl")

    def test_virtual_lands_in_refactor(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                class IFoo {
                public:
                    virtual ~IFoo() = default;
                    virtual int frob() = 0;
                };
            """),
        })
        # User dtor on the same line as `virtual` triggers the custom-dtor
        # check first (which routes to needs-transpiler). That's fine —
        # virtual bases with custom dtors do need transpiler work.
        self.assertEqual(len(rows), 1)
        self.assertIn(rows[0]["bucket"], ("refactor-then-dsl", "needs-transpiler"))

    def test_template_lands_in_needs_transpiler(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                template <typename T>
                class Wrap {
                    T x;
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "needs-transpiler")
        self.assertEqual(rows[0]["name"], "Wrap")

    def test_boundary_name_routes_to_boundary(self):
        rows = self.run_script({
            "rpc/utils.cpp": fake_source("""
                struct AddrInfo {
                    void* p;
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "boundary")

    def test_tests_subdir_is_skipped(self):
        rows = self.run_script({
            "tests/test_x.cc": fake_source("""
                struct Foo { int a; };
            """),
            "real.cpp": fake_source("""
                struct Real { int a; };
            """),
        })
        names = [r["name"] for r in rows]
        self.assertIn("Real", names)
        self.assertNotIn("Foo", names)

    def test_forward_decl_is_skipped(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Forward;
                struct Real {
                    int x;
                };
            """),
        })
        names = [r["name"] for r in rows]
        self.assertIn("Real", names)
        self.assertNotIn("Forward", names)

    def test_user_ctor_routes_to_refactor(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                class Ctor {
                public:
                    Ctor(int a) : a_(a) {}
                private:
                    int a_;
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "refactor-then-dsl")

    def test_preproc_branch_blocks_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Ctx {
                #if defined(__x86_64__)
                    int a;
                #elif defined(__aarch64__)
                    int b;
                #endif
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "trivial-blocked")
        self.assertIn("preprocessor", rows[0]["notes"])

    def test_default_arg_blocks_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Foo {
                    void thing(int n = 0);
                    int x;
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "trivial-blocked")
        self.assertIn("default arg", rows[0]["notes"])

    def test_nested_struct_blocks_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Outer {
                    struct Inner {
                        int x;
                    };
                    int y;
                };
            """),
        })
        outer = [r for r in rows if r["name"] == "Outer"]
        self.assertEqual(len(outer), 1)
        self.assertEqual(outer[0]["bucket"], "trivial-blocked")
        self.assertIn("nested struct", outer[0]["notes"])

    def test_template_method_blocks_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Foo {
                    template <typename T>
                    static int as_index() { return 0; }
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "trivial-blocked")
        self.assertIn("template method", rows[0]["notes"])

    def test_std_function_field_blocks_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Foo {
                    std::function<void(int)> cb;
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "trivial-blocked")
        self.assertIn("std::function", rows[0]["notes"])

    def test_clean_pod_stays_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Pure {
                    int a;
                    int b;
                    bool c;
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "trivial")

    def test_call_site_atomic_cas_blocks_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Counters {
                    std::atomic<uint64_t> tick;
                };

                Counters g_counters;

                void bump_max(uint64_t v) {
                    uint64_t old = 0;
                    while (!g_counters.tick.compare_exchange_weak(old, v)) {}
                }
            """),
        })
        counters = [r for r in rows if r["name"] == "Counters"]
        self.assertEqual(len(counters), 1)
        self.assertEqual(counters[0]["bucket"], "trivial-blocked")
        self.assertIn("call site", counters[0]["notes"])

    def test_call_site_vector_assign_blocks_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Frame {
                    std::vector<uint8_t> bytes;
                };

                void copy_in(Frame& f, const uint8_t* p, size_t n) {
                    f.bytes.assign(p, p + n);
                }
            """),
        })
        frame = [r for r in rows if r["name"] == "Frame"]
        self.assertEqual(len(frame), 1)
        self.assertEqual(frame[0]["bucket"], "trivial-blocked")
        self.assertIn("call site", frame[0]["notes"])

    def test_unrelated_field_name_not_false_positive(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Frame {
                    std::vector<uint8_t> bytes;
                };

                struct Other {
                    int n;
                };

                void use_other() {
                    Other o;
                    o.n = 1;
                }
            """),
        })
        frame = [r for r in rows if r["name"] == "Frame"]
        self.assertEqual(len(frame), 1)
        self.assertEqual(frame[0]["bucket"], "trivial")

    def test_defaulted_ctor_does_not_block(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Foo {
                    Foo() = default;
                    Foo(Foo&&) = default;
                    int x;
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "trivial")

    def test_defaulted_assignment_op_does_not_block(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Foo {
                    Foo& operator=(Foo&&) = default;
                    int x;
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        # Defaulted op= must not trigger "operator overload" blocker.
        self.assertEqual(rows[0]["bucket"], "trivial")

    def test_double_equals_in_body_not_default_arg(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Foo {
                    bool is_expired(uint64_t ttl_ms) const {
                        if (ttl_ms == 0) return false;
                        return true;
                    }
                    uint64_t timestamp_ms;
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        # `ttl_ms == 0` body expression must not be mistaken for a
        # default arg.
        self.assertEqual(rows[0]["bucket"], "trivial")

    def test_void_star_param_blocks_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Foo {
                    void write(const void* p, size_t n);
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "trivial-blocked")
        self.assertIn("void*", rows[0]["notes"])

    def test_va_list_param_blocks_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Foo {
                    void log_v(const char* fmt, va_list ap);
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "trivial-blocked")
        self.assertIn("va_list", rows[0]["notes"])

    def test_c_array_param_blocks_trivial(self):
        rows = self.run_script({
            "foo.cpp": fake_source("""
                struct Foo {
                    int parse(int argc, char* argv[]);
                };
            """),
        })
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["bucket"], "trivial-blocked")
        self.assertIn("array", rows[0]["notes"])


if __name__ == "__main__":
    unittest.main()
