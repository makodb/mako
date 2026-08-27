# Phase 6.1: Structured Error Types Plan

## Overview

Implement structured error types for RPC operations to provide clear, categorized error handling.

## Design

### RpcErrorCategory

High-level error categories:
- NONE: No error
- CONNECTION: Network/connection issues
- PROTOCOL: RPC protocol violations
- APPLICATION: Application-level errors
- TIMEOUT: Various timeout conditions
- INTERNAL: Internal/unexpected errors

### RpcError Enum

Detailed error codes within each category:

**Connection Errors:**
- NOT_CONNECTED
- CONNECTION_REFUSED
- CONNECTION_RESET
- NETWORK_UNREACHABLE
- HOST_UNREACHABLE

**Protocol Errors:**
- INVALID_MESSAGE
- UNKNOWN_RPC_ID
- MARSHALLING_ERROR
- VERSION_MISMATCH

**Application Errors:**
- RPC_FAILED
- SERVICE_UNAVAILABLE
- PERMISSION_DENIED
- INVALID_ARGUMENT

**Timeout Errors:**
- CONNECT_TIMEOUT
- REQUEST_TIMEOUT
- RESPONSE_TIMEOUT
- IDLE_TIMEOUT

**Internal Errors:**
- UNKNOWN_ERROR
- OUT_OF_MEMORY
- INVALID_STATE

### Error Handling Model

Phase 6 uses structured `RpcError` codes and helper predicates for retry and
classification decisions. There is no RPC-specific exception type.

## Implementation Details

### Thread Safety

- All types are immutable after construction
- No interior mutability needed

### RustyCpp Compliance

- All functions annotated @safe
- No raw pointers
- Uses explicit error codes (no RPC-specific exception path)

## File Structure

New file: `src/srpc/rpc/errors.hpp`

## Estimated LOC

~150-200 lines
