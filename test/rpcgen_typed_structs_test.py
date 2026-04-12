#!/usr/bin/env python3

import argparse
import subprocess
import tempfile
from pathlib import Path


RPC_FIXTURE = """namespace typed_structs_fixture

service Alpha {
    ping(i32 id | string msg);
    nop(|);
    unnamed(i32 | i32);
    multi(i32 left, string right | i64 sum, i8 ok);
    defer stream(i32 stream_id | i64 sequence);
    raw passthrough();
};

service Beta {
    ping(i32 other_id | string echoed);
};
"""


def run_rpcgen(repo_root: Path, rpc_path: Path) -> None:
    cmd = [str(repo_root / "bin/rpcgen"), "--cpp", str(rpc_path)]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=repo_root)
    if proc.returncode != 0:
        raise RuntimeError(
            "rpcgen failed\n"
            f"command: {' '.join(cmd)}\n"
            f"stdout:\n{proc.stdout}\n"
            f"stderr:\n{proc.stderr}"
        )


def section_between(text: str, start_marker: str, end_marker: str) -> str:
    start = text.find(start_marker)
    if start < 0:
        raise AssertionError(f"missing marker: {start_marker}")
    end = text.find(end_marker, start)
    if end < 0:
        raise AssertionError(f"missing marker: {end_marker}")
    return text[start:end]


def assert_contains(haystack: str, needle: str) -> None:
    if needle not in haystack:
        raise AssertionError(f"expected to find snippet:\n{needle}\n")


def verify_alpha_service_block(block: str) -> None:
    assert_contains(block, "struct pingRequest {\n        rrr::i32 id;\n    };")
    assert_contains(block, "struct pingResponse {\n        std::string msg;\n    };")
    assert_contains(
        block,
        "friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const pingRequest& o) {\n"
        "        m << o.id;\n"
        "        return m;\n"
        "    }",
    )
    assert_contains(
        block,
        "friend inline rrr::Marshal& operator >>(rrr::Marshal& m, pingResponse& o) {\n"
        "        m >> o.msg;\n"
        "        return m;\n"
        "    }",
    )
    assert_contains(block, "struct nopRequest {\n    };")
    assert_contains(block, "struct nopResponse {\n    };")
    assert_contains(block, "struct unnamedRequest {\n        rrr::i32 in_0;\n    };")
    assert_contains(block, "struct unnamedResponse {\n        rrr::i32 out_0;\n    };")
    assert_contains(
        block,
        "struct multiRequest {\n"
        "        rrr::i32 left;\n"
        "        std::string right;\n"
        "    };",
    )
    assert_contains(
        block,
        "struct multiResponse {\n"
        "        rrr::i64 sum;\n"
        "        rrr::i8 ok;\n"
        "    };",
    )
    assert_contains(
        block,
        "struct streamRequest {\n"
        "        rrr::i32 stream_id;\n"
        "    };",
    )
    assert_contains(
        block,
        "struct streamResponse {\n"
        "        rrr::i64 sequence;\n"
        "    };",
    )

    structs_pos = block.find("struct pingRequest")
    enum_pos = block.find("enum {")
    if structs_pos < 0 or enum_pos < 0 or structs_pos > enum_pos:
        raise AssertionError("typed structs should appear before service RPC enum")

    assert_contains(
        block,
        "virtual rusty::Result<pingResponse, rrr::i32> ping(const pingRequest& req) {\n"
        "        pingResponse __typed_resp__;\n"
        "        this->ping(req.id, &__typed_resp__.msg);\n"
        "        return rusty::Result<pingResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    assert_contains(
        block,
        "virtual rusty::Result<nopResponse, rrr::i32> nop(const nopRequest& req) {\n"
        "        nopResponse __typed_resp__;\n"
        "        this->nop();\n"
        "        (void)req;\n"
        "        return rusty::Result<nopResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    assert_contains(
        block,
        "virtual rusty::Result<unnamedResponse, rrr::i32> unnamed(const unnamedRequest& req) {\n"
        "        unnamedResponse __typed_resp__;\n"
        "        this->unnamed(req.in_0, &__typed_resp__.out_0);\n"
        "        return rusty::Result<unnamedResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    assert_contains(
        block,
        "virtual rusty::Result<multiResponse, rrr::i32> multi(const multiRequest& req) {\n"
        "        multiResponse __typed_resp__;\n"
        "        this->multi(req.left, req.right, &__typed_resp__.sum, &__typed_resp__.ok);\n"
        "        return rusty::Result<multiResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    assert_contains(
        block,
        "virtual rusty::Result<streamResponse, rrr::i32> stream(const streamRequest& req) {\n"
        "        (void)req;\n"
        "        return rusty::Result<streamResponse, rrr::i32>::Err(ENOTSUP);\n"
        "    }",
    )

    if "virtual rusty::Result<passthroughResponse, rrr::i32> passthrough(const passthroughRequest& req)" in block:
        raise AssertionError("raw handlers should not generate typed service signatures")


def verify_beta_service_block(block: str) -> None:
    assert_contains(block, "struct pingRequest {\n        rrr::i32 other_id;\n    };")
    assert_contains(block, "struct pingResponse {\n        std::string echoed;\n    };")
    assert_contains(
        block,
        "friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const pingRequest& o) {\n"
        "        m << o.other_id;\n"
        "        return m;\n"
        "    }",
    )
    assert_contains(
        block,
        "virtual rusty::Result<pingResponse, rrr::i32> ping(const pingRequest& req) {\n"
        "        pingResponse __typed_resp__;\n"
        "        this->ping(req.other_id, &__typed_resp__.echoed);\n"
        "        return rusty::Result<pingResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )

def verify_alpha_proxy_block(block: str) -> None:
    assert_contains(
        block,
        "rusty::Result<pingResponse, rrr::i32> ping(const pingRequest& req) {\n"
        "        auto __fu_result__ = this->async_ping(req.id);\n"
        "        if (__fu_result__.is_err()) {\n"
        "            return rusty::Result<pingResponse, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        auto __fu__ = __fu_result__.unwrap();\n"
        "        rrr::i32 __ret__ = __fu__->get_error_code();\n"
        "        if (__ret__ != 0) {\n"
        "            return rusty::Result<pingResponse, rrr::i32>::Err(__ret__);\n"
        "        }\n"
        "        pingResponse __typed_resp__;\n"
        "        __fu__->get_reply() >> __typed_resp__.msg;\n"
        "        return rusty::Result<pingResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    assert_contains(
        block,
        "rusty::Result<nopResponse, rrr::i32> nop(const nopRequest& req) {\n"
        "        auto __fu_result__ = this->async_nop();\n"
        "        if (__fu_result__.is_err()) {\n"
        "            return rusty::Result<nopResponse, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        auto __fu__ = __fu_result__.unwrap();\n"
        "        rrr::i32 __ret__ = __fu__->get_error_code();\n"
        "        if (__ret__ != 0) {\n"
        "            return rusty::Result<nopResponse, rrr::i32>::Err(__ret__);\n"
        "        }\n"
        "        nopResponse __typed_resp__;\n"
        "        (void)req;\n"
        "        return rusty::Result<nopResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    assert_contains(
        block,
        "rusty::Result<streamResponse, rrr::i32> stream(const streamRequest& req) {\n"
        "        auto __fu_result__ = this->async_stream(req.stream_id);\n"
        "        if (__fu_result__.is_err()) {\n"
        "            return rusty::Result<streamResponse, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        auto __fu__ = __fu_result__.unwrap();\n"
        "        rrr::i32 __ret__ = __fu__->get_error_code();\n"
        "        if (__ret__ != 0) {\n"
        "            return rusty::Result<streamResponse, rrr::i32>::Err(__ret__);\n"
        "        }\n"
        "        streamResponse __typed_resp__;\n"
        "        __fu__->get_reply() >> __typed_resp__.sequence;\n"
        "        return rusty::Result<streamResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    if "rusty::Result<passthroughResponse, rrr::i32> passthrough(const passthroughRequest& req)" in block:
        raise AssertionError("raw proxy handlers should not generate typed sync overloads")


def verify_beta_proxy_block(block: str) -> None:
    assert_contains(
        block,
        "rusty::Result<pingResponse, rrr::i32> ping(const pingRequest& req) {\n"
        "        auto __fu_result__ = this->async_ping(req.other_id);\n"
        "        if (__fu_result__.is_err()) {\n"
        "            return rusty::Result<pingResponse, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        auto __fu__ = __fu_result__.unwrap();\n"
        "        rrr::i32 __ret__ = __fu__->get_error_code();\n"
        "        if (__ret__ != 0) {\n"
        "            return rusty::Result<pingResponse, rrr::i32>::Err(__ret__);\n"
        "        }\n"
        "        pingResponse __typed_resp__;\n"
        "        __fu__->get_reply() >> __typed_resp__.echoed;\n"
        "        return rusty::Result<pingResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate rpcgen typed struct emission.")
    parser.add_argument("--repo", required=True, help="Repository root path")
    args = parser.parse_args()

    repo_root = Path(args.repo).resolve()
    if not repo_root.exists():
        raise RuntimeError(f"repo path does not exist: {repo_root}")

    with tempfile.TemporaryDirectory() as tmpdir:
        rpc_path = Path(tmpdir) / "typed_structs_fixture.rpc"
        rpc_path.write_text(RPC_FIXTURE, encoding="utf-8")

        run_rpcgen(repo_root, rpc_path)
        header_path = rpc_path.with_suffix(".h")
        if not header_path.exists():
            raise AssertionError(f"missing generated header: {header_path}")

        generated = header_path.read_text(encoding="utf-8")
        alpha_block = section_between(
            generated,
            "class AlphaService: public rrr::Service {",
            "class AlphaProxy {",
        )
        beta_block = section_between(
            generated,
            "class BetaService: public rrr::Service {",
            "class BetaProxy {",
        )
        alpha_proxy_block = section_between(
            generated,
            "class AlphaProxy {",
            "class BetaService: public rrr::Service {",
        )
        beta_proxy_block = section_between(
            generated,
            "class BetaProxy {",
            "} // namespace typed_structs_fixture",
        )

        verify_alpha_service_block(alpha_block)
        verify_beta_service_block(beta_block)
        verify_alpha_proxy_block(alpha_proxy_block)
        verify_beta_proxy_block(beta_proxy_block)

        if generated.count("struct pingRequest {") != 2:
            raise AssertionError("expected per-service pingRequest structs (one in each service)")

    print("rpcgen typed request/response struct emission verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
