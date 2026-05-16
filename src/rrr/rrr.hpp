#pragma once


// Former `rrr:public` exports are folded into this root module.
// "base/unittest.hpp" is intentionally NOT re-exported through the
// umbrella: it defines free-form `EXPECT_EQ` / `EXPECT_TRUE` /
// `EXPECT_FALSE` / `EXPECT_BINARY_OP_GENERATOR` macros that collide
// with GoogleTest's identically-named macros in any TU that uses
// gtest (every channel-layer test does). In the modular world the
// macros lived in the rrr module's purview and didn't leak; with
// de-modularization the textual `#include` would expose them
// globally. Tests that need the old rrr-internal helpers can
// include `<base/unittest.hpp>` explicitly.
#include "rpc/frame_codec.hpp"
#include "rpc/fiber_channel.hpp"
#include "base/all.hpp"
#include "misc/marshal.hpp"
#include "misc/serializable.hpp"
// removed `#include "misc/recorder.hpp"`
// — `Recorder` class deleted; was unused after Phase 4e-35.
#include "rpc/idempotency.hpp"
#include "rpc/request_queue.hpp"
#include "rpc/completion_tracker.hpp"
#include "rpc/client.hpp"

// Imports go AFTER textual `#include`s. libc++ rejects the order
// `import std; ... #include <vector>` (the include lands after the
// module's already-imported std), so umbrella imports for modularized
// rrr submodules sit at the bottom of the textual chain.
import rrr.alarm;
import rrr.alock;
import rrr.basetypes;
import rrr.callbacks;
import rrr.channel;
import rrr.circuit_breaker;
import rrr.connection_metrics;
import rrr.connection_state;
import rrr.cpuinfo;
import rrr.dball;
import rrr.debugging;
import rrr.epoll_wrapper;
import rrr.errors;
import rrr.fiber;
import rrr.future;
import rrr.heartbeat;
import rrr.internal_protocol;
import rrr.load_balancer;
import rrr.logging;
import rrr.misc;
import rrr.netinfo;
import rrr.pollable_proxy;
import rrr.rand;
import rrr.reactor;
import rrr.reconnect_policy;
import rrr.request_options;
import rrr.server;
import rrr.stat;
import rrr.strop;
import rrr.tcp_channel;
import rrr.threading;
import rrr.utils;

namespace base = rrr;
