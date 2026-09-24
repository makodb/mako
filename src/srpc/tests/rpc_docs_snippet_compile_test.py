#!/usr/bin/env python3
"""Lint the ```cpp fences in docs/srpc-cpp-book.md, and (when a build tree is
available) syntax-check the compile-tagged snippets against the real srpc
module graph.

Two independent halves
----------------------
1. **Fence lint** (`extract_and_validate_cpp_snippets`) — pure text
   processing.  Every ```cpp fence in the book must carry exactly one tag
   from ALLOWED_CPP_TAGS, and compile tags must not be mixed with each
   other or with `srpc-no-compile`.  This half needs nothing but the book,
   so `--lint-only` runs it on a bare checkout with no submodules, no
   compiler and no build directory.

2. **Snippet compile** — wraps each compile-tagged snippet in a profile
   preamble and runs `clang++ -fsyntax-only` over it.  This needs the whole
   toolchain: Clang 22 with libc++, the rusty-cpp submodule headers, and a
   configured build tree that has already built `srpc` (so the BMIs and the
   battery module map exist).  When any of that is missing the half prints
   a skip line and the test still exits 0 — a docs lint must not fail
   because the C++ build was never configured.

Why the compile half looks the way it does
------------------------------------------
* Module discovery reads `goal0-battery-modules.modmap`, the Clang response
  file `scripts/emit_module_map.py` writes and CMakeLists.txt attaches to
  every Goal-0 battery target (`srpc_battery_modmap`).  It holds one
  `-fmodule-file="name=/abs/path.bmi"` line per module srpc was built
  against, `std` and the rusty ports included.  That is the same mechanism
  the battery's own pure-consumer TUs use, and it is the only one this repo
  has: `CMAKE_EXPORT_COMPILE_COMMANDS` is set nowhere, so there is no
  `compile_commands.json` to scrape.
* `-std=gnu++23`, not `-std=c++23`.  CMakeLists.txt sets
  `CMAKE_CXX_EXTENSIONS ON`, and the rusty umbrella is compiled
  `-std=gnu++23` precisely so consumers can `import rusty;` without
  tripping Clang's "GNU extensions was disabled in precompiled file"
  PCM-config check.  A consumer built `-std=c++23` fails that check.
* `-stdlib=libc++`, matching the project-wide `add_compile_options`.
* `-march=native`, matching the target features recorded in the production
  BMIs. Clang rejects consumers with a different feature set.
* The snippet units `#include "srpc.hpp"` — the umbrella lives at the
  repository root, and `-I <repo root>` is what the battery targets pass.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


COMPILE_TAG_TO_PROFILE = {
    "srpc-compile": "reliability",
    "srpc-compile-client": "client",
    "srpc-compile-server": "server",
    "srpc-compile-codegen": "codegen",
}
NON_COMPILE_TAG = "srpc-no-compile"
ALLOWED_CPP_TAGS = set(COMPILE_TAG_TO_PROFILE) | {NON_COMPILE_TAG}
COMMON_SNIPPET_PREAMBLE = """#include <errno.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <rusty/async.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/option.hpp>
#include <rusty/result.hpp>
"""

# `scripts/emit_module_map.py --output ${CMAKE_CURRENT_BINARY_DIR}/...`;
# srpc's CMakeLists.txt is the top-level list file, so this normally sits
# directly in the build directory (see the `srpc_battery_modmap` target).
BATTERY_MODMAP_NAME = "goal0-battery-modules.modmap"

# Mirrors the SRPC_CLANG22_HINTS search in CMakeLists.txt.  SRPC hard-requires
# Clang 22 (`message(FATAL_ERROR "SRPC requires Clang 22 or newer")`), so a
# stray `g++` on PATH is never an acceptable default here.
CXX_CANDIDATE_NAMES = ("clang++-22", "clang++")
CLANG22_HINT_DIRS = (
    "/home/linuxbrew/.linuxbrew/opt/llvm@22/bin",
    "$HOMEBREW_PREFIX/opt/llvm@22/bin",
    "/opt/homebrew/opt/llvm@22/bin",
    "/usr/local/opt/llvm@22/bin",
    "$HOME/.linuxbrew/opt/llvm@22/bin",
)


def find_battery_modmap(
    repo_root: Path, requested_build_dir: Path | None
) -> tuple[Path, Path] | None:
    """Locate the module map CMake hands the Goal-0 battery targets.

    Returns (modmap path, build directory) or None.  The map's
    `-fmodule-file=` paths are already absolute (emit_module_map.py resolves
    every relative reference against --build-dir), so the build directory is
    returned only to use as the compiler's working directory.
    """
    candidate_build_dirs: list[Path] = []
    if requested_build_dir is not None:
        candidate_build_dirs.append(requested_build_dir)
    else:
        candidate_build_dirs.extend(
            [
                repo_root / "build",
                repo_root / "cmake-build-debug",
                repo_root / "cmake-build-release",
            ]
        )

    for build_dir in candidate_build_dirs:
        if not build_dir.is_dir():
            continue
        direct = build_dir / BATTERY_MODMAP_NAME
        if direct.is_file():
            return direct, build_dir
        # srpc added as a superproject subdirectory puts the map under the
        # matching binary subdirectory instead.
        nested = sorted(build_dir.rglob(BATTERY_MODMAP_NAME))
        if nested:
            return nested[0], build_dir

    return None


def resolve_cxx(requested: str | None) -> str | None:
    """Pick the C++ driver: --cxx if given, else Clang 22 the way CMake finds it."""
    if requested:
        return shutil.which(requested) or (
            requested if Path(requested).is_file() else None
        )
    search_dirs = [
        Path(os.path.expandvars(os.path.expanduser(d))) for d in CLANG22_HINT_DIRS
    ]
    for name in CXX_CANDIDATE_NAMES:
        for directory in search_dirs:
            candidate = directory / name
            if candidate.is_file():
                return str(candidate)
        found = shutil.which(name)
        if found:
            return found
    return None


def extract_and_validate_cpp_snippets(book_text: str):
    snippets = []
    violations = []
    compile_tag_set = set(COMPILE_TAG_TO_PROFILE)
    lines = book_text.splitlines()
    i = 0
    total_cpp_fences = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith("```cpp"):
            total_cpp_fences += 1
            tags = set(line.split()[1:])
            compile_tags = sorted(tags & compile_tag_set)
            unknown_tags = sorted(tags - ALLOWED_CPP_TAGS)

            if not tags:
                violations.append(
                    f"line {i + 1}: missing cpp fence tag; "
                    f"use one of {{{', '.join(sorted(ALLOWED_CPP_TAGS))}}}"
                )
            if unknown_tags:
                violations.append(
                    f"line {i + 1}: unknown cpp fence tags: {', '.join(unknown_tags)}"
                )
            if len(compile_tags) > 1:
                violations.append(
                    f"line {i + 1}: multiple compile tags are not allowed: "
                    f"{', '.join(compile_tags)}"
                )
            if NON_COMPILE_TAG in tags and compile_tags:
                violations.append(
                    f"line {i + 1}: cannot mix {NON_COMPILE_TAG} with compile tags"
                )

            start = i + 1
            j = start
            while j < len(lines) and lines[j].strip() != "```":
                j += 1
            if j >= len(lines):
                raise RuntimeError(f"unterminated cpp fence starting near line {i + 1}")

            if len(compile_tags) == 1 and NON_COMPILE_TAG not in tags and not unknown_tags:
                snippet = "\n".join(lines[start:j]).strip()
                profile = COMPILE_TAG_TO_PROFILE[compile_tags[0]]
                snippets.append((i + 1, profile, snippet))
            i = j + 1
            continue
        i += 1
    return snippets, violations, total_cpp_fences


# srpc.hpp deliberately comments these six imports out ("trimmed from consumer
# umbrella: nothing outside srpc names it"), but every one is a real module
# declared in rust-modules.toml, so snippets that touch the reliability and
# metrics surface import them directly — exactly as tests/test_load_balancer.cc
# does for srpc.connection_metrics and srpc.load_balancer.
TRIMMED_MODULE_IMPORTS = """// Trimmed from the consumer umbrella — srpc.hpp comments these six out
// because nothing outside srpc names them. Snippets exercising the
// reliability/metrics APIs import the modules directly.
import srpc.circuit_breaker;
import srpc.connection_metrics;
import srpc.heartbeat;
import srpc.load_balancer;
import srpc.reconnect_policy;
import srpc.request_options;
"""

# The umbrella is `srpc.hpp` at the repository root; `-I <repo root>` is on the
# command line, matching how tests/*.cc reach it as "../srpc.hpp".
SNIPPET_UNIT_HEADER = f"""{COMMON_SNIPPET_PREAMBLE}
#include <time.h>
#include "srpc.hpp"

{TRIMMED_MODULE_IMPORTS}
using namespace srpc;
"""


def build_compile_unit(profile: str, idx: int, snippet: str) -> str:
    if profile == "reliability":
        return f"""{SNIPPET_UNIT_HEADER}
void snippet_{idx}() {{
{snippet}
}}

int main() {{
    snippet_{idx}();
    return 0;
}}
"""
    if profile == "client":
        return f"""{SNIPPET_UNIT_HEADER}
struct ClientHarness {{
    rusty::Arc<Client> arc;

    const Client* operator->() const {{ return arc.get(); }}
    const ConnectionMetrics& metrics() const {{ return arc->metrics(); }}

    template <typename F>
    void add_on_connected(F&& cb) const {{ arc->add_on_connected(std::forward<F>(cb)); }}
    template <typename F>
    void add_on_disconnected(F&& cb) const {{ arc->add_on_disconnected(std::forward<F>(cb)); }}
    template <typename F>
    void add_on_error(F&& cb) const {{ arc->add_on_error(std::forward<F>(cb)); }}
    template <typename F>
    void add_on_reconnecting(F&& cb) const {{ arc->add_on_reconnecting(std::forward<F>(cb)); }}
    template <typename F>
    void add_on_reconnected(F&& cb) const {{ arc->add_on_reconnected(std::forward<F>(cb)); }}
}};

void snippet_{idx}() {{
    auto __poll_thread = PollThread::create();
    ClientHarness client{{Client::create(__poll_thread.clone())}};
    constexpr i32 RPC_METHOD_ID = 0x1001;
    int arg1 = 7;
    int arg2 = 11;

{snippet}

    (void)client;
    (void)arg1;
    (void)arg2;
}}

int main() {{
    snippet_{idx}();
    return 0;
}}
"""
    if profile == "server":
        return f"""{SNIPPET_UNIT_HEADER}
inline int compute(int v) {{ return v; }}

class MyService : public Service {{
public:
    int __reg_to__(Server& svr, size_t svc_index) override {{
        (void)svr;
        (void)svc_index;
        return 0;
    }}

    void __dispatch__(i32 rpc_id, rusty::Box<Request> req, WeakServerConnection weak_sconn) const override {{
        (void)rpc_id;
        (void)req;
        (void)weak_sconn;
    }}
}};

void snippet_{idx}() {{
{snippet}
}}

int main() {{
    snippet_{idx}();
    return 0;
}}
"""
    if profile == "codegen":
        # No `Marshal`, no `operator>>`: SRPC has neither (docs/srpc-cpp-book.md
        # serialization chapter). Generated code calls the two free dispatchers
        # `srpc::Serialize_::serialize` / `srpc::Deserialize_::deserialize`
        # over a BinaryWriteArchive / BinaryReadArchive; canonical source is
        # misc/serializable.rs (module srpc.serializable, which srpc.hpp
        # imports).
        return f"""{SNIPPET_UNIT_HEADER}
struct UserInfo {{
    int id = 0;
    std::string name;
    double balance = 0.0;
}};

inline void serialize_user(const UserInfo& user, BinaryWriteArchive& ar) {{
    Serialize_::serialize(user.id, ar);
    Serialize_::serialize(user.name, ar);
    Serialize_::serialize(user.balance, ar);
}}

inline void deserialize_user(UserInfo& user, BinaryReadArchive& ar) {{
    Deserialize_::deserialize(user.id, ar);
    Deserialize_::deserialize(user.name, ar);
    Deserialize_::deserialize(user.balance, ar);
}}

class MyServiceProxy {{
public:
    explicit MyServiceProxy(const Client* client) : client_(client) {{}}

    i32 get_user(i32 id, UserInfo* user) {{
        (void)id;
        (void)user;
        return 0;
    }}

    // `request` takes three arguments — (rpc_id, attr, write_fn). The
    // two-argument overload was deliberately collapsed away; see
    // rpc/client.rs `fn request(&self, rpc_id: i32, attr: &FutureAttr, write_fn: F)`.
    FutureResult async_get_user(i32 id) {{
        (void)id;
        return client_->request(0x1001, FutureAttr(), [](BinaryWriteArchive&) {{}});
    }}

private:
    const Client* client_;
}};

void snippet_{idx}() {{
    auto poll_thread = PollThread::create();
    auto client = Client::create(poll_thread.clone());

{snippet}
}}

int main() {{
    snippet_{idx}();
    return 0;
}}
"""
    raise ValueError(f"unknown snippet compile profile: {profile}")


def snippet_compile_command(cxx: str, repo_root: Path, modmap: Path | None) -> list[str]:
    cmd = [
        cxx,
        # gnu++23, NOT c++23: CMAKE_CXX_EXTENSIONS is ON project-wide and the
        # rusty umbrella BMI is built -std=gnu++23, so a consumer without GNU
        # extensions trips Clang's "GNU extensions was disabled in precompiled
        # file" PCM-config check.
        "-std=gnu++23",
        "-stdlib=libc++",
        "-march=native",
        "-w",
        "-fsyntax-only",
    ]
    if modmap is not None:
        # Response file of -fmodule-file="name=/abs/path.bmi" lines, naming
        # every BMI srpc was built against (std and the rusty ports included).
        cmd.append(f"@{modmap}")
    cmd.extend(
        [
            "-I",
            str(repo_root),
            "-I",
            str(repo_root / "third-party/rusty-cpp/include"),
            "-x",
            "c++",
            "-",
        ]
    )
    return cmd


def compile_snippet(
    cxx: str,
    repo_root: Path,
    idx: int,
    line_no: int,
    profile: str,
    snippet: str,
    timeout_sec: float,
    modmap: Path | None,
    build_dir: Path | None,
):
    unit = build_compile_unit(profile, idx, snippet)
    cmd = snippet_compile_command(cxx, repo_root, modmap)
    run_cwd = str(build_dir) if build_dir is not None else None
    try:
        proc = subprocess.run(
            cmd,
            input=unit,
            capture_output=True,
            text=True,
            timeout=timeout_sec,
            cwd=run_cwd,
        )
    except subprocess.TimeoutExpired as exc:
        return (
            False,
            f"snippet tagged at line {line_no} timed out during compile\n"
            f"profile: {profile}\n"
            f"timeout_sec: {timeout_sec}\n"
            f"command: {' '.join(cmd)}\n"
            f"{exc.stdout or ''}{exc.stderr or ''}",
        )
    if proc.returncode != 0:
        return (
            False,
            f"snippet tagged at line {line_no} failed to compile\n"
            f"profile: {profile}\n"
            f"command: {' '.join(cmd)}\n"
            f"{proc.stdout}{proc.stderr}",
        )
    return True, ""


def default_repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description="Compile/lint SRPC C++ companion snippets.")
    parser.add_argument(
        "--book",
        default=None,
        help="Path to docs/srpc-cpp-book.md (default: <repo>/docs/srpc-cpp-book.md)",
    )
    parser.add_argument(
        "--repo",
        default=None,
        help="Repository root path (default: the directory containing tests/)",
    )
    parser.add_argument(
        "--cxx",
        default=None,
        help="C++ compiler executable (default: clang++-22, then clang++; "
             "SRPC requires Clang 22 with libc++)",
    )
    parser.add_argument("--build-dir", default=None, help="CMake build directory for module flags")
    parser.add_argument("--min-snippets", type=int, default=1, help="Minimum required tagged snippets")
    parser.add_argument(
        "--lint-only",
        action="store_true",
        help="Validate cpp fence tags and stop. Needs no compiler, no submodules "
             "and no build tree.",
    )
    parser.add_argument(
        "--snippet-timeout-sec",
        type=float,
        default=60.0,
        help="Per-snippet compile timeout in seconds",
    )
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve() if args.repo else default_repo_root()
    build_dir = Path(args.build_dir).resolve() if args.build_dir else None
    book_path = (
        Path(args.book).resolve() if args.book else repo_root / "docs" / "srpc-cpp-book.md"
    )

    if not book_path.exists():
        print(f"book not found: {book_path}", file=sys.stderr)
        return 2

    book_text = book_path.read_text(encoding="utf-8")
    try:
        snippets, violations, total_cpp_fences = extract_and_validate_cpp_snippets(book_text)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if total_cpp_fences == 0:
        print("expected at least one cpp fence in srpc-cpp-book", file=sys.stderr)
        return 2

    if violations:
        print("cpp fence tagging violations:", file=sys.stderr)
        print("\n".join(f"- {violation}" for violation in violations), file=sys.stderr)
        return 2

    if len(snippets) < args.min_snippets:
        print(
            f"expected at least {args.min_snippets} tagged snippets, found {len(snippets)}",
            file=sys.stderr,
        )
        return 2

    print(
        f"fence lint OK: {total_cpp_fences} cpp fences, all tagged; "
        f"{len(snippets)} carry a compile tag"
    )

    if args.lint_only:
        print("--lint-only: skipping the snippet compile half")
        return 0

    # --- compile half: every precondition is a skip, never a failure ---
    rusty_include = repo_root / "third-party/rusty-cpp/include/rusty"
    if not rusty_include.is_dir():
        print(
            f"SKIP compile half: no Rusty C++ headers at {rusty_include} "
            "(run `git submodule update --init --recursive`)"
        )
        return 0

    cxx = resolve_cxx(args.cxx)
    if cxx is None:
        requested = args.cxx or "/".join(CXX_CANDIDATE_NAMES)
        print(
            f"SKIP compile half: no usable C++ driver ({requested} not found); "
            "SRPC requires Clang 22 with libc++"
        )
        return 0

    located = find_battery_modmap(repo_root, build_dir)
    if located is None:
        searched = str(build_dir) if build_dir else "build/, cmake-build-debug/, cmake-build-release/"
        print(
            f"SKIP compile half: no {BATTERY_MODMAP_NAME} under {searched}. "
            "Configure CMake and build the `srpc_battery_modmap` target "
            "(it depends on `srpc`, so the BMIs exist) first."
        )
        return 0
    modmap, modmap_build_dir = located

    failures = []
    for idx, (line_no, profile, snippet) in enumerate(snippets, start=1):
        ok, message = compile_snippet(
            cxx,
            repo_root,
            idx,
            line_no,
            profile,
            snippet,
            args.snippet_timeout_sec,
            modmap,
            modmap_build_dir,
        )
        if not ok:
            failures.append(message)

    if failures:
        print("\n\n".join(failures), file=sys.stderr)
        return 1

    print(
        f"compiled {len(snippets)} tagged C++ companion snippets successfully "
        f"({cxx}, module map {modmap})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
