import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "runner", Path(__file__).with_name("run_mako_engine_native_health_proof.py")
)
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class NativeCommandTest(unittest.TestCase):
    def test_preserves_flags_and_redirects_only_output_dependencies(self):
        args = ["clang++", "-DKEEP=1", "-Iinclude", "-O3", "-MD", "-MP",
                "-MF", "old.d", "-MT", "old.o", "-MQ", "old.q",
                "@old.o.modmap", "-o", "old.o", "-c", "old.cc"]
        original, removed, command = runner.compile_command(
            {"arguments": args}, Path("proof.cc"), Path("proof.o")
        )
        self.assertEqual(original, args)
        self.assertEqual(command, ["clang++", "-DKEEP=1", "-Iinclude", "-O3",
                                  "-DMAKO_NATIVE_RUNTIME_HEALTH_PROOF=1",
                                  "-c", "proof.cc", "-o", "proof.o"])
        self.assertIn("@old.o.modmap", removed)

    def test_shell_tokenization_preserves_include_space(self):
        _, _, command = runner.compile_command(
            {"command": "clang++ '-Iinclude space' -o old.o -c old.cc"},
            Path("proof.cc"), Path("proof.o")
        )
        self.assertEqual(command[1], "-Iinclude space")

    def test_unknown_response_file_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unreviewed response"):
            runner.compile_command({"arguments": ["clang++", "@flags.rsp"]},
                                   Path("proof.cc"), Path("proof.o"))

    def test_missing_option_argument_is_rejected(self):
        for flag in ["-o", "-c", "-MF", "-MT", "-MQ"]:
            with self.subTest(flag=flag), self.assertRaisesRegex(ValueError, "missing argument"):
                runner.compile_command({"arguments": ["clang++", flag]},
                                       Path("proof.cc"), Path("proof.o"))


if __name__ == "__main__":
    unittest.main()
