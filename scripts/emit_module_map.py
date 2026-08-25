#!/usr/bin/env python3
"""Emit a Clang response file naming every C++ module BMI that a Goal-0
runtime-battery test translation unit may import.

Why this exists
---------------
The battery's test programs are the first *pure consumers* of srpc's C++23
named modules: unlike `srpc` itself they provide no module of their own.  With
CMake 4.4's `CXX_MODULE_STD` support that turns out to be a build-graph dead
end.  `srpc` compiles with the project's BENCH_CXXFLAGS (`-w -Wreturn-type -MD
-MP -DRUSTYCPP_DISABLE_ARC_LOG -DREUSE_FIBER -O2 -g -fno-omit-frame-pointer
-march=native`) while the vendored rusty-cpp port libraries compile with `-O3
-DNDEBUG -march=native`, so CMake synthesises two `std` variants
(`@cmake_cxx_std@synth_0` and `@synth_1`) and re-synthesises the port BMIs for
`srpc` (`rusty@synth_0`, `vec_port@synth_0`, ...).  The shared `-march=native`
is not what splits them -- BENCH_CXXFLAGS carries it precisely so that srpc's
own module units can import the ports' `-march=native` BMIs at all -- the split
comes from the remaining settings-signature difference (`-O3 -DNDEBUG` vs
srpc's `-O2 -g` plus its warning, dependency and define flags).  A
module-providing target only ever sees one consistent set, but a consumer
executable's link closure contains BOTH `srpc` (whose module reference
map resolves `std` to synth_0) and the plain `rusty`/`*_port` targets (which
resolve `std` to synth_1).  CMake's dyndep collation then fails with

    CMake Error: Disagreement of the location of the 'std' module.
    Location A: 'CMakeFiles/@cmake_cxx_std@synth_1.dir/....bmi' via by-name;
    Location B: 'CMakeFiles/@cmake_cxx_std@synth_0.dir/....bmi' via by-name.

The fix must not touch the production module graph: the flag divergence is
deliberate (the ports' flags are rusty-cpp's, in a different repository, and
`srpc`'s flags produce the gated ABI artifact).  So the battery targets opt out
of CMake's scanner
(`CXX_SCAN_FOR_MODULES OFF`) and are handed the *exact* module map that `srpc`
itself was built against.  That map is authoritative rather than
reconstructed: it is read straight out of `CMakeFiles/srpc.dir/CXXModules.json`,
which CMake writes when it collates `srpc`'s own dyndep.

`modules` holds the BMIs the target provides (absolute paths); `references`
holds every module visible to it, including the transitively imported
`rusty`/port BMIs and the synthesised `std`, as build-directory-relative
paths.  Provided BMIs win over references for the same name.
"""

import argparse
import json
import pathlib
import sys


def collect(modules_json: list[pathlib.Path],
            build_dir: pathlib.Path) -> dict[str, pathlib.Path]:
    entries: dict[str, pathlib.Path] = {}
    for source in modules_json:
        data = json.loads(source.read_text(encoding="utf-8"))
        for name, info in sorted(data.get("references", {}).items()):
            entries.setdefault(name, pathlib.Path(info["path"]))
        # Provided BMIs are authoritative and override any reference entry.
        for name, info in sorted(data.get("modules", {}).items()):
            entries[name] = pathlib.Path(info["bmi"])
    resolved: dict[str, pathlib.Path] = {}
    for name, path in entries.items():
        resolved[name] = path if path.is_absolute() else build_dir / path
    return resolved


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--modules-json",
        action="append",
        required=True,
        type=pathlib.Path,
        help="CMake CXXModules.json to read (repeatable)",
    )
    parser.add_argument(
        "--build-dir",
        required=True,
        type=pathlib.Path,
        help="build directory the relative reference paths are rooted at",
    )
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)

    build_dir = args.build_dir.resolve()
    for source in args.modules_json:
        if not source.is_file():
            parser.error(f"missing module description: {source}")
    entries = collect(args.modules_json, build_dir)
    if not entries:
        parser.error("no module entries were found; the module map would be "
                     "silently empty and every import would fail")

    lines = [
        f'-fmodule-file="{name}={entries[name]}"' for name in sorted(entries)
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
