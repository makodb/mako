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
    prefix multi(i32 left, string right | i64 sum, i8 ok);
    fiber async_sleep(i32 delay_ms | i32 ok);
    async async_wait(i32 value | i32 result);
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


def assert_not_contains(haystack: str, needle: str) -> None:
    if needle in haystack:
        raise AssertionError(f"snippet should NOT appear:\n{needle}\n")


def verify_alpha_service_block(block: str) -> None:
    assert_contains(block, "struct RpcPingRequest {\n        rrr::i32 id;\n    };")
    assert_contains(block, "struct RpcPingResponse {\n        std::string msg;\n    };")
    # the legacy `Marshal&` operator<<
    # emission is gone from auto-generated typed wrappers.  The
    # the `operator>>(Marshal&, ...)` form is
    # also dropped — every read path now routes through
    # `BinaryReadArchive`, including the routed
    # `operator>>(rusty::RefMut<Marshal>&, U&)` overload in
    # `client.hpp`.  Both Marshal-shaped operators are gone; only the
    # archive-shaped pair remains.
    assert_not_contains(
        block,
        "friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPingRequest& o)",
    )
    assert_not_contains(
        block,
        "friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPingResponse& o)",
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
        "struct RpcAsyncSleepRequest {\n"
        "        rrr::i32 delay_ms;\n"
        "    };",
    )
    assert_contains(
        block,
        "struct RpcAsyncSleepResponse {\n"
        "        rrr::i32 ok;\n"
        "    };",
    )
    assert_contains(
        block,
        "struct RpcAsyncWaitRequest {\n"
        "        rrr::i32 value;\n"
        "    };",
    )
    assert_contains(
        block,
        "struct RpcAsyncWaitResponse {\n"
        "        rrr::i32 result;\n"
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
        "virtual rusty::Result<RpcPingResponse, rrr::i32> ping(const RpcPingRequest& req);",
    )
    assert_contains(
        block,
        "virtual rusty::Result<RpcNopResponse, rrr::i32> nop(const RpcNopRequest& req);",
    )
    assert_contains(
        block,
        "virtual rusty::Result<RpcUnnamedResponse, rrr::i32> unnamed(const RpcUnnamedRequest& req);",
    )
    assert_contains(
        block,
        "virtual rusty::Result<RpcMultiResponse, rrr::i32> multi(const RpcMultiRequest& req);",
    )
    assert_contains(
        block,
        "virtual rusty::Result<RpcAsyncSleepResponse, rrr::i32> async_sleep(const RpcAsyncSleepRequest& req);",
    )
    assert_contains(
        block,
        "virtual rusty::Task<rusty::Result<RpcAsyncWaitResponse, rrr::i32>> async_wait(const RpcAsyncWaitRequest& req);",
    )
    assert_contains(
        block,
        "if ((ret = svr.reg_fast_rpc(ASYNC_WAIT, svc_index)) != 0) {",
    )
    assert_contains(
        block,
        "// @safe\n"
        "    virtual void stream(const RpcStreamRequest& req, RpcStreamResponse& resp, rrr::DeferredReply defer);",
    )
    assert_contains(
        block,
        "// @safe\n"
        "    void __ping__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        // @unsafe\n"
        "        {\n"
        "            RpcPingRequest __typed_req__;\n"
        "            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->src));\n"
        "            rrr::Deserialize_::deserialize(__typed_req__.id, __req_ar__);\n"
        "            auto __typed_result__ = this->ping(__typed_req__);\n"
        "            auto sconn_opt = weak_sconn.upgrade();\n"
        "            if (sconn_opt.is_some()) {\n"
        "                auto sconn = sconn_opt.unwrap();\n"
        "                if (__typed_result__.is_err()) {\n"
        "                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});\n"
        "                } else {\n"
        "                    auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::BinaryWriteArchive& m) {\n"
        "                        rrr::Serialize_::serialize(__typed_resp__.msg, m);\n"
        "                    });\n"
        "                }\n"
        "            }\n"
        "            // req automatically cleaned up by rusty::Box\n"
        "        }\n"
        "    }",
    )
    assert_contains(
        block,
        "void __nop__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        // @unsafe\n"
        "        {\n"
        "            RpcNopRequest __typed_req__;\n"
        "            auto __typed_result__ = this->nop(__typed_req__);\n"
        "            auto sconn_opt = weak_sconn.upgrade();\n"
        "            if (sconn_opt.is_some()) {\n"
        "                auto sconn = sconn_opt.unwrap();\n"
        "                if (__typed_result__.is_err()) {\n"
        "                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});\n"
        "                } else {\n"
        "                    auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                    (void)__typed_resp__;\n"
        "                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, rrr::ServerReplyFn{});\n"
        "                }\n"
        "            }\n"
        "            // req automatically cleaned up by rusty::Box\n"
        "        }\n"
        "    }",
    )
    assert_contains(
        block,
        "void __unnamed__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        // @unsafe\n"
        "        {\n"
        "            RpcUnnamedRequest __typed_req__;\n"
        "            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->src));\n"
        "            rrr::Deserialize_::deserialize(__typed_req__.in_0, __req_ar__);\n"
        "            auto __typed_result__ = this->unnamed(__typed_req__);\n"
        "            auto sconn_opt = weak_sconn.upgrade();\n"
        "            if (sconn_opt.is_some()) {\n"
        "                auto sconn = sconn_opt.unwrap();\n"
        "                if (__typed_result__.is_err()) {\n"
        "                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});\n"
        "                } else {\n"
        "                    auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::BinaryWriteArchive& m) {\n"
        "                        rrr::Serialize_::serialize(__typed_resp__.out_0, m);\n"
        "                    });\n"
        "                }\n"
        "            }\n"
        "            // req automatically cleaned up by rusty::Box\n"
        "        }\n"
        "    }",
    )
    assert_contains(
        block,
        "void __multi__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        // @unsafe\n"
        "        {\n"
        "            RpcMultiRequest __typed_req__;\n"
        "            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->src));\n"
        "            rrr::Deserialize_::deserialize(__typed_req__.left, __req_ar__);\n"
        "            rrr::Deserialize_::deserialize(__typed_req__.right, __req_ar__);\n"
        "            auto __typed_result__ = this->multi(__typed_req__);\n"
        "            auto sconn_opt = weak_sconn.upgrade();\n"
        "            if (sconn_opt.is_some()) {\n"
        "                auto sconn = sconn_opt.unwrap();\n"
        "                if (__typed_result__.is_err()) {\n"
        "                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});\n"
        "                } else {\n"
        "                    auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::BinaryWriteArchive& m) {\n"
        "                        rrr::Serialize_::serialize(__typed_resp__.sum, m);\n"
        "                        rrr::Serialize_::serialize(__typed_resp__.ok, m);\n"
        "                    });\n"
        "                }\n"
        "            }\n"
        "            // req automatically cleaned up by rusty::Box\n"
        "        }\n"
        "    }",
    )
    assert_contains(
        block,
        "void __async_sleep__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        // @unsafe\n"
        "        {\n"
        "            RpcAsyncSleepRequest __typed_req__;\n"
        "            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->src));\n"
        "            rrr::Deserialize_::deserialize(__typed_req__.delay_ms, __req_ar__);\n"
        "            auto __fiber_req__ = std::move(req);\n"
        "            auto __fiber_weak_sconn__ = weak_sconn;\n"
        "            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {\n"
        "                auto __typed_result__ = this->async_sleep(__typed_req__);\n"
        "                auto sconn_opt = __fiber_weak_sconn__.upgrade();\n"
        "                if (sconn_opt.is_some()) {\n"
        "                    auto sconn = sconn_opt.unwrap();\n"
        "                    if (__typed_result__.is_err()) {\n"
        "                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});\n"
        "                    } else {\n"
        "                        auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {\n"
        "                            rrr::Serialize_::serialize(__typed_resp__.ok, m);\n"
        "                        });\n"
        "                    }\n"
        "                }\n"
        "            });\n"
        "            (void)__fiber__;\n"
        "        }\n"
        "    }",
    )
    assert_contains(
        block,
        "void __async_wait__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        // @unsafe\n"
        "        {\n"
        "            RpcAsyncWaitRequest __typed_req__;\n"
        "            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->src));\n"
        "            rrr::Deserialize_::deserialize(__typed_req__.value, __req_ar__);\n"
        "            auto __async_req__ = std::move(req);\n"
        "            auto __async_weak_sconn__ = weak_sconn;\n"
        "            auto __async_task__ = this->async_wait(__typed_req__);\n"
        "            rrr::Reactor::get_reactor()->spawn_stackless_task_with_result(std::move(__async_task__), [__async_req__ = std::move(__async_req__), __async_weak_sconn__](auto __typed_result__) mutable {\n"
        "                auto sconn_opt = __async_weak_sconn__.upgrade();\n"
        "                if (sconn_opt.is_some()) {\n"
        "                    auto sconn = sconn_opt.unwrap();\n"
        "                    if (__typed_result__.is_err()) {\n"
        "                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__async_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});\n"
        "                    } else {\n"
        "                        auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__async_req__, 0, [&](rrr::BinaryWriteArchive& m) {\n"
        "                            rrr::Serialize_::serialize(__typed_resp__.result, m);\n"
        "                        });\n"
        "                    }\n"
        "                }\n"
        "            });\n"
        "        }\n"
        "    }",
    )
    assert_contains(
        block,
        "// @safe\n"
        "    void __stream__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        // @unsafe\n"
        "        {\n"
        "            RpcStreamRequest __typed_req__;\n"
        "            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->src));\n"
        "            rrr::Deserialize_::deserialize(__typed_req__.stream_id, __req_ar__);\n"
        "            auto __typed_resp__ = std::make_shared<RpcStreamResponse>();\n"
        "            auto __defer__ = rrr::DeferredReply::new_(\n"
        "                std::move(req),\n"
        "                weak_sconn,\n"
        "                [__typed_resp__](rrr::BinaryWriteArchive& m) {\n"
        "                    rrr::Serialize_::serialize(__typed_resp__->sequence, m);\n"
        "                },\n"
        "                []() {});\n"
        "            this->stream(__typed_req__, *__typed_resp__, std::move(__defer__));\n"
        "        }\n"
        "    }",
    )
    if "typed + legacy defer compatibility signatures" in block:
        raise AssertionError("legacy defer compatibility signatures should not be generated")

    if "virtual rusty::Result<RpcPassthroughResponse, rrr::i32> passthrough(const RpcPassthroughRequest& req)" in block:
        raise AssertionError("raw handlers should not generate typed service signatures")


def verify_beta_service_block(block: str) -> None:
    assert_contains(block, "struct RpcPingRequest {\n        rrr::i32 other_id;\n    };")
    assert_contains(block, "struct RpcPingResponse {\n        std::string echoed;\n    };")
    # 2 / Phase 3g-2: see
    # verify_alpha_service_block — both Marshal& operator<< and
    # operator>> emissions are dropped.
    assert_not_contains(
        block,
        "friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPingRequest& o)",
    )
    assert_not_contains(
        block,
        "friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPingRequest& o)",
    )
    assert_contains(
        block,
        "virtual rusty::Result<RpcPingResponse, rrr::i32> ping(const RpcPingRequest& req);",
    )
    assert_contains(
        block,
        "void __ping__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {\n"
        "        // @unsafe\n"
        "        {\n"
        "            RpcPingRequest __typed_req__;\n"
        "            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->src));\n"
        "            rrr::Deserialize_::deserialize(__typed_req__.other_id, __req_ar__);\n"
        "            auto __typed_result__ = this->ping(__typed_req__);\n"
        "            auto sconn_opt = weak_sconn.upgrade();\n"
        "            if (sconn_opt.is_some()) {\n"
        "                auto sconn = sconn_opt.unwrap();\n"
        "                if (__typed_result__.is_err()) {\n"
        "                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});\n"
        "                } else {\n"
        "                    auto __typed_resp__ = __typed_result__.unwrap();\n"
        "                    const_cast<rrr::ServerConnection&>(*sconn).reply(*req, 0, [&](rrr::BinaryWriteArchive& m) {\n"
        "                        rrr::Serialize_::serialize(__typed_resp__.echoed, m);\n"
        "                    });\n"
        "                }\n"
        "            }\n"
        "            // req automatically cleaned up by rusty::Box\n"
        "        }\n"
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
        "            auto __reply_guard__ = __fu__->get_reply();\n"
        "            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&__reply_guard__->src));\n"
        "            rrr::Deserialize_::deserialize(__typed_resp__.msg, __reply_ar__);\n"
        "            return rusty::Result<RpcPingResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "        }\n"
        "        auto operator co_await() const {\n"
        "            return rrr::make_typed_future_awaitable(*this);\n"
        "        }\n"
        "    };",
    )
    assert_contains(
        block,
        "rusty::Result<pingTypedFuture, rrr::i32> async_ping(const RpcPingRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        auto __fu_result__ = __cl__->request(AlphaService::PING, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {\n"
        "            rrr::Serialize_::serialize(req.id, __m__);\n"
        "        });\n"
        "        if (__fu_result__.is_err()) {\n"
            "            return rusty::Result<pingTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        return rusty::Result<pingTypedFuture, rrr::i32>::Ok(pingTypedFuture(__fu_result__.unwrap()));\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::TypedFutureResultAwaiter<pingTypedFuture> await_ping(const RpcPingRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        return rrr::make_typed_future_result_awaitable(this->async_ping(req, __fu_attr__));\n"
        "    }",
    )
    assert_contains(
        block,
        "rusty::Result<nopTypedFuture, rrr::i32> async_nop(const RpcNopRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        auto __fu_result__ = __cl__->request(AlphaService::NOP, __fu_attr__, [](rrr::BinaryWriteArchive&) {});\n"
        "        if (__fu_result__.is_err()) {\n"
            "            return rusty::Result<nopTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        (void)req;\n"
        "        return rusty::Result<nopTypedFuture, rrr::i32>::Ok(nopTypedFuture(__fu_result__.unwrap()));\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::TypedFutureResultAwaiter<nopTypedFuture> await_nop(const RpcNopRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        return rrr::make_typed_future_result_awaitable(this->async_nop(req, __fu_attr__));\n"
        "    }",
    )
    assert_contains(
        block,
        "rusty::Result<streamTypedFuture, rrr::i32> async_stream(const RpcStreamRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        auto __fu_result__ = __cl__->request(AlphaService::STREAM, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {\n"
        "            rrr::Serialize_::serialize(req.stream_id, __m__);\n"
        "        });\n"
        "        if (__fu_result__.is_err()) {\n"
        "            return rusty::Result<streamTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        return rusty::Result<streamTypedFuture, rrr::i32>::Ok(streamTypedFuture(__fu_result__.unwrap()));\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::TypedFutureResultAwaiter<streamTypedFuture> await_stream(const RpcStreamRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        return rrr::make_typed_future_result_awaitable(this->async_stream(req, __fu_attr__));\n"
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
        "rusty::Result<RpcStreamResponse, rrr::i32> stream(const RpcStreamRequest& req) {\n"
        "        auto __typed_fu_result__ = this->async_stream(req);\n"
        "        if (__typed_fu_result__.is_err()) {\n"
            "            return rusty::Result<RpcStreamResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());\n"
        "        }\n"
        "        return __typed_fu_result__.unwrap().resolve();\n"
        "    }",
    )
    if "rrr::FutureResult async_ping(const rrr::i32& id" in block:
        raise AssertionError("legacy proxy async wrappers should not be generated for non-raw methods")
    if "rrr::i32 ping(const rrr::i32& id, std::string* msg)" in block:
        raise AssertionError("legacy proxy sync wrappers should not be generated for non-raw methods")
    if "rrr::FutureResult async_nop(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr())" in block:
        raise AssertionError("legacy zero-arg async wrapper should not be generated for typed methods")
    if "rrr::i32 nop()" in block:
        raise AssertionError("legacy zero-arg sync wrapper should not be generated for typed methods")
    if "rusty::Result<RpcPassthroughResponse, rrr::i32> passthrough(const RpcPassthroughRequest& req)" in block:
        raise AssertionError("raw proxy handlers should not generate typed sync overloads")
    if "class passthroughTypedFuture {" in block:
        raise AssertionError("raw proxy handlers should not generate typed async wrappers")
    if "rusty::Result<passthroughTypedFuture, rrr::i32> async_passthrough(const RpcPassthroughRequest& req" in block:
        raise AssertionError("raw proxy handlers should not generate typed async signatures")
    assert_contains(
        block,
        "rrr::FutureResult async_passthrough(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {",
    )


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
        "            auto __reply_guard__ = __fu__->get_reply();\n"
        "            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&__reply_guard__->src));\n"
        "            rrr::Deserialize_::deserialize(__typed_resp__.echoed, __reply_ar__);\n"
        "            return rusty::Result<RpcPingResponse, rrr::i32>::Ok(__typed_resp__);\n"
        "        }\n"
        "        auto operator co_await() const {\n"
        "            return rrr::make_typed_future_awaitable(*this);\n"
        "        }\n"
        "    };",
    )
    assert_contains(
        block,
        "rusty::Result<pingTypedFuture, rrr::i32> async_ping(const RpcPingRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        auto __fu_result__ = __cl__->request(BetaService::PING, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {\n"
        "            rrr::Serialize_::serialize(req.other_id, __m__);\n"
        "        });\n"
        "        if (__fu_result__.is_err()) {\n"
            "            return rusty::Result<pingTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());\n"
        "        }\n"
        "        return rusty::Result<pingTypedFuture, rrr::i32>::Ok(pingTypedFuture(__fu_result__.unwrap()));\n"
        "    }",
    )
    assert_contains(
        block,
        "rrr::TypedFutureResultAwaiter<pingTypedFuture> await_ping(const RpcPingRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {\n"
        "        return rrr::make_typed_future_result_awaitable(this->async_ping(req, __fu_attr__));\n"
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
    if "rrr::FutureResult async_ping(const rrr::i32& other_id" in block:
        raise AssertionError("legacy beta proxy async wrappers should not be generated")
    if "rrr::i32 ping(const rrr::i32& other_id, std::string* echoed)" in block:
        raise AssertionError("legacy beta proxy sync wrappers should not be generated")


def verify_no_pointer_out_params(generated: str) -> None:
    """Borrow-check guard: no public T* out-param signatures in typed output.

    Scans the generated header for patterns that indicate legacy pointer-style
    out-parameters in public method signatures.  Internal wrapper code (private
    section, marshal operators) is allowed to use pointers.
    """
    import re
    violations = []

    # Forbidden pattern 1: deprecated wrappers should not exist
    if "[[deprecated" in generated:
        violations.append("found [[deprecated]] attribute in generated output")

    # Forbidden pattern 2: legacy sync proxy signature `i32 method(args..., T* out)`
    # Match: `rrr::i32 methodname(` followed later by `type* name)` on the same logical line.
    # Exclude: private wrapper methods (__name__wrapper__), marshal operators, raw handlers.
    for i, line in enumerate(generated.splitlines(), 1):
        stripped = line.strip()
        # Skip private wrappers, comments, marshal operators, struct fields
        if ("__wrapper__" in stripped or stripped.startswith("//") or
            "operator" in stripped or "friend " in stripped or
            stripped.startswith("m <<")):
            continue
        # Look for public method signatures with pointer out-params:
        # Pattern: `type* name)` or `type* name,` in a function declaration
        # But NOT in Box<T>, Arc<T>, shared_ptr<T>, or similar template params
        match = re.search(r'(?:rrr::i32|void)\s+\w+\([^)]*\w+\*\s+\w+[,)]', stripped)
        if match and "Box<" not in stripped and "Arc<" not in stripped:
            # Allow raw handler signatures (Box<Request> req, WeakServerConnection)
            if "passthrough" in stripped or "rusty::Box" in stripped:
                continue
            violations.append(f"line {i}: pointer out-param in public signature: {stripped.strip()}")

    if violations:
        raise AssertionError(
            "borrow-check guard failed — pointer out-params in typed output:\n"
            + "\n".join(f"  {v}" for v in violations)
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
            "class AlphaService {",
            "class AlphaProxy {",
        )
        beta_block = section_between(
            generated,
            "class BetaService {",
            "class BetaProxy {",
        )
        alpha_proxy_block = section_between(
            generated,
            "class AlphaProxy {",
            "class BetaService {",
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
        verify_no_pointer_out_params(generated)

        if generated.count("struct RpcPingRequest {") != 2:
            raise AssertionError("expected per-service RpcPingRequest structs (one in each service)")

    print("rpcgen typed request/response struct emission verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
