#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import textwrap
import tomllib
import unittest
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[2]
DRIVER_PATH = REPOSITORY / "scripts/extract_rrr_rust.py"
SPEC = importlib.util.spec_from_file_location("extract_rrr_rust", DRIVER_PATH)
assert SPEC is not None and SPEC.loader is not None
DRIVER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = DRIVER
SPEC.loader.exec_module(DRIVER)

GATE_PATH = REPOSITORY / "scripts/check_rrr_crate_mode.py"
GATE_SPEC = importlib.util.spec_from_file_location("check_rrr_crate_mode", GATE_PATH)
assert GATE_SPEC is not None and GATE_SPEC.loader is not None
GATE = importlib.util.module_from_spec(GATE_SPEC)
sys.modules[GATE_SPEC.name] = GATE
GATE_SPEC.loader.exec_module(GATE)


def source_block(source: str, block_id: str) -> str:
    marker = f"/*RUSTYCPP:GEN-BEGIN id={block_id} "
    marker_at = source.index(marker)
    prefix = source[:marker_at]
    end = prefix.rfind("#endif")
    start_directive = prefix.rfind("#if RUSTYCPP_RUST", 0, end)
    if end < 0 or start_directive < 0:
        raise AssertionError(f"cannot locate source block {block_id}")
    start = prefix.index("\n", start_directive) + 1
    return prefix[start:end].strip("\n") + "\n"


def split_generated(data: bytes) -> tuple[list[str], bytes]:
    header, payload = data.split(b"//\n", 1)
    return header.decode("utf-8").splitlines(), payload


def generated_by_label(
    generated: list[object], output_label: str
) -> object:
    return next(item for item in generated if item.output_label == output_label)


def subprocess_result(
    returncode: int, stdout: str, stderr: str
) -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(
        args=["rusty-cpp-transpiler", "--build-info"],
        returncode=returncode,
        stdout=stdout,
        stderr=stderr,
    )


class CheckedInCanaryTests(unittest.TestCase):
    def test_discarded_parallel_crate_stays_absent(self) -> None:
        self.assertFalse((REPOSITORY / "crates/srpc").exists())

    def test_retired_inline_carriers_stay_absent(self) -> None:
        retired = (
            "src/rrr/base/callback_wrapper.cpp",
            "src/rrr/rpc/internal_protocol.cpp",
            "src/rrr/misc/stat.cpp",
            "src/rrr/rpc/errors.cpp",
            "src/rrr/rpc/connection_metrics.cpp",
            "src/rrr/rpc/completion_tracker.cpp",
            "src/rrr/misc/rand.cpp",
            "src/rrr/rpc/request_options.cpp",
            "src/rrr/rpc/reconnect_policy.cpp",
            "src/rrr/rpc/circuit_breaker.cpp",
            "src/rrr/rpc/connection_state.cpp",
            "src/rrr/rpc/heartbeat.cpp",
            "src/rrr/base/basetypes.cpp",
            "src/rrr/rpc/request_queue.cpp",
            "src/rrr/rpc/load_balancer.cpp",
            "src/rrr/rpc/utils.cpp",
            "src/rrr/rpc/frame_codec.cpp",
        )
        self.assertTrue(all(not (REPOSITORY / path).exists() for path in retired))

    def test_source_boundary_census_tracks_the_remaining_scaffolding(self) -> None:
        output = subprocess.check_output(
            [sys.executable, "scripts/rrr_handwritten_census.py"],
            cwd=REPOSITORY,
            text=True,
        )
        self.assertIn(
            "source boundary: 1 hand-authored module units, "
            "SCAFFOLD=20 noncomment lines (10 DSL fences + 10 other)",
            output,
        )
        # Re-measured 2026-08-18 when the rusty-cpp pin moved
        # ebb51610 -> fa7dd9d9 and the src/rrr DSL blocks were regenerated.
        # dsl 8943 -> 8942: the rusty::HashSet Serialize body lost its
        # `let kv = e.unwrap();` line (std_port's set iterator yields the
        # element, not a (T, monostate) pair).
        # generated 11744 -> 12414: fa7dd9d9 emits more C++ per block
        # (nested field access goes through a probe lambda at every level,
        # enums carry an explicit underlying type, and so on).
        # NOT regenerated, deliberately: the six files carrying
        # `#[cpp_inherit]` impls (base/misc.cpp, reactor/reactor.cpp,
        # rpc/{inmemory_channel,pollable_proxy,server,tcp_channel}.cpp)
        # plus serializable.cpp's one `serializable.shared_ptr_holder`
        # block, whose GEN region is kept at its pre-fa7dd9d9 content.
        # fa7dd9d9 stopped emitting a base-class list for those and emits
        # Adapter/AdapterRef/AdapterRefMut wrappers instead, which breaks
        # every hand-written `Arc<Event> -> Arc<EventPollable>` /
        # `Box<Shim> -> Box<Base>` upcast in the reactor and channel code.
        # Adopting that model is its own migration.
        self.assertIn(
            "payload census:   dsl=52  generated=61 "
            "nonblank/non-// lines",
            output,
        )
        # 146 -> 148 when mako-dev merged in: PR #78's macOS support added
        # `#include <queue>` and `#include <stack>` to the
        # src/rrr/rpc/frame_codec.hpp compatibility shim. Two lines of std
        # includes in a shim that otherwise only re-exports the module; kept
        # so the macOS build keeps working, at the cost of the ratchet going
        # the wrong way by 2.
        self.assertIn(
            "12 compatibility headers, SCAFFOLD=148 noncomment lines", output
        )
        self.assertIn(
            "terminal C:      3 ABI headers/89 lines; 8 kernels/531 lines",
            output,
        )

    def test_modules_have_only_the_expected_structured_preambles(self) -> None:
        with (REPOSITORY / "src/rrr/module-preambles.toml").open("rb") as stream:
            self.assertEqual(
                tomllib.load(stream),
                {
                    "version": 1,
                    "module": [
                        {
                            "name": "rrr.basetypes",
                            "includes": [
                                {
                                    "path": "misc/srpc_timing.h",
                                    "form": "quote",
                                },
                                {
                                    "path": "rusty/sync/atomic.hpp",
                                    "form": "angle",
                                },
                            ],
                        },
                        {
                            "name": "rrr.connection_metrics",
                            "includes": [
                                {
                                    "path": "rusty/sync/atomic.hpp",
                                    "form": "angle",
                                },
                            ],
                        },
                        {
                            "name": "rrr.completion_tracker",
                            "includes": [
                                {
                                    "path": "rusty/sync/atomic.hpp",
                                    "form": "angle",
                                },
                            ],
                        },
                        {
                            "name": "rrr.rand",
                            "includes": [
                                {
                                    "path": "misc/srpc_rand.h",
                                    "form": "quote",
                                },
                            ],
                        },
                        {
                            "name": "rrr.circuit_breaker",
                            "includes": [
                                {
                                    "path": "misc/srpc_timing.h",
                                    "form": "quote",
                                },
                            ],
                        },
                        {
                            "name": "rrr.threading",
                            "includes": [
                                {
                                    "path": "pthread.h",
                                    "form": "angle",
                                },
                                {
                                    "path": "misc/srpc_timing.h",
                                    "form": "quote",
                                },
                                {
                                    "path": "rusty/sync/atomic.hpp",
                                    "form": "angle",
                                },
                            ],
                        },
                        {
                            "name": "rrr.utils",
                            "includes": [
                                {
                                    "path": "netdb.h",
                                    "form": "angle",
                                },
                            ],
                        },
                        {
                            "name": "rrr.frame_codec",
                            "includes": [
                                {
                                    "path": "vector",
                                    "form": "angle",
                                },
                                {
                                    "path": "rusty/io.hpp",
                                    "form": "angle",
                                },
                            ],
                        },
                        {
                            "name": "rrr.misc",
                            "includes": [
                                {
                                    "path": "base/rustc_markers.hpp",
                                    "form": "quote",
                                },
                            ],
                        },
                        {
                            "name": "rrr.inmemory_channel",
                            "includes": [
                                {
                                    "path": "base/rustc_markers.hpp",
                                    "form": "quote",
                                },
                            ],
                        },
                        {
                            "name": "rrr.server",
                            "includes": [
                                {
                                    "path": "base/rustc_markers.hpp",
                                    "form": "quote",
                                },
                                {
                                    "path": "rpc/srpc_server.h",
                                    "form": "quote",
                                },
                            ],
                        },
                        {
                            "name": "rrr.tcp_channel",
                            "includes": [
                                {
                                    "path": "base/rustc_markers.hpp",
                                    "form": "quote",
                                },
                                {
                                    "path": "rpc/srpc_connect.h",
                                    "form": "quote",
                                },
                            ],
                        },
                        {
                            "name": "rrr.epoll_wrapper",
                            "includes": [
                                {
                                    "path": "rusty/os/fd.hpp",
                                    "form": "angle",
                                },
                                {
                                    "path": "rusty/sync/atomic.hpp",
                                    "form": "angle",
                                },
                            ],
                        },
                        {
                            "name": "rrr.debugging",
                            "includes": [
                                {
                                    "path": "stdio.h",
                                    "form": "angle",
                                },
                                {
                                    "path": "source_location",
                                    "form": "angle",
                                },
                            ],
                        },
                        {
                            "name": "rrr.serializable",
                            "includes": [
                                {
                                    "path": "misc/serializable_support.hpp",
                                    "form": "quote",
                                },
                            ],
                        },
                        {
                            "name": "rrr.reactor",
                            "includes": [
                                {
                                    "path": "reactor/srpc_fiber.h",
                                    "form": "quote",
                                },
                                {
                                    "path": "set",
                                    "form": "angle",
                                },
                            ],
                        },
                    ],
                }
            )

    def test_utils_sidecars_are_narrow_and_fail_closed_inputs(self) -> None:
        with (REPOSITORY / "src/rrr/rust-type-map.toml").open("rb") as stream:
            self.assertEqual(
                tomllib.load(stream),
                {
                    "LegacyAddrInfo": "addrinfo",
                    "LegacyCChar": "std::string::value_type",
                    "LegacyStdString": "std::string",
                    "LegacyCallbackWrapper": "::rrr::detail::CallbackWrapper",
                    "LegacyChannelConnectionBase": "rrr::ChannelConnectionBase",
                    "LegacyStdDeque": "std::deque",
                    "LegacyTcpListener": "rusty::net::TcpListener",
                    "LegacySocketAddrV4": "rusty::net::SocketAddrV4",
                    "LegacyIoErrorKind": "rusty::io::Error::Kind",
                    "std::marker::PhantomPinned": "rusty::marker::PhantomPinned",
                    "BinaryReadArchive": "BinaryReadArchive",
                    "BinaryWriteArchive": "BinaryWriteArchive",
                    "SerializableBase": "SerializableBase",
                    "SerializableProxy": "rusty::Arc<SerializableBase>",
                    "SerializableSharedPtrHolder": "details::SerializableSharedPtrHolder",
                    "SrcFileCStr": "const char*",
                    "rusty": {
                        "CallbackWrapper": "::rrr::detail::CallbackWrapper",
                        "ReactorJobSet": "std::set",
                        "StdVector": "std::vector",
                        "PthreadSpinlock": "::pthread_spinlock_t",
                        "PthreadMutex": "::pthread_mutex_t",
                        "PthreadMutexAttr": "::pthread_mutexattr_t",
                        "PthreadCond": "::pthread_cond_t",
                        "PthreadCondAttr": "::pthread_condattr_t",
                        "BinaryReadArchive": "BinaryReadArchive",
                        "BinaryWriteArchive": "BinaryWriteArchive",
                        "SerializableBase": "SerializableBase",
                        "SerializableProxy": "rusty::Arc<SerializableBase>",
                        "SerializableSharedPtrHolder": "details::SerializableSharedPtrHolder",
                        "LoggingString": "std::string",
                        "CFile": "FILE",
                        "SourceLocation": "std::source_location",
                        "ReactorBoxEvent": "BoxEvent",
                        "ReactorFiber": "Fiber",
                        "ReactorIntEvent": "IntEvent",
                        "ReactorPollThread": "PollThread",
                        "ReactorFiberContext": "::srpc_fiber_ctx",
                        "ReactorFiberState": "::srpc_fiber",
                        "StdPair": "std::pair",
                        "Mutex": "rusty::Mutex",
                        "BTreeSet": "rusty::BTreeSet",
                        "BTreeMap": "rusty::BTreeMap",
                        "HashSet": "rusty::HashSet",
                        "HashMap": "rusty::HashMap",
                        "SerializableStdStringView": "std::string_view",
                        "SerializableStdList": "std::list",
                        "SerializableStdVector": "std::vector",
                        "SerializableStdSet": "std::set",
                        "SerializableStdUnorderedSet": "std::unordered_set",
                        "SerializableStdMap": "std::map",
                        "SerializableStdUnorderedMap": "std::unordered_map",
                        "SerializableV32": "v32",
                        "SerializableV64": "v64",
                        "LegacyCVoid": "void",
                        "RustcSinkBaseAdapterRefMut": "SinkBaseAdapterRefMut",
                        "RustcSourceBaseAdapterRefMut": "SourceBaseAdapterRefMut",
                        "SerializableSerializeDispatch": "Serialize_",
                        "SerializableDeserializeDispatch": "Deserialize_",
                        "SerializableRegistryFactory": "rusty::Function<SerializableProxy()>",
                        "rrr::serializable::BinaryWriteArchive": "rrr::BinaryWriteArchive",
                        "rrr::serializable::BinaryReadArchive": "rrr::BinaryReadArchive",
                        "LegacyOwnedFd": "rusty::os::fd::OwnedFd",
                        "RustcOwnedFd": "rusty::os::fd::OwnedFd",
                        "LegacyTcpListener": "rusty::net::TcpListener",
                        "RustcTcpListener": "rusty::net::TcpListener",
                        "RustcTcpStream": "rusty::net::TcpStream",
                        "RustcSocketAddrV4": "rusty::net::SocketAddrV4",
                        "RustcIoError": "rusty::io::Error",
                        "RustcIoErrorKind": "rusty::io::Error::Kind",
                        "errors::RpcError": "::rrr::RpcError",
                        "LegacyRpcError": "::rrr::RpcError",
                    },
                }
            )
        with (REPOSITORY / "src/rrr/cpp-module-index.toml").open("rb") as stream:
            self.assertEqual(
                tomllib.load(stream),
                {
                    "version": 1,
                    "modules": {
                        "rrr::basetypes": {
                            "cpp_module": "rrr.basetypes",
                            "namespace": "rrr",
                            "symbols": {
                                "SparseInt::buf_size": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "size_t(uint8_t)",
                                    ],
                                },
                                "SparseInt::dump32": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "size_t(int32_t,uint8_t*)",
                                    ],
                                },
                                "SparseInt::dump64": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "size_t(int64_t,uint8_t*)",
                                    ],
                                },
                                "SparseInt::load32": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "int32_t(const uint8_t*)",
                                    ],
                                },
                                "SparseInt::load64": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "int64_t(const uint8_t*)",
                                    ],
                                },
                                "Time": {
                                    "kind": "type",
                                    "callable_signatures": [
                                    ],
                                },
                                "Time::now": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "uint64_t(bool)",
                                    ],
                                },
                            },
                        },
                        "rrr::logging": {
                            "cpp_module": "rrr.logging",
                            "namespace": "rrr",
                            "symbols": {
                                "log_line": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "void(int32_t,int32_t,const int8_t*,const std::string&)",
                                    ],
                                },
                            },
                        },
                        "rrr::reactor": {
                            "cpp_module": "rrr.reactor",
                            "namespace": "rrr",
                            "symbols": {
                                "create_sp_box_event": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "rusty::Arc<BoxEvent<T>>()",
                                    ],
                                },
                                "Fiber": {
                                    "kind": "type",
                                    "callable_signatures": [
                                    ],
                                },
                                "IntEvent": {
                                    "kind": "type",
                                    "callable_signatures": [
                                    ],
                                },
                                "create_sp_int_event": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "rusty::Arc<IntEvent>(int32_t)",
                                    ],
                                },
                                "IntEvent::set": {
                                    "kind": "method",
                                    "callable_signatures": [
                                        "int32_t(int32_t)",
                                    ],
                                },
                                "IntEvent::wait": {
                                    "kind": "method",
                                    "callable_signatures": [
                                        "void()",
                                    ],
                                },
                                "Fiber::current_fiber": {
                                    "kind": "method",
                                    "callable_signatures": [
                                        "rusty::Option<rusty::Rc<Fiber>>()",
                                    ],
                                },
                                "Fiber::yield_": {
                                    "kind": "method",
                                    "callable_signatures": [
                                        "void()",
                                    ],
                                },
                                "fiber_sleep": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "void(uint64_t)",
                                    ],
                                },
                                "pollworker_is_on_poll_thread": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "bool()",
                                    ],
                                },
                                "PollThread": {
                                    "kind": "type",
                                    "callable_signatures": [
                                    ],
                                },
                                "PollThread::add_proxy": {
                                    "kind": "method",
                                    "callable_signatures": [
                                        "void(PollableProxy)",
                                    ],
                                },
                                "PollThread::update_mode": {
                                    "kind": "method",
                                    "callable_signatures": [
                                        "void(int32_t,int32_t)",
                                    ],
                                },
                                "PollThread::create": {
                                    "kind": "method",
                                    "callable_signatures": [
                                        "rusty::Arc<PollThread>()",
                                    ],
                                },
                                "PollThread::add": {
                                    "kind": "method",
                                    "callable_signatures": [
                                        "void(rusty::Arc<Job>)",
                                    ],
                                },
                                "fiber_create_run_impl": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "rusty::Rc<Fiber>(rusty::Function<void()>,const char*,int64_t)",
                                    ],
                                },
                            },
                        },
                        "rrr::debugging": {
                            "cpp_module": "rrr.debugging",
                            "namespace": "rrr",
                            "symbols": {
                                "verify": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "void(bool)",
                                    ],
                                },
                            },
                        },
                        "rrr::rand": {
                            "cpp_module": "rrr.rand",
                            "namespace": "rrr",
                            "symbols": {
                                "RandomGenerator::rand": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "int32_t(int32_t,int32_t)",
                                    ],
                                },
                            },
                        },
                        "rrr::errors": {
                            "cpp_module": "rrr.errors",
                            "namespace": "rrr",
                        },
                        "rrr::serializable": {
                            "cpp_module": "rrr.serializable",
                            "namespace": "rrr",
                            "symbols": {
                                "make_sink_proxy_buffer": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "SinkProxy(BufferSink*)",
                                    ],
                                },
                                "make_source_proxy_buffer": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "SourceProxy(BufferSource*)",
                                    ],
                                },
                                "serializable_holder_of": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "const Holder<T>*(const SerializableBase*)",
                                    ],
                                },
                                "SerializableRegistry::create": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "SerializableProxy(int32_t)",
                                    ],
                                },
                                "BinaryWriteArchive": {
                                    "kind": "type",
                                },
                                "BinaryReadArchive": {
                                    "kind": "type",
                                },
                                "SerializableBase": {
                                    "kind": "type",
                                },
                                "details::SerializableSharedPtrHolder": {
                                    "kind": "type_template_constructor",
                                    "callable_signatures": [
                                        "details::SerializableSharedPtrHolder<T>(rusty::Arc<T>)",
                                    ],
                                },
                                "Serialize_::serialize": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "void(const std::string&,BinaryWriteArchive&)",
                                    ],
                                },
                                "Deserialize_::deserialize": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "void(std::string&,BinaryReadArchive&)",
                                    ],
                                },
                            },
                        },
                        "rusty": {
                            "cpp_module": "rusty",
                            "namespace": "rusty",
                            "symbols": {
                                "Arc::get": {
                                    "kind": "method",
                                    "callable_signatures": [
                                        "const T*()",
                                    ],
                                },
                                "arc_make_default": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "rusty::Arc<T>()",
                                    ],
                                },
                                "os::fd::OwnedFd": {
                                    "kind": "type",
                                    "callable_signatures": [
                                    ],
                                },
                                "os::fd::OwnedFd::from_raw_fd": {
                                    "kind": "function",
                                    "callable_signatures": [
                                        "OwnedFd(int)",
                                    ],
                                },
                                "srpc_adl_serialize": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "void(const T&,Archive&)",
                                    ],
                                },
                                "srpc_adl_deserialize": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "void(T&,Archive&)",
                                    ],
                                },
                                "srpc_sink_write": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "void(Sink&,const uint8_t*,size_t)",
                                    ],
                                },
                                "srpc_source_read": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "size_t(Source&,uint8_t*,size_t)",
                                    ],
                                },
                                "srpc_arc_default": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "rusty::Arc<T>()",
                                    ],
                                },
                                "srpc_arc_copy": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "rusty::Arc<T>(const T&)",
                                    ],
                                },
                                "srpc_holder_proxy": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "rrr::SerializableProxy(rusty::Arc<T>)",
                                    ],
                                },
                                "srpc_factory_from_callable": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "rusty::Function<R()>(Callable)",
                                    ],
                                },
                            },
                        },
                        "std": {
                            "cpp_module": "std",
                            "namespace": "std",
                            "symbols": {
                                "make_pair": {
                                    "kind": "function_template",
                                    "callable_signatures": [
                                        "std::pair<A,B>(A,B)",
                                    ],
                                },
                                "cout": {
                                    "kind": "object",
                                    "callable_signatures": [
                                    ],
                                },
                            },
                        },
                    },
                }
            )

    def test_manifest_names_the_canonical_rust_sources(self) -> None:
        modules = DRIVER.load_manifest(
            REPOSITORY, REPOSITORY / "src/rrr/rust-modules.toml"
        )
        self.assertEqual(
            [
                (
                    module.cpp_module,
                    module.rust_module,
                    module.output_label,
                    module.canonical_source_label,
                )
                for module in modules
            ],
            [
                (
                    "rrr.basetypes",
                    "basetypes",
                    "src/rrr/base/basetypes.rs",
                    "src/rrr/base/basetypes.rs",
                ),
                (
                    "rrr.callback_wrapper",
                    "callback_wrapper",
                    "src/rrr/base/callback_wrapper.rs",
                    "src/rrr/base/callback_wrapper.rs",
                ),
                (
                    "rrr.internal_protocol",
                    "internal_protocol",
                    "src/rrr/rpc/internal_protocol.rs",
                    "src/rrr/rpc/internal_protocol.rs",
                ),
                (
                    "rrr.stat",
                    "stat",
                    "src/rrr/misc/stat.rs",
                    "src/rrr/misc/stat.rs",
                ),
                (
                    "rrr.errors",
                    "errors",
                    "src/rrr/rpc/errors.rs",
                    "src/rrr/rpc/errors.rs",
                ),
                (
                    "rrr.connection_metrics",
                    "connection_metrics",
                    "src/rrr/rpc/connection_metrics.rs",
                    "src/rrr/rpc/connection_metrics.rs",
                ),
                (
                    "rrr.completion_tracker",
                    "completion_tracker",
                    "src/rrr/rpc/completion_tracker.rs",
                    "src/rrr/rpc/completion_tracker.rs",
                ),
                (
                    "rrr.rand",
                    "rand",
                    "src/rrr/misc/rand.rs",
                    "src/rrr/misc/rand.rs",
                ),
                (
                    "rrr.request_options",
                    "request_options",
                    "src/rrr/rpc/request_options.rs",
                    "src/rrr/rpc/request_options.rs",
                ),
                (
                    "rrr.reconnect_policy",
                    "reconnect_policy",
                    "src/rrr/rpc/reconnect_policy.rs",
                    "src/rrr/rpc/reconnect_policy.rs",
                ),
                (
                    "rrr.circuit_breaker",
                    "circuit_breaker",
                    "src/rrr/rpc/circuit_breaker.rs",
                    "src/rrr/rpc/circuit_breaker.rs",
                ),
                (
                    "rrr.connection_state",
                    "connection_state",
                    "src/rrr/rpc/connection_state.rs",
                    "src/rrr/rpc/connection_state.rs",
                ),
                (
                    "rrr.heartbeat",
                    "heartbeat",
                    "src/rrr/rpc/heartbeat.rs",
                    "src/rrr/rpc/heartbeat.rs",
                ),
                (
                    "rrr.request_queue",
                    "request_queue",
                    "src/rrr/rpc/request_queue.rs",
                    "src/rrr/rpc/request_queue.rs",
                ),
                (
                    "rrr.load_balancer",
                    "load_balancer",
                    "src/rrr/rpc/load_balancer.rs",
                    "src/rrr/rpc/load_balancer.rs",
                ),
                (
                    "rrr.debugging",
                    "debugging",
                    "src/rrr/base/debugging.rs",
                    "src/rrr/base/debugging.rs",
                ),
                (
                    "rrr.logging",
                    "logging",
                    "src/rrr/base/logging.rs",
                    "src/rrr/base/logging.rs",
                ),
                (
                    "rrr.utils",
                    "utils",
                    "src/rrr/rpc/utils.rs",
                    "src/rrr/rpc/utils.rs",
                ),
                (
                    "rrr.frame_codec",
                    "frame_codec",
                    "src/rrr/rpc/frame_codec.rs",
                    "src/rrr/rpc/frame_codec.rs",
                ),
                (
                    "rrr.serializable",
                    "serializable",
                    "src/rrr/misc/serializable.rs",
                    "src/rrr/misc/serializable.rs",
                ),
                (
                    "rrr.serializable_envelope",
                    "serializable_envelope",
                    "src/rrr/misc/serializable_envelope.rs",
                    "src/rrr/misc/serializable_envelope.rs",
                ),
                (
                    "rrr.epoll_wrapper",
                    "epoll_wrapper",
                    "src/rrr/reactor/epoll_wrapper.rs",
                    "src/rrr/reactor/epoll_wrapper.rs",
                ),
                (
                    "rrr.misc",
                    "misc",
                    "src/rrr/base/misc.rs",
                    "src/rrr/base/misc.rs",
                ),
                (
                    "rrr.pollable_proxy",
                    "pollable_proxy",
                    "src/rrr/rpc/pollable_proxy.rs",
                    "src/rrr/rpc/pollable_proxy.rs",
                ),
                (
                    "rrr.reactor",
                    "reactor",
                    "src/rrr/reactor/reactor.rs",
                    "src/rrr/reactor/reactor.rs",
                ),
                (
                    "rrr.future",
                    "future",
                    "src/rrr/reactor/future.rs",
                    "src/rrr/reactor/future.rs",
                ),
                (
                    "rrr.idempotency",
                    "idempotency",
                    "src/rrr/rpc/idempotency.rs",
                    "src/rrr/rpc/idempotency.rs",
                ),
                (
                    "rrr.fiber",
                    "fiber",
                    "src/rrr/reactor/fiber.rs",
                    "src/rrr/reactor/fiber.rs",
                ),
                (
                    "rrr.channel",
                    "channel",
                    "src/rrr/rpc/channel.rs",
                    "src/rrr/rpc/channel.rs",
                ),
                (
                    "rrr.callbacks",
                    "callbacks",
                    "src/rrr/rpc/callbacks.rs",
                    "src/rrr/rpc/callbacks.rs",
                ),
                (
                    "rrr.inmemory_channel",
                    "inmemory_channel",
                    "src/rrr/rpc/inmemory_channel.rs",
                    "src/rrr/rpc/inmemory_channel.rs",
                ),
                (
                    "rrr.fiber_channel",
                    "fiber_channel",
                    "src/rrr/rpc/fiber_channel.rs",
                    "src/rrr/rpc/fiber_channel.rs",
                ),
                (
                    "rrr.threading",
                    "threading",
                    "src/rrr/base/threading.rs",
                    "src/rrr/base/threading.rs",
                ),
                (
                    "rrr.any_message",
                    "any_message",
                    "src/rrr/misc/any_message.rs",
                    "src/rrr/misc/any_message.rs",
                ),
                (
                    "rrr.tcp_channel",
                    "tcp_channel",
                    "src/rrr/rpc/tcp_channel.rs",
                    "src/rrr/rpc/tcp_channel.rs",
                ),
                (
                    "rrr.server",
                    "server",
                    "src/rrr/rpc/server.rs",
                    "src/rrr/rpc/server.rs",
                ),
                (
                    "rrr.client",
                    "client",
                    "src/rrr/rpc/client.rs",
                    "src/rrr/rpc/client.rs",
                ),
            ],
        )

    def test_cmake_provider_inventory_matches_the_canonical_manifest(self) -> None:
        modules = DRIVER.load_manifest(
            REPOSITORY, REPOSITORY / "src/rrr/rust-modules.toml"
        )
        cmake = (REPOSITORY / "src/rrr/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        match = re.search(
            r"set\(RRR_GOAL0_CANONICAL_MODULES\n(?P<body>.*?)\n\)",
            cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(match)
        assert match is not None
        cmake_modules = tuple(
            line.strip()
            for line in match.group("body").splitlines()
            if line.strip()
        )
        manifest_modules = tuple(module.rust_module for module in modules)
        self.assertEqual(cmake_modules, manifest_modules)
        self.assertEqual(len(cmake_modules), len(set(cmake_modules)))
        # Canonical sources mirror the C++ layout now, so CMake carries the
        # relative paths explicitly instead of deriving `src/<name>.rs`. The
        # list must equal the manifest's, in manifest order.
        relpath_match = re.search(
            r"set\(RRR_GOAL0_CANONICAL_SOURCE_RELPATH\n(?P<body>.*?)\n\)",
            cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(relpath_match)
        assert relpath_match is not None
        cmake_relpaths = tuple(
            line.strip()
            for line in relpath_match.group("body").splitlines()
            if line.strip()
        )
        self.assertEqual(
            cmake_relpaths,
            tuple(
                module.canonical_source_label[len("src/rrr/") :]
                for module in modules
            ),
        )
        self.assertIn(
            "${CMAKE_CURRENT_SOURCE_DIR}/${_RRR_GOAL0_RELPATH}",
            cmake,
        )
        self.assertIn(
            "${RRR_GOAL0_CRATE_CPP_DIR}/rrr.${_RRR_GOAL0_MODULE}.cppm",
            cmake,
        )
        for fragment in (
            'set(RRR_GOAL0_TYPE_MAP\n    ${CMAKE_CURRENT_SOURCE_DIR}/rust-type-map.toml',
            'set(RRR_GOAL0_CPP_MODULE_INDEX\n    ${CMAKE_CURRENT_SOURCE_DIR}/cpp-module-index.toml',
            '--type-map "${RRR_GOAL0_TYPE_MAP}"',
            '--cpp-module-index "${RRR_GOAL0_CPP_MODULE_INDEX}"',
            '"${RRR_GOAL0_TYPE_MAP}"',
            '"${RRR_GOAL0_CPP_MODULE_INDEX}"',
        ):
            self.assertIn(fragment, cmake)

    def test_flat_import_namespace_is_declared_and_passed_to_crate_mode(
        self,
    ) -> None:
        """The manifest key and the emitter flag must agree.

        The seventeen canonical sources carry NO per-item
        `cpp_import_namespace` marker: every private
        `use crate::<child>::<Name leaves>;` gets its contract from the
        crate-level namespace instead. That inference only happens when the
        emitter is actually invoked with `--flat-import-namespace`, so a
        manifest key with no flag (or a flag with no key) would silently
        change what the generated providers mean.
        """

        manifest = REPOSITORY / "src/rrr/rust-modules.toml"
        self.assertEqual(
            DRIVER.load_flat_import_namespace(REPOSITORY, manifest), "rrr"
        )
        cmake = (REPOSITORY / "src/rrr/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn("--flat-import-namespace rrr", cmake)
        gate = (REPOSITORY / "scripts/check_rrr_crate_mode.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("extraction.load_flat_import_namespace(", gate)
        self.assertIn('["--flat-import-namespace", flat_import_namespace]', gate)
        self.assertIn("*flat_import_arguments,", gate)
        for source in (
            module.output
            for module in DRIVER.load_manifest(REPOSITORY, manifest)
        ):
            # The invariant is "no per-item MARKER", i.e. no
            # `#[cfg_attr(any(), cpp_import_namespace(...))]` attribute. Scan
            # code only: `channel.rs` explains the emitter's leaf contract in
            # a doc comment, and prose naming the mechanism is not a marker.
            code = "\n".join(
                line
                for line in source.read_text(encoding="utf-8").splitlines()
                if not line.lstrip().startswith("//")
            )
            self.assertNotIn(
                "cpp_import_namespace",
                code,
                msg=f"{source} still carries a per-item marker",
            )

    def test_manifest_rejects_an_unknown_top_level_key(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / "rust-modules.toml"
            manifest.write_text(
                'schema_version = 2\nstray = "x"\n'
                '[[module]]\ncpp_module = "rrr.example"\n'
                'source = "src/rrr/src/example.rs"\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                DRIVER.ExtractionError, "manifest keys must be exactly"
            ):
                DRIVER.load_manifest(root, manifest)

    def test_manifest_rejects_a_non_namespace_flat_import_value(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = root / "rust-modules.toml"
            manifest.write_text(
                'schema_version = 2\nflat_import_namespace = "not a ns"\n'
                '[[module]]\ncpp_module = "rrr.example"\n'
                'source = "src/rrr/src/example.rs"\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(
                DRIVER.ExtractionError,
                "flat_import_namespace must be a C\\+\\+ namespace path",
            ):
                DRIVER.load_manifest(root, manifest)

        workflow = (REPOSITORY / ".github/workflows/ci.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn('canonical_input="src/rrr/src/frame_codec.rs"', workflow)
        # These used to pin the `<name>_generated_path=` / `..._before=$(stat
        # -c %Y ...)` / `test ... -gt ...` machinery of the determinism step.
        # That machinery is gone: it asserted outputs were REWRITTEN, which the
        # emitters deliberately do not do for byte-identical content, so it
        # failed CI on a correct build. The coverage it was protecting -- that
        # the facade and sidecar sub-checks still watch utils and frame_codec --
        # is now pinned directly on the disposition assertions that replaced it.
        self.assertIn(
            "scripts/ci/assert_crate_codegen.sh", workflow
        )
        for generated in ("rrr.utils.cppm", "rrr.frame_codec.cppm"):
            self.assertIn(generated, workflow)
        # This used to pin `test_rpc_tcp_channel`, one of the nineteen
        # `src/rrr/tests/` binaries the Goal-0 job named. 4a06ef0e stopped
        # building that corpus (it is srpc's, and runs in srpc's CI), so the
        # canary was pinning a target that no longer exists. What is worth
        # pinning is the property that made the breakage survivable-but-silent:
        # `ctest -R` exits 0 when its pattern matches nothing, so without this
        # flag a stale name list goes green having run zero tests. See
        # GoalZeroConsumerSelectionTests for the list-agreement checks.
        self.assertIn("--no-tests=error", workflow)
        self.assertIn('type_map_input="src/rrr/rust-type-map.toml"', workflow)
        self.assertIn(
            'module_index_input="src/rrr/cpp-module-index.toml"', workflow
        )
        self.assertIn(
            'for sidecar_input in "${type_map_input}" "${module_index_input}"',
            workflow,
        )

    def test_checked_in_modules_are_canonical_rust_sources(self) -> None:
        modules = DRIVER.load_manifest(
            REPOSITORY, REPOSITORY / "src/rrr/rust-modules.toml"
        )
        # `src/rrr` is now srpc's tree byte for byte, so the two-line
        # "// Canonical Rust source for the rrr.X module." banner is gone from
        # the seventeen sources srpc never carried it on. Nothing is dropped:
        # the banner only claimed ownership and location, and BOTH facts are
        # now enforced structurally and more tightly --
        #   * the driver already pins the file to an approved production root
        #     with a basename equal to the module (validate_production_source_path),
        #   * and the generated crate index must reach exactly this file
        #     through a `#[path]` attribute, checked here.
        # The "not a generated artifact" half of the banner's job stays as the
        # explicit marker assertions.
        library = (REPOSITORY / "src/rrr/src/lib.rs").read_text(encoding="utf-8")
        canonical_lines = 0
        for module in modules:
            with self.subTest(cpp_module=module.cpp_module):
                source = module.output.read_text(encoding="utf-8")
                self.assertIsNotNone(module.canonical_source_label)
                value = DRIVER.module_path_attribute_value(
                    module.canonical_source_label
                )
                self.assertIn(
                    f'#[path = "{value}"]\npub mod {module.rust_module};',
                    library,
                )
                self.assertEqual(
                    (REPOSITORY / "src/rrr/src" / value).resolve(),
                    module.output.resolve(),
                )
                self.assertNotIn("@generated", source)
                self.assertNotIn("provenance-input", source)
                canonical_lines += sum(
                    bool(line.strip()) and not line.lstrip().startswith("//")
                    for line in source.splitlines()
                )
        # 2574 -> 2563: the eleven `#[cfg_attr(any(), cpp_ctor)]` /
        # `#[cfg_attr(any(), cpp_import_namespace(rrr))]` marker lines were
        # deleted when these sources adopted factory-only construction and the
        # crate-level `flat_import_namespace`. No logic line moved.
        # 2563 -> 2677: +114, the canonical `src/rrr/src/debugging.rs` grafted
        # from srpc when `base/debugging.cpp` was retired.
        # 2677 -> 13656: the remaining nineteen carriers were retired in the
        # same way, so the crate now owns all thirty-seven modules. The count
        # is unchanged by the move to the layout-mirroring paths -- the same
        # bytes, minus a banner that was only ever comment lines.
        # 13656 -> 13654: -2, collapsing one `else { if .. }` into `else if`
        # in `reactor.rs::event_core_recycle_or_reset` (clippy 1.91's
        # `collapsible_else_if`, which the CI image pins and which fails the
        # source gate's `clippy -D warnings` step). Pure syntax: the closing
        # brace and the `else {` opener are the only lines that went away, no
        # logic line moved, and the dual-compile gate still reports the same
        # 1961 provider-owned strong ABI symbols from the generated C++.
        self.assertEqual(canonical_lines, 13654)

    def test_canonical_source_validation_never_normalizes_owned_bytes(self) -> None:
        payload = b"pub fn canonical() {}\n\n"
        self.assertIs(
            DRIVER.validate_canonical_source(payload, "src/rrr/src/example.rs"),
            payload,
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "LF line endings"):
            DRIVER.validate_canonical_source(
                b"pub fn canonical() {}\r\n", "src/rrr/src/example.rs"
            )

    def test_write_never_replaces_a_canonical_source_snapshot(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-canonical-write-") as temporary:
            root = Path(temporary)
            # A canonical source lives at its layout-mirroring path, not in
            # src/. src/ holds only the generated crate index, and the census
            # rejects anything else that appears there.
            source = root / "src/rrr/rpc/example.rs"
            source.parent.mkdir(parents=True)
            (root / "src/rrr/src").mkdir(parents=True)
            original = b"pub fn canonical() -> i32 { 1 }\n"
            changed = b"pub fn canonical() -> i32 { 2 }\n"
            source.write_bytes(original)
            generated = [
                DRIVER.GeneratedFile(
                    output_label="src/rrr/rpc/example.rs",
                    output=source,
                    content=original,
                    writable=False,
                )
            ]
            source.write_bytes(changed)
            with self.assertRaisesRegex(
                DRIVER.ExtractionError, "refusing to overwrite"
            ):
                DRIVER.apply_mode(root, generated, "write")
            self.assertEqual(source.read_bytes(), changed)

    def test_lib_is_manifest_generated_and_census_has_no_orphans(self) -> None:
        manifest = REPOSITORY / "src/rrr/rust-modules.toml"
        modules = DRIVER.load_manifest(REPOSITORY, manifest)
        expected_lib = DRIVER.render_lib(
            "src/rrr/rust-modules.toml", manifest, modules
        )
        self.assertEqual(
            (REPOSITORY / "src/rrr/src/lib.rs").read_bytes(),
            expected_lib,
        )
        self.assertEqual(
            DRIVER.rust_source_census(REPOSITORY),
            {
                "src/rrr/src/lib.rs",
            },
        )

    def test_unsafe_allowances_are_confined_to_the_audited_c_boundaries(self) -> None:
        with (REPOSITORY / "src/rrr/Cargo.toml").open("rb") as stream:
            cargo = tomllib.load(stream)
        self.assertEqual(cargo["lints"]["rust"]["unsafe_code"], "deny")

        unsafe_syntax = re.compile(
            r"#\s*\[\s*allow\s*\(\s*unsafe_code\s*\)\s*\]"
            r"|#\s*\[\s*unsafe\b"
            r"|\bunsafe\s+(?:(?:async|const)\s+)*fn\b"
            r"|\bunsafe\s+(?:extern|impl|trait)\b"
            r"|\bunsafe\s*\{"
        )
        # Canonical sources mirror the C++ layout; take every path from the
        # manifest so this audit can never drift from where the bytes live.
        rand_path = REPOSITORY / "src/rrr/misc/rand.rs"
        circuit_path = REPOSITORY / "src/rrr/rpc/circuit_breaker.rs"
        basetypes_path = REPOSITORY / "src/rrr/base/basetypes.rs"
        utils_path = REPOSITORY / "src/rrr/rpc/utils.rs"
        frame_codec_path = REPOSITORY / "src/rrr/rpc/frame_codec.rs"
        debugging_path = REPOSITORY / "src/rrr/base/debugging.rs"

        # Goal 0 complete: all thirty-seven modules are canonical Rust, and
        # `unsafe` is no longer confined to a handful of files -- the reactor,
        # the serializer, and the transports each own a real C/FFI boundary.
        # A blanket "no unsafe outside these six" assertion can no longer be
        # written, so it is replaced by an EXACT per-file census over EVERY
        # canonical source. Nothing is exempted: a file that should have no
        # unsafe is pinned at all-zero, which is exactly the old assertion,
        # and any new unsafe anywhere moves a number. Columns are
        #   (#[allow(unsafe_code)], #![allow(unsafe_code)], unsafe extern,
        #    unsafe fn, unsafe {, unsafe impl, unsafe trait)
        unsafe_census = {
            "src/rrr/base/basetypes.rs": (10, 0, 1, 4, 9, 0, 0),
            "src/rrr/base/callback_wrapper.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/rpc/internal_protocol.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/misc/stat.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/rpc/errors.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/rpc/connection_metrics.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/rpc/completion_tracker.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/misc/rand.rs": (3, 0, 1, 0, 2, 0, 0),
            "src/rrr/rpc/request_options.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/rpc/reconnect_policy.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/rpc/circuit_breaker.rs": (2, 0, 1, 0, 1, 0, 0),
            "src/rrr/rpc/connection_state.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/rpc/heartbeat.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/rpc/request_queue.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/rpc/load_balancer.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/base/debugging.rs": (4, 0, 1, 1, 8, 0, 0),
            "src/rrr/base/logging.rs": (5, 0, 1, 2, 7, 0, 0),
            "src/rrr/rpc/utils.rs": (5, 0, 1, 1, 5, 0, 0),
            "src/rrr/rpc/frame_codec.rs": (5, 0, 0, 3, 4, 0, 0),
            "src/rrr/misc/serializable.rs": (43, 0, 3, 14, 56, 0, 0),
            "src/rrr/misc/serializable_envelope.rs": (8, 0, 0, 2, 16, 0, 0),
            "src/rrr/reactor/epoll_wrapper.rs": (6, 0, 2, 0, 5, 0, 0),
            "src/rrr/base/misc.rs": (5, 0, 1, 0, 2, 2, 2),
            "src/rrr/rpc/pollable_proxy.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/reactor/reactor.rs": (0, 0, 3, 0, 66, 0, 0),
            "src/rrr/reactor/future.rs": (3, 0, 0, 0, 3, 0, 0),
            "src/rrr/rpc/idempotency.rs": (4, 0, 0, 0, 4, 0, 0),
            "src/rrr/reactor/fiber.rs": (0, 1, 0, 0, 10, 0, 0),
            "src/rrr/rpc/channel.rs": (2, 0, 0, 1, 0, 2, 2),
            "src/rrr/rpc/callbacks.rs": (0, 0, 0, 0, 0, 0, 0),
            "src/rrr/rpc/inmemory_channel.rs": (1, 1, 0, 3, 4, 1, 0),
            "src/rrr/rpc/fiber_channel.rs": (0, 0, 0, 1, 7, 0, 0),
            "src/rrr/base/threading.rs": (15, 0, 1, 13, 14, 0, 0),
            "src/rrr/misc/any_message.rs": (1, 0, 0, 0, 8, 0, 0),
            "src/rrr/rpc/tcp_channel.rs": (1, 0, 0, 6, 33, 5, 0),
            "src/rrr/rpc/server.rs": (1, 1, 1, 7, 44, 4, 0),
            "src/rrr/rpc/client.rs": (2, 1, 0, 2, 25, 2, 0),
        }
        measured = {}
        for module in DRIVER.load_manifest(
            REPOSITORY, REPOSITORY / "src/rrr/rust-modules.toml"
        ):
            text = module.output.read_text(encoding="utf-8")
            measured[module.output_label] = (
                text.count("#[allow(unsafe_code"),
                text.count("#![allow(unsafe_code"),
                text.count("unsafe extern"),
                text.count("unsafe fn"),
                text.count("unsafe {"),
                text.count("unsafe impl"),
                text.count("unsafe trait"),
            )
        self.assertEqual(measured, unsafe_census)

        rust = rand_path.read_text(encoding="utf-8")
        allowed_sections = (
            textwrap.dedent(
                """\
                #[allow(unsafe_code)]
                unsafe extern "C" {
                    fn srpc_rand_raw() -> i32;
                    fn srpc_rand_destroy();
                }
                """
            ).strip(),
            textwrap.dedent(
                """\
                #[allow(unsafe_code)]
                pub fn randgen_rand_raw() -> i32 {
                    unsafe { srpc_rand_raw() }
                }
                """
            ).strip(),
            textwrap.dedent(
                """\
                #[allow(unsafe_code)]
                pub fn randgen_destroy() {
                    unsafe { srpc_rand_destroy(); }
                }
                """
            ).strip(),
        )
        remainder = rust
        for section in allowed_sections:
            self.assertEqual(rust.count(section), 1)
            remainder = remainder.replace(section, "", 1)
        self.assertIsNone(
            unsafe_syntax.search(remainder),
            "rand.rs gained unsafe syntax outside its three exact C-boundary scopes",
        )

        circuit = circuit_path.read_text(encoding="utf-8")
        circuit_sections = (
            textwrap.dedent(
                """\
                #[allow(unsafe_code)]
                unsafe extern "C" {
                    fn srpc_clock_monotonic_us() -> u64;
                }
                """
            ).strip(),
            textwrap.dedent(
                """\
                #[allow(unsafe_code)]
                pub fn current_time_us() -> u64 {
                    unsafe { srpc_clock_monotonic_us() }
                }
                """
            ).strip(),
        )
        remainder = circuit
        for section in circuit_sections:
            self.assertEqual(circuit.count(section), 1)
            remainder = remainder.replace(section, "", 1)
        self.assertIsNone(
            unsafe_syntax.search(remainder),
            "circuit_breaker.rs gained unsafe syntax outside its exact clock boundary",
        )

        basetypes = basetypes_path.read_text(encoding="utf-8")
        self.assertEqual(basetypes.count("#[allow(unsafe_code)]"), 10)
        self.assertEqual(basetypes.count('unsafe extern "C"'), 1)
        self.assertEqual(basetypes.count("pub unsafe fn"), 4)
        self.assertEqual(basetypes.count("unsafe {"), 9)
        self.assertEqual(basetypes.count("/// # Safety"), 4)
        for symbol in (
            "srpc_clock_monotonic_us",
            "srpc_clock_realtime_coarse_us",
            "srpc_gettimeofday_us",
            "srpc_sleep_us",
        ):
            self.assertIn(symbol, basetypes)

        utils = utils_path.read_text(encoding="utf-8")
        self.assertEqual(utils.count("#[allow(unsafe_code)]"), 5)
        self.assertEqual(utils.count('unsafe extern "C"'), 1)
        self.assertEqual(utils.count("pub unsafe fn adopt"), 1)
        self.assertEqual(utils.count("unsafe {"), 5)
        self.assertEqual(utils.count("/// # Safety"), 1)
        self.assertIn("    info_: *mut LegacyAddrInfo,", utils)
        self.assertIn("    owned_: Cell<bool>,", utils)
        self.assertNotIn("pub info_:", utils)
        self.assertNotIn("pub owned_:", utils)
        for symbol in ("freeaddrinfo", "srpc_find_open_port"):
            self.assertIn(symbol, utils)

        frame_codec = frame_codec_path.read_text(encoding="utf-8")
        self.assertEqual(frame_codec.count("#[allow(unsafe_code"), 5)
        self.assertEqual(frame_codec.count("unsafe extern"), 0)
        self.assertEqual(frame_codec.count("pub unsafe fn"), 3)
        self.assertEqual(frame_codec.count("unsafe {"), 4)
        self.assertEqual(frame_codec.count("/// # Safety"), 3)
        self.assertNotIn("not_unsafe_ptr_arg_deref", frame_codec)
        for symbol in (
            "core::ptr::copy_nonoverlapping",
            "core::ptr::copy(",
            "rem.as_ptr().add(kFrameHeaderSize)",
        ):
            self.assertIn(symbol, frame_codec)

        # Audited C boundary: `rrr.debugging` reaches libc's execinfo pair,
        # `stderr`, and `fputs` through one `unsafe extern "C"` block in
        # `debugging_ffi`, then walks the C-owned `char**` byte by byte.
        debugging = debugging_path.read_text(encoding="utf-8")
        self.assertEqual(debugging.count("#[allow(unsafe_code)]"), 4)
        self.assertEqual(debugging.count('unsafe extern "C"'), 1)
        self.assertEqual(debugging.count("pub unsafe fn"), 1)
        self.assertEqual(debugging.count("unsafe {"), 8)
        self.assertEqual(debugging.count("/// # Safety"), 1)
        for symbol in (
            "srpc_stderr",
            "srpc_backtrace_capture",
            "srpc_backtrace_free",
            "fputs",
        ):
            self.assertIn(symbol, debugging)

        facade_manifest = REPOSITORY / "src/rrr/rusty-rustc/Cargo.toml"
        with facade_manifest.open("rb") as stream:
            facade_cargo = tomllib.load(stream)
        self.assertEqual(facade_cargo["package"]["name"], "rusty")
        self.assertEqual(facade_cargo["lib"]["path"], "src/lib.rs")
        self.assertEqual(
            facade_cargo["dependencies"]["rusty-cpp-markers"],
            {"path": "../rusty-cpp-markers"},
        )
        self.assertEqual(facade_cargo["lints"]["rust"]["unsafe_code"], "deny")
        self.assertFalse((facade_manifest.parent / "Cargo.lock").exists())
        facade = (facade_manifest.parent / "src/lib.rs").read_text(encoding="utf-8")
        logging_boundary = (
            "#[allow(unsafe_code)]\n"
            "        pub unsafe fn log_line("
            "_level: i32, _line: i32, _file: *const i8, _message: &String) {}"
        )
        self.assertEqual(facade.count(logging_boundary), 1)
        # The facade is the rustc-only model of the rusty C++ runtime. With
        # all thirty-seven modules canonical it has to model the runtime's own
        # unsafe surface (fd/socket handles, Arc, the archives, the reactor
        # types), so the earlier "one boundary (log_line)" and then
        # "log_line + the std::string byte model" forms no longer describe it.
        # Replace them with an EXACT count ratchet over the whole file. This
        # is not a relaxation: every count is equality, so any new unsafe in
        # the facade -- or any removal -- fails the gate.
        #   #[allow(unsafe_code)]        1  -> 58
        #   unsafe extern                1  ->  1
        #   unsafe fn                    1  -> 53
        #   unsafe {                     0  -> 29
        #   /// # Safety                 1  -> 38
        # `unsafe impl` / `unsafe trait` / `#![allow(unsafe_code)]` stay at
        # zero: the facade never asserts a thread-safety property, it only
        # models call boundaries.
        self.assertEqual(facade.count(logging_boundary), 1)
        self.assertEqual(facade.count("#[allow(unsafe_code"), 58)
        self.assertEqual(facade.count("#![allow(unsafe_code"), 0)
        self.assertEqual(facade.count("unsafe extern"), 1)
        self.assertEqual(facade.count("unsafe fn"), 53)
        self.assertEqual(facade.count("unsafe {"), 29)
        self.assertEqual(facade.count("unsafe impl"), 0)
        self.assertEqual(facade.count("unsafe trait"), 0)
        self.assertEqual(facade.count("/// # Safety"), 38)

        # The inert-attribute crate exists only so rustc accepts
        # `#[cpp_inherit]` on the trait impls the emitter turns into C++ base
        # classes. It must stay a proc-macro shim with no unsafe of its own.
        markers_manifest = REPOSITORY / "src/rrr/rusty-cpp-markers/Cargo.toml"
        with markers_manifest.open("rb") as stream:
            markers_cargo = tomllib.load(stream)
        self.assertEqual(markers_cargo["package"]["name"], "rusty-cpp-markers")
        self.assertTrue(markers_cargo["lib"]["proc-macro"])
        self.assertEqual(markers_cargo["lints"]["rust"]["unsafe_code"], "deny")
        markers = (markers_manifest.parent / "src/lib.rs").read_text(
            encoding="utf-8"
        )
        self.assertIsNone(unsafe_syntax.search(markers))
        self.assertIn("pub fn cpp_inherit(", markers)

        self.assertIn("inner: Option<Box<F>>", facade)
        self.assertIn("runtime_layout_padding: [u8; 32]", facade)
        self.assertIn("impl<F: ?Sized> Deref for Function<F>", facade)
        self.assertIn("impl<F: ?Sized> DerefMut for Function<F>", facade)
        # srpc's facade specializes the one-argument FnMut erasure on the
        # concrete `i32` the canonical sources use rather than a generic `A`.
        self.assertIn("impl Function<dyn FnMut(i32)>", facade)
        self.assertIn("pub type StdVector<T> = Vec<T>;", facade)

        self.assertEqual(
            cargo["workspace"]["members"],
            ["rusty-cpp-markers", "rusty-rustc"],
        )
        self.assertEqual(cargo["dependencies"]["rusty"], {"path": "rusty-rustc"})


class DriverBehaviorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="rrr-extractor-test-")
        self.root = Path(self.temporary.name)
        source_root = self.root / "src/rrr/rpc"
        source_root.mkdir(parents=True)
        self.interface = source_root / "example.cpp"
        self.interface.write_text(
            textwrap.dedent(
                """\
                module;
                export module rrr.example;

                #if RUSTYCPP_RUST
                const FIRST: i32 = 7;
                #endif
                /*RUSTYCPP:GEN-BEGIN id=example.1 version=1 rust_sha256=unused*/
                generated C++ 1
                /*RUSTYCPP:GEN-END id=example.1*/

                #if RUSTYCPP_RUST
                const SECOND: i32 = 11;
                #endif
                /*RUSTYCPP:GEN-BEGIN id=example.2 version=1 rust_sha256=unused*/
                generated C++ 2
                /*RUSTYCPP:GEN-END id=example.2*/
                """
            ),
            encoding="utf-8",
        )
        self.implementation = source_root / "example_impl.cc"
        self.implementation.write_text(
            textwrap.dedent(
                """\
                module rrr.example;

                #if RUSTYCPP_RUST
                fn implementation() -> i32 { 13 }
                #endif
                /*RUSTYCPP:GEN-BEGIN id=example.impl version=1 rust_sha256=unused*/
                generated C++ implementation
                /*RUSTYCPP:GEN-END id=example.impl*/
                """
            ),
            encoding="utf-8",
        )
        self.manifest = self.root / "src/rrr/rust-extraction.toml"
        self.write_manifest(
            """\
            schema_version = 1

            [[module]]
            cpp_module = "rrr.example"
            output = "src/rrr/src/example.rs"

            [[module.input]]
            source = "src/rrr/rpc/example.cpp"
            block_ids = ["example.2", "example.1"]

            [[module.input]]
            source = "src/rrr/rpc/example_impl.cc"
            block_ids = ["example.impl"]
            """
        )
        self.log = self.root / "argv.json"
        self.fake = self.root / "fake-inline-rust"
        self.fake.write_text(
            textwrap.dedent(
                """\
                #!/usr/bin/env python3
                import json
                import os
                from pathlib import Path
                import sys

                args = sys.argv[1:]
                log = Path(os.environ["FAKE_ARGV_LOG"])
                history = json.loads(log.read_text()) if log.exists() else []
                history.append(args)
                log.write_text(json.dumps(history))
                if args[0] != "inline-rust":
                    raise SystemExit(9)
                output = Path(args[args.index("--emit-rust") + 1])
                source = Path(args[args.index("--files") + 1]).read_text()
                block_ids = [
                    args[index + 1]
                    for index, value in enumerate(args)
                    if value == "--block-id"
                ]
                payloads = []
                for block_id in block_ids:
                    marker_at = source.index(
                        f"/*RUSTYCPP:GEN-BEGIN id={block_id} "
                    )
                    prefix = source[:marker_at]
                    end = prefix.rfind("#endif")
                    directive = prefix.rfind("#if RUSTYCPP_RUST", 0, end)
                    start = prefix.index("\\n", directive) + 1
                    payloads.append(prefix[start:end].strip("\\n"))
                output.write_text("\\n\\n".join(payloads) + "\\n")
                """
            ),
            encoding="utf-8",
        )
        self.fake.chmod(0o755)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def write_manifest(self, contents: str) -> None:
        self.manifest.parent.mkdir(parents=True, exist_ok=True)
        self.manifest.write_text(textwrap.dedent(contents), encoding="utf-8")

    def generate(self) -> list[object]:
        modules = DRIVER.load_manifest(self.root, self.manifest)
        executable = DRIVER.resolve_transpiler(self.root, str(self.fake))
        with mock.patch.dict(os.environ, {"FAKE_ARGV_LOG": str(self.log)}):
            return DRIVER.generate_all(
                self.root,
                modules,
                executable,
                "src/rrr/rust-extraction.toml",
                self.manifest,
            )

    def test_write_and_check_are_deterministic_and_use_one_call_per_source(self) -> None:
        generated = self.generate()
        DRIVER.apply_mode(self.root, generated, "write")
        first = {
            item.output_label: item.output.read_bytes()
            for item in generated
        }

        DRIVER.apply_mode(self.root, self.generate(), "check")
        self.assertEqual(
            {item.output_label: item.output.read_bytes() for item in generated},
            first,
        )

        history = json.loads(self.log.read_text(encoding="utf-8"))
        self.assertEqual(len(history), 4)
        for offset in (0, 2):
            self.assertEqual(history[offset][0:2], ["inline-rust", "--emit-rust"])
            self.assertEqual(
                history[offset][3:],
                [
                    "--block-id",
                    "example.2",
                    "--block-id",
                    "example.1",
                    "--files",
                    "src/rrr/rpc/example.cpp",
                ],
            )
            self.assertEqual(
                history[offset + 1][3:],
                [
                    "--block-id",
                    "example.impl",
                    "--files",
                    "src/rrr/rpc/example_impl.cc",
                ],
            )

    def test_two_sources_are_concatenated_in_manifest_order(self) -> None:
        generated = self.generate()
        module = generated_by_label(generated, "src/rrr/src/example.rs")
        header, payload = split_generated(module.content)
        self.assertEqual(
            payload,
            b"const SECOND: i32 = 11;\n\n"
            b"const FIRST: i32 = 7;\n\n"
            b"fn implementation() -> i32 { 13 }\n",
        )
        self.assertIn(
            "// provenance-input[0]-block-ids: example.2, example.1",
            header,
        )
        self.assertIn(
            "// provenance-input[1]-block-ids: example.impl",
            header,
        )
        history = json.loads(self.log.read_text(encoding="utf-8"))
        self.assertEqual(len(history), 2)

    def test_check_detects_drift_without_rewriting(self) -> None:
        generated = self.generate()
        DRIVER.apply_mode(self.root, generated, "write")
        output = self.root / "src/rrr/src/example.rs"
        output.write_text("tampered\n", encoding="utf-8")

        with self.assertRaisesRegex(DRIVER.ExtractionError, "stale"):
            DRIVER.apply_mode(self.root, generated, "check")
        self.assertEqual(output.read_text(encoding="utf-8"), "tampered\n")

    def test_check_and_write_reject_orphan_rust_sources(self) -> None:
        generated = self.generate()
        DRIVER.apply_mode(self.root, generated, "write")
        orphan = self.root / "src/rrr/src/orphan.rs"
        orphan.write_text("parallel implementation\n", encoding="utf-8")
        for mode in ("check", "write"):
            with self.subTest(mode=mode):
                with self.assertRaisesRegex(DRIVER.ExtractionError, "orphan"):
                    DRIVER.apply_mode(self.root, generated, mode)
        self.assertEqual(orphan.read_text(), "parallel implementation\n")

    def test_check_rejects_stale_and_missing_generated_lib(self) -> None:
        generated = self.generate()
        DRIVER.apply_mode(self.root, generated, "write")
        lib = generated_by_label(generated, "src/rrr/src/lib.rs")
        lib.output.write_text("stale lib\n", encoding="utf-8")
        with self.assertRaisesRegex(DRIVER.ExtractionError, "stale"):
            DRIVER.apply_mode(self.root, generated, "check")
        DRIVER.apply_mode(self.root, generated, "write")
        lib.output.unlink()
        with self.assertRaisesRegex(DRIVER.ExtractionError, "missing"):
            DRIVER.apply_mode(self.root, generated, "check")

    def test_output_symlink_is_rejected_at_load_and_before_write(self) -> None:
        generated = self.generate()
        output = self.root / "src/rrr/src/example.rs"
        output.parent.mkdir(parents=True)
        victim = self.root / "src/rrr/victim.rs"
        victim.write_text("do not overwrite\n", encoding="utf-8")
        output.symlink_to("../victim.rs")

        with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
            DRIVER.load_manifest(self.root, self.manifest)
        with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
            DRIVER.apply_mode(self.root, generated, "write")
        self.assertEqual(victim.read_text(encoding="utf-8"), "do not overwrite\n")

    def test_manifest_file_and_parent_symlinks_are_rejected_before_read(self) -> None:
        file_link = self.root / "manifest-link.toml"
        file_link.symlink_to(self.manifest)
        parent_link = self.root / "manifest-parent-link"
        parent_link.symlink_to(self.manifest.parent, target_is_directory=True)

        for manifest in (file_link, parent_link / self.manifest.name):
            with self.subTest(manifest=manifest):
                with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
                    DRIVER.load_manifest(self.root, manifest)

    def test_output_parent_symlink_is_rejected_at_load_and_before_census(self) -> None:
        generated = self.generate()
        victim = self.root / "src/rrr/generated-victim"
        victim.mkdir(parents=True)
        marker = victim / "marker"
        marker.write_text("do not touch\n", encoding="utf-8")
        (self.root / "src/rrr/src").symlink_to(
            victim, target_is_directory=True
        )

        with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
            DRIVER.load_manifest(self.root, self.manifest)
        with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
            DRIVER.apply_mode(self.root, generated, "write")
        self.assertEqual(marker.read_text(encoding="utf-8"), "do not touch\n")
        self.assertEqual(sorted(path.name for path in victim.iterdir()), ["marker"])

    def test_manifest_rejects_empty_input_and_block_ids(self) -> None:
        cases = [
            ("input = []", "input must be a non-empty"),
            (
                "[[module.input]]\n"
                "source = \"src/rrr/rpc/example.cpp\"\n"
                "block_ids = []",
                "block_ids must be a non-empty",
            ),
            (
                "[[module.input]]\n"
                "source = \"src/rrr/rpc/example.cpp\"\n"
                "block_ids = [\"example.1\", \"example.1\"]",
                "contains duplicate",
            ),
        ]
        for input_body, diagnostic in cases:
            with self.subTest(input_body=input_body):
                self.write_manifest(
                    f"""\
                    schema_version = 1
                    [[module]]
                    cpp_module = "rrr.example"
                    output = "src/rrr/src/example.rs"
                    {input_body}
                    """
                )
                with self.assertRaisesRegex(DRIVER.ExtractionError, diagnostic):
                    DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_reserves_generated_lib_from_module_ownership(self) -> None:
        self.write_manifest(
            """\
            schema_version = 1
            [[module]]
            cpp_module = "rrr.lib"
            output = "src/rrr/src/lib.rs"
            [[module.input]]
            source = "src/rrr/rpc/example.cpp"
            block_ids = ["example.1"]
            """
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "lib.rs is reserved"):
            DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_module_source_and_output_mismatches(self) -> None:
        cases = [
            (
                "rrr.other",
                "src/rrr/src/other.rs",
                "src/rrr/rpc/example.cpp",
                "example.1",
                "interface source .* must contain exactly",
            ),
            (
                "rrr.example",
                "src/rrr/src/wrong.rs",
                "src/rrr/rpc/example.cpp",
                "example.1",
                "output does not match cpp_module",
            ),
            (
                "rrr.example",
                "src/rrr/src/example.rs",
                "src/rrr/rpc/example_impl.cc",
                "example.impl",
                "interface source .* must contain exactly",
            ),
        ]
        for cpp_module, output, source, block_id, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                self.write_manifest(
                    f"""\
                    schema_version = 1
                    [[module]]
                    cpp_module = "{cpp_module}"
                    output = "{output}"
                    [[module.input]]
                    source = "{source}"
                    block_ids = ["{block_id}"]
                    """
                )
                with self.assertRaisesRegex(DRIVER.ExtractionError, diagnostic):
                    DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_restricts_sources_to_real_production_roots(self) -> None:
        outside = self.root / "src/rrr/tests/example.cpp"
        outside.parent.mkdir(parents=True)
        outside.write_bytes(self.interface.read_bytes())
        self.write_manifest(
            """\
            schema_version = 1
            [[module]]
            cpp_module = "rrr.example"
            output = "src/rrr/src/example.rs"
            [[module.input]]
            source = "src/rrr/tests/example.cpp"
            block_ids = ["example.1"]
            """
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "approved production"):
            DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_source_file_and_parent_symlinks(self) -> None:
        source_link = self.root / "src/rrr/rpc/source_link.cpp"
        source_link.symlink_to("example.cpp")
        parent_link = self.root / "src/rrr/base"
        parent_link.symlink_to("rpc", target_is_directory=True)
        cases = [
            "src/rrr/rpc/source_link.cpp",
            "src/rrr/base/example.cpp",
        ]
        for source in cases:
            with self.subTest(source=source):
                self.write_manifest(
                    f"""\
                    schema_version = 1
                    [[module]]
                    cpp_module = "rrr.example"
                    output = "src/rrr/src/example.rs"
                    [[module.input]]
                    source = "{source}"
                    block_ids = ["example.1"]
                    """
                )
                with self.assertRaisesRegex(DRIVER.ExtractionError, "symlink"):
                    DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_wrong_implementation_module(self) -> None:
        wrong = self.root / "src/rrr/rpc/wrong_impl.cc"
        wrong.write_text("module rrr.other;\n", encoding="utf-8")
        self.write_manifest(
            """\
            schema_version = 1
            [[module]]
            cpp_module = "rrr.example"
            output = "src/rrr/src/example.rs"
            [[module.input]]
            source = "src/rrr/rpc/example.cpp"
            block_ids = ["example.1"]
            [[module.input]]
            source = "src/rrr/rpc/wrong_impl.cc"
            block_ids = ["wrong.1"]
            """
        )
        with self.assertRaisesRegex(DRIVER.ExtractionError, "implementation source"):
            DRIVER.load_manifest(self.root, self.manifest)

    def test_manifest_rejects_duplicate_module_source_and_block_ownership(self) -> None:
        other = self.root / "src/rrr/rpc/other.cpp"
        other.write_text("export module rrr.other;\n", encoding="utf-8")
        cases = [
            (
                "rrr.example",
                "src/rrr/src/example.rs",
                "src/rrr/rpc/other.cpp",
                "other.1",
                "duplicate cpp_module ownership",
            ),
            (
                "rrr.other",
                "src/rrr/src/other.rs",
                "src/rrr/rpc/example.cpp",
                "other.1",
                "duplicate source ownership",
            ),
            (
                "rrr.other",
                "src/rrr/src/other.rs",
                "src/rrr/rpc/other.cpp",
                "example.1",
                "block ID .* already owned",
            ),
        ]
        for cpp_module, output, source, block_id, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                self.write_manifest(
                    f"""\
                    schema_version = 1
                    [[module]]
                    cpp_module = "rrr.example"
                    output = "src/rrr/src/example.rs"
                    [[module.input]]
                    source = "src/rrr/rpc/example.cpp"
                    block_ids = ["example.1"]
                    [[module]]
                    cpp_module = "{cpp_module}"
                    output = "{output}"
                    [[module.input]]
                    source = "{source}"
                    block_ids = ["{block_id}"]
                    """
                )
                with self.assertRaisesRegex(DRIVER.ExtractionError, diagnostic):
                    DRIVER.load_manifest(self.root, self.manifest)

    def test_toolchain_verification_fails_closed_on_git_drift(self) -> None:
        required = DRIVER.REQUIRED_RUSTY_CPP_COMMIT
        gitlink = f"160000 {required} 0 third-party/rusty-cpp"
        cases = [
            (["160000 deadbeef 0 third-party/rusty-cpp"], [], "gitlink pin"),
            ([gitlink, "deadbeef"], [], "submodule HEAD"),
            ([gitlink, required, " M transpiler/src/main.rs"], [], "local changes"),
        ]
        for git_results, _, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                with mock.patch.object(
                    DRIVER, "git_output", side_effect=git_results
                ):
                    with self.assertRaisesRegex(
                        DRIVER.ExtractionError, diagnostic
                    ):
                        DRIVER.verify_pinned_toolchain(self.root, self.fake)

    def test_toolchain_verification_requires_exact_clean_build_info(self) -> None:
        required = DRIVER.REQUIRED_RUSTY_CPP_COMMIT
        gitlink = f"160000 {required} 0 third-party/rusty-cpp"
        cases = [
            (
                subprocess_result(2, "", "unsupported"),
                "build-info failed",
            ),
            (subprocess_result(0, "", ""), "exactly one JSON line"),
            (subprocess_result(0, "not-json\n", ""), "invalid JSON"),
            (subprocess_result(0, "{}\n", ""), "keys must be exactly"),
            (
                subprocess_result(
                    0,
                    json.dumps({"git_hash": "0" * 40, "git_dirty": False})
                    + "\n",
                    "",
                ),
                "build commit mismatch",
            ),
            (
                subprocess_result(
                    0,
                    json.dumps({"git_hash": required, "git_dirty": True}) + "\n",
                    "",
                ),
                "git_dirty=false",
            ),
            (
                subprocess_result(
                    0,
                    json.dumps({"git_hash": required, "git_dirty": "false"})
                    + "\n",
                    "",
                ),
                "git_dirty=false",
            ),
        ]
        for completed, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                with mock.patch.object(
                    DRIVER,
                    "git_output",
                    side_effect=[gitlink, required, ""],
                ), mock.patch.object(
                    DRIVER.subprocess, "run", return_value=completed
                ):
                    with self.assertRaisesRegex(
                        DRIVER.ExtractionError, diagnostic
                    ):
                        DRIVER.verify_pinned_toolchain(self.root, self.fake)

        good = subprocess_result(
            0,
            json.dumps({"git_hash": required, "git_dirty": False}) + "\n",
            "",
        )
        with mock.patch.object(
            DRIVER,
            "git_output",
            side_effect=[gitlink, required, ""],
        ), mock.patch.object(DRIVER.subprocess, "run", return_value=good) as run:
            DRIVER.verify_pinned_toolchain(self.root, self.fake)
        run.assert_called_once_with(
            [str(self.fake), "--build-info"],
            cwd=self.root,
            text=True,
            stdout=DRIVER.subprocess.PIPE,
            stderr=DRIVER.subprocess.PIPE,
            check=False,
        )


class GoalZeroConsumerSelectionTests(unittest.TestCase):
    """The Goal-0 job builds a list of consumer targets and then runs a ctest
    pattern naming the same tests. Both lists were left naming `src/rrr/tests/`
    binaries after 4a06ef0e deleted them, which failed the job with
    `ninja: error: unknown target 'test_timer'`. Pin the two properties that
    turn that class of drift into a local failure instead of a red CI run.
    """

    @staticmethod
    def goal_zero_steps() -> tuple[str, str]:
        workflow = (REPOSITORY / ".github/workflows/ci.yml").read_text(
            encoding="utf-8"
        )
        build = re.search(r"--target ([^\n]*?)\n\s*-- -k 0", workflow)
        assert build is not None, "Goal-0 build step lost its --target list"
        selection = re.search(r"-R '\^\((.*?)\)\$'", workflow)
        assert selection is not None, "Goal-0 ctest step lost its -R pattern"
        return build.group(1), selection.group(1)

    def test_built_targets_and_selected_tests_agree(self) -> None:
        built, selected = self.goal_zero_steps()
        targets = [
            target
            for target in built.split()
            if target != "rrr_goal0_dual_compile"
        ]
        self.assertTrue(targets, "Goal-0 builds no consumer targets")
        self.assertEqual(sorted(targets), sorted(selected.split("|")))

    def test_selected_tests_are_targets_this_repository_defines(self) -> None:
        """The exact check the broken workflow would have failed: every name
        must be an `add_executable` in mako's own CMakeLists."""
        _, selected = self.goal_zero_steps()
        cmake = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")
        defined = set(re.findall(r"add_executable\(\s*([A-Za-z0-9_]+)", cmake))
        for name in selected.split("|"):
            with self.subTest(target=name):
                self.assertIn(name, defined)


class ForeignOwnedCheckoutTests(unittest.TestCase):
    """The pin attestation runs in a container whose checkout belongs to a
    different uid than the process, so bare `git` answers "detected dubious
    ownership" instead of reading the repository. `GIT_TEST_ASSUME_DIFFERENT_-
    OWNER` reproduces exactly that condition without needing a second uid.
    """

    # Build the fixture with the knob explicitly OFF. These tests must *own*
    # the ownership condition, not inherit it: if the variable is already set
    # in the environment (e.g. someone reproducing a CI failure by exporting
    # it for a whole gate run), bare git would disown even the scratch repo
    # this setUp just created and every test here would error in setUp instead
    # of testing anything.
    NATIVE_ENV = {
        key: value
        for key, value in os.environ.items()
        if key != "GIT_TEST_ASSUME_DIFFERENT_OWNER"
    }

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="rrr-ownership-")
        self.addCleanup(temporary.cleanup)
        self.repository = Path(temporary.name) / "checkout"
        self.repository.mkdir()
        for arguments in (
            ["init", "--quiet", "."],
            ["config", "user.name", "gate"],
            ["config", "user.email", "gate@example.invalid"],
            ["commit", "--quiet", "--allow-empty", "-m", "seed"],
        ):
            subprocess.run(
                ["git", *arguments],
                cwd=self.repository,
                env=self.NATIVE_ENV,
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        self.head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.repository,
            env=self.NATIVE_ENV,
            text=True,
            stdout=subprocess.PIPE,
            check=True,
        ).stdout.strip()

    def test_bare_git_really_is_refused(self) -> None:
        """Guard the guard: if this ever stops failing, the tests below stop
        proving anything, because they would pass without the exception."""
        completed = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=self.repository,
            env=dict(os.environ, GIT_TEST_ASSUME_DIFFERENT_OWNER="1"),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("dubious ownership", completed.stderr)

    def test_git_output_reads_a_foreign_owned_checkout(self) -> None:
        with mock.patch.dict(
            os.environ, {"GIT_TEST_ASSUME_DIFFERENT_OWNER": "1"}
        ):
            for module in (DRIVER, GATE):
                with self.subTest(module=module.__name__):
                    self.assertEqual(
                        module.git_output(
                            self.repository, ["rev-parse", "HEAD"], "probe"
                        ),
                        self.head,
                    )

    def test_ownership_exception_names_only_the_inspected_directory(self) -> None:
        flags = DRIVER.ownership_exception(self.repository)
        self.assertEqual(flags[::2], ["-c"] * (len(flags) // 2))
        self.assertEqual(
            {flag.removeprefix("safe.directory=") for flag in flags[1::2]},
            {str(self.repository), str(self.repository.resolve())},
        )
        # A blanket "trust everything" opt-out would also silence genuine
        # ownership problems in unrelated repositories.
        self.assertNotIn("safe.directory=*", flags)

    def test_repository_scripts_that_shell_out_to_git_survive(self) -> None:
        """Every script the source gate runs must tolerate a foreign-owned
        checkout, not just the pin attestation. `rrr_handwritten_census.py`
        did not, and CI died with `git ls-files ... exit status 128` once the
        attestation stopped failing first and stopped masking it."""
        if not (REPOSITORY / ".git").exists():
            self.skipTest("not a git checkout")
        completed = subprocess.run(
            [sys.executable, "scripts/rrr_handwritten_census.py"],
            cwd=REPOSITORY,
            env=dict(os.environ, GIT_TEST_ASSUME_DIFFERENT_OWNER="1"),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(
            completed.returncode, 0, msg=completed.stdout + completed.stderr
        )
        self.assertIn("source boundary:", completed.stdout)

    def test_pin_attestation_still_fails_closed_on_a_foreign_checkout(self) -> None:
        """Relaxing git's ownership heuristic must not relax the pin itself:
        against the real repository, a wrong required commit is still caught."""
        if not (REPOSITORY / ".git").exists():
            self.skipTest("not a git checkout")
        cases = (
            (DRIVER, DRIVER.ExtractionError),
            (GATE, GATE.GateError),
        )
        with mock.patch.dict(
            os.environ, {"GIT_TEST_ASSUME_DIFFERENT_OWNER": "1"}
        ):
            for module, failure in cases:
                with self.subTest(module=module.__name__):
                    with mock.patch.object(
                        module, "REQUIRED_RUSTY_CPP_COMMIT", "0" * 40
                    ):
                        with self.assertRaisesRegex(
                            failure, "gitlink pin mismatch"
                        ):
                            module.verify_pinned_toolchain(
                                REPOSITORY, Path("/nonexistent-transpiler")
                            )


class CrateCodegenAssertionTests(unittest.TestCase):
    """`scripts/ci/assert_crate_codegen.sh` replaced the CI step's output-mtime
    assertions, which were measuring "the file was rewritten" -- only true by
    accident, since the emitters skip writing byte-identical output. These
    tests pin the replacement, and in particular that it is not weaker: it must
    still FAIL when regeneration genuinely does not happen.
    """

    SCRIPT = REPOSITORY / "scripts/ci/assert_crate_codegen.sh"

    # A faithful sample of what `cmake --build --target rrr_goal0_crate_codegen`
    # prints when it really regenerates (trimmed to the lines that matter).
    REGENERATED = """\
[1/4] Building rusty-cpp-transpiler...
   Compiling rusty-cpp-transpiler v0.1.0 (/w/third-party/rusty-cpp/transpiler)
    Finished `release` profile [optimized] target(s) in 6m 23s
[2/4] Fingerprinting the Goal-0 rusty-cpp emitter
[3/4] Generating Goal-0 rrr crate C++ child modules
Transpiling crate 'rrr' (38 source files)
  src/frame_codec.rs → rrr.frame_codec.cppm (module: rrr.frame_codec)
  src/utils.rs → rrr.utils.cppm (module: rrr.utils)
  src/request_queue.rs → rrr.request_queue.cppm (module: rrr.request_queue)
  src/load_balancer.rs → rrr.load_balancer.cppm (module: rrr.load_balancer)
Done: 38 files transpiled, 0 errors
"""

    # What a no-op build prints: ninja has nothing to do, so the codegen edge
    # never fires and none of the generator's own output appears.
    NOT_REGENERATED = "ninja: no work to do.\n"

    def assert_script(self, stdin: str, *generated: str):
        # Invoked through `bash` on purpose: this repo has core.fileMode=false,
        # so a `chmod +x` in a worktree is silently NOT recorded and the script
        # lands in git as 100644. Relying on the exec bit made CI die with
        # PermissionError. The mode is now 100755 as well, but not depended on.
        return subprocess.run(
            ["bash", str(self.SCRIPT), *generated],
            input=stdin,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_passes_when_regeneration_reached_every_named_output(self) -> None:
        completed = self.assert_script(
            self.REGENERATED,
            "rrr.request_queue.cppm",
            "rrr.load_balancer.cppm",
            "rrr.utils.cppm",
            "rrr.frame_codec.cppm",
        )
        self.assertEqual(
            completed.returncode, 0, msg=completed.stdout + completed.stderr
        )

    def test_fails_when_regeneration_did_not_happen(self) -> None:
        """THE point of the gate. If this ever passes, stale generated C++
        ships silently and the whole step is decoration."""
        completed = self.assert_script(
            self.NOT_REGENERATED, "rrr.frame_codec.cppm"
        )
        self.assertEqual(completed.returncode, 1)
        self.assertIn("crate generation did not re-run", completed.stderr)

    def test_fails_when_the_generator_never_reached_the_output(self) -> None:
        """Regeneration ran but skipped the file we care about -- exactly the
        drift an mtime check cannot distinguish from success."""
        completed = self.assert_script(
            self.REGENERATED, "rrr.serializable.cppm"
        )
        self.assertEqual(completed.returncode, 1)
        self.assertIn(
            "generator did not report rrr.serializable.cppm", completed.stderr
        )

    def test_fails_when_generation_did_not_finish_cleanly(self) -> None:
        broken = self.REGENERATED.replace(
            "Done: 38 files transpiled, 0 errors",
            "Done: 38 files transpiled, 2 errors",
        )
        completed = self.assert_script(broken, "rrr.frame_codec.cppm")
        self.assertEqual(completed.returncode, 1)
        self.assertIn("did not finish cleanly", completed.stderr)

    def test_workflow_uses_the_script_and_no_output_mtime_assertions(self) -> None:
        workflow = (REPOSITORY / ".github/workflows/ci.yml").read_text(
            encoding="utf-8"
        )
        step = workflow.split(
            "Verify emitter and canonical Rust invalidate crate generation"
        )[1].split("Run focused production consumers")[0]
        self.assertIn("scripts/ci/assert_crate_codegen.sh", step)
        # The bug class: comparing generated-output timestamps. Nothing in the
        # step may go back to it. Scan code only -- the comments explain the
        # old `stat -c %Y` assertions and naming them is the point.
        code = "\n".join(
            line
            for line in step.splitlines()
            if not line.lstrip().startswith("#")
        )
        self.assertNotIn("stat -c %Y", code)
        # The steady check -- "a no-op build must NOT regenerate" -- is the one
        # assertion that was always correct, and must survive.
        self.assertIn(
            "crate generation reran without an emitter or source change", step
        )


class ArchiverResolutionTests(unittest.TestCase):
    """The gate needs llvm-ar from the same toolchain as its nm. It used to
    demand a bare `llvm-ar` beside nm, which the CI image does not have:
    apt.llvm.org installs version-suffixed binaries and only aliases
    clang/clang++/llvm-config, so `/usr/bin/llvm-nm-22` sits next to
    `llvm-ar-22` and the gate died with "ar is unavailable: /usr/bin/llvm-ar".
    """

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="rrr-archiver-")
        self.addCleanup(temporary.cleanup)
        self.bindir = Path(temporary.name)

    def tool(self, name: str) -> Path:
        path = self.bindir / name
        path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        path.chmod(0o755)
        return path

    def test_suffixed_toolchain_resolves_the_matching_archiver(self) -> None:
        """The exact CI layout: llvm-nm-22 beside llvm-ar-22, no plain
        llvm-ar. This is the case that was failing."""
        nm = self.tool("llvm-nm-22")
        expected = self.tool("llvm-ar-22")
        self.assertFalse((self.bindir / "llvm-ar").exists())
        self.assertEqual(GATE.resolve_archiver(REPOSITORY, nm), expected)

    def test_unsuffixed_toolchain_still_resolves(self) -> None:
        """The Homebrew-style layout that used to be the only one handled."""
        nm = self.tool("llvm-nm")
        expected = self.tool("llvm-ar")
        self.assertEqual(GATE.resolve_archiver(REPOSITORY, nm), expected)

    def test_suffixed_nm_prefers_the_suffixed_archiver(self) -> None:
        """With both spellings present, stay within one toolchain version."""
        nm = self.tool("llvm-nm-22")
        self.tool("llvm-ar")
        expected = self.tool("llvm-ar-22")
        self.assertEqual(GATE.resolve_archiver(REPOSITORY, nm), expected)

    def test_version_suffix_is_read_off_nm_never_hard_coded(self) -> None:
        """Any suffix, not just -22, so a toolchain bump needs no edit here."""
        for suffix in ("-19", "-22", "-30", "-22.1"):
            with self.subTest(suffix=suffix):
                bindir = self.bindir / f"tc{suffix}"
                bindir.mkdir()
                made = {}
                for stem in (f"llvm-nm{suffix}", f"llvm-ar{suffix}"):
                    path = bindir / stem
                    path.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
                    path.chmod(0o755)
                    made[stem] = path
                self.assertEqual(
                    GATE.resolve_archiver(REPOSITORY, made[f"llvm-nm{suffix}"]),
                    made[f"llvm-ar{suffix}"],
                )

    def test_falls_back_to_path_when_nothing_sits_beside_nm(self) -> None:
        nm = self.tool("llvm-nm-22")
        (self.bindir / "llvm-ar-22").unlink(missing_ok=True)
        elsewhere = self.bindir / "onpath"
        elsewhere.mkdir()
        expected = elsewhere / "llvm-ar-22"
        expected.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        expected.chmod(0o755)
        with mock.patch.object(
            GATE.shutil,
            "which",
            side_effect=lambda name: (
                str(expected) if name == "llvm-ar-22" else None
            ),
        ):
            self.assertEqual(GATE.resolve_archiver(REPOSITORY, nm), expected)

    def test_missing_archiver_fails_closed_naming_every_path_tried(self) -> None:
        """No archiver anywhere must stay a hard error, and the diagnostic has
        to say what it looked for -- that is the whole value of the message."""
        nm = self.tool("llvm-nm-22")
        with mock.patch.object(GATE.shutil, "which", return_value=None):
            with self.assertRaises(GATE.GateError) as caught:
                GATE.resolve_archiver(REPOSITORY, nm)
        message = str(caught.exception)
        self.assertIn("ar is unavailable", message)
        self.assertIn(str(self.bindir / "llvm-ar-22"), message)
        self.assertIn(str(self.bindir / "llvm-ar"), message)
        for name in ("llvm-ar-22 (PATH)", "llvm-ar (PATH)", "ar (PATH)"):
            self.assertIn(name, message)


class CrateModeGateTests(unittest.TestCase):
    def test_frame_codec_io_preamble_is_rejected_from_siblings(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-preamble-") as temporary:
            output = Path(temporary)
            (output / "CMakeLists.txt").write_text(
                "# generated\n", encoding="utf-8"
            )
            (output / "rrr.frame_codec.cppm").write_text(
                "// generated\nmodule;\n"
                "#include <vector>\n"
                "#include <rusty/io.hpp>\n"
                "#include <cstdint>\n"
                "export module rrr.frame_codec;\n"
                "import rrr.internal_protocol;\n",
                encoding="utf-8",
            )
            (output / "rrr.example.cppm").write_text(
                "module;\n"
                "#include <rusty/io.hpp>\n"
                "export module rrr.example;\n",
                encoding="utf-8",
            )
            (output / "rrr.cppm").write_text(
                "export module rrr;\n"
                "namespace rrr {\n"
                "export import rrr.frame_codec;\n"
                "export import rrr.example;\n",
                encoding="utf-8",
            )
            modules = [
                mock.Mock(cpp_module="rrr.frame_codec"),
                mock.Mock(cpp_module="rrr.example"),
            ]
            specs = {
                "rrr.frame_codec": GATE.AbiSpec(frozenset(), frozenset()),
                "rrr.example": GATE.AbiSpec(frozenset(), frozenset()),
            }
            with mock.patch.dict(GATE.ABI_SPECS, specs, clear=True):
                with self.assertRaisesRegex(
                    GATE.GateError,
                    "FrameCodec io preamble leaked into rrr.example",
                ):
                    GATE.require_cpp_surfaces(
                        Path("/repository"), output, modules
                    )

    def test_utils_preamble_is_rejected_from_sibling_children(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-preamble-") as temporary:
            output = Path(temporary)
            (output / "CMakeLists.txt").write_text("# generated\n", encoding="utf-8")
            (output / "rrr.utils.cppm").write_text(
                "// generated\nmodule;\n"
                "#include <netdb.h>\n"
                "#include <cstdint>\n"
                "export module rrr.utils;\n"
                "import rrr.logging;\n",
                encoding="utf-8",
            )
            (output / "rrr.example.cppm").write_text(
                "// generated\nmodule;\n"
                "#include <netdb.h>\n"
                "export module rrr.example;\n",
                encoding="utf-8",
            )
            (output / "rrr.cppm").write_text(
                "export module rrr;\n"
                "namespace rrr {\n"
                "export import rrr.utils;\n"
                "export import rrr.example;\n",
                encoding="utf-8",
            )
            modules = [
                mock.Mock(cpp_module="rrr.utils"),
                mock.Mock(cpp_module="rrr.example"),
            ]
            specs = {
                "rrr.utils": GATE.AbiSpec(frozenset(), frozenset()),
                "rrr.example": GATE.AbiSpec(frozenset(), frozenset()),
            }
            with mock.patch.dict(GATE.ABI_SPECS, specs, clear=True):
                with self.assertRaisesRegex(
                    GATE.GateError,
                    "utils netdb preamble leaked into rrr.example",
                ):
                    GATE.require_cpp_surfaces(Path("/repository"), output, modules)

    def test_placeholder_ratchet_checks_named_module_purview(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-placeholder-") as temporary:
            generated = Path(temporary) / "rrr.example.cppm"
            generated.write_text(
                "module;\n"
                "// Compiler runtime diagnostic: unsupported conversion.\n"
                "export module rrr.example;\n"
                "export int value();\n",
                encoding="utf-8",
            )
            self.assertIn(
                "export module rrr.example;",
                GATE.read_generated(generated, "test module"),
            )

            generated.write_text(
                "module;\n"
                "// Compiler runtime diagnostic: unsupported conversion.\n"
                "export module rrr.example;\n"
                "// TODO: lower this declaration.\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(GATE.GateError, "placeholder marker 'TODO'"):
                GATE.read_generated(generated, "test module")

    def test_generated_children_have_only_their_direct_module_imports(self) -> None:
        GATE.require_exact_module_imports(
            "export module rrr.rand;\nimport rusty;\n",
            "rrr.rand",
            ["rusty"],
        )
        GATE.require_exact_module_imports(
            "export module rrr.request_options;\nimport rrr.rand;\n",
            "rrr.request_options",
            ["rrr.rand"],
        )
        GATE.require_exact_module_imports(
            "export module rrr.reconnect_policy;\nimport rrr.rand;\n",
            "rrr.reconnect_policy",
            ["rrr.rand"],
        )
        GATE.require_exact_module_imports(
            "export module rrr.circuit_breaker;\n",
            "rrr.circuit_breaker",
            [],
        )
        GATE.require_exact_module_imports(
            "export module rrr.basetypes;\n",
            "rrr.basetypes",
            [],
        )
        GATE.require_exact_module_imports(
            "export module rrr.request_queue;\n"
            "import rusty;\n"
            "import rrr.circuit_breaker;\n",
            "rrr.request_queue",
            ["rusty", "rrr.circuit_breaker"],
        )
        GATE.require_exact_module_imports(
            "export module rrr.connection_state;\n",
            "rrr.connection_state",
            [],
        )
        GATE.require_exact_module_imports(
            "export module rrr.heartbeat;\nimport rrr.circuit_breaker;\n",
            "rrr.heartbeat",
            ["rrr.circuit_breaker"],
        )
        GATE.require_exact_module_imports(
            "export module rrr.load_balancer;\n",
            "rrr.load_balancer",
            [],
        )
        GATE.require_exact_module_imports(
            "export module rrr.frame_codec;\n"
            "import rrr.internal_protocol;\n",
            "rrr.frame_codec",
            ["rrr.internal_protocol"],
        )
        with self.assertRaisesRegex(GATE.GateError, "private imports must be exactly"):
            GATE.require_exact_module_imports(
                "export module rrr.rand;\n"
                "import rusty;\n"
                "import rrr.debugging;\n",
                "rrr.rand",
                ["rusty"],
            )
        with self.assertRaisesRegex(GATE.GateError, "exported=\\['rrr.rand'\\]"):
            GATE.require_exact_module_imports(
                "export module rrr.request_options;\n"
                "export import rrr.rand;\n",
                "rrr.request_options",
                ["rrr.rand"],
            )

    def test_executable_preserves_cxx_driver_symlink_spelling(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-driver-test-") as temporary:
            root = Path(temporary)
            real_driver = root / "clang-22"
            real_driver.write_text("", encoding="utf-8")
            real_driver.chmod(0o755)
            cxx_driver = root / "clang++"
            cxx_driver.symlink_to(real_driver.name)

            self.assertEqual(
                GATE.executable(root, str(cxx_driver), "Clang C++ compiler"),
                cxx_driver,
            )

    def test_symbol_owner_ignores_module_attachments_in_types(self) -> None:
        cases = {
            "rrr::f@rrr.errors(rrr::RpcError@rrr.errors)": "rrr.errors",
            "rrr::AvgStat@rrr.stat::avg() const": "rrr.stat",
            "rrr::value@rrr.internal_protocol": "rrr.internal_protocol",
            (
                "rrr::Facade<rrr::RpcError@rrr.errors>"
                "@rrr.client::call()"
            ): "rrr.client",
            "rrr::Facade<rrr::RpcError@rrr.errors>::call()": None,
            (
                "rrr::RpcError@rrr.errors "
                "rusty::clone<rrr::RpcError@rrr.errors>(int)"
            ): None,
            "rrr::Facade@rrr.types<int> rrr::make@rrr.errors<int>()": "rrr.errors",
            "rrr::operator<@rrr.errors(rrr::RpcError@rrr.errors, rrr::RpcError@rrr.errors)": "rrr.errors",
            "rrr::Widget@rrr.errors::operator bool() const": "rrr.errors",
            "rrr::Widget@rrr.errors::operator rrr::Facade@rrr.types<int>() const": "rrr.errors",
            "rrr::plain(int)": None,
        }
        for symbol, expected in cases.items():
            with self.subTest(symbol=symbol):
                self.assertEqual(GATE.symbol_owner_module(symbol), expected)

    def test_module_symbols_ratchets_only_strong_owned_definitions(self) -> None:
        output = "\n".join(
            [
                "0001 T rrr::f@rrr.errors(rrr::RpcError@rrr.errors)",
                "0002 W rrr::helper@rrr.errors()",
                "0003 t rrr::local@rrr.errors()",
                "0004 T rrr::other@rrr.client(rrr::RpcError@rrr.errors)",
                "0005 W rrr::RpcError@rrr.errors rusty::clone<int>(int)",
            ]
        )
        with mock.patch.object(GATE, "run", return_value=output):
            symbols = GATE.module_symbols(
                Path("/usr/bin/nm"),
                Path("/repository"),
                Path("/repository/librrr.a"),
                "rrr.errors",
            )
        self.assertEqual(
            symbols,
            {("T", "rrr::f@rrr.errors(rrr::RpcError@rrr.errors)")},
        )

    def test_symbol_census_uses_the_definition_owner_not_parameter_types(self) -> None:
        owned = (
            "rrr::ConnectionMetrics@rrr.connection_metrics::reset() const"
        )
        foreign = (
            "rrr::Client@rrr.client::Client("
            "rrr::ConnectionMetrics@rrr.connection_metrics)"
        )
        nm_output = f"0001 T {owned}\n0002 T {foreign}\n"
        with mock.patch.object(GATE, "run", return_value=nm_output):
            self.assertEqual(
                GATE.module_symbols(
                    Path("/nm"),
                    Path("/repository"),
                    Path("/library.a"),
                    "rrr.connection_metrics",
                ),
                {("T", owned)},
            )

    def test_gate_rechecks_extracted_rust_with_the_same_transpiler(self) -> None:
        root = Path("/repository")
        transpiler = Path("/tools/rusty-cpp-transpiler")
        with mock.patch.object(GATE, "run") as run:
            GATE.require_extraction_check(root, transpiler)
        run.assert_called_once_with(
            [
                sys.executable,
                GATE.EXTRACTION_DRIVER,
                "--check",
                "--transpiler",
                str(transpiler),
            ],
            root,
        )

    def test_extraction_drift_stops_gate_before_translation_setup(self) -> None:
        root = Path("/repository")
        transpiler = Path("/tools/rusty-cpp-transpiler")
        args = mock.Mock(
            transpiler=str(transpiler),
            clang="clang++",
            nm="nm",
        )
        with mock.patch.object(
            GATE, "repository_root", return_value=root
        ), mock.patch.object(
            GATE, "executable", return_value=transpiler
        ) as executable, mock.patch.object(
            GATE, "verify_pinned_toolchain"
        ) as verify, mock.patch.object(
            GATE,
            "require_extraction_check",
            side_effect=GATE.GateError("generated Rust output is stale"),
        ) as extraction:
            with self.assertRaisesRegex(GATE.GateError, "stale"):
                GATE.check(args)
        executable.assert_called_once_with(
            root, str(transpiler), "rusty-cpp transpiler"
        )
        verify.assert_called_once_with(root, transpiler)
        extraction.assert_called_once_with(root, transpiler)

    def test_gate_requires_exact_clean_transpiler_build_info(self) -> None:
        required = GATE.REQUIRED_RUSTY_CPP_COMMIT
        gitlink = f"160000 {required} 0 third-party/rusty-cpp"
        dirty = subprocess_result(
            0,
            json.dumps({"git_hash": required, "git_dirty": True}) + "\n",
            "",
        )
        with mock.patch.object(
            GATE,
            "git_output",
            side_effect=[gitlink, required, ""],
        ), mock.patch.object(GATE.subprocess, "run", return_value=dirty):
            with self.assertRaisesRegex(GATE.GateError, "git_dirty=false"):
                GATE.verify_pinned_toolchain(Path("/repository"), Path("/tool"))

        clean = subprocess_result(
            0,
            json.dumps({"git_hash": required, "git_dirty": False}) + "\n",
            "",
        )
        with mock.patch.object(
            GATE,
            "git_output",
            side_effect=[gitlink, required, ""],
        ), mock.patch.object(GATE.subprocess, "run", return_value=clean):
            GATE.verify_pinned_toolchain(Path("/repository"), Path("/tool"))

    def test_build_tree_output_is_reused_without_second_generation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-reuse-test-") as temporary:
            root = Path(temporary)
            generated = root / "generated"
            generated.mkdir()
            production = root / "production.a"
            production.touch()
            args = GATE.argparse.Namespace(
                transpiler="transpiler",
                clang="clang++",
                nm="nm",
                production_library=str(production),
                generated_dir=str(generated),
                runtime_library=[],
                runtime_module_root=[],
                cxx_flag=["-stdlib=libc++"],
                link_flag=["-lc++abi"],
            )
            modules = [mock.Mock(cpp_module="rrr.internal_protocol")]
            with mock.patch.object(
                GATE, "repository_root", return_value=root
            ), mock.patch.object(
                GATE,
                "executable",
                side_effect=[
                    Path("/transpiler"),
                    Path("/clang++"),
                    Path("/nm"),
                ],
            ), mock.patch.object(
                # The archiver is no longer resolved through `executable`: it
                # is derived from nm's own spelling so a version-suffixed
                # toolchain (the CI image) finds llvm-ar-NN.
                GATE,
                "resolve_archiver",
                return_value=Path("/ar"),
            ), mock.patch.object(
                GATE, "verify_pinned_toolchain"
            ), mock.patch.object(
                GATE, "require_extraction_check"
            ), mock.patch.object(
                GATE, "load_owned_modules", return_value=modules
            ), mock.patch.object(
                GATE, "check_generated_output"
            ) as check_output, mock.patch.object(
                GATE, "run"
            ) as run:
                GATE.check(args)

            run.assert_not_called()
            check_output.assert_called_once_with(
                root=root,
                output=generated,
                modules=modules,
                clang=Path("/clang++"),
                nm=Path("/nm"),
                archiver=Path("/ar"),
                production=production,
                runtime_libraries=[],
                cxx_flags=["-stdlib=libc++"],
                link_flags=["-lc++abi"],
                prebuilt_module_dirs=[],
            )

    def test_standalone_generation_consumes_the_structured_preamble(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-gate-generate-test-") as temporary:
            root = Path(temporary) / ".." / Path(temporary).name
            args = GATE.argparse.Namespace(
                transpiler="transpiler",
                clang="clang++",
                nm="nm",
                production_library=None,
                generated_dir=None,
                runtime_library=[],
                runtime_module_root=[],
                cxx_flag=[],
                link_flag=[],
            )
            modules = [mock.Mock(cpp_module="rrr.internal_protocol")]
            with mock.patch.object(
                GATE, "repository_root", return_value=root
            ), mock.patch.object(
                GATE,
                "executable",
                side_effect=[
                    Path("/transpiler"),
                    Path("/clang++"),
                    Path("/nm"),
                ],
            ), mock.patch.object(
                # The archiver is no longer resolved through `executable`: it
                # is derived from nm's own spelling so a version-suffixed
                # toolchain (the CI image) finds llvm-ar-NN.
                GATE,
                "resolve_archiver",
                return_value=Path("/ar"),
            ), mock.patch.object(
                GATE, "verify_pinned_toolchain"
            ), mock.patch.object(
                GATE, "require_extraction_check"
            ), mock.patch.object(
                GATE, "load_owned_modules", return_value=modules
            ), mock.patch.object(
                GATE.extraction,
                "load_flat_import_namespace",
                return_value="rrr",
            ), mock.patch.object(
                GATE, "check_generated_output"
            ), mock.patch.object(GATE, "run") as run:
                GATE.check(args)

            command = run.call_args.args[0]
            # The crate-level flat-import namespace must reach the emitter:
            # without it the seventeen marker-free canonical sources lose the
            # `cpp_import_namespace` contract their private `use crate::...`
            # items now depend on.
            self.assertIn("--flat-import-namespace", command)
            self.assertEqual(
                command[command.index("--flat-import-namespace") + 1], "rrr"
            )
            self.assertEqual(
                command[-6:],
                [
                    "--module-preamble",
                    str(root / GATE.MODULE_PREAMBLE),
                    "--type-map",
                    str(root / GATE.TYPE_MAP),
                    "--cpp-module-index",
                    str(root / GATE.CPP_MODULE_INDEX),
                ],
            )
            self.assertEqual(
                command[2], str((root / "src/rrr/Cargo.toml").resolve())
            )
            self.assertTrue(Path(command[2]).is_absolute())

    def test_cmake_crate_invocation_uses_an_absolute_manifest_variable(self) -> None:
        cmake = (REPOSITORY / "src/rrr/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            'get_filename_component(\n'
            '    RRR_GOAL0_CRATE_MANIFEST\n'
            '    "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"\n'
            '    ABSOLUTE\n'
            ')',
            cmake,
        )
        self.assertIn('--crate "${RRR_GOAL0_CRATE_MANIFEST}"', cmake)
        self.assertNotIn(
            '--crate "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"', cmake
        )

    def test_generated_gate_compiles_children_before_partial_root(self) -> None:
        modules = [
            mock.Mock(cpp_module="rrr.basetypes"),
            mock.Mock(cpp_module="rrr.callback_wrapper"),
            mock.Mock(cpp_module="rrr.internal_protocol"),
            mock.Mock(cpp_module="rrr.stat"),
            mock.Mock(cpp_module="rrr.errors"),
            mock.Mock(cpp_module="rrr.connection_metrics"),
            mock.Mock(cpp_module="rrr.completion_tracker"),
            mock.Mock(cpp_module="rrr.rand"),
            mock.Mock(cpp_module="rrr.request_options"),
            mock.Mock(cpp_module="rrr.reconnect_policy"),
            mock.Mock(cpp_module="rrr.circuit_breaker"),
            mock.Mock(cpp_module="rrr.connection_state"),
            mock.Mock(cpp_module="rrr.heartbeat"),
            mock.Mock(cpp_module="rrr.request_queue"),
            mock.Mock(cpp_module="rrr.load_balancer"),
            mock.Mock(cpp_module="rrr.debugging"),
            mock.Mock(cpp_module="rrr.logging"),
            mock.Mock(cpp_module="rrr.utils"),
            mock.Mock(cpp_module="rrr.frame_codec"),
            mock.Mock(cpp_module="rrr.serializable"),
            mock.Mock(cpp_module="rrr.serializable_envelope"),
            mock.Mock(cpp_module="rrr.epoll_wrapper"),
            mock.Mock(cpp_module="rrr.misc"),
            mock.Mock(cpp_module="rrr.pollable_proxy"),
            mock.Mock(cpp_module="rrr.reactor"),
            mock.Mock(cpp_module="rrr.future"),
            mock.Mock(cpp_module="rrr.idempotency"),
            mock.Mock(cpp_module="rrr.fiber"),
            mock.Mock(cpp_module="rrr.channel"),
            mock.Mock(cpp_module="rrr.callbacks"),
            mock.Mock(cpp_module="rrr.inmemory_channel"),
            mock.Mock(cpp_module="rrr.fiber_channel"),
            mock.Mock(cpp_module="rrr.threading"),
            mock.Mock(cpp_module="rrr.any_message"),
            mock.Mock(cpp_module="rrr.tcp_channel"),
            mock.Mock(cpp_module="rrr.server"),
            mock.Mock(cpp_module="rrr.client"),
        ]

        def symbols_for_module(
            _nm: Path, _root: Path, path: Path, module_name: str
        ) -> frozenset[tuple[str, str]]:
            # rrr.epoll_wrapper follows Rust std's sys-module pattern: its
            # platform implementation unit is compiled into librrr.a but is not
            # a crate output, so the production library legitimately carries
            # ABI_SPECS plus exactly PLATFORM_IMPL_SYMBOLS. Model that here or
            # the union check below has nothing to check.
            symbols = set(GATE.ABI_SPECS[module_name].symbols)
            if path == Path("/production.a"):
                symbols |= GATE.PLATFORM_IMPL_SYMBOLS.get(
                    module_name, frozenset()
                )
            return frozenset(symbols)

        completion_raw = list(GATE.ABI_SPECS["rrr.completion_tracker"].symbols)
        completion_raw.append(
            ("T", "initializer for module rrr.completion_tracker")
        )
        rand_raw = list(GATE.ABI_SPECS["rrr.rand"].symbols)
        rand_raw.append(("T", "initializer for module rrr.rand"))
        request_options_raw = list(
            GATE.ABI_SPECS["rrr.request_options"].symbols
        )
        request_options_raw.append(
            ("T", "initializer for module rrr.request_options")
        )
        reconnect_policy_raw = list(
            GATE.ABI_SPECS["rrr.reconnect_policy"].symbols
        )
        reconnect_policy_raw.append(
            ("T", "initializer for module rrr.reconnect_policy")
        )
        circuit_breaker_raw = list(
            GATE.ABI_SPECS["rrr.circuit_breaker"].symbols
        )
        circuit_breaker_raw.append(
            ("T", "initializer for module rrr.circuit_breaker")
        )
        basetypes_raw = list(GATE.ABI_SPECS["rrr.basetypes"].symbols)
        basetypes_raw.append(("T", "initializer for module rrr.basetypes"))
        request_queue_raw = list(GATE.ABI_SPECS["rrr.request_queue"].symbols)
        request_queue_raw.append(
            ("T", "initializer for module rrr.request_queue")
        )
        exact_raw = {
            name: [
                *GATE.ABI_SPECS[name].symbols,
                ("T", f"initializer for module {name}"),
            ]
            for name in (
                "rrr.connection_state",
                "rrr.heartbeat",
                "rrr.load_balancer",
                "rrr.frame_codec",
            )
        }
        utils_raw = list(GATE.ABI_SPECS["rrr.utils"].symbols)
        for symbol in (
            "rrr::AddrInfo@rrr.utils::AddrInfo(addrinfo*, rusty::Cell<bool>)",
            "rrr::AddrInfo@rrr.utils::AddrInfo(rrr::AddrInfo@rrr.utils&&)",
            "rrr::AddrInfo@rrr.utils::~AddrInfo()",
        ):
            utils_raw.append(("T", symbol))
        utils_raw.append(("T", "initializer for module rrr.utils"))

        def compiled_object(
            _clang: Path,
            _root: Path,
            _include: Path,
            _source: Path,
            _work: Path,
            module_name: str,
            _cxx_flags: list[str],
            _prebuilt_module_dirs: list[Path],
        ) -> Path:
            return Path(f"/{module_name}.o")

        with tempfile.TemporaryDirectory(prefix="rrr-gate-children-test-") as temporary:
            output = Path(temporary)
            with mock.patch.object(
                GATE, "require_cpp_surfaces"
            ), mock.patch.object(
                GATE, "require_zero_hand_slots"
            ), mock.patch.object(
                GATE,
                "compile_module",
                side_effect=compiled_object,
            ) as compile_module, mock.patch.object(
                GATE, "run"
            ) as run, mock.patch.object(
                GATE, "module_symbols", side_effect=symbols_for_module
            ), mock.patch.object(
                GATE, "completion_raw_symbols", return_value=completion_raw
            ), mock.patch.object(
                GATE, "rand_raw_symbols", return_value=rand_raw
            ), mock.patch.object(
                GATE,
                "request_options_raw_symbols",
                return_value=request_options_raw,
            ), mock.patch.object(
                GATE,
                "reconnect_policy_raw_symbols",
                return_value=reconnect_policy_raw,
            ), mock.patch.object(
                GATE,
                "circuit_breaker_raw_symbols",
                return_value=circuit_breaker_raw,
            ), mock.patch.object(
                GATE,
                "basetypes_raw_symbols",
                return_value=basetypes_raw,
            ), mock.patch.object(
                GATE,
                "request_queue_raw_symbols",
                return_value=request_queue_raw,
            ), mock.patch.object(
                GATE,
                "utils_raw_symbols",
                return_value=utils_raw,
            ), mock.patch.object(
                GATE,
                "exact_module_raw_symbols",
                side_effect=lambda _nm, _root, _binary, name: exact_raw[name],
            ):
                GATE.check_generated_output(
                    root=Path("/repository"),
                    output=output,
                    modules=modules,
                    clang=Path("/clang++"),
                    nm=Path("/nm"),
                    archiver=Path("/ar"),
                    production=Path("/production.a"),
                    runtime_libraries=[Path("/rusty.a")],
                    cxx_flags=["-stdlib=libc++"],
                    link_flags=["-lc++abi"],
                    prebuilt_module_dirs=[Path("/runtime-modules")],
                )

        compiled_names = [call.args[5] for call in compile_module.call_args_list]
        self.assertEqual(
            compiled_names,
            [
                "rrr.basetypes",
                "rrr.callback_wrapper",
                "rrr.internal_protocol",
                "rrr.stat",
                "rrr.errors",
                "rrr.connection_metrics",
                "rrr.completion_tracker",
                "rrr.rand",
                "rrr.request_options",
                "rrr.reconnect_policy",
                "rrr.circuit_breaker",
                "rrr.connection_state",
                "rrr.heartbeat",
                "rrr.request_queue",
                "rrr.load_balancer",
                "rrr.debugging",
                "rrr.logging",
                "rrr.utils",
                "rrr.frame_codec",
                "rrr.serializable",
                "rrr.serializable_envelope",
                "rrr.epoll_wrapper",
                "rrr.misc",
                "rrr.pollable_proxy",
                "rrr.reactor",
                "rrr.future",
                "rrr.idempotency",
                "rrr.fiber",
                "rrr.channel",
                "rrr.callbacks",
                "rrr.inmemory_channel",
                "rrr.fiber_channel",
                "rrr.threading",
                "rrr.any_message",
                "rrr.tcp_channel",
                "rrr.server",
                "rrr.client",
                "rrr",
            ],
        )
        self.assertTrue(
            all(
                call.args[7] == [Path("/runtime-modules")]
                for call in compile_module.call_args_list
            )
        )
        importer_compile_commands = [
            call.args[0]
            for call in run.call_args_list
            if "-c" in call.args[0]
            and any(argument.endswith("/importer.cpp") for argument in call.args[0])
        ]
        self.assertEqual(len(importer_compile_commands), 1)
        self.assertIn("-I", importer_compile_commands[0])
        self.assertIn(
            "/repository/third-party/rusty-cpp/include",
            importer_compile_commands[0],
        )
        self.assertIn(
            "-fprebuilt-module-path=/runtime-modules",
            importer_compile_commands[0],
        )
        link_commands = [
            call.args[0]
            for call in run.call_args_list
            if "-o" in call.args[0]
            and any("importer-" in argument for argument in call.args[0])
        ]
        self.assertEqual(len(link_commands), 2)
        for command in link_commands:
            self.assertIn("-stdlib=libc++", command)
            self.assertIn("/rusty.a", command)
            self.assertIn("-lc++abi", command)
            if GATE.sys.platform.startswith("linux"):
                self.assertIn("-Wl,--start-group", command)
                self.assertIn("-Wl,--end-group", command)

        # The rrr.logging forwarding fixture is retired: rrr.logging is a
        # canonical Rust module, so the production lane links librrr.a's own
        # provider and the generated lane links the generated object. Neither
        # lane carries a probe object any more, and there is nothing left to
        # order ahead of the archive.
        production_link = next(
            command
            for command in link_commands
            if any(argument == "/production.a" for argument in command)
        )
        self.assertFalse(
            [
                argument
                for argument in production_link
                if argument.endswith("/rrr.logging.probe.o")
                or argument.endswith("/rrr.logging.o")
            ]
        )
        generated_link = next(
            command
            for command in link_commands
            if command is not production_link
        )
        self.assertIn("/rrr.logging.o", generated_link)
        self.assertNotIn("/rrr.logging.probe.o", generated_link)

    def test_completion_raw_symbol_ratchet_pins_all_31_entries(self) -> None:
        # Factory-only construction: the two public constructors became the
        # static `new_()` / `with_config()` factories, so the two C1/C2
        # constructor aliases are gone and the raw total is 33 -> 31.
        entries = list(GATE.ABI_SPECS["rrr.completion_tracker"].symbols)
        entries.append(("T", "initializer for module rrr.completion_tracker"))
        self.assertEqual(len(entries), 31)
        GATE.require_completion_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 31 raw"):
            GATE.require_completion_raw_symbols("test provider", entries[:-1])

    def test_rand_raw_symbol_ratchet_pins_all_13_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.rand"].symbols)
        entries.append(("T", "initializer for module rrr.rand"))
        self.assertEqual(len(entries), 13)
        GATE.require_rand_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 13 raw"):
            GATE.require_rand_raw_symbols("test provider", entries[:-1])

    def test_request_options_raw_symbol_ratchet_pins_all_13_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.request_options"].symbols)
        entries.append(("T", "initializer for module rrr.request_options"))
        self.assertEqual(len(entries), 13)
        GATE.require_request_options_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 13 raw"):
            GATE.require_request_options_raw_symbols(
                "test provider", entries[:-1]
            )

    def test_reconnect_policy_raw_symbol_ratchet_pins_all_12_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.reconnect_policy"].symbols)
        entries.append(("T", "initializer for module rrr.reconnect_policy"))
        self.assertEqual(len(entries), 12)
        GATE.require_reconnect_policy_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 12 raw"):
            GATE.require_reconnect_policy_raw_symbols(
                "test provider", entries[:-1]
            )

    def test_circuit_breaker_raw_symbol_ratchet_pins_all_21_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.circuit_breaker"].symbols)
        entries.append(("T", "initializer for module rrr.circuit_breaker"))
        self.assertEqual(len(entries), 21)
        GATE.require_circuit_breaker_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 21 raw"):
            GATE.require_circuit_breaker_raw_symbols(
                "test provider", entries[:-1]
            )

    def test_exact_raw_symbol_ratchets_include_initializer(self) -> None:
        for module_name, expected_count in (
            ("rrr.connection_state", 14),
            ("rrr.heartbeat", 20),
            ("rrr.load_balancer", 7),
            ("rrr.frame_codec", 18),
        ):
            with self.subTest(module_name=module_name):
                entries = list(GATE.ABI_SPECS[module_name].symbols)
                entries.append(("T", f"initializer for module {module_name}"))
                self.assertEqual(len(entries), expected_count)
                GATE.require_exact_module_raw_symbols(
                    module_name, "test provider", entries
                )
                with self.assertRaisesRegex(GATE.GateError, "raw strong entries"):
                    GATE.require_exact_module_raw_symbols(
                        module_name, "test provider", entries[:-1]
                    )

    def test_utils_raw_symbol_ratchet_pins_all_15_entries(self) -> None:
        # Factory-only construction: `AddrInfo::new_()` / `AddrInfo::adopt()`
        # replaced the two public constructors, so their C1/C2 aliases are gone
        # and the raw total is 17 -> 15. The private fieldwise ctor, the move
        # ctor and the dtor still alias.
        entries = list(GATE.ABI_SPECS["rrr.utils"].symbols)
        for symbol in (
            "rrr::AddrInfo@rrr.utils::AddrInfo(addrinfo*, rusty::Cell<bool>)",
            "rrr::AddrInfo@rrr.utils::AddrInfo(rrr::AddrInfo@rrr.utils&&)",
            "rrr::AddrInfo@rrr.utils::~AddrInfo()",
        ):
            entries.append(("T", symbol))
        entries.append(("T", "initializer for module rrr.utils"))
        self.assertEqual(len(entries), 15)
        GATE.require_utils_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 15 raw"):
            GATE.require_utils_raw_symbols("test provider", entries[:-1])

    def test_basetypes_raw_symbol_ratchet_pins_all_29_entries(self) -> None:
        entries = list(GATE.ABI_SPECS["rrr.basetypes"].symbols)
        entries.append(("T", "initializer for module rrr.basetypes"))
        self.assertEqual(len(entries), 29)
        GATE.require_basetypes_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 29 raw"):
            GATE.require_basetypes_raw_symbols("test provider", entries[:-1])

    def test_request_queue_raw_symbol_ratchet_pins_all_28_entries(self) -> None:
        # Factory-only construction: the two public constructors became the
        # static `new_()` / `with_config()` factories, so the two C1/C2
        # constructor aliases are gone and the raw total is 30 -> 28.
        entries = list(GATE.ABI_SPECS["rrr.request_queue"].symbols)
        entries.append(("T", "initializer for module rrr.request_queue"))
        self.assertEqual(len(entries), 28)
        GATE.require_request_queue_raw_symbols("test provider", entries)
        with self.assertRaisesRegex(GATE.GateError, "exactly 28 raw"):
            GATE.require_request_queue_raw_symbols(
                "test provider", entries[:-1]
            )

    def test_basetypes_cpp_oracle_pins_abort_and_atomic_concurrency(self) -> None:
        source = GATE.importer_source()
        self.assertIn("std::abort();", GATE.ABI_SPECS["rrr.basetypes"].surface)
        self.assertIn("auto concurrent_counter = rrr::Counter::new_(0);", source)
        self.assertIn("for (std::size_t worker = 0; worker < 8; ++worker)", source)
        self.assertIn("concurrent_counter.peek_next() != 80000", source)
        self.assertIn(
            "sparse_wire_digest != UINT64_C(0x6d2ddf1efe2ab0b6)", source
        )

    def test_runtime_module_root_must_exist_and_contain_rusty_pcm(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-runtime-pcm-test-") as temporary:
            root = Path(temporary)
            with self.assertRaisesRegex(GATE.GateError, "unavailable"):
                GATE.resolve_prebuilt_module_dirs(root, ["missing"])

            empty = root / "empty"
            empty.mkdir()
            with self.assertRaisesRegex(GATE.GateError, "rusty.pcm"):
                GATE.resolve_prebuilt_module_dirs(root, [str(empty)])

    def test_runtime_module_dirs_are_nested_deduplicated_and_sorted(self) -> None:
        with tempfile.TemporaryDirectory(prefix="rrr-runtime-pcm-test-") as temporary:
            root = Path(temporary)
            first = root / "modules" / "zeta"
            second = root / "modules" / "alpha"
            first.mkdir(parents=True)
            second.mkdir(parents=True)
            (first / "rusty.pcm").touch()
            (first / "rusty-duplicate-dependency.pcm").touch()
            (second / "vec_port.pcm").touch()

            self.assertEqual(
                GATE.resolve_prebuilt_module_dirs(root, ["modules", "modules"]),
                sorted([first.resolve(), second.resolve()]),
            )

    def test_compile_module_passes_every_runtime_bmi_directory(self) -> None:
        with mock.patch.object(GATE, "run") as run:
            result = GATE.compile_module(
                Path("/clang++"),
                Path("/repository"),
                Path("/include"),
                Path("/source"),
                Path("/work"),
                "rrr.completion_tracker",
                ["-stdlib=libc++"],
                [Path("/runtime-z"), Path("/runtime-a")],
            )

        self.assertEqual(result, Path("/work/rrr.completion_tracker.o"))
        self.assertEqual(len(run.call_args_list), 2)
        for call in run.call_args_list:
            command = call.args[0]
            self.assertIn("-std=gnu++23", command)
            self.assertIn("/repository/src/rrr", command)
            self.assertIn("-fprebuilt-module-path=/work", command)
            self.assertIn("-fprebuilt-module-path=/runtime-z", command)
            self.assertIn("-fprebuilt-module-path=/runtime-a", command)

    def test_gate_abi_ratchet_covers_every_manifest_module(self) -> None:
        root = Path("/repository")
        modules = [
            mock.Mock(cpp_module="rrr.basetypes"),
            mock.Mock(cpp_module="rrr.callback_wrapper"),
            mock.Mock(cpp_module="rrr.internal_protocol"),
            mock.Mock(cpp_module="rrr.stat"),
            mock.Mock(cpp_module="rrr.errors"),
            mock.Mock(cpp_module="rrr.connection_metrics"),
            mock.Mock(cpp_module="rrr.completion_tracker"),
            mock.Mock(cpp_module="rrr.rand"),
            mock.Mock(cpp_module="rrr.request_options"),
            mock.Mock(cpp_module="rrr.reconnect_policy"),
            mock.Mock(cpp_module="rrr.circuit_breaker"),
            mock.Mock(cpp_module="rrr.connection_state"),
            mock.Mock(cpp_module="rrr.heartbeat"),
            mock.Mock(cpp_module="rrr.request_queue"),
            mock.Mock(cpp_module="rrr.load_balancer"),
            mock.Mock(cpp_module="rrr.debugging"),
            mock.Mock(cpp_module="rrr.logging"),
            mock.Mock(cpp_module="rrr.utils"),
            mock.Mock(cpp_module="rrr.frame_codec"),
            mock.Mock(cpp_module="rrr.serializable"),
            mock.Mock(cpp_module="rrr.serializable_envelope"),
            mock.Mock(cpp_module="rrr.epoll_wrapper"),
            mock.Mock(cpp_module="rrr.misc"),
            mock.Mock(cpp_module="rrr.pollable_proxy"),
            mock.Mock(cpp_module="rrr.reactor"),
            mock.Mock(cpp_module="rrr.future"),
            mock.Mock(cpp_module="rrr.idempotency"),
            mock.Mock(cpp_module="rrr.fiber"),
            mock.Mock(cpp_module="rrr.channel"),
            mock.Mock(cpp_module="rrr.callbacks"),
            mock.Mock(cpp_module="rrr.inmemory_channel"),
            mock.Mock(cpp_module="rrr.fiber_channel"),
            mock.Mock(cpp_module="rrr.threading"),
            mock.Mock(cpp_module="rrr.any_message"),
            mock.Mock(cpp_module="rrr.tcp_channel"),
            mock.Mock(cpp_module="rrr.server"),
            mock.Mock(cpp_module="rrr.client"),
        ]
        with mock.patch.object(
            GATE.extraction, "load_manifest", return_value=modules
        ) as load:
            self.assertEqual(GATE.load_owned_modules(root), modules)
        load.assert_called_once_with(root, root / GATE.EXTRACTION_MANIFEST)

        with mock.patch.object(
            GATE.extraction,
            "load_manifest",
            return_value=[*modules, mock.Mock(cpp_module="rrr.orphan")],
        ):
            with self.assertRaisesRegex(
                GATE.GateError, "missing ABI specification"
            ):
                GATE.load_owned_modules(root)


if __name__ == "__main__":
    unittest.main()
