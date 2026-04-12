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


def run_rpcgen(repo_root: Path, rpc_path: Path, cpp_mode: str = "typed") -> None:
    cmd = [str(repo_root / "bin/rpcgen"), "--cpp", "--cpp-mode", cpp_mode, str(rpc_path)]
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
    assert_contains(block, "struct RpcPingRequest {\n        rrr::i32 id;\n    };")
    assert_contains(block, "struct RpcPingResponse {\n        std::string msg;\n    };")
    assert_contains(
        block,
        "friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPingRequest& o) {\n"
        "        m << o.id;\n"
        "        return m;\n"
        "    }",
    )
    assert_contains(
        block,
        "friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPingResponse& o) {\n"
        "        m >> o.msg;\n"
        "        return m;\n"
        "    }",
    )
    assert_contains(block, "struct RpcNopRequest {\n    };")
    assert_contains(block, "struct RpcNopResponse {\n    };")
    assert_contains(block, "struct RpcUnnamedRequest {\n        rrr::i32 in_0;\n    };")
    assert_contains(block, "struct RpcUnnamedResponse {\n        rrr::i32 out_0;\n    };")
    assert_contains(
        block,
        "struct RpcMultiRequest {\n"
        "        rrr::i32 left;\n"
        "        std::string right;\n"
        "    };",
    )
    assert_contains(
        block,
        "struct RpcMultiResponse {\n"
        "        rrr::i64 sum;\n"
        "        rrr::i8 ok;\n"
        "    };",
    )
    assert_contains(
        block,
        "struct RpcStreamRequest {\n"
        "        rrr::i32 stream_id;\n"
        "    };",
    )
    assert_contains(
        block,
        "struct RpcStreamResponse {\n"
        "        rrr::i64 sequence;\n"
        "    };",
    )

    structs_pos = block.find("struct RpcPingRequest")
    enum_pos = block.find("enum {")
    if structs_pos < 0 or enum_pos < 0 or structs_pos > enum_pos:
        raise AssertionError("typed structs should appear before service RPC enum")

    assert_contains(
        block,
        "virtual rusty::Result<RpcPingResponse, rrr::i32> ping(const RpcPingRequest& req) {\n"
        "        RpcPingResponse __typed_resp__;\n"
        "        this->ping(req.id, &__typed_resp__.msg);\n"
        "        return rusty::Result<RpcPingResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    assert_contains(
        block,
        "virtual rusty::Result<RpcNopResponse, rrr::i32> nop(const RpcNopRequest& req) {\n"
        "        RpcNopResponse __typed_resp__;\n"
        "        this->nop();\n"
        "        (void)req;\n"
        "        return rusty::Result<RpcNopResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    assert_contains(
        block,
        "virtual rusty::Result<RpcUnnamedResponse, rrr::i32> unnamed(const RpcUnnamedRequest& req) {\n"
        "        RpcUnnamedResponse __typed_resp__;\n"
        "        this->unnamed(req.in_0, &__typed_resp__.out_0);\n"
        "        return rusty::Result<RpcUnnamedResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    assert_contains(
        block,
        "virtual rusty::Result<RpcMultiResponse, rrr::i32> multi(const RpcMultiRequest& req) {\n"
        "        RpcMultiResponse __typed_resp__;\n"
        "        this->multi(req.left, req.right, &__typed_resp__.sum, &__typed_resp__.ok);\n"
        "        return rusty::Result<RpcMultiResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    assert_contains(
        block,
        "virtual rusty::Result<RpcStreamResponse, rrr::i32> stream(const RpcStreamRequest& req) {\n"
        "        (void)req;\n"
        "        return rusty::Result<RpcStreamResponse, rrr::i32>::Err(ENOTSUP);\n"
        "    }",
    )
    assert_contains(
        block,
        "void __ping__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        RpcPingRequest __typed_req__;\n"
        "        req->m >> __typed_req__.id;\n"
        "        auto __typed_result__ = this->ping(__typed_req__);\n"
        "        auto sconn_opt = weak_sconn.upgrade();\n"
        "        if (sconn_opt.is_some()) {\n"
        "            auto sconn = sconn_opt.unwrap();\n"
        "            if (__typed_result__.is_err()) {\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());\n"
        "            } else {\n"
        "                auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {\n"
        "                    m << __typed_resp__.msg;\n"
        "                });\n"
        "            }\n"
        "        }\n"
        "        // req automatically cleaned up by rusty::Box\n"
        "    }",
    )
    assert_contains(
        block,
        "void __nop__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        RpcNopRequest __typed_req__;\n"
        "        auto __typed_result__ = this->nop(__typed_req__);\n"
        "        auto sconn_opt = weak_sconn.upgrade();\n"
        "        if (sconn_opt.is_some()) {\n"
        "            auto sconn = sconn_opt.unwrap();\n"
        "            if (__typed_result__.is_err()) {\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());\n"
        "            } else {\n"
        "                auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                (void)__typed_resp__;\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req);\n"
        "            }\n"
        "        }\n"
        "        // req automatically cleaned up by rusty::Box\n"
        "    }",
    )
    assert_contains(
        block,
        "void __unnamed__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        RpcUnnamedRequest __typed_req__;\n"
        "        req->m >> __typed_req__.in_0;\n"
        "        auto __typed_result__ = this->unnamed(__typed_req__);\n"
        "        auto sconn_opt = weak_sconn.upgrade();\n"
        "        if (sconn_opt.is_some()) {\n"
        "            auto sconn = sconn_opt.unwrap();\n"
        "            if (__typed_result__.is_err()) {\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());\n"
        "            } else {\n"
        "                auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {\n"
        "                    m << __typed_resp__.out_0;\n"
        "                });\n"
        "            }\n"
        "        }\n"
        "        // req automatically cleaned up by rusty::Box\n"
        "    }",
    )
    assert_contains(
        block,
        "void __multi__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        RpcMultiRequest __typed_req__;\n"
        "        req->m >> __typed_req__.left;\n"
        "        req->m >> __typed_req__.right;\n"
        "        auto __typed_result__ = this->multi(__typed_req__);\n"
        "        auto sconn_opt = weak_sconn.upgrade();\n"
        "        if (sconn_opt.is_some()) {\n"
        "            auto sconn = sconn_opt.unwrap();\n"
        "            if (__typed_result__.is_err()) {\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());\n"
        "            } else {\n"
        "                auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {\n"
        "                    m << __typed_resp__.sum;\n"
        "                    m << __typed_resp__.ok;\n"
        "                });\n"
        "            }\n"
        "        }\n"
        "        // req automatically cleaned up by rusty::Box\n"
        "    }",
    )
    assert_contains(
        block,
        "void __stream__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        RpcStreamRequest __typed_req__;\n"
        "        req->m >> __typed_req__.stream_id;\n"
        "        auto __typed_result__ = this->stream(__typed_req__);\n"
        "        if (__typed_result__.is_ok()) {\n"
        "            auto sconn_opt = weak_sconn.upgrade();\n"
        "            if (sconn_opt.is_some()) {\n"
        "                auto sconn = sconn_opt.unwrap();\n"
        "                auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {\n"
        "                    m << __typed_resp__.sequence;\n"
        "                });\n"
        "            }\n"
        "            return;\n"
        "        }\n"
        "        rrr::i32 __typed_err__ = __typed_result__.unwrap_err();\n"
        "        if (__typed_err__ != ENOTSUP) {\n"
        "            auto sconn_opt = weak_sconn.upgrade();\n"
        "            if (sconn_opt.is_some()) {\n"
        "                auto sconn = sconn_opt.unwrap();\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_err__);\n"
        "            }\n"
        "            return;\n"
        "        }\n"
        "        // Typed defer path is currently optional; ENOTSUP falls back to legacy deferred handler.\n"
        "        auto in_0 = std::make_shared<rrr::i32>(__typed_req__.stream_id);\n"
        "        auto out_0 = std::make_shared<rrr::i64>();\n"
        "        auto __marshal_reply__ = [=](rrr::Marshal& m) {\n"
        "            m << *out_0;\n"
        "        };\n"
        "        auto __cleanup__ = [=] {\n"
        "            (void)in_0;\n"
        "            (void)out_0;\n"
        "        };\n"
        "        rrr::DeferredReply __defer__(std::move(req), weak_sconn, __marshal_reply__, __cleanup__);\n"
        "        this->stream(*in_0, out_0.get(), std::move(__defer__));\n"
        "    }",
    )
    if "new rrr::i32" in block or "new rrr::i64" in block:
        raise AssertionError("deferred legacy fallback should not allocate request/response with new")
    if "delete in_0" in block or "delete out_0" in block:
        raise AssertionError("deferred legacy fallback should not emit manual delete cleanup")

    if "virtual rusty::Result<RpcPassthroughResponse, rrr::i32> passthrough(const RpcPassthroughRequest& req)" in block:
        raise AssertionError("raw handlers should not generate typed service signatures")


def verify_beta_service_block(block: str) -> None:
    assert_contains(block, "struct RpcPingRequest {\n        rrr::i32 other_id;\n    };")
    assert_contains(block, "struct RpcPingResponse {\n        std::string echoed;\n    };")
    assert_contains(
        block,
        "friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPingRequest& o) {\n"
        "        m << o.other_id;\n"
        "        return m;\n"
        "    }",
    )
    assert_contains(
        block,
        "virtual rusty::Result<RpcPingResponse, rrr::i32> ping(const RpcPingRequest& req) {\n"
        "        RpcPingResponse __typed_resp__;\n"
        "        this->ping(req.other_id, &__typed_resp__.echoed);\n"
        "        return rusty::Result<RpcPingResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "    }",
    )
    assert_contains(
        block,
        "void __ping__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        RpcPingRequest __typed_req__;\n"
        "        req->m >> __typed_req__.other_id;\n"
        "        auto __typed_result__ = this->ping(__typed_req__);\n"
        "        auto sconn_opt = weak_sconn.upgrade();\n"
        "        if (sconn_opt.is_some()) {\n"
        "            auto sconn = sconn_opt.unwrap();\n"
        "            if (__typed_result__.is_err()) {\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err());\n"
        "            } else {\n"
        "                auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::Marshal& m) {\n"
        "                    m << __typed_resp__.echoed;\n"
        "                });\n"
        "            }\n"
        "        }\n"
        "        // req automatically cleaned up by rusty::Box\n"
        "    }",
    )

def verify_alpha_proxy_block(block: str) -> None:
    assert_contains(
        block,
        "AlphaProxy(rrr::Client* cl): __cl__(cl) { }\n"
        "    // Alias typed request/response structs from the sibling Service class.\n"
        "    using RpcPingRequest = AlphaService::RpcPingRequest;\n"
        "    using RpcPingResponse = AlphaService::RpcPingResponse;\n",
    )
    assert_contains(
        block,
        "class pingTypedFuture {\n"
        "    private:\n"
        "        rusty::Arc<rrr::Future> __fu__;\n"
        "    public:\n"
        "        explicit pingTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }\n"
        "        bool ready() const {\n"
        "            return __fu__->ready();\n"
        "        }\n"
        "        void wait() const {\n"
        "            __fu__->wait();\n"
        "        }\n"
        "        rrr::i32 get_error_code() const {\n"
        "            return __fu__->get_error_code();\n"
        "        }\n"
        "        rusty::Arc<rrr::Future> raw_future() const {\n"
        "            return __fu__;\n"
        "        }\n"
        "        rusty::Result<RpcPingResponse, rrr::i32> resolve() const {\n"
        "            rrr::i32 __ret__ = __fu__->get_error_code();\n"
        "            if (__ret__ != 0) {\n"
        "                return rusty::Result<RpcPingResponse, rrr::i32>::Err(__ret__);\n"
        "            }\n"
        "            RpcPingResponse __typed_resp__;\n"
        "            __fu__->get_reply() >> __typed_resp__.msg;\n"
        "            return rusty::Result<RpcPingResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "        }\n"
        "    };",
    )
    assert_contains(
        block,
        "rrr::FutureResult async_ping(const rrr::i32& id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        RpcPingRequest __typed_req__;\n"
        "        __typed_req__.id = id;\n"
        "        auto __typed_fu_result__ = this->async_ping(__typed_req__, __fu_attr__);\n"
        "        if (__typed_fu_result__.is_err()) {\n"
        "            return rrr::FutureResult::Err(__typed_fu_result__.unwrap_err());\n"
        "        }\n"
        "        return rrr::FutureResult::Ok(__typed_fu_result__.unwrap().raw_future());\n"
        "    }",
    )
    assert_contains(
        block,
        "rusty::Result<pingTypedFuture, rrr::i32> async_ping(const RpcPingRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        auto __fu_result__ = __cl__->request(AlphaService::PING, __fu_attr__, [&](rrr::Marshal& __m__) {\n"
        "            __m__ << req.id;\n"
        "        });\n"
        "        if (__fu_result__.is_err()) {\n"
            "            return rusty::Result<pingTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        return rusty::Result<pingTypedFuture, rrr::i32>::Ok(pingTypedFuture(__fu_result__.unwrap()));\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::FutureResult async_nop(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        RpcNopRequest __typed_req__;\n"
        "        auto __typed_fu_result__ = this->async_nop(__typed_req__, __fu_attr__);\n"
        "        if (__typed_fu_result__.is_err()) {\n"
        "            return rrr::FutureResult::Err(__typed_fu_result__.unwrap_err());\n"
        "        }\n"
        "        return rrr::FutureResult::Ok(__typed_fu_result__.unwrap().raw_future());\n"
        "    }",
    )
    assert_contains(
        block,
        "rusty::Result<nopTypedFuture, rrr::i32> async_nop(const RpcNopRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        auto __fu_result__ = __cl__->request(AlphaService::NOP, __fu_attr__);\n"
        "        if (__fu_result__.is_err()) {\n"
        "            return rusty::Result<nopTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        (void)req;\n"
        "        return rusty::Result<nopTypedFuture, rrr::i32>::Ok(nopTypedFuture(__fu_result__.unwrap()));\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::FutureResult async_stream(const rrr::i32& stream_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        RpcStreamRequest __typed_req__;\n"
        "        __typed_req__.stream_id = stream_id;\n"
        "        auto __typed_fu_result__ = this->async_stream(__typed_req__, __fu_attr__);\n"
        "        if (__typed_fu_result__.is_err()) {\n"
        "            return rrr::FutureResult::Err(__typed_fu_result__.unwrap_err());\n"
        "        }\n"
        "        return rrr::FutureResult::Ok(__typed_fu_result__.unwrap().raw_future());\n"
        "    }",
    )
    assert_contains(
        block,
        "rusty::Result<streamTypedFuture, rrr::i32> async_stream(const RpcStreamRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        auto __fu_result__ = __cl__->request(AlphaService::STREAM, __fu_attr__, [&](rrr::Marshal& __m__) {\n"
        "            __m__ << req.stream_id;\n"
        "        });\n"
        "        if (__fu_result__.is_err()) {\n"
        "            return rusty::Result<streamTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        return rusty::Result<streamTypedFuture, rrr::i32>::Ok(streamTypedFuture(__fu_result__.unwrap()));\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::i32 ping(const rrr::i32& id, std::string* msg) {\n"
        "        RpcPingRequest __typed_req__;\n"
        "        __typed_req__.id = id;\n"
        "        auto __typed_result__ = this->ping(__typed_req__);\n"
        "        if (__typed_result__.is_err()) {\n"
        "            return __typed_result__.unwrap_err();\n"
        "        }\n"
        "        auto __typed_resp__ = __typed_result__.unwrap();\n"
        "        *msg = __typed_resp__.msg;\n"
        "        return 0;\n"
        "    }",
    )
    assert_contains(
        block,
        "rusty::Result<RpcPingResponse, rrr::i32> ping(const RpcPingRequest& req) {\n"
        "        auto __typed_fu_result__ = this->async_ping(req);\n"
        "        if (__typed_fu_result__.is_err()) {\n"
            "            return rusty::Result<RpcPingResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());\n"
        "        }\n"
        "        return __typed_fu_result__.unwrap().resolve();\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::i32 nop() {\n"
        "        RpcNopRequest __typed_req__;\n"
        "        auto __typed_result__ = this->nop(__typed_req__);\n"
        "        if (__typed_result__.is_err()) {\n"
        "            return __typed_result__.unwrap_err();\n"
        "        }\n"
        "        auto __typed_resp__ = __typed_result__.unwrap();\n"
        "        (void)__typed_resp__;\n"
        "        return 0;\n"
        "    }",
    )
    assert_contains(
        block,
        "rusty::Result<RpcNopResponse, rrr::i32> nop(const RpcNopRequest& req) {\n"
        "        auto __typed_fu_result__ = this->async_nop(req);\n"
        "        if (__typed_fu_result__.is_err()) {\n"
            "            return rusty::Result<RpcNopResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());\n"
        "        }\n"
        "        return __typed_fu_result__.unwrap().resolve();\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::i32 stream(const rrr::i32& stream_id, rrr::i64* sequence) {\n"
        "        RpcStreamRequest __typed_req__;\n"
        "        __typed_req__.stream_id = stream_id;\n"
        "        auto __typed_result__ = this->stream(__typed_req__);\n"
        "        if (__typed_result__.is_err()) {\n"
        "            return __typed_result__.unwrap_err();\n"
        "        }\n"
        "        auto __typed_resp__ = __typed_result__.unwrap();\n"
        "        *sequence = __typed_resp__.sequence;\n"
        "        return 0;\n"
        "    }",
    )
    assert_contains(
        block,
        "rusty::Result<RpcStreamResponse, rrr::i32> stream(const RpcStreamRequest& req) {\n"
        "        auto __typed_fu_result__ = this->async_stream(req);\n"
        "        if (__typed_fu_result__.is_err()) {\n"
            "            return rusty::Result<RpcStreamResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());\n"
        "        }\n"
        "        return __typed_fu_result__.unwrap().resolve();\n"
        "    }",
    )
    if "rusty::Result<RpcPassthroughResponse, rrr::i32> passthrough(const RpcPassthroughRequest& req)" in block:
        raise AssertionError("raw proxy handlers should not generate typed sync overloads")
    if "class passthroughTypedFuture {" in block:
        raise AssertionError("raw proxy handlers should not generate typed async wrappers")
    if "rusty::Result<passthroughTypedFuture, rrr::i32> async_passthrough(const RpcPassthroughRequest& req" in block:
        raise AssertionError("raw proxy handlers should not generate typed async signatures")


def verify_beta_proxy_block(block: str) -> None:
    assert_contains(
        block,
        "BetaProxy(rrr::Client* cl): __cl__(cl) { }\n"
        "    // Alias typed request/response structs from the sibling Service class.\n"
        "    using RpcPingRequest = BetaService::RpcPingRequest;\n"
        "    using RpcPingResponse = BetaService::RpcPingResponse;\n",
    )
    assert_contains(
        block,
        "class pingTypedFuture {\n"
        "    private:\n"
        "        rusty::Arc<rrr::Future> __fu__;\n"
        "    public:\n"
        "        explicit pingTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }\n"
        "        bool ready() const {\n"
        "            return __fu__->ready();\n"
        "        }\n"
        "        void wait() const {\n"
        "            __fu__->wait();\n"
        "        }\n"
        "        rrr::i32 get_error_code() const {\n"
        "            return __fu__->get_error_code();\n"
        "        }\n"
        "        rusty::Arc<rrr::Future> raw_future() const {\n"
        "            return __fu__;\n"
        "        }\n"
        "        rusty::Result<RpcPingResponse, rrr::i32> resolve() const {\n"
        "            rrr::i32 __ret__ = __fu__->get_error_code();\n"
        "            if (__ret__ != 0) {\n"
        "                return rusty::Result<RpcPingResponse, rrr::i32>::Err(__ret__);\n"
        "            }\n"
        "            RpcPingResponse __typed_resp__;\n"
        "            __fu__->get_reply() >> __typed_resp__.echoed;\n"
        "            return rusty::Result<RpcPingResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "        }\n"
        "    };",
    )
    assert_contains(
        block,
        "rusty::Result<pingTypedFuture, rrr::i32> async_ping(const RpcPingRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        auto __fu_result__ = __cl__->request(BetaService::PING, __fu_attr__, [&](rrr::Marshal& __m__) {\n"
        "            __m__ << req.other_id;\n"
        "        });\n"
        "        if (__fu_result__.is_err()) {\n"
            "            return rusty::Result<pingTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        return rusty::Result<pingTypedFuture, rrr::i32>::Ok(pingTypedFuture(__fu_result__.unwrap()));\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::FutureResult async_ping(const rrr::i32& other_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        RpcPingRequest __typed_req__;\n"
        "        __typed_req__.other_id = other_id;\n"
        "        auto __typed_fu_result__ = this->async_ping(__typed_req__, __fu_attr__);\n"
        "        if (__typed_fu_result__.is_err()) {\n"
        "            return rrr::FutureResult::Err(__typed_fu_result__.unwrap_err());\n"
        "        }\n"
        "        return rrr::FutureResult::Ok(__typed_fu_result__.unwrap().raw_future());\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::i32 ping(const rrr::i32& other_id, std::string* echoed) {\n"
        "        RpcPingRequest __typed_req__;\n"
        "        __typed_req__.other_id = other_id;\n"
        "        auto __typed_result__ = this->ping(__typed_req__);\n"
        "        if (__typed_result__.is_err()) {\n"
        "            return __typed_result__.unwrap_err();\n"
        "        }\n"
        "        auto __typed_resp__ = __typed_result__.unwrap();\n"
        "        *echoed = __typed_resp__.echoed;\n"
        "        return 0;\n"
        "    }",
    )
    assert_contains(
        block,
        "rusty::Result<RpcPingResponse, rrr::i32> ping(const RpcPingRequest& req) {\n"
        "        auto __typed_fu_result__ = this->async_ping(req);\n"
        "        if (__typed_fu_result__.is_err()) {\n"
            "            return rusty::Result<RpcPingResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());\n"
        "        }\n"
        "        return __typed_fu_result__.unwrap().resolve();\n"
        "    }",
    )


def verify_alpha_proxy_block_compat(block: str) -> None:
    if "// Alias typed request/response structs from the sibling Service class." in block:
        raise AssertionError("compat mode should not emit typed proxy aliases")
    if "class pingTypedFuture {" in block:
        raise AssertionError("compat mode should not emit typed proxy future wrappers")
    if "rusty::Result<RpcPingResponse, rrr::i32> ping(const RpcPingRequest& req)" in block:
        raise AssertionError("compat mode should not emit typed proxy sync overloads")
    if "rusty::Result<pingTypedFuture, rrr::i32> async_ping(const RpcPingRequest& req" in block:
        raise AssertionError("compat mode should not emit typed proxy async overloads")

    assert_contains(
        block,
        "rrr::FutureResult async_ping(const rrr::i32& id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        return __cl__->request(AlphaService::PING, __fu_attr__, [&](rrr::Marshal& __m__) {\n"
        "            __m__ << id;\n"
        "        });\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::i32 ping(const rrr::i32& id, std::string* msg) {\n"
        "        auto __fu_result__ = this->async_ping(id);\n"
        "        if (__fu_result__.is_err()) {\n"
        "            return __fu_result__.unwrap_err();  // Return error code\n"
        "        }\n"
        "        auto __fu__ = __fu_result__.unwrap();\n"
        "        rrr::i32 __ret__ = __fu__->get_error_code();\n"
        "        if (__ret__ == 0) {\n"
        "            __fu__->get_reply() >> *msg;\n"
        "        }\n"
        "        // Arc auto-released\n"
        "        return __ret__;\n"
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

        run_rpcgen(repo_root, rpc_path, cpp_mode="typed")
        header_path = rpc_path.with_suffix(".h")
        if not header_path.exists():
            raise AssertionError(f"missing generated header: {header_path}")

        generated = header_path.read_text(encoding="utf-8")
        assert_contains(generated, "// rpcgen cpp mode: typed")
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

        if generated.count("struct RpcPingRequest {") != 2:
            raise AssertionError("expected per-service RpcPingRequest structs (one in each service)")

        run_rpcgen(repo_root, rpc_path, cpp_mode="compat")
        compat_generated = header_path.read_text(encoding="utf-8")
        assert_contains(compat_generated, "// rpcgen cpp mode: compat")
        compat_alpha_proxy_block = section_between(
            compat_generated,
            "class AlphaProxy {",
            "class BetaService: public rrr::Service {",
        )
        verify_alpha_proxy_block_compat(compat_alpha_proxy_block)

    print("rpcgen typed/compat C++ mode emission verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
