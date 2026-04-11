#!/usr/bin/env python3

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path


def extract_tagged_cpp_snippets(book_text: str):
    snippets = []
    lines = book_text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith("```cpp"):
            tags = set(line.split()[1:])
            if "srpc-compile-client" in tags:
                profile = "client"
            elif "srpc-compile-server" in tags:
                profile = "server"
            elif "srpc-compile-codegen" in tags:
                profile = "codegen"
            elif "srpc-compile" in tags:
                profile = "reliability"
            else:
                i += 1
                continue
            start = i + 1
            j = start
            while j < len(lines) and lines[j].strip() != "```":
                j += 1
            if j >= len(lines):
                raise RuntimeError(f"unterminated cpp fence starting near line {i + 1}")
            snippet = "\n".join(lines[start:j]).strip()
            snippets.append((i + 1, profile, snippet))
            i = j + 1
            continue
        i += 1
    return snippets


def build_compile_unit(profile: str, idx: int, snippet: str) -> str:
    if profile == "reliability":
        return f"""#include <time.h>
#include "src/rrr/rpc/reconnect_policy.hpp"
#include "src/rrr/rpc/circuit_breaker.hpp"
#include "src/rrr/rpc/heartbeat.hpp"

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
#include "src/rrr/rpc/client.hpp"

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
#include "src/rrr/rpc/server.hpp"

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
#include "src/rrr/rpc/client.hpp"

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


def compile_snippet(cxx: str, repo_root: Path, idx: int, line_no: int, profile: str, snippet: str):
    unit = build_compile_unit(profile, idx, snippet)

    with tempfile.NamedTemporaryFile("w", suffix=".cc", delete=False) as f:
        path = Path(f.name)
        f.write(unit)

    cmd = [
        cxx,
        "-std=c++23",
        "-fsyntax-only",
        "-I",
        str(repo_root),
        "-I",
        str(repo_root / "src/rrr"),
        "-I",
        str(repo_root / "third-party/rusty-cpp/include"),
        str(path),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    path.unlink(missing_ok=True)
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
    parser = argparse.ArgumentParser(description="Compile srpc-book tagged cpp snippets.")
    parser.add_argument("--book", required=True, help="Path to docs/srpc-book.md")
    parser.add_argument("--repo", required=True, help="Repository root path")
    parser.add_argument("--cxx", default="g++", help="C++ compiler executable")
    parser.add_argument("--min-snippets", type=int, default=1, help="Minimum required tagged snippets")
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

    snippets = extract_tagged_cpp_snippets(book_path.read_text(encoding="utf-8"))
    if len(snippets) < args.min_snippets:
        print(
            f"expected at least {args.min_snippets} tagged snippets, found {len(snippets)}",
            file=sys.stderr,
        )
        return 2

    failures = []
    for idx, (line_no, profile, snippet) in enumerate(snippets, start=1):
        ok, message = compile_snippet(args.cxx, repo_root, idx, line_no, profile, snippet)
        if not ok:
            failures.append(message)

    if failures:
        print("\n\n".join(failures), file=sys.stderr)
        return 1

    print(f"compiled {len(snippets)} tagged srpc-book snippets successfully")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
