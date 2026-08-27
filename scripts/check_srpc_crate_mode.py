#!/usr/bin/env python3
"""Check rusty-cpp crate output against exact generated and production ABIs."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile

import extract_srpc_rust as extraction


DEFAULT_TRANSPILER = (
    "third-party/rusty-cpp/target/release/rusty-cpp-transpiler"
)
RUSTY_CPP_SUBMODULE = "third-party/rusty-cpp"
REQUIRED_RUSTY_CPP_COMMIT = "a1f8fef85e8d43bb00f85f8ef32e5ecc69408642"
EXTRACTION_DRIVER = "scripts/extract_srpc_rust.py"
# Crate-relative, like every other label the extraction driver owns: it
# resolves against `extraction.crate_root(root)` (the vendored srpc tree at
# `src/srpc`), not against the repository root. The three gate-owned emitter
# inputs below stay repository-relative -- the gate passes them straight to the
# transpiler as absolute paths.
EXTRACTION_MANIFEST = "rust-modules.toml"
MODULE_PREAMBLE = "src/srpc/module-preambles.toml"
TYPE_MAP = "src/srpc/rust-type-map.toml"
CPP_MODULE_INDEX = "src/srpc/cpp-module-index.toml"
NM_LINE = re.compile(r"^[0-9A-Fa-f]+\s+([A-Za-z])\s+(.+)$")
PLACEHOLDER = re.compile(r"\b(?:TODO|UNSUPPORTED|skipped)\b", re.IGNORECASE)

# A compiler diagnostic comment is not an unimplemented user lowering.
# rusty-cpp emits this exact informational marker when it breaks a by-value
# type cycle while ordering emitted declarations; the affected types are still
# fully defined and the module still compiles. srpc.tcp_channel is the live
# example: it carries this marker for TcpListener, defines TcpListener and all
# of its methods, and builds to a complete object with the ratcheted ABI.
# Upstream rusty-cpp main emits the identical text, so this one fixed form is
# allowlisted rather than treated as an unimplemented slot. The strict
# TODO/UNSUPPORTED/skipped ratchet still applies to every other spelling,
# including hand-attention slots such as TODO(interface_traits).
BENIGN_GENERATED_DIAGNOSTIC = re.compile(
    r"^// UNSUPPORTED: unsupported by-value circular type dependency "
    r"in scope [^:\n]+: \[[^\]\n]*\](?:; cycle path: [^\n]*)?$",
    re.MULTILINE,
)


@dataclass(frozen=True)
class AbiSpec:
    """Checked C++ surface and exact symbols for one canonical Rust module."""

    surface: frozenset[str]
    symbols: frozenset[tuple[str, str]]


ABI_SPECS = {
    "srpc.callback_wrapper": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.callback_wrapper;",
                "namespace srpc {",
                "namespace detail {",
                "export template<typename F>",
                "struct CallbackWrapper",
                "rusty::Option<rusty::Arc<F>> inner;",
                "static CallbackWrapper<F> from_callable(F callable) {",
                "rusty::Arc<F>::make(std::move(callable))",
                "bool has_value() const {",
                "const F& callable() const {",
                "CallbackWrapper<F> clone() const {",
                "static CallbackWrapper<F> default_() {",
                "static constexpr bool is_send",
                "static constexpr bool is_sync",
            }
        ),
        # CallbackWrapper is an exported class template. Its concrete weak
        # instantiations belong to importers, so module provider objects must
        # not acquire an out-of-line specialization ABI.
        symbols=frozenset(),
    ),
    "srpc.internal_protocol": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.internal_protocol;",
                "namespace srpc {",
                "export constexpr int32_t kInternalHeartbeatRpcId",
                "export constexpr uint32_t kResponseHeaderExtFlag",
                "export constexpr uint32_t kResponseSizeMask",
                "export bool response_has_extended_header(int32_t encoded_size);",
                "export int32_t response_payload_size(int32_t encoded_size);",
                "export int32_t encode_response_size(int32_t payload_size, bool extended_header);",
            }
        ),
        symbols=frozenset(
            {
                ("R", "srpc::kInternalHeartbeatRpcId@srpc.internal_protocol"),
                ("R", "srpc::kResponseHeaderExtFlag@srpc.internal_protocol"),
                ("R", "srpc::kResponseSizeMask@srpc.internal_protocol"),
                (
                    "T",
                    "srpc::encode_response_size@srpc.internal_protocol(int, bool)",
                ),
                (
                    "T",
                    "srpc::response_has_extended_header@srpc.internal_protocol(int)",
                ),
                (
                    "T",
                    "srpc::response_payload_size@srpc.internal_protocol(int)",
                ),
            }
        ),
    ),
    "srpc.stat": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.stat;",
                "namespace srpc {",
                "export struct AvgStat",
                "int64_t n_stat_;",
                "int64_t sum_;",
                "int64_t avg_;",
                "int64_t max_;",
                "int64_t min_;",
                "static AvgStat new_();",
                "void sample(int64_t s);",
                "void clear();",
                "AvgStat reset();",
                "AvgStat peek() const;",
                "int64_t avg() const;",
            }
        ),
        symbols=frozenset(
            {
                ("T", "srpc::AvgStat@srpc.stat::new_()"),
                ("T", "srpc::AvgStat@srpc.stat::sample(long)"),
                ("T", "srpc::AvgStat@srpc.stat::clear()"),
                ("T", "srpc::AvgStat@srpc.stat::reset()"),
                ("T", "srpc::AvgStat@srpc.stat::peek() const"),
                ("T", "srpc::AvgStat@srpc.stat::avg() const"),
            }
        ),
    ),
    "srpc.errors": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.errors;",
                "namespace srpc {",
                "export enum class RpcErrorCategory",
                "export enum class RpcError",
                "export std::string_view rpc_error_category_to_string(RpcErrorCategory cat);",
                "export std::string_view rpc_error_to_string(RpcError err);",
                "export RpcErrorCategory get_error_category(RpcError err);",
                "export bool is_connection_error(RpcError err);",
                "export bool is_timeout_error(RpcError err);",
                "export bool is_retryable_error(RpcError err);",
            }
        ),
        symbols=frozenset(
            {
                (
                    "T",
                    "srpc::get_error_category@srpc.errors(srpc::RpcError@srpc.errors)",
                ),
                (
                    "T",
                    "srpc::is_connection_error@srpc.errors(srpc::RpcError@srpc.errors)",
                ),
                (
                    "T",
                    "srpc::is_retryable_error@srpc.errors(srpc::RpcError@srpc.errors)",
                ),
                (
                    "T",
                    "srpc::is_timeout_error@srpc.errors(srpc::RpcError@srpc.errors)",
                ),
                (
                    "T",
                    "srpc::rpc_error_category_to_string@srpc.errors(srpc::RpcErrorCategory@srpc.errors)",
                ),
                (
                    "T",
                    "srpc::rpc_error_to_string@srpc.errors(srpc::RpcError@srpc.errors)",
                ),
            }
        ),
    ),
    "srpc.connection_metrics": AbiSpec(
        surface=frozenset(
            {
                "#include <rusty/sync/atomic.hpp>",
                "export module srpc.connection_metrics;",
                "namespace srpc {",
                "export struct ConnectionMetrics",
                "using rusty::sync::atomic::AtomicU64;",
                "using rusty::sync::atomic::Ordering;",
                "rusty::sync::atomic::AtomicU64 requests_sent_field;",
                "rusty::sync::atomic::AtomicU64 requests_completed_field;",
                "rusty::sync::atomic::AtomicU64 requests_failed_field;",
                "rusty::sync::atomic::AtomicU64 requests_timed_out_field;",
                "rusty::sync::atomic::AtomicU64 in_flight_requests_field;",
                "rusty::sync::atomic::AtomicU64 bytes_sent_field;",
                "rusty::sync::atomic::AtomicU64 bytes_received_field;",
                "rusty::sync::atomic::AtomicU64 reconnect_count_field;",
                "rusty::sync::atomic::AtomicU64 retry_attempts_field;",
                "rusty::sync::atomic::AtomicU64 queue_dropped_requests_field;",
                "rusty::sync::atomic::AtomicU64 circuit_open_rejections_field;",
                "rusty::sync::atomic::AtomicU64 circuit_open_transitions_field;",
                "rusty::sync::atomic::AtomicU64 circuit_half_open_transitions_field;",
                "rusty::sync::atomic::AtomicU64 circuit_closed_transitions_field;",
                "rusty::sync::atomic::AtomicU64 connect_time_ms_field;",
                "rusty::sync::atomic::AtomicU64 total_latency_us_field;",
                "rusty::sync::atomic::AtomicU64 min_latency_us_field;",
                "rusty::sync::atomic::AtomicU64 max_latency_us_field;",
                "static ConnectionMetrics new_();",
                "uint64_t requests_sent() const;",
                "uint64_t requests_completed() const;",
                "uint64_t requests_failed() const;",
                "uint64_t requests_timed_out() const;",
                "uint64_t in_flight_requests() const;",
                "uint64_t bytes_sent() const;",
                "uint64_t bytes_received() const;",
                "uint64_t reconnect_count() const;",
                "uint64_t retry_attempts() const;",
                "uint64_t queue_dropped_requests() const;",
                "uint64_t circuit_open_rejections() const;",
                "uint64_t circuit_open_transitions() const;",
                "uint64_t circuit_half_open_transitions() const;",
                "uint64_t circuit_closed_transitions() const;",
                "uint64_t connect_time_ms() const;",
                "uint64_t min_latency_us() const;",
                "uint64_t max_latency_us() const;",
                "uint64_t success_rate_percent() const;",
                "uint64_t avg_latency_us() const;",
                "uint64_t uptime_ms(uint64_t current_time_ms) const;",
                "void record_request_sent() const;",
                "void record_request_completed_with_latency(uint64_t latency_us) const;",
                "void record_request_completed() const;",
                "void record_request_failed() const;",
                "void record_request_timeout() const;",
                "void record_request_dropped() const;",
                "void record_bytes_sent(uint64_t bytes) const;",
                "void record_bytes_received(uint64_t bytes) const;",
                "void record_reconnect() const;",
                "void record_retry_attempt() const;",
                "void record_queue_drop() const;",
                "void record_circuit_open_rejection() const;",
                "void record_circuit_open_transition() const;",
                "void record_circuit_half_open_transition() const;",
                "void record_circuit_closed_transition() const;",
                "void record_connect(uint64_t current_time_ms) const;",
                "void reset() const;",
                "void decrement_in_flight() const;",
                "static constexpr bool is_send = true;",
                "static constexpr bool is_sync = true;",
            }
        ),
        symbols=frozenset(
            ("T", symbol)
            for symbol in {
                "srpc::ConnectionMetrics@srpc.connection_metrics::new_()",
                "srpc::ConnectionMetrics@srpc.connection_metrics::requests_sent() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::requests_completed() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::requests_failed() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::requests_timed_out() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::in_flight_requests() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::bytes_sent() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::bytes_received() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::reconnect_count() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::retry_attempts() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::queue_dropped_requests() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::circuit_open_rejections() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::circuit_open_transitions() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::circuit_half_open_transitions() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::circuit_closed_transitions() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::connect_time_ms() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::min_latency_us() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::max_latency_us() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::success_rate_percent() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::avg_latency_us() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::uptime_ms(unsigned long) const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_request_sent() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_request_completed_with_latency(unsigned long) const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_request_completed() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_request_failed() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_request_timeout() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_request_dropped() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_bytes_sent(unsigned long) const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_bytes_received(unsigned long) const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_reconnect() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_retry_attempt() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_queue_drop() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_circuit_open_rejection() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_circuit_open_transition() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_circuit_half_open_transition() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_circuit_closed_transition() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::record_connect(unsigned long) const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::reset() const",
                "srpc::ConnectionMetrics@srpc.connection_metrics::decrement_in_flight() const",
            }
        ),
    ),
    "srpc.completion_tracker": AbiSpec(
        surface=frozenset(
            {
                "#include <rusty/sync/atomic.hpp>",
                "export module srpc.completion_tracker;",
                "import std_port;",
                "export enum class CompletionStatus",
                "export struct CompletionTrackerConfig",
                "export struct CompletedEntry",
                "export struct CompletionTracker",
                "export struct CompletionQueryResult",
                "using rusty::HashSet;",
                "using rusty::VecDeque;",
                "using rusty::sync::atomic::AtomicU64;",
                "using rusty::sync::atomic::Ordering;",
                "using rusty::Mutex;",
                "rusty::Mutex<CompletionTrackerConfig> config_;",
                "rusty::Mutex<rusty::VecDeque<CompletedEntry>> lru_list_;",
                "rusty::Mutex<rusty::HashSet<int64_t>> completed_set_;",
                "rusty::sync::atomic::AtomicU64 total_tracked_;",
                "rusty::sync::atomic::AtomicU64 queries_;",
                "rusty::sync::atomic::AtomicU64 query_hits_;",
                "rusty::sync::atomic::AtomicU64 evictions_;",
                "static CompletionTracker new_();",
                "static CompletionTracker with_config(CompletionTrackerConfig config);",
                "bool enabled() const;",
                "CompletionTrackerConfig config() const;",
                "void set_config(CompletionTrackerConfig config);",
                "void mark_completed(int64_t xid, uint64_t current_time_ms);",
                "bool is_completed(int64_t xid, uint64_t current_time_ms);",
                "bool remove(int64_t xid);",
                "void clear();",
                "size_t size() const;",
                "uint64_t total_tracked() const;",
                "uint64_t queries() const;",
                "uint64_t query_hits() const;",
                "double hit_rate() const;",
                "uint64_t evictions() const;",
                "void reset_stats();",
                "size_t evict_expired(uint64_t current_time_ms);",
                "CompletionStatus status;",
                "int32_t error_code;",
                "bool has_cached_response;",
                "export std::string_view completion_status_to_string(CompletionStatus status);",
                "rusty::wrapping_add(this->timestamp_ms",
                "static constexpr bool is_send = true;",
                "static constexpr bool is_sync = true;",
            }
        ),
        symbols=frozenset(
            ("T", symbol)
            for symbol in {
                "srpc::CompletionTrackerConfig@srpc.completion_tracker::new_()",
                "srpc::CompletionTrackerConfig@srpc.completion_tracker::defaults()",
                "srpc::CompletionTrackerConfig@srpc.completion_tracker::small()",
                "srpc::CompletionTrackerConfig@srpc.completion_tracker::large()",
                "srpc::CompletionTrackerConfig@srpc.completion_tracker::disabled()",
                "srpc::CompletedEntry@srpc.completion_tracker::new_(long, unsigned long)",
                "srpc::CompletedEntry@srpc.completion_tracker::is_expired(unsigned long, unsigned long) const",
                "srpc::CompletionTracker@srpc.completion_tracker::new_()",
                "srpc::CompletionTracker@srpc.completion_tracker::with_config(srpc::CompletionTrackerConfig@srpc.completion_tracker)",
                "srpc::CompletionTracker@srpc.completion_tracker::enabled() const",
                "srpc::CompletionTracker@srpc.completion_tracker::config() const",
                "srpc::CompletionTracker@srpc.completion_tracker::set_config(srpc::CompletionTrackerConfig@srpc.completion_tracker)",
                "srpc::CompletionTracker@srpc.completion_tracker::mark_completed(long, unsigned long)",
                "srpc::CompletionTracker@srpc.completion_tracker::is_completed(long, unsigned long)",
                "srpc::CompletionTracker@srpc.completion_tracker::remove(long)",
                "srpc::CompletionTracker@srpc.completion_tracker::clear()",
                "srpc::CompletionTracker@srpc.completion_tracker::size() const",
                "srpc::CompletionTracker@srpc.completion_tracker::total_tracked() const",
                "srpc::CompletionTracker@srpc.completion_tracker::queries() const",
                "srpc::CompletionTracker@srpc.completion_tracker::query_hits() const",
                "srpc::CompletionTracker@srpc.completion_tracker::hit_rate() const",
                "srpc::CompletionTracker@srpc.completion_tracker::evictions() const",
                "srpc::CompletionTracker@srpc.completion_tracker::reset_stats()",
                "srpc::CompletionTracker@srpc.completion_tracker::evict_expired(unsigned long)",
                "srpc::CompletionQueryResult@srpc.completion_tracker::new_()",
                "srpc::CompletionQueryResult@srpc.completion_tracker::not_found()",
                "srpc::CompletionQueryResult@srpc.completion_tracker::completed(int, bool)",
                "srpc::CompletionQueryResult@srpc.completion_tracker::expired()",
                "srpc::CompletionQueryResult@srpc.completion_tracker::is_completed() const",
                "srpc::completion_status_to_string@srpc.completion_tracker(srpc::CompletionStatus@srpc.completion_tracker)",
            }
        ),
    ),
    "srpc.rand": AbiSpec(
        surface=frozenset(
            {
                '#include "misc/srpc_rand.h"',
                "export module srpc.rand;",
                "import vec_port.vec;",
                "namespace rusty_cpp_abi_detail {",
                "bytes_from_std_string(const std::string& input)",
                "std_string_from_bytes(rusty::Vec<uint8_t> input)",
                "f64_span_from_std_vector(const std::vector<double>& input)",
                "export using RandWeightVec = std::vector<double>;",
                "export struct RandomGenerator",
                "export double randgen_rand_max();",
                "export std::string randgen_zero_pad(std::string s, int32_t length);",
                "export int32_t randgen_rand_raw();",
                "export int32_t randgen_nu_constant_now();",
                "export void randgen_destroy();",
                "static int32_t rand(int32_t min, int32_t max);",
                "static double rand_double(double min, double max);",
                "static std::string int2str_n(int32_t i, int32_t length);",
                "static bool percentage_true(int32_t p);",
                "static int32_t nu_rand(int32_t a, int32_t x, int32_t y);",
                "static uint32_t weighted_select(const RandWeightVec& weight_vector);",
                "static void destroy();",
                "rusty::wrapping_sub(max",
                "rusty::wrapping_add((rusty::detail::deref_if_pointer_like(r)",
                "rusty::wrapping_sub(((static_cast<uint32_t>(k)))",
                "static constexpr bool is_send = true;",
                "static constexpr bool is_sync = true;",
            }
        ),
        symbols=frozenset(
            {
                ("T", "srpc::randgen_rand_max@srpc.rand()"),
                (
                    "T",
                    "srpc::randgen_zero_pad@srpc.rand(std::__1::basic_string<char, "
                    "std::__1::char_traits<char>, std::__1::allocator<char>>, int)",
                ),
                ("T", "srpc::randgen_rand_raw@srpc.rand()"),
                ("T", "srpc::randgen_nu_constant_now@srpc.rand()"),
                ("T", "srpc::randgen_destroy@srpc.rand()"),
                ("T", "srpc::RandomGenerator@srpc.rand::rand(int, int)"),
                (
                    "T",
                    "srpc::RandomGenerator@srpc.rand::rand_double(double, double)",
                ),
                (
                    "T",
                    "srpc::RandomGenerator@srpc.rand::int2str_n(int, int)",
                ),
                (
                    "T",
                    "srpc::RandomGenerator@srpc.rand::percentage_true(int)",
                ),
                (
                    "T",
                    "srpc::RandomGenerator@srpc.rand::nu_rand(int, int, int)",
                ),
                (
                    "T",
                    "srpc::RandomGenerator@srpc.rand::weighted_select("
                    "std::__1::vector<double, std::__1::allocator<double>> const&)",
                ),
                ("T", "srpc::RandomGenerator@srpc.rand::destroy()"),
            }
        ),
    ),
    "srpc.request_options": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.request_options;",
                "import srpc.rand;",
                "export enum class TimeoutType",
                "export constexpr TimeoutType TimeoutType_NONE();",
                "export constexpr TimeoutType TimeoutType_CONNECT_TIMEOUT();",
                "export constexpr TimeoutType TimeoutType_REQUEST_TIMEOUT();",
                "export constexpr TimeoutType TimeoutType_RESPONSE_TIMEOUT();",
                "export constexpr TimeoutType TimeoutType_TOTAL_TIMEOUT();",
                "inline constexpr TimeoutType TimeoutType_NONE()",
                "inline constexpr TimeoutType TimeoutType_CONNECT_TIMEOUT()",
                "inline constexpr TimeoutType TimeoutType_REQUEST_TIMEOUT()",
                "inline constexpr TimeoutType TimeoutType_RESPONSE_TIMEOUT()",
                "inline constexpr TimeoutType TimeoutType_TOTAL_TIMEOUT()",
                "export struct RequestOptions",
                "uint64_t timeout_ms;",
                "uint64_t total_timeout_ms;",
                "uint16_t max_retries;",
                "uint16_t base_delay_ms;",
                "uint16_t max_delay_ms;",
                "float jitter_factor;",
                "bool idempotent;",
                "static RequestOptions new_();",
                "static RequestOptions defaults();",
                "static RequestOptions with_retry(uint16_t max_retries, uint64_t timeout_ms);",
                "static RequestOptions idempotent_retry(uint16_t max_retries);",
                "static RequestOptions no_timeout();",
                "static RequestOptions fast();",
                "static RequestOptions patient();",
                "bool can_retry(uint16_t current_retry_count) const;",
                "uint64_t calculate_delay_ms(uint16_t attempt) const;",
                "bool is_total_timeout_exceeded(uint64_t elapsed_ms) const;",
                "uint64_t remaining_time_ms(uint64_t elapsed_ms) const;",
                "static constexpr bool is_send = true;",
                "static constexpr bool is_sync = true;",
                "export std::string_view timeout_type_to_string(TimeoutType ty);",
                "static_cast<double>(randgen_rand_raw())",
                "randgen_rand_max()",
            }
        ),
        symbols=frozenset(
            ("T", symbol)
            for symbol in {
                "srpc::RequestOptions@srpc.request_options::new_()",
                "srpc::RequestOptions@srpc.request_options::defaults()",
                "srpc::RequestOptions@srpc.request_options::with_retry(unsigned short, unsigned long)",
                "srpc::RequestOptions@srpc.request_options::idempotent_retry(unsigned short)",
                "srpc::RequestOptions@srpc.request_options::no_timeout()",
                "srpc::RequestOptions@srpc.request_options::fast()",
                "srpc::RequestOptions@srpc.request_options::patient()",
                "srpc::RequestOptions@srpc.request_options::can_retry(unsigned short) const",
                "srpc::RequestOptions@srpc.request_options::calculate_delay_ms(unsigned short) const",
                "srpc::RequestOptions@srpc.request_options::is_total_timeout_exceeded(unsigned long) const",
                "srpc::RequestOptions@srpc.request_options::remaining_time_ms(unsigned long) const",
                "srpc::timeout_type_to_string@srpc.request_options(srpc::TimeoutType@srpc.request_options)",
            }
        ),
    ),
    "srpc.reconnect_policy": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.reconnect_policy;",
                "import srpc.rand;",
                "export struct ReconnectPolicy",
                "bool auto_reconnect;",
                "uint32_t max_retries;",
                "uint32_t initial_delay_ms;",
                "uint32_t max_delay_ms;",
                "double backoff_multiplier;",
                "bool jitter_enabled;",
                "static ReconnectPolicy new_();",
                "static ReconnectPolicy aggressive();",
                "static ReconnectPolicy conservative();",
                "static ReconnectPolicy no_retry();",
                "static constexpr bool is_send = true;",
                "static constexpr bool is_sync = true;",
                "export struct ReconnectCalculator",
                "const ReconnectPolicy& policy;",
                "rusty::Cell<uint32_t> retries;",
                "static ReconnectCalculator new_(const ReconnectPolicy& policy);",
                "bool should_retry() const;",
                "uint32_t next_delay_ms() const;",
                "uint32_t peek_delay_ms() const;",
                "void reset() const;",
                "uint32_t retry_count() const;",
                "bool retries_exhausted() const;",
                "rusty::wrapping_add(count",
                "static_cast<double>(randgen_rand_raw())",
                "randgen_rand_max()",
            }
        ),
        symbols=frozenset(
            ("T", symbol)
            for symbol in {
                "srpc::ReconnectPolicy@srpc.reconnect_policy::new_()",
                "srpc::ReconnectPolicy@srpc.reconnect_policy::aggressive()",
                "srpc::ReconnectPolicy@srpc.reconnect_policy::conservative()",
                "srpc::ReconnectPolicy@srpc.reconnect_policy::no_retry()",
                "srpc::ReconnectCalculator@srpc.reconnect_policy::new_(srpc::ReconnectPolicy@srpc.reconnect_policy const&)",
                "srpc::ReconnectCalculator@srpc.reconnect_policy::should_retry() const",
                "srpc::ReconnectCalculator@srpc.reconnect_policy::next_delay_ms() const",
                "srpc::ReconnectCalculator@srpc.reconnect_policy::peek_delay_ms() const",
                "srpc::ReconnectCalculator@srpc.reconnect_policy::reset() const",
                "srpc::ReconnectCalculator@srpc.reconnect_policy::retry_count() const",
                "srpc::ReconnectCalculator@srpc.reconnect_policy::retries_exhausted() const",
            }
        ),
    ),
    "srpc.circuit_breaker": AbiSpec(
        surface=frozenset(
            {
                '#include "misc/srpc_timing.h"',
                "export module srpc.circuit_breaker;",
                "export enum class CircuitState",
                "export struct CircuitBreakerConfig",
                "uint32_t failure_threshold;",
                "uint32_t success_threshold;",
                "uint32_t timeout_ms;",
                "bool enabled;",
                "static CircuitBreakerConfig new_();",
                "static CircuitBreakerConfig defaults();",
                "static CircuitBreakerConfig sensitive();",
                "static CircuitBreakerConfig relaxed();",
                "static CircuitBreakerConfig disabled();",
                "export struct CircuitBreaker",
                "rusty::Cell<CircuitBreakerConfig> config_field;",
                "rusty::Cell<CircuitState> state_field;",
                "rusty::Cell<uint32_t> failure_count_field;",
                "rusty::Cell<uint32_t> success_count_field;",
                "rusty::Cell<uint64_t> last_failure_time;",
                "rusty::Cell<bool> probe_in_progress;",
                "static CircuitBreaker new_(CircuitBreakerConfig config);",
                "void set_config(CircuitBreakerConfig config) const;",
                "bool allow_request() const;",
                "void record_success() const;",
                "void record_failure() const;",
                "CircuitState state() const;",
                "bool is_open() const;",
                "bool is_closed() const;",
                "bool is_half_open() const;",
                "void reset() const;",
                "uint32_t failure_count() const;",
                "uint32_t success_count() const;",
                "CircuitBreakerConfig config() const;",
                "export uint64_t current_time_us();",
                "export std::string_view circuit_state_to_string(CircuitState state);",
                "rusty::wrapping_sub(now",
                "rusty::wrapping_add(this->failure_count_field.get()",
                "rusty::wrapping_add(this->success_count_field.get()",
            }
        ),
        symbols=frozenset(
            ("T", symbol)
            for symbol in {
                "srpc::current_time_us@srpc.circuit_breaker()",
                "srpc::circuit_state_to_string@srpc.circuit_breaker(srpc::CircuitState@srpc.circuit_breaker)",
                "srpc::CircuitBreakerConfig@srpc.circuit_breaker::new_()",
                "srpc::CircuitBreakerConfig@srpc.circuit_breaker::defaults()",
                "srpc::CircuitBreakerConfig@srpc.circuit_breaker::sensitive()",
                "srpc::CircuitBreakerConfig@srpc.circuit_breaker::relaxed()",
                "srpc::CircuitBreakerConfig@srpc.circuit_breaker::disabled()",
                "srpc::CircuitBreaker@srpc.circuit_breaker::new_(srpc::CircuitBreakerConfig@srpc.circuit_breaker)",
                "srpc::CircuitBreaker@srpc.circuit_breaker::set_config(srpc::CircuitBreakerConfig@srpc.circuit_breaker) const",
                "srpc::CircuitBreaker@srpc.circuit_breaker::allow_request() const",
                "srpc::CircuitBreaker@srpc.circuit_breaker::record_success() const",
                "srpc::CircuitBreaker@srpc.circuit_breaker::record_failure() const",
                "srpc::CircuitBreaker@srpc.circuit_breaker::state() const",
                "srpc::CircuitBreaker@srpc.circuit_breaker::is_open() const",
                "srpc::CircuitBreaker@srpc.circuit_breaker::is_closed() const",
                "srpc::CircuitBreaker@srpc.circuit_breaker::is_half_open() const",
                "srpc::CircuitBreaker@srpc.circuit_breaker::reset() const",
                "srpc::CircuitBreaker@srpc.circuit_breaker::failure_count() const",
                "srpc::CircuitBreaker@srpc.circuit_breaker::success_count() const",
                "srpc::CircuitBreaker@srpc.circuit_breaker::config() const",
            }
        ),
    ),
    "srpc.connection_state": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.connection_state;",
                "export enum class ConnectionState",
                "export struct ConnectionStateMachine",
                "export using StateChangeCallback = rusty::Function<void(ConnectionState, ConnectionState) const>;",
                "rusty::Cell<ConnectionState> state_field;",
                "StateChangeCallback on_state_change;",
                "static ConnectionStateMachine new_();",
                "ConnectionState state() const;",
                "bool can_transition_to(ConnectionState new_state) const;",
                "bool transition_to(ConnectionState new_state) const;",
                "void force_state(ConnectionState new_state) const;",
                "void set_on_state_change(StateChangeCallback callback);",
                "bool is_connected() const;",
                "bool is_failed() const;",
                "bool is_terminal() const;",
                "bool can_connect() const;",
                "bool is_usable() const;",
                "static bool is_valid_transition(ConnectionState from, ConnectionState to);",
                "export std::string_view connection_state_to_string(ConnectionState state);",
                ".on_state_change = rusty::default_like<StateChangeCallback>()",
                "rusty::is_empty(this->on_state_change)",
            }
        ),
        symbols=frozenset(
            ("T", symbol)
            for symbol in {
                "srpc::connection_state_to_string@srpc.connection_state(srpc::ConnectionState@srpc.connection_state)",
                "srpc::ConnectionStateMachine@srpc.connection_state::new_()",
                "srpc::ConnectionStateMachine@srpc.connection_state::state() const",
                "srpc::ConnectionStateMachine@srpc.connection_state::can_transition_to(srpc::ConnectionState@srpc.connection_state) const",
                "srpc::ConnectionStateMachine@srpc.connection_state::transition_to(srpc::ConnectionState@srpc.connection_state) const",
                "srpc::ConnectionStateMachine@srpc.connection_state::force_state(srpc::ConnectionState@srpc.connection_state) const",
                "srpc::ConnectionStateMachine@srpc.connection_state::set_on_state_change(rusty::Function<void (srpc::ConnectionState@srpc.connection_state, srpc::ConnectionState@srpc.connection_state) const>)",
                "srpc::ConnectionStateMachine@srpc.connection_state::is_connected() const",
                "srpc::ConnectionStateMachine@srpc.connection_state::is_failed() const",
                "srpc::ConnectionStateMachine@srpc.connection_state::is_terminal() const",
                "srpc::ConnectionStateMachine@srpc.connection_state::can_connect() const",
                "srpc::ConnectionStateMachine@srpc.connection_state::is_usable() const",
                "srpc::ConnectionStateMachine@srpc.connection_state::is_valid_transition(srpc::ConnectionState@srpc.connection_state, srpc::ConnectionState@srpc.connection_state)",
            }
        ),
    ),
    "srpc.heartbeat": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.heartbeat;",
                "export using HeartbeatTimeoutCallback = rusty::Function<void()>;",
                "export uint64_t heartbeat_time_us();",
                "export struct HeartbeatConfig",
                "bool enabled;",
                "uint32_t interval_ms;",
                "uint32_t timeout_ms;",
                "uint32_t max_missed;",
                "static HeartbeatConfig new_();",
                "static HeartbeatConfig defaults();",
                "static HeartbeatConfig aggressive();",
                "static HeartbeatConfig relaxed();",
                "static HeartbeatConfig disabled();",
                "export struct HeartbeatManager",
                "rusty::Cell<HeartbeatConfig> config_field;",
                "rusty::Cell<uint64_t> last_send_time;",
                "rusty::Cell<uint64_t> last_recv_time;",
                "rusty::Cell<uint32_t> missed_count_field;",
                "rusty::Cell<bool> pending_pong;",
                "rusty::Cell<bool> timed_out;",
                "rusty::RefCell<HeartbeatTimeoutCallback> on_timeout;",
                "static HeartbeatManager new_(const HeartbeatConfig& config);",
                "void set_config(const HeartbeatConfig& config) const;",
                "void set_on_timeout(HeartbeatTimeoutCallback callback) const;",
                "bool should_send_heartbeat() const;",
                "void on_heartbeat_sent() const;",
                "void on_pong_received() const;",
                "bool check_timeout() const;",
                "uint32_t time_until_next_heartbeat_ms() const;",
                "bool is_timed_out() const;",
                "uint32_t missed_count() const;",
                "bool is_pending_pong() const;",
                "void reset() const;",
                "HeartbeatConfig config() const;",
                "return current_time_us();",
                ".on_timeout = rusty::RefCell<HeartbeatTimeoutCallback>::new_(rusty::default_like<HeartbeatTimeoutCallback>())",
                "rusty::is_empty(((*callback)))",
                "rusty::wrapping_sub(now",
                "rusty::wrapping_add(this->missed_count_field.get()",
            }
        ),
        symbols=frozenset(
            ("T", symbol)
            for symbol in {
                "srpc::heartbeat_time_us@srpc.heartbeat()",
                "srpc::HeartbeatConfig@srpc.heartbeat::new_()",
                "srpc::HeartbeatConfig@srpc.heartbeat::defaults()",
                "srpc::HeartbeatConfig@srpc.heartbeat::aggressive()",
                "srpc::HeartbeatConfig@srpc.heartbeat::relaxed()",
                "srpc::HeartbeatConfig@srpc.heartbeat::disabled()",
                "srpc::HeartbeatManager@srpc.heartbeat::new_(srpc::HeartbeatConfig@srpc.heartbeat const&)",
                "srpc::HeartbeatManager@srpc.heartbeat::set_config(srpc::HeartbeatConfig@srpc.heartbeat const&) const",
                "srpc::HeartbeatManager@srpc.heartbeat::set_on_timeout(rusty::Function<void ()>) const",
                "srpc::HeartbeatManager@srpc.heartbeat::should_send_heartbeat() const",
                "srpc::HeartbeatManager@srpc.heartbeat::on_heartbeat_sent() const",
                "srpc::HeartbeatManager@srpc.heartbeat::on_pong_received() const",
                "srpc::HeartbeatManager@srpc.heartbeat::check_timeout() const",
                "srpc::HeartbeatManager@srpc.heartbeat::time_until_next_heartbeat_ms() const",
                "srpc::HeartbeatManager@srpc.heartbeat::is_timed_out() const",
                "srpc::HeartbeatManager@srpc.heartbeat::missed_count() const",
                "srpc::HeartbeatManager@srpc.heartbeat::is_pending_pong() const",
                "srpc::HeartbeatManager@srpc.heartbeat::reset() const",
                "srpc::HeartbeatManager@srpc.heartbeat::config() const",
            }
        ),
    ),
    "srpc.load_balancer": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.load_balancer;",
                "export enum class LoadBalancingStrategy",
                "RANDOM = 0,",
                "ROUND_ROBIN = 1,",
                "LEAST_CONNECTIONS = 2,",
                "LEAST_LATENCY = 3",
                "// Rust-only trait import marker: using _ = rusty::LoadBalancerClient;",
                "// Rust-only trait import marker: using _ = rusty::LoadBalancerMetrics;",
                "export struct LoadBalancerState",
                "rusty::Cell<size_t> round_robin_index_field;",
                "static LoadBalancerState new_();",
                "size_t next_round_robin_index(size_t pool_size) const;",
                "void reset() const;",
                "export struct LoadBalancer",
                "template<typename ClientVec>",
                "static size_t select(LoadBalancingStrategy strategy, const ClientVec& clients, const LoadBalancerState& state, size_t rand_value);",
                "static size_t select_random(size_t pool_size, size_t rand_value);",
                "static size_t select_round_robin(size_t pool_size, const LoadBalancerState& state);",
                "export std::string_view load_balancing_strategy_to_string(LoadBalancingStrategy strategy);",
                "size_t lb_pool_size(const ClientVec& clients);",
                "size_t lb_select_least_connections(const ClientVec& clients);",
                "size_t lb_select_least_latency(const ClientVec& clients);",
                "rusty::wrapping_add(current",
            }
        ),
        symbols=frozenset(
            ("T", symbol)
            for symbol in {
                "srpc::load_balancing_strategy_to_string@srpc.load_balancer(srpc::LoadBalancingStrategy@srpc.load_balancer)",
                "srpc::LoadBalancerState@srpc.load_balancer::new_()",
                "srpc::LoadBalancerState@srpc.load_balancer::next_round_robin_index(unsigned long) const",
                "srpc::LoadBalancerState@srpc.load_balancer::reset() const",
                "srpc::LoadBalancer@srpc.load_balancer::select_random(unsigned long, unsigned long)",
                "srpc::LoadBalancer@srpc.load_balancer::select_round_robin(unsigned long, srpc::LoadBalancerState@srpc.load_balancer const&)",
            }
        ),
    ),
    "srpc.frame_codec": AbiSpec(
        surface=frozenset(
            {
                "#include <vector>",
                "#include <rusty/io.hpp>",
                "export module srpc.frame_codec;",
                "import srpc.internal_protocol;",
                "export enum class FrameDecodeStatus",
                "NeedMoreBytes = 0,",
                "Complete = 1,",
                "Malformed = 2",
                "using FrameBytes = std::vector<uint8_t>;",
                "export using FrameCursor = rusty::io::Cursor<FrameBytes>;",
                "export constexpr size_t kFrameHeaderSize",
                "export constexpr int32_t kMaxFramePayloadSize",
                "export struct FrameHeader",
                "int32_t payload_size;",
                "bool extended_header_flag;",
                "int32_t total_frame_size() const;",
                "export struct FrameView",
                "FrameHeader header;",
                "const uint8_t* payload;",
                "size_t payload_size;",
                "export struct FrameStreamReader",
                "FrameCursor cursor_;",
                "rusty::Cell<bool> noncopy_;",
                "static FrameStreamReader new_();",
                "void append(const uint8_t* data, size_t size);",
                "FrameDecodeStatus next_frame(FrameView& out_view) const;",
                "void consume_frame();",
                "void reset();",
                "size_t buffered_bytes() const;",
                "bool empty() const;",
                "export std::string_view frame_decode_status_to_string(FrameDecodeStatus status);",
                "export bool frame_codec_write_header(std::span<uint8_t> out_buf, int32_t payload_size, bool extended_header_flag);",
                "export FrameDecodeStatus frame_codec_peek_header(std::span<const uint8_t> buf, FrameHeader& out_header);",
                "export FrameCursor make_frame_cursor();",
                "export bool frame_codec_encode_into(FrameBytes& out, const uint8_t* payload, int32_t payload_size, bool extended_header_flag);",
                "export void fsr_append(FrameStreamReader& reader, const uint8_t* data, size_t size);",
                "export void fsr_consume_frame(FrameStreamReader& reader);",
                "rusty::saturating_add(this->payload_size",
                "static constexpr bool is_send = true;",
                "static constexpr bool is_sync = true;",
            }
        ),
        symbols=frozenset(
            {
                ("R", "srpc::kFrameHeaderSize@srpc.frame_codec"),
                ("R", "srpc::kMaxFramePayloadSize@srpc.frame_codec"),
                (
                    "T",
                    "srpc::FrameHeader@srpc.frame_codec::total_frame_size() const",
                ),
                (
                    "T",
                    "srpc::FrameStreamReader@srpc.frame_codec::append(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::FrameStreamReader@srpc.frame_codec::buffered_bytes() const",
                ),
                (
                    "T",
                    "srpc::FrameStreamReader@srpc.frame_codec::consume_frame()",
                ),
                ("T", "srpc::FrameStreamReader@srpc.frame_codec::empty() const"),
                ("T", "srpc::FrameStreamReader@srpc.frame_codec::new_()"),
                (
                    "T",
                    "srpc::FrameStreamReader@srpc.frame_codec::next_frame(srpc::FrameView@srpc.frame_codec&) const",
                ),
                ("T", "srpc::FrameStreamReader@srpc.frame_codec::reset()"),
                (
                    "T",
                    "srpc::frame_codec_encode_into@srpc.frame_codec(std::__1::vector<unsigned char, std::__1::allocator<unsigned char>>&, unsigned char const*, int, bool)",
                ),
                (
                    "T",
                    "srpc::frame_codec_peek_header@srpc.frame_codec(std::__1::span<unsigned char const, 18446744073709551615ul>, srpc::FrameHeader@srpc.frame_codec&)",
                ),
                (
                    "T",
                    "srpc::frame_codec_write_header@srpc.frame_codec(std::__1::span<unsigned char, 18446744073709551615ul>, int, bool)",
                ),
                (
                    "T",
                    "srpc::frame_decode_status_to_string@srpc.frame_codec(srpc::FrameDecodeStatus@srpc.frame_codec)",
                ),
                (
                    "T",
                    "srpc::fsr_append@srpc.frame_codec(srpc::FrameStreamReader@srpc.frame_codec&, unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::fsr_consume_frame@srpc.frame_codec(srpc::FrameStreamReader@srpc.frame_codec&)",
                ),
                ("T", "srpc::make_frame_cursor@srpc.frame_codec()"),
            }
        ),
    ),
    "srpc.utils": AbiSpec(
        surface=frozenset(
            {
                "#include <netdb.h>",
                "export module srpc.utils;",
                "import srpc.logging;",
                "export struct AddrInfo",
                "addrinfo* info_;",
                "rusty::Cell<bool> owned_;",
                "AddrInfo(AddrInfo&& other) noexcept",
                "AddrInfo& operator=(AddrInfo&& other) noexcept",
                "static AddrInfo new_();",
                "static AddrInfo adopt(addrinfo* info);",
                "addrinfo* get() const;",
                "bool valid() const;",
                "~AddrInfo() noexcept(false);",
                "export int32_t find_open_port();",
                "export std::string get_host_name();",
                "srpc::log_line(3, 0, rusty::ptr::null(), message);",
                "srpc::log_line(1, 0, rusty::ptr::null(), message);",
                "rusty::sys::env::hostname();",
                "utils_ffi::srpc_find_open_port();",
                "utils_ffi::freeaddrinfo(this->info_);",
            }
        ),
        symbols=frozenset(
            ("T", symbol)
            for symbol in {
                "srpc::AddrInfo@srpc.utils::new_()",
                "srpc::AddrInfo@srpc.utils::adopt(addrinfo*)",
                "srpc::AddrInfo@srpc.utils::AddrInfo(addrinfo*, rusty::Cell<bool>)",
                "srpc::AddrInfo@srpc.utils::AddrInfo(srpc::AddrInfo@srpc.utils&&)",
                "srpc::AddrInfo@srpc.utils::get() const",
                "srpc::AddrInfo@srpc.utils::operator=(srpc::AddrInfo@srpc.utils&&)",
                "srpc::AddrInfo@srpc.utils::rusty_mark_forgotten() const",
                "srpc::AddrInfo@srpc.utils::valid() const",
                "srpc::AddrInfo@srpc.utils::~AddrInfo()",
                "srpc::find_open_port@srpc.utils()",
                "srpc::get_host_name@srpc.utils()",
            }
        ),
    ),
    "srpc.basetypes": AbiSpec(
        surface=frozenset(
            {
                '#include "misc/srpc_timing.h"',
                "#include <rusty/sync/atomic.hpp>",
                "export module srpc.basetypes;",
                "export using i8 = int8_t;",
                "export using i16 = int16_t;",
                "export using i32 = int32_t;",
                "export using i64 = int64_t;",
                "export using rusty::sync::atomic::AtomicI64;",
                "export using rusty::sync::atomic::Ordering;",
                "export constexpr uint64_t SRPC_USEC_PER_SEC",
                "export struct SparseInt",
                "static size_t buf_size(uint8_t byte0);",
                "static size_t dump32(int32_t val, uint8_t* buf);",
                "static size_t dump64(int64_t val, uint8_t* buf);",
                "static int32_t load32(const uint8_t* buf);",
                "static int64_t load64(const uint8_t* buf);",
                "static size_t val_size(int64_t val);",
                "export struct v32",
                "int32_t val_field;",
                "static v32 new_(int32_t v);",
                "export struct v64",
                "int64_t val_field;",
                "static v64 new_(int64_t v);",
                "export struct Counter",
                "rusty::sync::atomic::AtomicI64 next_field;",
                "static Counter new_(int64_t start);",
                "int64_t peek_next() const;",
                "int64_t next(int64_t step) const;",
                "void reset(int64_t start) const;",
                "export struct Time",
                "static uint64_t now(bool accurate);",
                "static void sleep(uint64_t t);",
                "export struct Timer",
                "uint64_t begin_us;",
                "uint64_t end_us;",
                "static Timer new_();",
                "void start();",
                "void stop();",
                "void reset();",
                "double elapsed() const;",
                "export void abort_if_false(bool cond);",
                "std::abort();",
                "export uint64_t time_now_us(bool accurate);",
                "srpc_clock_monotonic_us();",
                "srpc_clock_realtime_coarse_us();",
                "srpc_gettimeofday_us();",
                "srpc_sleep_us(uint64_t microseconds);",
                "rusty::wrapping_sub(end,",
            }
        ),
        symbols=frozenset(
            {
                ("R", "srpc::SRPC_USEC_PER_SEC@srpc.basetypes"),
                ("T", "srpc::abort_if_false@srpc.basetypes(bool)"),
                ("T", "srpc::time_now_us@srpc.basetypes(bool)"),
                ("T", "srpc::SparseInt@srpc.basetypes::buf_size(unsigned char)"),
                ("T", "srpc::SparseInt@srpc.basetypes::dump32(int, unsigned char*)"),
                ("T", "srpc::SparseInt@srpc.basetypes::dump64(long, unsigned char*)"),
                ("T", "srpc::SparseInt@srpc.basetypes::load32(unsigned char const*)"),
                ("T", "srpc::SparseInt@srpc.basetypes::load64(unsigned char const*)"),
                ("T", "srpc::SparseInt@srpc.basetypes::val_size(long)"),
                ("T", "srpc::v32@srpc.basetypes::new_(int)"),
                ("T", "srpc::v32@srpc.basetypes::set(int)"),
                ("T", "srpc::v32@srpc.basetypes::get() const"),
                ("T", "srpc::v32@srpc.basetypes::val_size() const"),
                ("T", "srpc::v64@srpc.basetypes::new_(long)"),
                ("T", "srpc::v64@srpc.basetypes::set(long)"),
                ("T", "srpc::v64@srpc.basetypes::get() const"),
                ("T", "srpc::v64@srpc.basetypes::val_size() const"),
                ("T", "srpc::Counter@srpc.basetypes::new_(long)"),
                ("T", "srpc::Counter@srpc.basetypes::peek_next() const"),
                ("T", "srpc::Counter@srpc.basetypes::next(long) const"),
                ("T", "srpc::Counter@srpc.basetypes::reset(long) const"),
                ("T", "srpc::Time@srpc.basetypes::now(bool)"),
                ("T", "srpc::Time@srpc.basetypes::sleep(unsigned long)"),
                ("T", "srpc::Timer@srpc.basetypes::new_()"),
                ("T", "srpc::Timer@srpc.basetypes::start()"),
                ("T", "srpc::Timer@srpc.basetypes::stop()"),
                ("T", "srpc::Timer@srpc.basetypes::reset()"),
                ("T", "srpc::Timer@srpc.basetypes::elapsed() const"),
            }
        ),
    ),
    "srpc.request_queue": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.request_queue;",
                "import vec_port.vec;",
                "import srpc.circuit_breaker;",
                "export enum class OverflowStrategy",
                "export constexpr OverflowStrategy OverflowStrategy_DROP_OLDEST();",
                "export constexpr OverflowStrategy OverflowStrategy_DROP_NEWEST();",
                "export constexpr OverflowStrategy OverflowStrategy_FAIL_FAST();",
                "export using QueuedRequestCallback = rusty::Function<void(int32_t)>;",
                "export constexpr int32_t kRequestQueueRejectedError = static_cast<int32_t>(35);",
                "export constexpr int32_t kRequestQueueRejectedError = static_cast<int32_t>(11);",
                "export constexpr int32_t kRequestQueueExpiredError = static_cast<int32_t>(60);",
                "export constexpr int32_t kRequestQueueExpiredError = static_cast<int32_t>(110);",
                "export std::string_view overflow_strategy_to_string(OverflowStrategy strategy);",
                "export uint64_t queued_request_time_us();",
                "export void rq_invoke_callback_safely(QueuedRequestCallback callback, int32_t error);",
                "export struct QueuedRequest",
                "int64_t xid;",
                "int32_t rpc_id;",
                "uint64_t timestamp_us;",
                "uint32_t retry_count;",
                "QueuedRequestCallback callback;",
                "uint32_t ttl_ms;",
                "static QueuedRequest new_();",
                "bool is_expired() const;",
                "uint32_t age_ms() const;",
                "export struct RequestQueueConfig",
                "size_t max_size;",
                "uint32_t default_ttl_ms;",
                "OverflowStrategy overflow_strategy;",
                "bool enabled;",
                "static RequestQueueConfig new_();",
                "static RequestQueueConfig defaults();",
                "static RequestQueueConfig small();",
                "static RequestQueueConfig large();",
                "static RequestQueueConfig disabled();",
                "export struct RequestQueue",
                "rusty::Cell<RequestQueueConfig> config_;",
                "rusty::Mutex<rusty::VecDeque<QueuedRequest>> queue_;",
                "static RequestQueue new_();",
                "static RequestQueue with_config(RequestQueueConfig config);",
                "bool enqueue(QueuedRequest request) const;",
                "rusty::Option<QueuedRequest> dequeue();",
                "size_t expire_stale() const;",
                "size_t size() const;",
                "bool empty() const;",
                "bool full();",
                "size_t remaining_capacity();",
                "void clear_all(int32_t error_code) const;",
                "RequestQueueConfig config() const;",
                "bool enabled() const;",
                "size_t max_size() const;",
                "void update_config(RequestQueueConfig config) const;",
                "return current_time_us();",
                "rusty::wrapping_sub(::srpc::queued_request_time_us()",
                "catch_unwind(AssertUnwindSafe(",
            }
        ),
        symbols=frozenset(
            {
                ("R", "srpc::kRequestQueueRejectedError@srpc.request_queue"),
                ("R", "srpc::kRequestQueueExpiredError@srpc.request_queue"),
                *(
                    ("T", symbol)
                    for symbol in {
                        "srpc::overflow_strategy_to_string@srpc.request_queue(srpc::OverflowStrategy@srpc.request_queue)",
                        "srpc::queued_request_time_us@srpc.request_queue()",
                        "srpc::rq_invoke_callback_safely@srpc.request_queue(rusty::Function<void (int)>, int)",
                        "srpc::QueuedRequest@srpc.request_queue::new_()",
                        "srpc::QueuedRequest@srpc.request_queue::is_expired() const",
                        "srpc::QueuedRequest@srpc.request_queue::age_ms() const",
                        "srpc::RequestQueueConfig@srpc.request_queue::new_()",
                        "srpc::RequestQueueConfig@srpc.request_queue::defaults()",
                        "srpc::RequestQueueConfig@srpc.request_queue::small()",
                        "srpc::RequestQueueConfig@srpc.request_queue::large()",
                        "srpc::RequestQueueConfig@srpc.request_queue::disabled()",
                        "srpc::RequestQueue@srpc.request_queue::new_()",
                        "srpc::RequestQueue@srpc.request_queue::with_config(srpc::RequestQueueConfig@srpc.request_queue)",
                        "srpc::RequestQueue@srpc.request_queue::enqueue(srpc::QueuedRequest@srpc.request_queue) const",
                        "srpc::RequestQueue@srpc.request_queue::dequeue()",
                        "srpc::RequestQueue@srpc.request_queue::expire_stale() const",
                        "srpc::RequestQueue@srpc.request_queue::size() const",
                        "srpc::RequestQueue@srpc.request_queue::empty() const",
                        "srpc::RequestQueue@srpc.request_queue::full()",
                        "srpc::RequestQueue@srpc.request_queue::remaining_capacity()",
                        "srpc::RequestQueue@srpc.request_queue::clear_all(int) const",
                        "srpc::RequestQueue@srpc.request_queue::config() const",
                        "srpc::RequestQueue@srpc.request_queue::enabled() const",
                        "srpc::RequestQueue@srpc.request_queue::max_size() const",
                        "srpc::RequestQueue@srpc.request_queue::update_config(srpc::RequestQueueConfig@srpc.request_queue) const",
                    }
                ),
            }
        ),
    ),
    # Measured on the crate-generated object, not declared: nine provider-owned
    # strong entries plus the module initializer (which `module_symbols` drops
    # because it carries no `@module` attachment). `verify` is an exported
    # function TEMPLATE, so its instantiations belong to importers and never
    # appear here; the `debugging_ffi` extern "C" block is declarations only.
    "srpc.debugging": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.debugging;",
                "import vec_port.vec;",
                "namespace srpc {",
                "namespace debugging_ffi {",
                "export bool likely(bool value);",
                "export bool unlikely(bool value);",
                "export void print_stack_trace(FILE* stream = stderr);",
                "export void verify_failed(std::string_view file, uint32_t line);",
                "export template<typename Expr>",
                "void verify(const Expr& expr, const std::source_location& location = std::source_location::current());",
                "struct BtCapture {",
                "bool ok;",
                "rusty::Vec<std::string> symbols;",
                "static BtCapture new_();",
                "export FILE* srpc_stderr();",
                "export int32_t srpc_backtrace_capture(std::string::value_type*** out_symbols);",
                "export void srpc_backtrace_free(std::string::value_type** symbols);",
                "export int32_t fputs(const std::string::value_type* text, FILE* stream);",
            }
        ),
        symbols=frozenset(
            {
                ("T", "srpc::BtCapture@srpc.debugging::new_()"),
                ("T", "srpc::bt_capture@srpc.debugging()"),
                ("T", "srpc::bt_empty_string@srpc.debugging()"),
                ("T", "srpc::bt_index_prefix@srpc.debugging(int)"),
                (
                    "T",
                    "srpc::bt_render@srpc.debugging(srpc::BtCapture@srpc.debugging const&)",
                ),
                ("T", "srpc::likely@srpc.debugging(bool)"),
                ("T", "srpc::print_stack_trace@srpc.debugging(_IO_FILE*)"),
                ("T", "srpc::unlikely@srpc.debugging(bool)"),
                (
                    "T",
                    "srpc::verify_failed@srpc.debugging(std::__1::basic_string_view<char, "
                    "std::__1::char_traits<char>>, unsigned int)",
                ),
            }
        ),
    ),
    # Goal 0 complete. The nineteen entries below were MEASURED, not written:
    # each generated .cppm was precompiled and assembled standalone and the
    # object read with `llvm-nm --defined-only --demangle`, keeping only the
    # strong entries whose declared entity carries this module's attachment
    # (`symbol_owner_module`). Weak template/lambda instantiations, `U`
    # references, and the module initializer are excluded by construction --
    # the initializer has no `@module` suffix, so `module_symbols` drops it.
    # The surface fragments are likewise taken from the emitted interface:
    # the module declaration, its exact private imports, and every exported
    # declaration in the interface prologue.
    "srpc.serializable": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.serializable;",
                "namespace srpc {",
                "import srpc.basetypes;",
                "import srpc.debugging;",
                "import rusty;",
                "import std;",
                "export class SerializableBase;",
                "export class Deserialize;",
                "export struct BufferSink;",
                "export struct BufferSource;",
                "export struct FdSink;",
                "export struct FdSource;",
                "export struct BinaryWriteArchive;",
                "export class Serialize;",
                "export struct BinaryReadArchive;",
                "export class SourceBase;",
                "export class SinkBase;",
                "export struct SerializableRegistry;",
                "export using v32 = v32;",
                "export using v64 = v64;",
                "export using SinkProxy = rusty::Box<SinkBase>;",
                "export using SourceProxy = rusty::Box<SourceBase>;",
                "export using SerializableProxy = rusty::Arc<SerializableBase>;",
                "export using SerializableRegistryFactory = rusty::Function<SerializableProxy()>;",
                "export SinkProxy make_sink_proxy_buffer(BufferSink* sink);",
                "export SourceProxy make_source_proxy_buffer(BufferSource* source);",
                "export SinkProxy make_sink_proxy_fd(FdSink* sink);",
                "export SourceProxy make_source_proxy_fd(FdSource* source);",
                "export void serializable_registry_register_factory(int32_t kind, rusty::Function<SerializableProxy()> factory);",
                "export rusty::Arc<SerializableBase> serializable_registry_create_impl(int32_t kind);",
                "export bool serializable_registry_is_registered_impl(int32_t kind);",
                "export void serializable_registry_clear_impl();",
                "export void deserialize(v32& self_, BinaryReadArchive& ar);",
                "export void deserialize(v64& self_, BinaryReadArchive& ar);",
                "export void deserialize(int32_t& self_, BinaryReadArchive& ar);",
                "export void deserialize(int8_t& self_, BinaryReadArchive& ar);",
                "export void deserialize(int16_t& self_, BinaryReadArchive& ar);",
                "export void deserialize(int64_t& self_, BinaryReadArchive& ar);",
                "export void deserialize(uint8_t& self_, BinaryReadArchive& ar);",
                "export void deserialize(uint16_t& self_, BinaryReadArchive& ar);",
                "export void deserialize(uint32_t& self_, BinaryReadArchive& ar);",
                "export void deserialize(uint64_t& self_, BinaryReadArchive& ar);",
                "export void deserialize(double& self_, BinaryReadArchive& ar);",
                "export void deserialize(std::string& self_, BinaryReadArchive& ar);",
                "export void serialize(const v32& self_, BinaryWriteArchive& ar);",
                "export void serialize(const v64& self_, BinaryWriteArchive& ar);",
                "export void serialize(const int32_t& self_, BinaryWriteArchive& ar);",
                "export void serialize(const int8_t& self_, BinaryWriteArchive& ar);",
                "export void serialize(const int16_t& self_, BinaryWriteArchive& ar);",
                "export void serialize(const int64_t& self_, BinaryWriteArchive& ar);",
                "export void serialize(const uint8_t& self_, BinaryWriteArchive& ar);",
                "export void serialize(const uint16_t& self_, BinaryWriteArchive& ar);",
                "export void serialize(const uint32_t& self_, BinaryWriteArchive& ar);",
                "export void serialize(const uint64_t& self_, BinaryWriteArchive& ar);",
                "export void serialize(const double& self_, BinaryWriteArchive& ar);",
                "export void serialize(const std::string_view& self_, BinaryWriteArchive& ar);",
                "export void serialize(const std::string& self_, BinaryWriteArchive& ar);",
                "export using ::srpc::details::__ufcs_SerializableBase::save;",
                "export using ::srpc::details::__ufcs_SerializableBase::load;",
                "export using ::srpc::details::__ufcs_SerializableBase::kind;",
                "export using ::srpc::details::__ufcs_SerializableBase::payload_type_id;",
                "export void srpc_fd_write_all(int32_t fd, const void* pointer, size_t length);",
                "export size_t srpc_fd_read_upto(int32_t fd, void* pointer, size_t length);",
                "export template <class U> class SerializableBaseAdapter;",
                "export template <class U> class SerializableBaseAdapterRef;",
                "export template <class U> class SerializableBaseAdapterRefMut;",
                "export template <class U> class DeserializeAdapter;",
                "export template <class U> class DeserializeAdapterRef;",
                "export template <class U> class DeserializeAdapterRefMut;",
                "export template <class U> class SerializeAdapter;",
                "export template <class U> class SerializeAdapterRef;",
                "export template <class U> class SerializeAdapterRefMut;",
                "export template <class U> class SourceBaseAdapter;",
                "export template <class U> class SourceBaseAdapterRef;",
                "export template <class U> class SourceBaseAdapterRefMut;",
                "export template <class U> class SinkBaseAdapter;",
                "export template <class U> class SinkBaseAdapterRef;",
                "export template <class U> class SinkBaseAdapterRefMut;",
                "export void serialize();",
                "export void deserialize();",
            }
        ),
        symbols=frozenset(
            {
                (
                    "D",
                    "typeinfo for srpc::Deserialize@srpc.serializable",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapter@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRef@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializableBase@srpc.serializable",
                ),
                (
                    "D",
                    "typeinfo for srpc::Serialize@srpc.serializable",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapter@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRef@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SinkBase@srpc.serializable",
                ),
                (
                    "D",
                    "typeinfo for srpc::SinkBaseAdapter@srpc.serializable<srpc::BufferSink@srpc.serializable>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SinkBaseAdapter@srpc.serializable<srpc::FdSink@srpc.serializable>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SinkBaseAdapterRef@srpc.serializable<srpc::BufferSink@srpc.serializable>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SinkBaseAdapterRef@srpc.serializable<srpc::FdSink@srpc.serializable>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SinkBaseAdapterRefMut@srpc.serializable<srpc::BufferSink@srpc.serializable>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SinkBaseAdapterRefMut@srpc.serializable<srpc::FdSink@srpc.serializable>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SourceBase@srpc.serializable",
                ),
                (
                    "D",
                    "typeinfo for srpc::SourceBaseAdapter@srpc.serializable<srpc::BufferSource@srpc.serializable>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SourceBaseAdapter@srpc.serializable<srpc::FdSource@srpc.serializable>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SourceBaseAdapterRef@srpc.serializable<srpc::BufferSource@srpc.serializable>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SourceBaseAdapterRef@srpc.serializable<srpc::FdSource@srpc.serializable>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SourceBaseAdapterRefMut@srpc.serializable<srpc::BufferSource@srpc.serializable>",
                ),
                (
                    "D",
                    "typeinfo for srpc::SourceBaseAdapterRefMut@srpc.serializable<srpc::FdSource@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::Deserialize@srpc.serializable",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<double>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<int>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<long>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<short>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapter@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<double>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<int>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<long>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<short>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRef@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<double>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<int>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<long>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<short>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializableBase@srpc.serializable",
                ),
                (
                    "D",
                    "vtable for srpc::Serialize@srpc.serializable",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<double>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<int>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<long>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<short>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapter@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<double>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<int>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<long>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<short>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRef@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<double>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<int>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<long>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<short>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for srpc::SinkBase@srpc.serializable",
                ),
                (
                    "D",
                    "vtable for srpc::SinkBaseAdapter@srpc.serializable<srpc::BufferSink@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::SinkBaseAdapter@srpc.serializable<srpc::FdSink@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::SinkBaseAdapterRef@srpc.serializable<srpc::BufferSink@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::SinkBaseAdapterRef@srpc.serializable<srpc::FdSink@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::SinkBaseAdapterRefMut@srpc.serializable<srpc::BufferSink@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::SinkBaseAdapterRefMut@srpc.serializable<srpc::FdSink@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::SourceBase@srpc.serializable",
                ),
                (
                    "D",
                    "vtable for srpc::SourceBaseAdapter@srpc.serializable<srpc::BufferSource@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::SourceBaseAdapter@srpc.serializable<srpc::FdSource@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::SourceBaseAdapterRef@srpc.serializable<srpc::BufferSource@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::SourceBaseAdapterRef@srpc.serializable<srpc::FdSource@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::SourceBaseAdapterRefMut@srpc.serializable<srpc::BufferSource@srpc.serializable>",
                ),
                (
                    "D",
                    "vtable for srpc::SourceBaseAdapterRefMut@srpc.serializable<srpc::FdSource@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::Deserialize@srpc.serializable",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapter@srpc.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRef@srpc.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializableBase@srpc.serializable",
                ),
                (
                    "R",
                    "typeinfo name for srpc::Serialize@srpc.serializable",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapter@srpc.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRef@srpc.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<srpc::v32@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<srpc::v64@srpc.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SerializeAdapterRefMut@srpc.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SinkBase@srpc.serializable",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SinkBaseAdapter@srpc.serializable<srpc::BufferSink@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SinkBaseAdapter@srpc.serializable<srpc::FdSink@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SinkBaseAdapterRef@srpc.serializable<srpc::BufferSink@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SinkBaseAdapterRef@srpc.serializable<srpc::FdSink@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SinkBaseAdapterRefMut@srpc.serializable<srpc::BufferSink@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SinkBaseAdapterRefMut@srpc.serializable<srpc::FdSink@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SourceBase@srpc.serializable",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SourceBaseAdapter@srpc.serializable<srpc::BufferSource@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SourceBaseAdapter@srpc.serializable<srpc::FdSource@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SourceBaseAdapterRef@srpc.serializable<srpc::BufferSource@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SourceBaseAdapterRef@srpc.serializable<srpc::FdSource@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SourceBaseAdapterRefMut@srpc.serializable<srpc::BufferSource@srpc.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for srpc::SourceBaseAdapterRefMut@srpc.serializable<srpc::FdSource@srpc.serializable>",
                ),
                (
                    "T",
                    "srpc::BinaryReadArchive@srpc.serializable::read_exact(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::BinaryReadArchive@srpc.serializable::read_or_abort(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::BinaryWriteArchive@srpc.serializable::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::BufferSink@srpc.serializable::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::BufferSource@srpc.serializable::eof() const",
                ),
                (
                    "T",
                    "srpc::BufferSource@srpc.serializable::new_(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::BufferSource@srpc.serializable::pos() const",
                ),
                (
                    "T",
                    "srpc::BufferSource@srpc.serializable::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::BufferSource@srpc.serializable::remaining() const",
                ),
                (
                    "T",
                    "srpc::Deserialize@srpc.serializable::~Deserialize()",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<double>::DeserializeAdapter(double)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<double>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<double>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<double>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<int>::DeserializeAdapter(int)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<int>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<int>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<int>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<long>::DeserializeAdapter(long)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<long>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<long>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<long>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>::DeserializeAdapter(srpc::v32@srpc.basetypes)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>::DeserializeAdapter(srpc::v64@srpc.basetypes)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<short>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<short>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<short>::DeserializeAdapter(short)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<short>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<signed char>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<signed char>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<signed char>::DeserializeAdapter(signed char)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<signed char>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::DeserializeAdapter(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned char>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<unsigned char>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned char>::DeserializeAdapter(unsigned char)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned char>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned int>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<unsigned int>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned int>::DeserializeAdapter(unsigned int)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned int>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned long>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<unsigned long>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned long>::DeserializeAdapter(unsigned long)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned long>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned short>::DeserializeAdapter(srpc::DeserializeAdapter@srpc.serializable<unsigned short>&&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned short>::DeserializeAdapter(unsigned short)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapter@srpc.serializable<unsigned short>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<double>::DeserializeAdapterRef(double const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<double>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<int>::DeserializeAdapterRef(int const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<int>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<long>::DeserializeAdapterRef(long const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<long>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<srpc::v32@srpc.basetypes>::DeserializeAdapterRef(srpc::v32@srpc.basetypes const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<srpc::v32@srpc.basetypes>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<srpc::v64@srpc.basetypes>::DeserializeAdapterRef(srpc::v64@srpc.basetypes const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<srpc::v64@srpc.basetypes>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<short>::DeserializeAdapterRef(short const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<short>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<signed char>::DeserializeAdapterRef(signed char const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<signed char>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::DeserializeAdapterRef(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<unsigned char>::DeserializeAdapterRef(unsigned char const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<unsigned char>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<unsigned int>::DeserializeAdapterRef(unsigned int const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<unsigned int>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<unsigned long>::DeserializeAdapterRef(unsigned long const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<unsigned long>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<unsigned short>::DeserializeAdapterRef(unsigned short const&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRef@srpc.serializable<unsigned short>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<double>::DeserializeAdapterRefMut(double&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<double>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<int>::DeserializeAdapterRefMut(int&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<int>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<long>::DeserializeAdapterRefMut(long&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<long>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<srpc::v32@srpc.basetypes>::DeserializeAdapterRefMut(srpc::v32@srpc.basetypes&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<srpc::v32@srpc.basetypes>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<srpc::v64@srpc.basetypes>::DeserializeAdapterRefMut(srpc::v64@srpc.basetypes&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<srpc::v64@srpc.basetypes>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<short>::DeserializeAdapterRefMut(short&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<short>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<signed char>::DeserializeAdapterRefMut(signed char&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<signed char>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::DeserializeAdapterRefMut(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned char>::DeserializeAdapterRefMut(unsigned char&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned char>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned int>::DeserializeAdapterRefMut(unsigned int&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned int>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned long>::DeserializeAdapterRefMut(unsigned long&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned long>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned short>::DeserializeAdapterRefMut(unsigned short&)",
                ),
                (
                    "T",
                    "srpc::DeserializeAdapterRefMut@srpc.serializable<unsigned short>::deserialize(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(double&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(int&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(long&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(srpc::v32@srpc.basetypes&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(srpc::v64@srpc.basetypes&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(short&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(signed char&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(unsigned char&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(unsigned int&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(unsigned long&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Deserialize_::deserialize@srpc.serializable(unsigned short&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::FdSink@srpc.serializable::fd() const",
                ),
                (
                    "T",
                    "srpc::FdSink@srpc.serializable::new_(int)",
                ),
                (
                    "T",
                    "srpc::FdSink@srpc.serializable::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::FdSource@srpc.serializable::fd() const",
                ),
                (
                    "T",
                    "srpc::FdSource@srpc.serializable::new_(int)",
                ),
                (
                    "T",
                    "srpc::FdSource@srpc.serializable::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SerializableBase@srpc.serializable::~SerializableBase()",
                ),
                (
                    "T",
                    "srpc::SerializableRegistry@srpc.serializable::clear_for_testing()",
                ),
                (
                    "T",
                    "srpc::SerializableRegistry@srpc.serializable::create(int)",
                ),
                (
                    "T",
                    "srpc::SerializableRegistry@srpc.serializable::is_registered(int)",
                ),
                (
                    "T",
                    "srpc::Serialize@srpc.serializable::~Serialize()",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<double>::SerializeAdapter(double)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<double>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<double>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<double>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<int>::SerializeAdapter(int)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<int>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<int>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<int>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<long>::SerializeAdapter(long)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<long>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<long>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<long>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>::SerializeAdapter(srpc::v32@srpc.basetypes)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<srpc::v32@srpc.basetypes>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>::SerializeAdapter(srpc::v64@srpc.basetypes)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<srpc::v64@srpc.basetypes>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<short>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<short>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<short>::SerializeAdapter(short)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<short>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<signed char>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<signed char>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<signed char>::SerializeAdapter(signed char)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<signed char>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::SerializeAdapter(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::SerializeAdapter(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned char>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<unsigned char>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned char>::SerializeAdapter(unsigned char)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned char>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned int>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<unsigned int>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned int>::SerializeAdapter(unsigned int)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned int>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned long>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<unsigned long>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned long>::SerializeAdapter(unsigned long)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned long>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned short>::SerializeAdapter(srpc::SerializeAdapter@srpc.serializable<unsigned short>&&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned short>::SerializeAdapter(unsigned short)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapter@srpc.serializable<unsigned short>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<double>::SerializeAdapterRef(double const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<double>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<int>::SerializeAdapterRef(int const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<int>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<long>::SerializeAdapterRef(long const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<long>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<srpc::v32@srpc.basetypes>::SerializeAdapterRef(srpc::v32@srpc.basetypes const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<srpc::v32@srpc.basetypes>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<srpc::v64@srpc.basetypes>::SerializeAdapterRef(srpc::v64@srpc.basetypes const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<srpc::v64@srpc.basetypes>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<short>::SerializeAdapterRef(short const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<short>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<signed char>::SerializeAdapterRef(signed char const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<signed char>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::SerializeAdapterRef(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::SerializeAdapterRef(std::__1::basic_string_view<char, std::__1::char_traits<char>> const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<unsigned char>::SerializeAdapterRef(unsigned char const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<unsigned char>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<unsigned int>::SerializeAdapterRef(unsigned int const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<unsigned int>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<unsigned long>::SerializeAdapterRef(unsigned long const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<unsigned long>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<unsigned short>::SerializeAdapterRef(unsigned short const&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRef@srpc.serializable<unsigned short>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<double>::SerializeAdapterRefMut(double&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<double>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<int>::SerializeAdapterRefMut(int&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<int>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<long>::SerializeAdapterRefMut(long&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<long>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<srpc::v32@srpc.basetypes>::SerializeAdapterRefMut(srpc::v32@srpc.basetypes&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<srpc::v32@srpc.basetypes>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<srpc::v64@srpc.basetypes>::SerializeAdapterRefMut(srpc::v64@srpc.basetypes&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<srpc::v64@srpc.basetypes>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<short>::SerializeAdapterRefMut(short&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<short>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<signed char>::SerializeAdapterRefMut(signed char&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<signed char>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::SerializeAdapterRefMut(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::SerializeAdapterRefMut(std::__1::basic_string_view<char, std::__1::char_traits<char>>&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<unsigned char>::SerializeAdapterRefMut(unsigned char&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<unsigned char>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<unsigned int>::SerializeAdapterRefMut(unsigned int&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<unsigned int>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<unsigned long>::SerializeAdapterRefMut(unsigned long&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<unsigned long>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<unsigned short>::SerializeAdapterRefMut(unsigned short&)",
                ),
                (
                    "T",
                    "srpc::SerializeAdapterRefMut@srpc.serializable<unsigned short>::serialize(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(double const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(int const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(long const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(srpc::v32@srpc.basetypes const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(srpc::v64@srpc.basetypes const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(short const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(signed char const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(std::__1::basic_string_view<char, std::__1::char_traits<char>> const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(unsigned char const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(unsigned int const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(unsigned long const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::Serialize_::serialize@srpc.serializable(unsigned short const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::SinkBase@srpc.serializable::~SinkBase()",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapter@srpc.serializable<srpc::BufferSink@srpc.serializable>::SinkBaseAdapter(srpc::BufferSink@srpc.serializable)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapter@srpc.serializable<srpc::BufferSink@srpc.serializable>::SinkBaseAdapter(srpc::SinkBaseAdapter@srpc.serializable<srpc::BufferSink@srpc.serializable>&&)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapter@srpc.serializable<srpc::BufferSink@srpc.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapter@srpc.serializable<srpc::FdSink@srpc.serializable>::SinkBaseAdapter(srpc::FdSink@srpc.serializable)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapter@srpc.serializable<srpc::FdSink@srpc.serializable>::SinkBaseAdapter(srpc::SinkBaseAdapter@srpc.serializable<srpc::FdSink@srpc.serializable>&&)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapter@srpc.serializable<srpc::FdSink@srpc.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapterRef@srpc.serializable<srpc::BufferSink@srpc.serializable>::SinkBaseAdapterRef(srpc::BufferSink@srpc.serializable const&)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapterRef@srpc.serializable<srpc::BufferSink@srpc.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapterRef@srpc.serializable<srpc::FdSink@srpc.serializable>::SinkBaseAdapterRef(srpc::FdSink@srpc.serializable const&)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapterRef@srpc.serializable<srpc::FdSink@srpc.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapterRefMut@srpc.serializable<srpc::BufferSink@srpc.serializable>::SinkBaseAdapterRefMut(srpc::BufferSink@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapterRefMut@srpc.serializable<srpc::BufferSink@srpc.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapterRefMut@srpc.serializable<srpc::FdSink@srpc.serializable>::SinkBaseAdapterRefMut(srpc::FdSink@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::SinkBaseAdapterRefMut@srpc.serializable<srpc::FdSink@srpc.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SourceBase@srpc.serializable::~SourceBase()",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapter@srpc.serializable<srpc::BufferSource@srpc.serializable>::SourceBaseAdapter(srpc::BufferSource@srpc.serializable)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapter@srpc.serializable<srpc::BufferSource@srpc.serializable>::SourceBaseAdapter(srpc::SourceBaseAdapter@srpc.serializable<srpc::BufferSource@srpc.serializable>&&)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapter@srpc.serializable<srpc::BufferSource@srpc.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapter@srpc.serializable<srpc::FdSource@srpc.serializable>::SourceBaseAdapter(srpc::FdSource@srpc.serializable)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapter@srpc.serializable<srpc::FdSource@srpc.serializable>::SourceBaseAdapter(srpc::SourceBaseAdapter@srpc.serializable<srpc::FdSource@srpc.serializable>&&)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapter@srpc.serializable<srpc::FdSource@srpc.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapterRef@srpc.serializable<srpc::BufferSource@srpc.serializable>::SourceBaseAdapterRef(srpc::BufferSource@srpc.serializable const&)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapterRef@srpc.serializable<srpc::BufferSource@srpc.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapterRef@srpc.serializable<srpc::FdSource@srpc.serializable>::SourceBaseAdapterRef(srpc::FdSource@srpc.serializable const&)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapterRef@srpc.serializable<srpc::FdSource@srpc.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapterRefMut@srpc.serializable<srpc::BufferSource@srpc.serializable>::SourceBaseAdapterRefMut(srpc::BufferSource@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapterRefMut@srpc.serializable<srpc::BufferSource@srpc.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapterRefMut@srpc.serializable<srpc::FdSource@srpc.serializable>::SourceBaseAdapterRefMut(srpc::FdSource@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::SourceBaseAdapterRefMut@srpc.serializable<srpc::FdSource@srpc.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::make_sink_proxy_buffer@srpc.serializable(srpc::BufferSink@srpc.serializable*)",
                ),
                (
                    "T",
                    "srpc::make_sink_proxy_fd@srpc.serializable(srpc::FdSink@srpc.serializable*)",
                ),
                (
                    "T",
                    "srpc::make_source_proxy_buffer@srpc.serializable(srpc::BufferSource@srpc.serializable*)",
                ),
                (
                    "T",
                    "srpc::make_source_proxy_fd@srpc.serializable(srpc::FdSource@srpc.serializable*)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(double&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(int&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(long&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(srpc::v32@srpc.basetypes&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(srpc::v64@srpc.basetypes&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(short&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(signed char&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(unsigned char&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(unsigned int&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(unsigned long&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::deserialize@srpc.serializable(unsigned short&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(double const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(int const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(long const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(srpc::v32@srpc.basetypes const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(srpc::v64@srpc.basetypes const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(short const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(signed char const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(std::__1::basic_string_view<char, std::__1::char_traits<char>> const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(unsigned char const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(unsigned int const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(unsigned long const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::rusty_ext::serialize@srpc.serializable(unsigned short const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::serializable_registry_clear_impl@srpc.serializable()",
                ),
                (
                    "T",
                    "srpc::serializable_registry_create_impl@srpc.serializable(int)",
                ),
                (
                    "T",
                    "srpc::serializable_registry_is_registered_impl@srpc.serializable(int)",
                ),
                (
                    "T",
                    "srpc::serializable_registry_register_factory@srpc.serializable(int, rusty::Function<rusty::Arc<srpc::SerializableBase@srpc.serializable> ()>)",
                ),
            }
        ),
    ),
    "srpc.serializable_envelope": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.serializable_envelope;",
                "namespace srpc {",
                "import srpc.basetypes;",
                "import srpc.debugging;",
                "import srpc.serializable;",
            }
        ),
        symbols=frozenset(
            {
            }
        ),
    ),
    "srpc.future": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.future;",
                "namespace srpc {",
                "import srpc.reactor;",
                "import std;",
            }
        ),
        symbols=frozenset(
            {
            }
        ),
    ),
    "srpc.logging": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.logging;",
                "namespace srpc {",
                "import srpc.debugging;",
                "import std;",
                "export struct Log;",
                "export extern rusty::sync::atomic::AtomicI32 LOG_LEVEL_S;",
                "export std::string_view log_level_tag(int32_t level);",
                "export void log_line(int32_t level, int32_t line, const int8_t* file, const std::string& msg);",
                "export void log_sink_write(const std::string& line);",
                "export std::string log_basename(const int8_t* fpath);",
                "export std::string log_time_now();",
                "export const std::string::value_type* srpc_path_basename(const std::string::value_type* path);",
                "export void srpc_time_now_str(std::string::value_type* now);",
            }
        ),
        symbols=frozenset(
            {
                (
                    "T",
                    "srpc::Log@srpc.logging::level_now()",
                ),
                (
                    "T",
                    "srpc::Log@srpc.logging::set_level(int)",
                ),
                (
                    "T",
                    "srpc::log_basename@srpc.logging(signed char const*)",
                ),
                (
                    "T",
                    "srpc::log_level_tag@srpc.logging(int)",
                ),
                (
                    "T",
                    "srpc::log_line@srpc.logging(int, int, signed char const*, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::log_sink_write@srpc.logging(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::log_time_now@srpc.logging()",
                ),
            }
        ),
    ),
    "srpc.idempotency": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.idempotency;",
                "namespace srpc {",
                "import vec_port.vec;",
                "import srpc.serializable;",
                "export struct IdempotencyKey;",
                "export struct IdempotencyKeyHash;",
                "export struct IdempotencyConfig;",
                "export struct CachedResponse;",
                "export struct IdempotencyKeyGenerator;",
                "export struct IdempotencyCache;",
                "export void cached_response_set(CachedResponse& entry, const rusty::Vec<uint8_t>& bytes);",
                "export void cached_response_get(const CachedResponse& entry, rusty::Vec<uint8_t>& out);",
            }
        ),
        symbols=frozenset(
            {
                (
                    "T",
                    "srpc::CachedResponse@srpc.idempotency::is_expired(unsigned long, unsigned long) const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::clear() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::config() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::enabled() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::evict_expired(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::evictions() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::hit_rate() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::hits() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::lookup(srpc::IdempotencyKey@srpc.idempotency const&, unsigned long, int&, rusty::port::vec::Vec@vec_port.vec<unsigned char, rusty::alloc::Global>&) const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::misses() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::new_()",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::remove(srpc::IdempotencyKey@srpc.idempotency const&) const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::reset_stats() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::set_config(srpc::IdempotencyConfig@srpc.idempotency const&) const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::size() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::store(srpc::IdempotencyKey@srpc.idempotency const&, int, rusty::port::vec::Vec@vec_port.vec<unsigned char, rusty::alloc::Global> const&, unsigned long) const",
                ),
                (
                    "T",
                    "srpc::IdempotencyCache@srpc.idempotency::with_config(srpc::IdempotencyConfig@srpc.idempotency)",
                ),
                (
                    "T",
                    "srpc::IdempotencyConfig@srpc.idempotency::defaults()",
                ),
                (
                    "T",
                    "srpc::IdempotencyConfig@srpc.idempotency::disabled()",
                ),
                (
                    "T",
                    "srpc::IdempotencyConfig@srpc.idempotency::large()",
                ),
                (
                    "T",
                    "srpc::IdempotencyConfig@srpc.idempotency::new_()",
                ),
                (
                    "T",
                    "srpc::IdempotencyConfig@srpc.idempotency::small()",
                ),
                (
                    "T",
                    "srpc::IdempotencyKey@srpc.idempotency::empty()",
                ),
                (
                    "T",
                    "srpc::IdempotencyKey@srpc.idempotency::is_valid() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyKey@srpc.idempotency::new_(unsigned long, unsigned long)",
                ),
                (
                    "T",
                    "srpc::IdempotencyKey@srpc.idempotency::operator==(srpc::IdempotencyKey@srpc.idempotency const&) const",
                ),
                (
                    "T",
                    "srpc::IdempotencyKeyGenerator@srpc.idempotency::client_id() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyKeyGenerator@srpc.idempotency::current_sequence() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyKeyGenerator@srpc.idempotency::new_(unsigned long)",
                ),
                (
                    "T",
                    "srpc::IdempotencyKeyGenerator@srpc.idempotency::next() const",
                ),
                (
                    "T",
                    "srpc::IdempotencyKeyGenerator@srpc.idempotency::set_client_id(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::IdempotencyKeyHash@srpc.idempotency::hash_one(srpc::IdempotencyKey@srpc.idempotency const&) const",
                ),
                (
                    "T",
                    "srpc::cached_response_get@srpc.idempotency(srpc::CachedResponse@srpc.idempotency const&, rusty::port::vec::Vec@vec_port.vec<unsigned char, rusty::alloc::Global>&)",
                ),
                (
                    "T",
                    "srpc::cached_response_set@srpc.idempotency(srpc::CachedResponse@srpc.idempotency&, rusty::port::vec::Vec@vec_port.vec<unsigned char, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "srpc::deserialize@srpc.idempotency(srpc::IdempotencyKey@srpc.idempotency&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::serialize@srpc.idempotency(srpc::IdempotencyKey@srpc.idempotency const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
            }
        ),
    ),
    "srpc.fiber": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.fiber;",
                "namespace srpc {",
                "import rc_port;",
                "import srpc.basetypes;",
                "import srpc.reactor;",
                "export uint64_t get_id();",
                "export bool in_fiber_context();",
                "export void yield();",
                "export void sleep_us(uint64_t microseconds);",
                "export void sleep_ms(uint64_t milliseconds);",
                "export void sleep_s(uint64_t seconds);",
                "export void sleep_until_us(uint64_t abs_time_us);",
                "export rusty::Option<rusty::Rc<Fiber>> current();",
            }
        ),
        symbols=frozenset(
            {
                (
                    "T",
                    "srpc::this_fiber::current@srpc.fiber()",
                ),
                (
                    "T",
                    "srpc::this_fiber::get_id@srpc.fiber()",
                ),
                (
                    "T",
                    "srpc::this_fiber::in_fiber_context@srpc.fiber()",
                ),
                (
                    "T",
                    "srpc::this_fiber::sleep_ms@srpc.fiber(unsigned long)",
                ),
                (
                    "T",
                    "srpc::this_fiber::sleep_s@srpc.fiber(unsigned long)",
                ),
                (
                    "T",
                    "srpc::this_fiber::sleep_until_us@srpc.fiber(unsigned long)",
                ),
                (
                    "T",
                    "srpc::this_fiber::sleep_us@srpc.fiber(unsigned long)",
                ),
                (
                    "T",
                    "srpc::this_fiber::yield@srpc.fiber()",
                ),
            }
        ),
    ),
    "srpc.misc": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.misc;",
                "namespace srpc {",
                "export class Job;",
                "export struct OneTimeJob;",
                "export int32_t get_ncpu();",
                "export std::string format_thousands(double val);",
                "export bool Ready(OneTimeJob& self_);",
                "export void Work(OneTimeJob& self_);",
                "export bool Done(OneTimeJob& self_);",
                "export int32_t srpc_get_ncpu();",
                "export int32_t srpc_format_fixed_2(double value, int8_t* output, size_t capacity);",
                "export template <class U> class JobAdapter;",
                "export template <class U> class JobAdapterRef;",
                "export template <class U> class JobAdapterRefMut;",
            }
        ),
        symbols=frozenset(
            {
                (
                    "D",
                    "typeinfo for srpc::Job@srpc.misc",
                ),
                (
                    "D",
                    "typeinfo for srpc::OneTimeJob@srpc.misc",
                ),
                (
                    "D",
                    "vtable for srpc::Job@srpc.misc",
                ),
                (
                    "D",
                    "vtable for srpc::OneTimeJob@srpc.misc",
                ),
                (
                    "R",
                    "typeinfo name for srpc::Job@srpc.misc",
                ),
                (
                    "R",
                    "typeinfo name for srpc::OneTimeJob@srpc.misc",
                ),
                (
                    "T",
                    "srpc::Job@srpc.misc::~Job()",
                ),
                (
                    "T",
                    "srpc::Job_::Done@srpc.misc(srpc::OneTimeJob@srpc.misc&)",
                ),
                (
                    "T",
                    "srpc::Job_::Ready@srpc.misc(srpc::OneTimeJob@srpc.misc&)",
                ),
                (
                    "T",
                    "srpc::Job_::Work@srpc.misc(srpc::OneTimeJob@srpc.misc&)",
                ),
                (
                    "T",
                    "srpc::OneTimeJob@srpc.misc::Done()",
                ),
                (
                    "T",
                    "srpc::OneTimeJob@srpc.misc::OneTimeJob(bool, bool, rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "srpc::OneTimeJob@srpc.misc::OneTimeJob(srpc::OneTimeJob@srpc.misc&&)",
                ),
                (
                    "T",
                    "srpc::OneTimeJob@srpc.misc::Ready()",
                ),
                (
                    "T",
                    "srpc::OneTimeJob@srpc.misc::Work()",
                ),
                (
                    "T",
                    "srpc::OneTimeJob@srpc.misc::new_(rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "srpc::format_thousands@srpc.misc(double)",
                ),
                (
                    "T",
                    "srpc::get_ncpu@srpc.misc()",
                ),
            }
        ),
    ),
    "srpc.channel": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.channel;",
                "namespace srpc {",
                "import srpc.callback_wrapper;",
                "export enum class ChannelError : int32_t;",
                "export constexpr ChannelError ChannelError_None();",
                "export constexpr ChannelError ChannelError_WouldBlock();",
                "export constexpr ChannelError ChannelError_ConnectionRefused();",
                "export constexpr ChannelError ChannelError_ConnectionReset();",
                "export constexpr ChannelError ChannelError_Timeout();",
                "export constexpr ChannelError ChannelError_AddressInUse();",
                "export constexpr ChannelError ChannelError_AddressInvalid();",
                "export constexpr ChannelError ChannelError_PermissionDenied();",
                "export constexpr ChannelError ChannelError_TooManyOpenFiles();",
                "export constexpr ChannelError ChannelError_Internal();",
                "export struct ChannelFrame;",
                "export class ChannelFactoryBase;",
                "export class ChannelListenerBase;",
                "export struct ConnectResult;",
                "export class ChannelConnectionBase;",
                "export using OnFrameCallback = ::srpc::detail::CallbackWrapper<rusty::Function<void(const ChannelFrame&) const>>;",
                "export using OnClosedCallback = ::srpc::detail::CallbackWrapper<rusty::Function<void(ChannelError) const>>;",
                "export using OnErrorCallback = ::srpc::detail::CallbackWrapper<rusty::Function<void(ChannelError, std::string_view) const>>;",
                "export using ChannelConnectionProxy = rusty::Box<ChannelConnectionBase>;",
                "export using OnAcceptCallback = ::srpc::detail::CallbackWrapper<rusty::Function<void(ChannelConnectionProxy) const>>;",
                "export using ChannelListenerProxy = rusty::Box<ChannelListenerBase>;",
                "export using ChannelFactoryProxy = rusty::Box<ChannelFactoryBase>;",
                "export std::string_view channel_error_to_string(ChannelError error);",
                "export template <class U> class ChannelFactoryBaseAdapter;",
                "export template <class U> class ChannelFactoryBaseAdapterRef;",
                "export template <class U> class ChannelFactoryBaseAdapterRefMut;",
                "export template <class U> class ChannelListenerBaseAdapter;",
                "export template <class U> class ChannelListenerBaseAdapterRef;",
                "export template <class U> class ChannelListenerBaseAdapterRefMut;",
                "export template <class U> class ChannelConnectionBaseAdapter;",
                "export template <class U> class ChannelConnectionBaseAdapterRef;",
                "export template <class U> class ChannelConnectionBaseAdapterRefMut;",
            }
        ),
        symbols=frozenset(
            {
                (
                    "D",
                    "typeinfo for srpc::ChannelConnectionBase@srpc.channel",
                ),
                (
                    "D",
                    "typeinfo for srpc::ChannelFactoryBase@srpc.channel",
                ),
                (
                    "D",
                    "typeinfo for srpc::ChannelListenerBase@srpc.channel",
                ),
                (
                    "D",
                    "vtable for srpc::ChannelConnectionBase@srpc.channel",
                ),
                (
                    "D",
                    "vtable for srpc::ChannelFactoryBase@srpc.channel",
                ),
                (
                    "D",
                    "vtable for srpc::ChannelListenerBase@srpc.channel",
                ),
                (
                    "R",
                    "typeinfo name for srpc::ChannelConnectionBase@srpc.channel",
                ),
                (
                    "R",
                    "typeinfo name for srpc::ChannelFactoryBase@srpc.channel",
                ),
                (
                    "R",
                    "typeinfo name for srpc::ChannelListenerBase@srpc.channel",
                ),
                (
                    "T",
                    "srpc::ChannelConnectionBase@srpc.channel::~ChannelConnectionBase()",
                ),
                (
                    "T",
                    "srpc::ChannelFactoryBase@srpc.channel::~ChannelFactoryBase()",
                ),
                (
                    "T",
                    "srpc::ChannelListenerBase@srpc.channel::~ChannelListenerBase()",
                ),
                (
                    "T",
                    "srpc::channel_error_to_string@srpc.channel(srpc::ChannelError@srpc.channel)",
                ),
            }
        ),
    ),
    "srpc.epoll_wrapper": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.epoll_wrapper;",
                "namespace srpc {",
                "import rusty;",
                "export class Pollable;",
                "export struct Epoll;",
                "export extern rusty::sync::atomic::AtomicI32 epoll_remove_count;",
                "export constexpr int32_t READ = static_cast<int32_t>(1);",
                "export constexpr int32_t WRITE = static_cast<int32_t>(2);",
                "export constexpr int32_t NO_CHANGE = static_cast<int32_t>(-1);",
                "export constexpr int32_t READABLE = static_cast<int32_t>(1);",
                "export constexpr int32_t WRITABLE = static_cast<int32_t>(2);",
                "export constexpr int32_t ERROR = static_cast<int32_t>(4);",
                "export void epoll_bump_remove_count();",
                "export template <class U> class PollableAdapter;",
                "export template <class U> class PollableAdapterRef;",
                "export template <class U> class PollableAdapterRefMut;",
                "export int32_t epoll_open();",
                "export int32_t epoll_add_impl(int32_t poll_fd, int32_t fd, int32_t poll_mode);",
                "export int32_t epoll_remove_impl(int32_t poll_fd, int32_t fd);",
                "export int32_t epoll_update_impl(int32_t poll_fd, int32_t fd, int32_t new_mode, int32_t old_mode);",
                "export int32_t epoll_wait(int32_t epoll_fd, EpollWaitEvent* events, int32_t max_events, int32_t timeout_ms);",
            }
        ),
        symbols=frozenset(
            {
                (
                    "D",
                    "typeinfo for srpc::Pollable@srpc.epoll_wrapper",
                ),
                (
                    "D",
                    "vtable for srpc::Pollable@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "srpc::LINUX_EPOLLERR@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "srpc::LINUX_EPOLLHUP@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "srpc::LINUX_EPOLLIN@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "srpc::LINUX_EPOLLOUT@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "srpc::LINUX_EPOLLRDHUP@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "srpc::PollMode::NO_CHANGE@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "srpc::PollMode::READ@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "srpc::PollMode::WRITE@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "srpc::PollReady::ERROR@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "srpc::PollReady::READABLE@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "srpc::PollReady::WRITABLE@srpc.epoll_wrapper",
                ),
                (
                    "R",
                    "typeinfo name for srpc::Pollable@srpc.epoll_wrapper",
                ),
                (
                    "T",
                    "srpc::Epoll@srpc.epoll_wrapper::Add(int, int)",
                ),
                (
                    "T",
                    "srpc::Epoll@srpc.epoll_wrapper::Remove(int)",
                ),
                (
                    "T",
                    "srpc::Epoll@srpc.epoll_wrapper::Update(int, int, int)",
                ),
                (
                    "T",
                    "srpc::Epoll@srpc.epoll_wrapper::fd() const",
                ),
                (
                    "T",
                    "srpc::Epoll@srpc.epoll_wrapper::new_()",
                ),
                (
                    "T",
                    "srpc::EpollWaitEvent@srpc.epoll_wrapper::default_()",
                ),
                (
                    "T",
                    "srpc::Pollable@srpc.epoll_wrapper::~Pollable()",
                ),
                (
                    "T",
                    "srpc::epoll_bump_remove_count@srpc.epoll_wrapper()",
                ),
            }
        ),
    ),
    "srpc.pollable_proxy": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.pollable_proxy;",
                "namespace srpc {",
                "export class PollableBase;",
                "export using PollableProxy = rusty::Box<PollableBase>;",
                "export template <class U> class PollableBaseAdapter;",
                "export template <class U> class PollableBaseAdapterRef;",
                "export template <class U> class PollableBaseAdapterRefMut;",
            }
        ),
        symbols=frozenset(
            {
                (
                    "D",
                    "typeinfo for srpc::PollableBase@srpc.pollable_proxy",
                ),
                (
                    "D",
                    "vtable for srpc::PollableBase@srpc.pollable_proxy",
                ),
                (
                    "R",
                    "typeinfo name for srpc::PollableBase@srpc.pollable_proxy",
                ),
                (
                    "T",
                    "srpc::PollableBase@srpc.pollable_proxy::~PollableBase()",
                ),
            }
        ),
    ),
    "srpc.callbacks": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.callbacks;",
                "namespace srpc {",
                "import vec_port.vec;",
                "import srpc.errors;",
                "export struct ConnectionCallbacks;",
                "export struct CallbackManager;",
                "export using ConnectionCallback = rusty::Arc<rusty::Function<void() const>>;",
                "export using ErrorCallback = rusty::Arc<rusty::Function<void(LegacyRpcError, const std::string&) const>>;",
                "export using ReconnectCallback = rusty::Arc<rusty::Function<void(bool) const>>;",
            }
        ),
        symbols=frozenset(
            {
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::add_on_connected(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::add_on_disconnected(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::add_on_error(rusty::Function<void (srpc::RpcError@srpc.errors, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const>) const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::add_on_reconnected(rusty::Function<void (bool) const>) const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::add_on_reconnecting(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::callback_count() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::clear_all() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::has_callbacks() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::inflight_enter() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::inflight_exit() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::invoke_on_connected() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::invoke_on_disconnected() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::invoke_on_error(srpc::RpcError@srpc.errors, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::invoke_on_reconnected(bool) const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::invoke_on_reconnecting() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::new_()",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::on_connected_count() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::on_disconnected_count() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::on_error_count() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::on_reconnected_count() const",
                ),
                (
                    "T",
                    "srpc::CallbackManager@srpc.callbacks::on_reconnecting_count() const",
                ),
                (
                    "T",
                    "srpc::ConnectionCallbacks@srpc.callbacks::clear()",
                ),
                (
                    "T",
                    "srpc::ConnectionCallbacks@srpc.callbacks::new_()",
                ),
                (
                    "T",
                    "srpc::ConnectionCallbacks@srpc.callbacks::total_count() const",
                ),
                (
                    "T",
                    "srpc::invoke_connection_callback_safely@srpc.callbacks(rusty::Arc<rusty::Function<void () const>> const&)",
                ),
                (
                    "T",
                    "srpc::invoke_error_callback_safely@srpc.callbacks(rusty::Arc<rusty::Function<void (srpc::RpcError@srpc.errors, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const>> const&, srpc::RpcError@srpc.errors, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::invoke_reconnect_callback_safely@srpc.callbacks(rusty::Arc<rusty::Function<void (bool) const>> const&, bool)",
                ),
            }
        ),
    ),
    "srpc.inmemory_channel": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.inmemory_channel;",
                "namespace srpc {",
                "import vec_port.vec;",
                "import std_port;",
                "import srpc.channel;",
                "export struct InMemoryConnectionStateInner;",
                "export struct InMemoryConnectionState;",
                "export struct InMemoryChannel;",
                "export struct InMemoryChannelShim;",
                "export struct InMemoryListenerInnerState;",
                "export struct InMemorySwitchboard;",
                "export struct InMemoryListener;",
                "export struct InMemoryListenerShim;",
                "export struct InMemoryFactory;",
                "export struct InMemoryFactoryShim;",
                "export ::srpc::ChannelError inmemory_channel_send_frame(const InMemoryChannel& channel, const ::srpc::ChannelFrame& frame);",
                "export void inmemory_channel_inject_drop_next_sends(const InMemoryChannel& channel, int32_t count);",
                "export void inmemory_channel_inject_send_error(const InMemoryChannel& channel, ::srpc::ChannelError error, int32_t count);",
                "export void inmemory_channel_clear_fault_injection(const InMemoryChannel& channel);",
                "export ::srpc::ChannelConnectionProxy make_inmemory_channel_proxy(rusty::Arc<InMemoryChannel> connection);",
                "export rusty::Option<rusty::Arc<InMemoryChannel>> inmemory_listener_accept_for_connect(const InMemoryListener& listener, const std::string& client_address);",
                "export ::srpc::ChannelListenerProxy make_inmemory_listener_proxy(rusty::Arc<InMemoryListener> listener);",
                "export ::srpc::ConnectResult inmemory_factory_connect(const InMemoryFactory& factory, std::string_view address);",
                "export rusty::Option<::srpc::ChannelListenerProxy> inmemory_factory_make_listener(const InMemoryFactory& factory);",
                "export ::srpc::ChannelFactoryProxy make_inmemory_factory_proxy(rusty::Arc<InMemoryFactory> factory);",
                "export std::tuple<rusty::Arc<InMemoryChannel>, rusty::Arc<InMemoryChannel>> make_channel_pair_for_testing(std::string a_address, std::string b_address);",
            }
        ),
        symbols=frozenset(
            {
                (
                    "D",
                    "typeinfo for srpc::InMemoryChannelShim@srpc.inmemory_channel",
                ),
                (
                    "D",
                    "typeinfo for srpc::InMemoryFactoryShim@srpc.inmemory_channel",
                ),
                (
                    "D",
                    "typeinfo for srpc::InMemoryListenerShim@srpc.inmemory_channel",
                ),
                (
                    "D",
                    "vtable for srpc::InMemoryChannelShim@srpc.inmemory_channel",
                ),
                (
                    "D",
                    "vtable for srpc::InMemoryFactoryShim@srpc.inmemory_channel",
                ),
                (
                    "D",
                    "vtable for srpc::InMemoryListenerShim@srpc.inmemory_channel",
                ),
                (
                    "R",
                    "typeinfo name for srpc::InMemoryChannelShim@srpc.inmemory_channel",
                ),
                (
                    "R",
                    "typeinfo name for srpc::InMemoryFactoryShim@srpc.inmemory_channel",
                ),
                (
                    "R",
                    "typeinfo name for srpc::InMemoryListenerShim@srpc.inmemory_channel",
                ),
                (
                    "T",
                    "srpc::InMemoryChannel@srpc.inmemory_channel::close() const",
                ),
                (
                    "T",
                    "srpc::InMemoryChannel@srpc.inmemory_channel::flush() const",
                ),
                (
                    "T",
                    "srpc::InMemoryChannel@srpc.inmemory_channel::is_closed() const",
                ),
                (
                    "T",
                    "srpc::InMemoryChannel@srpc.inmemory_channel::new_(rusty::Arc<srpc::InMemoryConnectionState@srpc.inmemory_channel>, bool)",
                ),
                (
                    "T",
                    "srpc::InMemoryChannel@srpc.inmemory_channel::peer_address() const",
                ),
                (
                    "T",
                    "srpc::InMemoryChannel@srpc.inmemory_channel::send_frame(srpc::ChannelFrame@srpc.channel const&) const",
                ),
                (
                    "T",
                    "srpc::InMemoryChannel@srpc.inmemory_channel::set_on_closed(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel) const>>) const",
                ),
                (
                    "T",
                    "srpc::InMemoryChannel@srpc.inmemory_channel::set_on_error(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>) const",
                ),
                (
                    "T",
                    "srpc::InMemoryChannel@srpc.inmemory_channel::set_on_frame(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelFrame@srpc.channel const&) const>>) const",
                ),
                (
                    "T",
                    "srpc::InMemoryChannelShim@srpc.inmemory_channel::InMemoryChannelShim(srpc::InMemoryChannelShim@srpc.inmemory_channel&&)",
                ),
                (
                    "T",
                    "srpc::InMemoryChannelShim@srpc.inmemory_channel::InMemoryChannelShim(rusty::Arc<srpc::InMemoryChannel@srpc.inmemory_channel>)",
                ),
                (
                    "T",
                    "srpc::InMemoryChannelShim@srpc.inmemory_channel::close()",
                ),
                (
                    "T",
                    "srpc::InMemoryChannelShim@srpc.inmemory_channel::flush()",
                ),
                (
                    "T",
                    "srpc::InMemoryChannelShim@srpc.inmemory_channel::is_closed() const",
                ),
                (
                    "T",
                    "srpc::InMemoryChannelShim@srpc.inmemory_channel::peer_address() const",
                ),
                (
                    "T",
                    "srpc::InMemoryChannelShim@srpc.inmemory_channel::send_frame(srpc::ChannelFrame@srpc.channel const&)",
                ),
                (
                    "T",
                    "srpc::InMemoryChannelShim@srpc.inmemory_channel::set_on_closed(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel) const>>)",
                ),
                (
                    "T",
                    "srpc::InMemoryChannelShim@srpc.inmemory_channel::set_on_error(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>)",
                ),
                (
                    "T",
                    "srpc::InMemoryChannelShim@srpc.inmemory_channel::set_on_frame(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelFrame@srpc.channel const&) const>>)",
                ),
                (
                    "T",
                    "srpc::InMemoryFactory@srpc.inmemory_channel::backend_name() const",
                ),
                (
                    "T",
                    "srpc::InMemoryFactory@srpc.inmemory_channel::connect(std::__1::basic_string_view<char, std::__1::char_traits<char>>) const",
                ),
                (
                    "T",
                    "srpc::InMemoryFactory@srpc.inmemory_channel::make_listener() const",
                ),
                (
                    "T",
                    "srpc::InMemoryFactory@srpc.inmemory_channel::new_(rusty::Arc<srpc::InMemorySwitchboard@srpc.inmemory_channel>)",
                ),
                (
                    "T",
                    "srpc::InMemoryFactoryShim@srpc.inmemory_channel::InMemoryFactoryShim(srpc::InMemoryFactoryShim@srpc.inmemory_channel&&)",
                ),
                (
                    "T",
                    "srpc::InMemoryFactoryShim@srpc.inmemory_channel::InMemoryFactoryShim(rusty::Arc<srpc::InMemoryFactory@srpc.inmemory_channel>)",
                ),
                (
                    "T",
                    "srpc::InMemoryFactoryShim@srpc.inmemory_channel::backend_name() const",
                ),
                (
                    "T",
                    "srpc::InMemoryFactoryShim@srpc.inmemory_channel::connect(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::InMemoryFactoryShim@srpc.inmemory_channel::make_listener()",
                ),
                (
                    "T",
                    "srpc::InMemoryListener@srpc.inmemory_channel::close() const",
                ),
                (
                    "T",
                    "srpc::InMemoryListener@srpc.inmemory_channel::is_closed() const",
                ),
                (
                    "T",
                    "srpc::InMemoryListener@srpc.inmemory_channel::listen(std::__1::basic_string_view<char, std::__1::char_traits<char>>) const",
                ),
                (
                    "T",
                    "srpc::InMemoryListener@srpc.inmemory_channel::local_address() const",
                ),
                (
                    "T",
                    "srpc::InMemoryListener@srpc.inmemory_channel::new_(rusty::Arc<srpc::InMemorySwitchboard@srpc.inmemory_channel>)",
                ),
                (
                    "T",
                    "srpc::InMemoryListener@srpc.inmemory_channel::set_on_accept(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>) const>>) const",
                ),
                (
                    "T",
                    "srpc::InMemoryListener@srpc.inmemory_channel::set_on_error(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>) const",
                ),
                (
                    "T",
                    "srpc::InMemoryListener@srpc.inmemory_channel::set_self_weak(rusty::sync::Weak<srpc::InMemoryListener@srpc.inmemory_channel>)",
                ),
                (
                    "T",
                    "srpc::InMemoryListenerShim@srpc.inmemory_channel::InMemoryListenerShim(srpc::InMemoryListenerShim@srpc.inmemory_channel&&)",
                ),
                (
                    "T",
                    "srpc::InMemoryListenerShim@srpc.inmemory_channel::InMemoryListenerShim(rusty::Arc<srpc::InMemoryListener@srpc.inmemory_channel>)",
                ),
                (
                    "T",
                    "srpc::InMemoryListenerShim@srpc.inmemory_channel::close()",
                ),
                (
                    "T",
                    "srpc::InMemoryListenerShim@srpc.inmemory_channel::is_closed() const",
                ),
                (
                    "T",
                    "srpc::InMemoryListenerShim@srpc.inmemory_channel::listen(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::InMemoryListenerShim@srpc.inmemory_channel::local_address() const",
                ),
                (
                    "T",
                    "srpc::InMemoryListenerShim@srpc.inmemory_channel::set_on_accept(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>) const>>)",
                ),
                (
                    "T",
                    "srpc::InMemoryListenerShim@srpc.inmemory_channel::set_on_error(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>)",
                ),
                (
                    "T",
                    "srpc::InMemorySwitchboard@srpc.inmemory_channel::find_listener(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "srpc::InMemorySwitchboard@srpc.inmemory_channel::new_()",
                ),
                (
                    "T",
                    "srpc::InMemorySwitchboard@srpc.inmemory_channel::register_listener(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, rusty::sync::Weak<srpc::InMemoryListener@srpc.inmemory_channel>) const",
                ),
                (
                    "T",
                    "srpc::InMemorySwitchboard@srpc.inmemory_channel::unregister_listener(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "srpc::channel_error_address_in_use@srpc.inmemory_channel()",
                ),
                (
                    "T",
                    "srpc::channel_error_connection_reset@srpc.inmemory_channel()",
                ),
                (
                    "T",
                    "srpc::channel_error_from_code@srpc.inmemory_channel(int)",
                ),
                (
                    "T",
                    "srpc::channel_error_internal@srpc.inmemory_channel()",
                ),
                (
                    "T",
                    "srpc::channel_error_none@srpc.inmemory_channel()",
                ),
                (
                    "T",
                    "srpc::empty_connection_inner@srpc.inmemory_channel()",
                ),
                (
                    "T",
                    "srpc::empty_listener_inner@srpc.inmemory_channel()",
                ),
                (
                    "T",
                    "srpc::inmemory_channel_clear_fault_injection@srpc.inmemory_channel(srpc::InMemoryChannel@srpc.inmemory_channel const&)",
                ),
                (
                    "T",
                    "srpc::inmemory_channel_inject_drop_next_sends@srpc.inmemory_channel(srpc::InMemoryChannel@srpc.inmemory_channel const&, int)",
                ),
                (
                    "T",
                    "srpc::inmemory_channel_inject_send_error@srpc.inmemory_channel(srpc::InMemoryChannel@srpc.inmemory_channel const&, srpc::ChannelError@srpc.channel, int)",
                ),
                (
                    "T",
                    "srpc::inmemory_channel_send_frame@srpc.inmemory_channel(srpc::InMemoryChannel@srpc.inmemory_channel const&, srpc::ChannelFrame@srpc.channel const&)",
                ),
                (
                    "T",
                    "srpc::inmemory_factory_connect@srpc.inmemory_channel(srpc::InMemoryFactory@srpc.inmemory_channel const&, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::inmemory_factory_make_listener@srpc.inmemory_channel(srpc::InMemoryFactory@srpc.inmemory_channel const&)",
                ),
                (
                    "T",
                    "srpc::inmemory_listener_accept_for_connect@srpc.inmemory_channel(srpc::InMemoryListener@srpc.inmemory_channel const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::inmemory_listener_listen_with_weak@srpc.inmemory_channel(srpc::InMemoryListener@srpc.inmemory_channel const&, std::__1::basic_string_view<char, std::__1::char_traits<char>>, rusty::Option<rusty::sync::Weak<srpc::InMemoryListener@srpc.inmemory_channel>>)",
                ),
                (
                    "T",
                    "srpc::make_channel_pair_for_testing@srpc.inmemory_channel(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "srpc::make_connection_state@srpc.inmemory_channel(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "srpc::make_inmemory_channel_proxy@srpc.inmemory_channel(rusty::Arc<srpc::InMemoryChannel@srpc.inmemory_channel>)",
                ),
                (
                    "T",
                    "srpc::make_inmemory_factory_proxy@srpc.inmemory_channel(rusty::Arc<srpc::InMemoryFactory@srpc.inmemory_channel>)",
                ),
                (
                    "T",
                    "srpc::make_inmemory_listener_proxy@srpc.inmemory_channel(rusty::Arc<srpc::InMemoryListener@srpc.inmemory_channel>)",
                ),
            }
        ),
    ),
    "srpc.fiber_channel": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.fiber_channel;",
                "namespace srpc {",
                "import vec_port.vec;",
                "import srpc.channel;",
                "import srpc.reactor;",
                "export struct OwnedFrame;",
                "export struct FiberChannel;",
            }
        ),
        symbols=frozenset(
            {
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::arm_waiter()",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::bind_callbacks()",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::channel_for_test()",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::close()",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::is_closed() const",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::new_(rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::on_inbound_closed()",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::on_inbound_frame(srpc::ChannelFrame@srpc.channel const&)",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::recv_frame()",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::send_frame(srpc::ChannelFrame@srpc.channel const&)",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::signal_pending_recv()",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::try_pop()",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::wait_for_signal()",
                ),
                (
                    "T",
                    "srpc::FiberChannel@srpc.fiber_channel::~FiberChannel()",
                ),
                (
                    "T",
                    "srpc::OwnedFrame@srpc.fiber_channel::default_()",
                ),
                (
                    "T",
                    "srpc::fiberchannel_owned_copy@srpc.fiber_channel(srpc::ChannelFrame@srpc.channel const&)",
                ),
            }
        ),
    ),
    "srpc.threading": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.threading;",
                "namespace srpc {",
                "import srpc.debugging;",
                "export struct SpinLock;",
                "export using AtomicBool = rusty::sync::atomic::AtomicBool;",
                "export using Ordering = rusty::sync::atomic::Ordering;",
                "export void cpu_pause();",
            }
        ),
        symbols=frozenset(
            {
                (
                    "T",
                    "srpc::Pthread_cond_broadcast@srpc.threading(pthread_cond_t*)",
                ),
                (
                    "T",
                    "srpc::Pthread_cond_destroy@srpc.threading(pthread_cond_t*)",
                ),
                (
                    "T",
                    "srpc::Pthread_cond_init@srpc.threading(pthread_cond_t*, pthread_condattr_t const*)",
                ),
                (
                    "T",
                    "srpc::Pthread_cond_signal@srpc.threading(pthread_cond_t*)",
                ),
                (
                    "T",
                    "srpc::Pthread_cond_wait@srpc.threading(pthread_cond_t*, pthread_mutex_t*)",
                ),
                (
                    "T",
                    "srpc::Pthread_mutex_destroy@srpc.threading(pthread_mutex_t*)",
                ),
                (
                    "T",
                    "srpc::Pthread_mutex_init@srpc.threading(pthread_mutex_t*, pthread_mutexattr_t const*)",
                ),
                (
                    "T",
                    "srpc::Pthread_mutex_lock@srpc.threading(pthread_mutex_t*)",
                ),
                (
                    "T",
                    "srpc::Pthread_mutex_unlock@srpc.threading(pthread_mutex_t*)",
                ),
                (
                    "T",
                    "srpc::Pthread_spin_destroy@srpc.threading(int volatile*)",
                ),
                (
                    "T",
                    "srpc::Pthread_spin_init@srpc.threading(int volatile*, int)",
                ),
                (
                    "T",
                    "srpc::Pthread_spin_lock@srpc.threading(int volatile*)",
                ),
                (
                    "T",
                    "srpc::Pthread_spin_unlock@srpc.threading(int volatile*)",
                ),
                (
                    "T",
                    "srpc::SpinLock@srpc.threading::lock() const",
                ),
                (
                    "T",
                    "srpc::SpinLock@srpc.threading::new_()",
                ),
                (
                    "T",
                    "srpc::SpinLock@srpc.threading::unlock() const",
                ),
                (
                    "T",
                    "srpc::cpu_pause@srpc.threading()",
                ),
            }
        ),
    ),
    "srpc.any_message": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.any_message;",
                "namespace srpc {",
                "import std_port;",
                "import srpc.debugging;",
                "import srpc.serializable;",
                "export struct AnyMessage;",
                "export using Factory = rusty::Function<rusty::Arc<SerializableBase>()>;",
                "export int32_t register_type(std::string name, std::type_index type_id, ::srpc::any_message_registry::Factory factory);",
                "export std::string name_for_type_owned(std::type_index type_id);",
                "export bool is_registered_name(const std::string& name);",
                "export bool is_registered_type(std::type_index type_id);",
                "export void clear_for_testing();",
                "export rusty::Option<rusty::Arc<SerializableBase>> create(const std::string& name);",
            }
        ),
        symbols=frozenset(
            {
                (
                    "T",
                    "srpc::AnyMessage@srpc.any_message::load(srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::AnyMessage@srpc.any_message::save(srpc::BinaryWriteArchive@srpc.serializable&) const",
                ),
                (
                    "T",
                    "srpc::any_message_registry::clear_for_testing@srpc.any_message()",
                ),
                (
                    "T",
                    "srpc::any_message_registry::create@srpc.any_message(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::any_message_registry::is_registered_name@srpc.any_message(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::any_message_registry::is_registered_type@srpc.any_message(std::__1::type_index)",
                ),
                (
                    "T",
                    "srpc::any_message_registry::name_for_type_owned@srpc.any_message(std::__1::type_index)",
                ),
                (
                    "T",
                    "srpc::any_message_registry::register_type@srpc.any_message(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, std::__1::type_index, rusty::Function<rusty::Arc<srpc::SerializableBase@srpc.serializable> ()>)",
                ),
                (
                    "T",
                    "srpc::deserialize@srpc.any_message(srpc::AnyMessage@srpc.any_message&, srpc::BinaryReadArchive@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::serialize@srpc.any_message(srpc::AnyMessage@srpc.any_message const&, srpc::BinaryWriteArchive@srpc.serializable&)",
                ),
            }
        ),
    ),
    "srpc.tcp_channel": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.tcp_channel;",
                "namespace srpc {",
                "import srpc.channel;",
                "import srpc.frame_codec;",
                "import srpc.pollable_proxy;",
                "import srpc.reactor;",
                "export struct TcpConnection;",
                "export struct TcpListener;",
                "export struct TcpFactory;",
                "export constexpr size_t kTcpConnectionOutboundHighWaterDefault = (static_cast<size_t>(4) * static_cast<size_t>(1024)) * static_cast<size_t>(1024);",
                "export ::srpc::ChannelConnectionProxy make_tcp_connection_channel_proxy(rusty::Arc<TcpConnection> conn);",
                "export ::srpc::ChannelListenerProxy make_tcp_listener_channel_proxy(rusty::Arc<TcpListener> listener);",
                "export ::srpc::ChannelFactoryProxy make_tcp_factory_proxy(rusty::Arc<TcpFactory> factory);",
                "export ::srpc::ConnectResult tcp_factory_connect(const TcpFactory& fac, std::string_view addr);",
                "export rusty::Option<::srpc::ChannelListenerProxy> tcp_factory_make_listener(const TcpFactory& self_);",
            }
        ),
        symbols=frozenset(
            {
                (
                    "D",
                    "typeinfo for srpc::TcpChannelShim@srpc.tcp_channel",
                ),
                (
                    "D",
                    "typeinfo for srpc::TcpFactoryShim@srpc.tcp_channel",
                ),
                (
                    "D",
                    "typeinfo for srpc::TcpListenerChannelShim@srpc.tcp_channel",
                ),
                (
                    "D",
                    "typeinfo for srpc::TcpListenerPollableShim@srpc.tcp_channel",
                ),
                (
                    "D",
                    "typeinfo for srpc::TcpPollableShim@srpc.tcp_channel",
                ),
                (
                    "D",
                    "vtable for srpc::TcpChannelShim@srpc.tcp_channel",
                ),
                (
                    "D",
                    "vtable for srpc::TcpFactoryShim@srpc.tcp_channel",
                ),
                (
                    "D",
                    "vtable for srpc::TcpListenerChannelShim@srpc.tcp_channel",
                ),
                (
                    "D",
                    "vtable for srpc::TcpListenerPollableShim@srpc.tcp_channel",
                ),
                (
                    "D",
                    "vtable for srpc::TcpPollableShim@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_ACCES@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_ADDR_IN_USE@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_ADDR_NOT_AVAILABLE@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_AGAIN@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_BROKEN_PIPE@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_CONNECTION_REFUSED@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_CONNECTION_RESET@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_HOST_UNREACHABLE@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_INTERRUPTED@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_NETWORK_UNREACHABLE@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_NOT_CONNECTED@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_OPERATION_NOT_PERMITTED@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_PROCESS_FD_LIMIT@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_SYSTEM_FD_LIMIT@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_TIMED_OUT@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_ERR_WOULD_BLOCK@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_MAX_FRAME_PAYLOAD_SIZE@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_POLL_NO_CHANGE@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_POLL_READ@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::TCP_POLL_WRITE@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::kRecvScratchBytes@srpc.tcp_channel",
                ),
                (
                    "R",
                    "srpc::kTcpConnectionOutboundHighWaterDefault@srpc.tcp_channel",
                ),
                (
                    "R",
                    "typeinfo name for srpc::TcpChannelShim@srpc.tcp_channel",
                ),
                (
                    "R",
                    "typeinfo name for srpc::TcpFactoryShim@srpc.tcp_channel",
                ),
                (
                    "R",
                    "typeinfo name for srpc::TcpListenerChannelShim@srpc.tcp_channel",
                ),
                (
                    "R",
                    "typeinfo name for srpc::TcpListenerPollableShim@srpc.tcp_channel",
                ),
                (
                    "R",
                    "typeinfo name for srpc::TcpPollableShim@srpc.tcp_channel",
                ),
                (
                    "T",
                    "srpc::TcpChannelShim@srpc.tcp_channel::TcpChannelShim(srpc::TcpChannelShim@srpc.tcp_channel&&)",
                ),
                (
                    "T",
                    "srpc::TcpChannelShim@srpc.tcp_channel::TcpChannelShim(rusty::Arc<srpc::TcpConnection@srpc.tcp_channel>)",
                ),
                (
                    "T",
                    "srpc::TcpChannelShim@srpc.tcp_channel::close()",
                ),
                (
                    "T",
                    "srpc::TcpChannelShim@srpc.tcp_channel::flush()",
                ),
                (
                    "T",
                    "srpc::TcpChannelShim@srpc.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "srpc::TcpChannelShim@srpc.tcp_channel::peer_address() const",
                ),
                (
                    "T",
                    "srpc::TcpChannelShim@srpc.tcp_channel::send_frame(srpc::ChannelFrame@srpc.channel const&)",
                ),
                (
                    "T",
                    "srpc::TcpChannelShim@srpc.tcp_channel::set_on_closed(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel) const>>)",
                ),
                (
                    "T",
                    "srpc::TcpChannelShim@srpc.tcp_channel::set_on_error(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>)",
                ),
                (
                    "T",
                    "srpc::TcpChannelShim@srpc.tcp_channel::set_on_frame(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelFrame@srpc.channel const&) const>>)",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::check_pending_write_update() const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::close() const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::content_size() const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::fd() const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::flush() const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::handle_error() const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::handle_read() const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::handle_write() const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::new_(int, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::peer_address() const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::poll_mode() const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::send_frame(srpc::ChannelFrame@srpc.channel const&) const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::set_on_closed(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel) const>>) const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::set_on_error(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>) const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::set_on_frame(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelFrame@srpc.channel const&) const>>) const",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::set_outbound_high_water(unsigned long)",
                ),
                (
                    "T",
                    "srpc::TcpConnection@srpc.tcp_channel::set_poll_thread(rusty::Arc<srpc::PollThread@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::TcpFactory@srpc.tcp_channel::backend_name() const",
                ),
                (
                    "T",
                    "srpc::TcpFactory@srpc.tcp_channel::connect(std::__1::basic_string_view<char, std::__1::char_traits<char>>) const",
                ),
                (
                    "T",
                    "srpc::TcpFactory@srpc.tcp_channel::make_listener() const",
                ),
                (
                    "T",
                    "srpc::TcpFactory@srpc.tcp_channel::new_(rusty::Arc<srpc::PollThread@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::TcpFactory@srpc.tcp_channel::set_connect_timeout_ms(int)",
                ),
                (
                    "T",
                    "srpc::TcpFactoryShim@srpc.tcp_channel::TcpFactoryShim(srpc::TcpFactoryShim@srpc.tcp_channel&&)",
                ),
                (
                    "T",
                    "srpc::TcpFactoryShim@srpc.tcp_channel::TcpFactoryShim(rusty::Arc<srpc::TcpFactory@srpc.tcp_channel>)",
                ),
                (
                    "T",
                    "srpc::TcpFactoryShim@srpc.tcp_channel::backend_name() const",
                ),
                (
                    "T",
                    "srpc::TcpFactoryShim@srpc.tcp_channel::connect(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::TcpFactoryShim@srpc.tcp_channel::make_listener()",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::check_pending_write_update() const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::close() const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::content_size() const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::fd() const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::handle_error() const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::handle_read() const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::handle_write() const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::listen(std::__1::basic_string_view<char, std::__1::char_traits<char>>) const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::local_address() const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::new_()",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::poll_mode() const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::set_on_accept(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>) const>>) const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::set_on_error(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>) const",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::set_poll_thread(rusty::Arc<srpc::PollThread@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::TcpListener@srpc.tcp_channel::set_self_weak(rusty::sync::Weak<srpc::TcpListener@srpc.tcp_channel>)",
                ),
                (
                    "T",
                    "srpc::TcpListenerChannelShim@srpc.tcp_channel::TcpListenerChannelShim(srpc::TcpListenerChannelShim@srpc.tcp_channel&&)",
                ),
                (
                    "T",
                    "srpc::TcpListenerChannelShim@srpc.tcp_channel::TcpListenerChannelShim(rusty::Arc<srpc::TcpListener@srpc.tcp_channel>)",
                ),
                (
                    "T",
                    "srpc::TcpListenerChannelShim@srpc.tcp_channel::close()",
                ),
                (
                    "T",
                    "srpc::TcpListenerChannelShim@srpc.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "srpc::TcpListenerChannelShim@srpc.tcp_channel::listen(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::TcpListenerChannelShim@srpc.tcp_channel::local_address() const",
                ),
                (
                    "T",
                    "srpc::TcpListenerChannelShim@srpc.tcp_channel::set_on_accept(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>) const>>)",
                ),
                (
                    "T",
                    "srpc::TcpListenerChannelShim@srpc.tcp_channel::set_on_error(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (srpc::ChannelError@srpc.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>)",
                ),
                (
                    "T",
                    "srpc::TcpListenerHandleReadScope@srpc.tcp_channel::TcpListenerHandleReadScope(srpc::TcpListenerHandleReadScope@srpc.tcp_channel&&)",
                ),
                (
                    "T",
                    "srpc::TcpListenerHandleReadScope@srpc.tcp_channel::TcpListenerHandleReadScope(rusty::sync::atomic::detail::Atomic<unsigned int> const*, bool)",
                ),
                (
                    "T",
                    "srpc::TcpListenerHandleReadScope@srpc.tcp_channel::acquired() const",
                ),
                (
                    "T",
                    "srpc::TcpListenerHandleReadScope@srpc.tcp_channel::new_(srpc::TcpListener@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::TcpListenerHandleReadScope@srpc.tcp_channel::operator=(srpc::TcpListenerHandleReadScope@srpc.tcp_channel&&)",
                ),
                (
                    "T",
                    "srpc::TcpListenerHandleReadScope@srpc.tcp_channel::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "srpc::TcpListenerHandleReadScope@srpc.tcp_channel::~TcpListenerHandleReadScope()",
                ),
                (
                    "T",
                    "srpc::TcpListenerPollableShim@srpc.tcp_channel::TcpListenerPollableShim(srpc::TcpListenerPollableShim@srpc.tcp_channel&&)",
                ),
                (
                    "T",
                    "srpc::TcpListenerPollableShim@srpc.tcp_channel::TcpListenerPollableShim(rusty::Arc<srpc::TcpListener@srpc.tcp_channel>)",
                ),
                (
                    "T",
                    "srpc::TcpListenerPollableShim@srpc.tcp_channel::check_pending_write_update() const",
                ),
                (
                    "T",
                    "srpc::TcpListenerPollableShim@srpc.tcp_channel::close()",
                ),
                (
                    "T",
                    "srpc::TcpListenerPollableShim@srpc.tcp_channel::content_size()",
                ),
                (
                    "T",
                    "srpc::TcpListenerPollableShim@srpc.tcp_channel::fd() const",
                ),
                (
                    "T",
                    "srpc::TcpListenerPollableShim@srpc.tcp_channel::handle_error()",
                ),
                (
                    "T",
                    "srpc::TcpListenerPollableShim@srpc.tcp_channel::handle_read()",
                ),
                (
                    "T",
                    "srpc::TcpListenerPollableShim@srpc.tcp_channel::handle_write()",
                ),
                (
                    "T",
                    "srpc::TcpListenerPollableShim@srpc.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "srpc::TcpListenerPollableShim@srpc.tcp_channel::poll_mode() const",
                ),
                (
                    "T",
                    "srpc::TcpPollableShim@srpc.tcp_channel::TcpPollableShim(srpc::TcpPollableShim@srpc.tcp_channel&&)",
                ),
                (
                    "T",
                    "srpc::TcpPollableShim@srpc.tcp_channel::TcpPollableShim(rusty::Arc<srpc::TcpConnection@srpc.tcp_channel>)",
                ),
                (
                    "T",
                    "srpc::TcpPollableShim@srpc.tcp_channel::check_pending_write_update() const",
                ),
                (
                    "T",
                    "srpc::TcpPollableShim@srpc.tcp_channel::close()",
                ),
                (
                    "T",
                    "srpc::TcpPollableShim@srpc.tcp_channel::content_size()",
                ),
                (
                    "T",
                    "srpc::TcpPollableShim@srpc.tcp_channel::fd() const",
                ),
                (
                    "T",
                    "srpc::TcpPollableShim@srpc.tcp_channel::handle_error()",
                ),
                (
                    "T",
                    "srpc::TcpPollableShim@srpc.tcp_channel::handle_read()",
                ),
                (
                    "T",
                    "srpc::TcpPollableShim@srpc.tcp_channel::handle_write()",
                ),
                (
                    "T",
                    "srpc::TcpPollableShim@srpc.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "srpc::TcpPollableShim@srpc.tcp_channel::poll_mode() const",
                ),
                (
                    "T",
                    "srpc::connect_errno_to_channel_error@srpc.tcp_channel(int)",
                ),
                (
                    "T",
                    "srpc::io_kind_to_channel_error@srpc.tcp_channel(rusty::io::Error::Kind)",
                ),
                (
                    "T",
                    "srpc::make_tcp_connection_channel_proxy@srpc.tcp_channel(rusty::Arc<srpc::TcpConnection@srpc.tcp_channel>)",
                ),
                (
                    "T",
                    "srpc::make_tcp_connection_pollable_proxy@srpc.tcp_channel(rusty::Arc<srpc::TcpConnection@srpc.tcp_channel>)",
                ),
                (
                    "T",
                    "srpc::make_tcp_factory_proxy@srpc.tcp_channel(rusty::Arc<srpc::TcpFactory@srpc.tcp_channel>)",
                ),
                (
                    "T",
                    "srpc::make_tcp_listener_channel_proxy@srpc.tcp_channel(rusty::Arc<srpc::TcpListener@srpc.tcp_channel>)",
                ),
                (
                    "T",
                    "srpc::make_tcp_listener_pollable_proxy@srpc.tcp_channel(rusty::Arc<srpc::TcpListener@srpc.tcp_channel>)",
                ),
                (
                    "T",
                    "srpc::set_nonblocking_fd@srpc.tcp_channel(int)",
                ),
                (
                    "T",
                    "srpc::tcp_factory_connect@srpc.tcp_channel(srpc::TcpFactory@srpc.tcp_channel const&, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::tcp_factory_connect_socket@srpc.tcp_channel(rusty::net::SocketAddrV4, int, srpc::ChannelError@srpc.channel&)",
                ),
                (
                    "T",
                    "srpc::tcp_factory_make_listener@srpc.tcp_channel(srpc::TcpFactory@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_append_inbound@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&, unsigned long)",
                ),
                (
                    "T",
                    "srpc::tcpconn_close@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_consume_inbound@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_deliver_on_closed_locked@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&, srpc::ChannelError@srpc.channel)",
                ),
                (
                    "T",
                    "srpc::tcpconn_drain_outbound_locked@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&, std::__1::vector<unsigned char, std::__1::allocator<unsigned char>>&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_drop_after_error@srpc.tcp_channel(std::__1::vector<unsigned char, std::__1::allocator<unsigned char>>&, unsigned long)",
                ),
                (
                    "T",
                    "srpc::tcpconn_errno_to_channel_error@srpc.tcp_channel(int)",
                ),
                (
                    "T",
                    "srpc::tcpconn_flush@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_handle_error@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_handle_read@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_handle_write@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_last_errno@srpc.tcp_channel()",
                ),
                (
                    "T",
                    "srpc::tcpconn_next_frame@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&, srpc::FrameView@srpc.frame_codec&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_recv_bytes@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&, srpc::RecvScratch@srpc.tcp_channel*)",
                ),
                (
                    "T",
                    "srpc::tcpconn_reset_fd@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_reset_inbound@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_scratch@srpc.tcp_channel()",
                ),
                (
                    "T",
                    "srpc::tcpconn_send_bytes@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&, std::__1::vector<unsigned char, std::__1::allocator<unsigned char>>&, unsigned long)",
                ),
                (
                    "T",
                    "srpc::tcpconn_send_frame@srpc.tcp_channel(srpc::TcpConnection@srpc.tcp_channel const&, srpc::ChannelFrame@srpc.channel const&)",
                ),
                (
                    "T",
                    "srpc::tcpconn_trim_sent@srpc.tcp_channel(std::__1::vector<unsigned char, std::__1::allocator<unsigned char>>&, unsigned long)",
                ),
                (
                    "T",
                    "srpc::tcplistener_accept_step@srpc.tcp_channel(srpc::TcpListener@srpc.tcp_channel const&, srpc::AcceptStep@srpc.tcp_channel*)",
                ),
                (
                    "T",
                    "srpc::tcplistener_accept_step_new@srpc.tcp_channel()",
                ),
                (
                    "T",
                    "srpc::tcplistener_close_accepted@srpc.tcp_channel(srpc::AcceptStep@srpc.tcp_channel&)",
                ),
                (
                    "T",
                    "srpc::tcplistener_handle_error@srpc.tcp_channel(srpc::TcpListener@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcplistener_handle_read@srpc.tcp_channel(srpc::TcpListener@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcplistener_is_bound@srpc.tcp_channel(srpc::TcpListener@srpc.tcp_channel const&)",
                ),
                (
                    "T",
                    "srpc::tcplistener_take_proxy@srpc.tcp_channel(srpc::AcceptStep@srpc.tcp_channel&)",
                ),
            }
        ),
    ),
    "srpc.reactor": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.reactor;",
                "namespace srpc {",
                "import vec_port.vec;",
                "import rc_port;",
                "import btree_port.btree.map;",
                "import btree_port.btree.set;",
                "import std_port;",
                "import srpc.basetypes;",
                "import srpc.debugging;",
                "import srpc.epoll_wrapper;",
                "import srpc.logging;",
                "import srpc.misc;",
                "import srpc.pollable_proxy;",
                "import std;",
                "export enum class EventStatus : int32_t;",
                "export constexpr EventStatus EventStatus_INIT();",
                "export constexpr EventStatus EventStatus_WAIT();",
                "export constexpr EventStatus EventStatus_READY();",
                "export constexpr EventStatus EventStatus_DONE();",
                "export constexpr EventStatus EventStatus_TIMEOUT();",
                "export constexpr EventStatus EventStatus_DEBUG();",
                "export struct StacklessTaskEntry;",
                "export struct StacklessCancelReport;",
                "export struct PollCommand_AddPollable;",
                "export struct PollCommand_RemovePollable;",
                "export struct PollCommand_ClosePollable;",
                "export struct PollCommand_UpdateMode;",
                "export struct PollCommand_AddJob;",
                "export struct PollCommand_RemoveJob;",
                "export struct PollCommand_Shutdown;",
                "export enum class FiberStatus : int32_t;",
                "export constexpr FiberStatus FiberStatus_INIT();",
                "export constexpr FiberStatus FiberStatus_STARTED();",
                "export constexpr FiberStatus FiberStatus_PAUSED();",
                "export constexpr FiberStatus FiberStatus_RESUMED();",
                "export constexpr FiberStatus FiberStatus_FINISHED();",
                "export constexpr FiberStatus FiberStatus_FINALIZING();",
                "export constexpr FiberStatus FiberStatus_RECYCLED();",
                "export enum class QuorumPolicy : int32_t;",
                "export constexpr QuorumPolicy QuorumPolicy_DEFAULT();",
                "export constexpr QuorumPolicy QuorumPolicy_ALL_NO();",
                "export constexpr QuorumPolicy QuorumPolicy_COMMITTED_SHORT();",
                "export constexpr QuorumPolicy QuorumPolicy_ALWAYS_READY();",
                "export class EventPollable;",
                "export struct PollThreadWorker;",
                "export struct PollThread;",
                "export struct EventState;",
                "export struct IntEvent;",
                "export struct SharedIntEvent;",
                "export struct NeverEvent;",
                "export struct TimeoutEvent;",
                "export struct WaitAny;",
                "export struct WaitAll;",
                "export struct fiber_yield_t;",
                "export struct fiber_task_t;",
                "export struct Fiber;",
                "export struct Reactor;",
                "export struct QuorumEvent;",
                "export struct QuorumEventWrapper;",
                "export using SrcFileCStr = const char*;",
                "export using EventTestFn = rusty::Function<bool(int32_t) const>;",
                "export using FiberFn = rusty::Function<void()>;",
                "export using FiberTaskFn = rusty::Function<void(fiber_yield_t&)>;",
                "export using StacklessPollFn = rusty::Function<bool(rusty::Context&)>;",
                "export using TaskVoid = rusty::Task<void>;",
                "export using PollCmdReceiver = rusty::sync::mpsc::Receiver<PollCommand>;",
                "export using FdPollableMap = rusty::HashMap<int32_t, ::srpc::PollableProxy>;",
                "export using FdModeMap = rusty::HashMap<int32_t, int32_t>;",
                "export using FdSet = rusty::HashSet<int32_t>;",
                "export using JobSet = std::set<rusty::Arc<Job>>;",
                "export using PollJoinSlot = rusty::Mutex<rusty::Option<rusty::thread::JoinHandle<rusty::Unit>>>;",
                "export using QuorumDanglingVec = rusty::Vec<std::pair<uint16_t, int64_t>>;",
                "export using QuorumFinalizeFn = rusty::Function<bool(QuorumDanglingVec&)>;",
                "export using StacklessProfileCountU64 = rusty::sync::atomic::AtomicU64;",
                "export using StacklessProfileCountUsize = rusty::sync::atomic::AtomicUsize;",
                "export constexpr size_t kDefaultStackBytes = static_cast<size_t>(1) << 20;",
                "export extern thread_local rusty::Option<rusty::Rc<Reactor>> sp_reactor_th_;",
                "export extern thread_local rusty::Option<rusty::Rc<Reactor>> sp_disk_reactor_th_;",
                "export extern thread_local rusty::RefCell<rusty::Option<rusty::Rc<Fiber>>> sp_running_fiber_th_;",
                "export extern thread_local uint64_t g_fiber_global_id;",
                "export extern thread_local rusty::HashMap<rusty::String, rusty::Vec<::srpc::PollableProxy>> reactor_clients_th_;",
                "export extern thread_local size_t reactor_prune_hwm_th_;",
                "export extern thread_local PollThreadWorker* g_current_poll_worker;",
                "export rusty::Arc<IntEvent> create_sp_int_event(int32_t target);",
                "export rusty::Arc<TimeoutEvent> create_sp_timeout_event(uint64_t wait_us);",
                "export rusty::Arc<NeverEvent> create_sp_never_event();",
                "export rusty::Arc<WaitAny> create_sp_waitany(rusty::Arc<EventPollable> a, rusty::Arc<EventPollable> b);",
                "export rusty::Arc<WaitAll> create_sp_waitall();",
                "export rusty::Arc<WaitAll> create_sp_waitall_from(const rusty::Vec<rusty::Arc<EventPollable>>& evs);",
                "export bool pollworker_is_on_poll_thread();",
                "export rusty::Rc<Fiber> fiber_create_run_impl(FiberFn func, const char* file, int64_t line);",
                "export void fiber_sleep(uint64_t microseconds);",
                "export extern \"C\" void fiber_task_entry_thunk(rusty::ffi::c_void* arg);",
                "export rusty::Arc<QuorumEvent> quorum_event_make(int32_t n_total, int32_t quorum);",
                "export rusty::Arc<QuorumEvent> create_sp_quorum_event(int32_t n_total, int32_t quorum);",
                "export bool test(const IntEvent& self_);",
                "export bool is_ready(const IntEvent& self_);",
                "export void log(const IntEvent& self_);",
                "export EventStatus status(const IntEvent& self_);",
                "export void set_status(const IntEvent& self_, EventStatus s);",
                "export uint64_t wakeup_time(const IntEvent& self_);",
                "export bool prunable(const IntEvent& self_);",
                "export void set_prunable(const IntEvent& self_, bool v);",
                "export rusty::Option<rusty::Rc<Fiber>> upgrade_fiber(const IntEvent& self_);",
                "export inline const rusty::Cell<EventStatus>& core_status(const IntEvent& self_);",
                "export inline rusty::thread::ThreadId core_owner_thread(const IntEvent& self_);",
                "export inline const EventState& core_state(const IntEvent& self_);",
                "export inline EventState& core_state_mut(IntEvent& self_);",
                "export inline const rusty::sync::Weak<EventPollable>& core_self(const IntEvent& self_);",
                "export inline rusty::sync::Weak<EventPollable>& core_self_mut(IntEvent& self_);",
                "export inline bool core_is_composite(const IntEvent& self_);",
                "export bool test(const NeverEvent& self_);",
                "export bool is_ready(const NeverEvent& self_);",
                "export void log(const NeverEvent& self_);",
                "export EventStatus status(const NeverEvent& self_);",
                "export void set_status(const NeverEvent& self_, EventStatus s);",
                "export uint64_t wakeup_time(const NeverEvent& self_);",
                "export bool prunable(const NeverEvent& self_);",
                "export void set_prunable(const NeverEvent& self_, bool v);",
                "export rusty::Option<rusty::Rc<Fiber>> upgrade_fiber(const NeverEvent& self_);",
                "export inline const rusty::Cell<EventStatus>& core_status(const NeverEvent& self_);",
                "export inline rusty::thread::ThreadId core_owner_thread(const NeverEvent& self_);",
                "export inline const EventState& core_state(const NeverEvent& self_);",
                "export inline EventState& core_state_mut(NeverEvent& self_);",
                "export inline const rusty::sync::Weak<EventPollable>& core_self(const NeverEvent& self_);",
                "export inline rusty::sync::Weak<EventPollable>& core_self_mut(NeverEvent& self_);",
                "export inline bool core_is_composite(const NeverEvent& self_);",
                "export bool test(const TimeoutEvent& self_);",
                "export bool is_ready(const TimeoutEvent& self_);",
                "export void log(const TimeoutEvent& self_);",
                "export EventStatus status(const TimeoutEvent& self_);",
                "export void set_status(const TimeoutEvent& self_, EventStatus s);",
                "export uint64_t wakeup_time(const TimeoutEvent& self_);",
                "export bool prunable(const TimeoutEvent& self_);",
                "export void set_prunable(const TimeoutEvent& self_, bool v);",
                "export rusty::Option<rusty::Rc<Fiber>> upgrade_fiber(const TimeoutEvent& self_);",
                "export inline const rusty::Cell<EventStatus>& core_status(const TimeoutEvent& self_);",
                "export inline rusty::thread::ThreadId core_owner_thread(const TimeoutEvent& self_);",
                "export inline const EventState& core_state(const TimeoutEvent& self_);",
                "export inline EventState& core_state_mut(TimeoutEvent& self_);",
                "export inline const rusty::sync::Weak<EventPollable>& core_self(const TimeoutEvent& self_);",
                "export inline rusty::sync::Weak<EventPollable>& core_self_mut(TimeoutEvent& self_);",
                "export inline bool core_is_composite(const TimeoutEvent& self_);",
                "export bool test(const WaitAny& self_);",
                "export bool is_ready(const WaitAny& self_);",
                "export void log(const WaitAny& self_);",
                "export EventStatus status(const WaitAny& self_);",
                "export void set_status(const WaitAny& self_, EventStatus s);",
                "export uint64_t wakeup_time(const WaitAny& self_);",
                "export bool prunable(const WaitAny& self_);",
                "export void set_prunable(const WaitAny& self_, bool v);",
                "export rusty::Option<rusty::Rc<Fiber>> upgrade_fiber(const WaitAny& self_);",
                "export inline const rusty::Cell<EventStatus>& core_status(const WaitAny& self_);",
                "export inline rusty::thread::ThreadId core_owner_thread(const WaitAny& self_);",
                "export inline const EventState& core_state(const WaitAny& self_);",
                "export inline EventState& core_state_mut(WaitAny& self_);",
                "export inline const rusty::sync::Weak<EventPollable>& core_self(const WaitAny& self_);",
                "export inline rusty::sync::Weak<EventPollable>& core_self_mut(WaitAny& self_);",
                "export inline bool core_is_composite(const WaitAny& self_);",
                "export bool test(const WaitAll& self_);",
                "export bool is_ready(const WaitAll& self_);",
                "export void log(const WaitAll& self_);",
                "export EventStatus status(const WaitAll& self_);",
                "export void set_status(const WaitAll& self_, EventStatus s);",
                "export uint64_t wakeup_time(const WaitAll& self_);",
                "export bool prunable(const WaitAll& self_);",
                "export void set_prunable(const WaitAll& self_, bool v);",
                "export rusty::Option<rusty::Rc<Fiber>> upgrade_fiber(const WaitAll& self_);",
                "export inline const rusty::Cell<EventStatus>& core_status(const WaitAll& self_);",
                "export inline rusty::thread::ThreadId core_owner_thread(const WaitAll& self_);",
                "export inline const EventState& core_state(const WaitAll& self_);",
                "export inline EventState& core_state_mut(WaitAll& self_);",
                "export inline const rusty::sync::Weak<EventPollable>& core_self(const WaitAll& self_);",
                "export inline rusty::sync::Weak<EventPollable>& core_self_mut(WaitAll& self_);",
                "export inline bool core_is_composite(const WaitAll& self_);",
                "export bool test(const QuorumEvent& self_);",
                "export bool is_ready(const QuorumEvent& self_);",
                "export void log(const QuorumEvent& self_);",
                "export EventStatus status(const QuorumEvent& self_);",
                "export void set_status(const QuorumEvent& self_, EventStatus s);",
                "export uint64_t wakeup_time(const QuorumEvent& self_);",
                "export bool prunable(const QuorumEvent& self_);",
                "export void set_prunable(const QuorumEvent& self_, bool v);",
                "export rusty::Option<rusty::Rc<Fiber>> upgrade_fiber(const QuorumEvent& self_);",
                "export inline const rusty::Cell<EventStatus>& core_status(const QuorumEvent& self_);",
                "export inline rusty::thread::ThreadId core_owner_thread(const QuorumEvent& self_);",
                "export inline const EventState& core_state(const QuorumEvent& self_);",
                "export inline EventState& core_state_mut(QuorumEvent& self_);",
                "export inline const rusty::sync::Weak<EventPollable>& core_self(const QuorumEvent& self_);",
                "export inline rusty::sync::Weak<EventPollable>& core_self_mut(QuorumEvent& self_);",
                "export inline bool core_is_composite(const QuorumEvent& self_);",
                "export struct PollCommand_Shutdown { static constexpr bool is_send = true; static constexpr bool is_sync = true; };",
                "export template <class U> class EventPollableAdapter;",
                "export template <class U> class EventPollableAdapterRef;",
                "export template <class U> class EventPollableAdapterRefMut;",
            }
        ),
        symbols=frozenset(
            {
                (
                    "D",
                    "typeinfo for janus::QuorumEvent@srpc.reactor",
                ),
                (
                    "D",
                    "typeinfo for srpc::EventPollable@srpc.reactor",
                ),
                (
                    "D",
                    "typeinfo for srpc::IntEvent@srpc.reactor",
                ),
                (
                    "D",
                    "typeinfo for srpc::NeverEvent@srpc.reactor",
                ),
                (
                    "D",
                    "typeinfo for srpc::TimeoutEvent@srpc.reactor",
                ),
                (
                    "D",
                    "typeinfo for srpc::WaitAll@srpc.reactor",
                ),
                (
                    "D",
                    "typeinfo for srpc::WaitAny@srpc.reactor",
                ),
                (
                    "D",
                    "vtable for janus::QuorumEvent@srpc.reactor",
                ),
                (
                    "D",
                    "vtable for srpc::EventPollable@srpc.reactor",
                ),
                (
                    "D",
                    "vtable for srpc::IntEvent@srpc.reactor",
                ),
                (
                    "D",
                    "vtable for srpc::NeverEvent@srpc.reactor",
                ),
                (
                    "D",
                    "vtable for srpc::TimeoutEvent@srpc.reactor",
                ),
                (
                    "D",
                    "vtable for srpc::WaitAll@srpc.reactor",
                ),
                (
                    "D",
                    "vtable for srpc::WaitAny@srpc.reactor",
                ),
                (
                    "R",
                    "srpc::STACKLESS_UNREGISTERED_SLOT@srpc.reactor",
                ),
                (
                    "R",
                    "srpc::kDefaultStackBytes@srpc.reactor",
                ),
                (
                    "R",
                    "typeinfo name for janus::QuorumEvent@srpc.reactor",
                ),
                (
                    "R",
                    "typeinfo name for srpc::EventPollable@srpc.reactor",
                ),
                (
                    "R",
                    "typeinfo name for srpc::IntEvent@srpc.reactor",
                ),
                (
                    "R",
                    "typeinfo name for srpc::NeverEvent@srpc.reactor",
                ),
                (
                    "R",
                    "typeinfo name for srpc::TimeoutEvent@srpc.reactor",
                ),
                (
                    "R",
                    "typeinfo name for srpc::WaitAll@srpc.reactor",
                ),
                (
                    "R",
                    "typeinfo name for srpc::WaitAny@srpc.reactor",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::QuorumEvent(janus::QuorumEvent@srpc.reactor&&)",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::QuorumEvent(rusty::Cell<srpc::EventStatus@srpc.reactor>, rusty::thread::ThreadId, srpc::EventState@srpc.reactor, rusty::Cell<bool>, rusty::sync::Weak<srpc::EventPollable@srpc.reactor>, rusty::Cell<int>, rusty::Cell<int>, rusty::RefCell<std_port::collections::hash::map::HashMap@std_port<unsigned short, long, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>>, int, int, rusty::Cell<janus::QuorumPolicy@srpc.reactor>, rusty::Cell<bool>, rusty::Cell<long>, rusty::Cell<bool>, rusty::Cell<unsigned int>, rusty::Cell<long>, rusty::Cell<unsigned long>, rusty::Arc<srpc::IntEvent@srpc.reactor>)",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::add_xid(unsigned short, long) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::finalize(unsigned long, rusty::Function<bool (rusty::port::vec::Vec@vec_port.vec<std::__1::pair<unsigned short, long>, rusty::alloc::Global>&)>) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::get_fiber_id() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::get_self() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::is_ready() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::is_slow() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::log() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::no() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::prunable() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::remove_xid(unsigned short) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::set_self(rusty::sync::Weak<srpc::EventPollable@srpc.reactor>)",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::set_status(srpc::EventStatus@srpc.reactor) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::status() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::test() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::vote_no() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::vote_yes() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::wait() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@srpc.reactor::yes() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::add_xid(unsigned short, long) const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::finalize(unsigned long, rusty::Function<bool (rusty::port::vec::Vec@vec_port.vec<std::__1::pair<unsigned short, long>, rusty::alloc::Global>&)>) const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::get_fiber_id() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::is_ready() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::is_slow() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::log() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::new_(int, int)",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::no() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::q() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::remove_xid(unsigned short) const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::test() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::vote_no() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::vote_yes() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::wait() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@srpc.reactor::yes() const",
                ),
                (
                    "T",
                    "janus::create_sp_quorum_event@srpc.reactor(int, int)",
                ),
                (
                    "T",
                    "janus::quorum_collect_dangling@srpc.reactor(janus::QuorumEvent@srpc.reactor const*)",
                ),
                (
                    "T",
                    "janus::quorum_event_finalize@srpc.reactor(janus::QuorumEvent@srpc.reactor const&, unsigned long, rusty::Function<bool (rusty::port::vec::Vec@vec_port.vec<std::__1::pair<unsigned short, long>, rusty::alloc::Global>&)>)",
                ),
                (
                    "T",
                    "janus::quorum_event_is_slow@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "janus::quorum_event_make@srpc.reactor(int, int)",
                ),
                (
                    "T",
                    "srpc::AddJob@srpc.reactor(rusty::Arc<srpc::Job@srpc.misc>)",
                ),
                (
                    "T",
                    "srpc::AddPollable@srpc.reactor(rusty::Box<srpc::PollableBase@srpc.pollable_proxy, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "srpc::ClosePollable@srpc.reactor(int)",
                ),
                (
                    "T",
                    "srpc::EventPollable@srpc.reactor::~EventPollable()",
                ),
                (
                    "T",
                    "srpc::EventPollable_::is_ready@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::is_ready@srpc.reactor(srpc::IntEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::is_ready@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::is_ready@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::is_ready@srpc.reactor(srpc::WaitAll@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::is_ready@srpc.reactor(srpc::WaitAny@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::log@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::log@srpc.reactor(srpc::IntEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::log@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::log@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::log@srpc.reactor(srpc::WaitAll@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::log@srpc.reactor(srpc::WaitAny@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::prunable@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::prunable@srpc.reactor(srpc::IntEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::prunable@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::prunable@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::prunable@srpc.reactor(srpc::WaitAll@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::prunable@srpc.reactor(srpc::WaitAny@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_prunable@srpc.reactor(janus::QuorumEvent@srpc.reactor const&, bool)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_prunable@srpc.reactor(srpc::IntEvent@srpc.reactor const&, bool)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_prunable@srpc.reactor(srpc::NeverEvent@srpc.reactor const&, bool)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_prunable@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&, bool)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_prunable@srpc.reactor(srpc::WaitAll@srpc.reactor const&, bool)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_prunable@srpc.reactor(srpc::WaitAny@srpc.reactor const&, bool)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_status@srpc.reactor(janus::QuorumEvent@srpc.reactor const&, srpc::EventStatus@srpc.reactor)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_status@srpc.reactor(srpc::IntEvent@srpc.reactor const&, srpc::EventStatus@srpc.reactor)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_status@srpc.reactor(srpc::NeverEvent@srpc.reactor const&, srpc::EventStatus@srpc.reactor)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_status@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&, srpc::EventStatus@srpc.reactor)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_status@srpc.reactor(srpc::WaitAll@srpc.reactor const&, srpc::EventStatus@srpc.reactor)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::set_status@srpc.reactor(srpc::WaitAny@srpc.reactor const&, srpc::EventStatus@srpc.reactor)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::status@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::status@srpc.reactor(srpc::IntEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::status@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::status@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::status@srpc.reactor(srpc::WaitAll@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::status@srpc.reactor(srpc::WaitAny@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::test@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::test@srpc.reactor(srpc::IntEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::test@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::test@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::test@srpc.reactor(srpc::WaitAll@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::test@srpc.reactor(srpc::WaitAny@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::upgrade_fiber@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::upgrade_fiber@srpc.reactor(srpc::IntEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::upgrade_fiber@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::upgrade_fiber@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::upgrade_fiber@srpc.reactor(srpc::WaitAll@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::upgrade_fiber@srpc.reactor(srpc::WaitAny@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::wakeup_time@srpc.reactor(janus::QuorumEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::wakeup_time@srpc.reactor(srpc::IntEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::wakeup_time@srpc.reactor(srpc::NeverEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::wakeup_time@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::wakeup_time@srpc.reactor(srpc::WaitAll@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventPollable_::wakeup_time@srpc.reactor(srpc::WaitAny@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::EventState@srpc.reactor::new_()",
                ),
                (
                    "T",
                    "srpc::Fiber@srpc.reactor::continue_() const",
                ),
                (
                    "T",
                    "srpc::Fiber@srpc.reactor::create_run_impl(rusty::Function<void ()>, char const*, long)",
                ),
                (
                    "T",
                    "srpc::Fiber@srpc.reactor::current_fiber()",
                ),
                (
                    "T",
                    "srpc::Fiber@srpc.reactor::finished() const",
                ),
                (
                    "T",
                    "srpc::Fiber@srpc.reactor::new_(rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "srpc::Fiber@srpc.reactor::run() const",
                ),
                (
                    "T",
                    "srpc::Fiber@srpc.reactor::sleep(unsigned long)",
                ),
                (
                    "T",
                    "srpc::Fiber@srpc.reactor::yield_() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::IntEvent(srpc::IntEvent@srpc.reactor&&)",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::IntEvent(rusty::Cell<srpc::EventStatus@srpc.reactor>, rusty::thread::ThreadId, srpc::EventState@srpc.reactor, rusty::Cell<bool>, rusty::sync::Weak<srpc::EventPollable@srpc.reactor>, rusty::Cell<int>, rusty::Cell<int>)",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::get() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::get_fiber_id() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::get_self() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::is_ready() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::log() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::prunable() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::record_place(char const*, int) const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::set(int) const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::set_self(rusty::sync::Weak<srpc::EventPollable@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::set_status(srpc::EventStatus@srpc.reactor) const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::status() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::test() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::wait() const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::IntEvent@srpc.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::NeverEvent(srpc::NeverEvent@srpc.reactor&&)",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::NeverEvent(rusty::Cell<srpc::EventStatus@srpc.reactor>, rusty::thread::ThreadId, srpc::EventState@srpc.reactor, rusty::Cell<bool>, rusty::sync::Weak<srpc::EventPollable@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::get_self() const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::is_ready() const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::log() const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::prunable() const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::record_place(char const*, int) const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::set_self(rusty::sync::Weak<srpc::EventPollable@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::set_status(srpc::EventStatus@srpc.reactor) const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::status() const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::test() const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::NeverEvent@srpc.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::PollThread(srpc::PollThread@srpc.reactor&&)",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::PollThread(rusty::sync::mpsc::Sender<std::__1::variant<srpc::PollCommand_AddPollable@srpc.reactor, srpc::PollCommand_RemovePollable@srpc.reactor, srpc::PollCommand_ClosePollable@srpc.reactor, srpc::PollCommand_UpdateMode@srpc.reactor, srpc::PollCommand_AddJob@srpc.reactor, srpc::PollCommand_RemoveJob@srpc.reactor, srpc::PollCommand_Shutdown@srpc.reactor>>, rusty::Mutex<rusty::Option<rusty::thread::JoinHandle<std::__1::tuple<>>>>, rusty::sync::atomic::detail::Atomic<unsigned long>, rusty::sync::atomic::detail::Atomic<bool>)",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::add(rusty::Arc<srpc::Job@srpc.misc>) const",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::add_proxy(rusty::Box<srpc::PollableBase@srpc.pollable_proxy, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::create()",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::get_remove_count() const",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::operator=(srpc::PollThread@srpc.reactor&&)",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::remove(srpc::Pollable@srpc.epoll_wrapper&) const",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::remove_fd(int) const",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::request_close(int) const",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::shutdown() const",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::update_mode(int, int) const",
                ),
                (
                    "T",
                    "srpc::PollThread@srpc.reactor::~PollThread()",
                ),
                (
                    "T",
                    "srpc::PollThreadWorker@srpc.reactor::create(rusty::sync::mpsc::Receiver<std::__1::variant<srpc::PollCommand_AddPollable@srpc.reactor, srpc::PollCommand_RemovePollable@srpc.reactor, srpc::PollCommand_ClosePollable@srpc.reactor, srpc::PollCommand_UpdateMode@srpc.reactor, srpc::PollCommand_AddJob@srpc.reactor, srpc::PollCommand_RemoveJob@srpc.reactor, srpc::PollCommand_Shutdown@srpc.reactor>>)",
                ),
                (
                    "T",
                    "srpc::PollThreadWorker@srpc.reactor::poll_loop()",
                ),
                (
                    "T",
                    "srpc::PollThreadWorker@srpc.reactor::update_mode(srpc::Pollable@srpc.epoll_wrapper&, int)",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::Reactor(rusty::Cell<int>, rusty::RefCell<rusty::VecDeque<rusty::Arc<srpc::EventPollable@srpc.reactor>>>, rusty::RefCell<rusty::VecDeque<rusty::Arc<srpc::EventPollable@srpc.reactor>>>, rusty::RefCell<rusty::VecDeque<rusty::Arc<srpc::EventPollable@srpc.reactor>>>, rusty::RefCell<rusty::VecDeque<rusty::Arc<srpc::EventPollable@srpc.reactor>>>, rusty::RefCell<btree_port::btree::map::BTreeMap@btree_port.btree.map<unsigned long, rusty::port::rc::Rc@rc_port<srpc::Fiber@srpc.reactor, rusty::alloc::Global>, rusty::alloc::Global>>, rusty::RefCell<rusty::port::vec::Vec@vec_port.vec<rusty::port::rc::Rc@rc_port<srpc::Fiber@srpc.reactor, rusty::alloc::Global>, rusty::alloc::Global>>, rusty::Cell<bool>, rusty::Cell<bool>, rusty::Cell<int>, rusty::Cell<int>, rusty::Cell<rusty::thread::ThreadId>, rusty::Cell<long>, rusty::Cell<long>, rusty::Cell<long>, rusty::Cell<long>, rusty::Cell<long>, rusty::RefCell<rusty::port::vec::Vec@vec_port.vec<srpc::StacklessTaskEntry@srpc.reactor, rusty::alloc::Global>>, rusty::RefCell<rusty::port::vec::Vec@vec_port.vec<unsigned long, rusty::alloc::Global>>, rusty::RefCell<rusty::VecDeque<unsigned long>>, rusty::marker::PhantomPinned)",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::check_timeout(rusty::VecDeque<rusty::Arc<srpc::EventPollable@srpc.reactor>>&) const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::continue_fiber(rusty::port::rc::Rc@rc_port<srpc::Fiber@srpc.reactor, rusty::alloc::Global> const&) const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::create_run_fiber(rusty::Function<void ()>) const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::display_waiting_ev() const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::enqueue_stackless_task(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::get_disk_reactor()",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::get_reactor()",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::new_()",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::process_stackless_tasks() const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::prune_finished_events() const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::recycle(rusty::port::rc::Rc@rc_port<srpc::Fiber@srpc.reactor, rusty::alloc::Global>&) const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::register_fiber(rusty::port::rc::Rc@rc_port<srpc::Fiber@srpc.reactor, rusty::alloc::Global> const&) const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::register_stackless_poller(rusty::Function<bool (rusty::Context&)>) const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::restore_running_fiber(rusty::Option<rusty::port::rc::Rc@rc_port<srpc::Fiber@srpc.reactor, rusty::alloc::Global>>) const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::run_loop(bool, bool) const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::save_running_fiber() const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::set_running_fiber(rusty::port::rc::Rc@rc_port<srpc::Fiber@srpc.reactor, rusty::alloc::Global> const&) const",
                ),
                (
                    "T",
                    "srpc::Reactor@srpc.reactor::~Reactor()",
                ),
                (
                    "T",
                    "srpc::RemoveJob@srpc.reactor(rusty::Arc<srpc::Job@srpc.misc>)",
                ),
                (
                    "T",
                    "srpc::RemovePollable@srpc.reactor(int)",
                ),
                (
                    "T",
                    "srpc::SharedIntEvent@srpc.reactor::set(int const&)",
                ),
                (
                    "T",
                    "srpc::SharedIntEvent@srpc.reactor::wait(rusty::Function<bool (int) const>)",
                ),
                (
                    "T",
                    "srpc::SharedIntEvent@srpc.reactor::wait_until_gte(int, int)",
                ),
                (
                    "T",
                    "srpc::Shutdown@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::TimeoutEvent(srpc::TimeoutEvent@srpc.reactor&&)",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::TimeoutEvent(rusty::Cell<srpc::EventStatus@srpc.reactor>, rusty::thread::ThreadId, srpc::EventState@srpc.reactor, rusty::Cell<bool>, rusty::sync::Weak<srpc::EventPollable@srpc.reactor>, unsigned long, unsigned long)",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::get_self() const",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::is_ready() const",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::log() const",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::prunable() const",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::set_self(rusty::sync::Weak<srpc::EventPollable@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::set_status(srpc::EventStatus@srpc.reactor) const",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::status() const",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::test() const",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::wait() const",
                ),
                (
                    "T",
                    "srpc::TimeoutEvent@srpc.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "srpc::UpdateMode@srpc.reactor(int, int)",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::WaitAll(srpc::WaitAll@srpc.reactor&&)",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::WaitAll(rusty::Cell<srpc::EventStatus@srpc.reactor>, rusty::thread::ThreadId, srpc::EventState@srpc.reactor, rusty::Cell<bool>, rusty::sync::Weak<srpc::EventPollable@srpc.reactor>, rusty::RefCell<rusty::port::vec::Vec@vec_port.vec<rusty::Arc<srpc::EventPollable@srpc.reactor>, rusty::alloc::Global>>)",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::add_event(rusty::Arc<srpc::EventPollable@srpc.reactor>) const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::get_self() const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::is_ready() const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::log() const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::prunable() const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::set_self(rusty::sync::Weak<srpc::EventPollable@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::set_status(srpc::EventStatus@srpc.reactor) const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::status() const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::test() const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::wait() const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::WaitAll@srpc.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::WaitAny(srpc::WaitAny@srpc.reactor&&)",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::WaitAny(rusty::Cell<srpc::EventStatus@srpc.reactor>, rusty::thread::ThreadId, srpc::EventState@srpc.reactor, rusty::Cell<bool>, rusty::sync::Weak<srpc::EventPollable@srpc.reactor>, rusty::port::vec::Vec@vec_port.vec<rusty::Arc<srpc::EventPollable@srpc.reactor>, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::get_self() const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::is_ready() const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::log() const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::prunable() const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::set_self(rusty::sync::Weak<srpc::EventPollable@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::set_status(srpc::EventStatus@srpc.reactor) const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::status() const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::test() const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::wait() const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::WaitAny@srpc.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "srpc::create_sp_int_event@srpc.reactor(int)",
                ),
                (
                    "T",
                    "srpc::create_sp_never_event@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::create_sp_timeout_event@srpc.reactor(unsigned long)",
                ),
                (
                    "T",
                    "srpc::create_sp_waitall@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::create_sp_waitall_from@srpc.reactor(rusty::port::vec::Vec@vec_port.vec<rusty::Arc<srpc::EventPollable@srpc.reactor>, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "srpc::create_sp_waitany@srpc.reactor(rusty::Arc<srpc::EventPollable@srpc.reactor>, rusty::Arc<srpc::EventPollable@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::current_thread_gettid@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::event_core_get_fiber_id@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::event_state_seed@srpc.reactor(srpc::EventState@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::fiber_create_run_impl@srpc.reactor(rusty::Function<void ()>, char const*, long)",
                ),
                (
                    "T",
                    "srpc::fiber_current_fiber@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::fiber_do_continue@srpc.reactor(srpc::Fiber@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::fiber_do_finalize@srpc.reactor(srpc::Fiber@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::fiber_do_yield@srpc.reactor(srpc::Fiber@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::fiber_engine_destroy@srpc.reactor(srpc_fiber*)",
                ),
                (
                    "T",
                    "srpc::fiber_engine_resume@srpc.reactor(srpc_fiber*)",
                ),
                (
                    "T",
                    "srpc::fiber_engine_start@srpc.reactor(srpc_fiber*, void*)",
                ),
                (
                    "T",
                    "srpc::fiber_engine_yield@srpc.reactor(srpc_fiber*)",
                ),
                (
                    "T",
                    "srpc::fiber_fn_clear@srpc.reactor(rusty::RefCell<rusty::Function<void ()>> const*)",
                ),
                (
                    "T",
                    "srpc::fiber_fn_invoke@srpc.reactor(rusty::RefCell<rusty::Function<void ()>> const*)",
                ),
                (
                    "T",
                    "srpc::fiber_fn_present@srpc.reactor(rusty::RefCell<rusty::Function<void ()>> const*)",
                ),
                (
                    "T",
                    "srpc::fiber_install_task@srpc.reactor(rusty::RefCell<rusty::Option<rusty::Box<srpc::fiber_task_t@srpc.reactor, rusty::alloc::Global>>> const*, rusty::Function<void (srpc::fiber_yield_t@srpc.reactor&)>)",
                ),
                (
                    "T",
                    "srpc::fiber_is_finished@srpc.reactor(srpc::Fiber@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::fiber_next_global_id@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::fiber_registry_key@srpc.reactor(rusty::port::rc::Rc@rc_port<srpc::Fiber@srpc.reactor, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "srpc::fiber_run@srpc.reactor(srpc::Fiber@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::fiber_run_wrapper@srpc.reactor(srpc::Fiber@srpc.reactor const&, srpc::fiber_yield_t@srpc.reactor*)",
                ),
                (
                    "T",
                    "srpc::fiber_sleep@srpc.reactor(unsigned long)",
                ),
                (
                    "T",
                    "srpc::fiber_task_body_invoke@srpc.reactor(rusty::Function<void (srpc::fiber_yield_t@srpc.reactor&)>&, srpc::fiber_yield_t@srpc.reactor&)",
                ),
                (
                    "T",
                    "srpc::fiber_task_invoke@srpc.reactor(rusty::RefCell<rusty::Option<rusty::Box<srpc::fiber_task_t@srpc.reactor, rusty::alloc::Global>>> const*)",
                ),
                (
                    "T",
                    "srpc::fiber_task_t@srpc.reactor::new_(rusty::Function<void (srpc::fiber_yield_t@srpc.reactor&)>)",
                ),
                (
                    "T",
                    "srpc::fiber_task_t@srpc.reactor::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "srpc::fiber_task_t@srpc.reactor::~fiber_task_t()",
                ),
                (
                    "T",
                    "srpc::fiber_yield_invoke@srpc.reactor(srpc::fiber_yield_t@srpc.reactor&)",
                ),
                (
                    "T",
                    "srpc::fiber_yield_invoke_ptr@srpc.reactor(srpc::fiber_yield_t@srpc.reactor*)",
                ),
                (
                    "T",
                    "srpc::fiber_yield_t@srpc.reactor::new_(srpc::fiber_task_t@srpc.reactor&)",
                ),
                (
                    "T",
                    "srpc::int_event_is_ready@srpc.reactor(srpc::IntEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::int_event_make@srpc.reactor(int)",
                ),
                (
                    "T",
                    "srpc::int_event_raw_ptr@srpc.reactor(rusty::Arc<srpc::IntEvent@srpc.reactor> const&)",
                ),
                (
                    "T",
                    "srpc::int_event_set@srpc.reactor(srpc::IntEvent@srpc.reactor const&, int)",
                ),
                (
                    "T",
                    "srpc::job_ready@srpc.reactor(rusty::Arc<srpc::Job@srpc.misc> const&)",
                ),
                (
                    "T",
                    "srpc::job_spawn_work@srpc.reactor(rusty::Arc<srpc::Job@srpc.misc> const&)",
                ),
                (
                    "T",
                    "srpc::never_event_make@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::pollable_proxy_fd@srpc.reactor(rusty::Box<srpc::PollableBase@srpc.pollable_proxy, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "srpc::pollable_proxy_mode@srpc.reactor(rusty::Box<srpc::PollableBase@srpc.pollable_proxy, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "srpc::pollthread_create@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::pollthread_drop@srpc.reactor(srpc::PollThread@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::pollworker_close_proxy_of@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&, int)",
                ),
                (
                    "T",
                    "srpc::pollworker_create@srpc.reactor(rusty::sync::mpsc::Receiver<std::__1::variant<srpc::PollCommand_AddPollable@srpc.reactor, srpc::PollCommand_RemovePollable@srpc.reactor, srpc::PollCommand_ClosePollable@srpc.reactor, srpc::PollCommand_UpdateMode@srpc.reactor, srpc::PollCommand_AddJob@srpc.reactor, srpc::PollCommand_RemoveJob@srpc.reactor, srpc::PollCommand_Shutdown@srpc.reactor>>)",
                ),
                (
                    "T",
                    "srpc::pollworker_do_add_job@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&, rusty::Arc<srpc::Job@srpc.misc>)",
                ),
                (
                    "T",
                    "srpc::pollworker_do_add_pollable@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&, rusty::Box<srpc::PollableBase@srpc.pollable_proxy, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "srpc::pollworker_do_close_pollable@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&, int)",
                ),
                (
                    "T",
                    "srpc::pollworker_do_remove_job@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&, rusty::Arc<srpc::Job@srpc.misc>)",
                ),
                (
                    "T",
                    "srpc::pollworker_do_remove_pollable@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&, int)",
                ),
                (
                    "T",
                    "srpc::pollworker_do_update_mode@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&, int, int)",
                ),
                (
                    "T",
                    "srpc::pollworker_is_on_poll_thread@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::pollworker_make@srpc.reactor(rusty::sync::mpsc::Receiver<std::__1::variant<srpc::PollCommand_AddPollable@srpc.reactor, srpc::PollCommand_RemovePollable@srpc.reactor, srpc::PollCommand_ClosePollable@srpc.reactor, srpc::PollCommand_UpdateMode@srpc.reactor, srpc::PollCommand_AddJob@srpc.reactor, srpc::PollCommand_RemoveJob@srpc.reactor, srpc::PollCommand_Shutdown@srpc.reactor>>)",
                ),
                (
                    "T",
                    "srpc::pollworker_poll_loop@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&)",
                ),
                (
                    "T",
                    "srpc::pollworker_process_commands@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&)",
                ),
                (
                    "T",
                    "srpc::pollworker_process_pending_removals@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&)",
                ),
                (
                    "T",
                    "srpc::pollworker_snapshot_fds@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&)",
                ),
                (
                    "T",
                    "srpc::pollworker_take_removals@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&)",
                ),
                (
                    "T",
                    "srpc::pollworker_trigger_job@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&)",
                ),
                (
                    "T",
                    "srpc::pollworker_update_mode@srpc.reactor(srpc::PollThreadWorker@srpc.reactor&, srpc::Pollable@srpc.epoll_wrapper&, int)",
                ),
                (
                    "T",
                    "srpc::reactor_create_run_fiber_at_impl@srpc.reactor(srpc::Reactor@srpc.reactor const&, rusty::Function<void ()>, char const*, long)",
                ),
                (
                    "T",
                    "srpc::reactor_create_run_fiber_impl@srpc.reactor(srpc::Reactor@srpc.reactor const&, rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "srpc::reactor_dec_active_fibers@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::reactor_get_or_create_fiber_impl@srpc.reactor(srpc::Reactor@srpc.reactor const&, rusty::Function<void ()>, char const*, long)",
                ),
                (
                    "T",
                    "srpc::reactor_live_fiber_count@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::reactor_log_create@srpc.reactor(bool)",
                ),
                (
                    "T",
                    "srpc::reactor_log_line@srpc.reactor(int, int, signed char const*, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "srpc::reactor_make@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::reactor_poll_one@srpc.reactor(srpc::Reactor@srpc.reactor const&, unsigned long, rusty::Function<bool (rusty::Context&)>*)",
                ),
                (
                    "T",
                    "srpc::reactor_spawn_stackless_task_impl@srpc.reactor(srpc::Reactor@srpc.reactor const&, rusty::Task<void>)",
                ),
                (
                    "T",
                    "srpc::reactor_tls_get@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::reactor_tls_get_disk@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::reactor_tls_restore_running@srpc.reactor(rusty::Option<rusty::port::rc::Rc@rc_port<srpc::Fiber@srpc.reactor, rusty::alloc::Global>>)",
                ),
                (
                    "T",
                    "srpc::reactor_tls_save_running@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::reactor_tls_set_running@srpc.reactor(rusty::port::rc::Rc@rc_port<srpc::Fiber@srpc.reactor, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "srpc::reactor_verify@srpc.reactor(bool)",
                ),
                (
                    "T",
                    "srpc::reusing_fiber@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::shared_int_event_set@srpc.reactor(srpc::SharedIntEvent@srpc.reactor&, int)",
                ),
                (
                    "T",
                    "srpc::shared_int_event_wait@srpc.reactor(srpc::SharedIntEvent@srpc.reactor&, rusty::Function<bool (int) const>)",
                ),
                (
                    "T",
                    "srpc::shared_int_event_wait_until_gte@srpc.reactor(srpc::SharedIntEvent@srpc.reactor&, int, int)",
                ),
                (
                    "T",
                    "srpc::stackless_profile_enabled@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::stackless_profile_env@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::stackless_profile_note_enqueue@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::stackless_profile_note_poll_ready@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::stackless_profile_note_register@srpc.reactor(unsigned long, bool, unsigned long)",
                ),
                (
                    "T",
                    "srpc::stackless_profile_report_periodic@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::stackless_profile_report_periodic_shim@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::stackless_profile_update_max_slots@srpc.reactor(unsigned long)",
                ),
                (
                    "T",
                    "srpc::thread_id_to_u64@srpc.reactor(rusty::thread::ThreadId)",
                ),
                (
                    "T",
                    "srpc::timeout_event_is_ready@srpc.reactor(srpc::TimeoutEvent@srpc.reactor const&)",
                ),
                (
                    "T",
                    "srpc::timeout_event_make@srpc.reactor(unsigned long)",
                ),
                (
                    "T",
                    "srpc::u64_to_thread_id@srpc.reactor(unsigned long)",
                ),
                (
                    "T",
                    "srpc::waitall_make@srpc.reactor()",
                ),
                (
                    "T",
                    "srpc::waitall_make_from@srpc.reactor(rusty::port::vec::Vec@vec_port.vec<rusty::Arc<srpc::EventPollable@srpc.reactor>, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "srpc::waitany_make@srpc.reactor(rusty::Arc<srpc::EventPollable@srpc.reactor>, rusty::Arc<srpc::EventPollable@srpc.reactor>)",
                ),
            }
        ),
    ),
    "srpc.server": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.server;",
                "namespace srpc {",
                "import vec_port.vec;",
                "import std_port;",
                "import srpc.basetypes;",
                "import srpc.channel;",
                "import srpc.debugging;",
                "import srpc.internal_protocol;",
                "import srpc.logging;",
                "import srpc.misc;",
                "import srpc.reactor;",
                "import srpc.serializable;",
                "import srpc.tcp_channel;",
                "export enum class ShutdownPhase;",
                "export constexpr ShutdownPhase ShutdownPhase_RUNNING();",
                "export constexpr ShutdownPhase ShutdownPhase_STOP_ACCEPTING();",
                "export constexpr ShutdownPhase ShutdownPhase_DRAINING();",
                "export constexpr ShutdownPhase ShutdownPhase_CLOSING();",
                "export constexpr ShutdownPhase ShutdownPhase_STOPPED();",
                "export enum class ServerConnStatus;",
                "export constexpr ServerConnStatus ServerConnStatus_CONNECTED();",
                "export constexpr ServerConnStatus ServerConnStatus_CLOSED();",
                "export struct PendingRequestGuard;",
                "export struct Request;",
                "export class Service;",
                "export struct RpcServiceContext;",
                "export struct ServerConnection;",
                "export struct DeferredReply;",
                "export struct ShutdownState;",
                "export struct ChannelSconns;",
                "export struct Server;",
                "export using ShutdownHook = rusty::Function<void()>;",
                "export using ServiceProxy = rusty::Box<Service>;",
                "export using ServerPendingRequestsAtomic = rusty::sync::atomic::AtomicI32;",
                "export using ServerDropHeartbeatRepliesAtomic = rusty::sync::atomic::AtomicBool;",
                "export using ServerReplyFn = rusty::Function<void(::srpc::BinaryWriteArchive&)>;",
                "export constexpr uint64_t kDefaultDrainTimeoutMs = static_cast<uint64_t>(30000);",
                "export std::string_view shutdown_phase_to_string(ShutdownPhase phase);",
                "export ServiceProxy make_service_proxy_from_box(rusty::Box<Service> svc);",
                "export rusty::Option<rusty::Arc<PollThread>> server_resolve_poll_thread(rusty::Option<rusty::Arc<PollThread>> poll_thread_worker);",
                "export std::string server_dsl_addr_to_string(const int8_t* addr);",
                "export uint64_t server_random_u64();",
                "export uint64_t server_now_nanos();",
                "export void server_wait_for_shutdown_impl(const rusty::Mutex<ShutdownState>& state, const rusty::Box<rusty::Condvar>& cond);",
                "export uint64_t server_generate_instance_id();",
                "export bool server_drain_impl(const rusty::Cell<ShutdownPhase>& phase, const rusty::Arc<ServerPendingRequestsAtomic>& pending, uint64_t timeout_ms);",
                "export void server_run_shutdown_hooks(const rusty::Mutex<rusty::Vec<ShutdownHook>>& hooks);",
                "export rusty::Option<int32_t> server_parse_port(const std::string& text);",
                "export void server_invoke_shutdown_hook_safely(ShutdownHook& hook);",
                "export void sconn_reply(const ServerConnection& sconn, const Request& req, int32_t error_code, ServerReplyFn write_fn);",
                "export void sconn_on_channel_frame(const rusty::sync::Weak<ServerConnection>& weak, const ::srpc::ChannelFrame& frame);",
                "export void sconn_on_channel_closed(const rusty::sync::Weak<ServerConnection>& weak);",
                "export void sconn_on_channel_error(const rusty::sync::Weak<ServerConnection>& weak, ::srpc::ChannelError err, std::string_view msg);",
                "export void request_fill_body(Request& req, std::span<const uint8_t> bytes);",
                "export void sconn_decode_request_and_dispatch(const ServerConnection& sconn, const uint8_t* bytes, size_t size);",
                "export srpc::ChannelConnectionBase* sconn_proxy_ptr(const rusty::Option<::srpc::ChannelConnectionProxy>& slot);",
                "export void sconn_dispatch_response_frame_via_channel(const ServerConnection& sconn, const uint8_t* bytes, size_t size);",
                "export int32_t srpc_parse_port(const uint8_t* text, size_t len, int32_t* out);",
                "export size_t srpc_cstr_len(const uint8_t* text);",
                "export uint64_t srpc_random_u64();",
                "export using WeakServerConnection = rusty::sync::Weak<ServerConnection>;",
                "export template <class U> class ServiceAdapter;",
                "export template <class U> class ServiceAdapterRef;",
                "export template <class U> class ServiceAdapterRefMut;",
            }
        ),
        symbols=frozenset(
            {
                (
                    "D",
                    "typeinfo for srpc::Service@srpc.server",
                ),
                (
                    "D",
                    "vtable for srpc::Service@srpc.server",
                ),
                (
                    "R",
                    "srpc::SERVER_ERR_ALREADY_EXISTS@srpc.server",
                ),
                (
                    "R",
                    "srpc::SERVER_ERR_INVALID_ARGUMENT@srpc.server",
                ),
                (
                    "R",
                    "srpc::SERVER_ERR_NO_ENTRY@srpc.server",
                ),
                (
                    "R",
                    "srpc::kDefaultDrainTimeoutMs@srpc.server",
                ),
                (
                    "R",
                    "typeinfo name for srpc::Service@srpc.server",
                ),
                (
                    "T",
                    "srpc::DeferredReply@srpc.server::DeferredReply(srpc::DeferredReply@srpc.server&&)",
                ),
                (
                    "T",
                    "srpc::DeferredReply@srpc.server::DeferredReply(rusty::Box<srpc::Request@srpc.server, rusty::alloc::Global>, rusty::sync::Weak<srpc::ServerConnection@srpc.server>, rusty::Function<void (srpc::BinaryWriteArchive@srpc.serializable&)>, rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "srpc::DeferredReply@srpc.server::new_(rusty::Box<srpc::Request@srpc.server, rusty::alloc::Global>, rusty::sync::Weak<srpc::ServerConnection@srpc.server>, rusty::Function<void (srpc::BinaryWriteArchive@srpc.serializable&)>, rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "srpc::DeferredReply@srpc.server::operator=(srpc::DeferredReply@srpc.server&&)",
                ),
                (
                    "T",
                    "srpc::DeferredReply@srpc.server::reply()",
                ),
                (
                    "T",
                    "srpc::DeferredReply@srpc.server::reply_error(int)",
                ),
                (
                    "T",
                    "srpc::DeferredReply@srpc.server::run_async(rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "srpc::DeferredReply@srpc.server::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "srpc::DeferredReply@srpc.server::~DeferredReply()",
                ),
                (
                    "T",
                    "srpc::PendingRequestGuard@srpc.server::PendingRequestGuard(srpc::PendingRequestGuard@srpc.server&&)",
                ),
                (
                    "T",
                    "srpc::PendingRequestGuard@srpc.server::PendingRequestGuard(rusty::Arc<rusty::sync::atomic::detail::Atomic<int>>)",
                ),
                (
                    "T",
                    "srpc::PendingRequestGuard@srpc.server::operator=(srpc::PendingRequestGuard@srpc.server&&)",
                ),
                (
                    "T",
                    "srpc::PendingRequestGuard@srpc.server::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "srpc::PendingRequestGuard@srpc.server::~PendingRequestGuard()",
                ),
                (
                    "T",
                    "srpc::Request@srpc.server::attach_pending_guard(rusty::Arc<rusty::sync::atomic::detail::Atomic<int>> const&)",
                ),
                (
                    "T",
                    "srpc::RpcServiceContext@srpc.server::new_(std_port::collections::hash::map::HashMap@std_port<int, unsigned long, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>, std_port::collections::hash::set::HashSet@std_port<int, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>, rusty::port::vec::Vec@vec_port.vec<rusty::RefCell<rusty::Box<srpc::Service@srpc.server, rusty::alloc::Global>>, rusty::alloc::Global>, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, rusty::Arc<rusty::sync::atomic::detail::Atomic<int>>, rusty::Arc<rusty::sync::atomic::detail::Atomic<bool>>, unsigned long)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::Server(srpc::Server@srpc.server&&)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::Server(rusty::port::vec::Vec@vec_port.vec<rusty::Box<srpc::Service@srpc.server, rusty::alloc::Global>, rusty::alloc::Global>, std_port::collections::hash::map::HashMap@std_port<int, unsigned long, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>, std_port::collections::hash::set::HashSet@std_port<int, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>, rusty::Option<rusty::Arc<srpc::RpcServiceContext@srpc.server>>, rusty::Option<rusty::Arc<srpc::PollThread@srpc.reactor>>, rusty::Mutex<srpc::ShutdownState@srpc.server>, rusty::Box<rusty::Condvar, rusty::alloc::Global>, rusty::Cell<srpc::ShutdownPhase@srpc.server>, rusty::Mutex<rusty::port::vec::Vec@vec_port.vec<rusty::Function<void ()>, rusty::alloc::Global>>, rusty::Arc<rusty::sync::atomic::detail::Atomic<int>>, rusty::Arc<rusty::sync::atomic::detail::Atomic<bool>>, unsigned long, rusty::Option<rusty::Box<srpc::ChannelFactoryBase@srpc.channel, rusty::alloc::Global>>, rusty::Option<rusty::Box<srpc::ChannelListenerBase@srpc.channel, rusty::alloc::Global>>, rusty::Arc<rusty::Mutex<srpc::ChannelSconns@srpc.server>>)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::add_shutdown_hook(rusty::Function<void ()>) const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::addr() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::decrement_pending() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::do_shutdown() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::drain(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::drop_heartbeat_replies() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::get_bound_port() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::graceful_shutdown(unsigned long)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::increment_pending() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::instance_id() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::is_channel_factory_bound() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::new_(rusty::Option<rusty::Arc<srpc::PollThread@srpc.reactor>>)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::operator=(srpc::Server@srpc.server&&)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::pending_request_count() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::phase() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::reg_fast_rpc(int, unsigned long)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::reg_rpc(int, unsigned long)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::reg_service(rusty::Box<srpc::Service@srpc.server, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::reg_service_proxy(rusty::Box<srpc::Service@srpc.server, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::service_count() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::set_channel_factory(rusty::Box<srpc::ChannelFactoryBase@srpc.channel, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::set_drop_heartbeat_replies(bool) const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::start(signed char const*)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::stop_accepting()",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::unreg(int)",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::wait_for_shutdown() const",
                ),
                (
                    "T",
                    "srpc::Server@srpc.server::~Server()",
                ),
                (
                    "T",
                    "srpc::ServerConnection@srpc.server::bind_channel(rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "srpc::ServerConnection@srpc.server::close() const",
                ),
                (
                    "T",
                    "srpc::ServerConnection@srpc.server::connected() const",
                ),
                (
                    "T",
                    "srpc::ServerConnection@srpc.server::install_self_weak_for_testing(rusty::sync::Weak<srpc::ServerConnection@srpc.server>)",
                ),
                (
                    "T",
                    "srpc::ServerConnection@srpc.server::is_channel_mode() const",
                ),
                (
                    "T",
                    "srpc::ServerConnection@srpc.server::is_closed() const",
                ),
                (
                    "T",
                    "srpc::ServerConnection@srpc.server::new_(rusty::Arc<srpc::RpcServiceContext@srpc.server>, int)",
                ),
                (
                    "T",
                    "srpc::ServerConnection@srpc.server::reply(srpc::Request@srpc.server const&, int, rusty::Function<void (srpc::BinaryWriteArchive@srpc.serializable&)>) const",
                ),
                (
                    "T",
                    "srpc::ServerConnection@srpc.server::run_async(rusty::Function<void ()>) const",
                ),
                (
                    "T",
                    "srpc::Service@srpc.server::~Service()",
                ),
                (
                    "T",
                    "srpc::make_empty_request_box@srpc.server()",
                ),
                (
                    "T",
                    "srpc::make_service_proxy_from_box@srpc.server(rusty::Box<srpc::Service@srpc.server, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "srpc::no_reply_writer@srpc.server()",
                ),
                (
                    "T",
                    "srpc::request_fill_body@srpc.server(srpc::Request@srpc.server&, std::__1::span<unsigned char const, 18446744073709551615ul>)",
                ),
                (
                    "T",
                    "srpc::sconn_decode_request_and_dispatch@srpc.server(srpc::ServerConnection@srpc.server const&, unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::sconn_dispatch_in_fiber@srpc.server(rusty::Arc<srpc::RpcServiceContext@srpc.server>, unsigned long, int, rusty::Box<srpc::Request@srpc.server, rusty::alloc::Global>, rusty::sync::Weak<srpc::ServerConnection@srpc.server>)",
                ),
                (
                    "T",
                    "srpc::sconn_dispatch_response_frame_via_channel@srpc.server(srpc::ServerConnection@srpc.server const&, unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::sconn_on_channel_closed@srpc.server(rusty::sync::Weak<srpc::ServerConnection@srpc.server> const&)",
                ),
                (
                    "T",
                    "srpc::sconn_on_channel_error@srpc.server(rusty::sync::Weak<srpc::ServerConnection@srpc.server> const&, srpc::ChannelError@srpc.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::sconn_on_channel_frame@srpc.server(rusty::sync::Weak<srpc::ServerConnection@srpc.server> const&, srpc::ChannelFrame@srpc.channel const&)",
                ),
                (
                    "T",
                    "srpc::sconn_proxy_ptr@srpc.server(rusty::Option<rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>> const&)",
                ),
                (
                    "T",
                    "srpc::sconn_reply@srpc.server(srpc::ServerConnection@srpc.server const&, srpc::Request@srpc.server const&, int, rusty::Function<void (srpc::BinaryWriteArchive@srpc.serializable&)>)",
                ),
                (
                    "T",
                    "srpc::server_drain_impl@srpc.server(rusty::Cell<srpc::ShutdownPhase@srpc.server> const&, rusty::Arc<rusty::sync::atomic::detail::Atomic<int>> const&, unsigned long)",
                ),
                (
                    "T",
                    "srpc::server_dsl_addr_to_string@srpc.server(signed char const*)",
                ),
                (
                    "T",
                    "srpc::server_generate_instance_id@srpc.server()",
                ),
                (
                    "T",
                    "srpc::server_invoke_shutdown_hook_safely@srpc.server(rusty::Function<void ()>&)",
                ),
                (
                    "T",
                    "srpc::server_now_nanos@srpc.server()",
                ),
                (
                    "T",
                    "srpc::server_parse_port@srpc.server(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::server_random_u64@srpc.server()",
                ),
                (
                    "T",
                    "srpc::server_resolve_poll_thread@srpc.server(rusty::Option<rusty::Arc<srpc::PollThread@srpc.reactor>>)",
                ),
                (
                    "T",
                    "srpc::server_run_shutdown_hooks@srpc.server(rusty::Mutex<rusty::port::vec::Vec@vec_port.vec<rusty::Function<void ()>, rusty::alloc::Global>> const&)",
                ),
                (
                    "T",
                    "srpc::server_wait_for_shutdown_impl@srpc.server(rusty::Mutex<srpc::ShutdownState@srpc.server> const&, rusty::Box<rusty::Condvar, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "srpc::shutdown_phase_to_string@srpc.server(srpc::ShutdownPhase@srpc.server)",
                ),
            }
        ),
    ),
    "srpc.client": AbiSpec(
        surface=frozenset(
            {
                "export module srpc.client;",
                "namespace srpc {",
                "import vec_port.vec;",
                "import btree_port.btree.map;",
                "import std_port;",
                "import srpc.basetypes;",
                "import srpc.callback_wrapper;",
                "import srpc.callbacks;",
                "import srpc.channel;",
                "import srpc.circuit_breaker;",
                "import srpc.connection_metrics;",
                "import srpc.connection_state;",
                "import srpc.errors;",
                "import srpc.fiber_channel;",
                "import srpc.heartbeat;",
                "import srpc.load_balancer;",
                "import srpc.logging;",
                "import srpc.misc;",
                "import srpc.rand;",
                "import srpc.reactor;",
                "import srpc.reconnect_policy;",
                "import srpc.request_options;",
                "import srpc.request_queue;",
                "import srpc.serializable;",
                "import srpc.tcp_channel;",
                "import srpc.debugging;",
                "export enum class DisconnectBehavior;",
                "export constexpr DisconnectBehavior DisconnectBehavior_QUEUE();",
                "export constexpr DisconnectBehavior DisconnectBehavior_FAIL_FAST();",
                "export struct ReplyBuffer;",
                "export struct BufferingConfig;",
                "export struct KeepaliveConfig;",
                "export struct PoolConfig;",
                "export struct FutureAttr;",
                "export struct FutureState;",
                "export struct Future;",
                "export struct ReconnectState;",
                "export struct ClientConnection;",
                "export struct Client;",
                "export struct PoolState;",
                "export struct ClientPool;",
                "export using WeakClientConnection = rusty::sync::Weak<ClientConnection>;",
                "export using FutureResult = rusty::Result<rusty::Arc<Future>, int32_t>;",
                "export using AsyncReplyCallback = rusty::Function<void(int32_t, const uint8_t*, size_t)>;",
                "export using OnReconnectCompleteCallbackFn = rusty::Function<void(bool)>;",
                "export using OnServerRestartCallbackFn = rusty::Function<void(uint64_t, uint64_t)>;",
                "export using OnConnectedCallbackFn = rusty::Function<void() const>;",
                "export using OnErrorCallbackFn = rusty::Function<void(::srpc::RpcError, const std::string&) const>;",
                "export using OnReconnectedCallbackFn = rusty::Function<void(bool) const>;",
                "export using LegacyStdString = std::string;",
                "export using LegacyStdStringView = std::string_view;",
                "export using c_char = int8_t;",
                "export using FutureCallback = ::srpc::detail::CallbackWrapper<rusty::Function<void(rusty::Arc<Future>) const>>;",
                "export constexpr int32_t CLIENT_ERR_AGAIN = static_cast<int32_t>(11);",
                "export constexpr int32_t CLIENT_ERR_WOULD_BLOCK = CLIENT_ERR_AGAIN;",
                "export constexpr int32_t CLIENT_ERR_BUSY = static_cast<int32_t>(16);",
                "export constexpr int32_t CLIENT_ERR_CANCELED = static_cast<int32_t>(125);",
                "export constexpr int32_t CLIENT_ERR_CONNECTION_ABORTED = static_cast<int32_t>(103);",
                "export constexpr int32_t CLIENT_ERR_CONNECTION_REFUSED = static_cast<int32_t>(111);",
                "export constexpr int32_t CLIENT_ERR_CONNECTION_RESET = static_cast<int32_t>(104);",
                "export constexpr int32_t CLIENT_ERR_HOST_UNREACHABLE = static_cast<int32_t>(113);",
                "export constexpr int32_t CLIENT_ERR_INVALID_ARGUMENT = static_cast<int32_t>(22);",
                "export constexpr int32_t CLIENT_ERR_IO = static_cast<int32_t>(5);",
                "export constexpr int32_t CLIENT_ERR_NETWORK_UNREACHABLE = static_cast<int32_t>(101);",
                "export constexpr int32_t CLIENT_ERR_NOT_CONNECTED = static_cast<int32_t>(107);",
                "export constexpr int32_t CLIENT_ERR_BROKEN_PIPE = static_cast<int32_t>(32);",
                "export constexpr int32_t CLIENT_ERR_TIMED_OUT = static_cast<int32_t>(110);",
                "export constexpr int32_t CLIENT_REQUEST_QUEUE_REJECTED_ERROR = static_cast<int32_t>(35);",
                "export constexpr int32_t CLIENT_REQUEST_QUEUE_REJECTED_ERROR = static_cast<int32_t>(11);",
                "export constexpr int32_t CLIENT_INT_MIN = std::numeric_limits<int32_t>::min();",
                "export constexpr int32_t CLIENT_RAND_MAX = std::numeric_limits<int32_t>::max();",
                "export constexpr int32_t CLIENT_INTERNAL_HEARTBEAT_RPC_ID = std::numeric_limits<int32_t>::min();",
                "export constexpr int32_t CLIENT_POLL_READ = static_cast<int32_t>(1);",
                "export constexpr int32_t CLIENT_POLL_NO_CHANGE = -1;",
                "export constexpr size_t kAsyncSlotCount = static_cast<size_t>(16384);",
                "export int32_t client_rand(int32_t min, int32_t max);",
                "export void client_verify(bool value);",
                "export void client_log_line(int32_t level, int32_t line, const int8_t* file, std::string message);",
                "export std::string client_text(std::string_view text);",
                "export std::string client_text_str(std::string_view prefix, std::string_view value, std::string_view suffix);",
                "export std::string client_text_i32(std::string_view prefix, int32_t value, std::string_view suffix);",
                "export std::string client_text_u32_str(std::string_view prefix, uint32_t value, std::string_view middle, std::string_view text, std::string_view suffix);",
                "export std::string client_text_u64_pair(std::string_view prefix, uint64_t first, std::string_view middle, uint64_t second, std::string_view suffix);",
                "export std::string client_text_str_i32(std::string_view prefix, std::string_view text, std::string_view middle, int32_t value, std::string_view suffix);",
                "export ::srpc::SinkProxy client_sink_proxy(::srpc::BufferSink& sink);",
                "export ::srpc::SourceProxy client_source_proxy(::srpc::BufferSource& source);",
                "export ReplyBuffer reply_buffer_empty();",
                "export void reply_buffer_fill(ReplyBuffer& rb, std::span<const uint8_t> bytes);",
                "export rusty::Vec<rusty::Option<AsyncReplyCallback>> make_prefilled_cb_slots();",
                "export ::srpc::RequestQueue make_pending_queue(const ::srpc::RequestQueueConfig& c);",
                "export uint64_t clientconn_monotonic_ms_now();",
                "export int32_t clientconn_reconnect(const ClientConnection& self_, OnReconnectCompleteCallbackFn on_complete);",
                "export ::srpc::BinaryWriteArchive make_write_archive(::srpc::BufferSink* sink);",
                "export void request_copy_reply(const rusty::Arc<Future>& final_fu, const rusty::Arc<Future>& attempt_fu);",
                "export ::srpc::TimeoutType classify_request_failure(int32_t err);",
                "export ::srpc::ChannelError clientconn_dispatch_frame_via_channel(const ClientConnection& conn, const uint8_t* body_bytes, size_t body_size);",
                "export void clientconn_enqueue_heartbeat_probe(const ClientConnection& conn);",
                "export std::string clientconn_addr_to_string(const int8_t* addr);",
                "export int32_t clientconn_connect_via_factory(const ClientConnection& conn, const int8_t* addr_i8);",
                "export rusty::Box<::srpc::FiberChannel> clientconn_make_fiber_channel(::srpc::ChannelConnectionProxy ch);",
                "export void clientconn_recv_job_entry(WeakClientConnection weak_self);",
                "export void clientconn_bind_channel_via_poll_thread(const ClientConnection& conn, ::srpc::ChannelConnectionProxy channel);",
                "export ::srpc::FiberChannel* clientconn_fiber_channel_ptr(const rusty::Option<rusty::Box<::srpc::FiberChannel>>& slot);",
                "export void clientconn_run_recv_loop(const ClientConnection& conn);",
                "export void clientconn_decode_response_and_notify(const ClientConnection& conn, const uint8_t* bytes, size_t size);",
                "export ::srpc::RpcError clientconn_map_system_error(int32_t err);",
                "export bool clientpool_is_client_healthy_with(PoolConfig cfg, const rusty::Arc<Client>& client);",
                "export size_t clientpool_get_healthy_client_count(const ClientPool& self_, const std::string& addr);",
                "export size_t clientpool_remove_unhealthy_clients(const ClientPool& self_, const std::string& addr);",
                "export size_t clientpool_close_idle_clients(const ClientPool& self_, const std::string& addr, uint64_t current_time_ms);",
                "export size_t clientpool_remove_all_unhealthy(const ClientPool& self_);",
                "export size_t clientpool_close_all_idle(const ClientPool& self_, uint64_t current_time_ms);",
                "export int32_t clientpool_connect_client(const rusty::Arc<Client>& client, const std::string& addr);",
                "export rusty::Option<rusty::Arc<Client>> clientpool_get_client(const ClientPool& self_, const std::string& addr);",
                "export using Fiber = Fiber;",
                "export using PollThread = PollThread;",
            }
        ),
        symbols=frozenset(
            {
                (
                    "R",
                    "srpc::CLIENT_ERR_AGAIN@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_BROKEN_PIPE@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_BUSY@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_CANCELED@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_CONNECTION_ABORTED@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_CONNECTION_REFUSED@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_CONNECTION_RESET@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_HOST_UNREACHABLE@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_INVALID_ARGUMENT@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_IO@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_NETWORK_UNREACHABLE@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_NOT_CONNECTED@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_TIMED_OUT@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_ERR_WOULD_BLOCK@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_INTERNAL_HEARTBEAT_RPC_ID@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_INT_MIN@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_POLL_NO_CHANGE@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_POLL_READ@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_RAND_MAX@srpc.client",
                ),
                (
                    "R",
                    "srpc::CLIENT_REQUEST_QUEUE_REJECTED_ERROR@srpc.client",
                ),
                (
                    "R",
                    "srpc::kAsyncSlotCount@srpc.client",
                ),
                (
                    "T",
                    "srpc::BufferingConfig@srpc.client::clone() const",
                ),
                (
                    "T",
                    "srpc::BufferingConfig@srpc.client::defaults()",
                ),
                (
                    "T",
                    "srpc::BufferingConfig@srpc.client::disabled()",
                ),
                (
                    "T",
                    "srpc::BufferingConfig@srpc.client::new_()",
                ),
                (
                    "T",
                    "srpc::BufferingConfig@srpc.client::to_queue_config() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::Client(srpc::Client@srpc.client&&)",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::Client(rusty::RefCell<rusty::Option<rusty::Arc<srpc::ClientConnection@srpc.client>>>, rusty::Arc<srpc::PollThread@srpc.reactor>, rusty::Cell<bool>, rusty::Cell<long>, rusty::Cell<unsigned long>, rusty::Cell<int>, rusty::Cell<srpc::KeepaliveConfig@srpc.client>, rusty::Cell<srpc::HeartbeatConfig@srpc.heartbeat>, rusty::Cell<srpc::CircuitBreakerConfig@srpc.circuit_breaker>, rusty::Cell<srpc::ReconnectPolicy@srpc.reconnect_policy>, rusty::Arc<srpc::CallbackManager@srpc.callbacks>, rusty::Mutex<rusty::Option<rusty::Box<srpc::ChannelFactoryBase@srpc.channel, rusty::alloc::Global>>>, srpc::ConnectionMetrics@srpc.connection_metrics)",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::add_on_connected(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::add_on_disconnected(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::add_on_error(rusty::Function<void (srpc::RpcError@srpc.errors, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const>) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::add_on_reconnected(rusty::Function<void (bool) const>) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::add_on_reconnecting(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::check_server_instance(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::circuit_breaker_config() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::circuit_breaker_state() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::clear_connection_callbacks() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::clear_pending_requests(int) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::client_mode() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::close() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::connect(signed char const*, bool) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::connected() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::connection() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::connection_state() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::create(rusty::Arc<srpc::PollThread@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::handle_free(long) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::has_connection() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::has_pending_channel_factory() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::heartbeat_config() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::host() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::is_idle(unsigned long, unsigned long) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::is_reconnecting() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::keepalive_config() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::metrics() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::new_(rusty::Arc<srpc::PollThread@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::operator=(srpc::Client@srpc.client&&)",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::pause() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::pending_request_count() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::reconnect(rusty::Function<void (bool)>) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::resume() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::rpc_id() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::server_instance_id() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_buffering_config(srpc::BufferingConfig@srpc.client const&) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_channel_factory(rusty::Box<srpc::ChannelFactoryBase@srpc.channel, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_circuit_breaker(srpc::CircuitBreakerConfig@srpc.circuit_breaker const&) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_client_mode(bool) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_heartbeat(srpc::HeartbeatConfig@srpc.heartbeat const&) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_keepalive(srpc::KeepaliveConfig@srpc.client const&) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_on_server_restart(rusty::Function<void (unsigned long, unsigned long)>) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_reconnect_policy(srpc::ReconnectPolicy@srpc.reconnect_policy const&) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_rpc_id(int) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_time(long) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::set_valid(bool) const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::time() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::timeout() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::try_reconnect_if_needed() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::validate_connection() const",
                ),
                (
                    "T",
                    "srpc::Client@srpc.client::~Client()",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::ClientConnection(srpc::ClientConnection@srpc.client&&)",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::ClientConnection(rusty::Arc<srpc::PollThread@srpc.reactor>, rusty::Mutex<rusty::Option<rusty::Box<srpc::FiberChannel@srpc.fiber_channel, rusty::alloc::Global>>>, rusty::Mutex<rusty::Option<rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>>>, rusty::Cell<bool>, rusty::Mutex<rusty::Option<rusty::Box<srpc::ChannelFactoryBase@srpc.channel, rusty::alloc::Global>>>, srpc::Counter@srpc.basetypes, rusty::Mutex<std_port::collections::hash::map::HashMap@std_port<long, rusty::Arc<srpc::Future@srpc.client>, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>>, rusty::Mutex<rusty::port::vec::Vec@vec_port.vec<rusty::Option<rusty::Function<void (int, unsigned char const*, unsigned long)>>, rusty::alloc::Global>>, srpc::ConnectionStateMachine@srpc.connection_state, rusty::Cell<srpc::ReconnectPolicy@srpc.reconnect_policy>, srpc::ReconnectState@srpc.client, rusty::Cell<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>, rusty::Cell<srpc::BufferingConfig@srpc.client>, srpc::RequestQueue@srpc.request_queue, rusty::Cell<unsigned long>, rusty::RefCell<rusty::Function<void (unsigned long, unsigned long)>>, rusty::Cell<srpc::KeepaliveConfig@srpc.client>, srpc::HeartbeatManager@srpc.heartbeat, srpc::CircuitBreaker@srpc.circuit_breaker, rusty::Arc<srpc::CallbackManager@srpc.callbacks>, rusty::Cell<unsigned long>, srpc::ConnectionMetrics@srpc.connection_metrics, rusty::sync::Weak<srpc::ClientConnection@srpc.client>, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, unsigned long, rusty::Cell<bool>, bool)",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::abort_reconnect()",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::allow_request_with_circuit_metrics() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::apply_keepalive_options()",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::bind_channel(rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::bind_channel_direct(rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::bind_channel_via_poll_thread(rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::bind_factory(rusty::Box<srpc::ChannelFactoryBase@srpc.channel, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::buffering_config() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::channel_reconnect_attempts_count() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::check_pending_write_update() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::check_server_instance(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::circuit_breaker_config() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::circuit_breaker_state() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::clear_pending_requests(int) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::close() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::connect(signed char const*) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::connect_via_factory(signed char const*) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::connected() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::connection_state() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::content_size() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::decode_response_and_notify(unsigned char const*, unsigned long) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::dispatch_frame_via_channel(unsigned char const*, unsigned long) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::enqueue_heartbeat_probe() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::fail_pending_future(long, int) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::fd() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::force_connected_for_testing()",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::handle_error() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::handle_free(long) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::handle_read() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::handle_write() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::heartbeat_config() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::host() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::install_self_weak_for_testing(rusty::sync::Weak<srpc::ClientConnection@srpc.client>)",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::invalidate_pending_futures() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::invoke_connected_callback() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::invoke_disconnected_callback() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::invoke_error_callback(int, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::invoke_reconnected_callback(bool) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::invoke_reconnecting_callback() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::is_channel_mode() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::is_closed() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::is_factory_bound() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::is_idle(unsigned long, unsigned long) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::is_reconnecting() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::keepalive_config() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::last_activity_time() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::map_system_error(int)",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::mark_closing() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::metrics() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::new_(rusty::Arc<srpc::PollThread@srpc.reactor>)",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::on_channel_closed_fan_out() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::on_request_dispatched(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::on_response_received(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::operator=(srpc::ClientConnection@srpc.client&&)",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::pause() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::pending_future_count() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::pending_request_count() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::poll_mode() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::reconnect(rusty::Function<void (bool)>) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::reconnect_policy() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::record_circuit_result(int) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::record_circuit_state_transition(srpc::CircuitState@srpc.circuit_breaker, srpc::CircuitState@srpc.circuit_breaker) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::replay_pending_requests() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::replay_pending_requests_for_test() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::reset_channel_mode_for_reconnect() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::resume() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::run_recv_loop() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::server_instance_id() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::set_buffering_config(srpc::BufferingConfig@srpc.client const&) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::set_callback_manager(rusty::Arc<srpc::CallbackManager@srpc.callbacks> const&)",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::set_circuit_breaker_config(srpc::CircuitBreakerConfig@srpc.circuit_breaker const&) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::set_heartbeat_config(srpc::HeartbeatConfig@srpc.heartbeat const&) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::set_keepalive(srpc::KeepaliveConfig@srpc.client const&) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::set_on_server_restart(rusty::Function<void (unsigned long, unsigned long)>) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::set_reconnect_address_for_testing(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::set_reconnect_policy(srpc::ReconnectPolicy@srpc.reconnect_policy const&) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::should_trip_circuit_for_error(int)",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::update_last_activity(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::update_pending_queue_config_for_test(srpc::RequestQueueConfig@srpc.request_queue const&) const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::validate_connection() const",
                ),
                (
                    "T",
                    "srpc::ClientConnection@srpc.client::~ClientConnection()",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::ClientPool(srpc::ClientPool@srpc.client&&)",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::ClientPool(rusty::Option<rusty::Arc<srpc::PollThread@srpc.reactor>>, rusty::Mutex<srpc::PoolState@srpc.client>, rusty::Mutex<srpc::PoolConfig@srpc.client>)",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::address_count() const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::close_all_idle(unsigned long) const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::close_idle_clients(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&, unsigned long) const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::get_client(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::get_healthy_client_count(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::is_client_healthy(rusty::Arc<srpc::Client@srpc.client> const&) const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::new_(rusty::Option<rusty::Arc<srpc::PollThread@srpc.reactor>>, srpc::PoolConfig@srpc.client)",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::operator=(srpc::ClientPool@srpc.client&&)",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::pool_config() const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::remove_all_unhealthy() const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::remove_unhealthy_clients(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::set_pool_config(srpc::PoolConfig@srpc.client) const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::total_client_count() const",
                ),
                (
                    "T",
                    "srpc::ClientPool@srpc.client::~ClientPool()",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::add_completion_callback(rusty::Function<void ()>) const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::create(long, srpc::FutureAttr@srpc.client)",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::get_error_code() const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::get_options() const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::get_reply() const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::get_retry_count() const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::get_timeout_type() const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::get_xid() const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::increment_retry_count()",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::new_(long, srpc::FutureAttr@srpc.client)",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::notify_ready(rusty::Arc<srpc::Future@srpc.client>) const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::ready() const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::safe_release(rusty::Arc<srpc::Future@srpc.client>)",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::set_options(srpc::RequestOptions@srpc.request_options const&) const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::set_timeout_type(srpc::TimeoutType@srpc.request_options)",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::should_retry() const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::timed_out() const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::timed_wait(double) const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::wait() const",
                ),
                (
                    "T",
                    "srpc::Future@srpc.client::wait_with_options() const",
                ),
                (
                    "T",
                    "srpc::FutureAttr@srpc.client::clone() const",
                ),
                (
                    "T",
                    "srpc::FutureAttr@srpc.client::default_()",
                ),
                (
                    "T",
                    "srpc::FutureAttr@srpc.client::new_(srpc::detail::CallbackWrapper@srpc.callback_wrapper<rusty::Function<void (rusty::Arc<srpc::Future@srpc.client>) const>>)",
                ),
                (
                    "T",
                    "srpc::FutureState@srpc.client::new_()",
                ),
                (
                    "T",
                    "srpc::KeepaliveConfig@srpc.client::aggressive()",
                ),
                (
                    "T",
                    "srpc::KeepaliveConfig@srpc.client::clone() const",
                ),
                (
                    "T",
                    "srpc::KeepaliveConfig@srpc.client::disabled()",
                ),
                (
                    "T",
                    "srpc::KeepaliveConfig@srpc.client::new_()",
                ),
                (
                    "T",
                    "srpc::KeepaliveConfig@srpc.client::relaxed()",
                ),
                (
                    "T",
                    "srpc::PoolConfig@srpc.client::aggressive()",
                ),
                (
                    "T",
                    "srpc::PoolConfig@srpc.client::clone() const",
                ),
                (
                    "T",
                    "srpc::PoolConfig@srpc.client::conservative()",
                ),
                (
                    "T",
                    "srpc::PoolConfig@srpc.client::defaults()",
                ),
                (
                    "T",
                    "srpc::PoolConfig@srpc.client::new_()",
                ),
                (
                    "T",
                    "srpc::PoolConfig@srpc.client::no_health_check()",
                ),
                (
                    "T",
                    "srpc::PoolState@srpc.client::new_()",
                ),
                (
                    "T",
                    "srpc::classify_request_failure@srpc.client(int)",
                ),
                (
                    "T",
                    "srpc::client_log_line@srpc.client(int, int, signed char const*, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "srpc::client_rand@srpc.client(int, int)",
                ),
                (
                    "T",
                    "srpc::client_sink_proxy@srpc.client(srpc::BufferSink@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::client_source_proxy@srpc.client(srpc::BufferSource@srpc.serializable&)",
                ),
                (
                    "T",
                    "srpc::client_text@srpc.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::client_text_i32@srpc.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, int, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::client_text_str@srpc.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::client_text_str_i32@srpc.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, int, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::client_text_str_pair@srpc.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::client_text_u32_str@srpc.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, unsigned int, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::client_text_u64_pair@srpc.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, unsigned long, std::__1::basic_string_view<char, std::__1::char_traits<char>>, unsigned long, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "srpc::client_verify@srpc.client(bool)",
                ),
                (
                    "T",
                    "srpc::clientconn_addr_to_string@srpc.client(signed char const*)",
                ),
                (
                    "T",
                    "srpc::clientconn_bind_channel_via_poll_thread@srpc.client(srpc::ClientConnection@srpc.client const&, rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "srpc::clientconn_connect_via_factory@srpc.client(srpc::ClientConnection@srpc.client const&, signed char const*)",
                ),
                (
                    "T",
                    "srpc::clientconn_decode_response_and_notify@srpc.client(srpc::ClientConnection@srpc.client const&, unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::clientconn_dispatch_frame_via_channel@srpc.client(srpc::ClientConnection@srpc.client const&, unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "srpc::clientconn_enqueue_heartbeat_probe@srpc.client(srpc::ClientConnection@srpc.client const&)",
                ),
                (
                    "T",
                    "srpc::clientconn_fiber_channel_ptr@srpc.client(rusty::Option<rusty::Box<srpc::FiberChannel@srpc.fiber_channel, rusty::alloc::Global>> const&)",
                ),
                (
                    "T",
                    "srpc::clientconn_make_fiber_channel@srpc.client(rusty::Box<srpc::ChannelConnectionBase@srpc.channel, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "srpc::clientconn_map_system_error@srpc.client(int)",
                ),
                (
                    "T",
                    "srpc::clientconn_monotonic_ms_now@srpc.client()",
                ),
                (
                    "T",
                    "srpc::clientconn_reconnect@srpc.client(srpc::ClientConnection@srpc.client const&, rusty::Function<void (bool)>)",
                ),
                (
                    "T",
                    "srpc::clientconn_recv_job_entry@srpc.client(rusty::sync::Weak<srpc::ClientConnection@srpc.client>)",
                ),
                (
                    "T",
                    "srpc::clientconn_run_recv_loop@srpc.client(srpc::ClientConnection@srpc.client const&)",
                ),
                (
                    "T",
                    "srpc::clientpool_close_all_idle@srpc.client(srpc::ClientPool@srpc.client const&, unsigned long)",
                ),
                (
                    "T",
                    "srpc::clientpool_close_idle_clients@srpc.client(srpc::ClientPool@srpc.client const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&, unsigned long)",
                ),
                (
                    "T",
                    "srpc::clientpool_connect_client@srpc.client(rusty::Arc<srpc::Client@srpc.client> const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::clientpool_get_client@srpc.client(srpc::ClientPool@srpc.client const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::clientpool_get_healthy_client_count@srpc.client(srpc::ClientPool@srpc.client const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::clientpool_is_client_healthy_with@srpc.client(srpc::PoolConfig@srpc.client, rusty::Arc<srpc::Client@srpc.client> const&)",
                ),
                (
                    "T",
                    "srpc::clientpool_remove_all_unhealthy@srpc.client(srpc::ClientPool@srpc.client const&)",
                ),
                (
                    "T",
                    "srpc::clientpool_remove_unhealthy_clients@srpc.client(srpc::ClientPool@srpc.client const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "srpc::clientpool_select@srpc.client(srpc::LoadBalancingStrategy@srpc.load_balancer, rusty::port::vec::Vec@vec_port.vec<rusty::Arc<srpc::Client@srpc.client>, rusty::alloc::Global> const&, srpc::LoadBalancerState@srpc.load_balancer const&, unsigned long)",
                ),
                (
                    "T",
                    "srpc::make_pending_queue@srpc.client(srpc::RequestQueueConfig@srpc.request_queue const&)",
                ),
                (
                    "T",
                    "srpc::make_prefilled_cb_slots@srpc.client()",
                ),
                (
                    "T",
                    "srpc::make_write_archive@srpc.client(srpc::BufferSink@srpc.serializable*)",
                ),
                (
                    "T",
                    "srpc::reply_buffer_empty@srpc.client()",
                ),
                (
                    "T",
                    "srpc::reply_buffer_fill@srpc.client(srpc::ReplyBuffer@srpc.client&, std::__1::span<unsigned char const, 18446744073709551615ul>)",
                ),
                (
                    "T",
                    "srpc::request_copy_reply@srpc.client(rusty::Arc<srpc::Future@srpc.client> const&, rusty::Arc<srpc::Future@srpc.client> const&)",
                ),
            }
        ),
    ),
}

# Symbols that a module acquires in the production library from a hand-written
# module *implementation unit* that is not part of the generated crate.
#
# srpc.epoll_wrapper follows Rust std's sys-module pattern: the generated
# .cppm is the interface unit, and reactor/epoll_platform_linux.cc is the
# platform implementation unit that CMake compiles into libsrpc.a (see the
# "Platform implementation units for srpc.epoll_wrapper" block in
# CMakeLists.txt). Those definitions are therefore legitimately absent from
# the independently compiled crate object and present in production.
#
# This is an exhaustive allowlist, not a relaxation: the crate object must
# still match ABI_SPECS exactly, and the production library must match
# ABI_SPECS plus exactly these entries -- no more, no less.
PLATFORM_IMPL_SYMBOLS = {
    "srpc.epoll_wrapper": frozenset(
        {
            ("T", "srpc::epoll_add_impl@srpc.epoll_wrapper(int, int, int)"),
            ("T", "srpc::epoll_event_zeroed@srpc.epoll_wrapper()"),
            ("T", "srpc::epoll_open@srpc.epoll_wrapper()"),
            ("T", "srpc::epoll_remove_impl@srpc.epoll_wrapper(int, int)"),
            (
                "T",
                "srpc::epoll_update_impl@srpc.epoll_wrapper(int, int, int, int)",
            ),
        }
    ),
}
EXPECTED_TOTAL_PLATFORM_SYMBOLS = 5


class GateError(RuntimeError):
    """A crate generation, compilation, import, or ABI-parity failure."""


def repository_root() -> Path:
    return Path(__file__).resolve().parents[1]


def executable(root: Path, value: str, description: str) -> Path:
    candidate = Path(value)
    if candidate.is_absolute() or "/" in value:
        resolved = candidate if candidate.is_absolute() else root / candidate
    else:
        found = shutil.which(value)
        if found is None:
            raise GateError(f"{description} is unavailable: {value}")
        resolved = Path(found)
    # Preserve the invoked basename. Clang selects C++ driver behavior from
    # argv[0], and resolving a `clang++ -> clang-N` symlink silently drops the
    # implicit C++ standard-library link in the direct gate commands.
    resolved = Path(os.path.abspath(resolved))
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise GateError(f"{description} is unavailable: {resolved}")
    return resolved


def resolve_archiver(root: Path, nm: Path) -> Path:
    """Locate llvm-ar for the same toolchain as `nm`.

    The gate needs llvm-ar to bundle the non-crate support objects into an
    archive (see compile_support_inputs). This used to hard-code a bare
    `llvm-ar` beside nm, which only holds when the toolchain is installed
    unsuffixed -- a Homebrew keg, say. apt.llvm.org, which the CI image uses,
    installs VERSION-SUFFIXED binaries and only runs update-alternatives for
    clang/clang++/llvm-config, so beside `/usr/bin/llvm-nm-22` there is an
    `llvm-ar-22` and no plain `llvm-ar` at all -- the gate died with
    "ar is unavailable: /usr/bin/llvm-ar".

    So mirror nm's NAME SHAPE, not just its directory: substitute
    `llvm-nm` -> `llvm-ar` while preserving whatever version suffix nm carries
    (`llvm-nm-22` -> `llvm-ar-22`). The version is never hard-coded; it is read
    off nm.

    Candidates are tried most-toolchain-specific first, so a matching llvm-ar
    always wins over a stray one:

      1. `<nm dir>/llvm-ar<suffix>`   -- same directory AND same version
      2. `<nm dir>/llvm-ar`           -- same directory, unversioned layout
      3. `llvm-ar<suffix>` on PATH
      4. `llvm-ar` on PATH
      5. `ar` on PATH                -- last resort, only if no llvm-ar exists

    If nothing is found this raises, naming every path it tried, rather than
    proceeding without an archiver.
    """
    suffix = nm.name[len("llvm-nm") :] if nm.name.startswith("llvm-nm") else ""
    # Preserve order, drop duplicates (suffix == "" collapses 1 into 2).
    beside_names = list(dict.fromkeys(["llvm-ar" + suffix, "llvm-ar"]))
    path_names = list(dict.fromkeys(["llvm-ar" + suffix, "llvm-ar", "ar"]))

    attempted = []
    for name in beside_names:
        candidate = nm.parent / name
        attempted.append(str(candidate))
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return Path(os.path.abspath(candidate))
    for name in path_names:
        attempted.append(f"{name} (PATH)")
        found = shutil.which(name)
        if found is not None:
            return Path(os.path.abspath(found))
    raise GateError(
        f"ar is unavailable for the toolchain of {nm}: tried "
        + ", ".join(attempted)
    )


def run(command: list[str], cwd: Path) -> str:
    completed = subprocess.run(
        command,
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        diagnostic = (completed.stdout + completed.stderr).strip()
        rendered = " ".join(command)
        raise GateError(
            f"command failed with exit {completed.returncode}: {rendered}\n{diagnostic}"
        )
    return completed.stdout


def git_output(cwd: Path, arguments: list[str], description: str) -> str:
    try:
        completed = subprocess.run(
            # See `extraction.ownership_exception`: vouch for exactly the
            # directory being inspected so the pin attestation survives a
            # container job whose checkout is owned by a different uid. It
            # relaxes git's ownership heuristic only -- every pin comparison
            # below still fails closed.
            ["git", *extraction.ownership_exception(cwd), *arguments],
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        raise GateError(f"cannot inspect {description}: {exc}") from exc
    if completed.returncode != 0:
        diagnostic = (completed.stdout + completed.stderr).strip()
        raise GateError(f"cannot inspect {description}: {diagnostic}")
    return completed.stdout.strip()


def verify_transpiler_build_info(root: Path, transpiler: Path) -> None:
    try:
        completed = subprocess.run(
            [str(transpiler), "--build-info"],
            cwd=root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    except OSError as exc:
        raise GateError(
            f"cannot read rusty-cpp transpiler build info: {exc}"
        ) from exc
    if completed.returncode != 0:
        diagnostic = (completed.stdout + completed.stderr).strip()
        raise GateError(
            "rusty-cpp transpiler --build-info failed with exit "
            f"{completed.returncode}: {diagnostic}"
        )
    lines = completed.stdout.splitlines()
    if len(lines) != 1:
        raise GateError(
            "rusty-cpp transpiler --build-info must emit exactly one JSON line"
        )
    try:
        build_info = json.loads(lines[0])
    except json.JSONDecodeError as exc:
        raise GateError(
            f"rusty-cpp transpiler --build-info emitted invalid JSON: {exc}"
        ) from exc
    if not isinstance(build_info, dict) or set(build_info) != {
        "git_hash",
        "git_dirty",
    }:
        raise GateError(
            "rusty-cpp transpiler --build-info JSON keys must be exactly "
            "git_hash and git_dirty"
        )
    if build_info["git_hash"] != REQUIRED_RUSTY_CPP_COMMIT:
        raise GateError(
            "rusty-cpp transpiler build commit mismatch: "
            f"expected {REQUIRED_RUSTY_CPP_COMMIT}, "
            f"got {build_info['git_hash']!r}"
        )
    if build_info["git_dirty"] is not False:
        raise GateError("rusty-cpp transpiler build must report git_dirty=false")


def verify_pinned_toolchain(root: Path, transpiler: Path) -> None:
    index_entry = git_output(
        root,
        ["ls-files", "--stage", "--", RUSTY_CPP_SUBMODULE],
        "rusty-cpp gitlink",
    ).split()
    if (
        len(index_entry) < 3
        or index_entry[0] != "160000"
        or index_entry[1] != REQUIRED_RUSTY_CPP_COMMIT
    ):
        actual = index_entry[1] if len(index_entry) >= 2 else "missing"
        raise GateError(
            "rusty-cpp gitlink pin mismatch: "
            f"expected {REQUIRED_RUSTY_CPP_COMMIT}, got {actual}"
        )
    submodule = root / RUSTY_CPP_SUBMODULE
    head = git_output(submodule, ["rev-parse", "HEAD"], "rusty-cpp HEAD")
    if head != REQUIRED_RUSTY_CPP_COMMIT:
        raise GateError(
            "rusty-cpp submodule HEAD mismatch: "
            f"expected {REQUIRED_RUSTY_CPP_COMMIT}, got {head}"
        )
    dirty = git_output(
        submodule,
        ["status", "--porcelain", "--untracked-files=no"],
        "rusty-cpp worktree",
    )
    if dirty:
        raise GateError("rusty-cpp submodule has tracked local changes")
    verify_transpiler_build_info(root, transpiler)


def require_extraction_check(root: Path, transpiler: Path) -> None:
    run(
        [
            sys.executable,
            EXTRACTION_DRIVER,
            "--check",
            "--transpiler",
            str(transpiler),
        ],
        root,
    )


def load_owned_modules(root: Path) -> list[extraction.ModuleEntry]:
    try:
        crate = extraction.crate_root(root)
        modules = extraction.load_manifest(crate, crate / EXTRACTION_MANIFEST)
    except extraction.ExtractionError as exc:
        raise GateError(f"cannot load extraction ownership: {exc}") from exc
    actual = {module.cpp_module for module in modules}
    expected = set(ABI_SPECS)
    if actual != expected:
        details = ["crate-mode ABI ratchet does not match extraction manifest"]
        if expected - actual:
            details.append(
                "missing manifest module(s): " + ", ".join(sorted(expected - actual))
            )
        if actual - expected:
            details.append(
                "missing ABI specification(s): " + ", ".join(sorted(actual - expected))
            )
        raise GateError("\n".join(details))
    return modules


def read_generated(path: Path, description: str) -> str:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise GateError(f"missing generated {description} {path}: {exc}") from exc
    # A generated module's global module fragment may contain compiler-owned
    # runtime support.  Some of those helpers use "unsupported" in legitimate
    # diagnostics (for example, an unreachable conversion branch), so it is
    # not sound to classify that fixed support text as an unimplemented user
    # lowering.  Generated Rust declarations and definitions live in the
    # named-module purview; keep the strict placeholder ratchet there.  Files
    # without a named-module declaration (the crate root CMake file included)
    # are still checked in full.
    module_declaration = re.search(r"^export module [^;\n]+;[ \t]*$", text, re.MULTILINE)
    placeholder_region = (
        text[module_declaration.start() :] if module_declaration is not None else text
    )
    # Drop the one allowlisted compiler diagnostic form (see
    # BENIGN_GENERATED_DIAGNOSTIC) before applying the strict token ratchet.
    # Only that exact comment line is removed, so any other TODO/UNSUPPORTED/
    # skipped text -- including a differently worded by-value-cycle marker --
    # still fails the gate.
    placeholder_region = BENIGN_GENERATED_DIAGNOSTIC.sub("", placeholder_region)
    placeholder = PLACEHOLDER.search(placeholder_region)
    if placeholder is not None:
        raise GateError(
            f"generated {description} contains placeholder marker "
            f"{placeholder.group(0)!r}: {path}"
        )
    return text


def require_exact_module_imports(
    text: str, module_name: str, expected: list[str]
) -> None:
    """Require the exact private named-module dependencies of a child."""

    matches = re.findall(
        r"^(export )?import ([^;\n]+);[ \t]*$",
        text,
        flags=re.MULTILINE,
    )
    actual = [imported for _, imported in matches]
    exported = [imported for prefix, imported in matches if prefix]
    if actual != expected or exported:
        raise GateError(
            f"generated {module_name} module private imports must be exactly "
            f"{expected!r}; got {actual!r}, exported={exported!r}"
        )


def require_cpp_surfaces(
    root: Path, output: Path, modules: list[extraction.ModuleEntry]
) -> None:
    expected_files = {f"{module.cpp_module}.cppm" for module in modules}
    expected_files.add("srpc.cppm")
    actual_files = {
        path.relative_to(output).as_posix()
        for path in output.rglob("*.cppm")
        if path.is_file()
    }
    if actual_files != expected_files:
        raise GateError(
            "generated C++ module census mismatch: expected "
            f"{sorted(expected_files)!r}, got {sorted(actual_files)!r}"
        )

    runtime_facade_output = output / "rusty"
    if runtime_facade_output.exists():
        raise GateError(
            "rustc-only rusty runtime facade leaked into generated C++ output"
        )
    generated_cmake = read_generated(output / "CMakeLists.txt", "crate CMake file")
    for forbidden in ("rusty/rusty.cppm", "add_subdirectory(rusty"):
        if forbidden in generated_cmake:
            raise GateError(
                "rustc-only rusty runtime facade leaked into generated CMake: "
                f"{forbidden!r}"
            )

    for module in modules:
        path = output / f"{module.cpp_module}.cppm"
        text = read_generated(path, f"child module {module.cpp_module}")
        missing = sorted(
            fragment
            for fragment in ABI_SPECS[module.cpp_module].surface
            if fragment not in text
        )
        if missing:
            raise GateError(
                f"generated module {module.cpp_module} is missing required surface:\n  "
                + "\n  ".join(missing)
            )

        if "namespace srpc::" in text:
            raise GateError(
                f"generated module {module.cpp_module} drifted to a nested namespace"
            )
        atomic_preamble = "#include <rusty/sync/atomic.hpp>"
        # srpc.epoll_wrapper and srpc.threading joined this set when their
        # carriers were retired: both canonical sources use
        # `std::sync::atomic`, so the structured preamble carries the same
        # include. Membership is still exact -- any other module that grew one
        # would fail the `elif` below.
        atomic_modules = {
            "srpc.basetypes",
            "srpc.connection_metrics",
            "srpc.completion_tracker",
            "srpc.epoll_wrapper",
            "srpc.threading",
        }
        if module.cpp_module in atomic_modules:
            if text.count(atomic_preamble) != 1:
                raise GateError(
                    f"generated {module.cpp_module} must contain exactly one "
                    "structured atomic preamble include"
                )
            ordered = (
                text.find("\nmodule;\n"),
                text.find(atomic_preamble),
                text.find("#include <cstdint>"),
                text.find(f"export module {module.cpp_module};"),
            )
            if -1 in ordered or list(ordered) != sorted(ordered):
                raise GateError(
                    f"generated {module.cpp_module} atomic preamble is not "
                    "between the global module fragment and standard includes"
                )
        elif atomic_preamble in text:
            raise GateError(
                f"atomic module preamble leaked into {module.cpp_module}"
            )

        rand_preamble = '#include "misc/srpc_rand.h"'
        if module.cpp_module == "srpc.rand":
            require_exact_module_imports(text, "srpc.rand", ["vec_port.vec"])
            if text.count(rand_preamble) != 1:
                raise GateError(
                    "generated srpc.rand must contain exactly one structured "
                    "C-kernel preamble include"
                )
            ordered = (
                text.find("\nmodule;\n"),
                text.find(rand_preamble),
                text.find("#include <cstdint>"),
                text.find("export module srpc.rand;"),
            )
            if -1 in ordered or list(ordered) != sorted(ordered):
                raise GateError(
                    "generated srpc.rand C-kernel preamble is not between the "
                    "global module fragment and standard includes"
                )
            if "std::abort()" in text:
                raise GateError(
                    "generated srpc.rand hard-aborts a Rust assertion instead "
                    "of preserving panic/unwind failure semantics"
                )
        elif rand_preamble in text:
            raise GateError(
                f"rand C-kernel preamble leaked into {module.cpp_module}"
            )

        timing_preamble = '#include "misc/srpc_timing.h"'
        # srpc.threading joined this set with its carrier's retirement: the
        # canonical spin lock calls the same `srpc_cpu_pause` timing kernel.
        # Unlike the other two it is not import-free -- its `verify()` calls
        # reach srpc.debugging -- so the exact-import expectation is per module
        # rather than a blanket empty list.
        timing_modules = {
            "srpc.basetypes": [],
            "srpc.circuit_breaker": [],
            "srpc.threading": ["srpc.debugging"],
        }
        if module.cpp_module in timing_modules:
            require_exact_module_imports(
                text, module.cpp_module, timing_modules[module.cpp_module]
            )
            if text.count(timing_preamble) != 1:
                raise GateError(
                    f"generated {module.cpp_module} must contain exactly one "
                    "structured timing-kernel preamble include"
                )
            ordered = (
                text.find("\nmodule;\n"),
                text.find(timing_preamble),
                text.find("#include <cstdint>"),
                text.find(f"export module {module.cpp_module};"),
            )
            if -1 in ordered or list(ordered) != sorted(ordered):
                raise GateError(
                    f"generated {module.cpp_module} timing preamble is not "
                    "between the global module fragment and standard includes"
                )
        elif timing_preamble in text:
            raise GateError(
                f"timing C-kernel preamble leaked into {module.cpp_module}"
            )

        if module.cpp_module == "srpc.request_options":
            require_exact_module_imports(
                text, "srpc.request_options", ["srpc.rand"]
            )
            for forbidden in (
                "namespace rand =",
                "using ::rand::",
                "using ::srpc::rand::",
                "using ::srpc::randgen_",
            ):
                if forbidden in text:
                    raise GateError(
                        "generated request-options private flat import leaked "
                        f"an alias/using surface: {forbidden!r}"
                    )
        elif module.cpp_module == "srpc.reconnect_policy":
            require_exact_module_imports(
                text, "srpc.reconnect_policy", ["srpc.rand"]
            )
            for forbidden in (
                "namespace rand =",
                "using ::rand::",
                "using ::srpc::rand::",
                "using ::srpc::randgen_",
            ):
                if forbidden in text:
                    raise GateError(
                        "generated reconnect-policy private flat import leaked "
                        f"an alias/using surface: {forbidden!r}"
                    )

        netdb_preamble = "#include <netdb.h>"
        if module.cpp_module == "srpc.connection_state":
            require_exact_module_imports(text, "srpc.connection_state", [])
        elif module.cpp_module == "srpc.heartbeat":
            require_exact_module_imports(
                text, "srpc.heartbeat", ["srpc.circuit_breaker"]
            )
        elif module.cpp_module == "srpc.request_queue":
            require_exact_module_imports(
                text,
                "srpc.request_queue",
                ["vec_port.vec", "srpc.circuit_breaker"],
            )
        elif module.cpp_module == "srpc.load_balancer":
            require_exact_module_imports(text, "srpc.load_balancer", [])
            live_cpp = "\n".join(
                line
                for line in text.splitlines()
                if not line.lstrip().startswith("//")
            )
            for forbidden in (
                "rusty::LoadBalancerClient",
                "rusty::LoadBalancerMetrics",
                "rusty::LoadBalancerClientHandle",
                "rusty::LoadBalancerClientVec",
                "requires ",
            ):
                if forbidden in live_cpp:
                    raise GateError(
                        "rustc-only load-balancer facade leaked into generated "
                        f"C++: {forbidden!r}"
                    )
        elif module.cpp_module == "srpc.utils":
            require_exact_module_imports(text, "srpc.utils", ["srpc.logging"])
            if text.count(netdb_preamble) != 1:
                raise GateError(
                    "generated srpc.utils must contain exactly one structured "
                    "netdb preamble include"
                )
            ordered = (
                text.find("\nmodule;\n"),
                text.find(netdb_preamble),
                text.find("#include <cstdint>"),
                text.find("export module srpc.utils;"),
            )
            if -1 in ordered or list(ordered) != sorted(ordered):
                raise GateError(
                    "generated srpc.utils netdb preamble is not between the "
                    "global module fragment and standard includes"
                )
            for forbidden in (
                "export import srpc.logging;",
                "namespace logging =",
                "using ::srpc::log_line",
                "srpc::logging::log_line",
            ):
                if forbidden in text:
                    raise GateError(
                        "generated utils private indexed import leaked or "
                        f"misresolved its surface: {forbidden!r}"
                    )
        elif module.cpp_module == "srpc.frame_codec":
            require_exact_module_imports(
                text, "srpc.frame_codec", ["srpc.internal_protocol"]
            )
            frame_preambles = ("#include <vector>", "#include <rusty/io.hpp>")
            for preamble in frame_preambles:
                if text.count(preamble) != 1:
                    raise GateError(
                        "generated srpc.frame_codec must contain exactly one "
                        f"structured preamble include {preamble!r}"
                    )
            ordered = (
                text.find("\nmodule;\n"),
                text.find(frame_preambles[0]),
                text.find(frame_preambles[1]),
                text.find("#include <cstdint>"),
                text.find("export module srpc.frame_codec;"),
            )
            if -1 in ordered or list(ordered) != sorted(ordered):
                raise GateError(
                    "generated srpc.frame_codec structured preambles are not "
                    "ordered between the global fragment and standard includes"
                )
            if "rusty::StdVector" in text:
                raise GateError(
                    "rustc-only StdVector facade leaked into generated FrameCodec"
                )
        elif netdb_preamble in text:
            raise GateError(
                f"utils netdb preamble leaked into {module.cpp_module}"
            )
        elif "#include <rusty/io.hpp>" in text:
            raise GateError(
                f"FrameCodec io preamble leaked into {module.cpp_module}"
            )

    root_text = read_generated(output / "srpc.cppm", "root module")
    if "#include <rusty/sync/atomic.hpp>" in root_text:
        raise GateError("atomic module preamble leaked into the crate root")
    if '#include "misc/srpc_rand.h"' in root_text:
        raise GateError("rand C-kernel preamble leaked into the crate root")
    if '#include "misc/srpc_timing.h"' in root_text:
        raise GateError("timing C-kernel preamble leaked into the crate root")
    if "#include <netdb.h>" in root_text:
        raise GateError("utils netdb preamble leaked into the crate root")
    if "#include <rusty/io.hpp>" in root_text:
        raise GateError("FrameCodec io preamble leaked into the crate root")
    root_required = {
        "export module srpc;",
        "namespace srpc {",
        *(f"export import {module.cpp_module};" for module in modules),
    }
    root_missing = sorted(
        fragment for fragment in root_required if fragment not in root_text
    )
    if root_missing:
        raise GateError(
            "generated root module is missing required surface:\n  "
            + "\n  ".join(root_missing)
        )


def require_zero_hand_slots(path: Path) -> None:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise GateError(f"missing generated hand-slot manifest {path}: {exc}") from exc
    if not re.search(
        r"^0 slot\(s\) requiring hand-attention across 0 file\(s\)\.$",
        text,
        re.MULTILINE,
    ):
        raise GateError(f"generated crate does not report zero hand slots: {path}")


def module_symbols(
    nm: Path,
    root: Path,
    binary: Path,
    module_name: str,
) -> set[tuple[str, str]]:
    output = run(
        [str(nm), "--defined-only", "--demangle", str(binary)],
        root,
    )
    symbols: set[tuple[str, str]] = set()
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        # Template instantiations and lambda helpers are optimization-sensitive
        # weak implementation details, not the strong module ABI ratcheted here.
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if symbol_owner_module(symbol) == module_name:
            symbols.add((kind, symbol))
    return symbols


def completion_raw_symbols(
    nm: Path,
    root: Path,
    binary: Path,
) -> list[tuple[str, str]]:
    """Return completion's strong entries without deduplicating aliases."""

    output = run(
        [str(nm), "--defined-only", "--demangle", str(binary)],
        root,
    )
    initializer = "initializer for module srpc.completion_tracker"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if (
            symbol_owner_module(symbol) == "srpc.completion_tracker"
            or symbol == initializer
        ):
            entries.append((kind, symbol))
    return entries


def require_completion_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin the initializer as well as the unique API.

    Factory-only construction: `CompletionTracker::new_()` and
    `CompletionTracker::with_config()` replaced the two public constructors, so
    this module now has NO constructor alias at all. An Itanium-ABI constructor
    is emitted twice (C1 complete-object and C2 base-object) and both demangle
    to the same name, which is exactly what the two entries here used to
    account for; a static factory is emitted once and is already covered by its
    ABI_SPECS entry. Measured on the object: two aliases -> zero, 33 -> 31.
    """

    expected = Counter(ABI_SPECS["srpc.completion_tracker"].symbols)
    expected[("T", "initializer for module srpc.completion_tracker")] += 1
    actual = Counter(entries)
    if actual == expected:
        return
    missing = sorted((expected - actual).elements())
    unexpected = sorted((actual - expected).elements())
    raise GateError(
        f"{description} completion ABI must contain exactly 31 raw strong "
        "entries (30 unique API symbols, no constructor alias, and the "
        f"module initializer); missing={missing!r}, unexpected={unexpected!r}"
    )


def rand_raw_symbols(
    nm: Path,
    root: Path,
    binary: Path,
) -> list[tuple[str, str]]:
    """Return rand's strong entries, including its module initializer."""

    output = run(
        [str(nm), "--defined-only", "--demangle", str(binary)],
        root,
    )
    initializer = "initializer for module srpc.rand"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if symbol_owner_module(symbol) == "srpc.rand" or symbol == initializer:
            entries.append((kind, symbol))
    return entries


def require_rand_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin rand's 12-function ABI and sole module initializer exactly."""

    expected = Counter(ABI_SPECS["srpc.rand"].symbols)
    expected[("T", "initializer for module srpc.rand")] += 1
    actual = Counter(entries)
    if actual == expected:
        return
    missing = sorted((expected - actual).elements())
    unexpected = sorted((actual - expected).elements())
    raise GateError(
        f"{description} rand ABI must contain exactly 13 raw strong entries "
        "(12 API symbols and the module initializer); "
        f"missing={missing!r}, unexpected={unexpected!r}"
    )


def request_options_raw_symbols(
    nm: Path,
    root: Path,
    binary: Path,
) -> list[tuple[str, str]]:
    """Return request-options strong entries, including its initializer."""

    output = run(
        [str(nm), "--defined-only", "--demangle", str(binary)],
        root,
    )
    initializer = "initializer for module srpc.request_options"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if (
            symbol_owner_module(symbol) == "srpc.request_options"
            or symbol == initializer
        ):
            entries.append((kind, symbol))
    return entries


def require_request_options_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin request-options' 12-function ABI and initializer exactly."""

    expected = Counter(ABI_SPECS["srpc.request_options"].symbols)
    expected[("T", "initializer for module srpc.request_options")] += 1
    actual = Counter(entries)
    if actual == expected:
        return
    missing = sorted((expected - actual).elements())
    unexpected = sorted((actual - expected).elements())
    raise GateError(
        f"{description} request-options ABI must contain exactly 13 raw strong "
        "entries (12 API symbols and the module initializer); "
        f"missing={missing!r}, unexpected={unexpected!r}"
    )


def reconnect_policy_raw_symbols(
    nm: Path,
    root: Path,
    binary: Path,
) -> list[tuple[str, str]]:
    """Return reconnect-policy strong entries, including its initializer."""

    output = run(
        [str(nm), "--defined-only", "--demangle", str(binary)],
        root,
    )
    initializer = "initializer for module srpc.reconnect_policy"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if (
            symbol_owner_module(symbol) == "srpc.reconnect_policy"
            or symbol == initializer
        ):
            entries.append((kind, symbol))
    return entries


def require_reconnect_policy_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin reconnect-policy's 11-function ABI and initializer exactly."""

    expected = Counter(ABI_SPECS["srpc.reconnect_policy"].symbols)
    expected[("T", "initializer for module srpc.reconnect_policy")] += 1
    actual = Counter(entries)
    if actual == expected:
        return
    missing = sorted((expected - actual).elements())
    unexpected = sorted((actual - expected).elements())
    raise GateError(
        f"{description} reconnect-policy ABI must contain exactly 12 raw "
        "strong entries (11 API symbols and the module initializer); "
        f"missing={missing!r}, unexpected={unexpected!r}"
    )


def circuit_breaker_raw_symbols(
    nm: Path,
    root: Path,
    binary: Path,
) -> list[tuple[str, str]]:
    """Return circuit-breaker strong entries, including its initializer."""

    output = run(
        [str(nm), "--defined-only", "--demangle", str(binary)],
        root,
    )
    initializer = "initializer for module srpc.circuit_breaker"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if (
            symbol_owner_module(symbol) == "srpc.circuit_breaker"
            or symbol == initializer
        ):
            entries.append((kind, symbol))
    return entries


def require_circuit_breaker_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin circuit-breaker's 20-function ABI and initializer exactly."""

    expected = Counter(ABI_SPECS["srpc.circuit_breaker"].symbols)
    expected[("T", "initializer for module srpc.circuit_breaker")] += 1
    actual = Counter(entries)
    if actual == expected:
        return
    missing = sorted((expected - actual).elements())
    unexpected = sorted((actual - expected).elements())
    raise GateError(
        f"{description} circuit-breaker ABI must contain exactly 21 raw "
        "strong entries (20 API symbols and the module initializer); "
        f"missing={missing!r}, unexpected={unexpected!r}"
    )


def exact_module_raw_symbols(
    nm: Path,
    root: Path,
    binary: Path,
    module_name: str,
) -> list[tuple[str, str]]:
    """Return one module's strong API entries and its sole initializer."""

    output = run(
        [str(nm), "--defined-only", "--demangle", str(binary)],
        root,
    )
    initializer = f"initializer for module {module_name}"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if symbol_owner_module(symbol) == module_name or symbol == initializer:
            entries.append((kind, symbol))
    return entries


def require_exact_module_raw_symbols(
    module_name: str,
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin a module's complete strong API and sole initializer exactly."""

    expected = Counter(ABI_SPECS[module_name].symbols)
    expected[("T", f"initializer for module {module_name}")] += 1
    actual = Counter(entries)
    if actual == expected:
        return
    missing = sorted((expected - actual).elements())
    unexpected = sorted((actual - expected).elements())
    raise GateError(
        f"{description} {module_name} ABI must contain exactly "
        f"{sum(expected.values())} raw strong entries (API symbols and the "
        f"module initializer); missing={missing!r}, unexpected={unexpected!r}"
    )


def basetypes_raw_symbols(
    nm: Path,
    root: Path,
    binary: Path,
) -> list[tuple[str, str]]:
    """Return basetypes strong entries, including its initializer."""

    output = run(
        [str(nm), "--defined-only", "--demangle", str(binary)],
        root,
    )
    initializer = "initializer for module srpc.basetypes"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if symbol_owner_module(symbol) == "srpc.basetypes" or symbol == initializer:
            entries.append((kind, symbol))
    return entries


def require_basetypes_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin basetypes' 28-entry API/data ABI and initializer exactly."""

    expected = Counter(ABI_SPECS["srpc.basetypes"].symbols)
    expected[("T", "initializer for module srpc.basetypes")] += 1
    actual = Counter(entries)
    if actual == expected:
        return
    missing = sorted((expected - actual).elements())
    unexpected = sorted((actual - expected).elements())
    raise GateError(
        f"{description} basetypes ABI must contain exactly 29 raw strong "
        "entries (28 API/data symbols and the module initializer); "
        f"missing={missing!r}, unexpected={unexpected!r}"
    )


def request_queue_raw_symbols(
    nm: Path,
    root: Path,
    binary: Path,
) -> list[tuple[str, str]]:
    """Return request-queue strong entries without constructor deduplication."""

    output = run(
        [str(nm), "--defined-only", "--demangle", str(binary)],
        root,
    )
    initializer = "initializer for module srpc.request_queue"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if symbol_owner_module(symbol) == "srpc.request_queue" or symbol == initializer:
            entries.append((kind, symbol))
    return entries


def require_request_queue_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin request-queue's API and initializer exactly.

    Factory-only construction: `RequestQueue::new_()` and
    `RequestQueue::with_config()` replaced the two public constructors, so this
    module now has NO constructor alias at all. An Itanium-ABI constructor is
    emitted twice (C1 complete-object and C2 base-object) and both demangle to
    the same name, which is exactly what the two `+= 1` lines here used to
    account for; a static factory is emitted once and is already covered by its
    ABI_SPECS entry. Measured on the object: two aliases -> zero, 30 -> 28.
    """

    expected = Counter(ABI_SPECS["srpc.request_queue"].symbols)
    expected[("T", "initializer for module srpc.request_queue")] += 1
    actual = Counter(entries)
    if actual == expected:
        return
    missing = sorted((expected - actual).elements())
    unexpected = sorted((actual - expected).elements())
    raise GateError(
        f"{description} request-queue ABI must contain exactly 28 raw strong "
        "entries (27 unique provider-owned symbols, no constructor alias, "
        f"and the module initializer); missing={missing!r}, "
        f"unexpected={unexpected!r}"
    )


def utils_raw_symbols(
    nm: Path,
    root: Path,
    binary: Path,
) -> list[tuple[str, str]]:
    """Return Utils strong entries without constructor/destructor deduplication."""

    return exact_module_raw_symbols(nm, root, binary, "srpc.utils")


def require_utils_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin Utils' API, C++ ctor/dtor aliases, and initializer exactly."""

    # Factory-only construction: `AddrInfo::new_()` and `AddrInfo::adopt()`
    # replaced the two public constructors. An Itanium-ABI constructor is
    # emitted twice (C1 complete-object and C2 base-object) and both demangle
    # to one name, so each contributed one ALIAS here on top of its unique
    # symbol; a static factory is emitted once and contributes no alias. The
    # private fieldwise ctor, the move ctor and the dtor are unaffected.
    # Measured on the object: five aliases -> three, 17 -> 15.
    aliased = (
        "srpc::AddrInfo@srpc.utils::AddrInfo(addrinfo*, rusty::Cell<bool>)",
        "srpc::AddrInfo@srpc.utils::AddrInfo(srpc::AddrInfo@srpc.utils&&)",
        "srpc::AddrInfo@srpc.utils::~AddrInfo()",
    )
    expected = Counter(ABI_SPECS["srpc.utils"].symbols)
    for symbol in aliased:
        expected[("T", symbol)] += 1
    expected[("T", "initializer for module srpc.utils")] += 1
    actual = Counter(entries)
    if actual == expected:
        return
    missing = sorted((expected - actual).elements())
    unexpected = sorted((actual - expected).elements())
    raise GateError(
        f"{description} Utils ABI must contain exactly 15 raw strong "
        "entries (11 unique provider-owned symbols, three C++ ABI aliases, "
        f"and the module initializer); missing={missing!r}, "
        f"unexpected={unexpected!r}"
    )




def function_parameter_open(symbol: str) -> int:
    """Return the outer function-parameter `(`, or the end for a data symbol."""

    close = symbol.rfind(")")
    if close == -1:
        return len(symbol)
    depth = 0
    for index in range(close, -1, -1):
        character = symbol[index]
        if character == ")":
            depth += 1
        elif character == "(":
            depth -= 1
            if depth == 0:
                return index
    return len(symbol)


def is_operator_angle(text: str, index: int) -> bool:
    """Return whether `<` at index spells a C++ operator rather than a template."""

    operator = text.rfind("operator", 0, index + 1)
    if operator == -1:
        return False
    candidate = "".join(text[operator : index + 1].split())
    return any(
        spelling.startswith(candidate)
        for spelling in (
            "operator<",
            "operator<=",
            "operator<=>",
            "operator<<",
            "operator<<=",
        )
    )


def actual_entity_declarator(symbol: str) -> str:
    """Remove parameter and optional return-type text from a demangled symbol."""

    prefix = symbol[: function_parameter_open(symbol)].rstrip()
    angle_depth = 0
    last_separator = -1
    for index, character in enumerate(prefix):
        if character == "<" and not is_operator_angle(prefix, index):
            angle_depth += 1
        elif character == ">" and angle_depth > 0:
            angle_depth -= 1
        elif character.isspace() and angle_depth == 0:
            last_separator = index
    return prefix[last_separator + 1 :]


def top_level_module_attachment(declarator: str) -> str | None:
    """Return the last module attachment outside template arguments."""

    angle_depth = 0
    owner: str | None = None
    for index, character in enumerate(declarator):
        if character == "<" and not is_operator_angle(declarator, index):
            angle_depth += 1
        elif character == ">" and angle_depth > 0:
            angle_depth -= 1
        elif character == "@" and angle_depth == 0:
            end = index + 1
            while end < len(declarator) and (
                declarator[end].isalnum() or declarator[end] in "._"
            ):
                end += 1
            module = declarator[index + 1 : end]
            if module and (
                end == len(declarator) or declarator[end] in "<:"
            ):
                owner = module
    return owner


def symbol_owner_module(symbol: str) -> str | None:
    """Return the module attached to the symbol's actual declared entity."""

    prefix = symbol[: function_parameter_open(symbol)].rstrip()
    qualified_operator = prefix.rfind("::operator")
    if qualified_operator != -1:
        # Conversion-operator target types may carry their own attachments.
        # A module attached to the qualified class owns the member operator;
        # namespace-qualified free operators instead fall through to the
        # attachment on the operator name itself.
        qualified_entity = actual_entity_declarator(
            prefix[:qualified_operator]
        )
        owner = top_level_module_attachment(qualified_entity)
        if owner is not None:
            return owner

    return top_level_module_attachment(actual_entity_declarator(symbol))


def format_symbols(symbols: set[tuple[str, str]]) -> str:
    return "\n".join(f"  {kind} {name}" for kind, name in sorted(symbols))


def require_expected_symbols(
    module_name: str,
    label: str,
    symbols: set[tuple[str, str]],
) -> None:
    expected = set(ABI_SPECS[module_name].symbols)
    if symbols == expected:
        return
    missing = expected - symbols
    unexpected = symbols - expected
    details = [
        f"{label} does not define the exact {len(expected)}-symbol "
        f"{module_name} ABI"
    ]
    if missing:
        details.append("missing:\n" + format_symbols(missing))
    if unexpected:
        details.append("unexpected:\n" + format_symbols(unexpected))
    raise GateError("\n".join(details))


def importer_source() -> str:
    return """\
#include <rusty/function.hpp>
#include <rusty/cell.hpp>
#include <rusty/io.hpp>
#include <rusty/move.hpp>
#include <rusty/option.hpp>
#include <rusty/refcell.hpp>
#include <rusty/slice.hpp>
#include <rusty/sync/atomic.hpp>
#include <rusty/traits.hpp>

#include <atomic>
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <netdb.h>
#include <span>
#include <sstream>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

import srpc.callback_wrapper;
import srpc.basetypes;
import srpc.debugging;
import srpc.circuit_breaker;
import srpc.completion_tracker;
import srpc.connection_metrics;
import srpc.connection_state;
import srpc.errors;
import srpc.frame_codec;
import srpc.heartbeat;
import srpc.internal_protocol;
import srpc.load_balancer;
import srpc.rand;
import srpc.reconnect_policy;
import srpc.request_options;
import srpc.request_queue;
import srpc.stat;
import srpc.utils;

static std::int32_t rand_raw_value = 0;
static std::uint32_t rand_raw_draws = 0;
static std::uint32_t rand_destroy_calls = 0;
static std::uint32_t rand_string_evaluations = 0;
static std::uint32_t rand_weight_evaluations = 0;
static std::uint64_t monotonic_now_us = 0;
static std::uint64_t realtime_now_us = 0;
static std::uint64_t gettimeofday_now_us = 0;
static std::uint64_t slept_us = 0;
static std::int32_t selected_open_port = 0;
// srpc.logging is canonical Rust now, so the gate no longer substitutes a
// forwarding fixture for it: Utils logs through the REAL provider and this
// observer reads what that provider actually emitted. `log_line` renders
//   "<tag>[<basename>:<line>] <23-char timestamp> | <message>\\n"
// and writes it to std::cout, so swapping cout's streambuf captures the whole
// decorated line. Every fact the old fixture asserted (level, line, file,
// message) is still asserted -- through the rendering rather than around it.
static std::ostringstream utils_log_sink;
static std::streambuf* utils_log_original = nullptr;
static std::uint32_t freeaddrinfo_calls = 0;
static std::int32_t hostname_mode = 0;
static std::size_t hostname_buffer_length = 0;

extern "C" int srpc_rand_raw(void) {
    ++rand_raw_draws;
    return rand_raw_value;
}

extern "C" void srpc_rand_destroy(void) {
    ++rand_destroy_calls;
}

extern "C" std::uint64_t srpc_clock_monotonic_us(void) {
    return monotonic_now_us;
}

extern "C" std::uint64_t srpc_clock_realtime_coarse_us(void) {
    return realtime_now_us;
}

extern "C" std::uint64_t srpc_gettimeofday_us(void) {
    return gettimeofday_now_us;
}

extern "C" void srpc_sleep_us(std::uint64_t microseconds) {
    slept_us = microseconds;
}

extern "C" int srpc_find_open_port(void) {
    return selected_open_port;
}

// Remaining C seams the canonical modules call. They are stubbed here for the
// same reason as the ones above: the generated lane must be deterministic, and
// linking the real object files would also pull in the neighbours that the
// stubs above deliberately replace.
extern "C" const char* srpc_path_basename(const char* path) {
    if (path == nullptr) {
        return nullptr;
    }
    const char* slash = std::strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

extern "C" void srpc_time_now_str(char* now) {
    // Exactly the 23 characters srpc.logging renders, so the decorated line the
    // Utils assertions match has a fixed width.
    std::memcpy(now, "2026-01-01 00:00:00.000", 24);
}

extern "C" std::int32_t srpc_get_ncpu(void) {
    return 8;
}

extern "C" std::int32_t srpc_format_fixed_2(
    double value, std::int8_t* output, std::size_t capacity) {
    return std::snprintf(
        reinterpret_cast<char*>(output), capacity, "%.2f", value);
}

extern "C" void srpc_cpu_pause(void) {}

// srpc.debugging reaches libc through exactly three plain-C seams
// (base/srpc_base.c in production). Model them here so the generated lane
// links and so the rendered report is observable: the capture stub hands back
// a fixed symbol array, and srpc_stderr hands back an in-memory FILE the
// importer can read.
static char debugging_report[512] = {};
static FILE* debugging_stream = nullptr;
static std::int32_t debugging_capture_frames = -1;
static std::uint32_t debugging_capture_calls = 0;
static std::uint32_t debugging_free_calls = 0;
static char* debugging_symbols[3] = {};
static char debugging_symbol_storage[3][16] = {
    "frame-zero", "frame-one", "frame-two"};

extern "C" FILE* srpc_stderr(void) {
    return debugging_stream;
}

extern "C" int srpc_backtrace_capture(char*** out_syms) {
    ++debugging_capture_calls;
    if (out_syms == nullptr) {
        return -1;
    }
    *out_syms = nullptr;
    if (debugging_capture_frames < 0) {
        return -1;
    }
    for (std::size_t index = 0; index < 3; ++index) {
        debugging_symbols[index] = debugging_symbol_storage[index];
    }
    *out_syms = debugging_symbols;
    return debugging_capture_frames;
}

extern "C" void srpc_backtrace_free(char**) {
    ++debugging_free_calls;
}

static void reset_debugging(std::int32_t frames) {
    debugging_capture_frames = frames;
    debugging_capture_calls = 0;
    debugging_free_calls = 0;
    std::memset(debugging_report, 0, sizeof(debugging_report));
    if (debugging_stream != nullptr) {
        std::fclose(debugging_stream);
    }
    debugging_stream =
        fmemopen(debugging_report, sizeof(debugging_report) - 1, "w");
}

static std::string_view debugging_rendered() {
    if (debugging_stream != nullptr) {
        std::fflush(debugging_stream);
    }
    return std::string_view(debugging_report);
}

extern "C" void freeaddrinfo(addrinfo* info) {
    ++freeaddrinfo_calls;
    delete info;
}

extern "C" int gethostname(char* name, std::size_t length) {
    hostname_buffer_length = length;
    if (hostname_mode < 0) {
        return -1;
    }
    const char fixed[] = "goal0-host";
    if (length != 0) {
        std::strncpy(name, fixed, length);
        name[length - 1] = '\0';
    }
    return 0;
}

static void reset_utils_log() {
    if (utils_log_original == nullptr) {
        utils_log_original = std::cout.rdbuf(utils_log_sink.rdbuf());
    }
    utils_log_sink.str(std::string());
}

static std::string utils_log_text() {
    std::cout.flush();
    return utils_log_sink.str();
}

// The timestamp is the only part of the rendered line the gate cannot pin, so
// match around it: exact prefix, exact suffix, exactly one line.
static bool utils_logged(
    std::string_view prefix, std::string_view suffix) {
    const std::string text = utils_log_text();
    return text.size() > prefix.size() + suffix.size() &&
           std::string_view(text).substr(0, prefix.size()) == prefix &&
           std::string_view(text).substr(text.size() - suffix.size()) ==
               suffix &&
           std::count(text.begin(), text.end(), '\\n') == 1;
}

static void install_rand_raw(std::int32_t value) {
    rand_raw_value = value;
    rand_raw_draws = 0;
}

static std::string make_rand_binary_string() {
    ++rand_string_evaluations;
    return std::string({
        static_cast<char>(0x00),
        static_cast<char>(0x80),
        static_cast<char>(0xff),
    });
}

static std::vector<double> make_rand_weights() {
    ++rand_weight_evaluations;
    return {1.0, 2.0, 3.0};
}

struct LoadBalancerProbeMetrics {
    std::uint64_t pending;
    std::uint64_t latency;
    std::uint64_t completed;

    std::uint64_t in_flight_requests() const { return pending; }
    std::uint64_t avg_latency_us() const { return latency; }
    std::uint64_t requests_completed() const { return completed; }
};

struct LoadBalancerProbeClient {
    LoadBalancerProbeMetrics metrics_value;

    const LoadBalancerProbeMetrics& metrics() const { return metrics_value; }
};

using LoadBalancerProbeClients =
    std::vector<std::shared_ptr<LoadBalancerProbeClient>>;

static_assert(std::is_same_v<srpc::RandWeightVec, std::vector<double>>);

static_assert(std::is_same_v<
              srpc::FrameCursor,
              rusty::io::Cursor<std::vector<std::uint8_t>>>);
static_assert(std::is_same_v<
              std::underlying_type_t<srpc::FrameDecodeStatus>,
              std::int32_t>);
static_assert(sizeof(srpc::FrameDecodeStatus) == 4);
static_assert(alignof(srpc::FrameDecodeStatus) == 4);
static_assert(srpc::kFrameHeaderSize == 4);
static_assert(srpc::kMaxFramePayloadSize == 64 * 1024 * 1024);
static_assert(std::is_standard_layout_v<srpc::FrameHeader>);
static_assert(std::is_trivially_copyable_v<srpc::FrameHeader>);
static_assert(srpc::FrameHeader::is_send && srpc::FrameHeader::is_sync);
static_assert(sizeof(srpc::FrameHeader) == 8);
static_assert(alignof(srpc::FrameHeader) == 4);
static_assert(offsetof(srpc::FrameHeader, payload_size) == 0);
static_assert(offsetof(srpc::FrameHeader, extended_header_flag) == 4);
static_assert(sizeof(srpc::FrameView) == 24);
static_assert(alignof(srpc::FrameView) == 8);
static_assert(offsetof(srpc::FrameView, header) == 0);
static_assert(offsetof(srpc::FrameView, payload) == 8);
static_assert(offsetof(srpc::FrameView, payload_size) == 16);
static_assert(sizeof(srpc::FrameCursor) == 32);
static_assert(alignof(srpc::FrameCursor) == 8);
static_assert(sizeof(srpc::FrameStreamReader) == 40);
static_assert(alignof(srpc::FrameStreamReader) == 8);
static_assert(offsetof(srpc::FrameStreamReader, cursor_) == 0);
static_assert(offsetof(srpc::FrameStreamReader, noncopy_) == 32);
static_assert(!std::is_default_constructible_v<srpc::FrameStreamReader>);
static_assert(!std::is_copy_constructible_v<srpc::FrameStreamReader>);
static_assert(!std::is_copy_assignable_v<srpc::FrameStreamReader>);
static_assert(std::is_move_constructible_v<srpc::FrameStreamReader>);
static_assert(std::is_move_assignable_v<srpc::FrameStreamReader>);
static_assert(std::is_same_v<
              decltype(&srpc::frame_decode_status_to_string),
              std::string_view (*)(srpc::FrameDecodeStatus)>);
static_assert(std::is_same_v<
              decltype(&srpc::frame_codec_write_header),
              bool (*)(std::span<std::uint8_t>, std::int32_t, bool)>);
static_assert(std::is_same_v<
              decltype(&srpc::frame_codec_peek_header),
              srpc::FrameDecodeStatus (*)(
                  std::span<const std::uint8_t>, srpc::FrameHeader&)>);
static_assert(std::is_same_v<
              decltype(&srpc::frame_codec_encode_into),
              bool (*)(std::vector<std::uint8_t>&, const std::uint8_t*,
                       std::int32_t, bool)>);
static_assert(std::is_same_v<
              decltype(&srpc::FrameHeader::total_frame_size),
              std::int32_t (srpc::FrameHeader::*)() const>);
static_assert(std::is_same_v<
              decltype(&srpc::FrameStreamReader::append),
              void (srpc::FrameStreamReader::*)(
                  const std::uint8_t*, std::size_t)>);
static_assert(std::is_same_v<
              decltype(&srpc::FrameStreamReader::next_frame),
              srpc::FrameDecodeStatus (srpc::FrameStreamReader::*)(
                  srpc::FrameView&) const>);
static_assert(std::is_same_v<
              decltype(&srpc::FrameStreamReader::buffered_bytes),
              std::size_t (srpc::FrameStreamReader::*)() const>);

static_assert(std::is_same_v<srpc::i8, std::int8_t>);
static_assert(std::is_same_v<srpc::i16, std::int16_t>);
static_assert(std::is_same_v<srpc::i32, std::int32_t>);
static_assert(std::is_same_v<srpc::i64, std::int64_t>);
static_assert(sizeof(srpc::SparseInt) == 1);
static_assert(alignof(srpc::SparseInt) == 1);
static_assert(sizeof(srpc::v32) == 4);
static_assert(alignof(srpc::v32) == 4);
static_assert(sizeof(srpc::v64) == 8);
static_assert(alignof(srpc::v64) == 8);
static_assert(sizeof(srpc::Counter) == 8);
static_assert(alignof(srpc::Counter) == 8);
static_assert(sizeof(srpc::Time) == 1);
static_assert(alignof(srpc::Time) == 1);
static_assert(sizeof(srpc::Timer) == 16);
static_assert(alignof(srpc::Timer) == 8);
static_assert(offsetof(srpc::v32, val_field) == 0);
static_assert(offsetof(srpc::v64, val_field) == 0);
static_assert(offsetof(srpc::Counter, next_field) == 0);
static_assert(offsetof(srpc::Timer, begin_us) == 0);
static_assert(offsetof(srpc::Timer, end_us) == 8);
static_assert(srpc::SparseInt::is_send && srpc::SparseInt::is_sync);
static_assert(srpc::v32::is_send && srpc::v32::is_sync);
static_assert(srpc::v64::is_send && srpc::v64::is_sync);
static_assert(srpc::Counter::is_send && srpc::Counter::is_sync);
static_assert(srpc::Time::is_send && srpc::Time::is_sync);
static_assert(srpc::Timer::is_send && srpc::Timer::is_sync);
static_assert(sizeof(srpc::AtomicI64) == 8);
static_assert(alignof(srpc::AtomicI64) == 8);
static_assert(std::is_copy_constructible_v<srpc::Counter>);
static_assert(std::is_copy_assignable_v<srpc::Counter>);
static_assert(std::is_move_constructible_v<srpc::Counter>);
static_assert(std::is_move_assignable_v<srpc::Counter>);
static_assert(std::is_same_v<
              decltype(&srpc::SparseInt::buf_size),
              std::size_t (*)(std::uint8_t)>);
static_assert(std::is_same_v<
              decltype(&srpc::SparseInt::dump32),
              std::size_t (*)(std::int32_t, std::uint8_t*)>);
static_assert(std::is_same_v<
              decltype(&srpc::SparseInt::dump64),
              std::size_t (*)(std::int64_t, std::uint8_t*)>);
static_assert(std::is_same_v<
              decltype(&srpc::SparseInt::load32),
              std::int32_t (*)(const std::uint8_t*)>);
static_assert(std::is_same_v<
              decltype(&srpc::SparseInt::load64),
              std::int64_t (*)(const std::uint8_t*)>);
static_assert(std::is_same_v<
              decltype(&srpc::Counter::next),
              std::int64_t (srpc::Counter::*)(std::int64_t) const>);
static_assert(std::is_same_v<
              decltype(&srpc::Timer::elapsed),
              double (srpc::Timer::*)() const>);

static_assert(std::is_same_v<
              std::underlying_type_t<srpc::CircuitState>, std::int32_t>);
static_assert(sizeof(srpc::CircuitState) == 4);
static_assert(alignof(srpc::CircuitState) == 4);
static_assert(std::is_standard_layout_v<srpc::CircuitBreakerConfig>);
static_assert(std::is_trivially_copyable_v<srpc::CircuitBreakerConfig>);
static_assert(srpc::CircuitBreakerConfig::is_send);
static_assert(srpc::CircuitBreakerConfig::is_sync);
static_assert(sizeof(srpc::CircuitBreakerConfig) == 16);
static_assert(alignof(srpc::CircuitBreakerConfig) == 4);
static_assert(offsetof(srpc::CircuitBreakerConfig, failure_threshold) == 0);
static_assert(offsetof(srpc::CircuitBreakerConfig, success_threshold) == 4);
static_assert(offsetof(srpc::CircuitBreakerConfig, timeout_ms) == 8);
static_assert(offsetof(srpc::CircuitBreakerConfig, enabled) == 12);
static_assert(sizeof(srpc::CircuitBreaker) == 48);
static_assert(alignof(srpc::CircuitBreaker) == 8);
static_assert(offsetof(srpc::CircuitBreaker, config_field) == 0);
static_assert(offsetof(srpc::CircuitBreaker, state_field) == 16);
static_assert(offsetof(srpc::CircuitBreaker, failure_count_field) == 20);
static_assert(offsetof(srpc::CircuitBreaker, success_count_field) == 24);
static_assert(offsetof(srpc::CircuitBreaker, last_failure_time) == 32);
static_assert(offsetof(srpc::CircuitBreaker, probe_in_progress) == 40);
static_assert(srpc::CircuitBreaker::is_send);
static_assert(!rusty::is_sync<srpc::CircuitBreaker>::value);
static_assert(std::is_same_v<
              decltype(&srpc::CircuitBreaker::new_),
              srpc::CircuitBreaker (*)(srpc::CircuitBreakerConfig)>);
static_assert(std::is_same_v<
              decltype(&srpc::CircuitBreaker::set_config),
              void (srpc::CircuitBreaker::*)(srpc::CircuitBreakerConfig) const>);
static_assert(std::is_same_v<
              decltype(&srpc::CircuitBreaker::allow_request),
              bool (srpc::CircuitBreaker::*)() const>);
static_assert(std::is_same_v<
              decltype(&srpc::current_time_us), std::uint64_t (*)()>);

static_assert(std::is_same_v<
              std::underlying_type_t<srpc::OverflowStrategy>, std::int32_t>);
static_assert(sizeof(srpc::OverflowStrategy) == 4);
static_assert(alignof(srpc::OverflowStrategy) == 4);
static_assert(std::is_same_v<
              srpc::QueuedRequestCallback,
              rusty::Function<void(std::int32_t)>>);
static_assert(std::is_same_v<
              decltype(&srpc::rq_invoke_callback_safely),
              void (*)(srpc::QueuedRequestCallback, std::int32_t)>);
static_assert(sizeof(srpc::QueuedRequestCallback) == 48);
static_assert(alignof(srpc::QueuedRequestCallback) == 16);
static_assert(sizeof(srpc::QueuedRequest) == 96);
static_assert(alignof(srpc::QueuedRequest) == 16);
static_assert(offsetof(srpc::QueuedRequest, xid) == 0);
static_assert(offsetof(srpc::QueuedRequest, rpc_id) == 8);
static_assert(offsetof(srpc::QueuedRequest, timestamp_us) == 16);
static_assert(offsetof(srpc::QueuedRequest, retry_count) == 24);
static_assert(offsetof(srpc::QueuedRequest, callback) == 32);
static_assert(offsetof(srpc::QueuedRequest, ttl_ms) == 80);
static_assert(!rusty::is_send<srpc::QueuedRequest>::value);
static_assert(!rusty::is_sync<srpc::QueuedRequest>::value);
static_assert(std::is_standard_layout_v<srpc::RequestQueueConfig>);
static_assert(std::is_trivially_copyable_v<srpc::RequestQueueConfig>);
static_assert(srpc::RequestQueueConfig::is_send);
static_assert(srpc::RequestQueueConfig::is_sync);
static_assert(sizeof(srpc::RequestQueueConfig) == 24);
static_assert(alignof(srpc::RequestQueueConfig) == 8);
static_assert(offsetof(srpc::RequestQueueConfig, max_size) == 0);
static_assert(offsetof(srpc::RequestQueueConfig, default_ttl_ms) == 8);
static_assert(offsetof(srpc::RequestQueueConfig, overflow_strategy) == 12);
static_assert(offsetof(srpc::RequestQueueConfig, enabled) == 16);
static_assert(sizeof(srpc::RequestQueue) == 96);
static_assert(alignof(srpc::RequestQueue) == 8);
static_assert(offsetof(srpc::RequestQueue, config_) == 0);
static_assert(offsetof(srpc::RequestQueue, queue_) == 24);
static_assert(!rusty::is_send<srpc::RequestQueue>::value);
static_assert(!rusty::is_sync<srpc::RequestQueue>::value);
static_assert(std::is_same_v<
              decltype(&srpc::RequestQueue::enqueue),
              bool (srpc::RequestQueue::*)(srpc::QueuedRequest) const>);
static_assert(std::is_same_v<
              decltype(&srpc::RequestQueue::dequeue),
              rusty::Option<srpc::QueuedRequest> (srpc::RequestQueue::*)()>);
static_assert(std::is_same_v<
              decltype(&srpc::RequestQueue::expire_stale),
              std::size_t (srpc::RequestQueue::*)() const>);
static_assert(std::is_same_v<
              decltype(&srpc::RequestQueue::full),
              bool (srpc::RequestQueue::*)()>);
static_assert(std::is_same_v<
              decltype(&srpc::RequestQueue::remaining_capacity),
              std::size_t (srpc::RequestQueue::*)()>);
static_assert(std::is_same_v<
              decltype(&srpc::RequestQueue::clear_all),
              void (srpc::RequestQueue::*)(std::int32_t) const>);
static_assert(std::is_same_v<
              decltype(&srpc::RequestQueue::update_config),
              void (srpc::RequestQueue::*)(srpc::RequestQueueConfig) const>);
static_assert(std::is_same_v<
              decltype(&srpc::randgen_zero_pad),
              std::string (*)(std::string, std::int32_t)>);
static_assert(std::is_same_v<
              decltype(&srpc::RandomGenerator::int2str_n),
              std::string (*)(std::int32_t, std::int32_t)>);
static_assert(std::is_same_v<
              decltype(&srpc::RandomGenerator::weighted_select),
              std::uint32_t (*)(const std::vector<double>&)>);

static_assert(std::is_same_v<
              std::underlying_type_t<srpc::ConnectionState>, std::int32_t>);
static_assert(sizeof(srpc::ConnectionState) == 4);
static_assert(alignof(srpc::ConnectionState) == 4);
static_assert(std::is_same_v<
              srpc::StateChangeCallback,
              rusty::Function<void(srpc::ConnectionState,
                                   srpc::ConnectionState) const>>);
static_assert(sizeof(srpc::StateChangeCallback) == 48);
static_assert(alignof(srpc::StateChangeCallback) == 16);
static_assert(sizeof(srpc::ConnectionStateMachine) == 64);
static_assert(alignof(srpc::ConnectionStateMachine) == 16);
static_assert(offsetof(srpc::ConnectionStateMachine, state_field) == 0);
static_assert(offsetof(srpc::ConnectionStateMachine, on_state_change) == 16);
static_assert(!std::is_copy_constructible_v<srpc::ConnectionStateMachine>);
static_assert(std::is_move_constructible_v<srpc::ConnectionStateMachine>);
static_assert(!rusty::is_send<srpc::StateChangeCallback>::value);
static_assert(!rusty::is_sync<srpc::StateChangeCallback>::value);
static_assert(!rusty::is_send<srpc::ConnectionStateMachine>::value);
static_assert(!rusty::is_sync<srpc::ConnectionStateMachine>::value);
static_assert(std::is_same_v<
              decltype(&srpc::ConnectionStateMachine::set_on_state_change),
              void (srpc::ConnectionStateMachine::*)(srpc::StateChangeCallback)>);
static_assert(std::is_same_v<
              decltype(&srpc::ConnectionStateMachine::transition_to),
              bool (srpc::ConnectionStateMachine::*)(srpc::ConnectionState) const>);

static_assert(std::is_same_v<
              srpc::HeartbeatTimeoutCallback,
              rusty::Function<void()>>);
static_assert(sizeof(srpc::HeartbeatTimeoutCallback) == 48);
static_assert(alignof(srpc::HeartbeatTimeoutCallback) == 16);
static_assert(std::is_standard_layout_v<srpc::HeartbeatConfig>);
static_assert(std::is_trivially_copyable_v<srpc::HeartbeatConfig>);
static_assert(srpc::HeartbeatConfig::is_send);
static_assert(srpc::HeartbeatConfig::is_sync);
static_assert(sizeof(srpc::HeartbeatConfig) == 16);
static_assert(alignof(srpc::HeartbeatConfig) == 4);
static_assert(offsetof(srpc::HeartbeatConfig, enabled) == 0);
static_assert(offsetof(srpc::HeartbeatConfig, interval_ms) == 4);
static_assert(offsetof(srpc::HeartbeatConfig, timeout_ms) == 8);
static_assert(offsetof(srpc::HeartbeatConfig, max_missed) == 12);
static_assert(sizeof(srpc::HeartbeatManager) == 112);
static_assert(alignof(srpc::HeartbeatManager) == 16);
static_assert(offsetof(srpc::HeartbeatManager, config_field) == 0);
static_assert(offsetof(srpc::HeartbeatManager, last_send_time) == 16);
static_assert(offsetof(srpc::HeartbeatManager, last_recv_time) == 24);
static_assert(offsetof(srpc::HeartbeatManager, missed_count_field) == 32);
static_assert(offsetof(srpc::HeartbeatManager, pending_pong) == 36);
static_assert(offsetof(srpc::HeartbeatManager, timed_out) == 37);
static_assert(offsetof(srpc::HeartbeatManager, on_timeout) == 48);
static_assert(!std::is_copy_constructible_v<srpc::HeartbeatManager>);
static_assert(std::is_move_constructible_v<srpc::HeartbeatManager>);
static_assert(!rusty::is_send<srpc::HeartbeatTimeoutCallback>::value);
static_assert(!rusty::is_sync<srpc::HeartbeatTimeoutCallback>::value);
static_assert(!rusty::is_send<srpc::HeartbeatManager>::value);
static_assert(!rusty::is_sync<srpc::HeartbeatManager>::value);
static_assert(std::is_same_v<
              decltype(&srpc::HeartbeatManager::new_),
              srpc::HeartbeatManager (*)(const srpc::HeartbeatConfig&)>);
static_assert(std::is_same_v<
              decltype(&srpc::HeartbeatManager::set_on_timeout),
              void (srpc::HeartbeatManager::*)(srpc::HeartbeatTimeoutCallback) const>);
static_assert(std::is_same_v<
              decltype(&srpc::HeartbeatManager::check_timeout),
              bool (srpc::HeartbeatManager::*)() const>);
static_assert(std::is_same_v<
              decltype(&srpc::heartbeat_time_us), std::uint64_t (*)()>);
static_assert(std::is_same_v<
              std::underlying_type_t<srpc::LoadBalancingStrategy>,
              std::int32_t>);
static_assert(sizeof(srpc::LoadBalancingStrategy) == 4);
static_assert(alignof(srpc::LoadBalancingStrategy) == 4);
static_assert(sizeof(srpc::LoadBalancerState) == 8);
static_assert(alignof(srpc::LoadBalancerState) == 8);
static_assert(offsetof(srpc::LoadBalancerState, round_robin_index_field) == 0);
static_assert(std::is_standard_layout_v<srpc::LoadBalancerState>);
static_assert(srpc::LoadBalancerState::is_send);
static_assert(!rusty::is_sync<srpc::LoadBalancerState>::value);
static_assert(sizeof(srpc::LoadBalancer) == 1);
static_assert(std::is_empty_v<srpc::LoadBalancer>);
static_assert(srpc::LoadBalancer::is_send && srpc::LoadBalancer::is_sync);
static_assert(std::is_same_v<
              decltype(&srpc::LoadBalancer::select_random),
              std::size_t (*)(std::size_t, std::size_t)>);
static_assert(std::is_same_v<
              decltype(&srpc::LoadBalancer::select_round_robin),
              std::size_t (*)(std::size_t,
                              const srpc::LoadBalancerState&)>);
static_assert(std::is_same_v<
              decltype(&srpc::load_balancing_strategy_to_string),
              std::string_view (*)(srpc::LoadBalancingStrategy)>);
static_assert(sizeof(srpc::AddrInfo) == 16);
static_assert(alignof(srpc::AddrInfo) == 8);
static_assert(offsetof(srpc::AddrInfo, info_) == 0);
static_assert(offsetof(srpc::AddrInfo, owned_) == 8);
static_assert(offsetof(srpc::AddrInfo, _rusty_forgotten) == 9);
static_assert(std::is_standard_layout_v<srpc::AddrInfo>);
static_assert(!std::is_copy_constructible_v<srpc::AddrInfo>);
static_assert(!std::is_copy_assignable_v<srpc::AddrInfo>);
static_assert(std::is_move_constructible_v<srpc::AddrInfo>);
static_assert(std::is_move_assignable_v<srpc::AddrInfo>);
// The move constructor itself is noexcept (pinned in the generated surface),
// but is_nothrow_constructible also accounts for the legacy noexcept(false)
// destructor, so the aggregate trait is deliberately false.
static_assert(!std::is_nothrow_move_constructible_v<srpc::AddrInfo>);
static_assert(std::is_nothrow_move_assignable_v<srpc::AddrInfo>);
static_assert(!std::is_nothrow_destructible_v<srpc::AddrInfo>);
static_assert(std::is_same_v<
              decltype(&srpc::AddrInfo::get),
              addrinfo* (srpc::AddrInfo::*)() const>);
static_assert(std::is_same_v<
              decltype(&srpc::AddrInfo::valid),
              bool (srpc::AddrInfo::*)() const>);
static_assert(std::is_same_v<
              decltype(&srpc::find_open_port), std::int32_t (*)()>);
static_assert(std::is_same_v<
              decltype(&srpc::get_host_name), std::string (*)()>);
static_assert(std::is_same_v<
              decltype(&srpc::randgen_zero_pad),
              std::string (*)(std::string, std::int32_t)>);
static_assert(std::is_same_v<
              decltype(&srpc::RandomGenerator::int2str_n),
              std::string (*)(std::int32_t, std::int32_t)>);
static_assert(std::is_same_v<
              decltype(&srpc::RandomGenerator::weighted_select),
              std::uint32_t (*)(const std::vector<double>&)>);

static_assert(std::is_same_v<
              std::underlying_type_t<srpc::TimeoutType>, std::int32_t>);
static_assert(sizeof(srpc::TimeoutType) == 4);
static_assert(alignof(srpc::TimeoutType) == 4);
static_assert(std::is_trivially_copyable_v<srpc::TimeoutType>);
static_assert(std::is_standard_layout_v<srpc::RequestOptions>);
static_assert(std::is_trivially_copyable_v<srpc::RequestOptions>);
static_assert(srpc::RequestOptions::is_send);
static_assert(srpc::RequestOptions::is_sync);
static_assert(sizeof(srpc::RequestOptions) == 32);
static_assert(alignof(srpc::RequestOptions) == 8);
static_assert(offsetof(srpc::RequestOptions, timeout_ms) == 0);
static_assert(offsetof(srpc::RequestOptions, total_timeout_ms) == 8);
static_assert(offsetof(srpc::RequestOptions, max_retries) == 16);
static_assert(offsetof(srpc::RequestOptions, base_delay_ms) == 18);
static_assert(offsetof(srpc::RequestOptions, max_delay_ms) == 20);
static_assert(offsetof(srpc::RequestOptions, jitter_factor) == 24);
static_assert(offsetof(srpc::RequestOptions, idempotent) == 28);
static_assert(std::is_same_v<
              decltype(&srpc::RequestOptions::new_),
              srpc::RequestOptions (*)()>);
static_assert(std::is_same_v<
              decltype(&srpc::RequestOptions::with_retry),
              srpc::RequestOptions (*)(std::uint16_t, std::uint64_t)>);
static_assert(std::is_same_v<
              decltype(&srpc::RequestOptions::can_retry),
              bool (srpc::RequestOptions::*)(std::uint16_t) const>);
static_assert(std::is_same_v<
              decltype(&srpc::RequestOptions::calculate_delay_ms),
              std::uint64_t (srpc::RequestOptions::*)(std::uint16_t) const>);
static_assert(std::is_same_v<
              decltype(&srpc::RequestOptions::is_total_timeout_exceeded),
              bool (srpc::RequestOptions::*)(std::uint64_t) const>);
static_assert(std::is_same_v<
              decltype(&srpc::RequestOptions::remaining_time_ms),
              std::uint64_t (srpc::RequestOptions::*)(std::uint64_t) const>);
static_assert(std::is_same_v<
              decltype(&srpc::timeout_type_to_string),
              std::string_view (*)(srpc::TimeoutType)>);

static_assert(std::is_standard_layout_v<srpc::ReconnectPolicy>);
static_assert(std::is_trivially_copyable_v<srpc::ReconnectPolicy>);
static_assert(srpc::ReconnectPolicy::is_send);
static_assert(srpc::ReconnectPolicy::is_sync);
static_assert(sizeof(srpc::ReconnectPolicy) == 32);
static_assert(alignof(srpc::ReconnectPolicy) == 8);
static_assert(offsetof(srpc::ReconnectPolicy, auto_reconnect) == 0);
static_assert(offsetof(srpc::ReconnectPolicy, max_retries) == 4);
static_assert(offsetof(srpc::ReconnectPolicy, initial_delay_ms) == 8);
static_assert(offsetof(srpc::ReconnectPolicy, max_delay_ms) == 12);
static_assert(offsetof(srpc::ReconnectPolicy, backoff_multiplier) == 16);
static_assert(offsetof(srpc::ReconnectPolicy, jitter_enabled) == 24);
static_assert(sizeof(srpc::ReconnectCalculator) == 16);
static_assert(alignof(srpc::ReconnectCalculator) == 8);
static_assert(!std::is_copy_constructible_v<srpc::ReconnectCalculator>);
static_assert(std::is_move_constructible_v<srpc::ReconnectCalculator>);
static_assert(std::is_same_v<
              decltype(srpc::ReconnectCalculator::policy),
              const srpc::ReconnectPolicy&>);
static_assert(std::is_same_v<
              decltype(srpc::ReconnectCalculator::retries),
              rusty::Cell<std::uint32_t>>);
static_assert(std::is_same_v<
              decltype(&srpc::ReconnectPolicy::new_),
              srpc::ReconnectPolicy (*)()>);
static_assert(std::is_same_v<
              decltype(&srpc::ReconnectCalculator::new_),
              srpc::ReconnectCalculator (*)(const srpc::ReconnectPolicy&)>);
static_assert(std::is_same_v<
              decltype(&srpc::ReconnectCalculator::should_retry),
              bool (srpc::ReconnectCalculator::*)() const>);
static_assert(std::is_same_v<
              decltype(&srpc::ReconnectCalculator::next_delay_ms),
              std::uint32_t (srpc::ReconnectCalculator::*)() const>);
static_assert(std::is_same_v<
              decltype(&srpc::ReconnectCalculator::peek_delay_ms),
              std::uint32_t (srpc::ReconnectCalculator::*)() const>);
static_assert(std::is_same_v<
              decltype(&srpc::ReconnectCalculator::reset),
              void (srpc::ReconnectCalculator::*)() const>);
static_assert(std::is_same_v<
              decltype(&srpc::ReconnectCalculator::retry_count),
              std::uint32_t (srpc::ReconnectCalculator::*)() const>);
static_assert(std::is_same_v<
              decltype(&srpc::ReconnectCalculator::retries_exhausted),
              bool (srpc::ReconnectCalculator::*)() const>);

static_assert(sizeof(srpc::RpcErrorCategory) == sizeof(std::int32_t));
static_assert(sizeof(srpc::RpcError) == sizeof(std::int32_t));
static_assert(std::is_same_v<
              std::underlying_type_t<srpc::RpcErrorCategory>, std::int32_t>);
static_assert(std::is_same_v<
              std::underlying_type_t<srpc::RpcError>, std::int32_t>);
static_assert(std::is_trivially_copyable_v<srpc::RpcErrorCategory>);
static_assert(std::is_trivially_copyable_v<srpc::RpcError>);

namespace callback_oracle {

template<typename F>
struct CallbackWrapper {
    rusty::Option<rusty::Arc<F>> inner;

    static CallbackWrapper<F> from_callable(F callable) {
        return CallbackWrapper<F>{.inner = rusty::Option<rusty::Arc<F>>(
            rusty::Arc<F>::make(std::move(callable)))};
    }
    bool has_value() const {
        return this->inner.is_some();
    }
    const F& callable() const {
        return rusty::detail::deref_if_pointer_like(
            rusty::detail::deref_if_pointer_like(
                this->inner.as_ref().unwrap()));
    }
    CallbackWrapper<F> clone() const {
        return CallbackWrapper<F>{.inner = rusty::clone(this->inner)};
    }
    static CallbackWrapper<F> default_() {
        return CallbackWrapper<F>{
            .inner = rusty::Option<rusty::Arc<F>>{rusty::None}};
    }
    static constexpr bool is_send =
        rusty::is_send<F>::value && rusty::is_sync<F>::value;
    static constexpr bool is_sync =
        rusty::is_send<F>::value && rusty::is_sync<F>::value;
};

} // namespace callback_oracle

struct CallbackStatefulCallable {
    std::vector<int>* observations;
    mutable int calls = 0;

    void operator()(int) const {
        observations->push_back(++calls);
    }
};

struct CallbackMoveObservedCallable {
    std::shared_ptr<int> moves;

    explicit CallbackMoveObservedCallable(std::shared_ptr<int> count)
        : moves(std::move(count)) {}
    CallbackMoveObservedCallable(const CallbackMoveObservedCallable&) = delete;
    CallbackMoveObservedCallable& operator=(
        const CallbackMoveObservedCallable&) = delete;
    CallbackMoveObservedCallable(CallbackMoveObservedCallable&& other) noexcept
        : moves(std::move(other.moves)) {
        ++*moves;
    }
    CallbackMoveObservedCallable& operator=(
        CallbackMoveObservedCallable&&) = delete;

    void operator()() const {}
};

struct MutableHeartbeatCallable {
    int* calls;

    void operator()() {
        ++*calls;
    }
};

using CallbackFunction = rusty::Function<void(int) const>;
using CallbackActual =
    srpc::detail::CallbackWrapper<CallbackFunction>;
using CallbackOracle =
    callback_oracle::CallbackWrapper<CallbackFunction>;

static_assert(std::is_standard_layout_v<CallbackActual>);
static_assert(std::is_standard_layout_v<CallbackOracle>);
static_assert(sizeof(CallbackActual) == sizeof(CallbackOracle));
static_assert(alignof(CallbackActual) == alignof(CallbackOracle));
static_assert(sizeof(CallbackActual) == 2 * sizeof(void*));
static_assert(alignof(CallbackActual) == alignof(void*));
static_assert(offsetof(CallbackActual, inner) == 0);
static_assert(offsetof(CallbackActual, inner) ==
              offsetof(CallbackOracle, inner));
static_assert(
    std::is_default_constructible_v<CallbackActual> ==
    std::is_default_constructible_v<CallbackOracle>);
static_assert(
    std::is_copy_constructible_v<CallbackActual> ==
    std::is_copy_constructible_v<CallbackOracle>);
static_assert(
    std::is_copy_assignable_v<CallbackActual> ==
    std::is_copy_assignable_v<CallbackOracle>);
static_assert(
    std::is_move_constructible_v<CallbackActual> ==
    std::is_move_constructible_v<CallbackOracle>);
static_assert(
    std::is_move_assignable_v<CallbackActual> ==
    std::is_move_assignable_v<CallbackOracle>);
static_assert(
    std::is_nothrow_move_constructible_v<CallbackActual> ==
    std::is_nothrow_move_constructible_v<CallbackOracle>);
static_assert(
    std::is_nothrow_move_assignable_v<CallbackActual> ==
    std::is_nothrow_move_assignable_v<CallbackOracle>);
static_assert(
    std::is_trivially_destructible_v<CallbackActual> ==
    std::is_trivially_destructible_v<CallbackOracle>);
static_assert(std::is_same_v<
    decltype(CallbackActual::from_callable(
        std::declval<CallbackFunction>())),
    CallbackActual>);
static_assert(std::is_same_v<
    decltype(std::declval<const CallbackActual&>().callable()),
    const CallbackFunction&>);
static_assert(CallbackActual::is_send == CallbackOracle::is_send);
static_assert(CallbackActual::is_sync == CallbackOracle::is_sync);
static_assert(std::is_standard_layout_v<srpc::AvgStat>);
static_assert(std::is_trivially_copyable_v<srpc::AvgStat>);
static_assert(sizeof(srpc::AvgStat) == 5 * sizeof(std::int64_t));
static_assert(alignof(srpc::AvgStat) == alignof(std::int64_t));
static_assert(offsetof(srpc::AvgStat, n_stat_) == 0 * sizeof(std::int64_t));
static_assert(offsetof(srpc::AvgStat, sum_) == 1 * sizeof(std::int64_t));
static_assert(offsetof(srpc::AvgStat, avg_) == 2 * sizeof(std::int64_t));
static_assert(offsetof(srpc::AvgStat, max_) == 3 * sizeof(std::int64_t));
static_assert(offsetof(srpc::AvgStat, min_) == 4 * sizeof(std::int64_t));

using MetricsAtomicU64 = rusty::sync::atomic::AtomicU64;
static_assert(sizeof(MetricsAtomicU64) == sizeof(std::uint64_t));
static_assert(alignof(MetricsAtomicU64) == alignof(std::uint64_t));
static_assert(std::is_standard_layout_v<srpc::ConnectionMetrics>);
static_assert(std::is_copy_constructible_v<srpc::ConnectionMetrics>);
static_assert(std::is_copy_assignable_v<srpc::ConnectionMetrics>);
static_assert(std::is_move_constructible_v<srpc::ConnectionMetrics>);
static_assert(std::is_move_assignable_v<srpc::ConnectionMetrics>);
static_assert(!std::is_trivially_copyable_v<srpc::ConnectionMetrics>);
static_assert(srpc::ConnectionMetrics::is_send);
static_assert(srpc::ConnectionMetrics::is_sync);
static_assert(
    sizeof(srpc::ConnectionMetrics) == 18 * sizeof(std::uint64_t));
static_assert(
    alignof(srpc::ConnectionMetrics) == alignof(std::uint64_t));
static_assert(offsetof(srpc::ConnectionMetrics, requests_sent_field) ==
              0 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, requests_completed_field) ==
              1 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, requests_failed_field) ==
              2 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, requests_timed_out_field) ==
              3 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, in_flight_requests_field) ==
              4 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, bytes_sent_field) ==
              5 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, bytes_received_field) ==
              6 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, reconnect_count_field) ==
              7 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, retry_attempts_field) ==
              8 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, queue_dropped_requests_field) ==
              9 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, circuit_open_rejections_field) ==
              10 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, circuit_open_transitions_field) ==
              11 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, circuit_half_open_transitions_field) ==
              12 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, circuit_closed_transitions_field) ==
              13 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, connect_time_ms_field) ==
              14 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, total_latency_us_field) ==
              15 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, min_latency_us_field) ==
              16 * sizeof(MetricsAtomicU64));
static_assert(offsetof(srpc::ConnectionMetrics, max_latency_us_field) ==
              17 * sizeof(MetricsAtomicU64));

static_assert(std::is_same_v<
              std::underlying_type_t<srpc::CompletionStatus>, std::int32_t>);
static_assert(sizeof(srpc::CompletionStatus) == 4);
static_assert(alignof(srpc::CompletionStatus) == 4);
static_assert(std::is_trivially_copyable_v<srpc::CompletionStatus>);

static_assert(std::is_standard_layout_v<srpc::CompletionTrackerConfig>);
static_assert(std::is_trivially_copyable_v<srpc::CompletionTrackerConfig>);
static_assert(srpc::CompletionTrackerConfig::is_send);
static_assert(srpc::CompletionTrackerConfig::is_sync);
static_assert(sizeof(srpc::CompletionTrackerConfig) == 24);
static_assert(alignof(srpc::CompletionTrackerConfig) == 8);
static_assert(offsetof(srpc::CompletionTrackerConfig, ttl_ms) == 0);
static_assert(offsetof(srpc::CompletionTrackerConfig, max_entries) == 8);
static_assert(offsetof(srpc::CompletionTrackerConfig, enabled) == 16);

static_assert(std::is_standard_layout_v<srpc::CompletedEntry>);
static_assert(std::is_trivially_copyable_v<srpc::CompletedEntry>);
static_assert(srpc::CompletedEntry::is_send);
static_assert(srpc::CompletedEntry::is_sync);
static_assert(sizeof(srpc::CompletedEntry) == 16);
static_assert(alignof(srpc::CompletedEntry) == 8);
static_assert(offsetof(srpc::CompletedEntry, xid) == 0);
static_assert(offsetof(srpc::CompletedEntry, timestamp_ms) == 8);

static_assert(std::is_standard_layout_v<srpc::CompletionQueryResult>);
static_assert(std::is_trivially_copyable_v<srpc::CompletionQueryResult>);
static_assert(srpc::CompletionQueryResult::is_send);
static_assert(srpc::CompletionQueryResult::is_sync);
static_assert(sizeof(srpc::CompletionQueryResult) == 12);
static_assert(alignof(srpc::CompletionQueryResult) == 4);
static_assert(offsetof(srpc::CompletionQueryResult, status) == 0);
static_assert(offsetof(srpc::CompletionQueryResult, error_code) == 4);
static_assert(offsetof(srpc::CompletionQueryResult, has_cached_response) == 8);

static_assert(std::is_standard_layout_v<srpc::CompletionTracker>);
static_assert(srpc::CompletionTracker::is_send);
static_assert(srpc::CompletionTracker::is_sync);
static_assert(sizeof(srpc::CompletionTracker) == 256);
static_assert(alignof(srpc::CompletionTracker) == 8);
static_assert(offsetof(srpc::CompletionTracker, config_) == 0);
static_assert(offsetof(srpc::CompletionTracker, lru_list_) == 64);
static_assert(offsetof(srpc::CompletionTracker, completed_set_) == 136);
static_assert(offsetof(srpc::CompletionTracker, total_tracked_) == 224);
static_assert(offsetof(srpc::CompletionTracker, queries_) == 232);
static_assert(offsetof(srpc::CompletionTracker, query_hits_) == 240);
static_assert(offsetof(srpc::CompletionTracker, evictions_) == 248);
static_assert(std::is_same_v<
              decltype(&srpc::CompletionTracker::mark_completed),
              void (srpc::CompletionTracker::*)(std::int64_t, std::uint64_t)>);
static_assert(std::is_same_v<
              decltype(&srpc::CompletionTracker::is_completed),
              bool (srpc::CompletionTracker::*)(std::int64_t, std::uint64_t)>);
static_assert(std::is_same_v<
              decltype(&srpc::CompletionTracker::set_config),
              void (srpc::CompletionTracker::*)(srpc::CompletionTrackerConfig)>);
static_assert(std::is_same_v<
              decltype(&srpc::CompletionTracker::config),
              srpc::CompletionTrackerConfig (srpc::CompletionTracker::*)() const>);

static bool stat_is(
    const srpc::AvgStat& stat,
    std::int64_t count,
    std::int64_t sum,
    std::int64_t average,
    std::int64_t maximum,
    std::int64_t minimum) {
    return stat.n_stat_ == count && stat.sum_ == sum &&
           stat.avg_ == average && stat.max_ == maximum &&
           stat.min_ == minimum;
}

static bool metrics_are_reset(const srpc::ConnectionMetrics& metrics) {
    using rusty::sync::atomic::Ordering;
    return metrics.requests_sent() == 0 &&
           metrics.requests_completed() == 0 &&
           metrics.requests_failed() == 0 &&
           metrics.requests_timed_out() == 0 &&
           metrics.in_flight_requests() == 0 &&
           metrics.bytes_sent() == 0 &&
           metrics.bytes_received() == 0 &&
           metrics.reconnect_count() == 0 &&
           metrics.retry_attempts() == 0 &&
           metrics.queue_dropped_requests() == 0 &&
           metrics.circuit_open_rejections() == 0 &&
           metrics.circuit_open_transitions() == 0 &&
           metrics.circuit_half_open_transitions() == 0 &&
           metrics.circuit_closed_transitions() == 0 &&
           metrics.connect_time_ms() == 0 &&
           metrics.total_latency_us_field.load(Ordering::Relaxed) == 0 &&
           metrics.min_latency_us_field.load(Ordering::Relaxed) ==
               std::numeric_limits<std::uint64_t>::max() &&
           metrics.min_latency_us() == 0 &&
           metrics.max_latency_us() == 0 &&
           metrics.avg_latency_us() == 0 &&
           metrics.success_rate_percent() == 100;
}

static bool metrics_concurrent_updates_are_atomic() {
    constexpr std::uint64_t kThreads = 8;
    constexpr std::uint64_t kOpsPerThread = 2000;
    constexpr std::uint64_t kRounds = 3;
    constexpr std::uint64_t kUpdates = kThreads * kOpsPerThread;
    constexpr std::uint64_t kLatencyTotal =
        kOpsPerThread * kThreads * (kThreads + 1) / 2;

    for (std::uint64_t round = 0; round < kRounds; ++round) {
        auto metrics = srpc::ConnectionMetrics::new_();
        std::atomic<std::uint64_t> ready{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> workers;
        workers.reserve(kThreads);

        for (std::uint64_t thread_index = 0;
             thread_index < kThreads;
             ++thread_index) {
            workers.emplace_back([&, latency = thread_index + 1] {
                ready.fetch_add(1, std::memory_order_relaxed);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (std::uint64_t operation = 0;
                     operation < kOpsPerThread;
                     ++operation) {
                    metrics.record_request_sent();
                    metrics.record_request_completed_with_latency(latency);
                    metrics.record_request_sent();
                    metrics.record_request_failed();
                    metrics.record_request_sent();
                    metrics.record_request_timeout();
                    metrics.record_request_sent();
                    metrics.record_request_dropped();
                    metrics.record_bytes_sent(3);
                    metrics.record_bytes_received(5);
                    metrics.record_reconnect();
                    metrics.record_retry_attempt();
                    metrics.record_queue_drop();
                    metrics.record_circuit_open_rejection();
                    metrics.record_circuit_open_transition();
                    metrics.record_circuit_half_open_transition();
                    metrics.record_circuit_closed_transition();
                }
            });
        }
        while (ready.load(std::memory_order_acquire) != kThreads) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }

        if (metrics.requests_sent() != 4 * kUpdates ||
            metrics.requests_completed() != kUpdates ||
            metrics.requests_failed() != kUpdates ||
            metrics.requests_timed_out() != kUpdates ||
            metrics.in_flight_requests() != 0 ||
            metrics.bytes_sent() != 3 * kUpdates ||
            metrics.bytes_received() != 5 * kUpdates ||
            metrics.reconnect_count() != kUpdates ||
            metrics.retry_attempts() != kUpdates ||
            metrics.queue_dropped_requests() != kUpdates ||
            metrics.circuit_open_rejections() != kUpdates ||
            metrics.circuit_open_transitions() != kUpdates ||
            metrics.circuit_half_open_transitions() != kUpdates ||
            metrics.circuit_closed_transitions() != kUpdates ||
            metrics.total_latency_us_field.load(
                rusty::sync::atomic::Ordering::Relaxed) != kLatencyTotal ||
            metrics.min_latency_us() != 1 ||
            metrics.max_latency_us() != kThreads ||
            metrics.avg_latency_us() != kLatencyTotal / kUpdates ||
            metrics.success_rate_percent() != 25) {
            return false;
        }
        for (std::uint64_t extra = 0; extra < kThreads; ++extra) {
            metrics.record_request_dropped();
        }
        if (metrics.in_flight_requests() != 0) {
            return false;
        }
    }
    return true;
}

static bool completion_tracker_concurrent_operations_are_safe() {
    constexpr std::uint64_t kThreads = 8;
    constexpr std::uint64_t kOpsPerThread = 500;
    constexpr std::uint64_t kRounds = 3;
    constexpr std::uint64_t kUpdates = kThreads * kOpsPerThread;

    for (std::uint64_t round = 0; round < kRounds; ++round) {
        auto config = srpc::CompletionTrackerConfig::defaults();
        config.ttl_ms = 0;
        config.max_entries = kUpdates + 1;
        auto tracker = srpc::CompletionTracker::with_config(config);
        std::atomic<std::uint64_t> ready{0};
        std::atomic<bool> start{false};
        std::atomic<bool> failed{false};
        std::vector<std::thread> workers;
        workers.reserve(kThreads);

        for (std::uint64_t thread_index = 0;
             thread_index < kThreads;
             ++thread_index) {
            workers.emplace_back([&, thread_index] {
                ready.fetch_add(1, std::memory_order_relaxed);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                for (std::uint64_t operation = 0;
                     operation < kOpsPerThread;
                     ++operation) {
                    const auto xid = static_cast<std::int64_t>(
                        thread_index * kOpsPerThread + operation + 1);
                    tracker.mark_completed(xid, operation);
                    if (!tracker.is_completed(xid, operation)) {
                        failed.store(true, std::memory_order_relaxed);
                    }
                }
            });
        }
        while (ready.load(std::memory_order_acquire) != kThreads) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }

        if (failed.load(std::memory_order_relaxed) ||
            tracker.size() != kUpdates ||
            tracker.total_tracked() != kUpdates ||
            tracker.queries() != kUpdates ||
            tracker.query_hits() != kUpdates ||
            tracker.evictions() != 0 || tracker.hit_rate() != 1.0) {
            return false;
        }
    }
    return true;
}

template <typename I>
static bool basetypes_round_trip(I value) {
    std::array<std::uint8_t, 12> encoded{};
    const auto sentinel = static_cast<std::uint8_t>(value) ^ 0xff;
    encoded.fill(sentinel);
    std::size_t size = 0;
    I decoded{};
    if constexpr (sizeof(I) == 4) {
        size = srpc::SparseInt::dump32(value, encoded.data());
        decoded = srpc::SparseInt::load32(encoded.data());
    } else {
        size = srpc::SparseInt::dump64(value, encoded.data());
        decoded = srpc::SparseInt::load64(encoded.data());
    }
    if (size != srpc::SparseInt::val_size(static_cast<std::int64_t>(value)) ||
        srpc::SparseInt::buf_size(encoded[0]) != size || decoded != value) {
        return false;
    }
    if constexpr (sizeof(I) == 8) {
        if (size == 8) {
            return encoded[8] != sentinel && encoded[9] == sentinel;
        }
    }
    return encoded[size] == sentinel;
}

static srpc::QueuedRequest make_queued_request(
    std::int64_t xid,
    srpc::QueuedRequestCallback callback = {}) {
    auto request = srpc::QueuedRequest::new_();
    request.xid = xid;
    request.callback = std::move(callback);
    return request;
}

int main() {
    constexpr int kMin = (-2147483647 - 1);
    if (srpc::kInternalHeartbeatRpcId != kMin) {
        return 1;
    }
    if (srpc::kResponseHeaderExtFlag != 0x80000000u ||
        srpc::kResponseSizeMask != 0x7fffffffu) {
        return 2;
    }
    struct Row {
        int input;
        bool has_extended;
        int payload;
        int plain;
        int extended;
    };
    constexpr Row rows[] = {
        {0, false, 0, 0, kMin},
        {1, false, 1, 1, kMin + 1},
        {2147483647, false, 2147483647, 2147483647, -1},
        {kMin, true, 0, 0, kMin},
        {kMin + 1, true, 1, 1, kMin + 1},
        {-1, true, 2147483647, 2147483647, -1},
    };
    for (const auto& row : rows) {
        if (srpc::response_has_extended_header(row.input) != row.has_extended) {
            return 3;
        }
        if (srpc::response_payload_size(row.input) != row.payload) {
            return 4;
        }
        if (srpc::encode_response_size(row.input, false) != row.plain) {
            return 5;
        }
        if (srpc::encode_response_size(row.input, true) != row.extended) {
            return 6;
        }
    }

    auto stat = srpc::AvgStat::new_();
    if (!stat_is(stat, 0, 0, 0, 0, 0) || stat.avg() != 0) {
        return 10;
    }
    stat.sample(3);
    stat.sample(-5);
    stat.sample(8);
    if (!stat_is(stat, 3, 6, 2, 8, -5) || stat.avg() != 2) {
        return 11;
    }
    const auto peeked = stat.peek();
    if (!stat_is(peeked, 3, 6, 2, 8, -5) ||
        !stat_is(stat, 3, 6, 2, 8, -5)) {
        return 12;
    }
    const auto reset = stat.reset();
    if (!stat_is(reset, 3, 6, 2, 8, -5) ||
        !stat_is(stat, 0, 0, 0, 0, 0)) {
        return 13;
    }
    stat.sample(-7);
    stat.sample(-2);
    if (!stat_is(stat, 2, -9, -4, 0, -7) || stat.avg() != -4) {
        return 14;
    }
    stat.clear();
    if (!stat_is(stat, 0, 0, 0, 0, 0)) {
        return 15;
    }

    struct CategoryRow {
        srpc::RpcErrorCategory category;
        int discriminant;
        std::string_view name;
    };
    constexpr CategoryRow categories[] = {
        {srpc::RpcErrorCategory::NONE, 0, "NONE"},
        {srpc::RpcErrorCategory::CONNECTION, 1, "CONNECTION"},
        {srpc::RpcErrorCategory::PROTOCOL, 2, "PROTOCOL"},
        {srpc::RpcErrorCategory::APPLICATION, 3, "APPLICATION"},
        {srpc::RpcErrorCategory::TIMEOUT, 4, "TIMEOUT"},
        {srpc::RpcErrorCategory::INTERNAL, 5, "INTERNAL"},
    };
    for (const auto& row : categories) {
        if (static_cast<int>(row.category) != row.discriminant ||
            srpc::rpc_error_category_to_string(row.category) != row.name) {
            return 20;
        }
    }
    constexpr int invalid_categories[] = {-1, 6, 999};
    for (const auto value : invalid_categories) {
        if (srpc::rpc_error_category_to_string(
                static_cast<srpc::RpcErrorCategory>(value)) != "UNKNOWN") {
            return 21;
        }
    }

    struct ErrorRow {
        srpc::RpcError error;
        int discriminant;
        std::string_view name;
        srpc::RpcErrorCategory category;
        bool retryable;
    };
    constexpr ErrorRow errors[] = {
        {srpc::RpcError::OK, 0, "OK", srpc::RpcErrorCategory::NONE, false},
        {srpc::RpcError::NOT_CONNECTED, 100, "NOT_CONNECTED", srpc::RpcErrorCategory::CONNECTION, false},
        {srpc::RpcError::CONNECTION_REFUSED, 101, "CONNECTION_REFUSED", srpc::RpcErrorCategory::CONNECTION, false},
        {srpc::RpcError::CONNECTION_RESET, 102, "CONNECTION_RESET", srpc::RpcErrorCategory::CONNECTION, true},
        {srpc::RpcError::NETWORK_UNREACHABLE, 103, "NETWORK_UNREACHABLE", srpc::RpcErrorCategory::CONNECTION, true},
        {srpc::RpcError::HOST_UNREACHABLE, 104, "HOST_UNREACHABLE", srpc::RpcErrorCategory::CONNECTION, true},
        {srpc::RpcError::CONNECTION_CLOSED, 105, "CONNECTION_CLOSED", srpc::RpcErrorCategory::CONNECTION, false},
        {srpc::RpcError::CIRCUIT_OPEN, 106, "CIRCUIT_OPEN", srpc::RpcErrorCategory::CONNECTION, false},
        {srpc::RpcError::INVALID_MESSAGE, 200, "INVALID_MESSAGE", srpc::RpcErrorCategory::PROTOCOL, false},
        {srpc::RpcError::UNKNOWN_RPC_ID, 201, "UNKNOWN_RPC_ID", srpc::RpcErrorCategory::PROTOCOL, false},
        {srpc::RpcError::MARSHALLING_ERROR, 202, "MARSHALLING_ERROR", srpc::RpcErrorCategory::PROTOCOL, false},
        {srpc::RpcError::VERSION_MISMATCH, 203, "VERSION_MISMATCH", srpc::RpcErrorCategory::PROTOCOL, false},
        {srpc::RpcError::CHECKSUM_ERROR, 204, "CHECKSUM_ERROR", srpc::RpcErrorCategory::PROTOCOL, false},
        {srpc::RpcError::RPC_FAILED, 300, "RPC_FAILED", srpc::RpcErrorCategory::APPLICATION, false},
        {srpc::RpcError::SERVICE_UNAVAILABLE, 301, "SERVICE_UNAVAILABLE", srpc::RpcErrorCategory::APPLICATION, true},
        {srpc::RpcError::PERMISSION_DENIED, 302, "PERMISSION_DENIED", srpc::RpcErrorCategory::APPLICATION, false},
        {srpc::RpcError::INVALID_ARGUMENT, 303, "INVALID_ARGUMENT", srpc::RpcErrorCategory::APPLICATION, false},
        {srpc::RpcError::NOT_FOUND, 304, "NOT_FOUND", srpc::RpcErrorCategory::APPLICATION, false},
        {srpc::RpcError::ALREADY_EXISTS, 305, "ALREADY_EXISTS", srpc::RpcErrorCategory::APPLICATION, false},
        {srpc::RpcError::CONNECT_TIMEOUT, 400, "CONNECT_TIMEOUT", srpc::RpcErrorCategory::TIMEOUT, true},
        {srpc::RpcError::REQUEST_TIMEOUT, 401, "REQUEST_TIMEOUT", srpc::RpcErrorCategory::TIMEOUT, true},
        {srpc::RpcError::RESPONSE_TIMEOUT, 402, "RESPONSE_TIMEOUT", srpc::RpcErrorCategory::TIMEOUT, true},
        {srpc::RpcError::IDLE_TIMEOUT, 403, "IDLE_TIMEOUT", srpc::RpcErrorCategory::TIMEOUT, false},
        {srpc::RpcError::HEARTBEAT_TIMEOUT, 404, "HEARTBEAT_TIMEOUT", srpc::RpcErrorCategory::TIMEOUT, false},
        {srpc::RpcError::UNKNOWN_ERROR, 500, "UNKNOWN_ERROR", srpc::RpcErrorCategory::INTERNAL, false},
        {srpc::RpcError::OUT_OF_MEMORY, 501, "OUT_OF_MEMORY", srpc::RpcErrorCategory::INTERNAL, false},
        {srpc::RpcError::INVALID_STATE, 502, "INVALID_STATE", srpc::RpcErrorCategory::INTERNAL, false},
        {srpc::RpcError::INTERNAL_ERROR, 503, "INTERNAL_ERROR", srpc::RpcErrorCategory::INTERNAL, false},
    };
    for (const auto& row : errors) {
        if (static_cast<int>(row.error) != row.discriminant ||
            srpc::rpc_error_to_string(row.error) != row.name ||
            srpc::get_error_category(row.error) != row.category ||
            srpc::is_connection_error(row.error) !=
                (row.category == srpc::RpcErrorCategory::CONNECTION) ||
            srpc::is_timeout_error(row.error) !=
                (row.category == srpc::RpcErrorCategory::TIMEOUT) ||
            srpc::is_retryable_error(row.error) != row.retryable) {
            return 22;
        }
    }

    struct ErrorBoundaryRow {
        int code;
        std::string_view name;
        srpc::RpcErrorCategory category;
        bool connection;
        bool timeout;
        bool retryable;
    };
    constexpr ErrorBoundaryRow boundaries[] = {
        {99, "UNKNOWN", srpc::RpcErrorCategory::INTERNAL, false, false, false},
        {100, "NOT_CONNECTED", srpc::RpcErrorCategory::CONNECTION, true, false, false},
        {199, "UNKNOWN", srpc::RpcErrorCategory::CONNECTION, true, false, false},
        {200, "INVALID_MESSAGE", srpc::RpcErrorCategory::PROTOCOL, false, false, false},
        {399, "UNKNOWN", srpc::RpcErrorCategory::APPLICATION, false, false, false},
        {400, "CONNECT_TIMEOUT", srpc::RpcErrorCategory::TIMEOUT, false, true, true},
        {499, "UNKNOWN", srpc::RpcErrorCategory::TIMEOUT, false, true, false},
        {500, "UNKNOWN_ERROR", srpc::RpcErrorCategory::INTERNAL, false, false, false},
        {999, "UNKNOWN", srpc::RpcErrorCategory::INTERNAL, false, false, false},
    };
    for (const auto& row : boundaries) {
        const auto error = static_cast<srpc::RpcError>(row.code);
        if (srpc::rpc_error_to_string(error) != row.name ||
            srpc::get_error_category(error) != row.category ||
            srpc::is_connection_error(error) != row.connection ||
            srpc::is_timeout_error(error) != row.timeout ||
            srpc::is_retryable_error(error) != row.retryable) {
            return 23;
        }
    }

    auto metrics = srpc::ConnectionMetrics::new_();
    if (!metrics_are_reset(metrics) || metrics.uptime_ms(1234) != 0) {
        return 30;
    }
    metrics.record_request_dropped();
    if (metrics.in_flight_requests() != 0) {
        return 31;
    }
    metrics.record_request_sent();
    metrics.record_request_sent();
    metrics.record_request_sent();
    metrics.record_request_completed_with_latency(30);
    metrics.record_request_completed_with_latency(10);
    if (metrics.requests_sent() != 3 ||
        metrics.requests_completed() != 2 ||
        metrics.in_flight_requests() != 1 ||
        metrics.total_latency_us_field.load(
            rusty::sync::atomic::Ordering::Relaxed) != 40 ||
        metrics.min_latency_us() != 10 || metrics.max_latency_us() != 30 ||
        metrics.avg_latency_us() != 20 ||
        metrics.success_rate_percent() != 66) {
        return 32;
    }
    metrics.record_request_failed();
    metrics.record_request_timeout();
    metrics.record_request_dropped();
    if (metrics.requests_failed() != 1 ||
        metrics.requests_timed_out() != 1 ||
        metrics.in_flight_requests() != 0) {
        return 33;
    }
    metrics.record_request_completed();
    if (metrics.requests_completed() != 3 ||
        metrics.avg_latency_us() != 13 ||
        metrics.success_rate_percent() != 100 ||
        metrics.in_flight_requests() != 0) {
        return 34;
    }

    metrics.record_bytes_sent(11);
    metrics.record_bytes_sent(7);
    metrics.record_bytes_received(23);
    metrics.record_reconnect();
    metrics.record_retry_attempt();
    metrics.record_queue_drop();
    metrics.record_circuit_open_rejection();
    metrics.record_circuit_open_transition();
    metrics.record_circuit_half_open_transition();
    metrics.record_circuit_closed_transition();
    if (metrics.bytes_sent() != 18 || metrics.bytes_received() != 23 ||
        metrics.reconnect_count() != 1 || metrics.retry_attempts() != 1 ||
        metrics.queue_dropped_requests() != 1 ||
        metrics.circuit_open_rejections() != 1 ||
        metrics.circuit_open_transitions() != 1 ||
        metrics.circuit_half_open_transitions() != 1 ||
        metrics.circuit_closed_transitions() != 1) {
        return 35;
    }

    metrics.record_connect(1000);
    if (metrics.connect_time_ms() != 1000 || metrics.uptime_ms(999) != 0 ||
        metrics.uptime_ms(1000) != 0 || metrics.uptime_ms(1123) != 123) {
        return 36;
    }
    const auto metrics_snapshot = metrics;
    metrics.record_bytes_sent(1);
    if (metrics_snapshot.bytes_sent() != 18 || metrics.bytes_sent() != 19) {
        return 37;
    }
    metrics.reset();
    if (!metrics_are_reset(metrics)) {
        return 38;
    }
    metrics.bytes_sent_field.store(
        std::numeric_limits<std::uint64_t>::max(),
        rusty::sync::atomic::Ordering::Relaxed);
    metrics.record_bytes_sent(1);
    if (metrics.bytes_sent() != 0) {
        return 39;
    }
    metrics.requests_completed_field.store(
        std::numeric_limits<std::uint64_t>::max(),
        rusty::sync::atomic::Ordering::Relaxed);
    metrics.requests_sent_field.store(
        3, rusty::sync::atomic::Ordering::Relaxed);
    constexpr auto kWrappedPercent =
        (std::numeric_limits<std::uint64_t>::max() * std::uint64_t{100}) /
        std::uint64_t{3};
    if (metrics.success_rate_percent() != kWrappedPercent) {
        return 40;
    }
    if (!metrics_concurrent_updates_are_atomic()) {
        return 41;
    }

    CallbackActual empty;
    if (empty.has_value() || CallbackActual::default_().has_value()) {
        return 50;
    }

    std::vector<int> observations;
    auto original = CallbackActual::from_callable(
        CallbackStatefulCallable{&observations});
    auto copy = original;
    auto cloned = original.clone();
    original.callable()(1);
    copy.callable()(2);
    cloned.callable()(3);
    if (observations != std::vector<int>{1, 2, 3}) {
        return 51;
    }
    if (&original.callable() != &copy.callable() ||
        &original.callable() != &cloned.callable()) {
        return 52;
    }

    auto owned = std::make_unique<int>(41);
    int result = 0;
    auto move_only = CallbackActual::from_callable(
        [payload = std::move(owned), &result](int value) {
            result = *payload + value;
        });
    if (owned != nullptr || !move_only.has_value()) {
        return 53;
    }
    move_only.callable()(1);
    if (result != 42) {
        return 54;
    }

    int named_result = 0;
    std::function<void(int)> named =
        [&](int value) { named_result = value; };
    auto named_wrapper = CallbackActual::from_callable(std::move(named));
    named_wrapper.callable()(17);
    if (named_result != 17) {
        return 55;
    }

    using CallbackActualMove =
        srpc::detail::CallbackWrapper<CallbackMoveObservedCallable>;
    using CallbackOracleMove =
        callback_oracle::CallbackWrapper<CallbackMoveObservedCallable>;
    static_assert(sizeof(CallbackActualMove) == sizeof(CallbackOracleMove));
    static_assert(alignof(CallbackActualMove) == alignof(CallbackOracleMove));
    auto actual_moves = std::make_shared<int>(0);
    auto oracle_moves = std::make_shared<int>(0);
    auto actual_move_wrapper = CallbackActualMove::from_callable(
        CallbackMoveObservedCallable{actual_moves});
    auto oracle_move_wrapper = CallbackOracleMove::from_callable(
        CallbackMoveObservedCallable{oracle_moves});
    if (!actual_move_wrapper.has_value() ||
        !oracle_move_wrapper.has_value()) {
        return 56;
    }
    if (*actual_moves != 1 || *oracle_moves != 1 ||
        *actual_moves != *oracle_moves) {
        return 57;
    }

    const auto completion_defaults =
        srpc::CompletionTrackerConfig::defaults();
    const auto completion_small = srpc::CompletionTrackerConfig::small();
    const auto completion_large = srpc::CompletionTrackerConfig::large();
    const auto completion_disabled =
        srpc::CompletionTrackerConfig::disabled();
    if (completion_defaults.ttl_ms != 60000 ||
        completion_defaults.max_entries != 100000 ||
        !completion_defaults.enabled ||
        completion_small.ttl_ms != 30000 ||
        completion_small.max_entries != 10000 ||
        !completion_small.enabled ||
        completion_large.ttl_ms != 300000 ||
        completion_large.max_entries != 1000000 ||
        !completion_large.enabled || completion_disabled.enabled) {
        return 60;
    }

    const auto completion_not_found =
        srpc::CompletionQueryResult::not_found();
    const auto completion_ok =
        srpc::CompletionQueryResult::completed(0, true);
    const auto completion_error =
        srpc::CompletionQueryResult::completed(-7, false);
    const auto completion_expired =
        srpc::CompletionQueryResult::expired();
    if (completion_not_found.status != srpc::CompletionStatus::NOT_FOUND ||
        completion_not_found.error_code != 0 ||
        completion_not_found.has_cached_response ||
        completion_not_found.is_completed() ||
        completion_ok.status != srpc::CompletionStatus::COMPLETED ||
        completion_ok.error_code != 0 ||
        !completion_ok.has_cached_response || !completion_ok.is_completed() ||
        completion_error.status !=
            srpc::CompletionStatus::COMPLETED_WITH_ERROR ||
        completion_error.error_code != -7 ||
        completion_error.has_cached_response ||
        !completion_error.is_completed() ||
        completion_expired.status != srpc::CompletionStatus::EXPIRED ||
        completion_expired.is_completed() ||
        srpc::completion_status_to_string(srpc::CompletionStatus::NOT_FOUND) !=
            "NOT_FOUND" ||
        srpc::completion_status_to_string(srpc::CompletionStatus::COMPLETED) !=
            "COMPLETED" ||
        srpc::completion_status_to_string(
            srpc::CompletionStatus::COMPLETED_WITH_ERROR) !=
            "COMPLETED_WITH_ERROR" ||
        srpc::completion_status_to_string(srpc::CompletionStatus::EXPIRED) !=
            "EXPIRED" ||
        srpc::completion_status_to_string(
            static_cast<srpc::CompletionStatus>(99)) != "UNKNOWN") {
        return 61;
    }

    const auto wrapping_entry = srpc::CompletedEntry::new_(
        77, std::numeric_limits<std::uint64_t>::max() - 5);
    if (wrapping_entry.xid != 77 ||
        wrapping_entry.timestamp_ms !=
            std::numeric_limits<std::uint64_t>::max() - 5 ||
        wrapping_entry.is_expired(1000, 0) ||
        wrapping_entry.is_expired(4, 10) ||
        !wrapping_entry.is_expired(5, 10)) {
        return 62;
    }

    auto disabled_tracker = srpc::CompletionTracker::with_config(completion_disabled);
    disabled_tracker.mark_completed(1, 0);
    if (disabled_tracker.enabled() || disabled_tracker.size() != 0 ||
        disabled_tracker.total_tracked() != 0 ||
        disabled_tracker.is_completed(1, 0) ||
        disabled_tracker.queries() != 1 ||
        disabled_tracker.query_hits() != 0) {
        return 63;
    }

    auto lifecycle_config = srpc::CompletionTrackerConfig::defaults();
    lifecycle_config.ttl_ms = 10;
    lifecycle_config.max_entries = 2;
    auto lifecycle_tracker = srpc::CompletionTracker::with_config(lifecycle_config);
    lifecycle_tracker.mark_completed(1, 0);
    lifecycle_tracker.mark_completed(1, 1);
    lifecycle_tracker.mark_completed(2, 0);
    if (lifecycle_tracker.size() != 2 ||
        lifecycle_tracker.total_tracked() != 2 ||
        lifecycle_tracker.queries() != 0 ||
        lifecycle_tracker.hit_rate() != 0.0 ||
        !lifecycle_tracker.is_completed(1, 10) ||
        lifecycle_tracker.is_completed(1, 11) ||
        lifecycle_tracker.size() != 1 ||
        lifecycle_tracker.queries() != 2 ||
        lifecycle_tracker.query_hits() != 1) {
        return 64;
    }
    lifecycle_tracker.mark_completed(3, 20);
    lifecycle_tracker.mark_completed(4, 20);
    if (lifecycle_tracker.size() != 2 ||
        lifecycle_tracker.total_tracked() != 4 ||
        lifecycle_tracker.evictions() != 1 ||
        lifecycle_tracker.is_completed(2, 20) ||
        lifecycle_tracker.evict_expired(31) != 2 ||
        lifecycle_tracker.size() != 0 ||
        lifecycle_tracker.evictions() != 3) {
        return 65;
    }

    auto mutation_config = srpc::CompletionTrackerConfig::defaults();
    mutation_config.ttl_ms = 0;
    auto mutation_tracker = srpc::CompletionTracker::with_config(mutation_config);
    mutation_tracker.mark_completed(10, 1);
    mutation_tracker.mark_completed(11, 1);
    if (!mutation_tracker.remove(10) || mutation_tracker.remove(10) ||
        mutation_tracker.size() != 1) {
        return 66;
    }
    mutation_tracker.clear();
    mutation_tracker.set_config(completion_disabled);
    const auto mutated_config = mutation_tracker.config();
    mutation_tracker.mark_completed(12, 1);
    if (mutation_tracker.size() != 0 || mutated_config.enabled ||
        mutated_config.ttl_ms != completion_disabled.ttl_ms ||
        mutated_config.max_entries != completion_disabled.max_entries) {
        return 67;
    }

    auto overflow_config = srpc::CompletionTrackerConfig::defaults();
    overflow_config.ttl_ms = 0;
    overflow_config.max_entries = 1;
    auto overflow_tracker = srpc::CompletionTracker::with_config(overflow_config);
    overflow_tracker.mark_completed(1, 0);
    using rusty::sync::atomic::Ordering;
    overflow_tracker.total_tracked_.store(
        std::numeric_limits<std::uint64_t>::max(), Ordering::Relaxed);
    overflow_tracker.queries_.store(
        std::numeric_limits<std::uint64_t>::max(), Ordering::Relaxed);
    overflow_tracker.query_hits_.store(
        std::numeric_limits<std::uint64_t>::max(), Ordering::Relaxed);
    overflow_tracker.evictions_.store(
        std::numeric_limits<std::uint64_t>::max(), Ordering::Relaxed);
    overflow_tracker.mark_completed(2, 0);
    if (!overflow_tracker.is_completed(2, 0) ||
        overflow_tracker.size() != 1 ||
        overflow_tracker.total_tracked() != 0 ||
        overflow_tracker.queries() != 0 ||
        overflow_tracker.query_hits() != 0 ||
        overflow_tracker.evictions() != 0 ||
        overflow_tracker.hit_rate() != 0.0) {
        return 68;
    }
    overflow_tracker.reset_stats();
    if (overflow_tracker.total_tracked() != 0 ||
        overflow_tracker.queries() != 0 ||
        overflow_tracker.query_hits() != 0 ||
        overflow_tracker.evictions() != 0) {
        return 69;
    }
    if (!completion_tracker_concurrent_operations_are_safe()) {
        return 70;
    }

    if (srpc::randgen_rand_max() !=
            static_cast<double>(std::numeric_limits<std::int32_t>::max()) ||
        srpc::randgen_nu_constant_now() != 0) {
        return 71;
    }

    rand_string_evaluations = 0;
    const auto padded_binary =
        srpc::randgen_zero_pad(make_rand_binary_string(), 5);
    const auto truncated_binary =
        srpc::randgen_zero_pad(make_rand_binary_string(), 2);
    if (rand_string_evaluations != 2 || padded_binary.size() != 5 ||
        padded_binary[0] != '0' || padded_binary[1] != '0' ||
        static_cast<unsigned char>(padded_binary[2]) != 0x00 ||
        static_cast<unsigned char>(padded_binary[3]) != 0x80 ||
        static_cast<unsigned char>(padded_binary[4]) != 0xff ||
        truncated_binary.size() != 2 ||
        static_cast<unsigned char>(truncated_binary[0]) != 0x80 ||
        static_cast<unsigned char>(truncated_binary[1]) != 0xff ||
        srpc::randgen_zero_pad("7", 3) != "007" ||
        srpc::randgen_zero_pad("1234", 3) != "234" ||
        srpc::randgen_zero_pad("1234", 0) != "") {
        return 72;
    }

    if (srpc::RandomGenerator::int2str_n(0, 1) != "0" ||
        srpc::RandomGenerator::int2str_n(42, 5) != "00042" ||
        srpc::RandomGenerator::int2str_n(-7, 4) != "00-7" ||
        srpc::RandomGenerator::int2str_n(12345, 3) != "345" ||
        srpc::RandomGenerator::int2str_n(-12345, 4) != "2345" ||
        srpc::RandomGenerator::int2str_n(
            std::numeric_limits<std::int32_t>::max(), 10) != "2147483647" ||
        srpc::RandomGenerator::int2str_n(
            std::numeric_limits<std::int32_t>::min(), 11) != "-2147483648" ||
        srpc::RandomGenerator::int2str_n(
            std::numeric_limits<std::int32_t>::min(), 10) != "2147483648") {
        return 73;
    }

    install_rand_raw(17);
    if (srpc::randgen_rand_raw() != 17 || rand_raw_draws != 1) {
        return 74;
    }
    install_rand_raw(5);
    if (srpc::RandomGenerator::rand(-10, -5) != -5 || rand_raw_draws != 1) {
        return 75;
    }
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    if (srpc::RandomGenerator::rand(
            0, std::numeric_limits<std::int32_t>::max()) !=
            std::numeric_limits<std::int32_t>::max() ||
        rand_raw_draws != 1) {
        return 76;
    }

    install_rand_raw(123);
    if (srpc::RandomGenerator::rand_double(4.5, 4.5) != 4.5 ||
        rand_raw_draws != 0) {
        return 77;
    }
    const auto scaled_rand = srpc::RandomGenerator::rand_double(-1.0, 1.0);
    const auto expected_scaled_rand =
        (123.0 /
         (static_cast<double>(std::numeric_limits<std::int32_t>::max()) / 2.0)) -
        1.0;
    if (scaled_rand != expected_scaled_rand || rand_raw_draws != 1) {
        return 78;
    }

    install_rand_raw(0);
    if (srpc::RandomGenerator::percentage_true(0) || rand_raw_draws != 1) {
        return 79;
    }
    install_rand_raw(0);
    if (!srpc::RandomGenerator::percentage_true(1) || rand_raw_draws != 1) {
        return 80;
    }
    install_rand_raw(5);
    if (srpc::RandomGenerator::nu_rand(1022, 0, 999) != 5 ||
        rand_raw_draws != 2) {
        return 81;
    }

    install_rand_raw(99);
    const std::vector<double> empty_weights;
    if (srpc::RandomGenerator::weighted_select(empty_weights) !=
            std::numeric_limits<std::uint32_t>::max() ||
        rand_raw_draws != 0) {
        return 82;
    }
    install_rand_raw(99);
    const std::vector<double> zero_weights{0.0, 0.0};
    if (srpc::RandomGenerator::weighted_select(zero_weights) != 0 ||
        rand_raw_draws != 0) {
        return 83;
    }

    const std::vector<double> weights{1.0, 2.0, 3.0};
    install_rand_raw(0);
    if (srpc::RandomGenerator::weighted_select(weights) != 0 ||
        rand_raw_draws != 1) {
        return 84;
    }
    install_rand_raw(std::numeric_limits<std::int32_t>::max() / 2);
    if (srpc::RandomGenerator::weighted_select(weights) != 1 ||
        rand_raw_draws != 1) {
        return 85;
    }
    rand_weight_evaluations = 0;
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    if (srpc::RandomGenerator::weighted_select(make_rand_weights()) != 2 ||
        rand_raw_draws != 1 || rand_weight_evaluations != 1) {
        return 86;
    }

    const auto destroys_before = rand_destroy_calls;
    srpc::randgen_destroy();
    srpc::RandomGenerator::destroy();
    if (rand_destroy_calls != destroys_before + 2) {
        return 87;
    }

    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    if (srpc::RandomGenerator::rand(7, 7) != 7 || rand_raw_draws != 1) {
        return 88;
    }
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    if (srpc::RandomGenerator::rand(
            std::numeric_limits<std::int32_t>::min(), -1) != -1 ||
        rand_raw_draws != 1) {
        return 89;
    }

    bool rand_failed = false;
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    try {
        static_cast<void>(srpc::RandomGenerator::rand(
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
    } catch (...) {
        rand_failed = true;
    }
    if (!rand_failed || rand_raw_draws != 1) {
        return 90;
    }

    rand_failed = false;
    install_rand_raw(11);
    try {
        static_cast<void>(srpc::RandomGenerator::rand(9, 8));
    } catch (...) {
        rand_failed = true;
    }
    if (!rand_failed || rand_raw_draws != 0) {
        return 91;
    }

    rand_failed = false;
    install_rand_raw(123);
    try {
        static_cast<void>(srpc::RandomGenerator::rand_double(2.0, 1.0));
    } catch (...) {
        rand_failed = true;
    }
    if (!rand_failed || rand_raw_draws != 0) {
        return 92;
    }

    rand_failed = false;
    install_rand_raw(123);
    try {
        static_cast<void>(srpc::RandomGenerator::rand_double(
            0.0, std::numeric_limits<double>::quiet_NaN()));
    } catch (...) {
        rand_failed = true;
    }
    if (!rand_failed || rand_raw_draws != 0) {
        return 93;
    }

    rand_failed = false;
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    try {
        static_cast<void>(srpc::RandomGenerator::nu_rand(
            0, std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
    } catch (...) {
        rand_failed = true;
    }
    if (!rand_failed || rand_raw_draws != 2) {
        return 94;
    }

    const std::vector<double> positive_boundary_weights{
        1.0,
        static_cast<double>(std::numeric_limits<std::int32_t>::max() - 1),
    };
    install_rand_raw(1);
    if (srpc::RandomGenerator::weighted_select(positive_boundary_weights) != 0 ||
        rand_raw_draws != 1) {
        return 95;
    }

    if (static_cast<std::int32_t>(srpc::TimeoutType::NONE) != 0 ||
        static_cast<std::int32_t>(srpc::TimeoutType::CONNECT_TIMEOUT) != 1 ||
        static_cast<std::int32_t>(srpc::TimeoutType::REQUEST_TIMEOUT) != 2 ||
        static_cast<std::int32_t>(srpc::TimeoutType::RESPONSE_TIMEOUT) != 3 ||
        static_cast<std::int32_t>(srpc::TimeoutType::TOTAL_TIMEOUT) != 4 ||
        srpc::TimeoutType_NONE() != srpc::TimeoutType::NONE ||
        srpc::TimeoutType_CONNECT_TIMEOUT() !=
            srpc::TimeoutType::CONNECT_TIMEOUT ||
        srpc::TimeoutType_REQUEST_TIMEOUT() !=
            srpc::TimeoutType::REQUEST_TIMEOUT ||
        srpc::TimeoutType_RESPONSE_TIMEOUT() !=
            srpc::TimeoutType::RESPONSE_TIMEOUT ||
        srpc::TimeoutType_TOTAL_TIMEOUT() != srpc::TimeoutType::TOTAL_TIMEOUT ||
        srpc::timeout_type_to_string(srpc::TimeoutType::NONE) != "NONE" ||
        srpc::timeout_type_to_string(srpc::TimeoutType::CONNECT_TIMEOUT) !=
            "CONNECT_TIMEOUT" ||
        srpc::timeout_type_to_string(srpc::TimeoutType::REQUEST_TIMEOUT) !=
            "REQUEST_TIMEOUT" ||
        srpc::timeout_type_to_string(srpc::TimeoutType::RESPONSE_TIMEOUT) !=
            "RESPONSE_TIMEOUT" ||
        srpc::timeout_type_to_string(srpc::TimeoutType::TOTAL_TIMEOUT) !=
            "TOTAL_TIMEOUT" ||
        srpc::timeout_type_to_string(static_cast<srpc::TimeoutType>(99)) !=
            "UNKNOWN") {
        return 96;
    }

    const auto request_defaults = srpc::RequestOptions::defaults();
    const auto request_new = srpc::RequestOptions::new_();
    if (request_defaults.timeout_ms != 1000 ||
        request_defaults.total_timeout_ms != 0 ||
        request_defaults.max_retries != 0 ||
        request_defaults.base_delay_ms != 50 ||
        request_defaults.max_delay_ms != 5000 ||
        request_defaults.jitter_factor != 0.1f ||
        request_defaults.idempotent ||
        request_new.timeout_ms != request_defaults.timeout_ms ||
        request_new.total_timeout_ms != request_defaults.total_timeout_ms ||
        request_new.max_retries != request_defaults.max_retries ||
        request_new.base_delay_ms != request_defaults.base_delay_ms ||
        request_new.max_delay_ms != request_defaults.max_delay_ms ||
        request_new.jitter_factor != request_defaults.jitter_factor ||
        request_new.idempotent != request_defaults.idempotent ||
        request_defaults.can_retry(0)) {
        return 97;
    }

    const auto request_retry = srpc::RequestOptions::with_retry(3, 2000);
    const auto request_idempotent =
        srpc::RequestOptions::idempotent_retry(10);
    const auto request_no_timeout = srpc::RequestOptions::no_timeout();
    const auto request_fast = srpc::RequestOptions::fast();
    const auto request_patient = srpc::RequestOptions::patient();
    if (request_retry.timeout_ms != 2000 || request_retry.max_retries != 3 ||
        !request_retry.idempotent || !request_retry.can_retry(0) ||
        !request_retry.can_retry(2) || request_retry.can_retry(3) ||
        request_idempotent.timeout_ms != 1000 ||
        request_idempotent.max_retries != 10 ||
        !request_idempotent.idempotent || request_no_timeout.timeout_ms != 0 ||
        request_fast.timeout_ms != 100 || request_fast.max_retries != 2 ||
        request_fast.base_delay_ms != 10 || request_fast.max_delay_ms != 100 ||
        request_patient.timeout_ms != 10000 ||
        request_patient.total_timeout_ms != 60000 ||
        request_patient.max_retries != 5 ||
        request_patient.base_delay_ms != 500 ||
        request_patient.max_delay_ms != 10000) {
        return 98;
    }

    auto request_limited = request_defaults;
    request_limited.total_timeout_ms = 5000;
    if (request_limited.is_total_timeout_exceeded(4999) ||
        !request_limited.is_total_timeout_exceeded(5000) ||
        request_limited.remaining_time_ms(0) != 5000 ||
        request_limited.remaining_time_ms(4999) != 1 ||
        request_limited.remaining_time_ms(5000) != 0 ||
        request_limited.remaining_time_ms(
            std::numeric_limits<std::uint64_t>::max()) != 0 ||
        request_defaults.remaining_time_ms(
            std::numeric_limits<std::uint64_t>::max()) !=
            std::numeric_limits<std::uint64_t>::max()) {
        return 99;
    }

    auto request_delay = request_defaults;
    request_delay.base_delay_ms = 100;
    request_delay.max_delay_ms = 500;
    request_delay.jitter_factor = 0.0f;
    install_rand_raw(17);
    if (request_delay.calculate_delay_ms(0) != 100 ||
        request_delay.calculate_delay_ms(1) != 200 ||
        request_delay.calculate_delay_ms(2) != 400 ||
        request_delay.calculate_delay_ms(3) != 500 ||
        request_delay.calculate_delay_ms(
            std::numeric_limits<std::uint16_t>::max()) != 500 ||
        rand_raw_draws != 0) {
        return 100;
    }

    request_delay.jitter_factor = -0.1f;
    if (request_delay.calculate_delay_ms(0) != 100 || rand_raw_draws != 0) {
        return 101;
    }
    request_delay.jitter_factor = std::numeric_limits<float>::quiet_NaN();
    if (request_delay.calculate_delay_ms(0) != 100 || rand_raw_draws != 0) {
        return 102;
    }

    request_delay.jitter_factor = 0.2f;
    install_rand_raw(0);
    const auto request_low_expected = static_cast<std::uint64_t>(
        100.0 + 100.0 * static_cast<double>(request_delay.jitter_factor) *
                    ((0.0 / static_cast<double>(
                                std::numeric_limits<std::int32_t>::max())) -
                     0.5));
    if (request_delay.calculate_delay_ms(0) != request_low_expected ||
        rand_raw_draws != 1) {
        return 103;
    }
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    const auto request_high_expected = static_cast<std::uint64_t>(
        100.0 + 100.0 * static_cast<double>(request_delay.jitter_factor) * 0.5);
    if (request_delay.calculate_delay_ms(0) != request_high_expected ||
        rand_raw_draws != 1) {
        return 104;
    }

    request_delay.base_delay_ms = 1000;
    request_delay.max_delay_ms = 500;
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    const auto request_capped_expected = static_cast<std::uint64_t>(
        500.0 + 500.0 * static_cast<double>(request_delay.jitter_factor) * 0.5);
    if (request_delay.calculate_delay_ms(0) != request_capped_expected ||
        rand_raw_draws != 1) {
        return 105;
    }

    request_delay.base_delay_ms = 0;
    install_rand_raw(123);
    if (request_delay.calculate_delay_ms(
            std::numeric_limits<std::uint16_t>::max()) != 0 ||
        rand_raw_draws != 1) {
        return 106;
    }

    request_delay.base_delay_ms = 100;
    request_delay.max_delay_ms = 500;
    request_delay.jitter_factor = 10.0f;
    install_rand_raw(-std::numeric_limits<std::int32_t>::max());
    if (request_delay.calculate_delay_ms(0) != 0 || rand_raw_draws != 1) {
        return 107;
    }

    request_delay.base_delay_ms = std::numeric_limits<std::uint16_t>::max();
    request_delay.max_delay_ms = std::numeric_limits<std::uint16_t>::max();
    request_delay.jitter_factor = std::numeric_limits<float>::max();
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    if (request_delay.calculate_delay_ms(0) !=
            std::numeric_limits<std::uint64_t>::max() ||
        rand_raw_draws != 1) {
        return 108;
    }

    const auto reconnect_new = srpc::ReconnectPolicy::new_();
    const auto reconnect_conservative = srpc::ReconnectPolicy::conservative();
    const auto reconnect_aggressive = srpc::ReconnectPolicy::aggressive();
    const auto reconnect_none = srpc::ReconnectPolicy::no_retry();
    if (!reconnect_new.auto_reconnect || reconnect_new.max_retries != 5 ||
        reconnect_new.initial_delay_ms != 1000 ||
        reconnect_new.max_delay_ms != 30000 ||
        reconnect_new.backoff_multiplier != 2.0 ||
        !reconnect_new.jitter_enabled ||
        reconnect_conservative.auto_reconnect != reconnect_new.auto_reconnect ||
        reconnect_conservative.max_retries != reconnect_new.max_retries ||
        reconnect_conservative.initial_delay_ms !=
            reconnect_new.initial_delay_ms ||
        reconnect_conservative.max_delay_ms != reconnect_new.max_delay_ms ||
        reconnect_conservative.backoff_multiplier !=
            reconnect_new.backoff_multiplier ||
        reconnect_conservative.jitter_enabled !=
            reconnect_new.jitter_enabled ||
        !reconnect_aggressive.auto_reconnect ||
        reconnect_aggressive.max_retries != 0 ||
        reconnect_aggressive.initial_delay_ms != 100 ||
        reconnect_aggressive.max_delay_ms != 5000 ||
        reconnect_aggressive.backoff_multiplier != 1.5 ||
        !reconnect_aggressive.jitter_enabled ||
        reconnect_none.auto_reconnect || reconnect_none.max_retries != 0 ||
        reconnect_none.initial_delay_ms != 0 ||
        reconnect_none.max_delay_ms != 0 ||
        reconnect_none.backoff_multiplier != 1.0 ||
        reconnect_none.jitter_enabled) {
        return 109;
    }

    auto reconnect_limited = reconnect_new;
    reconnect_limited.max_retries = 3;
    reconnect_limited.initial_delay_ms = 100;
    reconnect_limited.max_delay_ms = 250;
    reconnect_limited.jitter_enabled = false;
    auto reconnect_calculator =
        srpc::ReconnectCalculator::new_(reconnect_limited);
    if (&reconnect_calculator.policy != &reconnect_limited ||
        reconnect_calculator.retry_count() != 0 ||
        reconnect_calculator.peek_delay_ms() != 100 ||
        !reconnect_calculator.should_retry() ||
        reconnect_calculator.retries_exhausted()) {
        return 110;
    }
    install_rand_raw(17);
    if (reconnect_calculator.next_delay_ms() != 100 ||
        reconnect_calculator.retry_count() != 1 ||
        reconnect_calculator.peek_delay_ms() != 200 ||
        reconnect_calculator.next_delay_ms() != 200 ||
        reconnect_calculator.retry_count() != 2 ||
        reconnect_calculator.peek_delay_ms() != 250 ||
        reconnect_calculator.next_delay_ms() != 250 ||
        reconnect_calculator.retry_count() != 3 ||
        reconnect_calculator.should_retry() ||
        !reconnect_calculator.retries_exhausted() || rand_raw_draws != 0) {
        return 111;
    }
    reconnect_calculator.reset();
    if (reconnect_calculator.retry_count() != 0 ||
        !reconnect_calculator.should_retry() ||
        reconnect_calculator.retries_exhausted()) {
        return 112;
    }

    auto reconnect_unlimited = reconnect_aggressive;
    reconnect_unlimited.jitter_enabled = false;
    auto unlimited_calculator =
        srpc::ReconnectCalculator::new_(reconnect_unlimited);
    auto no_retry_calculator = srpc::ReconnectCalculator::new_(reconnect_none);
    if (!unlimited_calculator.should_retry() ||
        unlimited_calculator.retries_exhausted() ||
        no_retry_calculator.should_retry() ||
        !no_retry_calculator.retries_exhausted()) {
        return 113;
    }

    auto reconnect_jitter = reconnect_new;
    reconnect_jitter.initial_delay_ms = 100;
    reconnect_jitter.max_delay_ms = 1000;
    auto jitter_calculator =
        srpc::ReconnectCalculator::new_(reconnect_jitter);
    install_rand_raw(0);
    if (jitter_calculator.next_delay_ms() != 50 || rand_raw_draws != 1 ||
        jitter_calculator.peek_delay_ms() != 200 || rand_raw_draws != 1) {
        return 114;
    }
    jitter_calculator.reset();
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    if (jitter_calculator.next_delay_ms() != 150 || rand_raw_draws != 1) {
        return 115;
    }
    reconnect_jitter.initial_delay_ms = 0;
    jitter_calculator.reset();
    install_rand_raw(123);
    if (jitter_calculator.next_delay_ms() != 0 || rand_raw_draws != 0) {
        return 116;
    }

    const auto circuit_new = srpc::CircuitBreakerConfig::new_();
    const auto circuit_defaults = srpc::CircuitBreakerConfig::defaults();
    const auto circuit_sensitive = srpc::CircuitBreakerConfig::sensitive();
    const auto circuit_relaxed = srpc::CircuitBreakerConfig::relaxed();
    const auto circuit_disabled = srpc::CircuitBreakerConfig::disabled();
    if (circuit_new.failure_threshold != circuit_defaults.failure_threshold ||
        circuit_new.success_threshold != circuit_defaults.success_threshold ||
        circuit_new.timeout_ms != circuit_defaults.timeout_ms ||
        circuit_new.enabled != circuit_defaults.enabled ||
        circuit_defaults.failure_threshold != 5 ||
        circuit_defaults.success_threshold != 3 ||
        circuit_defaults.timeout_ms != 30000 || !circuit_defaults.enabled ||
        circuit_sensitive.failure_threshold != 3 ||
        circuit_sensitive.success_threshold != 5 ||
        circuit_sensitive.timeout_ms != 60000 ||
        circuit_relaxed.failure_threshold != 10 ||
        circuit_relaxed.success_threshold != 2 ||
        circuit_relaxed.timeout_ms != 15000 ||
        circuit_disabled.failure_threshold != 0 ||
        circuit_disabled.success_threshold != 0 ||
        circuit_disabled.timeout_ms != 0 || circuit_disabled.enabled ||
        srpc::circuit_state_to_string(srpc::CircuitState::CLOSED) != "CLOSED" ||
        srpc::circuit_state_to_string(srpc::CircuitState::OPEN) != "OPEN" ||
        srpc::circuit_state_to_string(srpc::CircuitState::HALF_OPEN) !=
            "HALF_OPEN") {
        return 117;
    }

    monotonic_now_us = 1'000'000;
    auto circuit_config = circuit_defaults;
    circuit_config.failure_threshold = 2;
    circuit_config.success_threshold = 2;
    circuit_config.timeout_ms = 10;
    auto circuit = srpc::CircuitBreaker::new_(circuit_config);
    if (!circuit.is_closed() || circuit.is_open() ||
        !circuit.allow_request() || circuit.failure_count() != 0 ||
        srpc::current_time_us() != monotonic_now_us) {
        return 118;
    }
    circuit.record_failure();
    circuit.record_failure();
    if (!circuit.is_open() || circuit.failure_count() != 0 ||
        circuit.allow_request() || circuit.last_failure_time.get() != 1'000'000) {
        return 119;
    }
    monotonic_now_us = 1'009'999;
    if (circuit.allow_request()) {
        return 120;
    }
    monotonic_now_us = 1'010'000;
    if (!circuit.allow_request() || !circuit.is_half_open() ||
        circuit.allow_request()) {
        return 121;
    }
    circuit.record_success();
    if (circuit.success_count() != 1 || !circuit.allow_request()) {
        return 122;
    }
    circuit.record_success();
    if (!circuit.is_closed() || circuit.success_count() != 0) {
        return 123;
    }
    circuit.failure_count_field.set(std::numeric_limits<std::uint32_t>::max());
    circuit.record_failure();
    if (circuit.failure_count() != 0 || !circuit.is_closed()) {
        return 124;
    }

    srpc::StateChangeCallback empty_state_callback{};
    if (empty_state_callback || !empty_state_callback.is_empty()) {
        return 125;
    }
    auto state_machine = srpc::ConnectionStateMachine::new_();
    if (!state_machine.on_state_change.is_empty() ||
        state_machine.state() != srpc::ConnectionState::NEW ||
        state_machine.transition_to(srpc::ConnectionState::CONNECTED) ||
        !state_machine.transition_to(srpc::ConnectionState::CONNECTING)) {
        return 126;
    }
    int state_callback_calls = 0;
    srpc::ConnectionState observed_from = srpc::ConnectionState::NEW;
    srpc::ConnectionState observed_to = srpc::ConnectionState::NEW;
    state_machine.set_on_state_change(
        [&](srpc::ConnectionState from, srpc::ConnectionState to) {
            ++state_callback_calls;
            observed_from = from;
            observed_to = to;
        });
    if (state_machine.on_state_change.is_empty() ||
        !state_machine.transition_to(srpc::ConnectionState::CONNECTED) ||
        state_callback_calls != 1 ||
        observed_from != srpc::ConnectionState::CONNECTING ||
        observed_to != srpc::ConnectionState::CONNECTED) {
        return 127;
    }
    state_machine.force_state(srpc::ConnectionState::FAILED);
    if (state_callback_calls != 2 ||
        observed_from != srpc::ConnectionState::CONNECTED ||
        observed_to != srpc::ConnectionState::FAILED ||
        !state_machine.is_failed() || !state_machine.is_terminal()) {
        return 128;
    }

    srpc::HeartbeatTimeoutCallback empty_heartbeat_callback{};
    if (empty_heartbeat_callback || !empty_heartbeat_callback.is_empty()) {
        return 129;
    }
    int moved_callback_calls = 0;
    srpc::HeartbeatTimeoutCallback moved_from =
        MutableHeartbeatCallable{&moved_callback_calls};
    auto moved_to = std::move(moved_from);
    if (moved_from || !moved_from.is_empty() || !moved_to ||
        moved_to.is_empty()) {
        return 130;
    }
    moved_to();
    if (moved_callback_calls != 1) {
        return 131;
    }

    const auto heartbeat_defaults = srpc::HeartbeatConfig::defaults();
    const auto heartbeat_aggressive = srpc::HeartbeatConfig::aggressive();
    const auto heartbeat_relaxed = srpc::HeartbeatConfig::relaxed();
    const auto heartbeat_disabled = srpc::HeartbeatConfig::disabled();
    if (!heartbeat_defaults.enabled || heartbeat_defaults.interval_ms != 10000 ||
        heartbeat_defaults.timeout_ms != 5000 ||
        heartbeat_defaults.max_missed != 3 ||
        heartbeat_aggressive.interval_ms != 5000 ||
        heartbeat_aggressive.timeout_ms != 2000 ||
        heartbeat_aggressive.max_missed != 2 ||
        heartbeat_relaxed.interval_ms != 30000 ||
        heartbeat_relaxed.timeout_ms != 15000 ||
        heartbeat_relaxed.max_missed != 5 || heartbeat_disabled.enabled) {
        return 132;
    }

    auto empty_timeout_config = heartbeat_defaults;
    empty_timeout_config.interval_ms = 1;
    empty_timeout_config.timeout_ms = 0;
    empty_timeout_config.max_missed = 1;
    auto empty_timeout = srpc::HeartbeatManager::new_(empty_timeout_config);
    if (!(*empty_timeout.on_timeout.borrow()).is_empty()) {
        return 133;
    }
    monotonic_now_us = std::numeric_limits<std::uint64_t>::max() - 5;
    empty_timeout.on_heartbeat_sent();
    monotonic_now_us = 4;
    if (!empty_timeout.check_timeout() || !empty_timeout.is_timed_out() ||
        empty_timeout.missed_count() != 1 ||
        empty_timeout.is_pending_pong()) {
        return 134;
    }

    auto heartbeat_config = heartbeat_defaults;
    heartbeat_config.interval_ms = 1;
    heartbeat_config.timeout_ms = 2;
    heartbeat_config.max_missed = 2;
    auto heartbeat = srpc::HeartbeatManager::new_(heartbeat_config);
    int heartbeat_callback_calls = 0;
    heartbeat.set_on_timeout(
        MutableHeartbeatCallable{&heartbeat_callback_calls});
    monotonic_now_us = 1'000'000;
    if (srpc::heartbeat_time_us() != monotonic_now_us ||
        !heartbeat.should_send_heartbeat()) {
        return 135;
    }
    heartbeat.on_heartbeat_sent();
    monotonic_now_us = 1'001'999;
    if (heartbeat.check_timeout()) {
        return 136;
    }
    monotonic_now_us = 1'002'000;
    if (heartbeat.check_timeout() || heartbeat.missed_count() != 1 ||
        heartbeat.is_timed_out()) {
        return 137;
    }
    monotonic_now_us = 1'003'000;
    if (!heartbeat.should_send_heartbeat()) {
        return 138;
    }
    heartbeat.on_heartbeat_sent();
    monotonic_now_us = 1'005'000;
    if (!heartbeat.check_timeout() || !heartbeat.is_timed_out() ||
        heartbeat_callback_calls != 1 || heartbeat.check_timeout() ||
        heartbeat_callback_calls != 1) {
        return 139;
    }
    heartbeat.reset();
    if (heartbeat.missed_count() != 0 || heartbeat.is_timed_out() ||
        heartbeat.is_pending_pong()) {
        return 140;
    }

    auto wrapping_heartbeat = srpc::HeartbeatManager::new_(heartbeat_config);
    wrapping_heartbeat.missed_count_field.set(
        std::numeric_limits<std::uint32_t>::max());
    monotonic_now_us = std::numeric_limits<std::uint64_t>::max() - 5;
    wrapping_heartbeat.on_heartbeat_sent();
    // The wrapped delta is exactly 2,000 us: 1,994 - (UINT64_MAX - 5).
    monotonic_now_us = 1'994;
    if (wrapping_heartbeat.check_timeout() ||
        wrapping_heartbeat.missed_count() != 0 ||
        wrapping_heartbeat.is_timed_out()) {
        return 141;
    }

    using enum srpc::LoadBalancingStrategy;
    if (srpc::load_balancing_strategy_to_string(RANDOM) != "RANDOM" ||
        srpc::load_balancing_strategy_to_string(ROUND_ROBIN) !=
            "ROUND_ROBIN" ||
        srpc::load_balancing_strategy_to_string(LEAST_CONNECTIONS) !=
            "LEAST_CONNECTIONS" ||
        srpc::load_balancing_strategy_to_string(LEAST_LATENCY) !=
            "LEAST_LATENCY" ||
        srpc::load_balancing_strategy_to_string(
            static_cast<srpc::LoadBalancingStrategy>(255)) != "UNKNOWN") {
        return 177;
    }

    auto load_balancer_state = srpc::LoadBalancerState::new_();
    if (load_balancer_state.next_round_robin_index(0) != 0 ||
        load_balancer_state.next_round_robin_index(3) != 0 ||
        load_balancer_state.next_round_robin_index(3) != 1 ||
        load_balancer_state.next_round_robin_index(3) != 2 ||
        load_balancer_state.next_round_robin_index(3) != 0) {
        return 178;
    }
    load_balancer_state.reset();
    if (load_balancer_state.next_round_robin_index(3) != 0) {
        return 179;
    }
    load_balancer_state.round_robin_index_field.set(
        std::numeric_limits<std::size_t>::max());
    if (load_balancer_state.next_round_robin_index(3) !=
            std::numeric_limits<std::size_t>::max() ||
        load_balancer_state.round_robin_index_field.get() != 0) {
        return 180;
    }

    LoadBalancerProbeClients empty_load_balancer_clients;
    if (srpc::LoadBalancer::select(
            RANDOM, empty_load_balancer_clients, load_balancer_state, 19) != 0) {
        return 181;
    }
    LoadBalancerProbeClients load_balancer_clients{
        std::make_shared<LoadBalancerProbeClient>(
            LoadBalancerProbeClient{LoadBalancerProbeMetrics{5, 0, 0}}),
        std::make_shared<LoadBalancerProbeClient>(
            LoadBalancerProbeClient{LoadBalancerProbeMetrics{2, 80, 10}}),
        std::make_shared<LoadBalancerProbeClient>(
            LoadBalancerProbeClient{LoadBalancerProbeMetrics{2, 30, 3}}),
    };
    if (srpc::lb_pool_size(load_balancer_clients) != 3 ||
        srpc::LoadBalancer::select(
            RANDOM, load_balancer_clients, load_balancer_state, 8) != 2 ||
        srpc::LoadBalancer::select(
            static_cast<srpc::LoadBalancingStrategy>(255),
            load_balancer_clients,
            load_balancer_state,
            8) != 2 ||
        srpc::LoadBalancer::select(
            LEAST_CONNECTIONS,
            load_balancer_clients,
            load_balancer_state,
            0) != 1 ||
        srpc::LoadBalancer::select(
            LEAST_LATENCY,
            load_balancer_clients,
            load_balancer_state,
            0) != 2) {
        return 182;
    }
    load_balancer_state.reset();
    if (srpc::LoadBalancer::select(
            ROUND_ROBIN, load_balancer_clients, load_balancer_state, 0) != 0 ||
        srpc::LoadBalancer::select(
            ROUND_ROBIN, load_balancer_clients, load_balancer_state, 0) != 1 ||
        srpc::LoadBalancer::select(
            ROUND_ROBIN, load_balancer_clients, load_balancer_state, 0) != 2) {
        return 183;
    }

    {
        auto empty = srpc::AddrInfo::new_();
        if (empty.get() != nullptr || empty.valid() || empty.owned_.get() ||
            empty._rusty_forgotten) {
            return 184;
        }
    }
    const auto free_before = freeaddrinfo_calls;
    {
        auto* first = new addrinfo{};
        auto* second = new addrinfo{};
        auto source = srpc::AddrInfo::adopt(first);
        if (source.get() != first || !source.valid() || !source.owned_.get()) {
            return 185;
        }
        srpc::AddrInfo moved(std::move(source));
        if (moved.get() != first || !moved.owned_.get() ||
            source.get() != first || !source._rusty_forgotten) {
            return 186;
        }
        auto target = srpc::AddrInfo::adopt(second);
        target = std::move(moved);
        if (target.get() != first || !target.owned_.get() ||
            moved.get() != first || !moved._rusty_forgotten) {
            return 187;
        }
        target = std::move(target);
        if (target.get() != first || target._rusty_forgotten) {
            return 188;
        }
    }
    if (freeaddrinfo_calls - free_before != 2) {
        return 189;
    }

    // "I " is Log::INFO, "E " is Log::ERROR; a null file renders "<unknown>"
    // and the call site's line is 0 -- exactly the level/line/file the old
    // forwarding fixture recorded.
    selected_open_port = 4321;
    reset_utils_log();
    if (srpc::find_open_port() != 4321 ||
        !utils_logged("I [<unknown>:0] ", " | Found open port: 4321\\n")) {
        return 190;
    }
    selected_open_port = 0;
    reset_utils_log();
    if (srpc::find_open_port() != -1 ||
        !utils_logged("E [<unknown>:0] ", " | Failed to find open port.\\n")) {
        return 191;
    }
    selected_open_port = -1;
    reset_utils_log();
    if (srpc::find_open_port() != -1 ||
        !utils_logged("E [<unknown>:0] ", " | Failed to find open port.\\n")) {
        return 192;
    }

    hostname_mode = 1;
    reset_utils_log();
    if (srpc::get_host_name() != "goal0-host" ||
        hostname_buffer_length != 255 || !utils_log_text().empty()) {
        return 193;
    }
    hostname_mode = -1;
    reset_utils_log();
    if (!srpc::get_host_name().empty() ||
        !utils_logged("E [<unknown>:0] ", " | Failed to get hostname.\\n")) {
        return 194;
    }


    if (srpc::frame_decode_status_to_string(
            srpc::FrameDecodeStatus::NeedMoreBytes) != "NeedMoreBytes" ||
        srpc::frame_decode_status_to_string(
            srpc::FrameDecodeStatus::Complete) != "Complete" ||
        srpc::frame_decode_status_to_string(
            srpc::FrameDecodeStatus::Malformed) != "Malformed") {
        return 195;
    }
    bool invalid_frame_status_threw = false;
    try {
        (void)srpc::frame_decode_status_to_string(
            static_cast<srpc::FrameDecodeStatus>(99));
    } catch (const std::exception&) {
        invalid_frame_status_threw = true;
    }
    if (!invalid_frame_status_threw) {
        return 196;
    }

    const auto frame_native_bytes = [](std::int32_t value) {
        return std::bit_cast<std::array<std::uint8_t, 4>>(value);
    };
    std::array<std::uint8_t, 5> frame_header_bytes{
        0xa1, 0xa2, 0xa3, 0xa4, 0xa5};
    const auto original_frame_header_bytes = frame_header_bytes;
    if (srpc::frame_codec_write_header(
            std::span<std::uint8_t>(frame_header_bytes.data(), 3), 1, false) ||
        frame_header_bytes != original_frame_header_bytes ||
        srpc::frame_codec_write_header(frame_header_bytes, -1, true) ||
        frame_header_bytes != original_frame_header_bytes) {
        return 197;
    }
    if (!srpc::frame_codec_write_header(frame_header_bytes, 0, true) ||
        std::memcmp(frame_header_bytes.data(),
                    frame_native_bytes(INT32_MIN).data(), 4) != 0 ||
        frame_header_bytes[4] != 0xa5 ||
        // A size past the bound must be refused, so this side can never put a
        // header on the wire that the peer's decoder is obliged to reject.
        srpc::frame_codec_write_header(
            frame_header_bytes, INT32_MAX, true) ||
        srpc::frame_codec_write_header(
            frame_header_bytes, srpc::kMaxFramePayloadSize + 1, true) ||
        !srpc::frame_codec_write_header(
            frame_header_bytes, srpc::kMaxFramePayloadSize, true) ||
        std::memcmp(frame_header_bytes.data(),
                    frame_native_bytes(srpc::encode_response_size(
                        srpc::kMaxFramePayloadSize, true))
                        .data(),
                    4) != 0) {
        return 198;
    }

    srpc::FrameHeader decoded_frame_header{17, true};
    if (srpc::frame_codec_peek_header(
            std::span<const std::uint8_t>(frame_header_bytes.data(), 3),
            decoded_frame_header) != srpc::FrameDecodeStatus::NeedMoreBytes ||
        decoded_frame_header.payload_size != 17 ||
        !decoded_frame_header.extended_header_flag) {
        return 199;
    }
    for (const auto [encoded, payload, extended] :
         std::array{
             std::tuple{0, 0, false},
             std::tuple{srpc::encode_response_size(
                            srpc::kMaxFramePayloadSize, false),
                        srpc::kMaxFramePayloadSize, false},
             std::tuple{INT32_MIN, 0, true},
             std::tuple{srpc::encode_response_size(
                            srpc::kMaxFramePayloadSize, true),
                        srpc::kMaxFramePayloadSize, true},
         }) {
        const auto bytes = frame_native_bytes(encoded);
        decoded_frame_header = srpc::FrameHeader{-7, !extended};
        if (srpc::frame_codec_peek_header(bytes, decoded_frame_header) !=
                srpc::FrameDecodeStatus::Complete ||
            decoded_frame_header.payload_size != payload ||
            decoded_frame_header.extended_header_flag != extended) {
            return 200;
        }
    }
    // Was `!= INT32_MIN + 3`, which PINNED the wrapping overflow as correct.
    // Every caller casts this to size_t, and casting a negative int32_t
    // sign-extends: a wrapped -2147483645 becomes 18446744071562067971, so
    // the "do I have the whole frame yet?" guard is true forever and the
    // stream wedges silently. It must saturate, never wrap.
    if (srpc::FrameHeader{INT32_MAX, false}.total_frame_size() < 0 ||
        srpc::FrameHeader{srpc::kMaxFramePayloadSize, false}
                .total_frame_size() !=
            srpc::kMaxFramePayloadSize +
                static_cast<std::int32_t>(srpc::kFrameHeaderSize)) {
        return 201;
    }
    // A desynchronised read must be REJECTED, not accepted as a valid header
    // claiming a payload that will never arrive. An all-ones word is the
    // canonical shape of one.
    {
        srpc::FrameHeader desync_header{0, false};
        if (srpc::frame_codec_peek_header(frame_native_bytes(-1),
                                          desync_header) !=
                srpc::FrameDecodeStatus::Malformed ||
            srpc::frame_codec_peek_header(
                frame_native_bytes(srpc::encode_response_size(
                    srpc::kMaxFramePayloadSize + 1, false)),
                desync_header) != srpc::FrameDecodeStatus::Malformed) {
            return 202;
        }
    }

    std::vector<std::uint8_t> encoded_frame{9, 8};
    const auto untouched_frame = encoded_frame;
    if (srpc::frame_codec_encode_into(
            encoded_frame, nullptr, -1, false) ||
        encoded_frame != untouched_frame ||
        srpc::frame_codec_encode_into(
            encoded_frame, nullptr, 1, false) ||
        encoded_frame != untouched_frame ||
        !srpc::frame_codec_encode_into(
            encoded_frame, nullptr, 0, false) ||
        encoded_frame.size() != 6) {
        return 202;
    }
    constexpr std::array<std::uint8_t, 3> first_frame_payload{'a', 'b', 'c'};
    std::vector<std::uint8_t> first_frame;
    if (!srpc::frame_codec_encode_into(
            first_frame,
            first_frame_payload.data(),
            static_cast<std::int32_t>(first_frame_payload.size()),
            false)) {
        return 203;
    }

    auto frame_reader = srpc::FrameStreamReader::new_();
    frame_reader.cursor_.set_position(99);
    srpc::FrameView frame_view{
        srpc::FrameHeader{91, true},
        reinterpret_cast<const std::uint8_t*>(1),
        77,
    };
    if (frame_reader.next_frame(frame_view) !=
            srpc::FrameDecodeStatus::NeedMoreBytes ||
        frame_reader.buffered_bytes() != 0) {
        return 204;
    }
    frame_reader.consume_frame();
    if (frame_reader.cursor_.position() != 99) {
        return 205;
    }
    frame_reader.reset();
    for (std::size_t index = 0; index < first_frame.size(); ++index) {
        frame_reader.append(&first_frame[index], 1);
        const auto status = frame_reader.next_frame(frame_view);
        if ((index + 1 < first_frame.size() &&
             status != srpc::FrameDecodeStatus::NeedMoreBytes) ||
            (index + 1 == first_frame.size() &&
             status != srpc::FrameDecodeStatus::Complete)) {
            return 206;
        }
    }
    if (frame_view.payload_size != first_frame_payload.size() ||
        std::memcmp(frame_view.payload,
                    first_frame_payload.data(),
                    first_frame_payload.size()) != 0) {
        return 207;
    }
    frame_reader.consume_frame();

    constexpr std::array<std::uint8_t, 2> second_frame_payload{0x55, 0xaa};
    std::vector<std::uint8_t> second_frame;
    if (!srpc::frame_codec_encode_into(
            second_frame,
            second_frame_payload.data(),
            static_cast<std::int32_t>(second_frame_payload.size()),
            true)) {
        return 208;
    }
    std::vector<std::uint8_t> coalesced_frames = first_frame;
    coalesced_frames.insert(
        coalesced_frames.end(), second_frame.begin(), second_frame.end());
    frame_reader.append(coalesced_frames.data(), coalesced_frames.size());
    if (frame_reader.next_frame(frame_view) !=
        srpc::FrameDecodeStatus::Complete) {
        return 209;
    }
    frame_reader.consume_frame();
    if (frame_reader.buffered_bytes() != second_frame.size() ||
        frame_reader.next_frame(frame_view) !=
            srpc::FrameDecodeStatus::Complete ||
        !frame_view.header.extended_header_flag ||
        std::memcmp(frame_view.payload,
                    second_frame_payload.data(),
                    second_frame_payload.size()) != 0) {
        return 210;
    }
    frame_reader.consume_frame();

    constexpr std::size_t compact_total = 64 * 1024;
    std::vector<std::uint8_t> compact_payload(compact_total - 4);
    for (std::size_t index = 0; index < compact_payload.size(); ++index) {
        compact_payload[index] = static_cast<std::uint8_t>((index * 17) & 0xff);
    }
    std::vector<std::uint8_t> compact_frame;
    if (!srpc::frame_codec_encode_into(
            compact_frame,
            compact_payload.data(),
            static_cast<std::int32_t>(compact_payload.size()),
            false)) {
        return 211;
    }
    compact_frame.insert(
        compact_frame.end(), second_frame.begin(), second_frame.end());
    frame_reader.append(compact_frame.data(), compact_frame.size());
    if (frame_reader.next_frame(frame_view) !=
        srpc::FrameDecodeStatus::Complete) {
        return 212;
    }
    frame_reader.consume_frame();
    if (frame_reader.cursor_.position() != 0 ||
        frame_reader.cursor_.get_ref().size() != second_frame.size() ||
        frame_reader.next_frame(frame_view) !=
            srpc::FrameDecodeStatus::Complete ||
        std::memcmp(frame_view.payload,
                    second_frame_payload.data(),
                    second_frame_payload.size()) != 0) {
        return 213;
    }


    constexpr std::array<std::int64_t, 37> sparse_boundaries{
        INT64_MIN,
        -36028797018963969LL, -36028797018963968LL,
        -36028797018963967LL, -281474976710657LL,
        -281474976710656LL, -281474976710655LL,
        -2199023255553LL, -2199023255552LL, -2199023255551LL,
        -17179869185LL, -17179869184LL, -17179869183LL,
        -134217729LL, -134217728LL, -134217727LL,
        -1048577LL, -1048576LL, -1048575LL,
        -8193LL, -8192LL, -8191LL, -65LL, -64LL, -63LL,
        -1LL, 0LL, 1LL, 62LL, 63LL, 64LL, 8191LL, 8192LL,
        1048575LL, 134217727LL, 36028797018963967LL, INT64_MAX,
    };
    for (const auto value : sparse_boundaries) {
        if (!basetypes_round_trip(value)) {
            return 142;
        }
        if (value >= INT32_MIN && value <= INT32_MAX &&
            !basetypes_round_trip(static_cast<std::int32_t>(value))) {
            return 143;
        }
    }
    std::uint64_t sparse_state = UINT64_C(0x9e3779b97f4a7c15);
    std::uint64_t sparse_wire_digest = UINT64_C(0xcbf29ce484222325);
    const auto hash_sparse_byte = [](std::uint64_t hash, std::uint8_t byte) {
        return (hash ^ byte) * UINT64_C(0x100000001b3);
    };
    for (std::size_t i = 0; i < 100000; ++i) {
        sparse_state ^= sparse_state >> 12;
        sparse_state ^= sparse_state << 25;
        sparse_state ^= sparse_state >> 27;
        const auto value = static_cast<std::int64_t>(
            sparse_state * UINT64_C(0x2545f4914f6cdd1d));
        if (!basetypes_round_trip(value) ||
            !basetypes_round_trip(static_cast<std::int32_t>(value))) {
            return 144;
        }
        std::array<std::uint8_t, 9> encoded64{};
        const auto reported64 =
            srpc::SparseInt::dump64(value, encoded64.data());
        sparse_wire_digest = hash_sparse_byte(sparse_wire_digest, 64);
        sparse_wire_digest = hash_sparse_byte(
            sparse_wire_digest, static_cast<std::uint8_t>(reported64));
        const auto written64 = reported64 == 8 ? 9 : reported64;
        for (std::size_t byte = 0; byte < written64; ++byte) {
            sparse_wire_digest =
                hash_sparse_byte(sparse_wire_digest, encoded64[byte]);
        }
        std::array<std::uint8_t, 5> encoded32{};
        const auto reported32 = srpc::SparseInt::dump32(
            static_cast<std::int32_t>(value), encoded32.data());
        sparse_wire_digest = hash_sparse_byte(sparse_wire_digest, 32);
        sparse_wire_digest = hash_sparse_byte(
            sparse_wire_digest, static_cast<std::uint8_t>(reported32));
        for (std::size_t byte = 0; byte < reported32; ++byte) {
            sparse_wire_digest =
                hash_sparse_byte(sparse_wire_digest, encoded32[byte]);
        }
    }
    if (sparse_wire_digest != UINT64_C(0x6d2ddf1efe2ab0b6)) {
        return 156;
    }
    for (const auto [value, truncated] : std::array{
             std::pair{INT64_C(36028797018963967),
                       INT64_C(36028797018963712)},
             std::pair{INT64_C(-36028797018963967),
                       INT64_C(-36028797018963968)},
         }) {
        std::array<std::uint8_t, 9> encoded{};
        const auto reported = srpc::SparseInt::dump64(value, encoded.data());
        std::array<std::uint8_t, 9> persisted{};
        std::copy_n(encoded.begin(), reported, persisted.begin());
        if (reported != 8 || encoded[0] != 0xfe ||
            srpc::SparseInt::load64(persisted.data()) != truncated) {
            return 145;
        }
    }

    auto base_v32 = srpc::v32::new_(-8192);
    base_v32.set(8192);
    auto base_v64 = srpc::v64::new_(36028797018963968LL);
    if (base_v32.get() != 8192 || base_v32.val_size() != 3 ||
        base_v64.get() != 36028797018963968LL || base_v64.val_size() != 9) {
        return 146;
    }
    auto base_counter = srpc::Counter::new_(7);
    if (base_counter.peek_next() != 7 || base_counter.next(5) != 7 ||
        base_counter.peek_next() != 12) {
        return 147;
    }
    base_counter.reset(std::numeric_limits<std::int64_t>::max());
    if (base_counter.next(1) != std::numeric_limits<std::int64_t>::max() ||
        base_counter.peek_next() != std::numeric_limits<std::int64_t>::min()) {
        return 148;
    }
    auto concurrent_counter = srpc::Counter::new_(0);
    {
        std::vector<std::thread> workers;
        for (std::size_t worker = 0; worker < 8; ++worker) {
            workers.emplace_back([&concurrent_counter]() {
                for (std::size_t i = 0; i < 10000; ++i) {
                    concurrent_counter.next(1);
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
    }
    if (concurrent_counter.peek_next() != 80000) {
        return 155;
    }
    srpc::AtomicI64 exported_atomic = srpc::AtomicI64::new_(11);
    if (exported_atomic.load(srpc::Ordering::Relaxed) != 11 ||
        srpc::SRPC_USEC_PER_SEC != 1000000) {
        return 149;
    }

    monotonic_now_us = 10;
    realtime_now_us = 20;
    gettimeofday_now_us = 1000000;
    slept_us = 0;
    srpc::abort_if_false(true);
    if (srpc::time_now_us(true) != 10 || srpc::Time::now(false) != 20) {
        return 150;
    }
    srpc::Time::sleep(37);
    auto base_timer = srpc::Timer::new_();
    base_timer.start();
    gettimeofday_now_us = 3250000;
    if (slept_us != 37 || base_timer.elapsed() != 2.25) {
        return 151;
    }
    base_timer.stop();
    gettimeofday_now_us = 9000000;
    if (base_timer.elapsed() != 2.25) {
        return 152;
    }
    base_timer.begin_us = 10;
    base_timer.end_us = 5;
    if (base_timer.elapsed() !=
        static_cast<double>(std::numeric_limits<std::uint64_t>::max() - 4) /
            1000000.0) {
        return 153;
    }
    base_timer.reset();
    if (base_timer.begin_us != 0 || base_timer.end_us != 0) {
        return 154;
    }
    if (srpc::kRequestQueueRejectedError != EAGAIN ||
        srpc::kRequestQueueExpiredError != ETIMEDOUT ||
        srpc::overflow_strategy_to_string(srpc::OverflowStrategy::DROP_OLDEST) !=
            "DROP_OLDEST" ||
        srpc::overflow_strategy_to_string(srpc::OverflowStrategy::DROP_NEWEST) !=
            "DROP_NEWEST" ||
        srpc::overflow_strategy_to_string(srpc::OverflowStrategy::FAIL_FAST) !=
            "FAIL_FAST" ||
        srpc::overflow_strategy_to_string(
            static_cast<srpc::OverflowStrategy>(99)) != "UNKNOWN") {
        return 157;
    }

    const auto queue_defaults = srpc::RequestQueueConfig::defaults();
    if (queue_defaults.max_size != 1000 ||
        queue_defaults.default_ttl_ms != 30000 ||
        queue_defaults.overflow_strategy != srpc::OverflowStrategy::DROP_OLDEST ||
        !queue_defaults.enabled || srpc::RequestQueueConfig::small().max_size != 10 ||
        srpc::RequestQueueConfig::large().max_size != 10000 ||
        srpc::RequestQueueConfig::disabled().enabled) {
        return 158;
    }

    bool direct_callback_called = false;
    srpc::rq_invoke_callback_safely(
        srpc::QueuedRequestCallback([&](std::int32_t error) {
            direct_callback_called = error == 314;
        }),
        314);
    if (!direct_callback_called) {
        return 156;
    }

    monotonic_now_us = 1'000'000;
    auto timed_request = srpc::QueuedRequest::new_();
    if (srpc::queued_request_time_us() != monotonic_now_us ||
        timed_request.timestamp_us != monotonic_now_us ||
        timed_request.xid != 0 || timed_request.rpc_id != 0 ||
        timed_request.retry_count != 0 || timed_request.callback ||
        timed_request.ttl_ms != 30000) {
        return 159;
    }
    timed_request.ttl_ms = 10;
    monotonic_now_us = 1'010'000;
    if (timed_request.is_expired() || timed_request.age_ms() != 10) {
        return 160;
    }
    monotonic_now_us = 1'011'000;
    if (!timed_request.is_expired() || timed_request.age_ms() != 11) {
        return 161;
    }
    timed_request.timestamp_us = std::numeric_limits<std::uint64_t>::max() - 499;
    timed_request.ttl_ms = 0;
    monotonic_now_us = 500;
    if (!timed_request.is_expired() || timed_request.age_ms() != 1) {
        return 162;
    }

    auto fifo_config = queue_defaults;
    fifo_config.max_size = 2;
    fifo_config.default_ttl_ms = 77;
    auto fifo = srpc::RequestQueue::with_config(fifo_config);
    if (!fifo.empty() || fifo.remaining_capacity() != 2) {
        return 163;
    }
    auto first = make_queued_request(1);
    first.ttl_ms = 0;
    if (!fifo.enqueue(std::move(first)) ||
        !fifo.enqueue(make_queued_request(2)) || !fifo.full() ||
        fifo.remaining_capacity() != 0) {
        return 164;
    }
    auto first_out = fifo.dequeue();
    auto second_out = fifo.dequeue();
    if (first_out.is_none() || second_out.is_none()) {
        return 165;
    }
    auto first_value = first_out.unwrap();
    auto second_value = second_out.unwrap();
    if (first_value.xid != 1 || first_value.ttl_ms != 77 ||
        second_value.xid != 2 || fifo.dequeue().is_some()) {
        return 165;
    }
    fifo.update_config(srpc::RequestQueueConfig::small());
    if (fifo.config().max_size != 10 || !fifo.enabled() || fifo.max_size() != 10) {
        return 166;
    }

    for (auto strategy : {srpc::OverflowStrategy::DROP_NEWEST,
                          srpc::OverflowStrategy::FAIL_FAST}) {
        auto config = queue_defaults;
        config.max_size = 1;
        config.overflow_strategy = strategy;
        auto queue = srpc::RequestQueue::with_config(config);
        if (!queue.enqueue(make_queued_request(3))) {
            return 167;
        }
        bool called = false;
        auto rejected = make_queued_request(
            4,
            srpc::QueuedRequestCallback([&](std::int32_t error) {
                if (error != srpc::kRequestQueueRejectedError ||
                    queue.queue_.try_lock().is_some()) {
                    throw std::logic_error("rejection callback lock contract");
                }
                called = true;
                throw std::runtime_error("expected rejection callback exception");
            }));
        if (queue.enqueue(std::move(rejected)) || !called || queue.size() != 1) {
            return 168;
        }
    }

    auto oldest_config = queue_defaults;
    oldest_config.max_size = 1;
    auto oldest_queue = srpc::RequestQueue::with_config(oldest_config);
    bool oldest_called = false;
    auto oldest = make_queued_request(
        5,
        srpc::QueuedRequestCallback([&](std::int32_t error) {
            if (error != srpc::kRequestQueueRejectedError ||
                oldest_queue.queue_.try_lock().is_some()) {
                throw std::logic_error("oldest callback lock contract");
            }
            oldest_called = true;
            throw std::runtime_error("expected oldest callback exception");
        }));
    if (!oldest_queue.enqueue(std::move(oldest)) ||
        !oldest_queue.enqueue(make_queued_request(6)) || !oldest_called) {
        return 169;
    }
    auto retained = oldest_queue.dequeue();
    if (retained.is_none() || retained.unwrap().xid != 6) {
        return 170;
    }

    auto disabled_queue = srpc::RequestQueue::with_config(srpc::RequestQueueConfig::disabled());
    bool disabled_called = false;
    auto disabled_request = make_queued_request(
        7,
        srpc::QueuedRequestCallback([&](std::int32_t error) {
            if (error != srpc::kRequestQueueRejectedError ||
                disabled_queue.queue_.try_lock().is_none()) {
                throw std::logic_error("disabled callback lock contract");
            }
            disabled_called = true;
            throw std::runtime_error("expected disabled callback exception");
        }));
    if (disabled_queue.enqueue(std::move(disabled_request)) ||
        !disabled_called || !disabled_queue.empty()) {
        return 171;
    }

    monotonic_now_us = 2'000'000;
    auto expiring = srpc::RequestQueue::new_();
    std::vector<std::int64_t> expired_order;
    for (std::int64_t xid : {8, 9}) {
        auto request = make_queued_request(
            xid,
            srpc::QueuedRequestCallback([&, xid](std::int32_t error) {
                if (error != srpc::kRequestQueueExpiredError ||
                    expiring.queue_.try_lock().is_none()) {
                    throw std::logic_error("expiration callback lock contract");
                }
                expired_order.push_back(xid);
                if (xid == 8) {
                    throw std::runtime_error("expected expiration callback exception");
                }
            }));
        request.timestamp_us = monotonic_now_us - 2'000;
        request.ttl_ms = 1;
        if (!expiring.enqueue(std::move(request))) {
            return 172;
        }
    }
    auto live = make_queued_request(10);
    live.timestamp_us = monotonic_now_us - 1'000;
    live.ttl_ms = 1;
    if (!expiring.enqueue(std::move(live)) || expiring.expire_stale() != 2 ||
        expired_order != std::vector<std::int64_t>({8, 9}) ||
        expiring.size() != 1) {
        return 173;
    }

    auto clearing = srpc::RequestQueue::new_();
    std::vector<std::int64_t> cleared_order;
    for (std::int64_t xid : {11, 12}) {
        auto request = make_queued_request(
            xid,
            srpc::QueuedRequestCallback([&, xid](std::int32_t error) {
                if (error != -77 || clearing.queue_.try_lock().is_none()) {
                    throw std::logic_error("clear callback lock contract");
                }
                cleared_order.push_back(xid);
                if (xid == 11) {
                    throw std::runtime_error("expected clear callback exception");
                }
            }));
        if (!clearing.enqueue(std::move(request))) {
            return 174;
        }
    }
    clearing.clear_all(-77);
    if (cleared_order != std::vector<std::int64_t>({11, 12}) ||
        !clearing.empty()) {
        return 175;
    }

    auto invalid_config = queue_defaults;
    invalid_config.max_size = 0;
    invalid_config.overflow_strategy = static_cast<srpc::OverflowStrategy>(99);
    auto invalid_queue = srpc::RequestQueue::with_config(invalid_config);
    if (!invalid_queue.enqueue(make_queued_request(13)) ||
        invalid_queue.size() != 1) {
        return 176;
    }

    // srpc.debugging: branch hints keep boolean identity, verify() only reaches
    // the failure tail on a false predicate, and the failure tail renders the
    // captured frames BEFORE it panics (the trace must survive a swallowed
    // panic). `frames - 1` is the historical loop bound, so a three-frame
    // capture renders two lines.
    if (!srpc::likely(true) || srpc::likely(false) ||
        !srpc::unlikely(true) || srpc::unlikely(false)) {
        return 177;
    }
    reset_debugging(3);
    srpc::verify(true);
    srpc::verify(reinterpret_cast<const void*>(1));
    srpc::verify(7);
    if (debugging_capture_calls != 0 || !debugging_rendered().empty()) {
        return 178;
    }

    reset_debugging(3);
    bool debugging_panicked = false;
    try {
        srpc::verify(false);
    } catch (const std::exception& error) {
        debugging_panicked =
            std::string_view(error.what()).find("verify failed at ") !=
            std::string_view::npos;
    }
    if (!debugging_panicked || debugging_capture_calls != 1 ||
        debugging_free_calls != 1) {
        return 179;
    }
    if (debugging_rendered() !=
        "  *** begin stack trace ***\\n"
        "0    frame-zero\\n"
        "1    frame-one\\n"
        "  ***  end stack trace  ***\\n") {
        return 180;
    }

    reset_debugging(-1);
    debugging_panicked = false;
    try {
        srpc::verify_failed("goal0.cc", 42);
    } catch (const std::exception& error) {
        debugging_panicked =
            std::string_view(error.what()) == "verify failed at goal0.cc, line 42";
    }
    if (!debugging_panicked || debugging_capture_calls != 1 ||
        debugging_free_calls != 0) {
        return 181;
    }
    if (debugging_rendered() != "  *** failed to obtain stack trace!\\n") {
        return 182;
    }

    reset_debugging(3);
    srpc::print_stack_trace(debugging_stream);
    if (debugging_rendered() !=
        "  *** begin stack trace ***\\n"
        "0    frame-zero\\n"
        "1    frame-one\\n"
        "  ***  end stack trace  ***\\n") {
        return 183;
    }
    return 0;
}
"""


def compile_module(
    clang: Path,
    root: Path,
    include: Path,
    source_dir: Path,
    work_dir: Path,
    module_name: str,
    cxx_flags: list[str],
    prebuilt_module_dirs: list[Path],
) -> Path:
    source = source_dir / f"{module_name}.cppm"
    pcm = work_dir / f"{module_name}.pcm"
    object_file = work_dir / f"{module_name}.o"
    module_path_flags = [
        f"-fprebuilt-module-path={path}"
        for path in (work_dir, *prebuilt_module_dirs)
    ]
    run(
        [
            str(clang),
            "-std=gnu++23",
            *cxx_flags,
            "-Wno-deprecated-declarations",
            "-I",
            str(include),
            "-I",
            str(root / "src/srpc"),
            *module_path_flags,
            "--precompile",
            str(source),
            "-o",
            str(pcm),
        ],
        root,
    )
    run(
        [
            str(clang),
            "-std=gnu++23",
            *cxx_flags,
            "-I",
            str(root / "src/srpc"),
            *module_path_flags,
            "-c",
            str(pcm),
            "-o",
            str(object_file),
        ],
        root,
    )
    return object_file


# Plain-C kernels compiled into libsrpc.a (see the "Goal-0 C demotion" block in
# src/srpc-cmake/CMakeLists.txt) plus srpc.epoll_wrapper's platform
# implementation unit. They are not crate outputs, so the generated lane has to
# build them itself to link the same closure production does.
# base/srpc_base.c, misc/srpc_rand.c, misc/srpc_timing.c and rpc/srpc_net.c are
# deliberately ABSENT: the importer defines its own observable stubs for the
# seams in them that the runtime contracts pin (the rand draws, the clocks,
# srpc_find_open_port, and the backtrace trio), and a C file is linked at
# object granularity -- pulling one of those objects for a symbol the importer
# does not stub would drag its stubbed neighbours in too. The handful of
# functions the modules still need from those four files are stubbed in the
# importer instead, beside the ones already there. The real kernels are covered
# separately by srpc_goal0_rand_kernel_smoke and by the production lane, which
# links libsrpc.a.
SUPPORT_C_KERNELS = (
    "src/srpc/misc/srpc_io.c",
    "src/srpc/rpc/srpc_connect.c",
    "src/srpc/rpc/srpc_server.c",
    "src/srpc/reactor/srpc_fiber.c",
)
SUPPORT_MODULE_IMPLEMENTATIONS = ("src/srpc/reactor/epoll_platform_linux.cc",)
# The fiber context-switch trampoline is real assembly, selected by host
# architecture exactly as CMake's `reactor/*.S` glob selects it.
SUPPORT_ASSEMBLY = {
    "x86_64": "src/srpc/reactor/fiber_context_x86_64.S",
    "aarch64": "src/srpc/reactor/fiber_context_aarch64.S",
    "arm64": "src/srpc/reactor/fiber_context_aarch64.S",
}


def compile_support_inputs(
    clang: Path,
    archiver: Path,
    root: Path,
    include: Path,
    work_dir: Path,
    cxx_flags: list[str],
    prebuilt_module_dirs: list[Path],
) -> list[Path]:
    objects: list[Path] = []
    module_path_flags = [
        f"-fprebuilt-module-path={path}"
        for path in (work_dir, *prebuilt_module_dirs)
    ]
    for label in SUPPORT_C_KERNELS:
        source = root / label
        object_file = work_dir / f"{Path(label).stem}.c.o"
        run(
            [
                str(clang),
                "-x",
                "c",
                "-std=gnu11",
                "-I",
                str(root / "src/srpc"),
                "-c",
                str(source),
                "-o",
                str(object_file),
            ],
            root,
        )
        objects.append(object_file)
    assembly_label = SUPPORT_ASSEMBLY.get(platform.machine())
    if assembly_label is None:
        raise GateError(
            "no fiber context-switch trampoline for host architecture "
            f"{platform.machine()!r}"
        )
    assembly_object = work_dir / "fiber_context.S.o"
    run(
        [
            str(clang),
            "-c",
            str(root / assembly_label),
            "-o",
            str(assembly_object),
        ],
        root,
    )
    objects.append(assembly_object)
    for label in SUPPORT_MODULE_IMPLEMENTATIONS:
        source = root / label
        object_file = work_dir / f"{Path(label).stem}.impl.o"
        run(
            [
                str(clang),
                "-std=gnu++23",
                *cxx_flags,
                "-I",
                str(include),
                "-I",
                str(root / "src/srpc"),
                *module_path_flags,
                "-c",
                str(source),
                "-o",
                str(object_file),
            ],
            root,
        )
        objects.append(object_file)
    # Bundle them as an ARCHIVE, not as loose objects. The importer
    # deliberately defines its own observable stubs for several of these C
    # seams (srpc_rand_raw, the clocks, srpc_find_open_port, and the debugging
    # trio) so the runtime contracts are deterministic; loose objects would be
    # unconditionally included and collide with those. An archive member is
    # pulled only for a symbol that is still undefined, so the importer's stubs
    # win and only the genuinely missing kernels come from here -- the same
    # resolution order the production lane gets from libsrpc.a.
    archive = work_dir / "libgoal0support.a"
    run(
        [
            str(archiver),
            "rcs",
            str(archive),
            *(str(path) for path in objects),
        ],
        root,
    )
    return [archive]


def grouped_link_inputs(paths: list[Path]) -> list[str]:
    rendered = [str(path) for path in paths]
    if sys.platform.startswith("linux"):
        return ["-Wl,--start-group", *rendered, "-Wl,--end-group"]
    return rendered


def resolve_file(root: Path, raw: str, description: str) -> Path:
    path = Path(raw)
    if not path.is_absolute():
        path = root / path
    path = path.resolve()
    if not path.is_file():
        raise GateError(f"{description} is unavailable: {path}")
    return path


def resolve_generated_dir(root: Path, raw: str) -> Path:
    output = Path(raw)
    if not output.is_absolute():
        output = root / output
    output = output.resolve()
    if not output.is_dir():
        raise GateError(f"generated crate directory is unavailable: {output}")
    return output


def resolve_prebuilt_module_dirs(root: Path, raw_roots: list[str]) -> list[Path]:
    directories: set[Path] = set()
    found_rusty = False
    for raw in raw_roots:
        module_root = Path(raw)
        if not module_root.is_absolute():
            module_root = root / module_root
        module_root = module_root.resolve()
        if not module_root.is_dir():
            raise GateError(
                f"runtime prebuilt-module root is unavailable: {module_root}"
            )
        for pcm in module_root.rglob("*.pcm"):
            if pcm.is_file():
                directories.add(pcm.parent.resolve())
                found_rusty = found_rusty or pcm.name == "rusty.pcm"
    if raw_roots and not found_rusty:
        raise GateError(
            "runtime prebuilt-module roots do not contain rusty.pcm"
        )
    return sorted(directories)


def crate_compile_order(
    output: Path, modules: list[extraction.ModuleEntry]
) -> list[extraction.ModuleEntry]:
    """Manifest modules reordered so every child follows the ones it imports.

    A generated child is a C++ named-module interface unit, so clang can only
    precompile it once the BMIs of the crate modules it imports already exist.
    The manifest is an OWNERSHIP record, not a build schedule: its row order
    is srpc's, and mako's rusty-cpp pin does not have to produce the same
    import graph srpc's does (`srpc.utils` imports `srpc.logging` here, and the
    two sit in the opposite relative order upstream). Deriving the schedule
    from what the generated units actually import keeps this gate correct for
    any manifest order and any pin. Manifest order remains the stable
    tie-break, so the schedule is still deterministic.
    """

    by_name = {module.cpp_module: module for module in modules}
    dependencies = {}
    for module in modules:
        text = read_generated(
            output / f"{module.cpp_module}.cppm",
            f"module {module.cpp_module}",
        )
        imported = re.findall(
            r"^(?:export )?import ([^;\n]+);[ \t]*$", text, re.MULTILINE
        )
        dependencies[module.cpp_module] = [
            name for name in imported if name in by_name
        ]

    ordered: list[extraction.ModuleEntry] = []
    placed: set[str] = set()
    visiting: list[str] = []

    def place(name: str) -> None:
        if name in placed:
            return
        if name in visiting:
            cycle = " -> ".join([*visiting[visiting.index(name) :], name])
            raise GateError(
                f"generated crate modules form an import cycle: {cycle}"
            )
        visiting.append(name)
        for dependency in dependencies[name]:
            place(dependency)
        visiting.pop()
        placed.add(name)
        ordered.append(by_name[name])

    for module in modules:
        place(module.cpp_module)
    return ordered


def check_generated_output(
    *,
    root: Path,
    output: Path,
    modules: list[extraction.ModuleEntry],
    clang: Path,
    nm: Path,
    archiver: Path,
    production: Path | None,
    runtime_libraries: list[Path],
    cxx_flags: list[str],
    link_flags: list[str],
    prebuilt_module_dirs: list[Path],
) -> None:
    require_cpp_surfaces(root, output, modules)
    require_zero_hand_slots(output / "rusty_hand_slots.md")
    include = root / "third-party/rusty-cpp/include"

    # Compilation products live outside the build-tree generation directory.
    # That directory remains a deterministic crate-output census shared by the
    # production target and this gate.
    with tempfile.TemporaryDirectory(prefix="srpc-crate-mode-compile-") as temporary:
        work = Path(temporary)
        # Compile in import order (see crate_compile_order); keep the object
        # list in manifest order so the link closure below is unchanged.
        compiled = {
            module.cpp_module: compile_module(
                clang,
                root,
                include,
                output,
                work,
                module.cpp_module,
                cxx_flags,
                prebuilt_module_dirs,
            )
            for module in crate_compile_order(output, modules)
        }
        generated_objects = [compiled[module.cpp_module] for module in modules]
        # Compile the partial umbrella only after every child BMI exists. It is
        # a syntax/import-closure proof, not a production provider or link input.
        compile_module(
            clang,
            root,
            include,
            output,
            work,
            "srpc",
            cxx_flags,
            prebuilt_module_dirs,
        )

        # libsrpc.a carries two things the generated crate object set does not:
        # the plain-C kernels the canonical Rust calls through extern "C", and
        # srpc.epoll_wrapper's platform implementation unit. Compile the same
        # inputs here so the generated lane links the identical closure rather
        # than a smaller one. This must follow the child compiles: the
        # implementation unit imports its own interface's BMI.
        support_objects = compile_support_inputs(
            clang,
            archiver,
            root,
            include,
            work,
            cxx_flags,
            prebuilt_module_dirs,
        )

        importer = work / "importer.cpp"
        importer_object = work / "importer.o"
        importer.write_text(importer_source(), encoding="utf-8")
        run(
            [
                str(clang),
                "-std=gnu++23",
                *cxx_flags,
                "-I",
                str(include),
                f"-fprebuilt-module-path={work}",
                *(
                    f"-fprebuilt-module-path={path}"
                    for path in prebuilt_module_dirs
                ),
                "-c",
                str(importer),
                "-o",
                str(importer_object),
            ],
            root,
        )

        link_sets: list[tuple[str, list[Path]]] = [
            (
                "generated",
                [*generated_objects, *support_objects, *runtime_libraries],
            ),
        ]
        if production is not None:
            # The forwarding srpc.logging fixture is gone: srpc.logging is a
            # canonical Rust module now, so both lanes link the REAL provider
            # (the generated object here, libsrpc.a's copy there) and the
            # importer observes Utils' logging through std::cout instead of
            # around it. There is no longer any dependency object to order
            # ahead of the archive.
            link_sets.append(
                ("production", [production, *runtime_libraries])
            )
        for label, link_inputs in link_sets:
            executable_path = work / f"importer-{label}"
            run(
                [
                    str(clang),
                    "-std=gnu++23",
                    *cxx_flags,
                    str(importer_object),
                    *grouped_link_inputs(link_inputs),
                    *link_flags,
                    "-o",
                    str(executable_path),
                ],
                root,
            )
            run([str(executable_path)], root)

        for module, generated_object in zip(
            modules, generated_objects, strict=True
        ):
            generated_symbols = module_symbols(
                nm, root, generated_object, module.cpp_module
            )
            require_expected_symbols(
                module.cpp_module,
                "crate-generated object",
                generated_symbols,
            )

            if module.cpp_module == "srpc.completion_tracker":
                require_completion_raw_symbols(
                    "crate-generated object",
                    completion_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "srpc.rand":
                require_rand_raw_symbols(
                    "crate-generated object",
                    rand_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "srpc.request_options":
                require_request_options_raw_symbols(
                    "crate-generated object",
                    request_options_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "srpc.reconnect_policy":
                require_reconnect_policy_raw_symbols(
                    "crate-generated object",
                    reconnect_policy_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "srpc.circuit_breaker":
                require_circuit_breaker_raw_symbols(
                    "crate-generated object",
                    circuit_breaker_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "srpc.request_queue":
                require_request_queue_raw_symbols(
                    "crate-generated object",
                    request_queue_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "srpc.basetypes":
                require_basetypes_raw_symbols(
                    "crate-generated object",
                    basetypes_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "srpc.utils":
                require_utils_raw_symbols(
                    "crate-generated object",
                    utils_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module in {
                "srpc.connection_state",
                "srpc.heartbeat",
                "srpc.load_balancer",
                "srpc.frame_codec",
            }:
                require_exact_module_raw_symbols(
                    module.cpp_module,
                    "crate-generated object",
                    exact_module_raw_symbols(
                        nm, root, generated_object, module.cpp_module
                    ),
                )

            if production is not None:
                production_symbols = module_symbols(
                    nm, root, production, module.cpp_module
                )
                require_expected_symbols(
                    module.cpp_module,
                    "production library",
                    production_symbols
                    - PLATFORM_IMPL_SYMBOLS.get(
                        module.cpp_module, frozenset()
                    ),
                )
                platform = PLATFORM_IMPL_SYMBOLS.get(
                    module.cpp_module, frozenset()
                )
                if production_symbols != generated_symbols | platform:
                    raise GateError(
                        f"production {module.cpp_module} ABI differs from "
                        "the independently compiled generated-object ABI "
                        "plus its allowlisted platform implementation unit"
                    )
                if module.cpp_module == "srpc.completion_tracker":
                    require_completion_raw_symbols(
                        "production library",
                        completion_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "srpc.rand":
                    require_rand_raw_symbols(
                        "production library",
                        rand_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "srpc.request_options":
                    require_request_options_raw_symbols(
                        "production library",
                        request_options_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "srpc.reconnect_policy":
                    require_reconnect_policy_raw_symbols(
                        "production library",
                        reconnect_policy_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "srpc.circuit_breaker":
                    require_circuit_breaker_raw_symbols(
                        "production library",
                        circuit_breaker_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "srpc.request_queue":
                    require_request_queue_raw_symbols(
                        "production library",
                        request_queue_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "srpc.basetypes":
                    require_basetypes_raw_symbols(
                        "production library",
                        basetypes_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "srpc.utils":
                    require_utils_raw_symbols(
                        "production library",
                        utils_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module in {
                    "srpc.connection_state",
                    "srpc.heartbeat",
                    "srpc.load_balancer",
                    "srpc.frame_codec",
                }:
                    require_exact_module_raw_symbols(
                        module.cpp_module,
                        "production library",
                        exact_module_raw_symbols(
                            nm, root, production, module.cpp_module
                        ),
                    )


def check(args: argparse.Namespace) -> None:
    root = repository_root()
    # Crate mode validates local runtime-provided dependencies relative to the
    # manifest path it receives. Always hand crate mode a root-resolved
    # absolute manifest so its dependency walk is independent of this gate's
    # working directory.
    crate_manifest = (root / "src/srpc/Cargo.toml").resolve()
    transpiler = executable(root, args.transpiler, "rusty-cpp transpiler")
    verify_pinned_toolchain(root, transpiler)
    require_extraction_check(root, transpiler)
    modules = load_owned_modules(root)
    clang = executable(root, args.clang, "Clang C++ compiler")
    nm = executable(root, args.nm, "nm")
    # llvm-ar comes from the same toolchain as the configured nm; the gate
    # needs it to bundle the non-crate support objects as an archive (see
    # compile_support_inputs).
    archiver = resolve_archiver(root, Path(nm))
    production_raw = getattr(args, "production_library", None)
    production = (
        resolve_file(root, production_raw, "production library")
        if production_raw
        else None
    )
    runtime_libraries = [
        resolve_file(root, raw, "runtime library")
        for raw in (getattr(args, "runtime_library", None) or [])
    ]
    cxx_flags = list(getattr(args, "cxx_flag", None) or [])
    link_flags = list(getattr(args, "link_flag", None) or [])
    prebuilt_module_dirs = resolve_prebuilt_module_dirs(
        root, list(getattr(args, "runtime_module_root", None) or [])
    )

    generated_raw = getattr(args, "generated_dir", None)
    if generated_raw:
        output = resolve_generated_dir(root, generated_raw)
        check_generated_output(
            root=root,
            output=output,
            modules=modules,
            clang=clang,
            nm=nm,
            archiver=archiver,
            production=production,
            runtime_libraries=runtime_libraries,
            cxx_flags=cxx_flags,
            link_flags=link_flags,
            prebuilt_module_dirs=prebuilt_module_dirs,
        )
    else:
        crate = extraction.crate_root(root)
        flat_import_namespace = extraction.load_flat_import_namespace(
            crate, crate / EXTRACTION_MANIFEST
        )
        flat_import_arguments = (
            ["--flat-import-namespace", flat_import_namespace]
            if flat_import_namespace is not None
            else []
        )
        with tempfile.TemporaryDirectory(prefix="srpc-crate-mode-") as temporary:
            output = Path(temporary)
            run(
                [
                    str(transpiler),
                    "--crate",
                    str(crate_manifest),
                    "--output-dir",
                    str(output),
                    "--cxx-namespace",
                    "srpc",
                    *flat_import_arguments,
                    "--module-preamble",
                    str(root / MODULE_PREAMBLE),
                    "--type-map",
                    str(root / TYPE_MAP),
                    "--cpp-module-index",
                    str(root / CPP_MODULE_INDEX),
                ],
                root,
            )
            check_generated_output(
                root=root,
                output=output,
                modules=modules,
                clang=clang,
                nm=nm,
                archiver=archiver,
                production=production,
                runtime_libraries=runtime_libraries,
                cxx_flags=cxx_flags,
                link_flags=link_flags,
                prebuilt_module_dirs=prebuilt_module_dirs,
            )

    symbol_count = sum(len(spec.symbols) for spec in ABI_SPECS.values())
    production_label = " and production library" if production is not None else ""
    print(
        f"checked whole srpc crate ({len(modules) + 1} modules compiled, "
        "partial root compile-only, 0 hand slots), combined importer against generated "
        f"objects{production_label}, "
        "CallbackWrapper C++ layout/runtime/move parity, AvgStat layout/runtime, "
        "RpcError runtime contracts, ConnectionMetrics layout/concurrent/wrapping "
        "runtime contracts, CompletionTracker C++ layout/thread-safe lifecycle/"
        "wrapping runtime contracts, RandomGenerator byte-adapter/single-evaluation/"
        "precondition/wrapping/empty-weight/C-FFI runtime contracts, "
        "RequestOptions layout/factory/retry/timeout/jitter runtime contracts, "
        "ReconnectPolicy layout/factory/backoff/retry/jitter runtime contracts, and "
        "CircuitBreaker layout/factory/state/timeout/wrapping runtime contracts, "
        "RequestQueue layout/FIFO/config/overflow/expiry/callback-isolation/"
        "wrapping runtime contracts, "
        "Basetypes aliases/layout/sparse-wire/atomic/timing runtime contracts, "
        "ConnectionState layout/empty-callback/transition runtime contracts, and "
        "Heartbeat layout/empty-and-moved-callback/timing/timeout/wrapping runtime "
        "contracts, LoadBalancer layout/strategy/selection/wrapping runtime "
        "contracts, Utils layout/move/teardown/port/hostname/logging runtime "
        "contracts, FrameCodec layout/wire/fragmentation/compaction/wrapping runtime "
        "contracts, Debugging branch-hint/verify/backtrace-render/panic-tail "
        "runtime contracts, and "
        f"{symbol_count} exact provider-owned strong ABI symbols"
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--production-library",
        help="production libsrpc archive to link/run and compare with direct generated objects",
    )
    parser.add_argument(
        "--generated-dir",
        help=(
            "pre-generated rusty-cpp crate output directory; when omitted, "
            "the gate generates into a temporary directory"
        ),
    )
    parser.add_argument(
        "--runtime-library",
        action="append",
        default=[],
        help=(
            "support archive appended to every link lane; may be repeated"
        ),
    )
    parser.add_argument(
        "--runtime-module-root",
        action="append",
        default=[],
        help=(
            "tree containing the configured rusty-cpp .pcm files needed by "
            "crate modules that import rusty; may be repeated"
        ),
    )
    parser.add_argument(
        "--cxx-flag",
        action="append",
        default=[],
        help="compiler-driver flag used for module compilation and linking",
    )
    parser.add_argument(
        "--link-flag",
        action="append",
        default=[],
        help="additional flag appended to every link command",
    )
    parser.add_argument(
        "--transpiler",
        default=os.environ.get("RUSTY_CPP_TRANSPILER", DEFAULT_TRANSPILER),
    )
    parser.add_argument("--clang", default=os.environ.get("CXX", "clang++"))
    parser.add_argument("--nm", default=os.environ.get("NM", "nm"))
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    try:
        check(parse_args(argv))
    except GateError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
