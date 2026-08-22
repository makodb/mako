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

import extract_rrr_rust as extraction


DEFAULT_TRANSPILER = (
    "third-party/rusty-cpp/target/release/rusty-cpp-transpiler"
)
RUSTY_CPP_SUBMODULE = "third-party/rusty-cpp"
REQUIRED_RUSTY_CPP_COMMIT = "a1f8fef85e8d43bb00f85f8ef32e5ecc69408642"
EXTRACTION_DRIVER = "scripts/extract_rrr_rust.py"
EXTRACTION_MANIFEST = "src/rrr/rust-modules.toml"
MODULE_PREAMBLE = "src/rrr/module-preambles.toml"
TYPE_MAP = "src/rrr/rust-type-map.toml"
CPP_MODULE_INDEX = "src/rrr/cpp-module-index.toml"
NM_LINE = re.compile(r"^[0-9A-Fa-f]+\s+([A-Za-z])\s+(.+)$")
PLACEHOLDER = re.compile(r"\b(?:TODO|UNSUPPORTED|skipped)\b", re.IGNORECASE)

# A compiler diagnostic comment is not an unimplemented user lowering.
# rusty-cpp emits this exact informational marker when it breaks a by-value
# type cycle while ordering emitted declarations; the affected types are still
# fully defined and the module still compiles. rrr.tcp_channel is the live
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
    "rrr.callback_wrapper": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.callback_wrapper;",
                "namespace rrr {",
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
    "rrr.internal_protocol": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.internal_protocol;",
                "namespace rrr {",
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
                ("R", "rrr::kInternalHeartbeatRpcId@rrr.internal_protocol"),
                ("R", "rrr::kResponseHeaderExtFlag@rrr.internal_protocol"),
                ("R", "rrr::kResponseSizeMask@rrr.internal_protocol"),
                (
                    "T",
                    "rrr::encode_response_size@rrr.internal_protocol(int, bool)",
                ),
                (
                    "T",
                    "rrr::response_has_extended_header@rrr.internal_protocol(int)",
                ),
                (
                    "T",
                    "rrr::response_payload_size@rrr.internal_protocol(int)",
                ),
            }
        ),
    ),
    "rrr.stat": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.stat;",
                "namespace rrr {",
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
                ("T", "rrr::AvgStat@rrr.stat::new_()"),
                ("T", "rrr::AvgStat@rrr.stat::sample(long)"),
                ("T", "rrr::AvgStat@rrr.stat::clear()"),
                ("T", "rrr::AvgStat@rrr.stat::reset()"),
                ("T", "rrr::AvgStat@rrr.stat::peek() const"),
                ("T", "rrr::AvgStat@rrr.stat::avg() const"),
            }
        ),
    ),
    "rrr.errors": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.errors;",
                "namespace rrr {",
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
                    "rrr::get_error_category@rrr.errors(rrr::RpcError@rrr.errors)",
                ),
                (
                    "T",
                    "rrr::is_connection_error@rrr.errors(rrr::RpcError@rrr.errors)",
                ),
                (
                    "T",
                    "rrr::is_retryable_error@rrr.errors(rrr::RpcError@rrr.errors)",
                ),
                (
                    "T",
                    "rrr::is_timeout_error@rrr.errors(rrr::RpcError@rrr.errors)",
                ),
                (
                    "T",
                    "rrr::rpc_error_category_to_string@rrr.errors(rrr::RpcErrorCategory@rrr.errors)",
                ),
                (
                    "T",
                    "rrr::rpc_error_to_string@rrr.errors(rrr::RpcError@rrr.errors)",
                ),
            }
        ),
    ),
    "rrr.connection_metrics": AbiSpec(
        surface=frozenset(
            {
                "#include <rusty/sync/atomic.hpp>",
                "export module rrr.connection_metrics;",
                "namespace rrr {",
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
                "rrr::ConnectionMetrics@rrr.connection_metrics::new_()",
                "rrr::ConnectionMetrics@rrr.connection_metrics::requests_sent() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::requests_completed() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::requests_failed() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::requests_timed_out() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::in_flight_requests() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::bytes_sent() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::bytes_received() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::reconnect_count() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::retry_attempts() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::queue_dropped_requests() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::circuit_open_rejections() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::circuit_open_transitions() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::circuit_half_open_transitions() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::circuit_closed_transitions() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::connect_time_ms() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::min_latency_us() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::max_latency_us() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::success_rate_percent() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::avg_latency_us() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::uptime_ms(unsigned long) const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_sent() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_completed_with_latency(unsigned long) const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_completed() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_failed() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_timeout() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_request_dropped() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_bytes_sent(unsigned long) const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_bytes_received(unsigned long) const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_reconnect() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_retry_attempt() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_queue_drop() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_circuit_open_rejection() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_circuit_open_transition() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_circuit_half_open_transition() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_circuit_closed_transition() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::record_connect(unsigned long) const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::reset() const",
                "rrr::ConnectionMetrics@rrr.connection_metrics::decrement_in_flight() const",
            }
        ),
    ),
    "rrr.completion_tracker": AbiSpec(
        surface=frozenset(
            {
                "#include <rusty/sync/atomic.hpp>",
                "export module rrr.completion_tracker;",
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
                "rrr::CompletionTrackerConfig@rrr.completion_tracker::new_()",
                "rrr::CompletionTrackerConfig@rrr.completion_tracker::defaults()",
                "rrr::CompletionTrackerConfig@rrr.completion_tracker::small()",
                "rrr::CompletionTrackerConfig@rrr.completion_tracker::large()",
                "rrr::CompletionTrackerConfig@rrr.completion_tracker::disabled()",
                "rrr::CompletedEntry@rrr.completion_tracker::new_(long, unsigned long)",
                "rrr::CompletedEntry@rrr.completion_tracker::is_expired(unsigned long, unsigned long) const",
                "rrr::CompletionTracker@rrr.completion_tracker::new_()",
                "rrr::CompletionTracker@rrr.completion_tracker::with_config(rrr::CompletionTrackerConfig@rrr.completion_tracker)",
                "rrr::CompletionTracker@rrr.completion_tracker::enabled() const",
                "rrr::CompletionTracker@rrr.completion_tracker::config() const",
                "rrr::CompletionTracker@rrr.completion_tracker::set_config(rrr::CompletionTrackerConfig@rrr.completion_tracker)",
                "rrr::CompletionTracker@rrr.completion_tracker::mark_completed(long, unsigned long)",
                "rrr::CompletionTracker@rrr.completion_tracker::is_completed(long, unsigned long)",
                "rrr::CompletionTracker@rrr.completion_tracker::remove(long)",
                "rrr::CompletionTracker@rrr.completion_tracker::clear()",
                "rrr::CompletionTracker@rrr.completion_tracker::size() const",
                "rrr::CompletionTracker@rrr.completion_tracker::total_tracked() const",
                "rrr::CompletionTracker@rrr.completion_tracker::queries() const",
                "rrr::CompletionTracker@rrr.completion_tracker::query_hits() const",
                "rrr::CompletionTracker@rrr.completion_tracker::hit_rate() const",
                "rrr::CompletionTracker@rrr.completion_tracker::evictions() const",
                "rrr::CompletionTracker@rrr.completion_tracker::reset_stats()",
                "rrr::CompletionTracker@rrr.completion_tracker::evict_expired(unsigned long)",
                "rrr::CompletionQueryResult@rrr.completion_tracker::new_()",
                "rrr::CompletionQueryResult@rrr.completion_tracker::not_found()",
                "rrr::CompletionQueryResult@rrr.completion_tracker::completed(int, bool)",
                "rrr::CompletionQueryResult@rrr.completion_tracker::expired()",
                "rrr::CompletionQueryResult@rrr.completion_tracker::is_completed() const",
                "rrr::completion_status_to_string@rrr.completion_tracker(rrr::CompletionStatus@rrr.completion_tracker)",
            }
        ),
    ),
    "rrr.rand": AbiSpec(
        surface=frozenset(
            {
                '#include "misc/srpc_rand.h"',
                "export module rrr.rand;",
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
                ("T", "rrr::randgen_rand_max@rrr.rand()"),
                (
                    "T",
                    "rrr::randgen_zero_pad@rrr.rand(std::__1::basic_string<char, "
                    "std::__1::char_traits<char>, std::__1::allocator<char>>, int)",
                ),
                ("T", "rrr::randgen_rand_raw@rrr.rand()"),
                ("T", "rrr::randgen_nu_constant_now@rrr.rand()"),
                ("T", "rrr::randgen_destroy@rrr.rand()"),
                ("T", "rrr::RandomGenerator@rrr.rand::rand(int, int)"),
                (
                    "T",
                    "rrr::RandomGenerator@rrr.rand::rand_double(double, double)",
                ),
                (
                    "T",
                    "rrr::RandomGenerator@rrr.rand::int2str_n(int, int)",
                ),
                (
                    "T",
                    "rrr::RandomGenerator@rrr.rand::percentage_true(int)",
                ),
                (
                    "T",
                    "rrr::RandomGenerator@rrr.rand::nu_rand(int, int, int)",
                ),
                (
                    "T",
                    "rrr::RandomGenerator@rrr.rand::weighted_select("
                    "std::__1::vector<double, std::__1::allocator<double>> const&)",
                ),
                ("T", "rrr::RandomGenerator@rrr.rand::destroy()"),
            }
        ),
    ),
    "rrr.request_options": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.request_options;",
                "import rrr.rand;",
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
                "rrr::RequestOptions@rrr.request_options::new_()",
                "rrr::RequestOptions@rrr.request_options::defaults()",
                "rrr::RequestOptions@rrr.request_options::with_retry(unsigned short, unsigned long)",
                "rrr::RequestOptions@rrr.request_options::idempotent_retry(unsigned short)",
                "rrr::RequestOptions@rrr.request_options::no_timeout()",
                "rrr::RequestOptions@rrr.request_options::fast()",
                "rrr::RequestOptions@rrr.request_options::patient()",
                "rrr::RequestOptions@rrr.request_options::can_retry(unsigned short) const",
                "rrr::RequestOptions@rrr.request_options::calculate_delay_ms(unsigned short) const",
                "rrr::RequestOptions@rrr.request_options::is_total_timeout_exceeded(unsigned long) const",
                "rrr::RequestOptions@rrr.request_options::remaining_time_ms(unsigned long) const",
                "rrr::timeout_type_to_string@rrr.request_options(rrr::TimeoutType@rrr.request_options)",
            }
        ),
    ),
    "rrr.reconnect_policy": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.reconnect_policy;",
                "import rrr.rand;",
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
                "rrr::ReconnectPolicy@rrr.reconnect_policy::new_()",
                "rrr::ReconnectPolicy@rrr.reconnect_policy::aggressive()",
                "rrr::ReconnectPolicy@rrr.reconnect_policy::conservative()",
                "rrr::ReconnectPolicy@rrr.reconnect_policy::no_retry()",
                "rrr::ReconnectCalculator@rrr.reconnect_policy::new_(rrr::ReconnectPolicy@rrr.reconnect_policy const&)",
                "rrr::ReconnectCalculator@rrr.reconnect_policy::should_retry() const",
                "rrr::ReconnectCalculator@rrr.reconnect_policy::next_delay_ms() const",
                "rrr::ReconnectCalculator@rrr.reconnect_policy::peek_delay_ms() const",
                "rrr::ReconnectCalculator@rrr.reconnect_policy::reset() const",
                "rrr::ReconnectCalculator@rrr.reconnect_policy::retry_count() const",
                "rrr::ReconnectCalculator@rrr.reconnect_policy::retries_exhausted() const",
            }
        ),
    ),
    "rrr.circuit_breaker": AbiSpec(
        surface=frozenset(
            {
                '#include "misc/srpc_timing.h"',
                "export module rrr.circuit_breaker;",
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
                "rrr::current_time_us@rrr.circuit_breaker()",
                "rrr::circuit_state_to_string@rrr.circuit_breaker(rrr::CircuitState@rrr.circuit_breaker)",
                "rrr::CircuitBreakerConfig@rrr.circuit_breaker::new_()",
                "rrr::CircuitBreakerConfig@rrr.circuit_breaker::defaults()",
                "rrr::CircuitBreakerConfig@rrr.circuit_breaker::sensitive()",
                "rrr::CircuitBreakerConfig@rrr.circuit_breaker::relaxed()",
                "rrr::CircuitBreakerConfig@rrr.circuit_breaker::disabled()",
                "rrr::CircuitBreaker@rrr.circuit_breaker::new_(rrr::CircuitBreakerConfig@rrr.circuit_breaker)",
                "rrr::CircuitBreaker@rrr.circuit_breaker::set_config(rrr::CircuitBreakerConfig@rrr.circuit_breaker) const",
                "rrr::CircuitBreaker@rrr.circuit_breaker::allow_request() const",
                "rrr::CircuitBreaker@rrr.circuit_breaker::record_success() const",
                "rrr::CircuitBreaker@rrr.circuit_breaker::record_failure() const",
                "rrr::CircuitBreaker@rrr.circuit_breaker::state() const",
                "rrr::CircuitBreaker@rrr.circuit_breaker::is_open() const",
                "rrr::CircuitBreaker@rrr.circuit_breaker::is_closed() const",
                "rrr::CircuitBreaker@rrr.circuit_breaker::is_half_open() const",
                "rrr::CircuitBreaker@rrr.circuit_breaker::reset() const",
                "rrr::CircuitBreaker@rrr.circuit_breaker::failure_count() const",
                "rrr::CircuitBreaker@rrr.circuit_breaker::success_count() const",
                "rrr::CircuitBreaker@rrr.circuit_breaker::config() const",
            }
        ),
    ),
    "rrr.connection_state": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.connection_state;",
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
                "rrr::connection_state_to_string@rrr.connection_state(rrr::ConnectionState@rrr.connection_state)",
                "rrr::ConnectionStateMachine@rrr.connection_state::new_()",
                "rrr::ConnectionStateMachine@rrr.connection_state::state() const",
                "rrr::ConnectionStateMachine@rrr.connection_state::can_transition_to(rrr::ConnectionState@rrr.connection_state) const",
                "rrr::ConnectionStateMachine@rrr.connection_state::transition_to(rrr::ConnectionState@rrr.connection_state) const",
                "rrr::ConnectionStateMachine@rrr.connection_state::force_state(rrr::ConnectionState@rrr.connection_state) const",
                "rrr::ConnectionStateMachine@rrr.connection_state::set_on_state_change(rusty::Function<void (rrr::ConnectionState@rrr.connection_state, rrr::ConnectionState@rrr.connection_state) const>)",
                "rrr::ConnectionStateMachine@rrr.connection_state::is_connected() const",
                "rrr::ConnectionStateMachine@rrr.connection_state::is_failed() const",
                "rrr::ConnectionStateMachine@rrr.connection_state::is_terminal() const",
                "rrr::ConnectionStateMachine@rrr.connection_state::can_connect() const",
                "rrr::ConnectionStateMachine@rrr.connection_state::is_usable() const",
                "rrr::ConnectionStateMachine@rrr.connection_state::is_valid_transition(rrr::ConnectionState@rrr.connection_state, rrr::ConnectionState@rrr.connection_state)",
            }
        ),
    ),
    "rrr.heartbeat": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.heartbeat;",
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
                "rrr::heartbeat_time_us@rrr.heartbeat()",
                "rrr::HeartbeatConfig@rrr.heartbeat::new_()",
                "rrr::HeartbeatConfig@rrr.heartbeat::defaults()",
                "rrr::HeartbeatConfig@rrr.heartbeat::aggressive()",
                "rrr::HeartbeatConfig@rrr.heartbeat::relaxed()",
                "rrr::HeartbeatConfig@rrr.heartbeat::disabled()",
                "rrr::HeartbeatManager@rrr.heartbeat::new_(rrr::HeartbeatConfig@rrr.heartbeat const&)",
                "rrr::HeartbeatManager@rrr.heartbeat::set_config(rrr::HeartbeatConfig@rrr.heartbeat const&) const",
                "rrr::HeartbeatManager@rrr.heartbeat::set_on_timeout(rusty::Function<void ()>) const",
                "rrr::HeartbeatManager@rrr.heartbeat::should_send_heartbeat() const",
                "rrr::HeartbeatManager@rrr.heartbeat::on_heartbeat_sent() const",
                "rrr::HeartbeatManager@rrr.heartbeat::on_pong_received() const",
                "rrr::HeartbeatManager@rrr.heartbeat::check_timeout() const",
                "rrr::HeartbeatManager@rrr.heartbeat::time_until_next_heartbeat_ms() const",
                "rrr::HeartbeatManager@rrr.heartbeat::is_timed_out() const",
                "rrr::HeartbeatManager@rrr.heartbeat::missed_count() const",
                "rrr::HeartbeatManager@rrr.heartbeat::is_pending_pong() const",
                "rrr::HeartbeatManager@rrr.heartbeat::reset() const",
                "rrr::HeartbeatManager@rrr.heartbeat::config() const",
            }
        ),
    ),
    "rrr.load_balancer": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.load_balancer;",
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
                "rrr::load_balancing_strategy_to_string@rrr.load_balancer(rrr::LoadBalancingStrategy@rrr.load_balancer)",
                "rrr::LoadBalancerState@rrr.load_balancer::new_()",
                "rrr::LoadBalancerState@rrr.load_balancer::next_round_robin_index(unsigned long) const",
                "rrr::LoadBalancerState@rrr.load_balancer::reset() const",
                "rrr::LoadBalancer@rrr.load_balancer::select_random(unsigned long, unsigned long)",
                "rrr::LoadBalancer@rrr.load_balancer::select_round_robin(unsigned long, rrr::LoadBalancerState@rrr.load_balancer const&)",
            }
        ),
    ),
    "rrr.frame_codec": AbiSpec(
        surface=frozenset(
            {
                "#include <vector>",
                "#include <rusty/io.hpp>",
                "export module rrr.frame_codec;",
                "import rrr.internal_protocol;",
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
                "rusty::wrapping_add(this->payload_size",
                "static constexpr bool is_send = true;",
                "static constexpr bool is_sync = true;",
            }
        ),
        symbols=frozenset(
            {
                ("R", "rrr::kFrameHeaderSize@rrr.frame_codec"),
                ("R", "rrr::kMaxFramePayloadSize@rrr.frame_codec"),
                (
                    "T",
                    "rrr::FrameHeader@rrr.frame_codec::total_frame_size() const",
                ),
                (
                    "T",
                    "rrr::FrameStreamReader@rrr.frame_codec::append(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::FrameStreamReader@rrr.frame_codec::buffered_bytes() const",
                ),
                (
                    "T",
                    "rrr::FrameStreamReader@rrr.frame_codec::consume_frame()",
                ),
                ("T", "rrr::FrameStreamReader@rrr.frame_codec::empty() const"),
                ("T", "rrr::FrameStreamReader@rrr.frame_codec::new_()"),
                (
                    "T",
                    "rrr::FrameStreamReader@rrr.frame_codec::next_frame(rrr::FrameView@rrr.frame_codec&) const",
                ),
                ("T", "rrr::FrameStreamReader@rrr.frame_codec::reset()"),
                (
                    "T",
                    "rrr::frame_codec_encode_into@rrr.frame_codec(std::__1::vector<unsigned char, std::__1::allocator<unsigned char>>&, unsigned char const*, int, bool)",
                ),
                (
                    "T",
                    "rrr::frame_codec_peek_header@rrr.frame_codec(std::__1::span<unsigned char const, 18446744073709551615ul>, rrr::FrameHeader@rrr.frame_codec&)",
                ),
                (
                    "T",
                    "rrr::frame_codec_write_header@rrr.frame_codec(std::__1::span<unsigned char, 18446744073709551615ul>, int, bool)",
                ),
                (
                    "T",
                    "rrr::frame_decode_status_to_string@rrr.frame_codec(rrr::FrameDecodeStatus@rrr.frame_codec)",
                ),
                (
                    "T",
                    "rrr::fsr_append@rrr.frame_codec(rrr::FrameStreamReader@rrr.frame_codec&, unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::fsr_consume_frame@rrr.frame_codec(rrr::FrameStreamReader@rrr.frame_codec&)",
                ),
                ("T", "rrr::make_frame_cursor@rrr.frame_codec()"),
            }
        ),
    ),
    "rrr.utils": AbiSpec(
        surface=frozenset(
            {
                "#include <netdb.h>",
                "export module rrr.utils;",
                "import rrr.logging;",
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
                "rrr::log_line(3, 0, rusty::ptr::null(), message);",
                "rrr::log_line(1, 0, rusty::ptr::null(), message);",
                "rusty::sys::env::hostname();",
                "utils_ffi::srpc_find_open_port();",
                "utils_ffi::freeaddrinfo(this->info_);",
            }
        ),
        symbols=frozenset(
            ("T", symbol)
            for symbol in {
                "rrr::AddrInfo@rrr.utils::new_()",
                "rrr::AddrInfo@rrr.utils::adopt(addrinfo*)",
                "rrr::AddrInfo@rrr.utils::AddrInfo(addrinfo*, rusty::Cell<bool>)",
                "rrr::AddrInfo@rrr.utils::AddrInfo(rrr::AddrInfo@rrr.utils&&)",
                "rrr::AddrInfo@rrr.utils::get() const",
                "rrr::AddrInfo@rrr.utils::operator=(rrr::AddrInfo@rrr.utils&&)",
                "rrr::AddrInfo@rrr.utils::rusty_mark_forgotten() const",
                "rrr::AddrInfo@rrr.utils::valid() const",
                "rrr::AddrInfo@rrr.utils::~AddrInfo()",
                "rrr::find_open_port@rrr.utils()",
                "rrr::get_host_name@rrr.utils()",
            }
        ),
    ),
    "rrr.basetypes": AbiSpec(
        surface=frozenset(
            {
                '#include "misc/srpc_timing.h"',
                "#include <rusty/sync/atomic.hpp>",
                "export module rrr.basetypes;",
                "export using i8 = int8_t;",
                "export using i16 = int16_t;",
                "export using i32 = int32_t;",
                "export using i64 = int64_t;",
                "export using rusty::sync::atomic::AtomicI64;",
                "export using rusty::sync::atomic::Ordering;",
                "export constexpr uint64_t RRR_USEC_PER_SEC",
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
                ("R", "rrr::RRR_USEC_PER_SEC@rrr.basetypes"),
                ("T", "rrr::abort_if_false@rrr.basetypes(bool)"),
                ("T", "rrr::time_now_us@rrr.basetypes(bool)"),
                ("T", "rrr::SparseInt@rrr.basetypes::buf_size(unsigned char)"),
                ("T", "rrr::SparseInt@rrr.basetypes::dump32(int, unsigned char*)"),
                ("T", "rrr::SparseInt@rrr.basetypes::dump64(long, unsigned char*)"),
                ("T", "rrr::SparseInt@rrr.basetypes::load32(unsigned char const*)"),
                ("T", "rrr::SparseInt@rrr.basetypes::load64(unsigned char const*)"),
                ("T", "rrr::SparseInt@rrr.basetypes::val_size(long)"),
                ("T", "rrr::v32@rrr.basetypes::new_(int)"),
                ("T", "rrr::v32@rrr.basetypes::set(int)"),
                ("T", "rrr::v32@rrr.basetypes::get() const"),
                ("T", "rrr::v32@rrr.basetypes::val_size() const"),
                ("T", "rrr::v64@rrr.basetypes::new_(long)"),
                ("T", "rrr::v64@rrr.basetypes::set(long)"),
                ("T", "rrr::v64@rrr.basetypes::get() const"),
                ("T", "rrr::v64@rrr.basetypes::val_size() const"),
                ("T", "rrr::Counter@rrr.basetypes::new_(long)"),
                ("T", "rrr::Counter@rrr.basetypes::peek_next() const"),
                ("T", "rrr::Counter@rrr.basetypes::next(long) const"),
                ("T", "rrr::Counter@rrr.basetypes::reset(long) const"),
                ("T", "rrr::Time@rrr.basetypes::now(bool)"),
                ("T", "rrr::Time@rrr.basetypes::sleep(unsigned long)"),
                ("T", "rrr::Timer@rrr.basetypes::new_()"),
                ("T", "rrr::Timer@rrr.basetypes::start()"),
                ("T", "rrr::Timer@rrr.basetypes::stop()"),
                ("T", "rrr::Timer@rrr.basetypes::reset()"),
                ("T", "rrr::Timer@rrr.basetypes::elapsed() const"),
            }
        ),
    ),
    "rrr.request_queue": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.request_queue;",
                "import vec_port.vec;",
                "import rrr.circuit_breaker;",
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
                "rusty::wrapping_sub(::rrr::queued_request_time_us()",
                "catch_unwind(AssertUnwindSafe(",
            }
        ),
        symbols=frozenset(
            {
                ("R", "rrr::kRequestQueueRejectedError@rrr.request_queue"),
                ("R", "rrr::kRequestQueueExpiredError@rrr.request_queue"),
                *(
                    ("T", symbol)
                    for symbol in {
                        "rrr::overflow_strategy_to_string@rrr.request_queue(rrr::OverflowStrategy@rrr.request_queue)",
                        "rrr::queued_request_time_us@rrr.request_queue()",
                        "rrr::rq_invoke_callback_safely@rrr.request_queue(rusty::Function<void (int)>, int)",
                        "rrr::QueuedRequest@rrr.request_queue::new_()",
                        "rrr::QueuedRequest@rrr.request_queue::is_expired() const",
                        "rrr::QueuedRequest@rrr.request_queue::age_ms() const",
                        "rrr::RequestQueueConfig@rrr.request_queue::new_()",
                        "rrr::RequestQueueConfig@rrr.request_queue::defaults()",
                        "rrr::RequestQueueConfig@rrr.request_queue::small()",
                        "rrr::RequestQueueConfig@rrr.request_queue::large()",
                        "rrr::RequestQueueConfig@rrr.request_queue::disabled()",
                        "rrr::RequestQueue@rrr.request_queue::new_()",
                        "rrr::RequestQueue@rrr.request_queue::with_config(rrr::RequestQueueConfig@rrr.request_queue)",
                        "rrr::RequestQueue@rrr.request_queue::enqueue(rrr::QueuedRequest@rrr.request_queue) const",
                        "rrr::RequestQueue@rrr.request_queue::dequeue()",
                        "rrr::RequestQueue@rrr.request_queue::expire_stale() const",
                        "rrr::RequestQueue@rrr.request_queue::size() const",
                        "rrr::RequestQueue@rrr.request_queue::empty() const",
                        "rrr::RequestQueue@rrr.request_queue::full()",
                        "rrr::RequestQueue@rrr.request_queue::remaining_capacity()",
                        "rrr::RequestQueue@rrr.request_queue::clear_all(int) const",
                        "rrr::RequestQueue@rrr.request_queue::config() const",
                        "rrr::RequestQueue@rrr.request_queue::enabled() const",
                        "rrr::RequestQueue@rrr.request_queue::max_size() const",
                        "rrr::RequestQueue@rrr.request_queue::update_config(rrr::RequestQueueConfig@rrr.request_queue) const",
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
    "rrr.debugging": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.debugging;",
                "import vec_port.vec;",
                "namespace rrr {",
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
                ("T", "rrr::BtCapture@rrr.debugging::new_()"),
                ("T", "rrr::bt_capture@rrr.debugging()"),
                ("T", "rrr::bt_empty_string@rrr.debugging()"),
                ("T", "rrr::bt_index_prefix@rrr.debugging(int)"),
                (
                    "T",
                    "rrr::bt_render@rrr.debugging(rrr::BtCapture@rrr.debugging const&)",
                ),
                ("T", "rrr::likely@rrr.debugging(bool)"),
                ("T", "rrr::print_stack_trace@rrr.debugging(_IO_FILE*)"),
                ("T", "rrr::unlikely@rrr.debugging(bool)"),
                (
                    "T",
                    "rrr::verify_failed@rrr.debugging(std::__1::basic_string_view<char, "
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
    "rrr.serializable": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.serializable;",
                "namespace rrr {",
                "import rrr.basetypes;",
                "import rrr.debugging;",
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
                "export using ::rrr::details::__ufcs_SerializableBase::save;",
                "export using ::rrr::details::__ufcs_SerializableBase::load;",
                "export using ::rrr::details::__ufcs_SerializableBase::kind;",
                "export using ::rrr::details::__ufcs_SerializableBase::payload_type_id;",
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
                    "typeinfo for rrr::Deserialize@rrr.serializable",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapter@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRef@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializableBase@rrr.serializable",
                ),
                (
                    "D",
                    "typeinfo for rrr::Serialize@rrr.serializable",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapter@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRef@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<double>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SinkBase@rrr.serializable",
                ),
                (
                    "D",
                    "typeinfo for rrr::SinkBaseAdapter@rrr.serializable<rrr::BufferSink@rrr.serializable>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SinkBaseAdapter@rrr.serializable<rrr::FdSink@rrr.serializable>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SinkBaseAdapterRef@rrr.serializable<rrr::BufferSink@rrr.serializable>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SinkBaseAdapterRef@rrr.serializable<rrr::FdSink@rrr.serializable>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SinkBaseAdapterRefMut@rrr.serializable<rrr::BufferSink@rrr.serializable>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SinkBaseAdapterRefMut@rrr.serializable<rrr::FdSink@rrr.serializable>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SourceBase@rrr.serializable",
                ),
                (
                    "D",
                    "typeinfo for rrr::SourceBaseAdapter@rrr.serializable<rrr::BufferSource@rrr.serializable>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SourceBaseAdapter@rrr.serializable<rrr::FdSource@rrr.serializable>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SourceBaseAdapterRef@rrr.serializable<rrr::BufferSource@rrr.serializable>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SourceBaseAdapterRef@rrr.serializable<rrr::FdSource@rrr.serializable>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SourceBaseAdapterRefMut@rrr.serializable<rrr::BufferSource@rrr.serializable>",
                ),
                (
                    "D",
                    "typeinfo for rrr::SourceBaseAdapterRefMut@rrr.serializable<rrr::FdSource@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::Deserialize@rrr.serializable",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<double>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<int>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<long>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<short>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapter@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<double>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<int>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<long>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<short>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRef@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<double>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<int>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<long>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<short>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializableBase@rrr.serializable",
                ),
                (
                    "D",
                    "vtable for rrr::Serialize@rrr.serializable",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<double>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<int>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<long>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<short>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapter@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<double>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<int>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<long>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<short>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRef@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<double>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<int>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<long>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<short>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<signed char>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned char>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned int>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned long>",
                ),
                (
                    "D",
                    "vtable for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned short>",
                ),
                (
                    "D",
                    "vtable for rrr::SinkBase@rrr.serializable",
                ),
                (
                    "D",
                    "vtable for rrr::SinkBaseAdapter@rrr.serializable<rrr::BufferSink@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::SinkBaseAdapter@rrr.serializable<rrr::FdSink@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::SinkBaseAdapterRef@rrr.serializable<rrr::BufferSink@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::SinkBaseAdapterRef@rrr.serializable<rrr::FdSink@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::SinkBaseAdapterRefMut@rrr.serializable<rrr::BufferSink@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::SinkBaseAdapterRefMut@rrr.serializable<rrr::FdSink@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::SourceBase@rrr.serializable",
                ),
                (
                    "D",
                    "vtable for rrr::SourceBaseAdapter@rrr.serializable<rrr::BufferSource@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::SourceBaseAdapter@rrr.serializable<rrr::FdSource@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::SourceBaseAdapterRef@rrr.serializable<rrr::BufferSource@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::SourceBaseAdapterRef@rrr.serializable<rrr::FdSource@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::SourceBaseAdapterRefMut@rrr.serializable<rrr::BufferSource@rrr.serializable>",
                ),
                (
                    "D",
                    "vtable for rrr::SourceBaseAdapterRefMut@rrr.serializable<rrr::FdSource@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::Deserialize@rrr.serializable",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapter@rrr.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRef@rrr.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializableBase@rrr.serializable",
                ),
                (
                    "R",
                    "typeinfo name for rrr::Serialize@rrr.serializable",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapter@rrr.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRef@rrr.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<double>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<rrr::v32@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<rrr::v64@rrr.basetypes>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<signed char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned char>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned int>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned long>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SerializeAdapterRefMut@rrr.serializable<unsigned short>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SinkBase@rrr.serializable",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SinkBaseAdapter@rrr.serializable<rrr::BufferSink@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SinkBaseAdapter@rrr.serializable<rrr::FdSink@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SinkBaseAdapterRef@rrr.serializable<rrr::BufferSink@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SinkBaseAdapterRef@rrr.serializable<rrr::FdSink@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SinkBaseAdapterRefMut@rrr.serializable<rrr::BufferSink@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SinkBaseAdapterRefMut@rrr.serializable<rrr::FdSink@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SourceBase@rrr.serializable",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SourceBaseAdapter@rrr.serializable<rrr::BufferSource@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SourceBaseAdapter@rrr.serializable<rrr::FdSource@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SourceBaseAdapterRef@rrr.serializable<rrr::BufferSource@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SourceBaseAdapterRef@rrr.serializable<rrr::FdSource@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SourceBaseAdapterRefMut@rrr.serializable<rrr::BufferSource@rrr.serializable>",
                ),
                (
                    "R",
                    "typeinfo name for rrr::SourceBaseAdapterRefMut@rrr.serializable<rrr::FdSource@rrr.serializable>",
                ),
                (
                    "T",
                    "rrr::BinaryReadArchive@rrr.serializable::read_exact(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::BinaryReadArchive@rrr.serializable::read_or_abort(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::BinaryWriteArchive@rrr.serializable::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::BufferSink@rrr.serializable::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::BufferSource@rrr.serializable::eof() const",
                ),
                (
                    "T",
                    "rrr::BufferSource@rrr.serializable::new_(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::BufferSource@rrr.serializable::pos() const",
                ),
                (
                    "T",
                    "rrr::BufferSource@rrr.serializable::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::BufferSource@rrr.serializable::remaining() const",
                ),
                (
                    "T",
                    "rrr::Deserialize@rrr.serializable::~Deserialize()",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<double>::DeserializeAdapter(double)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<double>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<double>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<double>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<int>::DeserializeAdapter(int)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<int>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<int>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<int>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<long>::DeserializeAdapter(long)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<long>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<long>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<long>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>::DeserializeAdapter(rrr::v32@rrr.basetypes)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>::DeserializeAdapter(rrr::v64@rrr.basetypes)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<short>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<short>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<short>::DeserializeAdapter(short)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<short>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<signed char>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<signed char>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<signed char>::DeserializeAdapter(signed char)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<signed char>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::DeserializeAdapter(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned char>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<unsigned char>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned char>::DeserializeAdapter(unsigned char)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned char>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned int>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<unsigned int>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned int>::DeserializeAdapter(unsigned int)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned int>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned long>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<unsigned long>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned long>::DeserializeAdapter(unsigned long)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned long>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned short>::DeserializeAdapter(rrr::DeserializeAdapter@rrr.serializable<unsigned short>&&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned short>::DeserializeAdapter(unsigned short)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapter@rrr.serializable<unsigned short>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<double>::DeserializeAdapterRef(double const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<double>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<int>::DeserializeAdapterRef(int const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<int>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<long>::DeserializeAdapterRef(long const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<long>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<rrr::v32@rrr.basetypes>::DeserializeAdapterRef(rrr::v32@rrr.basetypes const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<rrr::v32@rrr.basetypes>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<rrr::v64@rrr.basetypes>::DeserializeAdapterRef(rrr::v64@rrr.basetypes const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<rrr::v64@rrr.basetypes>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<short>::DeserializeAdapterRef(short const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<short>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<signed char>::DeserializeAdapterRef(signed char const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<signed char>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::DeserializeAdapterRef(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<unsigned char>::DeserializeAdapterRef(unsigned char const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<unsigned char>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<unsigned int>::DeserializeAdapterRef(unsigned int const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<unsigned int>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<unsigned long>::DeserializeAdapterRef(unsigned long const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<unsigned long>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<unsigned short>::DeserializeAdapterRef(unsigned short const&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRef@rrr.serializable<unsigned short>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<double>::DeserializeAdapterRefMut(double&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<double>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<int>::DeserializeAdapterRefMut(int&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<int>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<long>::DeserializeAdapterRefMut(long&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<long>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<rrr::v32@rrr.basetypes>::DeserializeAdapterRefMut(rrr::v32@rrr.basetypes&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<rrr::v32@rrr.basetypes>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<rrr::v64@rrr.basetypes>::DeserializeAdapterRefMut(rrr::v64@rrr.basetypes&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<rrr::v64@rrr.basetypes>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<short>::DeserializeAdapterRefMut(short&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<short>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<signed char>::DeserializeAdapterRefMut(signed char&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<signed char>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::DeserializeAdapterRefMut(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned char>::DeserializeAdapterRefMut(unsigned char&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned char>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned int>::DeserializeAdapterRefMut(unsigned int&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned int>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned long>::DeserializeAdapterRefMut(unsigned long&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned long>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned short>::DeserializeAdapterRefMut(unsigned short&)",
                ),
                (
                    "T",
                    "rrr::DeserializeAdapterRefMut@rrr.serializable<unsigned short>::deserialize(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(double&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(int&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(long&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(rrr::v32@rrr.basetypes&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(rrr::v64@rrr.basetypes&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(short&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(signed char&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(unsigned char&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(unsigned int&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(unsigned long&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Deserialize_::deserialize@rrr.serializable(unsigned short&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::FdSink@rrr.serializable::fd() const",
                ),
                (
                    "T",
                    "rrr::FdSink@rrr.serializable::new_(int)",
                ),
                (
                    "T",
                    "rrr::FdSink@rrr.serializable::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::FdSource@rrr.serializable::fd() const",
                ),
                (
                    "T",
                    "rrr::FdSource@rrr.serializable::new_(int)",
                ),
                (
                    "T",
                    "rrr::FdSource@rrr.serializable::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SerializableBase@rrr.serializable::~SerializableBase()",
                ),
                (
                    "T",
                    "rrr::SerializableRegistry@rrr.serializable::clear_for_testing()",
                ),
                (
                    "T",
                    "rrr::SerializableRegistry@rrr.serializable::create(int)",
                ),
                (
                    "T",
                    "rrr::SerializableRegistry@rrr.serializable::is_registered(int)",
                ),
                (
                    "T",
                    "rrr::Serialize@rrr.serializable::~Serialize()",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<double>::SerializeAdapter(double)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<double>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<double>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<double>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<int>::SerializeAdapter(int)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<int>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<int>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<int>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<long>::SerializeAdapter(long)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<long>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<long>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<long>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>::SerializeAdapter(rrr::v32@rrr.basetypes)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<rrr::v32@rrr.basetypes>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>::SerializeAdapter(rrr::v64@rrr.basetypes)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<rrr::v64@rrr.basetypes>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<short>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<short>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<short>::SerializeAdapter(short)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<short>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<signed char>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<signed char>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<signed char>::SerializeAdapter(signed char)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<signed char>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::SerializeAdapter(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::SerializeAdapter(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned char>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<unsigned char>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned char>::SerializeAdapter(unsigned char)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned char>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned int>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<unsigned int>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned int>::SerializeAdapter(unsigned int)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned int>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned long>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<unsigned long>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned long>::SerializeAdapter(unsigned long)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned long>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned short>::SerializeAdapter(rrr::SerializeAdapter@rrr.serializable<unsigned short>&&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned short>::SerializeAdapter(unsigned short)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapter@rrr.serializable<unsigned short>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<double>::SerializeAdapterRef(double const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<double>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<int>::SerializeAdapterRef(int const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<int>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<long>::SerializeAdapterRef(long const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<long>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<rrr::v32@rrr.basetypes>::SerializeAdapterRef(rrr::v32@rrr.basetypes const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<rrr::v32@rrr.basetypes>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<rrr::v64@rrr.basetypes>::SerializeAdapterRef(rrr::v64@rrr.basetypes const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<rrr::v64@rrr.basetypes>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<short>::SerializeAdapterRef(short const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<short>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<signed char>::SerializeAdapterRef(signed char const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<signed char>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::SerializeAdapterRef(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::SerializeAdapterRef(std::__1::basic_string_view<char, std::__1::char_traits<char>> const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<unsigned char>::SerializeAdapterRef(unsigned char const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<unsigned char>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<unsigned int>::SerializeAdapterRef(unsigned int const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<unsigned int>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<unsigned long>::SerializeAdapterRef(unsigned long const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<unsigned long>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<unsigned short>::SerializeAdapterRef(unsigned short const&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRef@rrr.serializable<unsigned short>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<double>::SerializeAdapterRefMut(double&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<double>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<int>::SerializeAdapterRefMut(int&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<int>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<long>::SerializeAdapterRefMut(long&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<long>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<rrr::v32@rrr.basetypes>::SerializeAdapterRefMut(rrr::v32@rrr.basetypes&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<rrr::v32@rrr.basetypes>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<rrr::v64@rrr.basetypes>::SerializeAdapterRefMut(rrr::v64@rrr.basetypes&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<rrr::v64@rrr.basetypes>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<short>::SerializeAdapterRefMut(short&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<short>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<signed char>::SerializeAdapterRefMut(signed char&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<signed char>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::SerializeAdapterRefMut(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::SerializeAdapterRefMut(std::__1::basic_string_view<char, std::__1::char_traits<char>>&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<std::__1::basic_string_view<char, std::__1::char_traits<char>>>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<unsigned char>::SerializeAdapterRefMut(unsigned char&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<unsigned char>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<unsigned int>::SerializeAdapterRefMut(unsigned int&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<unsigned int>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<unsigned long>::SerializeAdapterRefMut(unsigned long&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<unsigned long>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<unsigned short>::SerializeAdapterRefMut(unsigned short&)",
                ),
                (
                    "T",
                    "rrr::SerializeAdapterRefMut@rrr.serializable<unsigned short>::serialize(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(double const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(int const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(long const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(rrr::v32@rrr.basetypes const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(rrr::v64@rrr.basetypes const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(short const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(signed char const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(std::__1::basic_string_view<char, std::__1::char_traits<char>> const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(unsigned char const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(unsigned int const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(unsigned long const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::Serialize_::serialize@rrr.serializable(unsigned short const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::SinkBase@rrr.serializable::~SinkBase()",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapter@rrr.serializable<rrr::BufferSink@rrr.serializable>::SinkBaseAdapter(rrr::BufferSink@rrr.serializable)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapter@rrr.serializable<rrr::BufferSink@rrr.serializable>::SinkBaseAdapter(rrr::SinkBaseAdapter@rrr.serializable<rrr::BufferSink@rrr.serializable>&&)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapter@rrr.serializable<rrr::BufferSink@rrr.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapter@rrr.serializable<rrr::FdSink@rrr.serializable>::SinkBaseAdapter(rrr::FdSink@rrr.serializable)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapter@rrr.serializable<rrr::FdSink@rrr.serializable>::SinkBaseAdapter(rrr::SinkBaseAdapter@rrr.serializable<rrr::FdSink@rrr.serializable>&&)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapter@rrr.serializable<rrr::FdSink@rrr.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapterRef@rrr.serializable<rrr::BufferSink@rrr.serializable>::SinkBaseAdapterRef(rrr::BufferSink@rrr.serializable const&)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapterRef@rrr.serializable<rrr::BufferSink@rrr.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapterRef@rrr.serializable<rrr::FdSink@rrr.serializable>::SinkBaseAdapterRef(rrr::FdSink@rrr.serializable const&)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapterRef@rrr.serializable<rrr::FdSink@rrr.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapterRefMut@rrr.serializable<rrr::BufferSink@rrr.serializable>::SinkBaseAdapterRefMut(rrr::BufferSink@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapterRefMut@rrr.serializable<rrr::BufferSink@rrr.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapterRefMut@rrr.serializable<rrr::FdSink@rrr.serializable>::SinkBaseAdapterRefMut(rrr::FdSink@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::SinkBaseAdapterRefMut@rrr.serializable<rrr::FdSink@rrr.serializable>::write_bytes(unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SourceBase@rrr.serializable::~SourceBase()",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapter@rrr.serializable<rrr::BufferSource@rrr.serializable>::SourceBaseAdapter(rrr::BufferSource@rrr.serializable)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapter@rrr.serializable<rrr::BufferSource@rrr.serializable>::SourceBaseAdapter(rrr::SourceBaseAdapter@rrr.serializable<rrr::BufferSource@rrr.serializable>&&)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapter@rrr.serializable<rrr::BufferSource@rrr.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapter@rrr.serializable<rrr::FdSource@rrr.serializable>::SourceBaseAdapter(rrr::FdSource@rrr.serializable)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapter@rrr.serializable<rrr::FdSource@rrr.serializable>::SourceBaseAdapter(rrr::SourceBaseAdapter@rrr.serializable<rrr::FdSource@rrr.serializable>&&)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapter@rrr.serializable<rrr::FdSource@rrr.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapterRef@rrr.serializable<rrr::BufferSource@rrr.serializable>::SourceBaseAdapterRef(rrr::BufferSource@rrr.serializable const&)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapterRef@rrr.serializable<rrr::BufferSource@rrr.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapterRef@rrr.serializable<rrr::FdSource@rrr.serializable>::SourceBaseAdapterRef(rrr::FdSource@rrr.serializable const&)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapterRef@rrr.serializable<rrr::FdSource@rrr.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapterRefMut@rrr.serializable<rrr::BufferSource@rrr.serializable>::SourceBaseAdapterRefMut(rrr::BufferSource@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapterRefMut@rrr.serializable<rrr::BufferSource@rrr.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapterRefMut@rrr.serializable<rrr::FdSource@rrr.serializable>::SourceBaseAdapterRefMut(rrr::FdSource@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::SourceBaseAdapterRefMut@rrr.serializable<rrr::FdSource@rrr.serializable>::read_bytes(unsigned char*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::make_sink_proxy_buffer@rrr.serializable(rrr::BufferSink@rrr.serializable*)",
                ),
                (
                    "T",
                    "rrr::make_sink_proxy_fd@rrr.serializable(rrr::FdSink@rrr.serializable*)",
                ),
                (
                    "T",
                    "rrr::make_source_proxy_buffer@rrr.serializable(rrr::BufferSource@rrr.serializable*)",
                ),
                (
                    "T",
                    "rrr::make_source_proxy_fd@rrr.serializable(rrr::FdSource@rrr.serializable*)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(double&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(int&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(long&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(rrr::v32@rrr.basetypes&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(rrr::v64@rrr.basetypes&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(short&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(signed char&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(unsigned char&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(unsigned int&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(unsigned long&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::deserialize@rrr.serializable(unsigned short&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(double const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(int const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(long const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(rrr::v32@rrr.basetypes const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(rrr::v64@rrr.basetypes const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(short const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(signed char const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(std::__1::basic_string_view<char, std::__1::char_traits<char>> const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(unsigned char const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(unsigned int const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(unsigned long const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::rusty_ext::serialize@rrr.serializable(unsigned short const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::serializable_registry_clear_impl@rrr.serializable()",
                ),
                (
                    "T",
                    "rrr::serializable_registry_create_impl@rrr.serializable(int)",
                ),
                (
                    "T",
                    "rrr::serializable_registry_is_registered_impl@rrr.serializable(int)",
                ),
                (
                    "T",
                    "rrr::serializable_registry_register_factory@rrr.serializable(int, rusty::Function<rusty::Arc<rrr::SerializableBase@rrr.serializable> ()>)",
                ),
            }
        ),
    ),
    "rrr.serializable_envelope": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.serializable_envelope;",
                "namespace rrr {",
                "import rrr.basetypes;",
                "import rrr.debugging;",
                "import rrr.serializable;",
            }
        ),
        symbols=frozenset(
            {
            }
        ),
    ),
    "rrr.future": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.future;",
                "namespace rrr {",
                "import rrr.reactor;",
                "import std;",
            }
        ),
        symbols=frozenset(
            {
            }
        ),
    ),
    "rrr.logging": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.logging;",
                "namespace rrr {",
                "import rrr.debugging;",
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
                    "rrr::Log@rrr.logging::level_now()",
                ),
                (
                    "T",
                    "rrr::Log@rrr.logging::set_level(int)",
                ),
                (
                    "T",
                    "rrr::log_basename@rrr.logging(signed char const*)",
                ),
                (
                    "T",
                    "rrr::log_level_tag@rrr.logging(int)",
                ),
                (
                    "T",
                    "rrr::log_line@rrr.logging(int, int, signed char const*, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::log_sink_write@rrr.logging(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::log_time_now@rrr.logging()",
                ),
            }
        ),
    ),
    "rrr.idempotency": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.idempotency;",
                "namespace rrr {",
                "import vec_port.vec;",
                "import rrr.serializable;",
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
                    "rrr::CachedResponse@rrr.idempotency::is_expired(unsigned long, unsigned long) const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::clear() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::config() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::enabled() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::evict_expired(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::evictions() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::hit_rate() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::hits() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::lookup(rrr::IdempotencyKey@rrr.idempotency const&, unsigned long, int&, rusty::port::vec::Vec@vec_port.vec<unsigned char, rusty::alloc::Global>&) const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::misses() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::new_()",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::remove(rrr::IdempotencyKey@rrr.idempotency const&) const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::reset_stats() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::set_config(rrr::IdempotencyConfig@rrr.idempotency const&) const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::size() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::store(rrr::IdempotencyKey@rrr.idempotency const&, int, rusty::port::vec::Vec@vec_port.vec<unsigned char, rusty::alloc::Global> const&, unsigned long) const",
                ),
                (
                    "T",
                    "rrr::IdempotencyCache@rrr.idempotency::with_config(rrr::IdempotencyConfig@rrr.idempotency)",
                ),
                (
                    "T",
                    "rrr::IdempotencyConfig@rrr.idempotency::defaults()",
                ),
                (
                    "T",
                    "rrr::IdempotencyConfig@rrr.idempotency::disabled()",
                ),
                (
                    "T",
                    "rrr::IdempotencyConfig@rrr.idempotency::large()",
                ),
                (
                    "T",
                    "rrr::IdempotencyConfig@rrr.idempotency::new_()",
                ),
                (
                    "T",
                    "rrr::IdempotencyConfig@rrr.idempotency::small()",
                ),
                (
                    "T",
                    "rrr::IdempotencyKey@rrr.idempotency::empty()",
                ),
                (
                    "T",
                    "rrr::IdempotencyKey@rrr.idempotency::is_valid() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyKey@rrr.idempotency::new_(unsigned long, unsigned long)",
                ),
                (
                    "T",
                    "rrr::IdempotencyKey@rrr.idempotency::operator==(rrr::IdempotencyKey@rrr.idempotency const&) const",
                ),
                (
                    "T",
                    "rrr::IdempotencyKeyGenerator@rrr.idempotency::client_id() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyKeyGenerator@rrr.idempotency::current_sequence() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyKeyGenerator@rrr.idempotency::new_(unsigned long)",
                ),
                (
                    "T",
                    "rrr::IdempotencyKeyGenerator@rrr.idempotency::next() const",
                ),
                (
                    "T",
                    "rrr::IdempotencyKeyGenerator@rrr.idempotency::set_client_id(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::IdempotencyKeyHash@rrr.idempotency::hash_one(rrr::IdempotencyKey@rrr.idempotency const&) const",
                ),
                (
                    "T",
                    "rrr::cached_response_get@rrr.idempotency(rrr::CachedResponse@rrr.idempotency const&, rusty::port::vec::Vec@vec_port.vec<unsigned char, rusty::alloc::Global>&)",
                ),
                (
                    "T",
                    "rrr::cached_response_set@rrr.idempotency(rrr::CachedResponse@rrr.idempotency&, rusty::port::vec::Vec@vec_port.vec<unsigned char, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "rrr::deserialize@rrr.idempotency(rrr::IdempotencyKey@rrr.idempotency&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::serialize@rrr.idempotency(rrr::IdempotencyKey@rrr.idempotency const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
            }
        ),
    ),
    "rrr.fiber": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.fiber;",
                "namespace rrr {",
                "import rc_port;",
                "import rrr.basetypes;",
                "import rrr.reactor;",
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
                    "rrr::this_fiber::current@rrr.fiber()",
                ),
                (
                    "T",
                    "rrr::this_fiber::get_id@rrr.fiber()",
                ),
                (
                    "T",
                    "rrr::this_fiber::in_fiber_context@rrr.fiber()",
                ),
                (
                    "T",
                    "rrr::this_fiber::sleep_ms@rrr.fiber(unsigned long)",
                ),
                (
                    "T",
                    "rrr::this_fiber::sleep_s@rrr.fiber(unsigned long)",
                ),
                (
                    "T",
                    "rrr::this_fiber::sleep_until_us@rrr.fiber(unsigned long)",
                ),
                (
                    "T",
                    "rrr::this_fiber::sleep_us@rrr.fiber(unsigned long)",
                ),
                (
                    "T",
                    "rrr::this_fiber::yield@rrr.fiber()",
                ),
            }
        ),
    ),
    "rrr.misc": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.misc;",
                "namespace rrr {",
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
                    "typeinfo for rrr::Job@rrr.misc",
                ),
                (
                    "D",
                    "typeinfo for rrr::OneTimeJob@rrr.misc",
                ),
                (
                    "D",
                    "vtable for rrr::Job@rrr.misc",
                ),
                (
                    "D",
                    "vtable for rrr::OneTimeJob@rrr.misc",
                ),
                (
                    "R",
                    "typeinfo name for rrr::Job@rrr.misc",
                ),
                (
                    "R",
                    "typeinfo name for rrr::OneTimeJob@rrr.misc",
                ),
                (
                    "T",
                    "rrr::Job@rrr.misc::~Job()",
                ),
                (
                    "T",
                    "rrr::Job_::Done@rrr.misc(rrr::OneTimeJob@rrr.misc&)",
                ),
                (
                    "T",
                    "rrr::Job_::Ready@rrr.misc(rrr::OneTimeJob@rrr.misc&)",
                ),
                (
                    "T",
                    "rrr::Job_::Work@rrr.misc(rrr::OneTimeJob@rrr.misc&)",
                ),
                (
                    "T",
                    "rrr::OneTimeJob@rrr.misc::Done()",
                ),
                (
                    "T",
                    "rrr::OneTimeJob@rrr.misc::OneTimeJob(bool, bool, rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "rrr::OneTimeJob@rrr.misc::OneTimeJob(rrr::OneTimeJob@rrr.misc&&)",
                ),
                (
                    "T",
                    "rrr::OneTimeJob@rrr.misc::Ready()",
                ),
                (
                    "T",
                    "rrr::OneTimeJob@rrr.misc::Work()",
                ),
                (
                    "T",
                    "rrr::OneTimeJob@rrr.misc::new_(rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "rrr::format_thousands@rrr.misc(double)",
                ),
                (
                    "T",
                    "rrr::get_ncpu@rrr.misc()",
                ),
            }
        ),
    ),
    "rrr.channel": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.channel;",
                "namespace rrr {",
                "import rrr.callback_wrapper;",
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
                "export using OnFrameCallback = ::rrr::detail::CallbackWrapper<rusty::Function<void(const ChannelFrame&) const>>;",
                "export using OnClosedCallback = ::rrr::detail::CallbackWrapper<rusty::Function<void(ChannelError) const>>;",
                "export using OnErrorCallback = ::rrr::detail::CallbackWrapper<rusty::Function<void(ChannelError, std::string_view) const>>;",
                "export using ChannelConnectionProxy = rusty::Box<ChannelConnectionBase>;",
                "export using OnAcceptCallback = ::rrr::detail::CallbackWrapper<rusty::Function<void(ChannelConnectionProxy) const>>;",
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
                    "typeinfo for rrr::ChannelConnectionBase@rrr.channel",
                ),
                (
                    "D",
                    "typeinfo for rrr::ChannelFactoryBase@rrr.channel",
                ),
                (
                    "D",
                    "typeinfo for rrr::ChannelListenerBase@rrr.channel",
                ),
                (
                    "D",
                    "vtable for rrr::ChannelConnectionBase@rrr.channel",
                ),
                (
                    "D",
                    "vtable for rrr::ChannelFactoryBase@rrr.channel",
                ),
                (
                    "D",
                    "vtable for rrr::ChannelListenerBase@rrr.channel",
                ),
                (
                    "R",
                    "typeinfo name for rrr::ChannelConnectionBase@rrr.channel",
                ),
                (
                    "R",
                    "typeinfo name for rrr::ChannelFactoryBase@rrr.channel",
                ),
                (
                    "R",
                    "typeinfo name for rrr::ChannelListenerBase@rrr.channel",
                ),
                (
                    "T",
                    "rrr::ChannelConnectionBase@rrr.channel::~ChannelConnectionBase()",
                ),
                (
                    "T",
                    "rrr::ChannelFactoryBase@rrr.channel::~ChannelFactoryBase()",
                ),
                (
                    "T",
                    "rrr::ChannelListenerBase@rrr.channel::~ChannelListenerBase()",
                ),
                (
                    "T",
                    "rrr::channel_error_to_string@rrr.channel(rrr::ChannelError@rrr.channel)",
                ),
            }
        ),
    ),
    "rrr.epoll_wrapper": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.epoll_wrapper;",
                "namespace rrr {",
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
                    "typeinfo for rrr::Pollable@rrr.epoll_wrapper",
                ),
                (
                    "D",
                    "vtable for rrr::Pollable@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "rrr::LINUX_EPOLLERR@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "rrr::LINUX_EPOLLHUP@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "rrr::LINUX_EPOLLIN@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "rrr::LINUX_EPOLLOUT@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "rrr::LINUX_EPOLLRDHUP@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "rrr::PollMode::NO_CHANGE@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "rrr::PollMode::READ@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "rrr::PollMode::WRITE@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "rrr::PollReady::ERROR@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "rrr::PollReady::READABLE@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "rrr::PollReady::WRITABLE@rrr.epoll_wrapper",
                ),
                (
                    "R",
                    "typeinfo name for rrr::Pollable@rrr.epoll_wrapper",
                ),
                (
                    "T",
                    "rrr::Epoll@rrr.epoll_wrapper::Add(int, int)",
                ),
                (
                    "T",
                    "rrr::Epoll@rrr.epoll_wrapper::Remove(int)",
                ),
                (
                    "T",
                    "rrr::Epoll@rrr.epoll_wrapper::Update(int, int, int)",
                ),
                (
                    "T",
                    "rrr::Epoll@rrr.epoll_wrapper::fd() const",
                ),
                (
                    "T",
                    "rrr::Epoll@rrr.epoll_wrapper::new_()",
                ),
                (
                    "T",
                    "rrr::EpollWaitEvent@rrr.epoll_wrapper::default_()",
                ),
                (
                    "T",
                    "rrr::Pollable@rrr.epoll_wrapper::~Pollable()",
                ),
                (
                    "T",
                    "rrr::epoll_bump_remove_count@rrr.epoll_wrapper()",
                ),
            }
        ),
    ),
    "rrr.pollable_proxy": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.pollable_proxy;",
                "namespace rrr {",
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
                    "typeinfo for rrr::PollableBase@rrr.pollable_proxy",
                ),
                (
                    "D",
                    "vtable for rrr::PollableBase@rrr.pollable_proxy",
                ),
                (
                    "R",
                    "typeinfo name for rrr::PollableBase@rrr.pollable_proxy",
                ),
                (
                    "T",
                    "rrr::PollableBase@rrr.pollable_proxy::~PollableBase()",
                ),
            }
        ),
    ),
    "rrr.callbacks": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.callbacks;",
                "namespace rrr {",
                "import vec_port.vec;",
                "import rrr.errors;",
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
                    "rrr::CallbackManager@rrr.callbacks::add_on_connected(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::add_on_disconnected(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::add_on_error(rusty::Function<void (rrr::RpcError@rrr.errors, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const>) const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::add_on_reconnected(rusty::Function<void (bool) const>) const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::add_on_reconnecting(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::callback_count() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::clear_all() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::has_callbacks() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::inflight_enter() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::inflight_exit() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::invoke_on_connected() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::invoke_on_disconnected() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::invoke_on_error(rrr::RpcError@rrr.errors, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::invoke_on_reconnected(bool) const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::invoke_on_reconnecting() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::new_()",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::on_connected_count() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::on_disconnected_count() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::on_error_count() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::on_reconnected_count() const",
                ),
                (
                    "T",
                    "rrr::CallbackManager@rrr.callbacks::on_reconnecting_count() const",
                ),
                (
                    "T",
                    "rrr::ConnectionCallbacks@rrr.callbacks::clear()",
                ),
                (
                    "T",
                    "rrr::ConnectionCallbacks@rrr.callbacks::new_()",
                ),
                (
                    "T",
                    "rrr::ConnectionCallbacks@rrr.callbacks::total_count() const",
                ),
                (
                    "T",
                    "rrr::invoke_connection_callback_safely@rrr.callbacks(rusty::Arc<rusty::Function<void () const>> const&)",
                ),
                (
                    "T",
                    "rrr::invoke_error_callback_safely@rrr.callbacks(rusty::Arc<rusty::Function<void (rrr::RpcError@rrr.errors, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const>> const&, rrr::RpcError@rrr.errors, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::invoke_reconnect_callback_safely@rrr.callbacks(rusty::Arc<rusty::Function<void (bool) const>> const&, bool)",
                ),
            }
        ),
    ),
    "rrr.inmemory_channel": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.inmemory_channel;",
                "namespace rrr {",
                "import vec_port.vec;",
                "import std_port;",
                "import rrr.channel;",
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
                "export ::rrr::ChannelError inmemory_channel_send_frame(const InMemoryChannel& channel, const ::rrr::ChannelFrame& frame);",
                "export void inmemory_channel_inject_drop_next_sends(const InMemoryChannel& channel, int32_t count);",
                "export void inmemory_channel_inject_send_error(const InMemoryChannel& channel, ::rrr::ChannelError error, int32_t count);",
                "export void inmemory_channel_clear_fault_injection(const InMemoryChannel& channel);",
                "export ::rrr::ChannelConnectionProxy make_inmemory_channel_proxy(rusty::Arc<InMemoryChannel> connection);",
                "export rusty::Option<rusty::Arc<InMemoryChannel>> inmemory_listener_accept_for_connect(const InMemoryListener& listener, const std::string& client_address);",
                "export ::rrr::ChannelListenerProxy make_inmemory_listener_proxy(rusty::Arc<InMemoryListener> listener);",
                "export ::rrr::ConnectResult inmemory_factory_connect(const InMemoryFactory& factory, std::string_view address);",
                "export rusty::Option<::rrr::ChannelListenerProxy> inmemory_factory_make_listener(const InMemoryFactory& factory);",
                "export ::rrr::ChannelFactoryProxy make_inmemory_factory_proxy(rusty::Arc<InMemoryFactory> factory);",
                "export std::tuple<rusty::Arc<InMemoryChannel>, rusty::Arc<InMemoryChannel>> make_channel_pair_for_testing(std::string a_address, std::string b_address);",
            }
        ),
        symbols=frozenset(
            {
                (
                    "D",
                    "typeinfo for rrr::InMemoryChannelShim@rrr.inmemory_channel",
                ),
                (
                    "D",
                    "typeinfo for rrr::InMemoryFactoryShim@rrr.inmemory_channel",
                ),
                (
                    "D",
                    "typeinfo for rrr::InMemoryListenerShim@rrr.inmemory_channel",
                ),
                (
                    "D",
                    "vtable for rrr::InMemoryChannelShim@rrr.inmemory_channel",
                ),
                (
                    "D",
                    "vtable for rrr::InMemoryFactoryShim@rrr.inmemory_channel",
                ),
                (
                    "D",
                    "vtable for rrr::InMemoryListenerShim@rrr.inmemory_channel",
                ),
                (
                    "R",
                    "typeinfo name for rrr::InMemoryChannelShim@rrr.inmemory_channel",
                ),
                (
                    "R",
                    "typeinfo name for rrr::InMemoryFactoryShim@rrr.inmemory_channel",
                ),
                (
                    "R",
                    "typeinfo name for rrr::InMemoryListenerShim@rrr.inmemory_channel",
                ),
                (
                    "T",
                    "rrr::InMemoryChannel@rrr.inmemory_channel::close() const",
                ),
                (
                    "T",
                    "rrr::InMemoryChannel@rrr.inmemory_channel::flush() const",
                ),
                (
                    "T",
                    "rrr::InMemoryChannel@rrr.inmemory_channel::is_closed() const",
                ),
                (
                    "T",
                    "rrr::InMemoryChannel@rrr.inmemory_channel::new_(rusty::Arc<rrr::InMemoryConnectionState@rrr.inmemory_channel>, bool)",
                ),
                (
                    "T",
                    "rrr::InMemoryChannel@rrr.inmemory_channel::peer_address() const",
                ),
                (
                    "T",
                    "rrr::InMemoryChannel@rrr.inmemory_channel::send_frame(rrr::ChannelFrame@rrr.channel const&) const",
                ),
                (
                    "T",
                    "rrr::InMemoryChannel@rrr.inmemory_channel::set_on_closed(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel) const>>) const",
                ),
                (
                    "T",
                    "rrr::InMemoryChannel@rrr.inmemory_channel::set_on_error(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>) const",
                ),
                (
                    "T",
                    "rrr::InMemoryChannel@rrr.inmemory_channel::set_on_frame(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelFrame@rrr.channel const&) const>>) const",
                ),
                (
                    "T",
                    "rrr::InMemoryChannelShim@rrr.inmemory_channel::InMemoryChannelShim(rrr::InMemoryChannelShim@rrr.inmemory_channel&&)",
                ),
                (
                    "T",
                    "rrr::InMemoryChannelShim@rrr.inmemory_channel::InMemoryChannelShim(rusty::Arc<rrr::InMemoryChannel@rrr.inmemory_channel>)",
                ),
                (
                    "T",
                    "rrr::InMemoryChannelShim@rrr.inmemory_channel::close()",
                ),
                (
                    "T",
                    "rrr::InMemoryChannelShim@rrr.inmemory_channel::flush()",
                ),
                (
                    "T",
                    "rrr::InMemoryChannelShim@rrr.inmemory_channel::is_closed() const",
                ),
                (
                    "T",
                    "rrr::InMemoryChannelShim@rrr.inmemory_channel::peer_address() const",
                ),
                (
                    "T",
                    "rrr::InMemoryChannelShim@rrr.inmemory_channel::send_frame(rrr::ChannelFrame@rrr.channel const&)",
                ),
                (
                    "T",
                    "rrr::InMemoryChannelShim@rrr.inmemory_channel::set_on_closed(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel) const>>)",
                ),
                (
                    "T",
                    "rrr::InMemoryChannelShim@rrr.inmemory_channel::set_on_error(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>)",
                ),
                (
                    "T",
                    "rrr::InMemoryChannelShim@rrr.inmemory_channel::set_on_frame(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelFrame@rrr.channel const&) const>>)",
                ),
                (
                    "T",
                    "rrr::InMemoryFactory@rrr.inmemory_channel::backend_name() const",
                ),
                (
                    "T",
                    "rrr::InMemoryFactory@rrr.inmemory_channel::connect(std::__1::basic_string_view<char, std::__1::char_traits<char>>) const",
                ),
                (
                    "T",
                    "rrr::InMemoryFactory@rrr.inmemory_channel::make_listener() const",
                ),
                (
                    "T",
                    "rrr::InMemoryFactory@rrr.inmemory_channel::new_(rusty::Arc<rrr::InMemorySwitchboard@rrr.inmemory_channel>)",
                ),
                (
                    "T",
                    "rrr::InMemoryFactoryShim@rrr.inmemory_channel::InMemoryFactoryShim(rrr::InMemoryFactoryShim@rrr.inmemory_channel&&)",
                ),
                (
                    "T",
                    "rrr::InMemoryFactoryShim@rrr.inmemory_channel::InMemoryFactoryShim(rusty::Arc<rrr::InMemoryFactory@rrr.inmemory_channel>)",
                ),
                (
                    "T",
                    "rrr::InMemoryFactoryShim@rrr.inmemory_channel::backend_name() const",
                ),
                (
                    "T",
                    "rrr::InMemoryFactoryShim@rrr.inmemory_channel::connect(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::InMemoryFactoryShim@rrr.inmemory_channel::make_listener()",
                ),
                (
                    "T",
                    "rrr::InMemoryListener@rrr.inmemory_channel::close() const",
                ),
                (
                    "T",
                    "rrr::InMemoryListener@rrr.inmemory_channel::is_closed() const",
                ),
                (
                    "T",
                    "rrr::InMemoryListener@rrr.inmemory_channel::listen(std::__1::basic_string_view<char, std::__1::char_traits<char>>) const",
                ),
                (
                    "T",
                    "rrr::InMemoryListener@rrr.inmemory_channel::local_address() const",
                ),
                (
                    "T",
                    "rrr::InMemoryListener@rrr.inmemory_channel::new_(rusty::Arc<rrr::InMemorySwitchboard@rrr.inmemory_channel>)",
                ),
                (
                    "T",
                    "rrr::InMemoryListener@rrr.inmemory_channel::set_on_accept(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>) const>>) const",
                ),
                (
                    "T",
                    "rrr::InMemoryListener@rrr.inmemory_channel::set_on_error(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>) const",
                ),
                (
                    "T",
                    "rrr::InMemoryListener@rrr.inmemory_channel::set_self_weak(rusty::sync::Weak<rrr::InMemoryListener@rrr.inmemory_channel>)",
                ),
                (
                    "T",
                    "rrr::InMemoryListenerShim@rrr.inmemory_channel::InMemoryListenerShim(rrr::InMemoryListenerShim@rrr.inmemory_channel&&)",
                ),
                (
                    "T",
                    "rrr::InMemoryListenerShim@rrr.inmemory_channel::InMemoryListenerShim(rusty::Arc<rrr::InMemoryListener@rrr.inmemory_channel>)",
                ),
                (
                    "T",
                    "rrr::InMemoryListenerShim@rrr.inmemory_channel::close()",
                ),
                (
                    "T",
                    "rrr::InMemoryListenerShim@rrr.inmemory_channel::is_closed() const",
                ),
                (
                    "T",
                    "rrr::InMemoryListenerShim@rrr.inmemory_channel::listen(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::InMemoryListenerShim@rrr.inmemory_channel::local_address() const",
                ),
                (
                    "T",
                    "rrr::InMemoryListenerShim@rrr.inmemory_channel::set_on_accept(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>) const>>)",
                ),
                (
                    "T",
                    "rrr::InMemoryListenerShim@rrr.inmemory_channel::set_on_error(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>)",
                ),
                (
                    "T",
                    "rrr::InMemorySwitchboard@rrr.inmemory_channel::find_listener(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "rrr::InMemorySwitchboard@rrr.inmemory_channel::new_()",
                ),
                (
                    "T",
                    "rrr::InMemorySwitchboard@rrr.inmemory_channel::register_listener(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, rusty::sync::Weak<rrr::InMemoryListener@rrr.inmemory_channel>) const",
                ),
                (
                    "T",
                    "rrr::InMemorySwitchboard@rrr.inmemory_channel::unregister_listener(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "rrr::channel_error_address_in_use@rrr.inmemory_channel()",
                ),
                (
                    "T",
                    "rrr::channel_error_connection_reset@rrr.inmemory_channel()",
                ),
                (
                    "T",
                    "rrr::channel_error_from_code@rrr.inmemory_channel(int)",
                ),
                (
                    "T",
                    "rrr::channel_error_internal@rrr.inmemory_channel()",
                ),
                (
                    "T",
                    "rrr::channel_error_none@rrr.inmemory_channel()",
                ),
                (
                    "T",
                    "rrr::empty_connection_inner@rrr.inmemory_channel()",
                ),
                (
                    "T",
                    "rrr::empty_listener_inner@rrr.inmemory_channel()",
                ),
                (
                    "T",
                    "rrr::inmemory_channel_clear_fault_injection@rrr.inmemory_channel(rrr::InMemoryChannel@rrr.inmemory_channel const&)",
                ),
                (
                    "T",
                    "rrr::inmemory_channel_inject_drop_next_sends@rrr.inmemory_channel(rrr::InMemoryChannel@rrr.inmemory_channel const&, int)",
                ),
                (
                    "T",
                    "rrr::inmemory_channel_inject_send_error@rrr.inmemory_channel(rrr::InMemoryChannel@rrr.inmemory_channel const&, rrr::ChannelError@rrr.channel, int)",
                ),
                (
                    "T",
                    "rrr::inmemory_channel_send_frame@rrr.inmemory_channel(rrr::InMemoryChannel@rrr.inmemory_channel const&, rrr::ChannelFrame@rrr.channel const&)",
                ),
                (
                    "T",
                    "rrr::inmemory_factory_connect@rrr.inmemory_channel(rrr::InMemoryFactory@rrr.inmemory_channel const&, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::inmemory_factory_make_listener@rrr.inmemory_channel(rrr::InMemoryFactory@rrr.inmemory_channel const&)",
                ),
                (
                    "T",
                    "rrr::inmemory_listener_accept_for_connect@rrr.inmemory_channel(rrr::InMemoryListener@rrr.inmemory_channel const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::inmemory_listener_listen_with_weak@rrr.inmemory_channel(rrr::InMemoryListener@rrr.inmemory_channel const&, std::__1::basic_string_view<char, std::__1::char_traits<char>>, rusty::Option<rusty::sync::Weak<rrr::InMemoryListener@rrr.inmemory_channel>>)",
                ),
                (
                    "T",
                    "rrr::make_channel_pair_for_testing@rrr.inmemory_channel(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "rrr::make_connection_state@rrr.inmemory_channel(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "rrr::make_inmemory_channel_proxy@rrr.inmemory_channel(rusty::Arc<rrr::InMemoryChannel@rrr.inmemory_channel>)",
                ),
                (
                    "T",
                    "rrr::make_inmemory_factory_proxy@rrr.inmemory_channel(rusty::Arc<rrr::InMemoryFactory@rrr.inmemory_channel>)",
                ),
                (
                    "T",
                    "rrr::make_inmemory_listener_proxy@rrr.inmemory_channel(rusty::Arc<rrr::InMemoryListener@rrr.inmemory_channel>)",
                ),
            }
        ),
    ),
    "rrr.fiber_channel": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.fiber_channel;",
                "namespace rrr {",
                "import vec_port.vec;",
                "import rrr.channel;",
                "import rrr.reactor;",
                "export struct OwnedFrame;",
                "export struct FiberChannel;",
            }
        ),
        symbols=frozenset(
            {
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::arm_waiter()",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::bind_callbacks()",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::channel_for_test()",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::close()",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::is_closed() const",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::new_(rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::on_inbound_closed()",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::on_inbound_frame(rrr::ChannelFrame@rrr.channel const&)",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::recv_frame()",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::send_frame(rrr::ChannelFrame@rrr.channel const&)",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::signal_pending_recv()",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::try_pop()",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::wait_for_signal()",
                ),
                (
                    "T",
                    "rrr::FiberChannel@rrr.fiber_channel::~FiberChannel()",
                ),
                (
                    "T",
                    "rrr::OwnedFrame@rrr.fiber_channel::default_()",
                ),
                (
                    "T",
                    "rrr::fiberchannel_owned_copy@rrr.fiber_channel(rrr::ChannelFrame@rrr.channel const&)",
                ),
            }
        ),
    ),
    "rrr.threading": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.threading;",
                "namespace rrr {",
                "import rrr.debugging;",
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
                    "rrr::Pthread_cond_broadcast@rrr.threading(pthread_cond_t*)",
                ),
                (
                    "T",
                    "rrr::Pthread_cond_destroy@rrr.threading(pthread_cond_t*)",
                ),
                (
                    "T",
                    "rrr::Pthread_cond_init@rrr.threading(pthread_cond_t*, pthread_condattr_t const*)",
                ),
                (
                    "T",
                    "rrr::Pthread_cond_signal@rrr.threading(pthread_cond_t*)",
                ),
                (
                    "T",
                    "rrr::Pthread_cond_wait@rrr.threading(pthread_cond_t*, pthread_mutex_t*)",
                ),
                (
                    "T",
                    "rrr::Pthread_mutex_destroy@rrr.threading(pthread_mutex_t*)",
                ),
                (
                    "T",
                    "rrr::Pthread_mutex_init@rrr.threading(pthread_mutex_t*, pthread_mutexattr_t const*)",
                ),
                (
                    "T",
                    "rrr::Pthread_mutex_lock@rrr.threading(pthread_mutex_t*)",
                ),
                (
                    "T",
                    "rrr::Pthread_mutex_unlock@rrr.threading(pthread_mutex_t*)",
                ),
                (
                    "T",
                    "rrr::Pthread_spin_destroy@rrr.threading(int volatile*)",
                ),
                (
                    "T",
                    "rrr::Pthread_spin_init@rrr.threading(int volatile*, int)",
                ),
                (
                    "T",
                    "rrr::Pthread_spin_lock@rrr.threading(int volatile*)",
                ),
                (
                    "T",
                    "rrr::Pthread_spin_unlock@rrr.threading(int volatile*)",
                ),
                (
                    "T",
                    "rrr::SpinLock@rrr.threading::lock() const",
                ),
                (
                    "T",
                    "rrr::SpinLock@rrr.threading::new_()",
                ),
                (
                    "T",
                    "rrr::SpinLock@rrr.threading::unlock() const",
                ),
                (
                    "T",
                    "rrr::cpu_pause@rrr.threading()",
                ),
            }
        ),
    ),
    "rrr.any_message": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.any_message;",
                "namespace rrr {",
                "import std_port;",
                "import rrr.debugging;",
                "import rrr.serializable;",
                "export struct AnyMessage;",
                "export using Factory = rusty::Function<rusty::Arc<SerializableBase>()>;",
                "export int32_t register_type(std::string name, std::type_index type_id, ::rrr::any_message_registry::Factory factory);",
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
                    "rrr::AnyMessage@rrr.any_message::load(rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::AnyMessage@rrr.any_message::save(rrr::BinaryWriteArchive@rrr.serializable&) const",
                ),
                (
                    "T",
                    "rrr::any_message_registry::clear_for_testing@rrr.any_message()",
                ),
                (
                    "T",
                    "rrr::any_message_registry::create@rrr.any_message(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::any_message_registry::is_registered_name@rrr.any_message(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::any_message_registry::is_registered_type@rrr.any_message(std::__1::type_index)",
                ),
                (
                    "T",
                    "rrr::any_message_registry::name_for_type_owned@rrr.any_message(std::__1::type_index)",
                ),
                (
                    "T",
                    "rrr::any_message_registry::register_type@rrr.any_message(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, std::__1::type_index, rusty::Function<rusty::Arc<rrr::SerializableBase@rrr.serializable> ()>)",
                ),
                (
                    "T",
                    "rrr::deserialize@rrr.any_message(rrr::AnyMessage@rrr.any_message&, rrr::BinaryReadArchive@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::serialize@rrr.any_message(rrr::AnyMessage@rrr.any_message const&, rrr::BinaryWriteArchive@rrr.serializable&)",
                ),
            }
        ),
    ),
    "rrr.tcp_channel": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.tcp_channel;",
                "namespace rrr {",
                "import rrr.channel;",
                "import rrr.frame_codec;",
                "import rrr.pollable_proxy;",
                "import rrr.reactor;",
                "export struct TcpConnection;",
                "export struct TcpListener;",
                "export struct TcpFactory;",
                "export constexpr size_t kTcpConnectionOutboundHighWaterDefault = (static_cast<size_t>(4) * static_cast<size_t>(1024)) * static_cast<size_t>(1024);",
                "export ::rrr::ChannelConnectionProxy make_tcp_connection_channel_proxy(rusty::Arc<TcpConnection> conn);",
                "export ::rrr::ChannelListenerProxy make_tcp_listener_channel_proxy(rusty::Arc<TcpListener> listener);",
                "export ::rrr::ChannelFactoryProxy make_tcp_factory_proxy(rusty::Arc<TcpFactory> factory);",
                "export ::rrr::ConnectResult tcp_factory_connect(const TcpFactory& fac, std::string_view addr);",
                "export rusty::Option<::rrr::ChannelListenerProxy> tcp_factory_make_listener(const TcpFactory& self_);",
            }
        ),
        symbols=frozenset(
            {
                (
                    "D",
                    "typeinfo for rrr::TcpChannelShim@rrr.tcp_channel",
                ),
                (
                    "D",
                    "typeinfo for rrr::TcpFactoryShim@rrr.tcp_channel",
                ),
                (
                    "D",
                    "typeinfo for rrr::TcpListenerChannelShim@rrr.tcp_channel",
                ),
                (
                    "D",
                    "typeinfo for rrr::TcpListenerPollableShim@rrr.tcp_channel",
                ),
                (
                    "D",
                    "typeinfo for rrr::TcpPollableShim@rrr.tcp_channel",
                ),
                (
                    "D",
                    "vtable for rrr::TcpChannelShim@rrr.tcp_channel",
                ),
                (
                    "D",
                    "vtable for rrr::TcpFactoryShim@rrr.tcp_channel",
                ),
                (
                    "D",
                    "vtable for rrr::TcpListenerChannelShim@rrr.tcp_channel",
                ),
                (
                    "D",
                    "vtable for rrr::TcpListenerPollableShim@rrr.tcp_channel",
                ),
                (
                    "D",
                    "vtable for rrr::TcpPollableShim@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_ACCES@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_ADDR_IN_USE@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_ADDR_NOT_AVAILABLE@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_AGAIN@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_BROKEN_PIPE@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_CONNECTION_REFUSED@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_CONNECTION_RESET@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_HOST_UNREACHABLE@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_INTERRUPTED@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_NETWORK_UNREACHABLE@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_NOT_CONNECTED@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_OPERATION_NOT_PERMITTED@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_PROCESS_FD_LIMIT@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_SYSTEM_FD_LIMIT@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_TIMED_OUT@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_ERR_WOULD_BLOCK@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_MAX_FRAME_PAYLOAD_SIZE@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_POLL_NO_CHANGE@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_POLL_READ@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::TCP_POLL_WRITE@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::kRecvScratchBytes@rrr.tcp_channel",
                ),
                (
                    "R",
                    "rrr::kTcpConnectionOutboundHighWaterDefault@rrr.tcp_channel",
                ),
                (
                    "R",
                    "typeinfo name for rrr::TcpChannelShim@rrr.tcp_channel",
                ),
                (
                    "R",
                    "typeinfo name for rrr::TcpFactoryShim@rrr.tcp_channel",
                ),
                (
                    "R",
                    "typeinfo name for rrr::TcpListenerChannelShim@rrr.tcp_channel",
                ),
                (
                    "R",
                    "typeinfo name for rrr::TcpListenerPollableShim@rrr.tcp_channel",
                ),
                (
                    "R",
                    "typeinfo name for rrr::TcpPollableShim@rrr.tcp_channel",
                ),
                (
                    "T",
                    "rrr::TcpChannelShim@rrr.tcp_channel::TcpChannelShim(rrr::TcpChannelShim@rrr.tcp_channel&&)",
                ),
                (
                    "T",
                    "rrr::TcpChannelShim@rrr.tcp_channel::TcpChannelShim(rusty::Arc<rrr::TcpConnection@rrr.tcp_channel>)",
                ),
                (
                    "T",
                    "rrr::TcpChannelShim@rrr.tcp_channel::close()",
                ),
                (
                    "T",
                    "rrr::TcpChannelShim@rrr.tcp_channel::flush()",
                ),
                (
                    "T",
                    "rrr::TcpChannelShim@rrr.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "rrr::TcpChannelShim@rrr.tcp_channel::peer_address() const",
                ),
                (
                    "T",
                    "rrr::TcpChannelShim@rrr.tcp_channel::send_frame(rrr::ChannelFrame@rrr.channel const&)",
                ),
                (
                    "T",
                    "rrr::TcpChannelShim@rrr.tcp_channel::set_on_closed(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel) const>>)",
                ),
                (
                    "T",
                    "rrr::TcpChannelShim@rrr.tcp_channel::set_on_error(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>)",
                ),
                (
                    "T",
                    "rrr::TcpChannelShim@rrr.tcp_channel::set_on_frame(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelFrame@rrr.channel const&) const>>)",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::check_pending_write_update() const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::close() const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::content_size() const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::fd() const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::flush() const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::handle_error() const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::handle_read() const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::handle_write() const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::new_(int, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::peer_address() const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::poll_mode() const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::send_frame(rrr::ChannelFrame@rrr.channel const&) const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::set_on_closed(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel) const>>) const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::set_on_error(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>) const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::set_on_frame(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelFrame@rrr.channel const&) const>>) const",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::set_outbound_high_water(unsigned long)",
                ),
                (
                    "T",
                    "rrr::TcpConnection@rrr.tcp_channel::set_poll_thread(rusty::Arc<rrr::PollThread@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::TcpFactory@rrr.tcp_channel::backend_name() const",
                ),
                (
                    "T",
                    "rrr::TcpFactory@rrr.tcp_channel::connect(std::__1::basic_string_view<char, std::__1::char_traits<char>>) const",
                ),
                (
                    "T",
                    "rrr::TcpFactory@rrr.tcp_channel::make_listener() const",
                ),
                (
                    "T",
                    "rrr::TcpFactory@rrr.tcp_channel::new_(rusty::Arc<rrr::PollThread@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::TcpFactory@rrr.tcp_channel::set_connect_timeout_ms(int)",
                ),
                (
                    "T",
                    "rrr::TcpFactoryShim@rrr.tcp_channel::TcpFactoryShim(rrr::TcpFactoryShim@rrr.tcp_channel&&)",
                ),
                (
                    "T",
                    "rrr::TcpFactoryShim@rrr.tcp_channel::TcpFactoryShim(rusty::Arc<rrr::TcpFactory@rrr.tcp_channel>)",
                ),
                (
                    "T",
                    "rrr::TcpFactoryShim@rrr.tcp_channel::backend_name() const",
                ),
                (
                    "T",
                    "rrr::TcpFactoryShim@rrr.tcp_channel::connect(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::TcpFactoryShim@rrr.tcp_channel::make_listener()",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::check_pending_write_update() const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::close() const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::content_size() const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::fd() const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::handle_error() const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::handle_read() const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::handle_write() const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::listen(std::__1::basic_string_view<char, std::__1::char_traits<char>>) const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::local_address() const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::new_()",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::poll_mode() const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::set_on_accept(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>) const>>) const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::set_on_error(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>) const",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::set_poll_thread(rusty::Arc<rrr::PollThread@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::TcpListener@rrr.tcp_channel::set_self_weak(rusty::sync::Weak<rrr::TcpListener@rrr.tcp_channel>)",
                ),
                (
                    "T",
                    "rrr::TcpListenerChannelShim@rrr.tcp_channel::TcpListenerChannelShim(rrr::TcpListenerChannelShim@rrr.tcp_channel&&)",
                ),
                (
                    "T",
                    "rrr::TcpListenerChannelShim@rrr.tcp_channel::TcpListenerChannelShim(rusty::Arc<rrr::TcpListener@rrr.tcp_channel>)",
                ),
                (
                    "T",
                    "rrr::TcpListenerChannelShim@rrr.tcp_channel::close()",
                ),
                (
                    "T",
                    "rrr::TcpListenerChannelShim@rrr.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "rrr::TcpListenerChannelShim@rrr.tcp_channel::listen(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::TcpListenerChannelShim@rrr.tcp_channel::local_address() const",
                ),
                (
                    "T",
                    "rrr::TcpListenerChannelShim@rrr.tcp_channel::set_on_accept(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>) const>>)",
                ),
                (
                    "T",
                    "rrr::TcpListenerChannelShim@rrr.tcp_channel::set_on_error(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rrr::ChannelError@rrr.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>) const>>)",
                ),
                (
                    "T",
                    "rrr::TcpListenerHandleReadScope@rrr.tcp_channel::TcpListenerHandleReadScope(rrr::TcpListenerHandleReadScope@rrr.tcp_channel&&)",
                ),
                (
                    "T",
                    "rrr::TcpListenerHandleReadScope@rrr.tcp_channel::TcpListenerHandleReadScope(rusty::sync::atomic::detail::Atomic<unsigned int> const*, bool)",
                ),
                (
                    "T",
                    "rrr::TcpListenerHandleReadScope@rrr.tcp_channel::acquired() const",
                ),
                (
                    "T",
                    "rrr::TcpListenerHandleReadScope@rrr.tcp_channel::new_(rrr::TcpListener@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::TcpListenerHandleReadScope@rrr.tcp_channel::operator=(rrr::TcpListenerHandleReadScope@rrr.tcp_channel&&)",
                ),
                (
                    "T",
                    "rrr::TcpListenerHandleReadScope@rrr.tcp_channel::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "rrr::TcpListenerHandleReadScope@rrr.tcp_channel::~TcpListenerHandleReadScope()",
                ),
                (
                    "T",
                    "rrr::TcpListenerPollableShim@rrr.tcp_channel::TcpListenerPollableShim(rrr::TcpListenerPollableShim@rrr.tcp_channel&&)",
                ),
                (
                    "T",
                    "rrr::TcpListenerPollableShim@rrr.tcp_channel::TcpListenerPollableShim(rusty::Arc<rrr::TcpListener@rrr.tcp_channel>)",
                ),
                (
                    "T",
                    "rrr::TcpListenerPollableShim@rrr.tcp_channel::check_pending_write_update() const",
                ),
                (
                    "T",
                    "rrr::TcpListenerPollableShim@rrr.tcp_channel::close()",
                ),
                (
                    "T",
                    "rrr::TcpListenerPollableShim@rrr.tcp_channel::content_size()",
                ),
                (
                    "T",
                    "rrr::TcpListenerPollableShim@rrr.tcp_channel::fd() const",
                ),
                (
                    "T",
                    "rrr::TcpListenerPollableShim@rrr.tcp_channel::handle_error()",
                ),
                (
                    "T",
                    "rrr::TcpListenerPollableShim@rrr.tcp_channel::handle_read()",
                ),
                (
                    "T",
                    "rrr::TcpListenerPollableShim@rrr.tcp_channel::handle_write()",
                ),
                (
                    "T",
                    "rrr::TcpListenerPollableShim@rrr.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "rrr::TcpListenerPollableShim@rrr.tcp_channel::poll_mode() const",
                ),
                (
                    "T",
                    "rrr::TcpPollableShim@rrr.tcp_channel::TcpPollableShim(rrr::TcpPollableShim@rrr.tcp_channel&&)",
                ),
                (
                    "T",
                    "rrr::TcpPollableShim@rrr.tcp_channel::TcpPollableShim(rusty::Arc<rrr::TcpConnection@rrr.tcp_channel>)",
                ),
                (
                    "T",
                    "rrr::TcpPollableShim@rrr.tcp_channel::check_pending_write_update() const",
                ),
                (
                    "T",
                    "rrr::TcpPollableShim@rrr.tcp_channel::close()",
                ),
                (
                    "T",
                    "rrr::TcpPollableShim@rrr.tcp_channel::content_size()",
                ),
                (
                    "T",
                    "rrr::TcpPollableShim@rrr.tcp_channel::fd() const",
                ),
                (
                    "T",
                    "rrr::TcpPollableShim@rrr.tcp_channel::handle_error()",
                ),
                (
                    "T",
                    "rrr::TcpPollableShim@rrr.tcp_channel::handle_read()",
                ),
                (
                    "T",
                    "rrr::TcpPollableShim@rrr.tcp_channel::handle_write()",
                ),
                (
                    "T",
                    "rrr::TcpPollableShim@rrr.tcp_channel::is_closed() const",
                ),
                (
                    "T",
                    "rrr::TcpPollableShim@rrr.tcp_channel::poll_mode() const",
                ),
                (
                    "T",
                    "rrr::connect_errno_to_channel_error@rrr.tcp_channel(int)",
                ),
                (
                    "T",
                    "rrr::io_kind_to_channel_error@rrr.tcp_channel(rusty::io::Error::Kind)",
                ),
                (
                    "T",
                    "rrr::make_tcp_connection_channel_proxy@rrr.tcp_channel(rusty::Arc<rrr::TcpConnection@rrr.tcp_channel>)",
                ),
                (
                    "T",
                    "rrr::make_tcp_connection_pollable_proxy@rrr.tcp_channel(rusty::Arc<rrr::TcpConnection@rrr.tcp_channel>)",
                ),
                (
                    "T",
                    "rrr::make_tcp_factory_proxy@rrr.tcp_channel(rusty::Arc<rrr::TcpFactory@rrr.tcp_channel>)",
                ),
                (
                    "T",
                    "rrr::make_tcp_listener_channel_proxy@rrr.tcp_channel(rusty::Arc<rrr::TcpListener@rrr.tcp_channel>)",
                ),
                (
                    "T",
                    "rrr::make_tcp_listener_pollable_proxy@rrr.tcp_channel(rusty::Arc<rrr::TcpListener@rrr.tcp_channel>)",
                ),
                (
                    "T",
                    "rrr::set_nonblocking_fd@rrr.tcp_channel(int)",
                ),
                (
                    "T",
                    "rrr::tcp_factory_connect@rrr.tcp_channel(rrr::TcpFactory@rrr.tcp_channel const&, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::tcp_factory_connect_socket@rrr.tcp_channel(rusty::net::SocketAddrV4, int, rrr::ChannelError@rrr.channel&)",
                ),
                (
                    "T",
                    "rrr::tcp_factory_make_listener@rrr.tcp_channel(rrr::TcpFactory@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_append_inbound@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&, unsigned long)",
                ),
                (
                    "T",
                    "rrr::tcpconn_close@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_consume_inbound@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_deliver_on_closed_locked@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&, rrr::ChannelError@rrr.channel)",
                ),
                (
                    "T",
                    "rrr::tcpconn_drain_outbound_locked@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&, std::__1::vector<unsigned char, std::__1::allocator<unsigned char>>&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_drop_after_error@rrr.tcp_channel(std::__1::vector<unsigned char, std::__1::allocator<unsigned char>>&, unsigned long)",
                ),
                (
                    "T",
                    "rrr::tcpconn_errno_to_channel_error@rrr.tcp_channel(int)",
                ),
                (
                    "T",
                    "rrr::tcpconn_flush@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_handle_error@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_handle_read@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_handle_write@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_last_errno@rrr.tcp_channel()",
                ),
                (
                    "T",
                    "rrr::tcpconn_next_frame@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&, rrr::FrameView@rrr.frame_codec&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_recv_bytes@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&, rrr::RecvScratch@rrr.tcp_channel*)",
                ),
                (
                    "T",
                    "rrr::tcpconn_reset_fd@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_reset_inbound@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_scratch@rrr.tcp_channel()",
                ),
                (
                    "T",
                    "rrr::tcpconn_send_bytes@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&, std::__1::vector<unsigned char, std::__1::allocator<unsigned char>>&, unsigned long)",
                ),
                (
                    "T",
                    "rrr::tcpconn_send_frame@rrr.tcp_channel(rrr::TcpConnection@rrr.tcp_channel const&, rrr::ChannelFrame@rrr.channel const&)",
                ),
                (
                    "T",
                    "rrr::tcpconn_trim_sent@rrr.tcp_channel(std::__1::vector<unsigned char, std::__1::allocator<unsigned char>>&, unsigned long)",
                ),
                (
                    "T",
                    "rrr::tcplistener_accept_step@rrr.tcp_channel(rrr::TcpListener@rrr.tcp_channel const&, rrr::AcceptStep@rrr.tcp_channel*)",
                ),
                (
                    "T",
                    "rrr::tcplistener_accept_step_new@rrr.tcp_channel()",
                ),
                (
                    "T",
                    "rrr::tcplistener_close_accepted@rrr.tcp_channel(rrr::AcceptStep@rrr.tcp_channel&)",
                ),
                (
                    "T",
                    "rrr::tcplistener_handle_error@rrr.tcp_channel(rrr::TcpListener@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcplistener_handle_read@rrr.tcp_channel(rrr::TcpListener@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcplistener_is_bound@rrr.tcp_channel(rrr::TcpListener@rrr.tcp_channel const&)",
                ),
                (
                    "T",
                    "rrr::tcplistener_take_proxy@rrr.tcp_channel(rrr::AcceptStep@rrr.tcp_channel&)",
                ),
            }
        ),
    ),
    "rrr.reactor": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.reactor;",
                "namespace rrr {",
                "import vec_port.vec;",
                "import rc_port;",
                "import btree_port.btree.map;",
                "import btree_port.btree.set;",
                "import std_port;",
                "import rrr.basetypes;",
                "import rrr.debugging;",
                "import rrr.epoll_wrapper;",
                "import rrr.logging;",
                "import rrr.misc;",
                "import rrr.pollable_proxy;",
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
                "export using FdPollableMap = rusty::HashMap<int32_t, ::rrr::PollableProxy>;",
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
                "export extern thread_local rusty::HashMap<rusty::String, rusty::Vec<::rrr::PollableProxy>> reactor_clients_th_;",
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
                    "typeinfo for janus::QuorumEvent@rrr.reactor",
                ),
                (
                    "D",
                    "typeinfo for rrr::EventPollable@rrr.reactor",
                ),
                (
                    "D",
                    "typeinfo for rrr::IntEvent@rrr.reactor",
                ),
                (
                    "D",
                    "typeinfo for rrr::NeverEvent@rrr.reactor",
                ),
                (
                    "D",
                    "typeinfo for rrr::TimeoutEvent@rrr.reactor",
                ),
                (
                    "D",
                    "typeinfo for rrr::WaitAll@rrr.reactor",
                ),
                (
                    "D",
                    "typeinfo for rrr::WaitAny@rrr.reactor",
                ),
                (
                    "D",
                    "vtable for janus::QuorumEvent@rrr.reactor",
                ),
                (
                    "D",
                    "vtable for rrr::EventPollable@rrr.reactor",
                ),
                (
                    "D",
                    "vtable for rrr::IntEvent@rrr.reactor",
                ),
                (
                    "D",
                    "vtable for rrr::NeverEvent@rrr.reactor",
                ),
                (
                    "D",
                    "vtable for rrr::TimeoutEvent@rrr.reactor",
                ),
                (
                    "D",
                    "vtable for rrr::WaitAll@rrr.reactor",
                ),
                (
                    "D",
                    "vtable for rrr::WaitAny@rrr.reactor",
                ),
                (
                    "R",
                    "rrr::STACKLESS_UNREGISTERED_SLOT@rrr.reactor",
                ),
                (
                    "R",
                    "rrr::kDefaultStackBytes@rrr.reactor",
                ),
                (
                    "R",
                    "typeinfo name for janus::QuorumEvent@rrr.reactor",
                ),
                (
                    "R",
                    "typeinfo name for rrr::EventPollable@rrr.reactor",
                ),
                (
                    "R",
                    "typeinfo name for rrr::IntEvent@rrr.reactor",
                ),
                (
                    "R",
                    "typeinfo name for rrr::NeverEvent@rrr.reactor",
                ),
                (
                    "R",
                    "typeinfo name for rrr::TimeoutEvent@rrr.reactor",
                ),
                (
                    "R",
                    "typeinfo name for rrr::WaitAll@rrr.reactor",
                ),
                (
                    "R",
                    "typeinfo name for rrr::WaitAny@rrr.reactor",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::QuorumEvent(janus::QuorumEvent@rrr.reactor&&)",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::QuorumEvent(rusty::Cell<rrr::EventStatus@rrr.reactor>, rusty::thread::ThreadId, rrr::EventState@rrr.reactor, rusty::Cell<bool>, rusty::sync::Weak<rrr::EventPollable@rrr.reactor>, rusty::Cell<int>, rusty::Cell<int>, rusty::RefCell<std_port::collections::hash::map::HashMap@std_port<unsigned short, long, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>>, int, int, rusty::Cell<janus::QuorumPolicy@rrr.reactor>, rusty::Cell<bool>, rusty::Cell<int>, rusty::Cell<int>, rusty::Cell<int>, rusty::Cell<long>, rusty::Cell<bool>, rusty::Cell<unsigned int>, rusty::Cell<long>, rusty::Cell<unsigned long>, rusty::Arc<rrr::IntEvent@rrr.reactor>)",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::add_xid(unsigned short, long) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::finalize(unsigned long, rusty::Function<bool (rusty::port::vec::Vec@vec_port.vec<std::__1::pair<unsigned short, long>, rusty::alloc::Global>&)>) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::get_fiber_id() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::get_self() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::is_ready() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::is_slow() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::log() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::no() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::prunable() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::remove_xid(unsigned short) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::set_self(rusty::sync::Weak<rrr::EventPollable@rrr.reactor>)",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::set_status(rrr::EventStatus@rrr.reactor) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::status() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::test() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::vote_no() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::vote_yes() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::wait() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "janus::QuorumEvent@rrr.reactor::yes() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::add_xid(unsigned short, long) const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::finalize(unsigned long, rusty::Function<bool (rusty::port::vec::Vec@vec_port.vec<std::__1::pair<unsigned short, long>, rusty::alloc::Global>&)>) const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::get_fiber_id() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::is_ready() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::is_slow() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::log() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::new_(int, int)",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::no() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::q() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::remove_xid(unsigned short) const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::test() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::vote_no() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::vote_yes() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::wait() const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "janus::QuorumEventWrapper@rrr.reactor::yes() const",
                ),
                (
                    "T",
                    "janus::create_sp_quorum_event@rrr.reactor(int, int)",
                ),
                (
                    "T",
                    "janus::quorum_collect_dangling@rrr.reactor(janus::QuorumEvent@rrr.reactor const*)",
                ),
                (
                    "T",
                    "janus::quorum_event_finalize@rrr.reactor(janus::QuorumEvent@rrr.reactor const&, unsigned long, rusty::Function<bool (rusty::port::vec::Vec@vec_port.vec<std::__1::pair<unsigned short, long>, rusty::alloc::Global>&)>)",
                ),
                (
                    "T",
                    "janus::quorum_event_is_slow@rrr.reactor(janus::QuorumEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "janus::quorum_event_make@rrr.reactor(int, int)",
                ),
                (
                    "T",
                    "rrr::AddJob@rrr.reactor(rusty::Arc<rrr::Job@rrr.misc>)",
                ),
                (
                    "T",
                    "rrr::AddPollable@rrr.reactor(rusty::Box<rrr::PollableBase@rrr.pollable_proxy, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "rrr::ClosePollable@rrr.reactor(int)",
                ),
                (
                    "T",
                    "rrr::EventPollable@rrr.reactor::~EventPollable()",
                ),
                (
                    "T",
                    "rrr::EventPollable_::is_ready@rrr.reactor(janus::QuorumEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::is_ready@rrr.reactor(rrr::IntEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::is_ready@rrr.reactor(rrr::NeverEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::is_ready@rrr.reactor(rrr::TimeoutEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::is_ready@rrr.reactor(rrr::WaitAll@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::is_ready@rrr.reactor(rrr::WaitAny@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::log@rrr.reactor(janus::QuorumEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::log@rrr.reactor(rrr::IntEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::log@rrr.reactor(rrr::NeverEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::log@rrr.reactor(rrr::TimeoutEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::log@rrr.reactor(rrr::WaitAll@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::log@rrr.reactor(rrr::WaitAny@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::prunable@rrr.reactor(janus::QuorumEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::prunable@rrr.reactor(rrr::IntEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::prunable@rrr.reactor(rrr::NeverEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::prunable@rrr.reactor(rrr::TimeoutEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::prunable@rrr.reactor(rrr::WaitAll@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::prunable@rrr.reactor(rrr::WaitAny@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_prunable@rrr.reactor(janus::QuorumEvent@rrr.reactor const&, bool)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_prunable@rrr.reactor(rrr::IntEvent@rrr.reactor const&, bool)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_prunable@rrr.reactor(rrr::NeverEvent@rrr.reactor const&, bool)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_prunable@rrr.reactor(rrr::TimeoutEvent@rrr.reactor const&, bool)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_prunable@rrr.reactor(rrr::WaitAll@rrr.reactor const&, bool)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_prunable@rrr.reactor(rrr::WaitAny@rrr.reactor const&, bool)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_status@rrr.reactor(janus::QuorumEvent@rrr.reactor const&, rrr::EventStatus@rrr.reactor)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_status@rrr.reactor(rrr::IntEvent@rrr.reactor const&, rrr::EventStatus@rrr.reactor)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_status@rrr.reactor(rrr::NeverEvent@rrr.reactor const&, rrr::EventStatus@rrr.reactor)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_status@rrr.reactor(rrr::TimeoutEvent@rrr.reactor const&, rrr::EventStatus@rrr.reactor)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_status@rrr.reactor(rrr::WaitAll@rrr.reactor const&, rrr::EventStatus@rrr.reactor)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::set_status@rrr.reactor(rrr::WaitAny@rrr.reactor const&, rrr::EventStatus@rrr.reactor)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::status@rrr.reactor(janus::QuorumEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::status@rrr.reactor(rrr::IntEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::status@rrr.reactor(rrr::NeverEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::status@rrr.reactor(rrr::TimeoutEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::status@rrr.reactor(rrr::WaitAll@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::status@rrr.reactor(rrr::WaitAny@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::test@rrr.reactor(janus::QuorumEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::test@rrr.reactor(rrr::IntEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::test@rrr.reactor(rrr::NeverEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::test@rrr.reactor(rrr::TimeoutEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::test@rrr.reactor(rrr::WaitAll@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::test@rrr.reactor(rrr::WaitAny@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::upgrade_fiber@rrr.reactor(janus::QuorumEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::upgrade_fiber@rrr.reactor(rrr::IntEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::upgrade_fiber@rrr.reactor(rrr::NeverEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::upgrade_fiber@rrr.reactor(rrr::TimeoutEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::upgrade_fiber@rrr.reactor(rrr::WaitAll@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::upgrade_fiber@rrr.reactor(rrr::WaitAny@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::wakeup_time@rrr.reactor(janus::QuorumEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::wakeup_time@rrr.reactor(rrr::IntEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::wakeup_time@rrr.reactor(rrr::NeverEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::wakeup_time@rrr.reactor(rrr::TimeoutEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::wakeup_time@rrr.reactor(rrr::WaitAll@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventPollable_::wakeup_time@rrr.reactor(rrr::WaitAny@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::EventState@rrr.reactor::new_()",
                ),
                (
                    "T",
                    "rrr::Fiber@rrr.reactor::continue_() const",
                ),
                (
                    "T",
                    "rrr::Fiber@rrr.reactor::create_run_impl(rusty::Function<void ()>, char const*, long)",
                ),
                (
                    "T",
                    "rrr::Fiber@rrr.reactor::current_fiber()",
                ),
                (
                    "T",
                    "rrr::Fiber@rrr.reactor::finished() const",
                ),
                (
                    "T",
                    "rrr::Fiber@rrr.reactor::new_(rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "rrr::Fiber@rrr.reactor::run() const",
                ),
                (
                    "T",
                    "rrr::Fiber@rrr.reactor::sleep(unsigned long)",
                ),
                (
                    "T",
                    "rrr::Fiber@rrr.reactor::yield_() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::IntEvent(rrr::IntEvent@rrr.reactor&&)",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::IntEvent(rusty::Cell<rrr::EventStatus@rrr.reactor>, rusty::thread::ThreadId, rrr::EventState@rrr.reactor, rusty::Cell<bool>, rusty::sync::Weak<rrr::EventPollable@rrr.reactor>, rusty::Cell<int>, rusty::Cell<int>)",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::get() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::get_fiber_id() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::get_self() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::is_ready() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::log() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::prunable() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::record_place(char const*, int) const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::set(int) const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::set_self(rusty::sync::Weak<rrr::EventPollable@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::set_status(rrr::EventStatus@rrr.reactor) const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::status() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::test() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::wait() const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::IntEvent@rrr.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::NeverEvent(rrr::NeverEvent@rrr.reactor&&)",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::NeverEvent(rusty::Cell<rrr::EventStatus@rrr.reactor>, rusty::thread::ThreadId, rrr::EventState@rrr.reactor, rusty::Cell<bool>, rusty::sync::Weak<rrr::EventPollable@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::get_self() const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::is_ready() const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::log() const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::prunable() const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::record_place(char const*, int) const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::set_self(rusty::sync::Weak<rrr::EventPollable@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::set_status(rrr::EventStatus@rrr.reactor) const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::status() const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::test() const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::NeverEvent@rrr.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::PollThread(rrr::PollThread@rrr.reactor&&)",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::PollThread(rusty::sync::mpsc::Sender<std::__1::variant<rrr::PollCommand_AddPollable@rrr.reactor, rrr::PollCommand_RemovePollable@rrr.reactor, rrr::PollCommand_ClosePollable@rrr.reactor, rrr::PollCommand_UpdateMode@rrr.reactor, rrr::PollCommand_AddJob@rrr.reactor, rrr::PollCommand_RemoveJob@rrr.reactor, rrr::PollCommand_Shutdown@rrr.reactor>>, rusty::Mutex<rusty::Option<rusty::thread::JoinHandle<std::__1::tuple<>>>>, rusty::sync::atomic::detail::Atomic<unsigned long>, rusty::sync::atomic::detail::Atomic<bool>)",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::add(rusty::Arc<rrr::Job@rrr.misc>) const",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::add_proxy(rusty::Box<rrr::PollableBase@rrr.pollable_proxy, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::create()",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::get_remove_count() const",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::operator=(rrr::PollThread@rrr.reactor&&)",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::remove(rrr::Pollable@rrr.epoll_wrapper&) const",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::remove_fd(int) const",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::request_close(int) const",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::shutdown() const",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::update_mode(int, int) const",
                ),
                (
                    "T",
                    "rrr::PollThread@rrr.reactor::~PollThread()",
                ),
                (
                    "T",
                    "rrr::PollThreadWorker@rrr.reactor::create(rusty::sync::mpsc::Receiver<std::__1::variant<rrr::PollCommand_AddPollable@rrr.reactor, rrr::PollCommand_RemovePollable@rrr.reactor, rrr::PollCommand_ClosePollable@rrr.reactor, rrr::PollCommand_UpdateMode@rrr.reactor, rrr::PollCommand_AddJob@rrr.reactor, rrr::PollCommand_RemoveJob@rrr.reactor, rrr::PollCommand_Shutdown@rrr.reactor>>)",
                ),
                (
                    "T",
                    "rrr::PollThreadWorker@rrr.reactor::poll_loop()",
                ),
                (
                    "T",
                    "rrr::PollThreadWorker@rrr.reactor::update_mode(rrr::Pollable@rrr.epoll_wrapper&, int)",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::Reactor(rusty::Cell<int>, rusty::RefCell<rusty::VecDeque<rusty::Arc<rrr::EventPollable@rrr.reactor>>>, rusty::RefCell<rusty::VecDeque<rusty::Arc<rrr::EventPollable@rrr.reactor>>>, rusty::RefCell<rusty::VecDeque<rusty::Arc<rrr::EventPollable@rrr.reactor>>>, rusty::RefCell<rusty::VecDeque<rusty::Arc<rrr::EventPollable@rrr.reactor>>>, rusty::RefCell<btree_port::btree::map::BTreeMap@btree_port.btree.map<unsigned long, rusty::port::rc::Rc@rc_port<rrr::Fiber@rrr.reactor, rusty::alloc::Global>, rusty::alloc::Global>>, rusty::RefCell<rusty::port::vec::Vec@vec_port.vec<rusty::port::rc::Rc@rc_port<rrr::Fiber@rrr.reactor, rusty::alloc::Global>, rusty::alloc::Global>>, rusty::Cell<bool>, rusty::Cell<bool>, rusty::Cell<int>, rusty::Cell<int>, rusty::Cell<rusty::thread::ThreadId>, rusty::Cell<long>, rusty::Cell<long>, rusty::Cell<long>, rusty::Cell<long>, rusty::Cell<long>, rusty::RefCell<rusty::port::vec::Vec@vec_port.vec<rrr::StacklessTaskEntry@rrr.reactor, rusty::alloc::Global>>, rusty::RefCell<rusty::port::vec::Vec@vec_port.vec<unsigned long, rusty::alloc::Global>>, rusty::RefCell<rusty::VecDeque<unsigned long>>, rusty::marker::PhantomPinned)",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::check_timeout(rusty::VecDeque<rusty::Arc<rrr::EventPollable@rrr.reactor>>&) const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::continue_fiber(rusty::port::rc::Rc@rc_port<rrr::Fiber@rrr.reactor, rusty::alloc::Global> const&) const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::create_run_fiber(rusty::Function<void ()>) const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::display_waiting_ev() const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::enqueue_stackless_task(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::get_disk_reactor()",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::get_reactor()",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::new_()",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::process_stackless_tasks() const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::prune_finished_events() const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::recycle(rusty::port::rc::Rc@rc_port<rrr::Fiber@rrr.reactor, rusty::alloc::Global>&) const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::register_fiber(rusty::port::rc::Rc@rc_port<rrr::Fiber@rrr.reactor, rusty::alloc::Global> const&) const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::register_stackless_poller(rusty::Function<bool (rusty::Context&)>) const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::restore_running_fiber(rusty::Option<rusty::port::rc::Rc@rc_port<rrr::Fiber@rrr.reactor, rusty::alloc::Global>>) const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::run_loop(bool, bool) const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::save_running_fiber() const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::set_running_fiber(rusty::port::rc::Rc@rc_port<rrr::Fiber@rrr.reactor, rusty::alloc::Global> const&) const",
                ),
                (
                    "T",
                    "rrr::Reactor@rrr.reactor::~Reactor()",
                ),
                (
                    "T",
                    "rrr::RemoveJob@rrr.reactor(rusty::Arc<rrr::Job@rrr.misc>)",
                ),
                (
                    "T",
                    "rrr::RemovePollable@rrr.reactor(int)",
                ),
                (
                    "T",
                    "rrr::SharedIntEvent@rrr.reactor::set(int const&)",
                ),
                (
                    "T",
                    "rrr::SharedIntEvent@rrr.reactor::wait(rusty::Function<bool (int) const>)",
                ),
                (
                    "T",
                    "rrr::SharedIntEvent@rrr.reactor::wait_until_gte(int, int)",
                ),
                (
                    "T",
                    "rrr::Shutdown@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::TimeoutEvent(rrr::TimeoutEvent@rrr.reactor&&)",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::TimeoutEvent(rusty::Cell<rrr::EventStatus@rrr.reactor>, rusty::thread::ThreadId, rrr::EventState@rrr.reactor, rusty::Cell<bool>, rusty::sync::Weak<rrr::EventPollable@rrr.reactor>, unsigned long, unsigned long)",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::get_self() const",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::is_ready() const",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::log() const",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::prunable() const",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::set_self(rusty::sync::Weak<rrr::EventPollable@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::set_status(rrr::EventStatus@rrr.reactor) const",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::status() const",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::test() const",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::wait() const",
                ),
                (
                    "T",
                    "rrr::TimeoutEvent@rrr.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "rrr::UpdateMode@rrr.reactor(int, int)",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::WaitAll(rrr::WaitAll@rrr.reactor&&)",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::WaitAll(rusty::Cell<rrr::EventStatus@rrr.reactor>, rusty::thread::ThreadId, rrr::EventState@rrr.reactor, rusty::Cell<bool>, rusty::sync::Weak<rrr::EventPollable@rrr.reactor>, rusty::RefCell<rusty::port::vec::Vec@vec_port.vec<rusty::Arc<rrr::EventPollable@rrr.reactor>, rusty::alloc::Global>>)",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::add_event(rusty::Arc<rrr::EventPollable@rrr.reactor>) const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::get_self() const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::is_ready() const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::log() const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::prunable() const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::set_self(rusty::sync::Weak<rrr::EventPollable@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::set_status(rrr::EventStatus@rrr.reactor) const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::status() const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::test() const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::wait() const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::WaitAll@rrr.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::WaitAny(rrr::WaitAny@rrr.reactor&&)",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::WaitAny(rusty::Cell<rrr::EventStatus@rrr.reactor>, rusty::thread::ThreadId, rrr::EventState@rrr.reactor, rusty::Cell<bool>, rusty::sync::Weak<rrr::EventPollable@rrr.reactor>, rusty::port::vec::Vec@vec_port.vec<rusty::Arc<rrr::EventPollable@rrr.reactor>, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::get_self() const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::is_composite_event() const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::is_ready() const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::log() const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::prunable() const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::set_prunable(bool) const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::set_self(rusty::sync::Weak<rrr::EventPollable@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::set_status(rrr::EventStatus@rrr.reactor) const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::status() const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::test() const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::upgrade_fiber() const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::wait() const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::wait_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::WaitAny@rrr.reactor::wakeup_time() const",
                ),
                (
                    "T",
                    "rrr::create_sp_int_event@rrr.reactor(int)",
                ),
                (
                    "T",
                    "rrr::create_sp_never_event@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::create_sp_timeout_event@rrr.reactor(unsigned long)",
                ),
                (
                    "T",
                    "rrr::create_sp_waitall@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::create_sp_waitall_from@rrr.reactor(rusty::port::vec::Vec@vec_port.vec<rusty::Arc<rrr::EventPollable@rrr.reactor>, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "rrr::create_sp_waitany@rrr.reactor(rusty::Arc<rrr::EventPollable@rrr.reactor>, rusty::Arc<rrr::EventPollable@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::current_thread_gettid@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::event_core_get_fiber_id@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::event_state_seed@rrr.reactor(rrr::EventState@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::fiber_create_run_impl@rrr.reactor(rusty::Function<void ()>, char const*, long)",
                ),
                (
                    "T",
                    "rrr::fiber_current_fiber@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::fiber_do_continue@rrr.reactor(rrr::Fiber@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::fiber_do_finalize@rrr.reactor(rrr::Fiber@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::fiber_do_yield@rrr.reactor(rrr::Fiber@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::fiber_engine_destroy@rrr.reactor(srpc_fiber*)",
                ),
                (
                    "T",
                    "rrr::fiber_engine_resume@rrr.reactor(srpc_fiber*)",
                ),
                (
                    "T",
                    "rrr::fiber_engine_start@rrr.reactor(srpc_fiber*, void*)",
                ),
                (
                    "T",
                    "rrr::fiber_engine_yield@rrr.reactor(srpc_fiber*)",
                ),
                (
                    "T",
                    "rrr::fiber_fn_clear@rrr.reactor(rusty::RefCell<rusty::Function<void ()>> const*)",
                ),
                (
                    "T",
                    "rrr::fiber_fn_invoke@rrr.reactor(rusty::RefCell<rusty::Function<void ()>> const*)",
                ),
                (
                    "T",
                    "rrr::fiber_fn_present@rrr.reactor(rusty::RefCell<rusty::Function<void ()>> const*)",
                ),
                (
                    "T",
                    "rrr::fiber_install_task@rrr.reactor(rusty::RefCell<rusty::Option<rusty::Box<rrr::fiber_task_t@rrr.reactor, rusty::alloc::Global>>> const*, rusty::Function<void (rrr::fiber_yield_t@rrr.reactor&)>)",
                ),
                (
                    "T",
                    "rrr::fiber_is_finished@rrr.reactor(rrr::Fiber@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::fiber_next_global_id@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::fiber_registry_key@rrr.reactor(rusty::port::rc::Rc@rc_port<rrr::Fiber@rrr.reactor, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "rrr::fiber_run@rrr.reactor(rrr::Fiber@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::fiber_run_wrapper@rrr.reactor(rrr::Fiber@rrr.reactor const&, rrr::fiber_yield_t@rrr.reactor*)",
                ),
                (
                    "T",
                    "rrr::fiber_sleep@rrr.reactor(unsigned long)",
                ),
                (
                    "T",
                    "rrr::fiber_task_body_invoke@rrr.reactor(rusty::Function<void (rrr::fiber_yield_t@rrr.reactor&)>&, rrr::fiber_yield_t@rrr.reactor&)",
                ),
                (
                    "T",
                    "rrr::fiber_task_invoke@rrr.reactor(rusty::RefCell<rusty::Option<rusty::Box<rrr::fiber_task_t@rrr.reactor, rusty::alloc::Global>>> const*)",
                ),
                (
                    "T",
                    "rrr::fiber_task_t@rrr.reactor::new_(rusty::Function<void (rrr::fiber_yield_t@rrr.reactor&)>)",
                ),
                (
                    "T",
                    "rrr::fiber_task_t@rrr.reactor::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "rrr::fiber_task_t@rrr.reactor::~fiber_task_t()",
                ),
                (
                    "T",
                    "rrr::fiber_yield_invoke@rrr.reactor(rrr::fiber_yield_t@rrr.reactor&)",
                ),
                (
                    "T",
                    "rrr::fiber_yield_invoke_ptr@rrr.reactor(rrr::fiber_yield_t@rrr.reactor*)",
                ),
                (
                    "T",
                    "rrr::fiber_yield_t@rrr.reactor::new_(rrr::fiber_task_t@rrr.reactor&)",
                ),
                (
                    "T",
                    "rrr::int_event_is_ready@rrr.reactor(rrr::IntEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::int_event_make@rrr.reactor(int)",
                ),
                (
                    "T",
                    "rrr::int_event_raw_ptr@rrr.reactor(rusty::Arc<rrr::IntEvent@rrr.reactor> const&)",
                ),
                (
                    "T",
                    "rrr::int_event_set@rrr.reactor(rrr::IntEvent@rrr.reactor const&, int)",
                ),
                (
                    "T",
                    "rrr::job_ready@rrr.reactor(rusty::Arc<rrr::Job@rrr.misc> const&)",
                ),
                (
                    "T",
                    "rrr::job_spawn_work@rrr.reactor(rusty::Arc<rrr::Job@rrr.misc> const&)",
                ),
                (
                    "T",
                    "rrr::never_event_make@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::pollable_proxy_fd@rrr.reactor(rusty::Box<rrr::PollableBase@rrr.pollable_proxy, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "rrr::pollable_proxy_mode@rrr.reactor(rusty::Box<rrr::PollableBase@rrr.pollable_proxy, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "rrr::pollthread_create@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::pollthread_drop@rrr.reactor(rrr::PollThread@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::pollworker_close_proxy_of@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&, int)",
                ),
                (
                    "T",
                    "rrr::pollworker_create@rrr.reactor(rusty::sync::mpsc::Receiver<std::__1::variant<rrr::PollCommand_AddPollable@rrr.reactor, rrr::PollCommand_RemovePollable@rrr.reactor, rrr::PollCommand_ClosePollable@rrr.reactor, rrr::PollCommand_UpdateMode@rrr.reactor, rrr::PollCommand_AddJob@rrr.reactor, rrr::PollCommand_RemoveJob@rrr.reactor, rrr::PollCommand_Shutdown@rrr.reactor>>)",
                ),
                (
                    "T",
                    "rrr::pollworker_do_add_job@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&, rusty::Arc<rrr::Job@rrr.misc>)",
                ),
                (
                    "T",
                    "rrr::pollworker_do_add_pollable@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&, rusty::Box<rrr::PollableBase@rrr.pollable_proxy, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "rrr::pollworker_do_close_pollable@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&, int)",
                ),
                (
                    "T",
                    "rrr::pollworker_do_remove_job@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&, rusty::Arc<rrr::Job@rrr.misc>)",
                ),
                (
                    "T",
                    "rrr::pollworker_do_remove_pollable@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&, int)",
                ),
                (
                    "T",
                    "rrr::pollworker_do_update_mode@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&, int, int)",
                ),
                (
                    "T",
                    "rrr::pollworker_is_on_poll_thread@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::pollworker_make@rrr.reactor(rusty::sync::mpsc::Receiver<std::__1::variant<rrr::PollCommand_AddPollable@rrr.reactor, rrr::PollCommand_RemovePollable@rrr.reactor, rrr::PollCommand_ClosePollable@rrr.reactor, rrr::PollCommand_UpdateMode@rrr.reactor, rrr::PollCommand_AddJob@rrr.reactor, rrr::PollCommand_RemoveJob@rrr.reactor, rrr::PollCommand_Shutdown@rrr.reactor>>)",
                ),
                (
                    "T",
                    "rrr::pollworker_poll_loop@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&)",
                ),
                (
                    "T",
                    "rrr::pollworker_process_commands@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&)",
                ),
                (
                    "T",
                    "rrr::pollworker_process_pending_removals@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&)",
                ),
                (
                    "T",
                    "rrr::pollworker_snapshot_fds@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&)",
                ),
                (
                    "T",
                    "rrr::pollworker_take_removals@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&)",
                ),
                (
                    "T",
                    "rrr::pollworker_trigger_job@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&)",
                ),
                (
                    "T",
                    "rrr::pollworker_update_mode@rrr.reactor(rrr::PollThreadWorker@rrr.reactor&, rrr::Pollable@rrr.epoll_wrapper&, int)",
                ),
                (
                    "T",
                    "rrr::reactor_create_run_fiber_at_impl@rrr.reactor(rrr::Reactor@rrr.reactor const&, rusty::Function<void ()>, char const*, long)",
                ),
                (
                    "T",
                    "rrr::reactor_create_run_fiber_impl@rrr.reactor(rrr::Reactor@rrr.reactor const&, rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "rrr::reactor_dec_active_fibers@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::reactor_get_or_create_fiber_impl@rrr.reactor(rrr::Reactor@rrr.reactor const&, rusty::Function<void ()>, char const*, long)",
                ),
                (
                    "T",
                    "rrr::reactor_live_fiber_count@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::reactor_log_create@rrr.reactor(bool)",
                ),
                (
                    "T",
                    "rrr::reactor_log_line@rrr.reactor(int, int, signed char const*, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "rrr::reactor_make@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::reactor_poll_one@rrr.reactor(rrr::Reactor@rrr.reactor const&, unsigned long, rusty::Function<bool (rusty::Context&)>*)",
                ),
                (
                    "T",
                    "rrr::reactor_spawn_stackless_task_impl@rrr.reactor(rrr::Reactor@rrr.reactor const&, rusty::Task<void>)",
                ),
                (
                    "T",
                    "rrr::reactor_tls_get@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::reactor_tls_get_disk@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::reactor_tls_restore_running@rrr.reactor(rusty::Option<rusty::port::rc::Rc@rc_port<rrr::Fiber@rrr.reactor, rusty::alloc::Global>>)",
                ),
                (
                    "T",
                    "rrr::reactor_tls_save_running@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::reactor_tls_set_running@rrr.reactor(rusty::port::rc::Rc@rc_port<rrr::Fiber@rrr.reactor, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "rrr::reactor_verify@rrr.reactor(bool)",
                ),
                (
                    "T",
                    "rrr::reusing_fiber@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::shared_int_event_set@rrr.reactor(rrr::SharedIntEvent@rrr.reactor&, int)",
                ),
                (
                    "T",
                    "rrr::shared_int_event_wait@rrr.reactor(rrr::SharedIntEvent@rrr.reactor&, rusty::Function<bool (int) const>)",
                ),
                (
                    "T",
                    "rrr::shared_int_event_wait_until_gte@rrr.reactor(rrr::SharedIntEvent@rrr.reactor&, int, int)",
                ),
                (
                    "T",
                    "rrr::stackless_profile_enabled@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::stackless_profile_env@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::stackless_profile_note_enqueue@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::stackless_profile_note_poll_ready@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::stackless_profile_note_register@rrr.reactor(unsigned long, bool, unsigned long)",
                ),
                (
                    "T",
                    "rrr::stackless_profile_report_periodic@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::stackless_profile_report_periodic_shim@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::stackless_profile_update_max_slots@rrr.reactor(unsigned long)",
                ),
                (
                    "T",
                    "rrr::thread_id_to_u64@rrr.reactor(rusty::thread::ThreadId)",
                ),
                (
                    "T",
                    "rrr::timeout_event_is_ready@rrr.reactor(rrr::TimeoutEvent@rrr.reactor const&)",
                ),
                (
                    "T",
                    "rrr::timeout_event_make@rrr.reactor(unsigned long)",
                ),
                (
                    "T",
                    "rrr::u64_to_thread_id@rrr.reactor(unsigned long)",
                ),
                (
                    "T",
                    "rrr::waitall_make@rrr.reactor()",
                ),
                (
                    "T",
                    "rrr::waitall_make_from@rrr.reactor(rusty::port::vec::Vec@vec_port.vec<rusty::Arc<rrr::EventPollable@rrr.reactor>, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "rrr::waitany_make@rrr.reactor(rusty::Arc<rrr::EventPollable@rrr.reactor>, rusty::Arc<rrr::EventPollable@rrr.reactor>)",
                ),
            }
        ),
    ),
    "rrr.server": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.server;",
                "namespace rrr {",
                "import vec_port.vec;",
                "import std_port;",
                "import rrr.basetypes;",
                "import rrr.channel;",
                "import rrr.debugging;",
                "import rrr.internal_protocol;",
                "import rrr.logging;",
                "import rrr.misc;",
                "import rrr.reactor;",
                "import rrr.serializable;",
                "import rrr.tcp_channel;",
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
                "export using ServerReplyFn = rusty::Function<void(::rrr::BinaryWriteArchive&)>;",
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
                "export void sconn_on_channel_frame(const rusty::sync::Weak<ServerConnection>& weak, const ::rrr::ChannelFrame& frame);",
                "export void sconn_on_channel_closed(const rusty::sync::Weak<ServerConnection>& weak);",
                "export void sconn_on_channel_error(const rusty::sync::Weak<ServerConnection>& weak, ::rrr::ChannelError err, std::string_view msg);",
                "export void request_fill_body(Request& req, std::span<const uint8_t> bytes);",
                "export void sconn_decode_request_and_dispatch(const ServerConnection& sconn, const uint8_t* bytes, size_t size);",
                "export rrr::ChannelConnectionBase* sconn_proxy_ptr(const rusty::Option<::rrr::ChannelConnectionProxy>& slot);",
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
                    "typeinfo for rrr::Service@rrr.server",
                ),
                (
                    "D",
                    "vtable for rrr::Service@rrr.server",
                ),
                (
                    "R",
                    "rrr::SERVER_ERR_ALREADY_EXISTS@rrr.server",
                ),
                (
                    "R",
                    "rrr::SERVER_ERR_INVALID_ARGUMENT@rrr.server",
                ),
                (
                    "R",
                    "rrr::SERVER_ERR_NO_ENTRY@rrr.server",
                ),
                (
                    "R",
                    "rrr::kDefaultDrainTimeoutMs@rrr.server",
                ),
                (
                    "R",
                    "typeinfo name for rrr::Service@rrr.server",
                ),
                (
                    "T",
                    "rrr::DeferredReply@rrr.server::DeferredReply(rrr::DeferredReply@rrr.server&&)",
                ),
                (
                    "T",
                    "rrr::DeferredReply@rrr.server::DeferredReply(rusty::Box<rrr::Request@rrr.server, rusty::alloc::Global>, rusty::sync::Weak<rrr::ServerConnection@rrr.server>, rusty::Function<void (rrr::BinaryWriteArchive@rrr.serializable&)>, rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "rrr::DeferredReply@rrr.server::new_(rusty::Box<rrr::Request@rrr.server, rusty::alloc::Global>, rusty::sync::Weak<rrr::ServerConnection@rrr.server>, rusty::Function<void (rrr::BinaryWriteArchive@rrr.serializable&)>, rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "rrr::DeferredReply@rrr.server::operator=(rrr::DeferredReply@rrr.server&&)",
                ),
                (
                    "T",
                    "rrr::DeferredReply@rrr.server::reply()",
                ),
                (
                    "T",
                    "rrr::DeferredReply@rrr.server::reply_error(int)",
                ),
                (
                    "T",
                    "rrr::DeferredReply@rrr.server::run_async(rusty::Function<void ()>)",
                ),
                (
                    "T",
                    "rrr::DeferredReply@rrr.server::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "rrr::DeferredReply@rrr.server::~DeferredReply()",
                ),
                (
                    "T",
                    "rrr::PendingRequestGuard@rrr.server::PendingRequestGuard(rrr::PendingRequestGuard@rrr.server&&)",
                ),
                (
                    "T",
                    "rrr::PendingRequestGuard@rrr.server::PendingRequestGuard(rusty::Arc<rusty::sync::atomic::detail::Atomic<int>>)",
                ),
                (
                    "T",
                    "rrr::PendingRequestGuard@rrr.server::operator=(rrr::PendingRequestGuard@rrr.server&&)",
                ),
                (
                    "T",
                    "rrr::PendingRequestGuard@rrr.server::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "rrr::PendingRequestGuard@rrr.server::~PendingRequestGuard()",
                ),
                (
                    "T",
                    "rrr::Request@rrr.server::attach_pending_guard(rusty::Arc<rusty::sync::atomic::detail::Atomic<int>> const&)",
                ),
                (
                    "T",
                    "rrr::RpcServiceContext@rrr.server::new_(std_port::collections::hash::map::HashMap@std_port<int, unsigned long, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>, std_port::collections::hash::set::HashSet@std_port<int, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>, rusty::port::vec::Vec@vec_port.vec<rusty::RefCell<rusty::Box<rrr::Service@rrr.server, rusty::alloc::Global>>, rusty::alloc::Global>, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, rusty::Arc<rusty::sync::atomic::detail::Atomic<int>>, rusty::Arc<rusty::sync::atomic::detail::Atomic<bool>>, unsigned long)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::Server(rrr::Server@rrr.server&&)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::Server(rusty::port::vec::Vec@vec_port.vec<rusty::Box<rrr::Service@rrr.server, rusty::alloc::Global>, rusty::alloc::Global>, std_port::collections::hash::map::HashMap@std_port<int, unsigned long, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>, std_port::collections::hash::set::HashSet@std_port<int, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>, rusty::Option<rusty::Arc<rrr::RpcServiceContext@rrr.server>>, rusty::Option<rusty::Arc<rrr::PollThread@rrr.reactor>>, rusty::Mutex<rrr::ShutdownState@rrr.server>, rusty::Box<rusty::Condvar, rusty::alloc::Global>, rusty::Cell<rrr::ShutdownPhase@rrr.server>, rusty::Mutex<rusty::port::vec::Vec@vec_port.vec<rusty::Function<void ()>, rusty::alloc::Global>>, rusty::Arc<rusty::sync::atomic::detail::Atomic<int>>, rusty::Arc<rusty::sync::atomic::detail::Atomic<bool>>, unsigned long, rusty::Option<rusty::Box<rrr::ChannelFactoryBase@rrr.channel, rusty::alloc::Global>>, rusty::Option<rusty::Box<rrr::ChannelListenerBase@rrr.channel, rusty::alloc::Global>>, rusty::Arc<rusty::Mutex<rrr::ChannelSconns@rrr.server>>)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::add_shutdown_hook(rusty::Function<void ()>) const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::addr() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::decrement_pending() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::do_shutdown() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::drain(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::drop_heartbeat_replies() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::get_bound_port() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::graceful_shutdown(unsigned long)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::increment_pending() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::instance_id() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::is_channel_factory_bound() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::new_(rusty::Option<rusty::Arc<rrr::PollThread@rrr.reactor>>)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::operator=(rrr::Server@rrr.server&&)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::pending_request_count() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::phase() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::reg_fast_rpc(int, unsigned long)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::reg_rpc(int, unsigned long)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::reg_service(rusty::Box<rrr::Service@rrr.server, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::reg_service_proxy(rusty::Box<rrr::Service@rrr.server, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::service_count() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::set_channel_factory(rusty::Box<rrr::ChannelFactoryBase@rrr.channel, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::set_drop_heartbeat_replies(bool) const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::start(signed char const*)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::stop_accepting()",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::unreg(int)",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::wait_for_shutdown() const",
                ),
                (
                    "T",
                    "rrr::Server@rrr.server::~Server()",
                ),
                (
                    "T",
                    "rrr::ServerConnection@rrr.server::bind_channel(rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "rrr::ServerConnection@rrr.server::close() const",
                ),
                (
                    "T",
                    "rrr::ServerConnection@rrr.server::connected() const",
                ),
                (
                    "T",
                    "rrr::ServerConnection@rrr.server::install_self_weak_for_testing(rusty::sync::Weak<rrr::ServerConnection@rrr.server>)",
                ),
                (
                    "T",
                    "rrr::ServerConnection@rrr.server::is_channel_mode() const",
                ),
                (
                    "T",
                    "rrr::ServerConnection@rrr.server::is_closed() const",
                ),
                (
                    "T",
                    "rrr::ServerConnection@rrr.server::new_(rusty::Arc<rrr::RpcServiceContext@rrr.server>, int)",
                ),
                (
                    "T",
                    "rrr::ServerConnection@rrr.server::reply(rrr::Request@rrr.server const&, int, rusty::Function<void (rrr::BinaryWriteArchive@rrr.serializable&)>) const",
                ),
                (
                    "T",
                    "rrr::ServerConnection@rrr.server::run_async(rusty::Function<void ()>) const",
                ),
                (
                    "T",
                    "rrr::Service@rrr.server::~Service()",
                ),
                (
                    "T",
                    "rrr::make_empty_request_box@rrr.server()",
                ),
                (
                    "T",
                    "rrr::make_service_proxy_from_box@rrr.server(rusty::Box<rrr::Service@rrr.server, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "rrr::no_reply_writer@rrr.server()",
                ),
                (
                    "T",
                    "rrr::request_fill_body@rrr.server(rrr::Request@rrr.server&, std::__1::span<unsigned char const, 18446744073709551615ul>)",
                ),
                (
                    "T",
                    "rrr::sconn_decode_request_and_dispatch@rrr.server(rrr::ServerConnection@rrr.server const&, unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::sconn_dispatch_in_fiber@rrr.server(rusty::Arc<rrr::RpcServiceContext@rrr.server>, unsigned long, int, rusty::Box<rrr::Request@rrr.server, rusty::alloc::Global>, rusty::sync::Weak<rrr::ServerConnection@rrr.server>)",
                ),
                (
                    "T",
                    "rrr::sconn_dispatch_response_frame_via_channel@rrr.server(rrr::ServerConnection@rrr.server const&, unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::sconn_on_channel_closed@rrr.server(rusty::sync::Weak<rrr::ServerConnection@rrr.server> const&)",
                ),
                (
                    "T",
                    "rrr::sconn_on_channel_error@rrr.server(rusty::sync::Weak<rrr::ServerConnection@rrr.server> const&, rrr::ChannelError@rrr.channel, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::sconn_on_channel_frame@rrr.server(rusty::sync::Weak<rrr::ServerConnection@rrr.server> const&, rrr::ChannelFrame@rrr.channel const&)",
                ),
                (
                    "T",
                    "rrr::sconn_proxy_ptr@rrr.server(rusty::Option<rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>> const&)",
                ),
                (
                    "T",
                    "rrr::sconn_reply@rrr.server(rrr::ServerConnection@rrr.server const&, rrr::Request@rrr.server const&, int, rusty::Function<void (rrr::BinaryWriteArchive@rrr.serializable&)>)",
                ),
                (
                    "T",
                    "rrr::server_drain_impl@rrr.server(rusty::Cell<rrr::ShutdownPhase@rrr.server> const&, rusty::Arc<rusty::sync::atomic::detail::Atomic<int>> const&, unsigned long)",
                ),
                (
                    "T",
                    "rrr::server_dsl_addr_to_string@rrr.server(signed char const*)",
                ),
                (
                    "T",
                    "rrr::server_generate_instance_id@rrr.server()",
                ),
                (
                    "T",
                    "rrr::server_invoke_shutdown_hook_safely@rrr.server(rusty::Function<void ()>&)",
                ),
                (
                    "T",
                    "rrr::server_now_nanos@rrr.server()",
                ),
                (
                    "T",
                    "rrr::server_parse_port@rrr.server(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::server_random_u64@rrr.server()",
                ),
                (
                    "T",
                    "rrr::server_resolve_poll_thread@rrr.server(rusty::Option<rusty::Arc<rrr::PollThread@rrr.reactor>>)",
                ),
                (
                    "T",
                    "rrr::server_run_shutdown_hooks@rrr.server(rusty::Mutex<rusty::port::vec::Vec@vec_port.vec<rusty::Function<void ()>, rusty::alloc::Global>> const&)",
                ),
                (
                    "T",
                    "rrr::server_wait_for_shutdown_impl@rrr.server(rusty::Mutex<rrr::ShutdownState@rrr.server> const&, rusty::Box<rusty::Condvar, rusty::alloc::Global> const&)",
                ),
                (
                    "T",
                    "rrr::shutdown_phase_to_string@rrr.server(rrr::ShutdownPhase@rrr.server)",
                ),
            }
        ),
    ),
    "rrr.client": AbiSpec(
        surface=frozenset(
            {
                "export module rrr.client;",
                "namespace rrr {",
                "import vec_port.vec;",
                "import btree_port.btree.map;",
                "import std_port;",
                "import rrr.basetypes;",
                "import rrr.callback_wrapper;",
                "import rrr.callbacks;",
                "import rrr.channel;",
                "import rrr.circuit_breaker;",
                "import rrr.connection_metrics;",
                "import rrr.connection_state;",
                "import rrr.errors;",
                "import rrr.fiber_channel;",
                "import rrr.heartbeat;",
                "import rrr.load_balancer;",
                "import rrr.logging;",
                "import rrr.misc;",
                "import rrr.rand;",
                "import rrr.reactor;",
                "import rrr.reconnect_policy;",
                "import rrr.request_options;",
                "import rrr.request_queue;",
                "import rrr.serializable;",
                "import rrr.tcp_channel;",
                "import rrr.debugging;",
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
                "export using OnErrorCallbackFn = rusty::Function<void(::rrr::RpcError, const std::string&) const>;",
                "export using OnReconnectedCallbackFn = rusty::Function<void(bool) const>;",
                "export using LegacyStdString = std::string;",
                "export using LegacyStdStringView = std::string_view;",
                "export using c_char = int8_t;",
                "export using FutureCallback = ::rrr::detail::CallbackWrapper<rusty::Function<void(rusty::Arc<Future>) const>>;",
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
                "export ::rrr::SinkProxy client_sink_proxy(::rrr::BufferSink& sink);",
                "export ::rrr::SourceProxy client_source_proxy(::rrr::BufferSource& source);",
                "export ReplyBuffer reply_buffer_empty();",
                "export void reply_buffer_fill(ReplyBuffer& rb, std::span<const uint8_t> bytes);",
                "export rusty::Vec<rusty::Option<AsyncReplyCallback>> make_prefilled_cb_slots();",
                "export ::rrr::RequestQueue make_pending_queue(const ::rrr::RequestQueueConfig& c);",
                "export uint64_t clientconn_monotonic_ms_now();",
                "export int32_t clientconn_reconnect(const ClientConnection& self_, OnReconnectCompleteCallbackFn on_complete);",
                "export ::rrr::BinaryWriteArchive make_write_archive(::rrr::BufferSink* sink);",
                "export void request_copy_reply(const rusty::Arc<Future>& final_fu, const rusty::Arc<Future>& attempt_fu);",
                "export ::rrr::TimeoutType classify_request_failure(int32_t err);",
                "export ::rrr::ChannelError clientconn_dispatch_frame_via_channel(const ClientConnection& conn, const uint8_t* body_bytes, size_t body_size);",
                "export void clientconn_enqueue_heartbeat_probe(const ClientConnection& conn);",
                "export std::string clientconn_addr_to_string(const int8_t* addr);",
                "export int32_t clientconn_connect_via_factory(const ClientConnection& conn, const int8_t* addr_i8);",
                "export rusty::Box<::rrr::FiberChannel> clientconn_make_fiber_channel(::rrr::ChannelConnectionProxy ch);",
                "export void clientconn_recv_job_entry(WeakClientConnection weak_self);",
                "export void clientconn_bind_channel_via_poll_thread(const ClientConnection& conn, ::rrr::ChannelConnectionProxy channel);",
                "export ::rrr::FiberChannel* clientconn_fiber_channel_ptr(const rusty::Option<rusty::Box<::rrr::FiberChannel>>& slot);",
                "export void clientconn_run_recv_loop(const ClientConnection& conn);",
                "export void clientconn_decode_response_and_notify(const ClientConnection& conn, const uint8_t* bytes, size_t size);",
                "export ::rrr::RpcError clientconn_map_system_error(int32_t err);",
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
                    "rrr::CLIENT_ERR_AGAIN@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_BROKEN_PIPE@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_BUSY@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_CANCELED@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_CONNECTION_ABORTED@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_CONNECTION_REFUSED@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_CONNECTION_RESET@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_HOST_UNREACHABLE@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_INVALID_ARGUMENT@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_IO@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_NETWORK_UNREACHABLE@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_NOT_CONNECTED@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_TIMED_OUT@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_ERR_WOULD_BLOCK@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_INTERNAL_HEARTBEAT_RPC_ID@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_INT_MIN@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_POLL_NO_CHANGE@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_POLL_READ@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_RAND_MAX@rrr.client",
                ),
                (
                    "R",
                    "rrr::CLIENT_REQUEST_QUEUE_REJECTED_ERROR@rrr.client",
                ),
                (
                    "R",
                    "rrr::kAsyncSlotCount@rrr.client",
                ),
                (
                    "T",
                    "rrr::BufferingConfig@rrr.client::clone() const",
                ),
                (
                    "T",
                    "rrr::BufferingConfig@rrr.client::defaults()",
                ),
                (
                    "T",
                    "rrr::BufferingConfig@rrr.client::disabled()",
                ),
                (
                    "T",
                    "rrr::BufferingConfig@rrr.client::new_()",
                ),
                (
                    "T",
                    "rrr::BufferingConfig@rrr.client::to_queue_config() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::Client(rrr::Client@rrr.client&&)",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::Client(rusty::RefCell<rusty::Option<rusty::Arc<rrr::ClientConnection@rrr.client>>>, rusty::Arc<rrr::PollThread@rrr.reactor>, rusty::Cell<bool>, rusty::Cell<long>, rusty::Cell<unsigned long>, rusty::Cell<int>, rusty::Cell<rrr::KeepaliveConfig@rrr.client>, rusty::Cell<rrr::HeartbeatConfig@rrr.heartbeat>, rusty::Cell<rrr::CircuitBreakerConfig@rrr.circuit_breaker>, rusty::Cell<rrr::ReconnectPolicy@rrr.reconnect_policy>, rusty::Arc<rrr::CallbackManager@rrr.callbacks>, rusty::Mutex<rusty::Option<rusty::Box<rrr::ChannelFactoryBase@rrr.channel, rusty::alloc::Global>>>, rrr::ConnectionMetrics@rrr.connection_metrics)",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::add_on_connected(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::add_on_disconnected(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::add_on_error(rusty::Function<void (rrr::RpcError@rrr.errors, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const>) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::add_on_reconnected(rusty::Function<void (bool) const>) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::add_on_reconnecting(rusty::Function<void () const>) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::check_server_instance(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::circuit_breaker_config() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::circuit_breaker_state() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::clear_connection_callbacks() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::clear_pending_requests(int) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::client_mode() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::close() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::connect(signed char const*, bool) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::connected() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::connection() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::connection_state() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::create(rusty::Arc<rrr::PollThread@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::handle_free(long) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::has_connection() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::has_pending_channel_factory() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::heartbeat_config() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::host() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::is_idle(unsigned long, unsigned long) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::is_reconnecting() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::keepalive_config() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::metrics() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::new_(rusty::Arc<rrr::PollThread@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::operator=(rrr::Client@rrr.client&&)",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::pause() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::pending_request_count() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::reconnect(rusty::Function<void (bool)>) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::resume() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::rpc_id() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::server_instance_id() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_buffering_config(rrr::BufferingConfig@rrr.client const&) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_channel_factory(rusty::Box<rrr::ChannelFactoryBase@rrr.channel, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_circuit_breaker(rrr::CircuitBreakerConfig@rrr.circuit_breaker const&) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_client_mode(bool) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_heartbeat(rrr::HeartbeatConfig@rrr.heartbeat const&) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_keepalive(rrr::KeepaliveConfig@rrr.client const&) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_on_server_restart(rusty::Function<void (unsigned long, unsigned long)>) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_reconnect_policy(rrr::ReconnectPolicy@rrr.reconnect_policy const&) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_rpc_id(int) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_time(long) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_timeout(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::set_valid(bool) const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::time() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::timeout() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::try_reconnect_if_needed() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::validate_connection() const",
                ),
                (
                    "T",
                    "rrr::Client@rrr.client::~Client()",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::ClientConnection(rrr::ClientConnection@rrr.client&&)",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::ClientConnection(rusty::Arc<rrr::PollThread@rrr.reactor>, rusty::Mutex<rusty::Option<rusty::Box<rrr::FiberChannel@rrr.fiber_channel, rusty::alloc::Global>>>, rusty::Mutex<rusty::Option<rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>>>, rusty::Cell<bool>, rusty::Mutex<rusty::Option<rusty::Box<rrr::ChannelFactoryBase@rrr.channel, rusty::alloc::Global>>>, rrr::Counter@rrr.basetypes, rusty::Mutex<std_port::collections::hash::map::HashMap@std_port<long, rusty::Arc<rrr::Future@rrr.client>, std_port::hash::compat::DefaultHasher@std_port, rusty::alloc::Global>>, rusty::Mutex<rusty::port::vec::Vec@vec_port.vec<rusty::Option<rusty::Function<void (int, unsigned char const*, unsigned long)>>, rusty::alloc::Global>>, rrr::ConnectionStateMachine@rrr.connection_state, rusty::Cell<rrr::ReconnectPolicy@rrr.reconnect_policy>, rrr::ReconnectState@rrr.client, rusty::Cell<std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>>, rusty::Cell<rrr::BufferingConfig@rrr.client>, rrr::RequestQueue@rrr.request_queue, rusty::Cell<unsigned long>, rusty::RefCell<rusty::Function<void (unsigned long, unsigned long)>>, rusty::Cell<rrr::KeepaliveConfig@rrr.client>, rrr::HeartbeatManager@rrr.heartbeat, rrr::CircuitBreaker@rrr.circuit_breaker, rusty::Arc<rrr::CallbackManager@rrr.callbacks>, rusty::Cell<unsigned long>, rrr::ConnectionMetrics@rrr.connection_metrics, rusty::sync::Weak<rrr::ClientConnection@rrr.client>, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>, unsigned long, rusty::Cell<bool>, bool)",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::abort_reconnect()",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::allow_request_with_circuit_metrics() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::apply_keepalive_options()",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::bind_channel(rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::bind_channel_direct(rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::bind_channel_via_poll_thread(rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::bind_factory(rusty::Box<rrr::ChannelFactoryBase@rrr.channel, rusty::alloc::Global>) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::buffering_config() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::channel_reconnect_attempts_count() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::check_pending_write_update() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::check_server_instance(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::circuit_breaker_config() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::circuit_breaker_state() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::clear_pending_requests(int) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::close() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::connect(signed char const*) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::connect_via_factory(signed char const*) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::connected() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::connection_state() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::content_size() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::decode_response_and_notify(unsigned char const*, unsigned long) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::dispatch_frame_via_channel(unsigned char const*, unsigned long) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::enqueue_heartbeat_probe() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::fail_pending_future(long, int) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::fd() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::force_connected_for_testing()",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::handle_error() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::handle_free(long) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::handle_read() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::handle_write() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::heartbeat_config() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::host() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::install_self_weak_for_testing(rusty::sync::Weak<rrr::ClientConnection@rrr.client>)",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::invalidate_pending_futures() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::invoke_connected_callback() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::invoke_disconnected_callback() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::invoke_error_callback(int, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::invoke_reconnected_callback(bool) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::invoke_reconnecting_callback() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::is_channel_mode() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::is_closed() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::is_factory_bound() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::is_idle(unsigned long, unsigned long) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::is_reconnecting() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::keepalive_config() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::last_activity_time() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::map_system_error(int)",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::mark_closing() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::metrics() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::new_(rusty::Arc<rrr::PollThread@rrr.reactor>)",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::on_channel_closed_fan_out() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::on_request_dispatched(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::on_response_received(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::operator=(rrr::ClientConnection@rrr.client&&)",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::pause() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::pending_future_count() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::pending_request_count() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::poll_mode() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::reconnect(rusty::Function<void (bool)>) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::reconnect_policy() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::record_circuit_result(int) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::record_circuit_state_transition(rrr::CircuitState@rrr.circuit_breaker, rrr::CircuitState@rrr.circuit_breaker) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::replay_pending_requests() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::replay_pending_requests_for_test() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::reset_channel_mode_for_reconnect() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::resume() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::run_recv_loop() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::server_instance_id() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::set_buffering_config(rrr::BufferingConfig@rrr.client const&) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::set_callback_manager(rusty::Arc<rrr::CallbackManager@rrr.callbacks> const&)",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::set_circuit_breaker_config(rrr::CircuitBreakerConfig@rrr.circuit_breaker const&) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::set_heartbeat_config(rrr::HeartbeatConfig@rrr.heartbeat const&) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::set_keepalive(rrr::KeepaliveConfig@rrr.client const&) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::set_on_server_restart(rusty::Function<void (unsigned long, unsigned long)>) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::set_reconnect_address_for_testing(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::set_reconnect_policy(rrr::ReconnectPolicy@rrr.reconnect_policy const&) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::should_trip_circuit_for_error(int)",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::update_last_activity(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::update_pending_queue_config_for_test(rrr::RequestQueueConfig@rrr.request_queue const&) const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::validate_connection() const",
                ),
                (
                    "T",
                    "rrr::ClientConnection@rrr.client::~ClientConnection()",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::ClientPool(rrr::ClientPool@rrr.client&&)",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::ClientPool(rusty::Option<rusty::Arc<rrr::PollThread@rrr.reactor>>, rusty::Mutex<rrr::PoolState@rrr.client>, rusty::Mutex<rrr::PoolConfig@rrr.client>)",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::address_count() const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::close_all_idle(unsigned long) const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::close_idle_clients(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&, unsigned long) const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::get_client(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::get_healthy_client_count(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::is_client_healthy(rusty::Arc<rrr::Client@rrr.client> const&) const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::new_(rusty::Option<rusty::Arc<rrr::PollThread@rrr.reactor>>, rrr::PoolConfig@rrr.client)",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::operator=(rrr::ClientPool@rrr.client&&)",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::pool_config() const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::remove_all_unhealthy() const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::remove_unhealthy_clients(std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&) const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::rusty_mark_forgotten() const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::set_pool_config(rrr::PoolConfig@rrr.client) const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::total_client_count() const",
                ),
                (
                    "T",
                    "rrr::ClientPool@rrr.client::~ClientPool()",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::add_completion_callback(rusty::Function<void ()>) const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::create(long, rrr::FutureAttr@rrr.client)",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::get_error_code() const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::get_options() const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::get_reply() const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::get_retry_count() const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::get_timeout_type() const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::get_xid() const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::increment_retry_count()",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::new_(long, rrr::FutureAttr@rrr.client)",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::notify_ready(rusty::Arc<rrr::Future@rrr.client>) const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::ready() const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::safe_release(rusty::Arc<rrr::Future@rrr.client>)",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::set_options(rrr::RequestOptions@rrr.request_options const&) const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::set_timeout_type(rrr::TimeoutType@rrr.request_options)",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::should_retry() const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::timed_out() const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::timed_wait(double) const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::wait() const",
                ),
                (
                    "T",
                    "rrr::Future@rrr.client::wait_with_options() const",
                ),
                (
                    "T",
                    "rrr::FutureAttr@rrr.client::clone() const",
                ),
                (
                    "T",
                    "rrr::FutureAttr@rrr.client::default_()",
                ),
                (
                    "T",
                    "rrr::FutureAttr@rrr.client::new_(rrr::detail::CallbackWrapper@rrr.callback_wrapper<rusty::Function<void (rusty::Arc<rrr::Future@rrr.client>) const>>)",
                ),
                (
                    "T",
                    "rrr::FutureState@rrr.client::new_()",
                ),
                (
                    "T",
                    "rrr::KeepaliveConfig@rrr.client::aggressive()",
                ),
                (
                    "T",
                    "rrr::KeepaliveConfig@rrr.client::clone() const",
                ),
                (
                    "T",
                    "rrr::KeepaliveConfig@rrr.client::disabled()",
                ),
                (
                    "T",
                    "rrr::KeepaliveConfig@rrr.client::new_()",
                ),
                (
                    "T",
                    "rrr::KeepaliveConfig@rrr.client::relaxed()",
                ),
                (
                    "T",
                    "rrr::PoolConfig@rrr.client::aggressive()",
                ),
                (
                    "T",
                    "rrr::PoolConfig@rrr.client::clone() const",
                ),
                (
                    "T",
                    "rrr::PoolConfig@rrr.client::conservative()",
                ),
                (
                    "T",
                    "rrr::PoolConfig@rrr.client::defaults()",
                ),
                (
                    "T",
                    "rrr::PoolConfig@rrr.client::new_()",
                ),
                (
                    "T",
                    "rrr::PoolConfig@rrr.client::no_health_check()",
                ),
                (
                    "T",
                    "rrr::PoolState@rrr.client::new_()",
                ),
                (
                    "T",
                    "rrr::classify_request_failure@rrr.client(int)",
                ),
                (
                    "T",
                    "rrr::client_log_line@rrr.client(int, int, signed char const*, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>>)",
                ),
                (
                    "T",
                    "rrr::client_rand@rrr.client(int, int)",
                ),
                (
                    "T",
                    "rrr::client_sink_proxy@rrr.client(rrr::BufferSink@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::client_source_proxy@rrr.client(rrr::BufferSource@rrr.serializable&)",
                ),
                (
                    "T",
                    "rrr::client_text@rrr.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::client_text_i32@rrr.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, int, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::client_text_str@rrr.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::client_text_str_i32@rrr.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, int, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::client_text_str_pair@rrr.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::client_text_u32_str@rrr.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, unsigned int, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::client_text_u64_pair@rrr.client(std::__1::basic_string_view<char, std::__1::char_traits<char>>, unsigned long, std::__1::basic_string_view<char, std::__1::char_traits<char>>, unsigned long, std::__1::basic_string_view<char, std::__1::char_traits<char>>)",
                ),
                (
                    "T",
                    "rrr::client_verify@rrr.client(bool)",
                ),
                (
                    "T",
                    "rrr::clientconn_addr_to_string@rrr.client(signed char const*)",
                ),
                (
                    "T",
                    "rrr::clientconn_bind_channel_via_poll_thread@rrr.client(rrr::ClientConnection@rrr.client const&, rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "rrr::clientconn_connect_via_factory@rrr.client(rrr::ClientConnection@rrr.client const&, signed char const*)",
                ),
                (
                    "T",
                    "rrr::clientconn_decode_response_and_notify@rrr.client(rrr::ClientConnection@rrr.client const&, unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::clientconn_dispatch_frame_via_channel@rrr.client(rrr::ClientConnection@rrr.client const&, unsigned char const*, unsigned long)",
                ),
                (
                    "T",
                    "rrr::clientconn_enqueue_heartbeat_probe@rrr.client(rrr::ClientConnection@rrr.client const&)",
                ),
                (
                    "T",
                    "rrr::clientconn_fiber_channel_ptr@rrr.client(rusty::Option<rusty::Box<rrr::FiberChannel@rrr.fiber_channel, rusty::alloc::Global>> const&)",
                ),
                (
                    "T",
                    "rrr::clientconn_make_fiber_channel@rrr.client(rusty::Box<rrr::ChannelConnectionBase@rrr.channel, rusty::alloc::Global>)",
                ),
                (
                    "T",
                    "rrr::clientconn_map_system_error@rrr.client(int)",
                ),
                (
                    "T",
                    "rrr::clientconn_monotonic_ms_now@rrr.client()",
                ),
                (
                    "T",
                    "rrr::clientconn_reconnect@rrr.client(rrr::ClientConnection@rrr.client const&, rusty::Function<void (bool)>)",
                ),
                (
                    "T",
                    "rrr::clientconn_recv_job_entry@rrr.client(rusty::sync::Weak<rrr::ClientConnection@rrr.client>)",
                ),
                (
                    "T",
                    "rrr::clientconn_run_recv_loop@rrr.client(rrr::ClientConnection@rrr.client const&)",
                ),
                (
                    "T",
                    "rrr::clientpool_close_all_idle@rrr.client(rrr::ClientPool@rrr.client const&, unsigned long)",
                ),
                (
                    "T",
                    "rrr::clientpool_close_idle_clients@rrr.client(rrr::ClientPool@rrr.client const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&, unsigned long)",
                ),
                (
                    "T",
                    "rrr::clientpool_connect_client@rrr.client(rusty::Arc<rrr::Client@rrr.client> const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::clientpool_get_client@rrr.client(rrr::ClientPool@rrr.client const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::clientpool_get_healthy_client_count@rrr.client(rrr::ClientPool@rrr.client const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::clientpool_is_client_healthy_with@rrr.client(rrr::PoolConfig@rrr.client, rusty::Arc<rrr::Client@rrr.client> const&)",
                ),
                (
                    "T",
                    "rrr::clientpool_remove_all_unhealthy@rrr.client(rrr::ClientPool@rrr.client const&)",
                ),
                (
                    "T",
                    "rrr::clientpool_remove_unhealthy_clients@rrr.client(rrr::ClientPool@rrr.client const&, std::__1::basic_string<char, std::__1::char_traits<char>, std::__1::allocator<char>> const&)",
                ),
                (
                    "T",
                    "rrr::clientpool_select@rrr.client(rrr::LoadBalancingStrategy@rrr.load_balancer, rusty::port::vec::Vec@vec_port.vec<rusty::Arc<rrr::Client@rrr.client>, rusty::alloc::Global> const&, rrr::LoadBalancerState@rrr.load_balancer const&, unsigned long)",
                ),
                (
                    "T",
                    "rrr::make_pending_queue@rrr.client(rrr::RequestQueueConfig@rrr.request_queue const&)",
                ),
                (
                    "T",
                    "rrr::make_prefilled_cb_slots@rrr.client()",
                ),
                (
                    "T",
                    "rrr::make_write_archive@rrr.client(rrr::BufferSink@rrr.serializable*)",
                ),
                (
                    "T",
                    "rrr::reply_buffer_empty@rrr.client()",
                ),
                (
                    "T",
                    "rrr::reply_buffer_fill@rrr.client(rrr::ReplyBuffer@rrr.client&, std::__1::span<unsigned char const, 18446744073709551615ul>)",
                ),
                (
                    "T",
                    "rrr::request_copy_reply@rrr.client(rusty::Arc<rrr::Future@rrr.client> const&, rusty::Arc<rrr::Future@rrr.client> const&)",
                ),
            }
        ),
    ),
}

# Symbols that a module acquires in the production library from a hand-written
# module *implementation unit* that is not part of the generated crate.
#
# rrr.epoll_wrapper follows Rust std's sys-module pattern: the generated
# .cppm is the interface unit, and reactor/epoll_platform_linux.cc is the
# platform implementation unit that CMake compiles into librrr.a (see the
# "Platform implementation units for rrr.epoll_wrapper" block in
# CMakeLists.txt). Those definitions are therefore legitimately absent from
# the independently compiled crate object and present in production.
#
# This is an exhaustive allowlist, not a relaxation: the crate object must
# still match ABI_SPECS exactly, and the production library must match
# ABI_SPECS plus exactly these entries -- no more, no less.
PLATFORM_IMPL_SYMBOLS = {
    "rrr.epoll_wrapper": frozenset(
        {
            ("T", "rrr::epoll_add_impl@rrr.epoll_wrapper(int, int, int)"),
            ("T", "rrr::epoll_event_zeroed@rrr.epoll_wrapper()"),
            ("T", "rrr::epoll_open@rrr.epoll_wrapper()"),
            ("T", "rrr::epoll_remove_impl@rrr.epoll_wrapper(int, int)"),
            (
                "T",
                "rrr::epoll_update_impl@rrr.epoll_wrapper(int, int, int, int)",
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
        modules = extraction.load_manifest(root, root / EXTRACTION_MANIFEST)
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
    expected_files.add("rrr.cppm")
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

        if "namespace rrr::" in text:
            raise GateError(
                f"generated module {module.cpp_module} drifted to a nested namespace"
            )
        atomic_preamble = "#include <rusty/sync/atomic.hpp>"
        # rrr.epoll_wrapper and rrr.threading joined this set when their
        # carriers were retired: both canonical sources use
        # `std::sync::atomic`, so the structured preamble carries the same
        # include. Membership is still exact -- any other module that grew one
        # would fail the `elif` below.
        atomic_modules = {
            "rrr.basetypes",
            "rrr.connection_metrics",
            "rrr.completion_tracker",
            "rrr.epoll_wrapper",
            "rrr.threading",
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
        if module.cpp_module == "rrr.rand":
            require_exact_module_imports(text, "rrr.rand", ["vec_port.vec"])
            if text.count(rand_preamble) != 1:
                raise GateError(
                    "generated rrr.rand must contain exactly one structured "
                    "C-kernel preamble include"
                )
            ordered = (
                text.find("\nmodule;\n"),
                text.find(rand_preamble),
                text.find("#include <cstdint>"),
                text.find("export module rrr.rand;"),
            )
            if -1 in ordered or list(ordered) != sorted(ordered):
                raise GateError(
                    "generated rrr.rand C-kernel preamble is not between the "
                    "global module fragment and standard includes"
                )
            if "std::abort()" in text:
                raise GateError(
                    "generated rrr.rand hard-aborts a Rust assertion instead "
                    "of preserving panic/unwind failure semantics"
                )
        elif rand_preamble in text:
            raise GateError(
                f"rand C-kernel preamble leaked into {module.cpp_module}"
            )

        timing_preamble = '#include "misc/srpc_timing.h"'
        # rrr.threading joined this set with its carrier's retirement: the
        # canonical spin lock calls the same `srpc_cpu_pause` timing kernel.
        # Unlike the other two it is not import-free -- its `verify()` calls
        # reach rrr.debugging -- so the exact-import expectation is per module
        # rather than a blanket empty list.
        timing_modules = {
            "rrr.basetypes": [],
            "rrr.circuit_breaker": [],
            "rrr.threading": ["rrr.debugging"],
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

        if module.cpp_module == "rrr.request_options":
            require_exact_module_imports(
                text, "rrr.request_options", ["rrr.rand"]
            )
            for forbidden in (
                "namespace rand =",
                "using ::rand::",
                "using ::rrr::rand::",
                "using ::rrr::randgen_",
            ):
                if forbidden in text:
                    raise GateError(
                        "generated request-options private flat import leaked "
                        f"an alias/using surface: {forbidden!r}"
                    )
        elif module.cpp_module == "rrr.reconnect_policy":
            require_exact_module_imports(
                text, "rrr.reconnect_policy", ["rrr.rand"]
            )
            for forbidden in (
                "namespace rand =",
                "using ::rand::",
                "using ::rrr::rand::",
                "using ::rrr::randgen_",
            ):
                if forbidden in text:
                    raise GateError(
                        "generated reconnect-policy private flat import leaked "
                        f"an alias/using surface: {forbidden!r}"
                    )

        netdb_preamble = "#include <netdb.h>"
        if module.cpp_module == "rrr.connection_state":
            require_exact_module_imports(text, "rrr.connection_state", [])
        elif module.cpp_module == "rrr.heartbeat":
            require_exact_module_imports(
                text, "rrr.heartbeat", ["rrr.circuit_breaker"]
            )
        elif module.cpp_module == "rrr.request_queue":
            require_exact_module_imports(
                text,
                "rrr.request_queue",
                ["vec_port.vec", "rrr.circuit_breaker"],
            )
        elif module.cpp_module == "rrr.load_balancer":
            require_exact_module_imports(text, "rrr.load_balancer", [])
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
        elif module.cpp_module == "rrr.utils":
            require_exact_module_imports(text, "rrr.utils", ["rrr.logging"])
            if text.count(netdb_preamble) != 1:
                raise GateError(
                    "generated rrr.utils must contain exactly one structured "
                    "netdb preamble include"
                )
            ordered = (
                text.find("\nmodule;\n"),
                text.find(netdb_preamble),
                text.find("#include <cstdint>"),
                text.find("export module rrr.utils;"),
            )
            if -1 in ordered or list(ordered) != sorted(ordered):
                raise GateError(
                    "generated rrr.utils netdb preamble is not between the "
                    "global module fragment and standard includes"
                )
            for forbidden in (
                "export import rrr.logging;",
                "namespace logging =",
                "using ::rrr::log_line",
                "rrr::logging::log_line",
            ):
                if forbidden in text:
                    raise GateError(
                        "generated utils private indexed import leaked or "
                        f"misresolved its surface: {forbidden!r}"
                    )
        elif module.cpp_module == "rrr.frame_codec":
            require_exact_module_imports(
                text, "rrr.frame_codec", ["rrr.internal_protocol"]
            )
            frame_preambles = ("#include <vector>", "#include <rusty/io.hpp>")
            for preamble in frame_preambles:
                if text.count(preamble) != 1:
                    raise GateError(
                        "generated rrr.frame_codec must contain exactly one "
                        f"structured preamble include {preamble!r}"
                    )
            ordered = (
                text.find("\nmodule;\n"),
                text.find(frame_preambles[0]),
                text.find(frame_preambles[1]),
                text.find("#include <cstdint>"),
                text.find("export module rrr.frame_codec;"),
            )
            if -1 in ordered or list(ordered) != sorted(ordered):
                raise GateError(
                    "generated rrr.frame_codec structured preambles are not "
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

    root_text = read_generated(output / "rrr.cppm", "root module")
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
        "export module rrr;",
        "namespace rrr {",
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
    initializer = "initializer for module rrr.completion_tracker"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if (
            symbol_owner_module(symbol) == "rrr.completion_tracker"
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

    expected = Counter(ABI_SPECS["rrr.completion_tracker"].symbols)
    expected[("T", "initializer for module rrr.completion_tracker")] += 1
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
    initializer = "initializer for module rrr.rand"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if symbol_owner_module(symbol) == "rrr.rand" or symbol == initializer:
            entries.append((kind, symbol))
    return entries


def require_rand_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin rand's 12-function ABI and sole module initializer exactly."""

    expected = Counter(ABI_SPECS["rrr.rand"].symbols)
    expected[("T", "initializer for module rrr.rand")] += 1
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
    initializer = "initializer for module rrr.request_options"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if (
            symbol_owner_module(symbol) == "rrr.request_options"
            or symbol == initializer
        ):
            entries.append((kind, symbol))
    return entries


def require_request_options_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin request-options' 12-function ABI and initializer exactly."""

    expected = Counter(ABI_SPECS["rrr.request_options"].symbols)
    expected[("T", "initializer for module rrr.request_options")] += 1
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
    initializer = "initializer for module rrr.reconnect_policy"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if (
            symbol_owner_module(symbol) == "rrr.reconnect_policy"
            or symbol == initializer
        ):
            entries.append((kind, symbol))
    return entries


def require_reconnect_policy_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin reconnect-policy's 11-function ABI and initializer exactly."""

    expected = Counter(ABI_SPECS["rrr.reconnect_policy"].symbols)
    expected[("T", "initializer for module rrr.reconnect_policy")] += 1
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
    initializer = "initializer for module rrr.circuit_breaker"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if (
            symbol_owner_module(symbol) == "rrr.circuit_breaker"
            or symbol == initializer
        ):
            entries.append((kind, symbol))
    return entries


def require_circuit_breaker_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin circuit-breaker's 20-function ABI and initializer exactly."""

    expected = Counter(ABI_SPECS["rrr.circuit_breaker"].symbols)
    expected[("T", "initializer for module rrr.circuit_breaker")] += 1
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
    initializer = "initializer for module rrr.basetypes"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if symbol_owner_module(symbol) == "rrr.basetypes" or symbol == initializer:
            entries.append((kind, symbol))
    return entries


def require_basetypes_raw_symbols(
    description: str,
    entries: list[tuple[str, str]],
) -> None:
    """Pin basetypes' 28-entry API/data ABI and initializer exactly."""

    expected = Counter(ABI_SPECS["rrr.basetypes"].symbols)
    expected[("T", "initializer for module rrr.basetypes")] += 1
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
    initializer = "initializer for module rrr.request_queue"
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        match = NM_LINE.match(line)
        if match is None:
            continue
        kind, symbol = match.groups()
        if not kind.isupper() or kind in {"U", "V", "W"}:
            continue
        if symbol_owner_module(symbol) == "rrr.request_queue" or symbol == initializer:
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

    expected = Counter(ABI_SPECS["rrr.request_queue"].symbols)
    expected[("T", "initializer for module rrr.request_queue")] += 1
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

    return exact_module_raw_symbols(nm, root, binary, "rrr.utils")


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
        "rrr::AddrInfo@rrr.utils::AddrInfo(addrinfo*, rusty::Cell<bool>)",
        "rrr::AddrInfo@rrr.utils::AddrInfo(rrr::AddrInfo@rrr.utils&&)",
        "rrr::AddrInfo@rrr.utils::~AddrInfo()",
    )
    expected = Counter(ABI_SPECS["rrr.utils"].symbols)
    for symbol in aliased:
        expected[("T", symbol)] += 1
    expected[("T", "initializer for module rrr.utils")] += 1
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

import rrr.callback_wrapper;
import rrr.basetypes;
import rrr.debugging;
import rrr.circuit_breaker;
import rrr.completion_tracker;
import rrr.connection_metrics;
import rrr.connection_state;
import rrr.errors;
import rrr.frame_codec;
import rrr.heartbeat;
import rrr.internal_protocol;
import rrr.load_balancer;
import rrr.rand;
import rrr.reconnect_policy;
import rrr.request_options;
import rrr.request_queue;
import rrr.stat;
import rrr.utils;

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
// rrr.logging is canonical Rust now, so the gate no longer substitutes a
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
    // Exactly the 23 characters rrr.logging renders, so the decorated line the
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

// rrr.debugging reaches libc through exactly three plain-C seams
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

static_assert(std::is_same_v<rrr::RandWeightVec, std::vector<double>>);

static_assert(std::is_same_v<
              rrr::FrameCursor,
              rusty::io::Cursor<std::vector<std::uint8_t>>>);
static_assert(std::is_same_v<
              std::underlying_type_t<rrr::FrameDecodeStatus>,
              std::int32_t>);
static_assert(sizeof(rrr::FrameDecodeStatus) == 4);
static_assert(alignof(rrr::FrameDecodeStatus) == 4);
static_assert(rrr::kFrameHeaderSize == 4);
static_assert(rrr::kMaxFramePayloadSize == INT32_MAX);
static_assert(std::is_standard_layout_v<rrr::FrameHeader>);
static_assert(std::is_trivially_copyable_v<rrr::FrameHeader>);
static_assert(rrr::FrameHeader::is_send && rrr::FrameHeader::is_sync);
static_assert(sizeof(rrr::FrameHeader) == 8);
static_assert(alignof(rrr::FrameHeader) == 4);
static_assert(offsetof(rrr::FrameHeader, payload_size) == 0);
static_assert(offsetof(rrr::FrameHeader, extended_header_flag) == 4);
static_assert(sizeof(rrr::FrameView) == 24);
static_assert(alignof(rrr::FrameView) == 8);
static_assert(offsetof(rrr::FrameView, header) == 0);
static_assert(offsetof(rrr::FrameView, payload) == 8);
static_assert(offsetof(rrr::FrameView, payload_size) == 16);
static_assert(sizeof(rrr::FrameCursor) == 32);
static_assert(alignof(rrr::FrameCursor) == 8);
static_assert(sizeof(rrr::FrameStreamReader) == 40);
static_assert(alignof(rrr::FrameStreamReader) == 8);
static_assert(offsetof(rrr::FrameStreamReader, cursor_) == 0);
static_assert(offsetof(rrr::FrameStreamReader, noncopy_) == 32);
static_assert(!std::is_default_constructible_v<rrr::FrameStreamReader>);
static_assert(!std::is_copy_constructible_v<rrr::FrameStreamReader>);
static_assert(!std::is_copy_assignable_v<rrr::FrameStreamReader>);
static_assert(std::is_move_constructible_v<rrr::FrameStreamReader>);
static_assert(std::is_move_assignable_v<rrr::FrameStreamReader>);
static_assert(std::is_same_v<
              decltype(&rrr::frame_decode_status_to_string),
              std::string_view (*)(rrr::FrameDecodeStatus)>);
static_assert(std::is_same_v<
              decltype(&rrr::frame_codec_write_header),
              bool (*)(std::span<std::uint8_t>, std::int32_t, bool)>);
static_assert(std::is_same_v<
              decltype(&rrr::frame_codec_peek_header),
              rrr::FrameDecodeStatus (*)(
                  std::span<const std::uint8_t>, rrr::FrameHeader&)>);
static_assert(std::is_same_v<
              decltype(&rrr::frame_codec_encode_into),
              bool (*)(std::vector<std::uint8_t>&, const std::uint8_t*,
                       std::int32_t, bool)>);
static_assert(std::is_same_v<
              decltype(&rrr::FrameHeader::total_frame_size),
              std::int32_t (rrr::FrameHeader::*)() const>);
static_assert(std::is_same_v<
              decltype(&rrr::FrameStreamReader::append),
              void (rrr::FrameStreamReader::*)(
                  const std::uint8_t*, std::size_t)>);
static_assert(std::is_same_v<
              decltype(&rrr::FrameStreamReader::next_frame),
              rrr::FrameDecodeStatus (rrr::FrameStreamReader::*)(
                  rrr::FrameView&) const>);
static_assert(std::is_same_v<
              decltype(&rrr::FrameStreamReader::buffered_bytes),
              std::size_t (rrr::FrameStreamReader::*)() const>);

static_assert(std::is_same_v<rrr::i8, std::int8_t>);
static_assert(std::is_same_v<rrr::i16, std::int16_t>);
static_assert(std::is_same_v<rrr::i32, std::int32_t>);
static_assert(std::is_same_v<rrr::i64, std::int64_t>);
static_assert(sizeof(rrr::SparseInt) == 1);
static_assert(alignof(rrr::SparseInt) == 1);
static_assert(sizeof(rrr::v32) == 4);
static_assert(alignof(rrr::v32) == 4);
static_assert(sizeof(rrr::v64) == 8);
static_assert(alignof(rrr::v64) == 8);
static_assert(sizeof(rrr::Counter) == 8);
static_assert(alignof(rrr::Counter) == 8);
static_assert(sizeof(rrr::Time) == 1);
static_assert(alignof(rrr::Time) == 1);
static_assert(sizeof(rrr::Timer) == 16);
static_assert(alignof(rrr::Timer) == 8);
static_assert(offsetof(rrr::v32, val_field) == 0);
static_assert(offsetof(rrr::v64, val_field) == 0);
static_assert(offsetof(rrr::Counter, next_field) == 0);
static_assert(offsetof(rrr::Timer, begin_us) == 0);
static_assert(offsetof(rrr::Timer, end_us) == 8);
static_assert(rrr::SparseInt::is_send && rrr::SparseInt::is_sync);
static_assert(rrr::v32::is_send && rrr::v32::is_sync);
static_assert(rrr::v64::is_send && rrr::v64::is_sync);
static_assert(rrr::Counter::is_send && rrr::Counter::is_sync);
static_assert(rrr::Time::is_send && rrr::Time::is_sync);
static_assert(rrr::Timer::is_send && rrr::Timer::is_sync);
static_assert(sizeof(rrr::AtomicI64) == 8);
static_assert(alignof(rrr::AtomicI64) == 8);
static_assert(std::is_copy_constructible_v<rrr::Counter>);
static_assert(std::is_copy_assignable_v<rrr::Counter>);
static_assert(std::is_move_constructible_v<rrr::Counter>);
static_assert(std::is_move_assignable_v<rrr::Counter>);
static_assert(std::is_same_v<
              decltype(&rrr::SparseInt::buf_size),
              std::size_t (*)(std::uint8_t)>);
static_assert(std::is_same_v<
              decltype(&rrr::SparseInt::dump32),
              std::size_t (*)(std::int32_t, std::uint8_t*)>);
static_assert(std::is_same_v<
              decltype(&rrr::SparseInt::dump64),
              std::size_t (*)(std::int64_t, std::uint8_t*)>);
static_assert(std::is_same_v<
              decltype(&rrr::SparseInt::load32),
              std::int32_t (*)(const std::uint8_t*)>);
static_assert(std::is_same_v<
              decltype(&rrr::SparseInt::load64),
              std::int64_t (*)(const std::uint8_t*)>);
static_assert(std::is_same_v<
              decltype(&rrr::Counter::next),
              std::int64_t (rrr::Counter::*)(std::int64_t) const>);
static_assert(std::is_same_v<
              decltype(&rrr::Timer::elapsed),
              double (rrr::Timer::*)() const>);

static_assert(std::is_same_v<
              std::underlying_type_t<rrr::CircuitState>, std::int32_t>);
static_assert(sizeof(rrr::CircuitState) == 4);
static_assert(alignof(rrr::CircuitState) == 4);
static_assert(std::is_standard_layout_v<rrr::CircuitBreakerConfig>);
static_assert(std::is_trivially_copyable_v<rrr::CircuitBreakerConfig>);
static_assert(rrr::CircuitBreakerConfig::is_send);
static_assert(rrr::CircuitBreakerConfig::is_sync);
static_assert(sizeof(rrr::CircuitBreakerConfig) == 16);
static_assert(alignof(rrr::CircuitBreakerConfig) == 4);
static_assert(offsetof(rrr::CircuitBreakerConfig, failure_threshold) == 0);
static_assert(offsetof(rrr::CircuitBreakerConfig, success_threshold) == 4);
static_assert(offsetof(rrr::CircuitBreakerConfig, timeout_ms) == 8);
static_assert(offsetof(rrr::CircuitBreakerConfig, enabled) == 12);
static_assert(sizeof(rrr::CircuitBreaker) == 48);
static_assert(alignof(rrr::CircuitBreaker) == 8);
static_assert(offsetof(rrr::CircuitBreaker, config_field) == 0);
static_assert(offsetof(rrr::CircuitBreaker, state_field) == 16);
static_assert(offsetof(rrr::CircuitBreaker, failure_count_field) == 20);
static_assert(offsetof(rrr::CircuitBreaker, success_count_field) == 24);
static_assert(offsetof(rrr::CircuitBreaker, last_failure_time) == 32);
static_assert(offsetof(rrr::CircuitBreaker, probe_in_progress) == 40);
static_assert(rrr::CircuitBreaker::is_send);
static_assert(!rusty::is_sync<rrr::CircuitBreaker>::value);
static_assert(std::is_same_v<
              decltype(&rrr::CircuitBreaker::new_),
              rrr::CircuitBreaker (*)(rrr::CircuitBreakerConfig)>);
static_assert(std::is_same_v<
              decltype(&rrr::CircuitBreaker::set_config),
              void (rrr::CircuitBreaker::*)(rrr::CircuitBreakerConfig) const>);
static_assert(std::is_same_v<
              decltype(&rrr::CircuitBreaker::allow_request),
              bool (rrr::CircuitBreaker::*)() const>);
static_assert(std::is_same_v<
              decltype(&rrr::current_time_us), std::uint64_t (*)()>);

static_assert(std::is_same_v<
              std::underlying_type_t<rrr::OverflowStrategy>, std::int32_t>);
static_assert(sizeof(rrr::OverflowStrategy) == 4);
static_assert(alignof(rrr::OverflowStrategy) == 4);
static_assert(std::is_same_v<
              rrr::QueuedRequestCallback,
              rusty::Function<void(std::int32_t)>>);
static_assert(std::is_same_v<
              decltype(&rrr::rq_invoke_callback_safely),
              void (*)(rrr::QueuedRequestCallback, std::int32_t)>);
static_assert(sizeof(rrr::QueuedRequestCallback) == 48);
static_assert(alignof(rrr::QueuedRequestCallback) == 16);
static_assert(sizeof(rrr::QueuedRequest) == 96);
static_assert(alignof(rrr::QueuedRequest) == 16);
static_assert(offsetof(rrr::QueuedRequest, xid) == 0);
static_assert(offsetof(rrr::QueuedRequest, rpc_id) == 8);
static_assert(offsetof(rrr::QueuedRequest, timestamp_us) == 16);
static_assert(offsetof(rrr::QueuedRequest, retry_count) == 24);
static_assert(offsetof(rrr::QueuedRequest, callback) == 32);
static_assert(offsetof(rrr::QueuedRequest, ttl_ms) == 80);
static_assert(!rusty::is_send<rrr::QueuedRequest>::value);
static_assert(!rusty::is_sync<rrr::QueuedRequest>::value);
static_assert(std::is_standard_layout_v<rrr::RequestQueueConfig>);
static_assert(std::is_trivially_copyable_v<rrr::RequestQueueConfig>);
static_assert(rrr::RequestQueueConfig::is_send);
static_assert(rrr::RequestQueueConfig::is_sync);
static_assert(sizeof(rrr::RequestQueueConfig) == 24);
static_assert(alignof(rrr::RequestQueueConfig) == 8);
static_assert(offsetof(rrr::RequestQueueConfig, max_size) == 0);
static_assert(offsetof(rrr::RequestQueueConfig, default_ttl_ms) == 8);
static_assert(offsetof(rrr::RequestQueueConfig, overflow_strategy) == 12);
static_assert(offsetof(rrr::RequestQueueConfig, enabled) == 16);
static_assert(sizeof(rrr::RequestQueue) == 96);
static_assert(alignof(rrr::RequestQueue) == 8);
static_assert(offsetof(rrr::RequestQueue, config_) == 0);
static_assert(offsetof(rrr::RequestQueue, queue_) == 24);
static_assert(!rusty::is_send<rrr::RequestQueue>::value);
static_assert(!rusty::is_sync<rrr::RequestQueue>::value);
static_assert(std::is_same_v<
              decltype(&rrr::RequestQueue::enqueue),
              bool (rrr::RequestQueue::*)(rrr::QueuedRequest) const>);
static_assert(std::is_same_v<
              decltype(&rrr::RequestQueue::dequeue),
              rusty::Option<rrr::QueuedRequest> (rrr::RequestQueue::*)()>);
static_assert(std::is_same_v<
              decltype(&rrr::RequestQueue::expire_stale),
              std::size_t (rrr::RequestQueue::*)() const>);
static_assert(std::is_same_v<
              decltype(&rrr::RequestQueue::full),
              bool (rrr::RequestQueue::*)()>);
static_assert(std::is_same_v<
              decltype(&rrr::RequestQueue::remaining_capacity),
              std::size_t (rrr::RequestQueue::*)()>);
static_assert(std::is_same_v<
              decltype(&rrr::RequestQueue::clear_all),
              void (rrr::RequestQueue::*)(std::int32_t) const>);
static_assert(std::is_same_v<
              decltype(&rrr::RequestQueue::update_config),
              void (rrr::RequestQueue::*)(rrr::RequestQueueConfig) const>);
static_assert(std::is_same_v<
              decltype(&rrr::randgen_zero_pad),
              std::string (*)(std::string, std::int32_t)>);
static_assert(std::is_same_v<
              decltype(&rrr::RandomGenerator::int2str_n),
              std::string (*)(std::int32_t, std::int32_t)>);
static_assert(std::is_same_v<
              decltype(&rrr::RandomGenerator::weighted_select),
              std::uint32_t (*)(const std::vector<double>&)>);

static_assert(std::is_same_v<
              std::underlying_type_t<rrr::ConnectionState>, std::int32_t>);
static_assert(sizeof(rrr::ConnectionState) == 4);
static_assert(alignof(rrr::ConnectionState) == 4);
static_assert(std::is_same_v<
              rrr::StateChangeCallback,
              rusty::Function<void(rrr::ConnectionState,
                                   rrr::ConnectionState) const>>);
static_assert(sizeof(rrr::StateChangeCallback) == 48);
static_assert(alignof(rrr::StateChangeCallback) == 16);
static_assert(sizeof(rrr::ConnectionStateMachine) == 64);
static_assert(alignof(rrr::ConnectionStateMachine) == 16);
static_assert(offsetof(rrr::ConnectionStateMachine, state_field) == 0);
static_assert(offsetof(rrr::ConnectionStateMachine, on_state_change) == 16);
static_assert(!std::is_copy_constructible_v<rrr::ConnectionStateMachine>);
static_assert(std::is_move_constructible_v<rrr::ConnectionStateMachine>);
static_assert(!rusty::is_send<rrr::StateChangeCallback>::value);
static_assert(!rusty::is_sync<rrr::StateChangeCallback>::value);
static_assert(!rusty::is_send<rrr::ConnectionStateMachine>::value);
static_assert(!rusty::is_sync<rrr::ConnectionStateMachine>::value);
static_assert(std::is_same_v<
              decltype(&rrr::ConnectionStateMachine::set_on_state_change),
              void (rrr::ConnectionStateMachine::*)(rrr::StateChangeCallback)>);
static_assert(std::is_same_v<
              decltype(&rrr::ConnectionStateMachine::transition_to),
              bool (rrr::ConnectionStateMachine::*)(rrr::ConnectionState) const>);

static_assert(std::is_same_v<
              rrr::HeartbeatTimeoutCallback,
              rusty::Function<void()>>);
static_assert(sizeof(rrr::HeartbeatTimeoutCallback) == 48);
static_assert(alignof(rrr::HeartbeatTimeoutCallback) == 16);
static_assert(std::is_standard_layout_v<rrr::HeartbeatConfig>);
static_assert(std::is_trivially_copyable_v<rrr::HeartbeatConfig>);
static_assert(rrr::HeartbeatConfig::is_send);
static_assert(rrr::HeartbeatConfig::is_sync);
static_assert(sizeof(rrr::HeartbeatConfig) == 16);
static_assert(alignof(rrr::HeartbeatConfig) == 4);
static_assert(offsetof(rrr::HeartbeatConfig, enabled) == 0);
static_assert(offsetof(rrr::HeartbeatConfig, interval_ms) == 4);
static_assert(offsetof(rrr::HeartbeatConfig, timeout_ms) == 8);
static_assert(offsetof(rrr::HeartbeatConfig, max_missed) == 12);
static_assert(sizeof(rrr::HeartbeatManager) == 112);
static_assert(alignof(rrr::HeartbeatManager) == 16);
static_assert(offsetof(rrr::HeartbeatManager, config_field) == 0);
static_assert(offsetof(rrr::HeartbeatManager, last_send_time) == 16);
static_assert(offsetof(rrr::HeartbeatManager, last_recv_time) == 24);
static_assert(offsetof(rrr::HeartbeatManager, missed_count_field) == 32);
static_assert(offsetof(rrr::HeartbeatManager, pending_pong) == 36);
static_assert(offsetof(rrr::HeartbeatManager, timed_out) == 37);
static_assert(offsetof(rrr::HeartbeatManager, on_timeout) == 48);
static_assert(!std::is_copy_constructible_v<rrr::HeartbeatManager>);
static_assert(std::is_move_constructible_v<rrr::HeartbeatManager>);
static_assert(!rusty::is_send<rrr::HeartbeatTimeoutCallback>::value);
static_assert(!rusty::is_sync<rrr::HeartbeatTimeoutCallback>::value);
static_assert(!rusty::is_send<rrr::HeartbeatManager>::value);
static_assert(!rusty::is_sync<rrr::HeartbeatManager>::value);
static_assert(std::is_same_v<
              decltype(&rrr::HeartbeatManager::new_),
              rrr::HeartbeatManager (*)(const rrr::HeartbeatConfig&)>);
static_assert(std::is_same_v<
              decltype(&rrr::HeartbeatManager::set_on_timeout),
              void (rrr::HeartbeatManager::*)(rrr::HeartbeatTimeoutCallback) const>);
static_assert(std::is_same_v<
              decltype(&rrr::HeartbeatManager::check_timeout),
              bool (rrr::HeartbeatManager::*)() const>);
static_assert(std::is_same_v<
              decltype(&rrr::heartbeat_time_us), std::uint64_t (*)()>);
static_assert(std::is_same_v<
              std::underlying_type_t<rrr::LoadBalancingStrategy>,
              std::int32_t>);
static_assert(sizeof(rrr::LoadBalancingStrategy) == 4);
static_assert(alignof(rrr::LoadBalancingStrategy) == 4);
static_assert(sizeof(rrr::LoadBalancerState) == 8);
static_assert(alignof(rrr::LoadBalancerState) == 8);
static_assert(offsetof(rrr::LoadBalancerState, round_robin_index_field) == 0);
static_assert(std::is_standard_layout_v<rrr::LoadBalancerState>);
static_assert(rrr::LoadBalancerState::is_send);
static_assert(!rusty::is_sync<rrr::LoadBalancerState>::value);
static_assert(sizeof(rrr::LoadBalancer) == 1);
static_assert(std::is_empty_v<rrr::LoadBalancer>);
static_assert(rrr::LoadBalancer::is_send && rrr::LoadBalancer::is_sync);
static_assert(std::is_same_v<
              decltype(&rrr::LoadBalancer::select_random),
              std::size_t (*)(std::size_t, std::size_t)>);
static_assert(std::is_same_v<
              decltype(&rrr::LoadBalancer::select_round_robin),
              std::size_t (*)(std::size_t,
                              const rrr::LoadBalancerState&)>);
static_assert(std::is_same_v<
              decltype(&rrr::load_balancing_strategy_to_string),
              std::string_view (*)(rrr::LoadBalancingStrategy)>);
static_assert(sizeof(rrr::AddrInfo) == 16);
static_assert(alignof(rrr::AddrInfo) == 8);
static_assert(offsetof(rrr::AddrInfo, info_) == 0);
static_assert(offsetof(rrr::AddrInfo, owned_) == 8);
static_assert(offsetof(rrr::AddrInfo, _rusty_forgotten) == 9);
static_assert(std::is_standard_layout_v<rrr::AddrInfo>);
static_assert(!std::is_copy_constructible_v<rrr::AddrInfo>);
static_assert(!std::is_copy_assignable_v<rrr::AddrInfo>);
static_assert(std::is_move_constructible_v<rrr::AddrInfo>);
static_assert(std::is_move_assignable_v<rrr::AddrInfo>);
// The move constructor itself is noexcept (pinned in the generated surface),
// but is_nothrow_constructible also accounts for the legacy noexcept(false)
// destructor, so the aggregate trait is deliberately false.
static_assert(!std::is_nothrow_move_constructible_v<rrr::AddrInfo>);
static_assert(std::is_nothrow_move_assignable_v<rrr::AddrInfo>);
static_assert(!std::is_nothrow_destructible_v<rrr::AddrInfo>);
static_assert(std::is_same_v<
              decltype(&rrr::AddrInfo::get),
              addrinfo* (rrr::AddrInfo::*)() const>);
static_assert(std::is_same_v<
              decltype(&rrr::AddrInfo::valid),
              bool (rrr::AddrInfo::*)() const>);
static_assert(std::is_same_v<
              decltype(&rrr::find_open_port), std::int32_t (*)()>);
static_assert(std::is_same_v<
              decltype(&rrr::get_host_name), std::string (*)()>);
static_assert(std::is_same_v<
              decltype(&rrr::randgen_zero_pad),
              std::string (*)(std::string, std::int32_t)>);
static_assert(std::is_same_v<
              decltype(&rrr::RandomGenerator::int2str_n),
              std::string (*)(std::int32_t, std::int32_t)>);
static_assert(std::is_same_v<
              decltype(&rrr::RandomGenerator::weighted_select),
              std::uint32_t (*)(const std::vector<double>&)>);

static_assert(std::is_same_v<
              std::underlying_type_t<rrr::TimeoutType>, std::int32_t>);
static_assert(sizeof(rrr::TimeoutType) == 4);
static_assert(alignof(rrr::TimeoutType) == 4);
static_assert(std::is_trivially_copyable_v<rrr::TimeoutType>);
static_assert(std::is_standard_layout_v<rrr::RequestOptions>);
static_assert(std::is_trivially_copyable_v<rrr::RequestOptions>);
static_assert(rrr::RequestOptions::is_send);
static_assert(rrr::RequestOptions::is_sync);
static_assert(sizeof(rrr::RequestOptions) == 32);
static_assert(alignof(rrr::RequestOptions) == 8);
static_assert(offsetof(rrr::RequestOptions, timeout_ms) == 0);
static_assert(offsetof(rrr::RequestOptions, total_timeout_ms) == 8);
static_assert(offsetof(rrr::RequestOptions, max_retries) == 16);
static_assert(offsetof(rrr::RequestOptions, base_delay_ms) == 18);
static_assert(offsetof(rrr::RequestOptions, max_delay_ms) == 20);
static_assert(offsetof(rrr::RequestOptions, jitter_factor) == 24);
static_assert(offsetof(rrr::RequestOptions, idempotent) == 28);
static_assert(std::is_same_v<
              decltype(&rrr::RequestOptions::new_),
              rrr::RequestOptions (*)()>);
static_assert(std::is_same_v<
              decltype(&rrr::RequestOptions::with_retry),
              rrr::RequestOptions (*)(std::uint16_t, std::uint64_t)>);
static_assert(std::is_same_v<
              decltype(&rrr::RequestOptions::can_retry),
              bool (rrr::RequestOptions::*)(std::uint16_t) const>);
static_assert(std::is_same_v<
              decltype(&rrr::RequestOptions::calculate_delay_ms),
              std::uint64_t (rrr::RequestOptions::*)(std::uint16_t) const>);
static_assert(std::is_same_v<
              decltype(&rrr::RequestOptions::is_total_timeout_exceeded),
              bool (rrr::RequestOptions::*)(std::uint64_t) const>);
static_assert(std::is_same_v<
              decltype(&rrr::RequestOptions::remaining_time_ms),
              std::uint64_t (rrr::RequestOptions::*)(std::uint64_t) const>);
static_assert(std::is_same_v<
              decltype(&rrr::timeout_type_to_string),
              std::string_view (*)(rrr::TimeoutType)>);

static_assert(std::is_standard_layout_v<rrr::ReconnectPolicy>);
static_assert(std::is_trivially_copyable_v<rrr::ReconnectPolicy>);
static_assert(rrr::ReconnectPolicy::is_send);
static_assert(rrr::ReconnectPolicy::is_sync);
static_assert(sizeof(rrr::ReconnectPolicy) == 32);
static_assert(alignof(rrr::ReconnectPolicy) == 8);
static_assert(offsetof(rrr::ReconnectPolicy, auto_reconnect) == 0);
static_assert(offsetof(rrr::ReconnectPolicy, max_retries) == 4);
static_assert(offsetof(rrr::ReconnectPolicy, initial_delay_ms) == 8);
static_assert(offsetof(rrr::ReconnectPolicy, max_delay_ms) == 12);
static_assert(offsetof(rrr::ReconnectPolicy, backoff_multiplier) == 16);
static_assert(offsetof(rrr::ReconnectPolicy, jitter_enabled) == 24);
static_assert(sizeof(rrr::ReconnectCalculator) == 16);
static_assert(alignof(rrr::ReconnectCalculator) == 8);
static_assert(!std::is_copy_constructible_v<rrr::ReconnectCalculator>);
static_assert(std::is_move_constructible_v<rrr::ReconnectCalculator>);
static_assert(std::is_same_v<
              decltype(rrr::ReconnectCalculator::policy),
              const rrr::ReconnectPolicy&>);
static_assert(std::is_same_v<
              decltype(rrr::ReconnectCalculator::retries),
              rusty::Cell<std::uint32_t>>);
static_assert(std::is_same_v<
              decltype(&rrr::ReconnectPolicy::new_),
              rrr::ReconnectPolicy (*)()>);
static_assert(std::is_same_v<
              decltype(&rrr::ReconnectCalculator::new_),
              rrr::ReconnectCalculator (*)(const rrr::ReconnectPolicy&)>);
static_assert(std::is_same_v<
              decltype(&rrr::ReconnectCalculator::should_retry),
              bool (rrr::ReconnectCalculator::*)() const>);
static_assert(std::is_same_v<
              decltype(&rrr::ReconnectCalculator::next_delay_ms),
              std::uint32_t (rrr::ReconnectCalculator::*)() const>);
static_assert(std::is_same_v<
              decltype(&rrr::ReconnectCalculator::peek_delay_ms),
              std::uint32_t (rrr::ReconnectCalculator::*)() const>);
static_assert(std::is_same_v<
              decltype(&rrr::ReconnectCalculator::reset),
              void (rrr::ReconnectCalculator::*)() const>);
static_assert(std::is_same_v<
              decltype(&rrr::ReconnectCalculator::retry_count),
              std::uint32_t (rrr::ReconnectCalculator::*)() const>);
static_assert(std::is_same_v<
              decltype(&rrr::ReconnectCalculator::retries_exhausted),
              bool (rrr::ReconnectCalculator::*)() const>);

static_assert(sizeof(rrr::RpcErrorCategory) == sizeof(std::int32_t));
static_assert(sizeof(rrr::RpcError) == sizeof(std::int32_t));
static_assert(std::is_same_v<
              std::underlying_type_t<rrr::RpcErrorCategory>, std::int32_t>);
static_assert(std::is_same_v<
              std::underlying_type_t<rrr::RpcError>, std::int32_t>);
static_assert(std::is_trivially_copyable_v<rrr::RpcErrorCategory>);
static_assert(std::is_trivially_copyable_v<rrr::RpcError>);

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
    rrr::detail::CallbackWrapper<CallbackFunction>;
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
static_assert(std::is_standard_layout_v<rrr::AvgStat>);
static_assert(std::is_trivially_copyable_v<rrr::AvgStat>);
static_assert(sizeof(rrr::AvgStat) == 5 * sizeof(std::int64_t));
static_assert(alignof(rrr::AvgStat) == alignof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, n_stat_) == 0 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, sum_) == 1 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, avg_) == 2 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, max_) == 3 * sizeof(std::int64_t));
static_assert(offsetof(rrr::AvgStat, min_) == 4 * sizeof(std::int64_t));

using MetricsAtomicU64 = rusty::sync::atomic::AtomicU64;
static_assert(sizeof(MetricsAtomicU64) == sizeof(std::uint64_t));
static_assert(alignof(MetricsAtomicU64) == alignof(std::uint64_t));
static_assert(std::is_standard_layout_v<rrr::ConnectionMetrics>);
static_assert(std::is_copy_constructible_v<rrr::ConnectionMetrics>);
static_assert(std::is_copy_assignable_v<rrr::ConnectionMetrics>);
static_assert(std::is_move_constructible_v<rrr::ConnectionMetrics>);
static_assert(std::is_move_assignable_v<rrr::ConnectionMetrics>);
static_assert(!std::is_trivially_copyable_v<rrr::ConnectionMetrics>);
static_assert(rrr::ConnectionMetrics::is_send);
static_assert(rrr::ConnectionMetrics::is_sync);
static_assert(
    sizeof(rrr::ConnectionMetrics) == 18 * sizeof(std::uint64_t));
static_assert(
    alignof(rrr::ConnectionMetrics) == alignof(std::uint64_t));
static_assert(offsetof(rrr::ConnectionMetrics, requests_sent_field) ==
              0 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, requests_completed_field) ==
              1 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, requests_failed_field) ==
              2 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, requests_timed_out_field) ==
              3 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, in_flight_requests_field) ==
              4 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, bytes_sent_field) ==
              5 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, bytes_received_field) ==
              6 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, reconnect_count_field) ==
              7 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, retry_attempts_field) ==
              8 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, queue_dropped_requests_field) ==
              9 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, circuit_open_rejections_field) ==
              10 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, circuit_open_transitions_field) ==
              11 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, circuit_half_open_transitions_field) ==
              12 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, circuit_closed_transitions_field) ==
              13 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, connect_time_ms_field) ==
              14 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, total_latency_us_field) ==
              15 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, min_latency_us_field) ==
              16 * sizeof(MetricsAtomicU64));
static_assert(offsetof(rrr::ConnectionMetrics, max_latency_us_field) ==
              17 * sizeof(MetricsAtomicU64));

static_assert(std::is_same_v<
              std::underlying_type_t<rrr::CompletionStatus>, std::int32_t>);
static_assert(sizeof(rrr::CompletionStatus) == 4);
static_assert(alignof(rrr::CompletionStatus) == 4);
static_assert(std::is_trivially_copyable_v<rrr::CompletionStatus>);

static_assert(std::is_standard_layout_v<rrr::CompletionTrackerConfig>);
static_assert(std::is_trivially_copyable_v<rrr::CompletionTrackerConfig>);
static_assert(rrr::CompletionTrackerConfig::is_send);
static_assert(rrr::CompletionTrackerConfig::is_sync);
static_assert(sizeof(rrr::CompletionTrackerConfig) == 24);
static_assert(alignof(rrr::CompletionTrackerConfig) == 8);
static_assert(offsetof(rrr::CompletionTrackerConfig, ttl_ms) == 0);
static_assert(offsetof(rrr::CompletionTrackerConfig, max_entries) == 8);
static_assert(offsetof(rrr::CompletionTrackerConfig, enabled) == 16);

static_assert(std::is_standard_layout_v<rrr::CompletedEntry>);
static_assert(std::is_trivially_copyable_v<rrr::CompletedEntry>);
static_assert(rrr::CompletedEntry::is_send);
static_assert(rrr::CompletedEntry::is_sync);
static_assert(sizeof(rrr::CompletedEntry) == 16);
static_assert(alignof(rrr::CompletedEntry) == 8);
static_assert(offsetof(rrr::CompletedEntry, xid) == 0);
static_assert(offsetof(rrr::CompletedEntry, timestamp_ms) == 8);

static_assert(std::is_standard_layout_v<rrr::CompletionQueryResult>);
static_assert(std::is_trivially_copyable_v<rrr::CompletionQueryResult>);
static_assert(rrr::CompletionQueryResult::is_send);
static_assert(rrr::CompletionQueryResult::is_sync);
static_assert(sizeof(rrr::CompletionQueryResult) == 12);
static_assert(alignof(rrr::CompletionQueryResult) == 4);
static_assert(offsetof(rrr::CompletionQueryResult, status) == 0);
static_assert(offsetof(rrr::CompletionQueryResult, error_code) == 4);
static_assert(offsetof(rrr::CompletionQueryResult, has_cached_response) == 8);

static_assert(std::is_standard_layout_v<rrr::CompletionTracker>);
static_assert(rrr::CompletionTracker::is_send);
static_assert(rrr::CompletionTracker::is_sync);
static_assert(sizeof(rrr::CompletionTracker) == 256);
static_assert(alignof(rrr::CompletionTracker) == 8);
static_assert(offsetof(rrr::CompletionTracker, config_) == 0);
static_assert(offsetof(rrr::CompletionTracker, lru_list_) == 64);
static_assert(offsetof(rrr::CompletionTracker, completed_set_) == 136);
static_assert(offsetof(rrr::CompletionTracker, total_tracked_) == 224);
static_assert(offsetof(rrr::CompletionTracker, queries_) == 232);
static_assert(offsetof(rrr::CompletionTracker, query_hits_) == 240);
static_assert(offsetof(rrr::CompletionTracker, evictions_) == 248);
static_assert(std::is_same_v<
              decltype(&rrr::CompletionTracker::mark_completed),
              void (rrr::CompletionTracker::*)(std::int64_t, std::uint64_t)>);
static_assert(std::is_same_v<
              decltype(&rrr::CompletionTracker::is_completed),
              bool (rrr::CompletionTracker::*)(std::int64_t, std::uint64_t)>);
static_assert(std::is_same_v<
              decltype(&rrr::CompletionTracker::set_config),
              void (rrr::CompletionTracker::*)(rrr::CompletionTrackerConfig)>);
static_assert(std::is_same_v<
              decltype(&rrr::CompletionTracker::config),
              rrr::CompletionTrackerConfig (rrr::CompletionTracker::*)() const>);

static bool stat_is(
    const rrr::AvgStat& stat,
    std::int64_t count,
    std::int64_t sum,
    std::int64_t average,
    std::int64_t maximum,
    std::int64_t minimum) {
    return stat.n_stat_ == count && stat.sum_ == sum &&
           stat.avg_ == average && stat.max_ == maximum &&
           stat.min_ == minimum;
}

static bool metrics_are_reset(const rrr::ConnectionMetrics& metrics) {
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
        auto metrics = rrr::ConnectionMetrics::new_();
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
        auto config = rrr::CompletionTrackerConfig::defaults();
        config.ttl_ms = 0;
        config.max_entries = kUpdates + 1;
        auto tracker = rrr::CompletionTracker::with_config(config);
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
        size = rrr::SparseInt::dump32(value, encoded.data());
        decoded = rrr::SparseInt::load32(encoded.data());
    } else {
        size = rrr::SparseInt::dump64(value, encoded.data());
        decoded = rrr::SparseInt::load64(encoded.data());
    }
    if (size != rrr::SparseInt::val_size(static_cast<std::int64_t>(value)) ||
        rrr::SparseInt::buf_size(encoded[0]) != size || decoded != value) {
        return false;
    }
    if constexpr (sizeof(I) == 8) {
        if (size == 8) {
            return encoded[8] != sentinel && encoded[9] == sentinel;
        }
    }
    return encoded[size] == sentinel;
}

static rrr::QueuedRequest make_queued_request(
    std::int64_t xid,
    rrr::QueuedRequestCallback callback = {}) {
    auto request = rrr::QueuedRequest::new_();
    request.xid = xid;
    request.callback = std::move(callback);
    return request;
}

int main() {
    constexpr int kMin = (-2147483647 - 1);
    if (rrr::kInternalHeartbeatRpcId != kMin) {
        return 1;
    }
    if (rrr::kResponseHeaderExtFlag != 0x80000000u ||
        rrr::kResponseSizeMask != 0x7fffffffu) {
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
        if (rrr::response_has_extended_header(row.input) != row.has_extended) {
            return 3;
        }
        if (rrr::response_payload_size(row.input) != row.payload) {
            return 4;
        }
        if (rrr::encode_response_size(row.input, false) != row.plain) {
            return 5;
        }
        if (rrr::encode_response_size(row.input, true) != row.extended) {
            return 6;
        }
    }

    auto stat = rrr::AvgStat::new_();
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
        rrr::RpcErrorCategory category;
        int discriminant;
        std::string_view name;
    };
    constexpr CategoryRow categories[] = {
        {rrr::RpcErrorCategory::NONE, 0, "NONE"},
        {rrr::RpcErrorCategory::CONNECTION, 1, "CONNECTION"},
        {rrr::RpcErrorCategory::PROTOCOL, 2, "PROTOCOL"},
        {rrr::RpcErrorCategory::APPLICATION, 3, "APPLICATION"},
        {rrr::RpcErrorCategory::TIMEOUT, 4, "TIMEOUT"},
        {rrr::RpcErrorCategory::INTERNAL, 5, "INTERNAL"},
    };
    for (const auto& row : categories) {
        if (static_cast<int>(row.category) != row.discriminant ||
            rrr::rpc_error_category_to_string(row.category) != row.name) {
            return 20;
        }
    }
    constexpr int invalid_categories[] = {-1, 6, 999};
    for (const auto value : invalid_categories) {
        if (rrr::rpc_error_category_to_string(
                static_cast<rrr::RpcErrorCategory>(value)) != "UNKNOWN") {
            return 21;
        }
    }

    struct ErrorRow {
        rrr::RpcError error;
        int discriminant;
        std::string_view name;
        rrr::RpcErrorCategory category;
        bool retryable;
    };
    constexpr ErrorRow errors[] = {
        {rrr::RpcError::OK, 0, "OK", rrr::RpcErrorCategory::NONE, false},
        {rrr::RpcError::NOT_CONNECTED, 100, "NOT_CONNECTED", rrr::RpcErrorCategory::CONNECTION, false},
        {rrr::RpcError::CONNECTION_REFUSED, 101, "CONNECTION_REFUSED", rrr::RpcErrorCategory::CONNECTION, false},
        {rrr::RpcError::CONNECTION_RESET, 102, "CONNECTION_RESET", rrr::RpcErrorCategory::CONNECTION, true},
        {rrr::RpcError::NETWORK_UNREACHABLE, 103, "NETWORK_UNREACHABLE", rrr::RpcErrorCategory::CONNECTION, true},
        {rrr::RpcError::HOST_UNREACHABLE, 104, "HOST_UNREACHABLE", rrr::RpcErrorCategory::CONNECTION, true},
        {rrr::RpcError::CONNECTION_CLOSED, 105, "CONNECTION_CLOSED", rrr::RpcErrorCategory::CONNECTION, false},
        {rrr::RpcError::CIRCUIT_OPEN, 106, "CIRCUIT_OPEN", rrr::RpcErrorCategory::CONNECTION, false},
        {rrr::RpcError::INVALID_MESSAGE, 200, "INVALID_MESSAGE", rrr::RpcErrorCategory::PROTOCOL, false},
        {rrr::RpcError::UNKNOWN_RPC_ID, 201, "UNKNOWN_RPC_ID", rrr::RpcErrorCategory::PROTOCOL, false},
        {rrr::RpcError::MARSHALLING_ERROR, 202, "MARSHALLING_ERROR", rrr::RpcErrorCategory::PROTOCOL, false},
        {rrr::RpcError::VERSION_MISMATCH, 203, "VERSION_MISMATCH", rrr::RpcErrorCategory::PROTOCOL, false},
        {rrr::RpcError::CHECKSUM_ERROR, 204, "CHECKSUM_ERROR", rrr::RpcErrorCategory::PROTOCOL, false},
        {rrr::RpcError::RPC_FAILED, 300, "RPC_FAILED", rrr::RpcErrorCategory::APPLICATION, false},
        {rrr::RpcError::SERVICE_UNAVAILABLE, 301, "SERVICE_UNAVAILABLE", rrr::RpcErrorCategory::APPLICATION, true},
        {rrr::RpcError::PERMISSION_DENIED, 302, "PERMISSION_DENIED", rrr::RpcErrorCategory::APPLICATION, false},
        {rrr::RpcError::INVALID_ARGUMENT, 303, "INVALID_ARGUMENT", rrr::RpcErrorCategory::APPLICATION, false},
        {rrr::RpcError::NOT_FOUND, 304, "NOT_FOUND", rrr::RpcErrorCategory::APPLICATION, false},
        {rrr::RpcError::ALREADY_EXISTS, 305, "ALREADY_EXISTS", rrr::RpcErrorCategory::APPLICATION, false},
        {rrr::RpcError::CONNECT_TIMEOUT, 400, "CONNECT_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, true},
        {rrr::RpcError::REQUEST_TIMEOUT, 401, "REQUEST_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, true},
        {rrr::RpcError::RESPONSE_TIMEOUT, 402, "RESPONSE_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, true},
        {rrr::RpcError::IDLE_TIMEOUT, 403, "IDLE_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, false},
        {rrr::RpcError::HEARTBEAT_TIMEOUT, 404, "HEARTBEAT_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, false},
        {rrr::RpcError::UNKNOWN_ERROR, 500, "UNKNOWN_ERROR", rrr::RpcErrorCategory::INTERNAL, false},
        {rrr::RpcError::OUT_OF_MEMORY, 501, "OUT_OF_MEMORY", rrr::RpcErrorCategory::INTERNAL, false},
        {rrr::RpcError::INVALID_STATE, 502, "INVALID_STATE", rrr::RpcErrorCategory::INTERNAL, false},
        {rrr::RpcError::INTERNAL_ERROR, 503, "INTERNAL_ERROR", rrr::RpcErrorCategory::INTERNAL, false},
    };
    for (const auto& row : errors) {
        if (static_cast<int>(row.error) != row.discriminant ||
            rrr::rpc_error_to_string(row.error) != row.name ||
            rrr::get_error_category(row.error) != row.category ||
            rrr::is_connection_error(row.error) !=
                (row.category == rrr::RpcErrorCategory::CONNECTION) ||
            rrr::is_timeout_error(row.error) !=
                (row.category == rrr::RpcErrorCategory::TIMEOUT) ||
            rrr::is_retryable_error(row.error) != row.retryable) {
            return 22;
        }
    }

    struct ErrorBoundaryRow {
        int code;
        std::string_view name;
        rrr::RpcErrorCategory category;
        bool connection;
        bool timeout;
        bool retryable;
    };
    constexpr ErrorBoundaryRow boundaries[] = {
        {99, "UNKNOWN", rrr::RpcErrorCategory::INTERNAL, false, false, false},
        {100, "NOT_CONNECTED", rrr::RpcErrorCategory::CONNECTION, true, false, false},
        {199, "UNKNOWN", rrr::RpcErrorCategory::CONNECTION, true, false, false},
        {200, "INVALID_MESSAGE", rrr::RpcErrorCategory::PROTOCOL, false, false, false},
        {399, "UNKNOWN", rrr::RpcErrorCategory::APPLICATION, false, false, false},
        {400, "CONNECT_TIMEOUT", rrr::RpcErrorCategory::TIMEOUT, false, true, true},
        {499, "UNKNOWN", rrr::RpcErrorCategory::TIMEOUT, false, true, false},
        {500, "UNKNOWN_ERROR", rrr::RpcErrorCategory::INTERNAL, false, false, false},
        {999, "UNKNOWN", rrr::RpcErrorCategory::INTERNAL, false, false, false},
    };
    for (const auto& row : boundaries) {
        const auto error = static_cast<rrr::RpcError>(row.code);
        if (rrr::rpc_error_to_string(error) != row.name ||
            rrr::get_error_category(error) != row.category ||
            rrr::is_connection_error(error) != row.connection ||
            rrr::is_timeout_error(error) != row.timeout ||
            rrr::is_retryable_error(error) != row.retryable) {
            return 23;
        }
    }

    auto metrics = rrr::ConnectionMetrics::new_();
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
        rrr::detail::CallbackWrapper<CallbackMoveObservedCallable>;
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
        rrr::CompletionTrackerConfig::defaults();
    const auto completion_small = rrr::CompletionTrackerConfig::small();
    const auto completion_large = rrr::CompletionTrackerConfig::large();
    const auto completion_disabled =
        rrr::CompletionTrackerConfig::disabled();
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
        rrr::CompletionQueryResult::not_found();
    const auto completion_ok =
        rrr::CompletionQueryResult::completed(0, true);
    const auto completion_error =
        rrr::CompletionQueryResult::completed(-7, false);
    const auto completion_expired =
        rrr::CompletionQueryResult::expired();
    if (completion_not_found.status != rrr::CompletionStatus::NOT_FOUND ||
        completion_not_found.error_code != 0 ||
        completion_not_found.has_cached_response ||
        completion_not_found.is_completed() ||
        completion_ok.status != rrr::CompletionStatus::COMPLETED ||
        completion_ok.error_code != 0 ||
        !completion_ok.has_cached_response || !completion_ok.is_completed() ||
        completion_error.status !=
            rrr::CompletionStatus::COMPLETED_WITH_ERROR ||
        completion_error.error_code != -7 ||
        completion_error.has_cached_response ||
        !completion_error.is_completed() ||
        completion_expired.status != rrr::CompletionStatus::EXPIRED ||
        completion_expired.is_completed() ||
        rrr::completion_status_to_string(rrr::CompletionStatus::NOT_FOUND) !=
            "NOT_FOUND" ||
        rrr::completion_status_to_string(rrr::CompletionStatus::COMPLETED) !=
            "COMPLETED" ||
        rrr::completion_status_to_string(
            rrr::CompletionStatus::COMPLETED_WITH_ERROR) !=
            "COMPLETED_WITH_ERROR" ||
        rrr::completion_status_to_string(rrr::CompletionStatus::EXPIRED) !=
            "EXPIRED" ||
        rrr::completion_status_to_string(
            static_cast<rrr::CompletionStatus>(99)) != "UNKNOWN") {
        return 61;
    }

    const auto wrapping_entry = rrr::CompletedEntry::new_(
        77, std::numeric_limits<std::uint64_t>::max() - 5);
    if (wrapping_entry.xid != 77 ||
        wrapping_entry.timestamp_ms !=
            std::numeric_limits<std::uint64_t>::max() - 5 ||
        wrapping_entry.is_expired(1000, 0) ||
        wrapping_entry.is_expired(4, 10) ||
        !wrapping_entry.is_expired(5, 10)) {
        return 62;
    }

    auto disabled_tracker = rrr::CompletionTracker::with_config(completion_disabled);
    disabled_tracker.mark_completed(1, 0);
    if (disabled_tracker.enabled() || disabled_tracker.size() != 0 ||
        disabled_tracker.total_tracked() != 0 ||
        disabled_tracker.is_completed(1, 0) ||
        disabled_tracker.queries() != 1 ||
        disabled_tracker.query_hits() != 0) {
        return 63;
    }

    auto lifecycle_config = rrr::CompletionTrackerConfig::defaults();
    lifecycle_config.ttl_ms = 10;
    lifecycle_config.max_entries = 2;
    auto lifecycle_tracker = rrr::CompletionTracker::with_config(lifecycle_config);
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

    auto mutation_config = rrr::CompletionTrackerConfig::defaults();
    mutation_config.ttl_ms = 0;
    auto mutation_tracker = rrr::CompletionTracker::with_config(mutation_config);
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

    auto overflow_config = rrr::CompletionTrackerConfig::defaults();
    overflow_config.ttl_ms = 0;
    overflow_config.max_entries = 1;
    auto overflow_tracker = rrr::CompletionTracker::with_config(overflow_config);
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

    if (rrr::randgen_rand_max() !=
            static_cast<double>(std::numeric_limits<std::int32_t>::max()) ||
        rrr::randgen_nu_constant_now() != 0) {
        return 71;
    }

    rand_string_evaluations = 0;
    const auto padded_binary =
        rrr::randgen_zero_pad(make_rand_binary_string(), 5);
    const auto truncated_binary =
        rrr::randgen_zero_pad(make_rand_binary_string(), 2);
    if (rand_string_evaluations != 2 || padded_binary.size() != 5 ||
        padded_binary[0] != '0' || padded_binary[1] != '0' ||
        static_cast<unsigned char>(padded_binary[2]) != 0x00 ||
        static_cast<unsigned char>(padded_binary[3]) != 0x80 ||
        static_cast<unsigned char>(padded_binary[4]) != 0xff ||
        truncated_binary.size() != 2 ||
        static_cast<unsigned char>(truncated_binary[0]) != 0x80 ||
        static_cast<unsigned char>(truncated_binary[1]) != 0xff ||
        rrr::randgen_zero_pad("7", 3) != "007" ||
        rrr::randgen_zero_pad("1234", 3) != "234" ||
        rrr::randgen_zero_pad("1234", 0) != "") {
        return 72;
    }

    if (rrr::RandomGenerator::int2str_n(0, 1) != "0" ||
        rrr::RandomGenerator::int2str_n(42, 5) != "00042" ||
        rrr::RandomGenerator::int2str_n(-7, 4) != "00-7" ||
        rrr::RandomGenerator::int2str_n(12345, 3) != "345" ||
        rrr::RandomGenerator::int2str_n(-12345, 4) != "2345" ||
        rrr::RandomGenerator::int2str_n(
            std::numeric_limits<std::int32_t>::max(), 10) != "2147483647" ||
        rrr::RandomGenerator::int2str_n(
            std::numeric_limits<std::int32_t>::min(), 11) != "-2147483648" ||
        rrr::RandomGenerator::int2str_n(
            std::numeric_limits<std::int32_t>::min(), 10) != "2147483648") {
        return 73;
    }

    install_rand_raw(17);
    if (rrr::randgen_rand_raw() != 17 || rand_raw_draws != 1) {
        return 74;
    }
    install_rand_raw(5);
    if (rrr::RandomGenerator::rand(-10, -5) != -5 || rand_raw_draws != 1) {
        return 75;
    }
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    if (rrr::RandomGenerator::rand(
            0, std::numeric_limits<std::int32_t>::max()) !=
            std::numeric_limits<std::int32_t>::max() ||
        rand_raw_draws != 1) {
        return 76;
    }

    install_rand_raw(123);
    if (rrr::RandomGenerator::rand_double(4.5, 4.5) != 4.5 ||
        rand_raw_draws != 0) {
        return 77;
    }
    const auto scaled_rand = rrr::RandomGenerator::rand_double(-1.0, 1.0);
    const auto expected_scaled_rand =
        (123.0 /
         (static_cast<double>(std::numeric_limits<std::int32_t>::max()) / 2.0)) -
        1.0;
    if (scaled_rand != expected_scaled_rand || rand_raw_draws != 1) {
        return 78;
    }

    install_rand_raw(0);
    if (rrr::RandomGenerator::percentage_true(0) || rand_raw_draws != 1) {
        return 79;
    }
    install_rand_raw(0);
    if (!rrr::RandomGenerator::percentage_true(1) || rand_raw_draws != 1) {
        return 80;
    }
    install_rand_raw(5);
    if (rrr::RandomGenerator::nu_rand(1022, 0, 999) != 5 ||
        rand_raw_draws != 2) {
        return 81;
    }

    install_rand_raw(99);
    const std::vector<double> empty_weights;
    if (rrr::RandomGenerator::weighted_select(empty_weights) !=
            std::numeric_limits<std::uint32_t>::max() ||
        rand_raw_draws != 0) {
        return 82;
    }
    install_rand_raw(99);
    const std::vector<double> zero_weights{0.0, 0.0};
    if (rrr::RandomGenerator::weighted_select(zero_weights) != 0 ||
        rand_raw_draws != 0) {
        return 83;
    }

    const std::vector<double> weights{1.0, 2.0, 3.0};
    install_rand_raw(0);
    if (rrr::RandomGenerator::weighted_select(weights) != 0 ||
        rand_raw_draws != 1) {
        return 84;
    }
    install_rand_raw(std::numeric_limits<std::int32_t>::max() / 2);
    if (rrr::RandomGenerator::weighted_select(weights) != 1 ||
        rand_raw_draws != 1) {
        return 85;
    }
    rand_weight_evaluations = 0;
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    if (rrr::RandomGenerator::weighted_select(make_rand_weights()) != 2 ||
        rand_raw_draws != 1 || rand_weight_evaluations != 1) {
        return 86;
    }

    const auto destroys_before = rand_destroy_calls;
    rrr::randgen_destroy();
    rrr::RandomGenerator::destroy();
    if (rand_destroy_calls != destroys_before + 2) {
        return 87;
    }

    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    if (rrr::RandomGenerator::rand(7, 7) != 7 || rand_raw_draws != 1) {
        return 88;
    }
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    if (rrr::RandomGenerator::rand(
            std::numeric_limits<std::int32_t>::min(), -1) != -1 ||
        rand_raw_draws != 1) {
        return 89;
    }

    bool rand_failed = false;
    install_rand_raw(std::numeric_limits<std::int32_t>::max());
    try {
        static_cast<void>(rrr::RandomGenerator::rand(
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
        static_cast<void>(rrr::RandomGenerator::rand(9, 8));
    } catch (...) {
        rand_failed = true;
    }
    if (!rand_failed || rand_raw_draws != 0) {
        return 91;
    }

    rand_failed = false;
    install_rand_raw(123);
    try {
        static_cast<void>(rrr::RandomGenerator::rand_double(2.0, 1.0));
    } catch (...) {
        rand_failed = true;
    }
    if (!rand_failed || rand_raw_draws != 0) {
        return 92;
    }

    rand_failed = false;
    install_rand_raw(123);
    try {
        static_cast<void>(rrr::RandomGenerator::rand_double(
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
        static_cast<void>(rrr::RandomGenerator::nu_rand(
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
    if (rrr::RandomGenerator::weighted_select(positive_boundary_weights) != 0 ||
        rand_raw_draws != 1) {
        return 95;
    }

    if (static_cast<std::int32_t>(rrr::TimeoutType::NONE) != 0 ||
        static_cast<std::int32_t>(rrr::TimeoutType::CONNECT_TIMEOUT) != 1 ||
        static_cast<std::int32_t>(rrr::TimeoutType::REQUEST_TIMEOUT) != 2 ||
        static_cast<std::int32_t>(rrr::TimeoutType::RESPONSE_TIMEOUT) != 3 ||
        static_cast<std::int32_t>(rrr::TimeoutType::TOTAL_TIMEOUT) != 4 ||
        rrr::TimeoutType_NONE() != rrr::TimeoutType::NONE ||
        rrr::TimeoutType_CONNECT_TIMEOUT() !=
            rrr::TimeoutType::CONNECT_TIMEOUT ||
        rrr::TimeoutType_REQUEST_TIMEOUT() !=
            rrr::TimeoutType::REQUEST_TIMEOUT ||
        rrr::TimeoutType_RESPONSE_TIMEOUT() !=
            rrr::TimeoutType::RESPONSE_TIMEOUT ||
        rrr::TimeoutType_TOTAL_TIMEOUT() != rrr::TimeoutType::TOTAL_TIMEOUT ||
        rrr::timeout_type_to_string(rrr::TimeoutType::NONE) != "NONE" ||
        rrr::timeout_type_to_string(rrr::TimeoutType::CONNECT_TIMEOUT) !=
            "CONNECT_TIMEOUT" ||
        rrr::timeout_type_to_string(rrr::TimeoutType::REQUEST_TIMEOUT) !=
            "REQUEST_TIMEOUT" ||
        rrr::timeout_type_to_string(rrr::TimeoutType::RESPONSE_TIMEOUT) !=
            "RESPONSE_TIMEOUT" ||
        rrr::timeout_type_to_string(rrr::TimeoutType::TOTAL_TIMEOUT) !=
            "TOTAL_TIMEOUT" ||
        rrr::timeout_type_to_string(static_cast<rrr::TimeoutType>(99)) !=
            "UNKNOWN") {
        return 96;
    }

    const auto request_defaults = rrr::RequestOptions::defaults();
    const auto request_new = rrr::RequestOptions::new_();
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

    const auto request_retry = rrr::RequestOptions::with_retry(3, 2000);
    const auto request_idempotent =
        rrr::RequestOptions::idempotent_retry(10);
    const auto request_no_timeout = rrr::RequestOptions::no_timeout();
    const auto request_fast = rrr::RequestOptions::fast();
    const auto request_patient = rrr::RequestOptions::patient();
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

    const auto reconnect_new = rrr::ReconnectPolicy::new_();
    const auto reconnect_conservative = rrr::ReconnectPolicy::conservative();
    const auto reconnect_aggressive = rrr::ReconnectPolicy::aggressive();
    const auto reconnect_none = rrr::ReconnectPolicy::no_retry();
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
        rrr::ReconnectCalculator::new_(reconnect_limited);
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
        rrr::ReconnectCalculator::new_(reconnect_unlimited);
    auto no_retry_calculator = rrr::ReconnectCalculator::new_(reconnect_none);
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
        rrr::ReconnectCalculator::new_(reconnect_jitter);
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

    const auto circuit_new = rrr::CircuitBreakerConfig::new_();
    const auto circuit_defaults = rrr::CircuitBreakerConfig::defaults();
    const auto circuit_sensitive = rrr::CircuitBreakerConfig::sensitive();
    const auto circuit_relaxed = rrr::CircuitBreakerConfig::relaxed();
    const auto circuit_disabled = rrr::CircuitBreakerConfig::disabled();
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
        rrr::circuit_state_to_string(rrr::CircuitState::CLOSED) != "CLOSED" ||
        rrr::circuit_state_to_string(rrr::CircuitState::OPEN) != "OPEN" ||
        rrr::circuit_state_to_string(rrr::CircuitState::HALF_OPEN) !=
            "HALF_OPEN") {
        return 117;
    }

    monotonic_now_us = 1'000'000;
    auto circuit_config = circuit_defaults;
    circuit_config.failure_threshold = 2;
    circuit_config.success_threshold = 2;
    circuit_config.timeout_ms = 10;
    auto circuit = rrr::CircuitBreaker::new_(circuit_config);
    if (!circuit.is_closed() || circuit.is_open() ||
        !circuit.allow_request() || circuit.failure_count() != 0 ||
        rrr::current_time_us() != monotonic_now_us) {
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

    rrr::StateChangeCallback empty_state_callback{};
    if (empty_state_callback || !empty_state_callback.is_empty()) {
        return 125;
    }
    auto state_machine = rrr::ConnectionStateMachine::new_();
    if (!state_machine.on_state_change.is_empty() ||
        state_machine.state() != rrr::ConnectionState::NEW ||
        state_machine.transition_to(rrr::ConnectionState::CONNECTED) ||
        !state_machine.transition_to(rrr::ConnectionState::CONNECTING)) {
        return 126;
    }
    int state_callback_calls = 0;
    rrr::ConnectionState observed_from = rrr::ConnectionState::NEW;
    rrr::ConnectionState observed_to = rrr::ConnectionState::NEW;
    state_machine.set_on_state_change(
        [&](rrr::ConnectionState from, rrr::ConnectionState to) {
            ++state_callback_calls;
            observed_from = from;
            observed_to = to;
        });
    if (state_machine.on_state_change.is_empty() ||
        !state_machine.transition_to(rrr::ConnectionState::CONNECTED) ||
        state_callback_calls != 1 ||
        observed_from != rrr::ConnectionState::CONNECTING ||
        observed_to != rrr::ConnectionState::CONNECTED) {
        return 127;
    }
    state_machine.force_state(rrr::ConnectionState::FAILED);
    if (state_callback_calls != 2 ||
        observed_from != rrr::ConnectionState::CONNECTED ||
        observed_to != rrr::ConnectionState::FAILED ||
        !state_machine.is_failed() || !state_machine.is_terminal()) {
        return 128;
    }

    rrr::HeartbeatTimeoutCallback empty_heartbeat_callback{};
    if (empty_heartbeat_callback || !empty_heartbeat_callback.is_empty()) {
        return 129;
    }
    int moved_callback_calls = 0;
    rrr::HeartbeatTimeoutCallback moved_from =
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

    const auto heartbeat_defaults = rrr::HeartbeatConfig::defaults();
    const auto heartbeat_aggressive = rrr::HeartbeatConfig::aggressive();
    const auto heartbeat_relaxed = rrr::HeartbeatConfig::relaxed();
    const auto heartbeat_disabled = rrr::HeartbeatConfig::disabled();
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
    auto empty_timeout = rrr::HeartbeatManager::new_(empty_timeout_config);
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
    auto heartbeat = rrr::HeartbeatManager::new_(heartbeat_config);
    int heartbeat_callback_calls = 0;
    heartbeat.set_on_timeout(
        MutableHeartbeatCallable{&heartbeat_callback_calls});
    monotonic_now_us = 1'000'000;
    if (rrr::heartbeat_time_us() != monotonic_now_us ||
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

    auto wrapping_heartbeat = rrr::HeartbeatManager::new_(heartbeat_config);
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

    using enum rrr::LoadBalancingStrategy;
    if (rrr::load_balancing_strategy_to_string(RANDOM) != "RANDOM" ||
        rrr::load_balancing_strategy_to_string(ROUND_ROBIN) !=
            "ROUND_ROBIN" ||
        rrr::load_balancing_strategy_to_string(LEAST_CONNECTIONS) !=
            "LEAST_CONNECTIONS" ||
        rrr::load_balancing_strategy_to_string(LEAST_LATENCY) !=
            "LEAST_LATENCY" ||
        rrr::load_balancing_strategy_to_string(
            static_cast<rrr::LoadBalancingStrategy>(255)) != "UNKNOWN") {
        return 177;
    }

    auto load_balancer_state = rrr::LoadBalancerState::new_();
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
    if (rrr::LoadBalancer::select(
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
    if (rrr::lb_pool_size(load_balancer_clients) != 3 ||
        rrr::LoadBalancer::select(
            RANDOM, load_balancer_clients, load_balancer_state, 8) != 2 ||
        rrr::LoadBalancer::select(
            static_cast<rrr::LoadBalancingStrategy>(255),
            load_balancer_clients,
            load_balancer_state,
            8) != 2 ||
        rrr::LoadBalancer::select(
            LEAST_CONNECTIONS,
            load_balancer_clients,
            load_balancer_state,
            0) != 1 ||
        rrr::LoadBalancer::select(
            LEAST_LATENCY,
            load_balancer_clients,
            load_balancer_state,
            0) != 2) {
        return 182;
    }
    load_balancer_state.reset();
    if (rrr::LoadBalancer::select(
            ROUND_ROBIN, load_balancer_clients, load_balancer_state, 0) != 0 ||
        rrr::LoadBalancer::select(
            ROUND_ROBIN, load_balancer_clients, load_balancer_state, 0) != 1 ||
        rrr::LoadBalancer::select(
            ROUND_ROBIN, load_balancer_clients, load_balancer_state, 0) != 2) {
        return 183;
    }

    {
        auto empty = rrr::AddrInfo::new_();
        if (empty.get() != nullptr || empty.valid() || empty.owned_.get() ||
            empty._rusty_forgotten) {
            return 184;
        }
    }
    const auto free_before = freeaddrinfo_calls;
    {
        auto* first = new addrinfo{};
        auto* second = new addrinfo{};
        auto source = rrr::AddrInfo::adopt(first);
        if (source.get() != first || !source.valid() || !source.owned_.get()) {
            return 185;
        }
        rrr::AddrInfo moved(std::move(source));
        if (moved.get() != first || !moved.owned_.get() ||
            source.get() != first || !source._rusty_forgotten) {
            return 186;
        }
        auto target = rrr::AddrInfo::adopt(second);
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
    if (rrr::find_open_port() != 4321 ||
        !utils_logged("I [<unknown>:0] ", " | Found open port: 4321\\n")) {
        return 190;
    }
    selected_open_port = 0;
    reset_utils_log();
    if (rrr::find_open_port() != -1 ||
        !utils_logged("E [<unknown>:0] ", " | Failed to find open port.\\n")) {
        return 191;
    }
    selected_open_port = -1;
    reset_utils_log();
    if (rrr::find_open_port() != -1 ||
        !utils_logged("E [<unknown>:0] ", " | Failed to find open port.\\n")) {
        return 192;
    }

    hostname_mode = 1;
    reset_utils_log();
    if (rrr::get_host_name() != "goal0-host" ||
        hostname_buffer_length != 255 || !utils_log_text().empty()) {
        return 193;
    }
    hostname_mode = -1;
    reset_utils_log();
    if (!rrr::get_host_name().empty() ||
        !utils_logged("E [<unknown>:0] ", " | Failed to get hostname.\\n")) {
        return 194;
    }


    if (rrr::frame_decode_status_to_string(
            rrr::FrameDecodeStatus::NeedMoreBytes) != "NeedMoreBytes" ||
        rrr::frame_decode_status_to_string(
            rrr::FrameDecodeStatus::Complete) != "Complete" ||
        rrr::frame_decode_status_to_string(
            rrr::FrameDecodeStatus::Malformed) != "Malformed") {
        return 195;
    }
    bool invalid_frame_status_threw = false;
    try {
        (void)rrr::frame_decode_status_to_string(
            static_cast<rrr::FrameDecodeStatus>(99));
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
    if (rrr::frame_codec_write_header(
            std::span<std::uint8_t>(frame_header_bytes.data(), 3), 1, false) ||
        frame_header_bytes != original_frame_header_bytes ||
        rrr::frame_codec_write_header(frame_header_bytes, -1, true) ||
        frame_header_bytes != original_frame_header_bytes) {
        return 197;
    }
    if (!rrr::frame_codec_write_header(frame_header_bytes, 0, true) ||
        std::memcmp(frame_header_bytes.data(),
                    frame_native_bytes(INT32_MIN).data(), 4) != 0 ||
        frame_header_bytes[4] != 0xa5 ||
        !rrr::frame_codec_write_header(
            frame_header_bytes, INT32_MAX, true) ||
        std::memcmp(frame_header_bytes.data(),
                    frame_native_bytes(-1).data(), 4) != 0) {
        return 198;
    }

    rrr::FrameHeader decoded_frame_header{17, true};
    if (rrr::frame_codec_peek_header(
            std::span<const std::uint8_t>(frame_header_bytes.data(), 3),
            decoded_frame_header) != rrr::FrameDecodeStatus::NeedMoreBytes ||
        decoded_frame_header.payload_size != 17 ||
        !decoded_frame_header.extended_header_flag) {
        return 199;
    }
    for (const auto [encoded, payload, extended] :
         std::array{
             std::tuple{0, 0, false},
             std::tuple{INT32_MAX, INT32_MAX, false},
             std::tuple{INT32_MIN, 0, true},
             std::tuple{-1, INT32_MAX, true},
         }) {
        const auto bytes = frame_native_bytes(encoded);
        decoded_frame_header = rrr::FrameHeader{-7, !extended};
        if (rrr::frame_codec_peek_header(bytes, decoded_frame_header) !=
                rrr::FrameDecodeStatus::Complete ||
            decoded_frame_header.payload_size != payload ||
            decoded_frame_header.extended_header_flag != extended) {
            return 200;
        }
    }
    if (rrr::FrameHeader{INT32_MAX, false}.total_frame_size() !=
        INT32_MIN + 3) {
        return 201;
    }

    std::vector<std::uint8_t> encoded_frame{9, 8};
    const auto untouched_frame = encoded_frame;
    if (rrr::frame_codec_encode_into(
            encoded_frame, nullptr, -1, false) ||
        encoded_frame != untouched_frame ||
        rrr::frame_codec_encode_into(
            encoded_frame, nullptr, 1, false) ||
        encoded_frame != untouched_frame ||
        !rrr::frame_codec_encode_into(
            encoded_frame, nullptr, 0, false) ||
        encoded_frame.size() != 6) {
        return 202;
    }
    constexpr std::array<std::uint8_t, 3> first_frame_payload{'a', 'b', 'c'};
    std::vector<std::uint8_t> first_frame;
    if (!rrr::frame_codec_encode_into(
            first_frame,
            first_frame_payload.data(),
            static_cast<std::int32_t>(first_frame_payload.size()),
            false)) {
        return 203;
    }

    auto frame_reader = rrr::FrameStreamReader::new_();
    frame_reader.cursor_.set_position(99);
    rrr::FrameView frame_view{
        rrr::FrameHeader{91, true},
        reinterpret_cast<const std::uint8_t*>(1),
        77,
    };
    if (frame_reader.next_frame(frame_view) !=
            rrr::FrameDecodeStatus::NeedMoreBytes ||
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
             status != rrr::FrameDecodeStatus::NeedMoreBytes) ||
            (index + 1 == first_frame.size() &&
             status != rrr::FrameDecodeStatus::Complete)) {
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
    if (!rrr::frame_codec_encode_into(
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
        rrr::FrameDecodeStatus::Complete) {
        return 209;
    }
    frame_reader.consume_frame();
    if (frame_reader.buffered_bytes() != second_frame.size() ||
        frame_reader.next_frame(frame_view) !=
            rrr::FrameDecodeStatus::Complete ||
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
    if (!rrr::frame_codec_encode_into(
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
        rrr::FrameDecodeStatus::Complete) {
        return 212;
    }
    frame_reader.consume_frame();
    if (frame_reader.cursor_.position() != 0 ||
        frame_reader.cursor_.get_ref().size() != second_frame.size() ||
        frame_reader.next_frame(frame_view) !=
            rrr::FrameDecodeStatus::Complete ||
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
            rrr::SparseInt::dump64(value, encoded64.data());
        sparse_wire_digest = hash_sparse_byte(sparse_wire_digest, 64);
        sparse_wire_digest = hash_sparse_byte(
            sparse_wire_digest, static_cast<std::uint8_t>(reported64));
        const auto written64 = reported64 == 8 ? 9 : reported64;
        for (std::size_t byte = 0; byte < written64; ++byte) {
            sparse_wire_digest =
                hash_sparse_byte(sparse_wire_digest, encoded64[byte]);
        }
        std::array<std::uint8_t, 5> encoded32{};
        const auto reported32 = rrr::SparseInt::dump32(
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
        const auto reported = rrr::SparseInt::dump64(value, encoded.data());
        std::array<std::uint8_t, 9> persisted{};
        std::copy_n(encoded.begin(), reported, persisted.begin());
        if (reported != 8 || encoded[0] != 0xfe ||
            rrr::SparseInt::load64(persisted.data()) != truncated) {
            return 145;
        }
    }

    auto base_v32 = rrr::v32::new_(-8192);
    base_v32.set(8192);
    auto base_v64 = rrr::v64::new_(36028797018963968LL);
    if (base_v32.get() != 8192 || base_v32.val_size() != 3 ||
        base_v64.get() != 36028797018963968LL || base_v64.val_size() != 9) {
        return 146;
    }
    auto base_counter = rrr::Counter::new_(7);
    if (base_counter.peek_next() != 7 || base_counter.next(5) != 7 ||
        base_counter.peek_next() != 12) {
        return 147;
    }
    base_counter.reset(std::numeric_limits<std::int64_t>::max());
    if (base_counter.next(1) != std::numeric_limits<std::int64_t>::max() ||
        base_counter.peek_next() != std::numeric_limits<std::int64_t>::min()) {
        return 148;
    }
    auto concurrent_counter = rrr::Counter::new_(0);
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
    rrr::AtomicI64 exported_atomic = rrr::AtomicI64::new_(11);
    if (exported_atomic.load(rrr::Ordering::Relaxed) != 11 ||
        rrr::RRR_USEC_PER_SEC != 1000000) {
        return 149;
    }

    monotonic_now_us = 10;
    realtime_now_us = 20;
    gettimeofday_now_us = 1000000;
    slept_us = 0;
    rrr::abort_if_false(true);
    if (rrr::time_now_us(true) != 10 || rrr::Time::now(false) != 20) {
        return 150;
    }
    rrr::Time::sleep(37);
    auto base_timer = rrr::Timer::new_();
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
    if (rrr::kRequestQueueRejectedError != EAGAIN ||
        rrr::kRequestQueueExpiredError != ETIMEDOUT ||
        rrr::overflow_strategy_to_string(rrr::OverflowStrategy::DROP_OLDEST) !=
            "DROP_OLDEST" ||
        rrr::overflow_strategy_to_string(rrr::OverflowStrategy::DROP_NEWEST) !=
            "DROP_NEWEST" ||
        rrr::overflow_strategy_to_string(rrr::OverflowStrategy::FAIL_FAST) !=
            "FAIL_FAST" ||
        rrr::overflow_strategy_to_string(
            static_cast<rrr::OverflowStrategy>(99)) != "UNKNOWN") {
        return 157;
    }

    const auto queue_defaults = rrr::RequestQueueConfig::defaults();
    if (queue_defaults.max_size != 1000 ||
        queue_defaults.default_ttl_ms != 30000 ||
        queue_defaults.overflow_strategy != rrr::OverflowStrategy::DROP_OLDEST ||
        !queue_defaults.enabled || rrr::RequestQueueConfig::small().max_size != 10 ||
        rrr::RequestQueueConfig::large().max_size != 10000 ||
        rrr::RequestQueueConfig::disabled().enabled) {
        return 158;
    }

    bool direct_callback_called = false;
    rrr::rq_invoke_callback_safely(
        rrr::QueuedRequestCallback([&](std::int32_t error) {
            direct_callback_called = error == 314;
        }),
        314);
    if (!direct_callback_called) {
        return 156;
    }

    monotonic_now_us = 1'000'000;
    auto timed_request = rrr::QueuedRequest::new_();
    if (rrr::queued_request_time_us() != monotonic_now_us ||
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
    auto fifo = rrr::RequestQueue::with_config(fifo_config);
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
    fifo.update_config(rrr::RequestQueueConfig::small());
    if (fifo.config().max_size != 10 || !fifo.enabled() || fifo.max_size() != 10) {
        return 166;
    }

    for (auto strategy : {rrr::OverflowStrategy::DROP_NEWEST,
                          rrr::OverflowStrategy::FAIL_FAST}) {
        auto config = queue_defaults;
        config.max_size = 1;
        config.overflow_strategy = strategy;
        auto queue = rrr::RequestQueue::with_config(config);
        if (!queue.enqueue(make_queued_request(3))) {
            return 167;
        }
        bool called = false;
        auto rejected = make_queued_request(
            4,
            rrr::QueuedRequestCallback([&](std::int32_t error) {
                if (error != rrr::kRequestQueueRejectedError ||
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
    auto oldest_queue = rrr::RequestQueue::with_config(oldest_config);
    bool oldest_called = false;
    auto oldest = make_queued_request(
        5,
        rrr::QueuedRequestCallback([&](std::int32_t error) {
            if (error != rrr::kRequestQueueRejectedError ||
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

    auto disabled_queue = rrr::RequestQueue::with_config(rrr::RequestQueueConfig::disabled());
    bool disabled_called = false;
    auto disabled_request = make_queued_request(
        7,
        rrr::QueuedRequestCallback([&](std::int32_t error) {
            if (error != rrr::kRequestQueueRejectedError ||
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
    auto expiring = rrr::RequestQueue::new_();
    std::vector<std::int64_t> expired_order;
    for (std::int64_t xid : {8, 9}) {
        auto request = make_queued_request(
            xid,
            rrr::QueuedRequestCallback([&, xid](std::int32_t error) {
                if (error != rrr::kRequestQueueExpiredError ||
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

    auto clearing = rrr::RequestQueue::new_();
    std::vector<std::int64_t> cleared_order;
    for (std::int64_t xid : {11, 12}) {
        auto request = make_queued_request(
            xid,
            rrr::QueuedRequestCallback([&, xid](std::int32_t error) {
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
    invalid_config.overflow_strategy = static_cast<rrr::OverflowStrategy>(99);
    auto invalid_queue = rrr::RequestQueue::with_config(invalid_config);
    if (!invalid_queue.enqueue(make_queued_request(13)) ||
        invalid_queue.size() != 1) {
        return 176;
    }

    // rrr.debugging: branch hints keep boolean identity, verify() only reaches
    // the failure tail on a false predicate, and the failure tail renders the
    // captured frames BEFORE it panics (the trace must survive a swallowed
    // panic). `frames - 1` is the historical loop bound, so a three-frame
    // capture renders two lines.
    if (!rrr::likely(true) || rrr::likely(false) ||
        !rrr::unlikely(true) || rrr::unlikely(false)) {
        return 177;
    }
    reset_debugging(3);
    rrr::verify(true);
    rrr::verify(reinterpret_cast<const void*>(1));
    rrr::verify(7);
    if (debugging_capture_calls != 0 || !debugging_rendered().empty()) {
        return 178;
    }

    reset_debugging(3);
    bool debugging_panicked = false;
    try {
        rrr::verify(false);
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
        rrr::verify_failed("goal0.cc", 42);
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
    rrr::print_stack_trace(debugging_stream);
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
            str(root / "src/rrr"),
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
            str(root / "src/rrr"),
            *module_path_flags,
            "-c",
            str(pcm),
            "-o",
            str(object_file),
        ],
        root,
    )
    return object_file


# Plain-C kernels compiled into librrr.a (see the "Goal-0 C demotion" block in
# src/rrr/CMakeLists.txt) plus rrr.epoll_wrapper's platform implementation
# unit. They are not crate outputs, so the generated lane has to build them
# itself to link the same closure production does.
# base/srpc_base.c, misc/srpc_rand.c, misc/srpc_timing.c and rpc/srpc_net.c are
# deliberately ABSENT: the importer defines its own observable stubs for the
# seams in them that the runtime contracts pin (the rand draws, the clocks,
# srpc_find_open_port, and the backtrace trio), and a C file is linked at
# object granularity -- pulling one of those objects for a symbol the importer
# does not stub would drag its stubbed neighbours in too. The handful of
# functions the modules still need from those four files are stubbed in the
# importer instead, beside the ones already there. The real kernels are covered
# separately by rrr_goal0_rand_kernel_smoke and by the production lane, which
# links librrr.a.
SUPPORT_C_KERNELS = (
    "src/rrr/misc/srpc_io.c",
    "src/rrr/rpc/srpc_connect.c",
    "src/rrr/rpc/srpc_server.c",
    "src/rrr/reactor/srpc_fiber.c",
)
SUPPORT_MODULE_IMPLEMENTATIONS = ("src/rrr/reactor/epoll_platform_linux.cc",)
# The fiber context-switch trampoline is real assembly, selected by host
# architecture exactly as CMake's `reactor/*.S` glob selects it.
SUPPORT_ASSEMBLY = {
    "x86_64": "src/rrr/reactor/fiber_context_x86_64.S",
    "aarch64": "src/rrr/reactor/fiber_context_aarch64.S",
    "arm64": "src/rrr/reactor/fiber_context_aarch64.S",
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
                str(root / "src/rrr"),
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
                str(root / "src/rrr"),
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
    # resolution order the production lane gets from librrr.a.
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
    with tempfile.TemporaryDirectory(prefix="rrr-crate-mode-compile-") as temporary:
        work = Path(temporary)
        generated_objects = [
            compile_module(
                clang,
                root,
                include,
                output,
                work,
                module.cpp_module,
                cxx_flags,
                prebuilt_module_dirs,
            )
            for module in modules
        ]
        # Compile the partial umbrella only after every child BMI exists. It is
        # a syntax/import-closure proof, not a production provider or link input.
        compile_module(
            clang,
            root,
            include,
            output,
            work,
            "rrr",
            cxx_flags,
            prebuilt_module_dirs,
        )

        # librrr.a carries two things the generated crate object set does not:
        # the plain-C kernels the canonical Rust calls through extern "C", and
        # rrr.epoll_wrapper's platform implementation unit. Compile the same
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
            # The forwarding rrr.logging fixture is gone: rrr.logging is a
            # canonical Rust module now, so both lanes link the REAL provider
            # (the generated object here, librrr.a's copy there) and the
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

            if module.cpp_module == "rrr.completion_tracker":
                require_completion_raw_symbols(
                    "crate-generated object",
                    completion_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "rrr.rand":
                require_rand_raw_symbols(
                    "crate-generated object",
                    rand_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "rrr.request_options":
                require_request_options_raw_symbols(
                    "crate-generated object",
                    request_options_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "rrr.reconnect_policy":
                require_reconnect_policy_raw_symbols(
                    "crate-generated object",
                    reconnect_policy_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "rrr.circuit_breaker":
                require_circuit_breaker_raw_symbols(
                    "crate-generated object",
                    circuit_breaker_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "rrr.request_queue":
                require_request_queue_raw_symbols(
                    "crate-generated object",
                    request_queue_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "rrr.basetypes":
                require_basetypes_raw_symbols(
                    "crate-generated object",
                    basetypes_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module == "rrr.utils":
                require_utils_raw_symbols(
                    "crate-generated object",
                    utils_raw_symbols(nm, root, generated_object),
                )
            elif module.cpp_module in {
                "rrr.connection_state",
                "rrr.heartbeat",
                "rrr.load_balancer",
                "rrr.frame_codec",
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
                if module.cpp_module == "rrr.completion_tracker":
                    require_completion_raw_symbols(
                        "production library",
                        completion_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "rrr.rand":
                    require_rand_raw_symbols(
                        "production library",
                        rand_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "rrr.request_options":
                    require_request_options_raw_symbols(
                        "production library",
                        request_options_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "rrr.reconnect_policy":
                    require_reconnect_policy_raw_symbols(
                        "production library",
                        reconnect_policy_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "rrr.circuit_breaker":
                    require_circuit_breaker_raw_symbols(
                        "production library",
                        circuit_breaker_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "rrr.request_queue":
                    require_request_queue_raw_symbols(
                        "production library",
                        request_queue_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "rrr.basetypes":
                    require_basetypes_raw_symbols(
                        "production library",
                        basetypes_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module == "rrr.utils":
                    require_utils_raw_symbols(
                        "production library",
                        utils_raw_symbols(nm, root, production),
                    )
                elif module.cpp_module in {
                    "rrr.connection_state",
                    "rrr.heartbeat",
                    "rrr.load_balancer",
                    "rrr.frame_codec",
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
    crate_manifest = (root / "src/rrr/Cargo.toml").resolve()
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
        flat_import_namespace = extraction.load_flat_import_namespace(
            root, root / EXTRACTION_MANIFEST
        )
        flat_import_arguments = (
            ["--flat-import-namespace", flat_import_namespace]
            if flat_import_namespace is not None
            else []
        )
        with tempfile.TemporaryDirectory(prefix="rrr-crate-mode-") as temporary:
            output = Path(temporary)
            run(
                [
                    str(transpiler),
                    "--crate",
                    str(crate_manifest),
                    "--output-dir",
                    str(output),
                    "--cxx-namespace",
                    "rrr",
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
        f"checked whole rrr crate ({len(modules) + 1} modules compiled, "
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
        help="production librrr archive to link/run and compare with direct generated objects",
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
