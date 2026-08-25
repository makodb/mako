#pragma once
// Anchor shim. The real `FiberChannel` declaration lives in the
// `srpc.fiber_channel` module (src/srpc/rpc/fiber_channel.cpp).
// `<memory>` is the actual anchor that keeps libc++ `operator new`
// in global-module attachment for downstream TUs.
#include <memory>
