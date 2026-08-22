#pragma once

// Shared process-wide registration for the STO/MassTrans runtime.
//
// TThread::id indexes Transaction::tinfo[MAX_THREADS], so independently
// allocating IDs in two adapters is memory corruption, not merely duplicate
// bookkeeping.  Native Mako workers and the Rust-facing local ABI therefore
// draw from this one allocator.

namespace mako::silo {

enum class thread_runtime {
  native_mako,
  local_abi,
  plain_masstree,
};

// Claim this OS thread for one storage adapter. Repeated claims by the same
// adapter are harmless; switching adapters would overwrite TLS runtime/RCU
// configuration and is rejected.
bool claim_thread_runtime(thread_runtime runtime) noexcept;

// Return this OS thread's process-wide TThread ID, reserving one on its first
// call. Returns -1 after the fixed Transaction::tinfo space is exhausted.
// IDs are intentionally never reused: per-thread RCU state can outlive the OS
// thread that created it. Calls are idempotent within the adapter claimed by
// claim_thread_runtime(); switching adapters on one worker is not supported.
int try_allocate_thread_id() noexcept;

// Bind this worker to the one process-wide MassTrans MasstreeContext, install
// its epoch callback, and start the single STO epoch advancer. Idempotent and
// thread-safe. Returns false only if setup or thread creation failed.
bool ensure_epoch_runtime() noexcept;

}  // namespace mako::silo
