#ifndef MAKO_EXAMPLES_MAKOCON_FFI_IMPL_HH
#define MAKO_EXAMPLES_MAKOCON_FFI_IMPL_HH

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <type_traits>

#include "transaction_ffi.h"

namespace makocon_ffi {

static_assert(sizeof(void*) == 8, "Redis transaction FFI expects a 64-bit process");
static_assert(std::is_standard_layout<TxnOperation>::value,
              "TxnOperation must stay C-compatible");
static_assert(std::is_standard_layout<TxnRequest>::value,
              "TxnRequest must stay C-compatible");
static_assert(std::is_standard_layout<TxnOpResult>::value,
              "TxnOpResult must stay C-compatible");
static_assert(std::is_standard_layout<TxnResponse>::value,
              "TxnResponse must stay C-compatible");
static_assert(sizeof(TxnOperation) == 64, "TxnOperation layout changed");
static_assert(sizeof(TxnRequest) == 16, "TxnRequest layout changed");
static_assert(sizeof(TxnOpResult) == 32, "TxnOpResult layout changed");
static_assert(sizeof(TxnResponse) == 24, "TxnResponse layout changed");
static_assert(offsetof(TxnOperation, flags) == 40, "TxnOperation.flags offset changed");
static_assert(offsetof(TxnOperation, expire_at_ms) == 48,
              "TxnOperation.expire_at_ms offset changed");
static_assert(offsetof(TxnOpResult, int_value) == 24,
              "TxnOpResult.int_value offset changed");

// @safe
inline bool allocate_response(const TxnRequest* request, TxnResponse* response) {
    if (!request || !response || request->num_ops == 0) {
        if (response) {
            response->transaction_success = false;
            response->num_results = 0;
            response->results = nullptr;
        }
        return false;
    }

    response->transaction_success = false;
    response->num_results = request->num_ops;
    response->results = static_cast<TxnOpResult*>(
        std::calloc(request->num_ops, sizeof(TxnOpResult)));
    if (!response->results) {
        response->num_results = 0;
        return false;
    }
    return true;
}

// @safe
inline void free_transaction_response(TxnResponse* response) {
    if (!response || !response->results) {
        return;
    }

    for (size_t i = 0; i < response->num_results; ++i) {
        std::free(response->results[i].data_ptr);
    }
    std::free(response->results);
    response->results = nullptr;
    response->num_results = 0;
}

// @safe
inline bool populate_metrics(
    MakoMetrics* metrics,
    std::chrono::steady_clock::time_point start_time,
    const std::atomic<uint64_t>& commits,
    const std::atomic<uint64_t>& aborts,
    const std::atomic<uint64_t>& retries) {
    if (!metrics) {
        return false;
    }

    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start_time);
    metrics->txn_commits = commits.load(std::memory_order_relaxed);
    metrics->txn_aborts = aborts.load(std::memory_order_relaxed);
    metrics->txn_retries = retries.load(std::memory_order_relaxed);
    metrics->uptime_seconds = static_cast<uint64_t>(uptime.count());
    return true;
}

// @safe
inline void record_txn_retry(std::atomic<uint64_t>& retries) {
    retries.fetch_add(1, std::memory_order_relaxed);
}

// @safe
inline bool opcode_supported_by_multitrd_queue(uint32_t op) {
    return op >= TXN_OP_GET && op <= TXN_OP_EXISTS;
}

}  // namespace makocon_ffi

#endif  // MAKO_EXAMPLES_MAKOCON_FFI_IMPL_HH
