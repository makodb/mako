# Phase 1.2: Reconnection Policy Configuration Plan

## Overview

Implement configurable reconnection policies for RPC clients to control retry behavior after connection failures.

## Design

### ReconnectPolicy Struct

```cpp
struct ReconnectPolicy {
    bool auto_reconnect;        // Enable automatic reconnection
    uint32_t max_retries;       // Maximum reconnection attempts (0 = unlimited)
    uint32_t initial_delay_ms;  // Initial delay before first retry
    uint32_t max_delay_ms;      // Maximum delay between retries
    double backoff_multiplier;  // Exponential backoff multiplier
    bool jitter_enabled;        // Add randomness to prevent thundering herd
};
```

### Exponential Backoff Algorithm

```
delay = min(initial_delay * (multiplier ^ retry_count), max_delay)
if jitter_enabled:
    delay = delay * random(0.5, 1.5)  // +/- 50% jitter
```

### Policy Presets

| Preset       | auto | max_retries | initial | max   | multiplier | jitter |
|--------------|------|-------------|---------|-------|------------|--------|
| AGGRESSIVE   | true | 0           | 100ms   | 5s    | 1.5        | true   |
| CONSERVATIVE | true | 5           | 1s      | 30s   | 2.0        | true   |
| NO_RETRY     | false| 0           | 0       | 0     | 1.0        | false  |

### ReconnectCalculator Class

```cpp
class ReconnectCalculator {
    const ReconnectPolicy& policy_;
    uint32_t retry_count_{0};

public:
    // Calculate next delay based on retry count
    uint32_t next_delay_ms();

    // Check if we should retry
    bool should_retry() const;

    // Reset retry count (on successful connect)
    void reset();

    // Get current retry count
    uint32_t retry_count() const;
};
```

## Implementation Details

### Thread Safety

- `ReconnectPolicy` is a simple POD struct - no interior mutability needed
- `ReconnectCalculator` uses `rusty::Cell<uint32_t>` for retry_count_

### RustyCpp Compliance

- All functions annotated @safe
- No raw pointers
- Uses rusty::Cell for mutable state
- Standard library random functions wrapped in @unsafe blocks

## File Structure

New file: `src/srpc/rpc/reconnect_policy.hpp`

## Estimated LOC

~100-150 lines
