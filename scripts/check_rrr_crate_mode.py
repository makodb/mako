#!/usr/bin/env python3
"""Check rusty-cpp crate output against exact generated and production ABIs."""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import json
import os
from pathlib import Path
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
REQUIRED_RUSTY_CPP_COMMIT = "f6d9a0f62510c6335e172cebe3164d2570840284"
EXTRACTION_DRIVER = "scripts/extract_rrr_rust.py"
EXTRACTION_MANIFEST = "src/rrr/rust-modules.toml"
MODULE_PREAMBLE = "src/rrr/module-preambles.toml"
NM_LINE = re.compile(r"^[0-9A-Fa-f]+\s+([A-Za-z])\s+(.+)$")
PLACEHOLDER = re.compile(r"\b(?:TODO|UNSUPPORTED|skipped)\b", re.IGNORECASE)


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
                "rusty::Arc<F>::new_(std::move(callable))",
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
                "import rusty;",
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
                "CompletionTracker();",
                "CompletionTracker(CompletionTrackerConfig config);",
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
                "rrr::CompletionTracker@rrr.completion_tracker::CompletionTracker()",
                "rrr::CompletionTracker@rrr.completion_tracker::CompletionTracker(rrr::CompletionTrackerConfig@rrr.completion_tracker)",
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
                "import rusty;",
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
}


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
            ["git", *arguments],
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
    placeholder = PLACEHOLDER.search(text)
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
    actual_files = {path.name for path in output.glob("*.cppm") if path.is_file()}
    if actual_files != expected_files:
        raise GateError(
            "generated C++ module census mismatch: expected "
            f"{sorted(expected_files)!r}, got {sorted(actual_files)!r}"
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
        atomic_modules = {
            "rrr.connection_metrics",
            "rrr.completion_tracker",
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
            require_exact_module_imports(text, "rrr.rand", ["rusty"])
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
        if module.cpp_module == "rrr.circuit_breaker":
            require_exact_module_imports(text, "rrr.circuit_breaker", [])
            if text.count(timing_preamble) != 1:
                raise GateError(
                    "generated rrr.circuit_breaker must contain exactly one "
                    "structured timing-kernel preamble include"
                )
            ordered = (
                text.find("\nmodule;\n"),
                text.find(timing_preamble),
                text.find("#include <cstdint>"),
                text.find("export module rrr.circuit_breaker;"),
            )
            if -1 in ordered or list(ordered) != sorted(ordered):
                raise GateError(
                    "generated rrr.circuit_breaker timing preamble is not "
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

    root_text = read_generated(output / "rrr.cppm", "root module")
    if "#include <rusty/sync/atomic.hpp>" in root_text:
        raise GateError("atomic module preamble leaked into the crate root")
    if '#include "misc/srpc_rand.h"' in root_text:
        raise GateError("rand C-kernel preamble leaked into the crate root")
    if '#include "misc/srpc_timing.h"' in root_text:
        raise GateError("timing C-kernel preamble leaked into the crate root")
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
    """Pin initializer and constructor aliases as well as the unique API."""

    expected = Counter(ABI_SPECS["rrr.completion_tracker"].symbols)
    expected.update(
        {
            (
                "T",
                "rrr::CompletionTracker@rrr.completion_tracker::"
                "CompletionTracker()",
            ): 1,
            (
                "T",
                "rrr::CompletionTracker@rrr.completion_tracker::"
                "CompletionTracker(rrr::CompletionTrackerConfig@"
                "rrr.completion_tracker)",
            ): 1,
            ("T", "initializer for module rrr.completion_tracker"): 1,
        }
    )
    actual = Counter(entries)
    if actual == expected:
        return
    missing = sorted((expected - actual).elements())
    unexpected = sorted((actual - expected).elements())
    raise GateError(
        f"{description} completion ABI must contain exactly 33 raw strong "
        "entries (30 unique API symbols, two constructor aliases, and the "
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
#include <rusty/move.hpp>
#include <rusty/option.hpp>
#include <rusty/slice.hpp>
#include <rusty/sync/atomic.hpp>
#include <rusty/traits.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

import rrr.callback_wrapper;
import rrr.circuit_breaker;
import rrr.completion_tracker;
import rrr.connection_metrics;
import rrr.errors;
import rrr.internal_protocol;
import rrr.rand;
import rrr.reconnect_policy;
import rrr.request_options;
import rrr.stat;

static std::int32_t rand_raw_value = 0;
static std::uint32_t rand_raw_draws = 0;
static std::uint32_t rand_destroy_calls = 0;
static std::uint32_t rand_string_evaluations = 0;
static std::uint32_t rand_weight_evaluations = 0;
static std::uint64_t monotonic_now_us = 0;

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

static_assert(std::is_same_v<rrr::RandWeightVec, std::vector<double>>);

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
        rrr::CompletionTracker tracker(config);
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

    rrr::CompletionTracker disabled_tracker(completion_disabled);
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
    rrr::CompletionTracker lifecycle_tracker(lifecycle_config);
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
    rrr::CompletionTracker mutation_tracker(mutation_config);
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
    rrr::CompletionTracker overflow_tracker(overflow_config);
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
            ("generated", [*generated_objects, *runtime_libraries]),
        ]
        if production is not None:
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

            if production is not None:
                production_symbols = module_symbols(
                    nm, root, production, module.cpp_module
                )
                require_expected_symbols(
                    module.cpp_module,
                    "production library",
                    production_symbols,
                )
                if production_symbols != generated_symbols:
                    raise GateError(
                        f"production {module.cpp_module} ABI differs from "
                        "the independently compiled generated-object ABI"
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


def check(args: argparse.Namespace) -> None:
    root = repository_root()
    transpiler = executable(root, args.transpiler, "rusty-cpp transpiler")
    verify_pinned_toolchain(root, transpiler)
    require_extraction_check(root, transpiler)
    modules = load_owned_modules(root)
    clang = executable(root, args.clang, "Clang C++ compiler")
    nm = executable(root, args.nm, "nm")
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
            production=production,
            runtime_libraries=runtime_libraries,
            cxx_flags=cxx_flags,
            link_flags=link_flags,
            prebuilt_module_dirs=prebuilt_module_dirs,
        )
    else:
        with tempfile.TemporaryDirectory(prefix="rrr-crate-mode-") as temporary:
            output = Path(temporary)
            run(
                [
                    str(transpiler),
                    "--crate",
                    "src/rrr/Cargo.toml",
                    "--output-dir",
                    str(output),
                    "--cxx-namespace",
                    "rrr",
                    "--module-preamble",
                    MODULE_PREAMBLE,
                ],
                root,
            )
            check_generated_output(
                root=root,
                output=output,
                modules=modules,
                clang=clang,
                nm=nm,
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
        "CircuitBreaker layout/factory/state/timeout/wrapping runtime contracts, and "
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
