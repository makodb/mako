#pragma once
// Anchor shim. The real `FiberChannel` declaration lives in the
// `rrr.fiber_channel` module (crates/srpc/cpp/generated/rrr.fiber_channel.cppm).
// `<memory>` is the actual anchor that keeps libc++ `operator new`
// in global-module attachment for downstream TUs.
#include <memory>
