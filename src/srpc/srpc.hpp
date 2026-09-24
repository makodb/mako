#pragma once


// Former `srpc:public` exports are folded into this root module.
// "base/unittest.hpp" is intentionally NOT re-exported through the
// umbrella: it defines free-form `EXPECT_EQ` / `EXPECT_TRUE` /
// `EXPECT_FALSE` / `EXPECT_BINARY_OP_GENERATOR` macros that collide
// with GoogleTest's identically-named macros in any TU that uses
// gtest (every channel-layer test does). In the modular world the
// macros lived in the srpc module's purview and didn't leak; with
// de-modularization the textual `#include` would expose them
// globally. Tests that need the old srpc-internal helpers can
// include `<base/unittest.hpp>` explicitly.
#include "rpc/frame_codec.hpp"
#include "rpc/fiber_channel.hpp"
#include "base/all.hpp"
#include "misc/serializable.hpp"
// removed `#include "misc/recorder.hpp"`
// — `Recorder` class deleted; was unused after Phase 4e-35.
#include "rpc/idempotency.hpp"
#include "rpc/request_queue.hpp"
#include "rpc/completion_tracker.hpp"

// Imports go AFTER textual `#include`s. libc++ rejects the order
// `import std; ... #include <vector>` (the include lands after the
// module's already-imported std), so umbrella imports for modularized
// srpc submodules sit at the bottom of the textual chain.
import srpc.basetypes;
import srpc.callbacks;
import srpc.channel;
// import srpc.circuit_breaker;  // trimmed from consumer umbrella: nothing outside srpc names it (build-time opt)
import srpc.client;
// import srpc.connection_metrics;  // trimmed from consumer umbrella: nothing outside srpc names it (build-time opt)
import srpc.connection_state;
import srpc.debugging;
// import srpc.epoll_wrapper;  // trimmed from consumer umbrella: nothing outside srpc names it (build-time opt)
import srpc.errors;
import srpc.fiber;
import srpc.fiber_channel;
import srpc.future;
// import srpc.heartbeat;  // trimmed from consumer umbrella: nothing outside srpc names it (build-time opt)
// import srpc.internal_protocol;  // trimmed from consumer umbrella: nothing outside srpc names it (build-time opt)
// import srpc.load_balancer;  // trimmed from consumer umbrella: nothing outside srpc names it (build-time opt)
import srpc.logging;
import srpc.misc;
import srpc.pollable_proxy;
import srpc.rand;
import srpc.reactor;
// import srpc.reconnect_policy;  // trimmed from consumer umbrella: nothing outside srpc names it (build-time opt)
// import srpc.request_options;  // trimmed from consumer umbrella: nothing outside srpc names it (build-time opt)
import srpc.server;
import srpc.stat;
import srpc.tcp_channel;
import srpc.threading;
import srpc.utils;

namespace base = srpc;
