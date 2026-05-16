#pragma once
// Anchor shim. The real `FiberChannel` declaration lives in the
// `rrr.fiber_channel` module (src/rrr/rpc/fiber_channel.cpp).
// `<memory>` is the actual anchor that keeps libc++ `operator new`
// in global-module attachment for downstream TUs.
#include <memory>
