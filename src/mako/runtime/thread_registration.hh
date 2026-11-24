#pragma once

namespace mako::silo {
enum class thread_runtime { native_mako, plain_masstree };

// A worker retains its runtime identity and slot across detach/reattach.
// Slots are never reassigned to another OS thread.
bool claim_thread_runtime(thread_runtime runtime) noexcept;
int try_allocate_thread_id() noexcept;
} // namespace mako::silo
