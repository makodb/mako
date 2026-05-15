module;

#include <cstdint>

export module rrr.request_options;

import std;

export namespace rrr {

enum class TimeoutType : uint8_t {
    NONE = 0,
    CONNECT_TIMEOUT,
    REQUEST_TIMEOUT,
    RESPONSE_TIMEOUT,
    TOTAL_TIMEOUT
};

struct RequestOptions {
    uint64_t timeout_ms = 1000;
    uint64_t total_timeout_ms = 0;

    uint16_t max_retries = 0;
    uint16_t base_delay_ms = 50;
    uint16_t max_delay_ms = 5000;
    float jitter_factor = 0.1f;

    bool idempotent = false;

    static RequestOptions defaults() {
        return RequestOptions{};
    }

    static RequestOptions with_retry(uint16_t max_retries, uint64_t timeout_ms = 1000) {
        RequestOptions opts;
        opts.timeout_ms = timeout_ms;
        opts.max_retries = max_retries;
        opts.idempotent = true;
        return opts;
    }

    static RequestOptions idempotent_retry(uint16_t max_retries = 3) {
        RequestOptions opts;
        opts.max_retries = max_retries;
        opts.idempotent = true;
        return opts;
    }

    static RequestOptions no_timeout() {
        RequestOptions opts;
        opts.timeout_ms = 0;
        opts.total_timeout_ms = 0;
        return opts;
    }

    static RequestOptions fast() {
        RequestOptions opts;
        opts.timeout_ms = 100;
        opts.max_retries = 2;
        opts.base_delay_ms = 10;
        opts.max_delay_ms = 100;
        opts.idempotent = true;
        return opts;
    }

    static RequestOptions patient() {
        RequestOptions opts;
        opts.timeout_ms = 10000;
        opts.total_timeout_ms = 60000;
        opts.max_retries = 5;
        opts.base_delay_ms = 500;
        opts.max_delay_ms = 10000;
        opts.idempotent = true;
        return opts;
    }

    bool can_retry(uint16_t current_retry_count) const {
        return idempotent && current_retry_count < max_retries;
    }

    uint64_t calculate_delay_ms(uint16_t attempt) const {
        double delay = static_cast<double>(base_delay_ms) * std::pow(2.0, attempt);

        if (delay > static_cast<double>(max_delay_ms)) {
            delay = static_cast<double>(max_delay_ms);
        }

        if (jitter_factor > 0.0f) {
            thread_local std::mt19937 gen(std::random_device{}());
            thread_local std::uniform_real_distribution<double> dist(-0.5, 0.5);

            double jitter = delay * static_cast<double>(jitter_factor) * dist(gen);
            delay += jitter;

            if (delay < 0.0) {
                delay = 0.0;
            }
        }

        return static_cast<uint64_t>(delay);
    }

    bool is_total_timeout_exceeded(uint64_t elapsed_ms) const {
        return total_timeout_ms > 0 && elapsed_ms >= total_timeout_ms;
    }

    uint64_t remaining_time_ms(uint64_t elapsed_ms) const {
        if (total_timeout_ms == 0) {
            return UINT64_MAX;
        }
        if (elapsed_ms >= total_timeout_ms) {
            return 0;
        }
        return total_timeout_ms - elapsed_ms;
    }
};

inline const char* timeout_type_to_string(TimeoutType type) {
    switch (type) {
        case TimeoutType::NONE: return "NONE";
        case TimeoutType::CONNECT_TIMEOUT: return "CONNECT_TIMEOUT";
        case TimeoutType::REQUEST_TIMEOUT: return "REQUEST_TIMEOUT";
        case TimeoutType::RESPONSE_TIMEOUT: return "RESPONSE_TIMEOUT";
        case TimeoutType::TOTAL_TIMEOUT: return "TOTAL_TIMEOUT";
        default: return "UNKNOWN";
    }
}

} // export namespace rrr
