#!/usr/bin/env python3

import argparse
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


def build_compile_unit(profile: str, idx: int, snippet: str) -> str:
    if profile == "reliability":
        return f"""#include <time.h>
import rrr;

using namespace rrr;

void snippet_{idx}() {{
{snippet}
}}

int main() {{
    snippet_{idx}();
    return 0;
}}
"""
    if profile == "client":
        return f"""#include <time.h>
#include <utility>
import rrr;

using namespace rrr;

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
        return f"""#include <time.h>
import rrr;

using namespace rrr;

inline int compute(int v) {{ return v; }}

class MyService : public Service {{
public:
    int __reg_to__(Server& svr, size_t svc_index) override {{
        (void)svr;
        (void)svc_index;
        return 0;
    }}

    void __dispatch__(i32 rpc_id, rusty::Box<Request> req, WeakServerConnection weak_sconn) override {{
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
        return f"""#include <time.h>
import rrr;

using namespace rrr;

struct UserInfo {{
    int id = 0;
    std::string name;
    double balance = 0.0;
}};

inline Marshal& operator>>(Marshal& m, UserInfo& user) {{
    m >> user.id >> user.name >> user.balance;
    return m;
}}

class MyServiceProxy {{
public:
    explicit MyServiceProxy(const Client* client) : client_(client) {{}}

    i32 get_user(i32 id, UserInfo* user) {{
        (void)id;
        (void)user;
        return 0;
    }}

    FutureResult async_get_user(i32 id) {{
        (void)id;
        return client_->request(0x1001, [](Marshal&) {{}});
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


def compile_snippet(
    cxx: str,
    repo_root: Path,
    idx: int,
    line_no: int,
    profile: str,
    snippet: str,
    timeout_sec: float,
):
    unit = build_compile_unit(profile, idx, snippet)
    cmd = [
        cxx,
        "-std=c++23",
        "-w",
        "-fsyntax-only",
        "-x",
        "c++",
        "-",
        "-I",
        str(repo_root),
        "-I",
        str(repo_root / "src/rrr"),
        "-I",
        str(repo_root / "third-party/rusty-cpp/include"),
        "-I",
        str(repo_root / "third-party/proxy/include"),
    ]
    try:
        proc = subprocess.run(
            cmd,
            input=unit,
            capture_output=True,
            text=True,
            timeout=timeout_sec,
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


def main():
    parser = argparse.ArgumentParser(description="Compile/lint srpc-book cpp snippets.")
    parser.add_argument("--book", required=True, help="Path to docs/srpc-book.md")
    parser.add_argument("--repo", required=True, help="Repository root path")
    parser.add_argument("--cxx", default="g++", help="C++ compiler executable")
    parser.add_argument("--min-snippets", type=int, default=1, help="Minimum required tagged snippets")
    parser.add_argument(
        "--snippet-timeout-sec",
        type=float,
        default=60.0,
        help="Per-snippet compile timeout in seconds",
    )
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    book_path = Path(args.book).resolve()

    if not book_path.exists():
        print(f"book not found: {book_path}", file=sys.stderr)
        return 2

    rusty_include = repo_root / "third-party/rusty-cpp/include/rusty"
    if not rusty_include.exists():
        print(
            f"missing Rusty C++ headers at {rusty_include}; run submodule update before this test",
            file=sys.stderr,
        )
        return 2

    proxy_include = repo_root / "third-party/proxy/include/proxy"
    if not proxy_include.exists():
        print(
            f"missing proxy headers at {proxy_include}; run submodule update before this test",
            file=sys.stderr,
        )
        return 2

    book_text = book_path.read_text(encoding="utf-8")
    try:
        snippets, violations, total_cpp_fences = extract_and_validate_cpp_snippets(book_text)
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if total_cpp_fences == 0:
        print("expected at least one cpp fence in srpc-book", file=sys.stderr)
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

    failures = []
    for idx, (line_no, profile, snippet) in enumerate(snippets, start=1):
        ok, message = compile_snippet(
            args.cxx,
            repo_root,
            idx,
            line_no,
            profile,
            snippet,
            args.snippet_timeout_sec,
        )
        if not ok:
            failures.append(message)

    if failures:
        print("\n\n".join(failures), file=sys.stderr)
        return 1

    print(f"compiled {len(snippets)} tagged srpc-book snippets successfully")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
