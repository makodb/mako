#![allow(clippy::missing_safety_doc)]
//! Panic-contained C ABI used by Mako's shared TPC-C workload adapter.
//!
//! The surface deliberately exposes only opaque owning handles and byte
//! slices. `StoTpccThread` is thread-affine and contains a lifetime-erased STO
//! transaction and native RCU scope whose actual borrows are tied to boxed
//! workers; their invariant and lifetime-erasing operations are documented
//! below.

use masstree::{
    RcuScope, Runtime as MasstreeRuntime, RuntimeConfig as MasstreeRuntimeConfig, Worker,
};
#[cfg(test)]
use std::thread;
use std::{
    cell::{Cell, RefCell},
    fmt::{self, Write as _},
    mem,
    panic::{catch_unwind, AssertUnwindSafe},
    ptr, slice,
    sync::{
        atomic::{AtomicI32, AtomicU64, Ordering},
        Arc,
    },
};
use sto_core::{
    AbortReason, AccessError, Active, CommitFailure, CommitOutcome, DefiniteOutcome, InvalidUse,
    Runtime, RuntimeConfig, RuntimeId, Transaction, WorkerContext,
};
use sto_masstree::{
    DenseResolvedCache, PointMutation, PointReadBatch, ResolvedRecord, ScanBound, ScanBytesRef,
    ScanControl, ScanDirection, ScanRequest, ScanScratch, Table, TableConfig, Value,
    ValueCopyOutcome,
};

const ERROR_CAPACITY: usize = 1_024;
const PAYMENT_VALUE_CAPACITY: usize = 164;
const PAYMENT_NAME_SCAN_LIMIT: usize = 32;
const PAYMENT_HISTORY_VALUE_LENGTH: usize = 30;
const PAYMENT_CUSTOMER_DATA_VALUE_LENGTH: usize = 303;
const NEW_ORDER_MAX_LINES: usize = 15;
const NEW_ORDER_ITEM_VALUE_MAX: usize = 87;
const NEW_ORDER_STOCK_VALUE_MAX: usize = 16;
const NEW_ORDER_OORDER_VALUE_MAX: usize = 13;
const NEW_ORDER_ORDER_LINE_VALUE_MAX: usize = 16;
const DELIVERY_DISTRICT_COUNT: usize = 10;
const DELIVERY_MAX_LINES_PER_DISTRICT: usize = 15;
const DELIVERY_OORDER_VALUE_MAX: usize = 13;
const DELIVERY_ORDER_LINE_VALUE_MAX: usize = 20;
const STOCK_LEVEL_MAX_ORDER_LINE_ROWS: usize = 20 * 15;
const STOCK_LEVEL_ITEM_SET_SLOTS: usize = 512;
static NEXT_THREAD_COOKIE: AtomicU64 = AtomicU64::new(1);

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Status {
    Ok = 0,
    Miss = 1,
    Duplicate = 2,
    Retry = 3,
    BufferTooSmall = 4,
    Fatal = 5,
}

impl Status {
    const fn code(self) -> i32 {
        self as i32
    }
}

type FfiResult<T> = Result<T, Status>;

struct ErrorBuffer {
    bytes: [u8; ERROR_CAPACITY],
    len: usize,
}

impl ErrorBuffer {
    const fn new() -> Self {
        Self {
            bytes: [0; ERROR_CAPACITY],
            len: 0,
        }
    }

    fn clear(&mut self) {
        self.len = 0;
    }

    fn as_bytes(&self) -> &[u8] {
        &self.bytes[..self.len]
    }
}

impl fmt::Write for ErrorBuffer {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        let available = self.bytes.len().saturating_sub(self.len);
        let mut copied = available.min(text.len());
        while copied != 0 && !text.is_char_boundary(copied) {
            copied -= 1;
        }
        self.bytes[self.len..self.len + copied].copy_from_slice(&text.as_bytes()[..copied]);
        self.len += copied;
        Ok(())
    }
}

thread_local! {
    static LAST_ERROR: RefCell<ErrorBuffer> = const { RefCell::new(ErrorBuffer::new()) };
    // A const-initialized integer TLS slot makes the hot affinity check one
    // TLS load and one compare. Cookies are allocated monotonically when a
    // handle is created, so OS-thread and TLS-address reuse cannot alias an
    // earlier owner.
    static CURRENT_THREAD_COOKIE: Cell<u64> = const { Cell::new(0) };
    #[cfg(test)]
    static RESOLVED_CACHE_SLOT_CALLS: Cell<usize> = const { Cell::new(0) };
    #[cfg(test)]
    static STOCK_LEVEL_CACHE_PARTITION: Cell<(usize, usize)> = const { Cell::new((0, 0)) };
}

#[inline(always)]
fn current_thread_cookie() -> u64 {
    CURRENT_THREAD_COOKIE.with(Cell::get)
}

fn allocate_current_thread_cookie() -> FfiResult<u64> {
    CURRENT_THREAD_COOKIE.with(|cookie| {
        let current = cookie.get();
        if current != 0 {
            return Ok(current);
        }
        let allocated = NEXT_THREAD_COOKIE
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |next| {
                next.checked_add(1)
            })
            .map_err(|_| fatal(format_args!("thread-affinity cookie space exhausted")))?;
        debug_assert_ne!(allocated, 0);
        cookie.set(allocated);
        Ok(allocated)
    })
}

#[cfg(test)]
fn clear_last_error() {
    LAST_ERROR.with(|slot| {
        if let Ok(mut error) = slot.try_borrow_mut() {
            error.clear();
        }
    });
}

fn set_last_error(arguments: fmt::Arguments<'_>) {
    LAST_ERROR.with(|slot| {
        if let Ok(mut error) = slot.try_borrow_mut() {
            error.clear();
            let _ = error.write_fmt(arguments);
        }
    });
}

fn fatal(arguments: fmt::Arguments<'_>) -> Status {
    set_last_error(arguments);
    Status::Fatal
}

fn boundary(operation: &'static str, body: impl FnOnce() -> FfiResult<Status>) -> i32 {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(status) | Err(status)) => status.code(),
        Err(_) => {
            set_last_error(format_args!(
                "{operation}: contained an unexpected Rust panic"
            ));
            Status::Fatal.code()
        }
    }
}

fn boundary_preserving_error(
    operation: &'static str,
    body: impl FnOnce() -> FfiResult<Status>,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(status) | Err(status)) => status.code(),
        Err(_) => {
            set_last_error(format_args!(
                "{operation}: contained an unexpected Rust panic"
            ));
            Status::Fatal.code()
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct StoTpccDbConfig {
    pub max_threads: u32,
    pub max_key_length: u32,
    pub max_items_per_txn: usize,
    pub max_locks_per_txn: usize,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct StoTpccTableConfig {
    pub max_retained_records: u64,
    pub max_retained_key_bytes: u64,
    pub max_consumed_record_ids: u64,
    pub scan_chunk_records: usize,
    pub scan_initial_key_arena_bytes: usize,
    pub scan_max_key_arena_bytes: usize,
    pub max_scan_chunks: usize,
    pub max_scan_physical_records: usize,
    pub trusted_scan_value_generation: u32,
    pub bounded_atomic_values: u32,
}

/// Wrapper-private request for the callback-free local TPC-C Payment prefix.
///
/// The C++ wrapper owns every pointee. The output allocations do not overlap
/// any handle, request, result, or key storage and remain at fixed addresses
/// and immutable after a successful call until the enclosing transaction
/// commits or aborts.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct MakoStoTpccPaymentPrefixRequest {
    pub warehouse_table: *const StoTpccTable,
    pub district_table: *const StoTpccTable,
    pub customer_table: *const StoTpccTable,
    pub customer_name_table: *const StoTpccTable,
    pub warehouse_key: *const u8,
    pub district_key: *const u8,
    pub customer_key_prefix: *const u8,
    pub customer_name_lower_key: *const u8,
    pub customer_name_upper_key: *const u8,
    pub customer_id: i32,
    pub payment_amount: f32,
    pub customer_by_name: u32,
    pub warehouse_output: *mut u8,
    pub district_output: *mut u8,
    pub customer_output: *mut u8,
    pub output_capacity: usize,
}

/// Successful lengths and selected customer for the Payment prefix.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct MakoStoTpccPaymentPrefixResult {
    pub warehouse_length: usize,
    pub district_length: usize,
    pub customer_length: usize,
    pub customer_id: i32,
}

/// Wrapper-private request for a complete local TPC-C Payment transaction.
/// The call resolves the active transaction before returning on every path.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct MakoStoTpccPaymentFullRequest {
    pub warehouse_table: *const StoTpccTable,
    pub district_table: *const StoTpccTable,
    pub customer_table: *const StoTpccTable,
    pub customer_name_table: *const StoTpccTable,
    pub history_table: *const StoTpccTable,
    pub warehouse_key: *const u8,
    pub district_key: *const u8,
    pub customer_key_prefix: *const u8,
    pub customer_name_lower_key: *const u8,
    pub customer_name_upper_key: *const u8,
    pub customer_id: i32,
    pub payment_amount: f32,
    pub timestamp: u32,
    pub warehouse_id: i32,
    pub district_id: i32,
    pub customer_warehouse_id: i32,
    pub customer_district_id: i32,
    pub customer_by_name: u32,
}

/// Successful metadata from a complete, committed local Payment transaction.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct MakoStoTpccPaymentFullResult {
    pub history_value_length: usize,
    pub customer_id: i32,
}

/// Wrapper-private request for a complete exact-home TPC-C NewOrder
/// transaction. The call resolves the active transaction before returning.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct MakoStoTpccNewOrderFullRequest {
    pub warehouse_table: *const StoTpccTable,
    pub district_table: *const StoTpccTable,
    pub customer_table: *const StoTpccTable,
    pub item_table: *const StoTpccTable,
    pub stock_table: *const StoTpccTable,
    pub new_order_table: *const StoTpccTable,
    pub oorder_table: *const StoTpccTable,
    pub oorder_c_id_idx_table: *const StoTpccTable,
    pub order_line_table: *const StoTpccTable,
    pub item_ids: *const u32,
    pub quantities: *const u32,
    pub warehouse_id: i32,
    pub district_id: i32,
    pub customer_id: i32,
    pub order_id: i32,
    pub entry_date: u32,
    pub line_count: u32,
}

/// Successful accounting from a complete, committed NewOrder transaction.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct MakoStoTpccNewOrderFullResult {
    pub reported_value_bytes: usize,
}

/// Wrapper-private request for a complete local TPC-C Delivery transaction.
/// The cursor array is deliberately external to STO and is updated in place.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct MakoStoTpccDeliveryFullRequest {
    pub new_order_table: *const StoTpccTable,
    pub oorder_table: *const StoTpccTable,
    pub order_line_table: *const StoTpccTable,
    pub customer_table: *const StoTpccTable,
    pub last_no_o_ids: *mut i32,
    pub warehouse_id: i32,
    pub carrier_id: i32,
    pub timestamp: u32,
}

/// Successful accounting from a complete, committed Delivery transaction.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct MakoStoTpccDeliveryFullResult {
    pub reported_value_bytes: usize,
    pub delivered_districts: u32,
    pub updated_order_lines: u32,
}

/// Wrapper-private request for the scan-and-join tail of one local TPC-C
/// StockLevel transaction. The district prefix has already run in C++.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct MakoStoTpccStockLevelFullRequest {
    pub order_line_table: *const StoTpccTable,
    pub stock_table: *const StoTpccTable,
    pub current_next_order_id: u64,
    pub warehouse_id: i32,
    pub district_id: i32,
    pub threshold: u32,
}

/// Successful accounting and query result from a committed StockLevel tail.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct MakoStoTpccStockLevelFullResult {
    pub reported_value_bytes: usize,
    pub scanned_order_line_rows: u32,
    pub distinct_item_ids: u32,
    pub low_stock_count: u32,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 120] = [(); mem::size_of::<MakoStoTpccPaymentPrefixRequest>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 32] = [(); mem::size_of::<MakoStoTpccPaymentPrefixResult>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 112] = [(); mem::size_of::<MakoStoTpccPaymentFullRequest>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); mem::size_of::<MakoStoTpccPaymentFullResult>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 112] = [(); mem::size_of::<MakoStoTpccNewOrderFullRequest>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 8] = [(); mem::size_of::<MakoStoTpccNewOrderFullResult>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 56] = [(); mem::size_of::<MakoStoTpccDeliveryFullRequest>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); mem::size_of::<MakoStoTpccDeliveryFullResult>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 40] = [(); mem::size_of::<MakoStoTpccStockLevelFullRequest>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 24] = [(); mem::size_of::<MakoStoTpccStockLevelFullResult>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 0] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, warehouse_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 8] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, district_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, customer_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 24] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, customer_name_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 32] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, warehouse_key)];
#[cfg(target_pointer_width = "64")]
const _: [(); 40] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, district_key)];
#[cfg(target_pointer_width = "64")]
const _: [(); 48] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, customer_key_prefix)];
#[cfg(target_pointer_width = "64")]
const _: [(); 56] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, customer_name_lower_key)];
#[cfg(target_pointer_width = "64")]
const _: [(); 64] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, customer_name_upper_key)];
#[cfg(target_pointer_width = "64")]
const _: [(); 72] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, customer_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 76] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, payment_amount)];
#[cfg(target_pointer_width = "64")]
const _: [(); 80] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, customer_by_name)];
#[cfg(target_pointer_width = "64")]
const _: [(); 88] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, warehouse_output)];
#[cfg(target_pointer_width = "64")]
const _: [(); 96] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, district_output)];
#[cfg(target_pointer_width = "64")]
const _: [(); 104] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, customer_output)];
#[cfg(target_pointer_width = "64")]
const _: [(); 112] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixRequest, output_capacity)];
#[cfg(target_pointer_width = "64")]
const _: [(); 0] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixResult, warehouse_length)];
#[cfg(target_pointer_width = "64")]
const _: [(); 8] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixResult, district_length)];
#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixResult, customer_length)];
#[cfg(target_pointer_width = "64")]
const _: [(); 24] = [(); mem::offset_of!(MakoStoTpccPaymentPrefixResult, customer_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 0] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, warehouse_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 8] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, district_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, customer_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 24] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, customer_name_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 32] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, history_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 40] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, warehouse_key)];
#[cfg(target_pointer_width = "64")]
const _: [(); 48] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, district_key)];
#[cfg(target_pointer_width = "64")]
const _: [(); 56] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, customer_key_prefix)];
#[cfg(target_pointer_width = "64")]
const _: [(); 64] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, customer_name_lower_key)];
#[cfg(target_pointer_width = "64")]
const _: [(); 72] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, customer_name_upper_key)];
#[cfg(target_pointer_width = "64")]
const _: [(); 80] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, customer_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 84] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, payment_amount)];
#[cfg(target_pointer_width = "64")]
const _: [(); 88] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, timestamp)];
#[cfg(target_pointer_width = "64")]
const _: [(); 92] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, warehouse_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 96] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, district_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 100] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, customer_warehouse_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 104] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, customer_district_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 108] = [(); mem::offset_of!(MakoStoTpccPaymentFullRequest, customer_by_name)];
#[cfg(target_pointer_width = "64")]
const _: [(); 0] = [(); mem::offset_of!(MakoStoTpccPaymentFullResult, history_value_length)];
#[cfg(target_pointer_width = "64")]
const _: [(); 8] = [(); mem::offset_of!(MakoStoTpccPaymentFullResult, customer_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 0] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, warehouse_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 8] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, district_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, customer_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 24] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, item_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 32] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, stock_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 40] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, new_order_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 48] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, oorder_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 56] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, oorder_c_id_idx_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 64] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, order_line_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 72] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, item_ids)];
#[cfg(target_pointer_width = "64")]
const _: [(); 80] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, quantities)];
#[cfg(target_pointer_width = "64")]
const _: [(); 88] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, warehouse_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 92] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, district_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 96] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, customer_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 100] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, order_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 104] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, entry_date)];
#[cfg(target_pointer_width = "64")]
const _: [(); 108] = [(); mem::offset_of!(MakoStoTpccNewOrderFullRequest, line_count)];
#[cfg(target_pointer_width = "64")]
const _: [(); 0] = [(); mem::offset_of!(MakoStoTpccNewOrderFullResult, reported_value_bytes)];
#[cfg(target_pointer_width = "64")]
const _: [(); 0] = [(); mem::offset_of!(MakoStoTpccDeliveryFullRequest, new_order_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 8] = [(); mem::offset_of!(MakoStoTpccDeliveryFullRequest, oorder_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); mem::offset_of!(MakoStoTpccDeliveryFullRequest, order_line_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 24] = [(); mem::offset_of!(MakoStoTpccDeliveryFullRequest, customer_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 32] = [(); mem::offset_of!(MakoStoTpccDeliveryFullRequest, last_no_o_ids)];
#[cfg(target_pointer_width = "64")]
const _: [(); 40] = [(); mem::offset_of!(MakoStoTpccDeliveryFullRequest, warehouse_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 44] = [(); mem::offset_of!(MakoStoTpccDeliveryFullRequest, carrier_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 48] = [(); mem::offset_of!(MakoStoTpccDeliveryFullRequest, timestamp)];
#[cfg(target_pointer_width = "64")]
const _: [(); 0] = [(); mem::offset_of!(MakoStoTpccDeliveryFullResult, reported_value_bytes)];
#[cfg(target_pointer_width = "64")]
const _: [(); 8] = [(); mem::offset_of!(MakoStoTpccDeliveryFullResult, delivered_districts)];
#[cfg(target_pointer_width = "64")]
const _: [(); 12] = [(); mem::offset_of!(MakoStoTpccDeliveryFullResult, updated_order_lines)];
#[cfg(target_pointer_width = "64")]
const _: [(); 0] = [(); mem::offset_of!(MakoStoTpccStockLevelFullRequest, order_line_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 8] = [(); mem::offset_of!(MakoStoTpccStockLevelFullRequest, stock_table)];
#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); mem::offset_of!(MakoStoTpccStockLevelFullRequest, current_next_order_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 24] = [(); mem::offset_of!(MakoStoTpccStockLevelFullRequest, warehouse_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 28] = [(); mem::offset_of!(MakoStoTpccStockLevelFullRequest, district_id)];
#[cfg(target_pointer_width = "64")]
const _: [(); 32] = [(); mem::offset_of!(MakoStoTpccStockLevelFullRequest, threshold)];
#[cfg(target_pointer_width = "64")]
const _: [(); 0] = [(); mem::offset_of!(MakoStoTpccStockLevelFullResult, reported_value_bytes)];
#[cfg(target_pointer_width = "64")]
const _: [(); 8] = [(); mem::offset_of!(MakoStoTpccStockLevelFullResult, scanned_order_line_rows)];
#[cfg(target_pointer_width = "64")]
const _: [(); 12] = [(); mem::offset_of!(MakoStoTpccStockLevelFullResult, distinct_item_ids)];
#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); mem::offset_of!(MakoStoTpccStockLevelFullResult, low_stock_count)];

pub struct StoTpccDb {
    sto: Arc<Runtime>,
    masstree: MasstreeRuntime,
    max_pending_size_deltas: usize,
}

struct TableState {
    table: Table,
    logical_rows: AtomicU64,
    runtime_id: RuntimeId,
}

pub struct StoTpccTable {
    state: Arc<TableState>,
    cache_policy: ResolvedCachePolicy,
    dense_policy: DenseCachePolicy,
    dense_cache: Option<DenseResolvedCache>,
    dense_stock_warehouse_id: AtomicI32,
}

struct PendingSizeDelta {
    table: Arc<TableState>,
    delta: i64,
}

const RESOLVED_CACHE_KEY_BYTES: usize = 32;
// One TPC-C warehouse has 30,000 base customer rows and 100,000 sealed stock
// rows. A 4,096-slot cache is a bounded 256 KiB compromise. Besides recurring
// customer keys, it retains the recent NewOrder stock identities that
// StockLevel is most likely to revisit; each exact hit bypasses one tree
// traversal while retaining ordinary STO observation and validation.
const RESOLVED_CACHE_ENTRIES: usize = 4_096;
const LAST_ONLY_CACHE_SLOT: usize = RESOLVED_CACHE_ENTRIES;
const RESOLVED_SCAN_CACHE_LIMIT: usize = 16;
const DENSE_TPCC_ITEM_SLOTS: usize = 100_000;
const _: () = assert!(RESOLVED_CACHE_ENTRIES.is_power_of_two());
const _: () = assert!(LAST_ONLY_CACHE_SLOT == RESOLVED_CACHE_ENTRIES);

pub type StoTpccResolvedCachePolicy = i32;
pub const STO_TPCC_RESOLVED_CACHE_FULL: StoTpccResolvedCachePolicy = 0;
pub const STO_TPCC_RESOLVED_CACHE_LAST_ONLY: StoTpccResolvedCachePolicy = 1;
pub const STO_TPCC_RESOLVED_CACHE_READ_THEN_WRITE: StoTpccResolvedCachePolicy = 2;
pub const STO_TPCC_RESOLVED_CACHE_NONE: StoTpccResolvedCachePolicy = 3;
pub const STO_TPCC_RESOLVED_CACHE_DENSE_ITEM: StoTpccResolvedCachePolicy = 4;
pub const STO_TPCC_RESOLVED_CACHE_DENSE_STOCK: StoTpccResolvedCachePolicy = 5;

pub type StoTpccFixedReadCallback = unsafe extern "C" fn(
    context: *mut std::ffi::c_void,
    index: usize,
    current_value: *const u8,
    current_value_length: usize,
) -> i32;

pub type StoTpccFixedModifyAction = i32;
pub const STO_TPCC_FIXED_MODIFY_KEEP: StoTpccFixedModifyAction = 0;
pub const STO_TPCC_FIXED_MODIFY_PUT: StoTpccFixedModifyAction = 1;
pub const STO_TPCC_FIXED_MODIFY_REMOVE: StoTpccFixedModifyAction = 2;
pub const STO_TPCC_FIXED_MODIFY_FAILED: StoTpccFixedModifyAction = 3;

pub type StoTpccFixedModifyCallback = unsafe extern "C" fn(
    context: *mut std::ffi::c_void,
    index: usize,
    current_value: *const u8,
    current_value_length: usize,
    out_replacement: *mut *const u8,
    out_replacement_length: *mut usize,
) -> StoTpccFixedModifyAction;

pub type StoTpccFixedPutMode = i32;
pub const STO_TPCC_FIXED_PUT_UPSERT: StoTpccFixedPutMode = 0;
pub const STO_TPCC_FIXED_PUT_INSERT: StoTpccFixedPutMode = 1;

/// One caller-owned value copied synchronously by `sto_tpcc_put_fixed`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct StoTpccFixedValue {
    pub data: *const u8,
    pub length: usize,
}

/// Aggregate result from one fixed-width put/insert batch.
///
/// `first_duplicate` is `usize::MAX` when no sequential insert position
/// observed a live transaction-local value.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct StoTpccFixedPutResult {
    pub inserted: usize,
    pub first_duplicate: usize,
}

/// One caller-owned heterogeneous INSERT copied synchronously by
/// `sto_tpcc_insert_many`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct StoTpccInsertOperation {
    pub table: *const StoTpccTable,
    pub key: *const u8,
    pub key_length: usize,
    pub value: *const u8,
    pub value_length: usize,
}

impl Default for StoTpccFixedPutResult {
    fn default() -> Self {
        Self {
            inserted: 0,
            first_duplicate: usize::MAX,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum FixedPutMode {
    Upsert,
    Insert,
}

impl FixedPutMode {
    fn from_raw(raw: StoTpccFixedPutMode) -> FfiResult<Self> {
        match raw {
            STO_TPCC_FIXED_PUT_UPSERT => Ok(Self::Upsert),
            STO_TPCC_FIXED_PUT_INSERT => Ok(Self::Insert),
            unknown => Err(fatal(format_args!(
                "invalid fixed put mode {unknown}; expected 0 (upsert) or 1 (insert)"
            ))),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ResolvedCachePolicy {
    Full,
    LastOnly,
    ReadThenWrite,
    None,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum DenseCachePolicy {
    None,
    Item,
    Stock,
}

impl ResolvedCachePolicy {
    fn from_raw(raw: StoTpccResolvedCachePolicy) -> FfiResult<(Self, DenseCachePolicy)> {
        match raw {
            STO_TPCC_RESOLVED_CACHE_FULL => Ok((Self::Full, DenseCachePolicy::None)),
            STO_TPCC_RESOLVED_CACHE_LAST_ONLY => Ok((Self::LastOnly, DenseCachePolicy::None)),
            STO_TPCC_RESOLVED_CACHE_READ_THEN_WRITE => {
                Ok((Self::ReadThenWrite, DenseCachePolicy::None))
            }
            STO_TPCC_RESOLVED_CACHE_NONE => Ok((Self::None, DenseCachePolicy::None)),
            STO_TPCC_RESOLVED_CACHE_DENSE_ITEM => Ok((Self::None, DenseCachePolicy::Item)),
            STO_TPCC_RESOLVED_CACHE_DENSE_STOCK => {
                Ok((Self::ReadThenWrite, DenseCachePolicy::Stock))
            }
            _ => Err(fatal(format_args!("invalid resolved-cache policy {raw}"))),
        }
    }
}

#[derive(Clone, Copy, Default)]
#[cfg_attr(test, derive(Debug, Eq, PartialEq))]
struct ResolvedCacheEntry {
    key: [u8; RESOLVED_CACHE_KEY_BYTES],
    key_len: u8,
    record: Option<ResolvedRecord>,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 64] = [(); mem::size_of::<ResolvedCacheEntry>()];

struct ResolvedCache {
    entries: Vec<ResolvedCacheEntry>,
    last_slot: Option<usize>,
}

#[derive(Clone, Copy)]
struct ResolvedCacheProbe {
    record: Option<ResolvedRecord>,
    miss_slot: Option<usize>,
}

impl Default for ResolvedCache {
    fn default() -> Self {
        Self {
            // Slots below `RESOLVED_CACHE_ENTRIES` form the direct-mapped Full
            // cache. The final slot is a separate lane for LastOnly tables;
            // keeping it in this allocation preserves ResolvedCache's inline
            // layout and avoids a second allocation or 64-byte field.
            entries: vec![ResolvedCacheEntry::default(); RESOLVED_CACHE_ENTRIES + 1],
            last_slot: None,
        }
    }
}

impl ResolvedCache {
    #[inline(always)]
    fn hash(table_hint: u64, key: &[u8]) -> u64 {
        let mut hash = (key.len() as u64) ^ table_hint.rotate_left(17) ^ 0x517c_c1b7_2722_0a95;
        let mut chunks = key.chunks_exact(std::mem::size_of::<u64>());
        for chunk in &mut chunks {
            let word = u64::from_ne_bytes(chunk.try_into().expect("eight-byte cache key chunk"));
            hash = (hash.rotate_left(5) ^ word).wrapping_mul(0x517c_c1b7_2722_0a95);
        }
        if !chunks.remainder().is_empty() {
            let mut tail = [0_u8; std::mem::size_of::<u64>()];
            tail[..chunks.remainder().len()].copy_from_slice(chunks.remainder());
            hash = (hash.rotate_left(5) ^ u64::from_ne_bytes(tail))
                .wrapping_mul(0x517c_c1b7_2722_0a95);
        }

        // TPC-C integer keys are big-endian. On little-endian hosts their
        // changing low-order bytes land high in the native word above, so a
        // plain power-of-two mask would ignore them. This finalizer folds
        // those bits into the slot index.
        hash ^= hash >> 33;
        hash = hash.wrapping_mul(0xff51_afd7_ed55_8ccd);
        hash ^ (hash >> 33)
    }

    #[inline(always)]
    fn slot(table: &Table, key: &[u8]) -> usize {
        #[cfg(test)]
        RESOLVED_CACHE_SLOT_CALLS.with(|calls| calls.set(calls.get() + 1));
        // The table address is only a distribution hint. Exact hits still
        // validate the never-reused table identity carried by ResolvedRecord,
        // so allocator address reuse cannot create a false cache hit.
        let table_hint = std::ptr::from_ref(table).addr() as u64;
        Self::hash(table_hint, key) as usize & (RESOLVED_CACHE_ENTRIES - 1)
    }

    #[inline(always)]
    fn entry_matches(
        entry: &ResolvedCacheEntry,
        table: &Table,
        key: &[u8],
    ) -> Option<ResolvedRecord> {
        let record = entry.record?;
        (table.owns_resolved(record)
            && usize::from(entry.key_len) == key.len()
            && entry.key[..key.len()] == *key)
            .then_some(record)
    }

    #[inline(always)]
    fn probe(&mut self, table: &Table, key: &[u8]) -> ResolvedCacheProbe {
        if key.len() > RESOLVED_CACHE_KEY_BYTES {
            self.last_slot = None;
            return ResolvedCacheProbe {
                record: None,
                miss_slot: None,
            };
        }
        if let Some(slot) = self.last_slot {
            if let Some(record) = Self::entry_matches(&self.entries[slot], table, key) {
                return ResolvedCacheProbe {
                    record: Some(record),
                    miss_slot: None,
                };
            }
        }

        let slot = Self::slot(table, key);
        if Some(slot) != self.last_slot {
            if let Some(record) = Self::entry_matches(&self.entries[slot], table, key) {
                self.last_slot = Some(slot);
                return ResolvedCacheProbe {
                    record: Some(record),
                    miss_slot: None,
                };
            }
        }
        ResolvedCacheProbe {
            record: None,
            miss_slot: Some(slot),
        }
    }

    #[inline(always)]
    fn matching(&mut self, table: &Table, key: &[u8]) -> Option<ResolvedRecord> {
        self.probe(table, key).record
    }

    #[inline(always)]
    fn remember(&mut self, table: &Table, key: &[u8], record: ResolvedRecord) {
        if key.len() > RESOLVED_CACHE_KEY_BYTES {
            self.last_slot = None;
            return;
        }
        debug_assert!(table.owns_resolved(record));
        let slot = Self::slot(table, key);
        self.remember_in_slot(key, record, slot);
    }

    #[inline(always)]
    fn remember_after_probe(
        &mut self,
        table: &Table,
        key: &[u8],
        record: ResolvedRecord,
        probe: ResolvedCacheProbe,
    ) {
        debug_assert!(probe.record.is_none());
        debug_assert!(table.owns_resolved(record));
        let Some(slot) = probe.miss_slot else {
            self.last_slot = None;
            return;
        };
        self.remember_in_slot(key, record, slot);
    }

    #[inline(always)]
    fn remember_in_slot(&mut self, key: &[u8], record: ResolvedRecord, slot: usize) {
        let entry = &mut self.entries[slot];
        entry.key[..key.len()].copy_from_slice(key);
        entry.key_len = key.len() as u8;
        entry.record = Some(record);
        self.last_slot = Some(slot);
    }

    #[inline(always)]
    fn matching_last_only(&mut self, table: &Table, key: &[u8]) -> Option<ResolvedRecord> {
        if key.len() > RESOLVED_CACHE_KEY_BYTES {
            self.entries[LAST_ONLY_CACHE_SLOT].record = None;
            return None;
        }
        Self::entry_matches(&self.entries[LAST_ONLY_CACHE_SLOT], table, key)
    }

    #[inline(always)]
    fn remember_last_only(&mut self, table: &Table, key: &[u8], record: ResolvedRecord) {
        if key.len() > RESOLVED_CACHE_KEY_BYTES {
            self.entries[LAST_ONLY_CACHE_SLOT].record = None;
            return;
        }
        debug_assert!(table.owns_resolved(record));
        let entry = &mut self.entries[LAST_ONLY_CACHE_SLOT];
        entry.key[..key.len()].copy_from_slice(key);
        entry.key_len = key.len() as u8;
        entry.record = Some(record);
    }
}

type ActiveTransaction = Transaction<'static, Active>;
type ActiveRcuScope = RcuScope<'static>;

struct ActiveAttempt {
    // Drop the logical transaction before releasing its native lifetime guard.
    transaction: ActiveTransaction,
    rcu_scope: ActiveRcuScope,
}

pub struct StoTpccThread {
    // This field must be destroyed before either boxed worker. The explicit
    // Drop implementation also takes and resolves it before field destruction.
    active: Option<ActiveAttempt>,
    sto_worker: Box<WorkerContext>,
    native_worker: Box<Worker>,
    owner_cookie: u64,
    // A small linear vector avoids allocating a hash table in every TPC-C
    // transaction. Thread creation reserves the runtime's complete item bound,
    // so recording size deltas cannot allocate after staging a mutation;
    // capacity is retained across attempts.
    pending_size: Vec<PendingSizeDelta>,
    // Stable append-only RecordIds permit a small worker-local directory
    // cache. It captures immediate get-then-put and recurring warehouse /
    // district keys without weakening per-attempt OCC validation.
    resolved_cache: ResolvedCache,
    // Native scan descriptors and key bytes are reused across transactions on
    // this thread-affine handle. A synchronous callback may borrow a row, but
    // the ABI forbids retaining pointers or re-entering this handle.
    scan_scratch: ScanScratch,
    // Directory results, exact-alias validation order, and STO point scratch
    // are retained for fixed-width TPC-C point-read batches.
    point_batch: PointReadBatch,
}

impl StoTpccThread {
    #[inline(always)]
    fn ensure_owner(&self) -> FfiResult<()> {
        if self.owner_cookie == current_thread_cookie() {
            Ok(())
        } else {
            Err(fatal(format_args!(
                "thread handle used or destroyed from a different OS thread"
            )))
        }
    }

    fn record_size_delta(&mut self, table: &Arc<TableState>, delta: i64) -> FfiResult<()> {
        if let Some(pending) = self
            .pending_size
            .iter_mut()
            .find(|pending| Arc::ptr_eq(&pending.table, table))
        {
            pending.delta = pending
                .delta
                .checked_add(delta)
                .ok_or_else(|| fatal(format_args!("transactional table-size delta overflow")))?;
            return Ok(());
        }

        self.pending_size.try_reserve(1).map_err(|_| {
            fatal(format_args!(
                "unable to reserve transactional table-size accounting"
            ))
        })?;
        self.pending_size.push(PendingSizeDelta {
            table: Arc::clone(table),
            delta,
        });
        Ok(())
    }

    fn apply_size_deltas(&self) -> FfiResult<()> {
        for pending in &self.pending_size {
            adjust_logical_rows(&pending.table.logical_rows, pending.delta)?;
        }
        Ok(())
    }
}

fn active_transaction(active: &mut Option<ActiveAttempt>) -> FfiResult<&mut ActiveTransaction> {
    active
        .as_mut()
        .map(|attempt| &mut attempt.transaction)
        .ok_or_else(|| {
            fatal(format_args!(
                "transactional operation requires an active transaction"
            ))
        })
}

fn abort_active_attempt_after_fatal(handle: &mut StoTpccThread) -> FfiResult<()> {
    let ActiveAttempt {
        transaction,
        rcu_scope,
    } = handle
        .active
        .take()
        .ok_or_else(|| fatal(format_args!("fatal operation found no active transaction")))?;
    let _ = transaction.abort();
    handle.pending_size.clear();
    rcu_scope
        .close()
        .map_err(|error| fatal(format_args!("unable to end native RCU scope: {error}")))
}

fn record_size_delta_after_staging(
    handle: &mut StoTpccThread,
    table: &Arc<TableState>,
    delta: i64,
) -> FfiResult<()> {
    if let Err(error) = handle.record_size_delta(table, delta) {
        // A staged table mutation must never remain committable when its
        // corresponding logical-size accounting cannot be recorded.
        abort_active_attempt_after_fatal(handle)?;
        return Err(error);
    }
    Ok(())
}

impl Drop for StoTpccThread {
    fn drop(&mut self) {
        if let Some(ActiveAttempt {
            transaction,
            rcu_scope,
        }) = self.active.take()
        {
            let _ = transaction.abort();
            drop(rcu_scope);
        }
        self.pending_size.clear();
    }
}

fn adjust_logical_rows(rows: &AtomicU64, delta: i64) -> FfiResult<()> {
    if delta == 0 {
        return Ok(());
    }
    let mut current = rows.load(Ordering::Relaxed);
    loop {
        let next = if delta > 0 {
            current.checked_add(delta as u64)
        } else {
            current.checked_sub(delta.unsigned_abs())
        }
        .ok_or_else(|| {
            fatal(format_args!(
                "committed logical table-size invariant failed"
            ))
        })?;
        match rows.compare_exchange_weak(current, next, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => return Ok(()),
            Err(observed) => current = observed,
        }
    }
}

fn status_from_access(operation: &str, error: AccessError) -> Status {
    let retry = matches!(
        error,
        AccessError::Conflict(_) | AccessError::InvalidUse(sto_core::InvalidUse::TransactionDoomed)
    );
    set_last_error(format_args!("{operation}: {error}"));
    if retry {
        Status::Retry
    } else {
        Status::Fatal
    }
}

fn status_from_abort(reason: AbortReason) -> Status {
    let retry = matches!(reason, AbortReason::Doomed | AbortReason::Conflict(_));
    set_last_error(format_args!("transaction commit aborted: {reason}"));
    if retry {
        Status::Retry
    } else {
        Status::Fatal
    }
}

unsafe fn required_ref<'a, T>(pointer: *const T, name: &str) -> FfiResult<&'a T> {
    if pointer.is_null() {
        return Err(fatal(format_args!("{name} must not be null")));
    }
    // SAFETY: The public C contract requires a live, aligned handle/config
    // pointer of the corresponding type for the duration of the call.
    Ok(unsafe { &*pointer })
}

unsafe fn required_mut<'a, T>(pointer: *mut T, name: &str) -> FfiResult<&'a mut T> {
    if pointer.is_null() {
        return Err(fatal(format_args!("{name} must not be null")));
    }
    // SAFETY: The public C contract requires exclusive access to the live,
    // aligned handle/output pointer for the duration of the call.
    Ok(unsafe { &mut *pointer })
}

unsafe fn bytes<'a>(pointer: *const u8, length: usize, name: &str) -> FfiResult<&'a [u8]> {
    if length == 0 {
        return Ok(&[]);
    }
    if pointer.is_null() {
        return Err(fatal(format_args!(
            "{name} must not be null when its length is nonzero"
        )));
    }
    // SAFETY: The C caller guarantees `length` readable bytes and no mutation
    // for the duration of the operation.
    Ok(unsafe { slice::from_raw_parts(pointer, length) })
}

unsafe fn fixed_keys<'a, const KEY_LENGTH: usize>(
    pointer: *const u8,
    count: usize,
) -> FfiResult<&'a [[u8; KEY_LENGTH]]> {
    let length = count
        .checked_mul(KEY_LENGTH)
        .filter(|length| *length <= isize::MAX as usize)
        .ok_or_else(|| fatal(format_args!("fixed key batch byte length overflows")))?;
    let packed = unsafe { bytes(pointer, length, "keys")? };
    // SAFETY: `[u8; KEY_LENGTH]` has byte alignment, `packed` contains exactly
    // `count * KEY_LENGTH` bytes, and the public contract keeps the input live
    // and immutable for the synchronous call.
    Ok(unsafe { slice::from_raw_parts(packed.as_ptr().cast::<[u8; KEY_LENGTH]>(), count) })
}

unsafe fn fixed_values<'a>(
    pointer: *const StoTpccFixedValue,
    key_count: usize,
) -> FfiResult<&'a [StoTpccFixedValue]> {
    if key_count == 0 {
        return Ok(&[]);
    }

    let byte_length = key_count
        .checked_mul(mem::size_of::<StoTpccFixedValue>())
        .filter(|length| *length <= isize::MAX as usize)
        .ok_or_else(|| fatal(format_args!("fixed value descriptor byte length overflows")))?;
    if pointer.is_null() {
        return Err(fatal(format_args!(
            "values must not be null for a nonempty fixed put batch"
        )));
    }
    if !pointer
        .addr()
        .is_multiple_of(mem::align_of::<StoTpccFixedValue>())
        || pointer.addr().checked_add(byte_length).is_none()
    {
        return Err(fatal(format_args!(
            "values must name one aligned, non-overflowing descriptor range"
        )));
    }
    // SAFETY: Nullness, alignment, length, and address overflow were checked;
    // the public ABI requires all `key_count` elements to remain readable for the
    // synchronous call.
    let values = unsafe { slice::from_raw_parts(pointer, key_count) };
    for (index, value) in values.iter().enumerate() {
        if value.length > isize::MAX as usize
            || (value.length != 0
                && (value.data.is_null() || value.data.addr().checked_add(value.length).is_none()))
        {
            return Err(fatal(format_args!(
                "fixed value {index} must name one non-overflowing readable range"
            )));
        }
    }
    Ok(values)
}

unsafe fn insert_operations<'a>(
    pointer: *const StoTpccInsertOperation,
    count: usize,
) -> FfiResult<&'a [StoTpccInsertOperation]> {
    if count == 0 {
        return Ok(&[]);
    }
    let byte_length = count
        .checked_mul(mem::size_of::<StoTpccInsertOperation>())
        .filter(|length| *length <= isize::MAX as usize)
        .ok_or_else(|| {
            fatal(format_args!(
                "insert operation descriptor byte length overflows"
            ))
        })?;
    if pointer.is_null()
        || !pointer
            .addr()
            .is_multiple_of(mem::align_of::<StoTpccInsertOperation>())
        || pointer.addr().checked_add(byte_length).is_none()
    {
        return Err(fatal(format_args!(
            "insert operations must name one aligned, non-overflowing descriptor range"
        )));
    }
    // SAFETY: Nullness, alignment, length, and address overflow were checked;
    // the public ABI keeps the complete descriptor range live for the call.
    let operations = unsafe { slice::from_raw_parts(pointer, count) };
    for (index, operation) in operations.iter().enumerate() {
        if operation.table.is_null() {
            return Err(fatal(format_args!(
                "insert operation {index} table must not be null"
            )));
        }
        // Validate every byte descriptor before staging the first mutation.
        // `slice::from_raw_parts` additionally requires its byte length to fit
        // `isize`; checking the address addition catches wrapped ranges before
        // the later hot loop materializes either slice.
        for (pointer, length, kind) in [
            (operation.key, operation.key_length, "key"),
            (operation.value, operation.value_length, "value"),
        ] {
            if length > isize::MAX as usize
                || (length != 0
                    && (pointer.is_null() || pointer.addr().checked_add(length).is_none()))
            {
                return Err(fatal(format_args!(
                    "insert operation {index} {kind} must name one non-overflowing readable range"
                )));
            }
        }
    }
    Ok(operations)
}

unsafe fn output_bytes<'a>(
    pointer: *mut u8,
    capacity: usize,
    name: &str,
) -> FfiResult<&'a mut [u8]> {
    if capacity == 0 {
        return Ok(&mut []);
    }
    if pointer.is_null() {
        return Err(fatal(format_args!(
            "{name} must not be null when its capacity is nonzero"
        )));
    }
    // SAFETY: The C caller guarantees `capacity` uniquely writable bytes for
    // the duration of the operation.
    Ok(unsafe { slice::from_raw_parts_mut(pointer, capacity) })
}

fn db_config(raw: StoTpccDbConfig) -> (RuntimeConfig, MasstreeRuntimeConfig) {
    let mut sto = RuntimeConfig::new();
    let mut native = MasstreeRuntimeConfig::new();
    if raw.max_threads != 0 {
        sto = sto.with_max_workers(raw.max_threads as usize);
        native = native.with_max_threads(raw.max_threads);
    }
    if raw.max_key_length != 0 {
        native = native.with_max_key_length(raw.max_key_length);
    }
    if raw.max_items_per_txn != 0 {
        sto = sto.with_max_items_per_transaction(raw.max_items_per_txn);
    }
    if raw.max_locks_per_txn != 0 {
        sto = sto.with_max_locks_per_transaction(raw.max_locks_per_txn);
    }
    (sto, native)
}

fn table_config(raw: StoTpccTableConfig) -> TableConfig {
    // Every transactional resource reachable through this closed FFI is a
    // sto-masstree table constructed here with the same policy. Core item
    // deduplication gives each table/record and table/membership identity at
    // most one request, while table namespaces keep different handles
    // distinct. The transaction-wide unique-lock contract therefore holds.
    let mut config = TableConfig::new().with_unique_lock_requests(true);
    if raw.max_retained_records != 0 {
        config = config.with_max_retained_records(raw.max_retained_records);
    }
    if raw.max_retained_key_bytes != 0 {
        config = config.with_max_retained_key_bytes(raw.max_retained_key_bytes);
    }
    if raw.max_consumed_record_ids != 0 {
        config = config.with_max_consumed_record_ids(raw.max_consumed_record_ids);
    }
    if raw.scan_chunk_records != 0 {
        config = config.with_scan_chunk_records(raw.scan_chunk_records);
    }
    if raw.scan_initial_key_arena_bytes != 0 {
        config = config.with_scan_initial_key_arena_bytes(raw.scan_initial_key_arena_bytes);
    }
    if raw.scan_max_key_arena_bytes != 0 {
        config = config.with_scan_max_key_arena_bytes(raw.scan_max_key_arena_bytes);
    }
    if raw.max_scan_chunks != 0 {
        config = config.with_max_scan_chunks(raw.max_scan_chunks);
    }
    if raw.max_scan_physical_records != 0 {
        config = config.with_max_scan_physical_records(raw.max_scan_physical_records);
    }
    if raw.trusted_scan_value_generation != 0 {
        config = config.with_trusted_scan_value_generation(true);
    }
    if raw.bounded_atomic_values != 0 {
        config = config.with_bounded_atomic_values(true);
    }
    config
}

unsafe fn optional_copy<T: Copy + Default>(pointer: *const T) -> T {
    if pointer.is_null() {
        T::default()
    } else {
        // SAFETY: A non-null optional config obeys the same validity contract
        // as required config pointers.
        unsafe { *pointer }
    }
}

/// # Safety
/// `out_db` must be uniquely writable. A non-null `config` must be readable.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_db_create(
    config: *const StoTpccDbConfig,
    out_db: *mut *mut StoTpccDb,
) -> i32 {
    boundary("sto_tpcc_db_create", || {
        let output = unsafe { required_mut(out_db, "out_db")? };
        *output = ptr::null_mut();
        let raw = unsafe { optional_copy(config) };
        let (sto_config, masstree_config) = db_config(raw);
        let max_pending_size_deltas = sto_config.max_items_per_transaction();
        let masstree = MasstreeRuntime::new(masstree_config)
            .map_err(|error| fatal(format_args!("unable to create Masstree runtime: {error}")))?;
        let sto = Runtime::new(sto_config)
            .map_err(|error| fatal(format_args!("unable to create STO runtime: {error}")))?;
        *output = Box::into_raw(Box::new(StoTpccDb {
            sto,
            masstree,
            max_pending_size_deltas,
        }));
        Ok(Status::Ok)
    })
}

/// # Safety
/// `db` must be null or a live handle returned by `sto_tpcc_db_create`, and it
/// must be destroyed exactly once.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_db_destroy(db: *mut StoTpccDb) -> i32 {
    boundary("sto_tpcc_db_destroy", || {
        if !db.is_null() {
            // SAFETY: Ownership of this allocation is returned exactly once by
            // the caller under the destruction contract.
            drop(unsafe { Box::from_raw(db) });
        }
        Ok(Status::Ok)
    })
}

unsafe fn create_table(
    db: *mut StoTpccDb,
    config: *const StoTpccTableConfig,
    cache_policy: StoTpccResolvedCachePolicy,
    out_table: *mut *mut StoTpccTable,
) -> FfiResult<Status> {
    let db = unsafe { required_ref(db, "db")? };
    let output = unsafe { required_mut(out_table, "out_table")? };
    *output = ptr::null_mut();
    let (cache_policy, dense_policy) = ResolvedCachePolicy::from_raw(cache_policy)?;
    let raw = unsafe { optional_copy(config) };
    let worker = db.masstree.attach().map_err(|error| {
        fatal(format_args!(
            "unable to attach Masstree table creator: {error}"
        ))
    })?;
    let table = Table::new_direct(&db.sto, &db.masstree, &worker, table_config(raw))
        .map_err(|error| fatal(format_args!("unable to create STO table: {error}")))?;
    drop(worker);
    let dense_cache = match dense_policy {
        DenseCachePolicy::Item | DenseCachePolicy::Stock => Some(
            table
                .dense_resolved_cache(DENSE_TPCC_ITEM_SLOTS)
                .map_err(|error| {
                    fatal(format_args!(
                        "unable to create dense TPC-C resolved cache: {error}"
                    ))
                })?,
        ),
        DenseCachePolicy::None => None,
    };
    *output = Box::into_raw(Box::new(StoTpccTable {
        state: Arc::new(TableState {
            table,
            logical_rows: AtomicU64::new(0),
            runtime_id: db.sto.id(),
        }),
        cache_policy,
        dense_policy,
        dense_cache,
        dense_stock_warehouse_id: AtomicI32::new(0),
    }));
    Ok(Status::Ok)
}

/// # Safety
/// Handles must be live, and `out_table` must be uniquely writable. Create
/// tables before attaching a transaction handle on this OS thread. This
/// compatibility creator always selects the Full resolved-record cache.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_table_create(
    db: *mut StoTpccDb,
    config: *const StoTpccTableConfig,
    out_table: *mut *mut StoTpccTable,
) -> i32 {
    boundary("sto_tpcc_table_create", || unsafe {
        create_table(db, config, STO_TPCC_RESOLVED_CACHE_FULL, out_table)
    })
}

/// # Safety
/// Handles must be live, and `out_table` must be uniquely writable. Create
/// tables before attaching a transaction handle on this OS thread. The policy
/// must be one of the `STO_TPCC_RESOLVED_CACHE_*` constants.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_table_create_with_cache_policy(
    db: *mut StoTpccDb,
    config: *const StoTpccTableConfig,
    cache_policy: StoTpccResolvedCachePolicy,
    out_table: *mut *mut StoTpccTable,
) -> i32 {
    boundary("sto_tpcc_table_create_with_cache_policy", || unsafe {
        create_table(db, config, cache_policy, out_table)
    })
}

/// Permanently closes this table's native directory to new keys.
///
/// Existing records remain readable and writable. Call this only after all
/// loader transactions have finished and while no transaction uses `table`.
///
/// # Safety
/// `table` must be a live table handle with no concurrent users.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_table_seal_directory_structure(table: *mut StoTpccTable) -> i32 {
    boundary("sto_tpcc_table_seal_directory_structure", || {
        let table = unsafe { required_ref(table, "table")? };
        match table.state.table.seal_directory_structure() {
            Ok(()) => Ok(Status::Ok),
            Err(error) => Ok(status_from_access("seal directory structure", error)),
        }
    })
}

/// # Safety
/// `table` must be null or a live table handle and must be destroyed once. No
/// transaction may concurrently use it.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_table_destroy(table: *mut StoTpccTable) -> i32 {
    boundary("sto_tpcc_table_destroy", || {
        if !table.is_null() {
            // SAFETY: Ownership is returned exactly once by contract.
            drop(unsafe { Box::from_raw(table) });
        }
        Ok(Status::Ok)
    })
}

/// # Safety
/// `table` must be live and `out_rows` uniquely writable.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_table_size(
    table: *const StoTpccTable,
    out_rows: *mut u64,
) -> i32 {
    boundary("sto_tpcc_table_size", || {
        let table = unsafe { required_ref(table, "table")? };
        let output = unsafe { required_mut(out_rows, "out_rows")? };
        *output = table.state.logical_rows.load(Ordering::Relaxed);
        Ok(Status::Ok)
    })
}

/// # Safety
/// `db` must be live and `out_thread` uniquely writable. The returned handle
/// must remain on this OS thread.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_thread_create(
    db: *mut StoTpccDb,
    out_thread: *mut *mut StoTpccThread,
) -> i32 {
    boundary("sto_tpcc_thread_create", || {
        let db = unsafe { required_ref(db, "db")? };
        let output = unsafe { required_mut(out_thread, "out_thread")? };
        *output = ptr::null_mut();
        // A transaction cannot update the logical size of more distinct
        // tables than it contains logical items. Reserve that runtime bound
        // once so size accounting never allocates after a mutation is staged.
        let mut pending_size = Vec::new();
        pending_size
            .try_reserve_exact(db.max_pending_size_deltas)
            .map_err(|_| {
                fatal(format_args!(
                    "unable to reserve transactional table-size accounting"
                ))
            })?;
        let native_worker = db
            .masstree
            .attach()
            .map_err(|error| fatal(format_args!("unable to attach Masstree worker: {error}")))?;
        let sto_worker = db
            .sto
            .attach()
            .map_err(|error| fatal(format_args!("unable to attach STO worker: {error}")))?;
        *output = Box::into_raw(Box::new(StoTpccThread {
            active: None,
            sto_worker: Box::new(sto_worker),
            native_worker: Box::new(native_worker),
            owner_cookie: allocate_current_thread_cookie()?,
            pending_size,
            resolved_cache: ResolvedCache::default(),
            scan_scratch: ScanScratch::default(),
            point_batch: PointReadBatch::with_capacity(300),
        }));
        Ok(Status::Ok)
    })
}

/// # Safety
/// `thread_handle` must be null or a live handle, on its creating OS thread,
/// and must be destroyed exactly once. An active transaction is aborted.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_thread_destroy(thread_handle: *mut StoTpccThread) -> i32 {
    boundary("sto_tpcc_thread_destroy", || {
        if thread_handle.is_null() {
            return Ok(Status::Ok);
        }
        // Check affinity before reclaiming the allocation: dropping either
        // worker on the wrong thread would violate its native contract.
        let handle = unsafe { required_ref(thread_handle, "thread")? };
        handle.ensure_owner()?;
        // SAFETY: After the affinity check, ownership is returned once by the
        // caller and Drop aborts any active transaction before its worker.
        drop(unsafe { Box::from_raw(thread_handle) });
        Ok(Status::Ok)
    })
}

/// # Safety
/// `thread_handle` must be a live, exclusively accessed, same-thread handle.
#[inline(always)]
fn txn_begin_impl(handle: &mut StoTpccThread) -> FfiResult<Status> {
    if handle.active.is_some() {
        return Err(fatal(format_args!("a transaction is already active")));
    }
    handle.pending_size.clear();
    let rcu_scope = handle
        .native_worker
        .rcu_scope()
        .map_err(|error| fatal(format_args!("unable to begin native RCU scope: {error}")))?;
    let transaction = handle
        .sto_worker
        .begin()
        .map_err(|error| fatal(format_args!("unable to begin transaction: {error}")))?;
    // SAFETY: Both workers are boxed, so moving the outer handle never moves
    // either borrowed pointee. `active` is consumed or dropped before those
    // boxes in every path. Both guards are same-thread capabilities.
    let transaction =
        unsafe { mem::transmute::<Transaction<'_, Active>, ActiveTransaction>(transaction) };
    let rcu_scope = unsafe { mem::transmute::<RcuScope<'_>, ActiveRcuScope>(rcu_scope) };
    handle.active = Some(ActiveAttempt {
        transaction,
        rcu_scope,
    });
    Ok(Status::Ok)
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_txn_begin(thread_handle: *mut StoTpccThread) -> i32 {
    boundary("sto_tpcc_txn_begin", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        txn_begin_impl(handle)
    })
}

/// Wrapper-private transaction begin. See `mako_sto_tpcc_get_trusted` for the
/// trusted handle contract; the operation still contains any Rust panic at
/// the ABI boundary.
///
/// # Safety
/// `thread_handle` must be the live, exclusively accessed, same-thread handle
/// owned by `rust_sto_tpcc_wrapper`.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_txn_begin_trusted(thread_handle: *mut StoTpccThread) -> i32 {
    boundary("mako_sto_tpcc_txn_begin_trusted", || {
        // SAFETY: The private C++ wrapper guarantees a live, same-thread,
        // exclusively accessed handle.
        txn_begin_impl(unsafe { &mut *thread_handle })
    })
}

/// # Safety
/// `thread_handle` must be a live, exclusively accessed, same-thread handle.
#[inline(always)]
fn txn_commit_impl(handle: &mut StoTpccThread) -> FfiResult<Status> {
    let ActiveAttempt {
        transaction,
        rcu_scope,
    } = handle
        .active
        .take()
        .ok_or_else(|| fatal(format_args!("no transaction is active")))?;
    let outcome = transaction.commit();
    let scope_result = rcu_scope
        .close()
        .map_err(|error| fatal(format_args!("unable to end native RCU scope: {error}")));
    match outcome {
        Ok(CommitOutcome::Committed(_)) => {
            let size_result = handle.apply_size_deltas();
            handle.pending_size.clear();
            size_result?;
            scope_result?;
            Ok(Status::Ok)
        }
        Ok(CommitOutcome::Aborted(reason)) => {
            handle.pending_size.clear();
            scope_result?;
            Ok(status_from_abort(reason))
        }
        Err(error @ CommitFailure::Poisoned { outcome, .. }) => {
            if matches!(outcome, DefiniteOutcome::Committed(_)) {
                let _ = handle.apply_size_deltas();
            }
            handle.pending_size.clear();
            scope_result?;
            Err(fatal(format_args!("transaction commit failed: {error}")))
        }
        Err(error @ CommitFailure::Indeterminate(_)) => {
            handle.pending_size.clear();
            scope_result?;
            Err(fatal(format_args!(
                "transaction outcome is indeterminate: {error}"
            )))
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_txn_commit(thread_handle: *mut StoTpccThread) -> i32 {
    boundary("sto_tpcc_txn_commit", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        txn_commit_impl(handle)
    })
}

/// Wrapper-private transaction commit. It skips only redundant handle and
/// affinity checks and retains the complete commit protocol and panic boundary.
///
/// # Safety
/// `thread_handle` must meet the private handle contract above and the wrapper
/// must have a matching active transaction.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_txn_commit_trusted(
    thread_handle: *mut StoTpccThread,
) -> i32 {
    boundary("mako_sto_tpcc_txn_commit_trusted", || {
        // SAFETY: Guaranteed by the wrapper-private contract.
        txn_commit_impl(unsafe { &mut *thread_handle })
    })
}

/// # Safety
/// `thread_handle` must be a live, exclusively accessed, same-thread handle.
/// The operation is intentionally idempotent when no transaction is active.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_txn_abort(thread_handle: *mut StoTpccThread) -> i32 {
    boundary("sto_tpcc_txn_abort", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        if let Some(ActiveAttempt {
            transaction,
            rcu_scope,
        }) = handle.active.take()
        {
            let _ = transaction.abort();
            rcu_scope
                .close()
                .map_err(|error| fatal(format_args!("unable to end native RCU scope: {error}")))?;
        }
        handle.pending_size.clear();
        Ok(Status::Ok)
    })
}

#[inline(always)]
fn get_impl(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    key: &[u8],
    output: &mut [u8],
    actual: &mut usize,
) -> FfiResult<Status> {
    *actual = 0;
    let (cached, full_miss_slot) = match table.cache_policy {
        ResolvedCachePolicy::Full => {
            let probe = handle.resolved_cache.probe(&table.state.table, key);
            (probe.record, probe.miss_slot)
        }
        ResolvedCachePolicy::LastOnly => (
            handle
                .resolved_cache
                .matching_last_only(&table.state.table, key),
            None,
        ),
        ResolvedCachePolicy::ReadThenWrite | ResolvedCachePolicy::None => (None, None),
    };
    let native_worker = &handle.native_worker;
    let transaction = active_transaction(&mut handle.active)?;
    let access = match cached {
        Some(resolved) => table
            .state
            .table
            .copy_get_resolved(transaction, resolved, output)
            .map(|outcome| (outcome, None)),
        None => table
            .state
            .table
            .copy_get_resolving(transaction, native_worker, key, output)
            .map(|(outcome, resolved)| (outcome, Some(resolved))),
    };
    match access {
        Ok((outcome, resolved)) => {
            let status = match outcome {
                ValueCopyOutcome::Miss => Status::Miss,
                ValueCopyOutcome::Copied { len } => {
                    *actual = len;
                    Status::Ok
                }
                ValueCopyOutcome::BufferTooSmall { required } => {
                    *actual = required;
                    set_last_error(format_args!(
                        "value buffer is too small: need {}, have {}",
                        required,
                        output.len()
                    ));
                    Status::BufferTooSmall
                }
            };
            let Some(resolved) = resolved else {
                return Ok(status);
            };
            match table.cache_policy {
                ResolvedCachePolicy::Full => {
                    let cache_probe = ResolvedCacheProbe {
                        record: None,
                        miss_slot: full_miss_slot,
                    };
                    handle.resolved_cache.remember_after_probe(
                        &table.state.table,
                        key,
                        resolved,
                        cache_probe,
                    );
                }
                ResolvedCachePolicy::LastOnly | ResolvedCachePolicy::ReadThenWrite => {
                    handle
                        .resolved_cache
                        .remember_last_only(&table.state.table, key, resolved);
                }
                ResolvedCachePolicy::None => {}
            }
            Ok(status)
        }
        Err(error) => Ok(status_from_access("get", error)),
    }
}

/// # Safety
/// Handles and byte ranges must be valid for the call. `out_actual` is
/// required; `out_value` must cover `value_capacity` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_get(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
    out_value: *mut u8,
    value_capacity: usize,
    out_actual: *mut usize,
) -> i32 {
    boundary("sto_tpcc_get", || {
        // Validate and initialize the required scalar output before every
        // other fallible public-ABI check. This preserves the header contract
        // even when a handle or byte range is rejected below.
        let actual = unsafe { required_mut(out_actual, "out_actual")? };
        *actual = 0;
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let key = unsafe { bytes(key, key_length, "key")? };
        let output = unsafe { output_bytes(out_value, value_capacity, "out_value")? };
        get_impl(handle, table, key, output, actual)
    })
}

/// Wrapper-private counterpart of `sto_tpcc_get`.
///
/// This symbol is intentionally absent from the public C header. It retains
/// the Rust panic boundary but relies on `rust_sto_tpcc_wrapper` to provide a
/// live same-thread handle with an active transaction, one of its live table
/// handles, valid byte ranges, and a uniquely writable result.
///
/// # Safety
/// Every handle, range, affinity, activity, and exclusivity condition in the
/// private contract above must hold for the complete call.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_get_trusted(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
    out_value: *mut u8,
    value_capacity: usize,
    out_actual: *mut usize,
) -> i32 {
    boundary("mako_sto_tpcc_get_trusted", || {
        // SAFETY: Every reference and slice precondition is part of the
        // wrapper-private contract above. Empty ranges avoid imposing Rust's
        // non-null requirement on a C++ zero-length buffer.
        let handle = unsafe { &mut *thread_handle };
        let table = unsafe { &*table };
        let key = if key_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(key, key_length) }
        };
        let output = if value_capacity == 0 {
            &mut [][..]
        } else {
            unsafe { slice::from_raw_parts_mut(out_value, value_capacity) }
        };
        let actual = unsafe { &mut *out_actual };
        get_impl(handle, table, key, output, actual)
    })
}

fn visit_fixed_width<const KEY_LENGTH: usize>(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    keys: &[[u8; KEY_LENGTH]],
    callback: StoTpccFixedReadCallback,
    callback_context: *mut std::ffi::c_void,
    visited: &mut usize,
) -> FfiResult<Status> {
    let mut callback_failed = false;
    let mut visit = |index: usize, current: Option<&[u8]>| {
        if callback_failed {
            return;
        }
        let current_pointer = current.map_or(ptr::null(), <[u8]>::as_ptr);
        let current_length = current.map_or(0, <[u8]>::len);
        // SAFETY: The function pointer is non-null and the value remains
        // leased for this invocation. The public contract forbids pointer
        // retention, re-entry, and unwinding across this frame.
        let result = unsafe { callback(callback_context, index, current_pointer, current_length) };
        *visited += 1;
        if result != 0 {
            callback_failed = true;
            set_last_error(format_args!(
                "fixed-read callback reported failure at input index {index}"
            ));
        }
    };

    let native_worker = &handle.native_worker;
    let point_batch = &mut handle.point_batch;
    let transaction = active_transaction(&mut handle.active)?;
    let mut session = table.state.table.point_session(transaction, native_worker);
    let access = session.visit_fixed_bytes(keys, point_batch, &mut visit);
    drop(session);

    if callback_failed {
        abort_active_attempt_after_fatal(handle)?;
        return Ok(Status::Fatal);
    }
    match access {
        Ok(count) => {
            debug_assert_eq!(count, keys.len());
            Ok(Status::Ok)
        }
        Err(error) => Ok(status_from_access("fixed read", error)),
    }
}

/// Visits transaction-local values for a packed batch of 4-, 8-, 12-, or
/// 16-byte keys.
///
/// # Safety
/// Handles, the packed key range, and `out_visited` must be valid. `callback`
/// must not unwind, retain value pointers, or re-enter an operation on
/// `thread_handle`. A nonzero callback result stops further callback delivery,
/// aborts the active transaction, and returns `FATAL`. After successful
/// table/callback/output validation, `out_visited` counts callbacks that were
/// invoked before any result. The raw callback observes complete stored values;
/// metadata-aware adapters may validate/remove a suffix and apply their own
/// logical-prefix limit.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_visit_fixed(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    keys: *const u8,
    key_count: usize,
    key_width: usize,
    callback: Option<StoTpccFixedReadCallback>,
    callback_context: *mut std::ffi::c_void,
    out_visited: *mut usize,
) -> i32 {
    boundary("sto_tpcc_visit_fixed", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let callback = callback.ok_or_else(|| fatal(format_args!("callback must not be null")))?;
        let visited = unsafe { required_mut(out_visited, "out_visited")? };
        *visited = 0;

        match key_width {
            4 => visit_fixed_width::<4>(
                handle,
                table,
                unsafe { fixed_keys::<4>(keys, key_count)? },
                callback,
                callback_context,
                visited,
            ),
            8 => visit_fixed_width::<8>(
                handle,
                table,
                unsafe { fixed_keys::<8>(keys, key_count)? },
                callback,
                callback_context,
                visited,
            ),
            12 => visit_fixed_width::<12>(
                handle,
                table,
                unsafe { fixed_keys::<12>(keys, key_count)? },
                callback,
                callback_context,
                visited,
            ),
            16 => visit_fixed_width::<16>(
                handle,
                table,
                unsafe { fixed_keys::<16>(keys, key_count)? },
                callback,
                callback_context,
                visited,
            ),
            width => Err(fatal(format_args!(
                "unsupported fixed key width {width}; expected 4, 8, 12, or 16"
            ))),
        }
    })
}

fn modify_fixed_width<const KEY_LENGTH: usize>(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    keys: &[[u8; KEY_LENGTH]],
    callback: StoTpccFixedModifyCallback,
    callback_context: *mut std::ffi::c_void,
    visited: &mut usize,
) -> FfiResult<Status> {
    let mut callback_failed = false;
    let mut visited_count = 0;
    let mut size_delta = 0_i64;
    let mut modify = |index: usize, current: Option<&Value>| {
        if callback_failed {
            return PointMutation::Keep;
        }

        let current_pointer = current.map_or(ptr::null(), |value| value.as_ref().as_ptr());
        let current_length = current.map_or(0, |value| value.as_ref().len());
        let mut replacement_pointer = ptr::null();
        let mut replacement_length = 0;
        // SAFETY: The function pointer is non-null and the current value is
        // leased for this invocation. The public contract forbids retention,
        // re-entry, and unwinding through this Rust frame.
        let action = unsafe {
            callback(
                callback_context,
                index,
                current_pointer,
                current_length,
                &mut replacement_pointer,
                &mut replacement_length,
            )
        };
        visited_count += 1;

        let delta = match action {
            STO_TPCC_FIXED_MODIFY_KEEP => return PointMutation::Keep,
            STO_TPCC_FIXED_MODIFY_PUT => i64::from(current.is_none()),
            STO_TPCC_FIXED_MODIFY_REMOVE => -i64::from(current.is_some()),
            STO_TPCC_FIXED_MODIFY_FAILED => {
                callback_failed = true;
                set_last_error(format_args!(
                    "fixed-mutation callback reported failure at input index {index}"
                ));
                return PointMutation::Keep;
            }
            unknown => {
                callback_failed = true;
                set_last_error(format_args!(
                    "fixed-mutation callback returned unknown action {unknown} at input index {index}"
                ));
                return PointMutation::Keep;
            }
        };
        let Some(next_delta) = size_delta.checked_add(delta) else {
            callback_failed = true;
            set_last_error(format_args!(
                "fixed-mutation table-size delta overflow at input index {index}"
            ));
            return PointMutation::Keep;
        };

        match action {
            STO_TPCC_FIXED_MODIFY_PUT => {
                let valid_length = replacement_length <= isize::MAX as usize;
                let valid_pointer = replacement_length == 0 || !replacement_pointer.is_null();
                let valid_range = replacement_pointer
                    .addr()
                    .checked_add(replacement_length)
                    .is_some();
                if !valid_length || !valid_pointer || !valid_range {
                    callback_failed = true;
                    set_last_error(format_args!(
                        "fixed-mutation callback returned an invalid replacement at input index {index}"
                    ));
                    return PointMutation::Keep;
                }
                let replacement = if replacement_length == 0 {
                    &[][..]
                } else {
                    // SAFETY: The callback contract keeps this byte range
                    // readable until it is copied here. Length, nullness, and
                    // address overflow were checked above.
                    unsafe { slice::from_raw_parts(replacement_pointer, replacement_length) }
                };
                size_delta = next_delta;
                PointMutation::Put(Value::from(replacement))
            }
            STO_TPCC_FIXED_MODIFY_REMOVE => {
                size_delta = next_delta;
                PointMutation::Remove
            }
            _ => unreachable!("fixed mutation action was validated above"),
        }
    };

    let native_worker = &handle.native_worker;
    let point_batch = &mut handle.point_batch;
    let transaction = active_transaction(&mut handle.active)?;
    let mut session = table.state.table.point_session(transaction, native_worker);
    let access = session.modify_fixed_visit(keys, point_batch, &mut modify);
    drop(session);
    *visited = visited_count;

    if callback_failed {
        abort_active_attempt_after_fatal(handle)?;
        return Ok(Status::Fatal);
    }
    match access {
        Ok(count) => {
            debug_assert_eq!(count, keys.len());
            if size_delta != 0 {
                record_size_delta_after_staging(handle, &table.state, size_delta)?;
            }
            Ok(Status::Ok)
        }
        Err(error) => Ok(status_from_access("fixed mutation", error)),
    }
}

/// Applies callback-selected mutations to packed 4-, 8-, 12-, or 16-byte keys.
///
/// # Safety
/// Handles, the packed key range, and `out_visited` must be valid and mutually
/// non-aliasing. `callback` must not unwind, retain borrowed pointers, mutate
/// the packed keys, or re-enter an operation on `thread_handle`. PUT bytes are
/// copied synchronously. Callback failure, an unknown action, or an invalid
/// replacement aborts the active transaction and returns `FATAL`.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_modify_fixed(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    keys: *const u8,
    key_count: usize,
    key_width: usize,
    callback: Option<StoTpccFixedModifyCallback>,
    callback_context: *mut std::ffi::c_void,
    out_visited: *mut usize,
) -> i32 {
    boundary("sto_tpcc_modify_fixed", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let callback = callback.ok_or_else(|| fatal(format_args!("callback must not be null")))?;
        let visited = unsafe { required_mut(out_visited, "out_visited")? };

        match key_width {
            4 => {
                let keys = unsafe { fixed_keys::<4>(keys, key_count)? };
                *visited = 0;
                modify_fixed_width(handle, table, keys, callback, callback_context, visited)
            }
            8 => {
                let keys = unsafe { fixed_keys::<8>(keys, key_count)? };
                *visited = 0;
                modify_fixed_width(handle, table, keys, callback, callback_context, visited)
            }
            12 => {
                let keys = unsafe { fixed_keys::<12>(keys, key_count)? };
                *visited = 0;
                modify_fixed_width(handle, table, keys, callback, callback_context, visited)
            }
            16 => {
                let keys = unsafe { fixed_keys::<16>(keys, key_count)? };
                *visited = 0;
                modify_fixed_width(handle, table, keys, callback, callback_context, visited)
            }
            width => Err(fatal(format_args!(
                "unsupported fixed key width {width}; expected 4, 8, 12, or 16"
            ))),
        }
    })
}

fn put_fixed_width<const KEY_LENGTH: usize>(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    keys: &[[u8; KEY_LENGTH]],
    values: &[StoTpccFixedValue],
    mode: FixedPutMode,
    output: &mut StoTpccFixedPutResult,
) -> FfiResult<Status> {
    let mut inserted = 0_i64;
    let mut first_duplicate = usize::MAX;
    let mut accounting_failed = false;
    let mut put = |index: usize, current: Option<&Value>| {
        if accounting_failed {
            return PointMutation::Keep;
        }
        if current.is_some() && mode == FixedPutMode::Insert {
            if first_duplicate == usize::MAX {
                first_duplicate = index;
            }
            return PointMutation::Keep;
        }
        if current.is_none() {
            let Some(next) = inserted.checked_add(1) else {
                accounting_failed = true;
                set_last_error(format_args!(
                    "fixed put inserted-row accounting overflows at input index {index}"
                ));
                return PointMutation::Keep;
            };
            inserted = next;
        }
        let value = values[index];
        let bytes = if value.length == 0 {
            &[][..]
        } else {
            // SAFETY: The complete descriptor array was validated before the
            // transaction began, and the C contract keeps every range live
            // and immutable for this synchronous call. `Value::from` copies
            // the bytes before this closure returns.
            unsafe { slice::from_raw_parts(value.data, value.length) }
        };
        PointMutation::Put(Value::from(bytes))
    };

    let native_worker = &handle.native_worker;
    let point_batch = &mut handle.point_batch;
    let transaction = active_transaction(&mut handle.active)?;
    let mut session = table.state.table.point_session(transaction, native_worker);
    let access = match mode {
        FixedPutMode::Upsert => session.modify_fixed_visit(keys, point_batch, &mut put),
        FixedPutMode::Insert => {
            session.modify_fixed_expected_absent_visit(keys, point_batch, &mut put)
        }
    };
    drop(session);

    if accounting_failed {
        abort_active_attempt_after_fatal(handle)?;
        return Ok(Status::Fatal);
    }
    match access {
        Ok(count) => {
            debug_assert_eq!(count, keys.len());
            if inserted != 0 {
                if let Err(status) = handle.record_size_delta(&table.state, inserted) {
                    // Committing after an accounting failure would make the
                    // externally visible row count inconsistent with the
                    // table. Resolve the attempt before returning the error.
                    abort_active_attempt_after_fatal(handle)?;
                    return Err(status);
                }
            }
            *output = StoTpccFixedPutResult {
                inserted: usize::try_from(inserted)
                    .expect("a nonnegative i64 inserted count fits usize"),
                first_duplicate,
            };
            if mode == FixedPutMode::Insert && first_duplicate != usize::MAX {
                Ok(Status::Duplicate)
            } else {
                Ok(Status::Ok)
            }
        }
        Err(error) => Ok(status_from_access("fixed put", error)),
    }
}

/// Stages packed fixed-width values without a per-row foreign callback.
///
/// # Safety
/// Handles, packed keys, all `key_count` value descriptors and byte ranges,
/// and `out_result` must be valid and mutually non-aliasing for this
/// synchronous call.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_put_fixed(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    keys: *const u8,
    key_count: usize,
    key_width: usize,
    values: *const StoTpccFixedValue,
    mode: StoTpccFixedPutMode,
    out_result: *mut StoTpccFixedPutResult,
) -> i32 {
    boundary("sto_tpcc_put_fixed", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let mode = FixedPutMode::from_raw(mode)?;
        let values = unsafe { fixed_values(values, key_count)? };

        macro_rules! dispatch {
            ($width:literal) => {{
                let keys = unsafe { fixed_keys::<$width>(keys, key_count)? };
                let output = unsafe { required_mut(out_result, "out_result")? };
                *output = StoTpccFixedPutResult::default();
                put_fixed_width(handle, table, keys, values, mode, output)
            }};
        }

        match key_width {
            4 => dispatch!(4),
            8 => dispatch!(8),
            12 => dispatch!(12),
            16 => dispatch!(16),
            width => Err(fatal(format_args!(
                "unsupported fixed key width {width}; expected 4, 8, 12, or 16"
            ))),
        }
    })
}

/// # Safety
/// Handles and both input byte ranges must be valid for the call.
#[inline(always)]
fn put_impl(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    key: &[u8],
    value: &[u8],
) -> FfiResult<Status> {
    let cached = match table.cache_policy {
        ResolvedCachePolicy::Full => handle.resolved_cache.matching(&table.state.table, key),
        ResolvedCachePolicy::LastOnly | ResolvedCachePolicy::ReadThenWrite => handle
            .resolved_cache
            .matching_last_only(&table.state.table, key),
        ResolvedCachePolicy::None => None,
    };
    let native_worker = &handle.native_worker;
    let transaction = active_transaction(&mut handle.active)?;
    let access = match cached {
        Some(resolved) => {
            table
                .state
                .table
                .put_resolved_with_previous_presence(transaction, resolved, value)
        }
        None => {
            table
                .state
                .table
                .put_with_previous_presence(transaction, native_worker, key, value)
        }
    };
    match access {
        Ok(previous_present) => {
            if !previous_present {
                record_size_delta_after_staging(handle, &table.state, 1)?;
            }
            Ok(Status::Ok)
        }
        Err(error) => Ok(status_from_access("put", error)),
    }
}

/// Wrapper-private PUT that retains eligible value bytes through transaction
/// finish instead of copying them into an owned intent.
///
/// # Safety
///
/// `value` must remain readable and immutable until the active transaction
/// commits or aborts. All ordinary trusted-handle preconditions also apply.
#[allow(
    unsafe_code,
    reason = "this private fast lane forwards the wrapper's transaction-lifetime guarantee"
)]
#[inline(always)]
unsafe fn put_borrowed_impl(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    key: &[u8],
    value: &[u8],
) -> FfiResult<Status> {
    let cached = match table.cache_policy {
        ResolvedCachePolicy::Full => handle.resolved_cache.matching(&table.state.table, key),
        ResolvedCachePolicy::LastOnly | ResolvedCachePolicy::ReadThenWrite => handle
            .resolved_cache
            .matching_last_only(&table.state.table, key),
        ResolvedCachePolicy::None => None,
    };
    let native_worker = &handle.native_worker;
    let transaction = active_transaction(&mut handle.active)?;
    let access = match cached {
        Some(resolved) => {
            // SAFETY: Forwarded from this function's value-lifetime contract.
            unsafe {
                table
                    .state
                    .table
                    .put_resolved_borrowed_with_previous_presence(transaction, resolved, value)
            }
        }
        None => {
            // SAFETY: Forwarded from this function's value-lifetime contract.
            unsafe {
                table.state.table.put_borrowed_with_previous_presence(
                    transaction,
                    native_worker,
                    key,
                    value,
                )
            }
        }
    };
    match access {
        Ok(previous_present) => {
            if !previous_present {
                record_size_delta_after_staging(handle, &table.state, 1)?;
            }
            Ok(Status::Ok)
        }
        Err(error) => Ok(status_from_access("borrowed put", error)),
    }
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_put(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32 {
    boundary("sto_tpcc_put", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let key = unsafe { bytes(key, key_length, "key")? };
        let value = unsafe { bytes(value, value_length, "value")? };
        put_impl(handle, table, key, value)
    })
}

/// Wrapper-private PUT with the trusted-handle and byte-range contract
/// documented by `mako_sto_tpcc_get_trusted`.
///
/// # Safety
/// Every handle, range, affinity, activity, and exclusivity condition in that
/// private contract must hold for the complete call.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_put_trusted(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32 {
    boundary("mako_sto_tpcc_put_trusted", || {
        // SAFETY: Guaranteed by the private C++ wrapper. Its supported TPC-C
        // values and keys are nonempty, but retain empty-slice support for the
        // general abstract index surface.
        let handle = unsafe { &mut *thread_handle };
        let table = unsafe { &*table };
        let key = if key_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(key, key_length) }
        };
        let value = if value_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(value, value_length) }
        };
        put_impl(handle, table, key, value)
    })
}

/// Wrapper-private PUT for the transactional ordered-index contract, whose
/// encoded value remains immutable through commit or abort.
///
/// # Safety
/// Every trusted PUT precondition must hold. In addition, the value range must
/// remain readable and immutable until the active transaction finishes.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_put_borrowed_trusted(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32 {
    boundary("mako_sto_tpcc_put_borrowed_trusted", || {
        // SAFETY: The private C++ wrapper guarantees live handles and ranges.
        let handle = unsafe { &mut *thread_handle };
        let table = unsafe { &*table };
        let key = if key_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(key, key_length) }
        };
        let value = if value_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(value, value_length) }
        };
        // SAFETY: The wrapper's transactional value contract keeps this range
        // readable and immutable until the active attempt resolves.
        unsafe { put_borrowed_impl(handle, table, key, value) }
    })
}

/// # Safety
/// Handles and both input byte ranges must be valid for the call.
#[inline(always)]
fn insert_impl(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    key: &[u8],
    value: &[u8],
) -> FfiResult<Status> {
    let native_worker = &handle.native_worker;
    let transaction = active_transaction(&mut handle.active)?;
    match table
        .state
        .table
        .insert_expected_absent(transaction, native_worker, key, value)
    {
        Ok(true) => {
            record_size_delta_after_staging(handle, &table.state, 1)?;
            Ok(Status::Ok)
        }
        Ok(false) => Ok(Status::Duplicate),
        Err(error) => Ok(status_from_access("insert", error)),
    }
}

/// Borrowing counterpart to [`insert_impl`] for the wrapper-private
/// transaction-lifetime contract.
///
/// # Safety
///
/// `value` must remain readable and immutable until the active transaction
/// commits or aborts.
#[allow(
    unsafe_code,
    reason = "this private fast lane forwards the wrapper's transaction-lifetime guarantee"
)]
#[inline(always)]
unsafe fn insert_borrowed_impl(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    key: &[u8],
    value: &[u8],
) -> FfiResult<Status> {
    let native_worker = &handle.native_worker;
    let transaction = active_transaction(&mut handle.active)?;
    // SAFETY: Forwarded from this function's value-lifetime contract.
    match unsafe {
        table
            .state
            .table
            .insert_expected_absent_borrowed(transaction, native_worker, key, value)
    } {
        Ok(true) => {
            record_size_delta_after_staging(handle, &table.state, 1)?;
            Ok(Status::Ok)
        }
        Ok(false) => Ok(Status::Duplicate),
        Err(error) => Ok(status_from_access("borrowed insert", error)),
    }
}

#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_insert(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32 {
    boundary("sto_tpcc_insert", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let key = unsafe { bytes(key, key_length, "key")? };
        let value = unsafe { bytes(value, value_length, "value")? };
        insert_impl(handle, table, key, value)
    })
}

/// Wrapper-private INSERT with the same preconditions as the trusted PUT.
///
/// # Safety
/// Every trusted PUT precondition must hold for this call.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_insert_trusted(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32 {
    boundary("mako_sto_tpcc_insert_trusted", || {
        // SAFETY: Guaranteed by the private C++ wrapper.
        let handle = unsafe { &mut *thread_handle };
        let table = unsafe { &*table };
        let key = if key_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(key, key_length) }
        };
        let value = if value_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(value, value_length) }
        };
        insert_impl(handle, table, key, value)
    })
}

/// Wrapper-private INSERT for the transactional ordered-index contract, whose
/// encoded value remains immutable through commit or abort.
///
/// # Safety
/// Every trusted INSERT precondition must hold. In addition, the value range
/// must remain readable and immutable until the active transaction finishes.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_insert_borrowed_trusted(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32 {
    boundary("mako_sto_tpcc_insert_borrowed_trusted", || {
        // SAFETY: The private C++ wrapper guarantees live handles and ranges.
        let handle = unsafe { &mut *thread_handle };
        let table = unsafe { &*table };
        let key = if key_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(key, key_length) }
        };
        let value = if value_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(value, value_length) }
        };
        // SAFETY: The wrapper's transactional value contract keeps this range
        // readable and immutable until the active attempt resolves.
        unsafe { insert_borrowed_impl(handle, table, key, value) }
    })
}

/// Applies a heterogeneous sequence of transaction-local INSERT operations.
///
/// # Safety
/// The thread, descriptor range, every table handle, every byte range, and
/// `out_result` must satisfy the public C header contract for the complete
/// synchronous call.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_insert_many(
    thread_handle: *mut StoTpccThread,
    operations: *const StoTpccInsertOperation,
    operation_count: usize,
    out_result: *mut StoTpccFixedPutResult,
) -> i32 {
    boundary("sto_tpcc_insert_many", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let operations = unsafe { insert_operations(operations, operation_count)? };
        let output = unsafe { required_mut(out_result, "out_result")? };
        *output = StoTpccFixedPutResult::default();
        // Empty batches remain transactional operations and reject an inactive
        // handle exactly like every other mutation surface.
        let _ = active_transaction(&mut handle.active)?;

        for (index, operation) in operations.iter().enumerate() {
            // SAFETY: insert_operations validated a non-null live table handle;
            // its liveness is part of the opaque-handle C contract.
            let table = unsafe { &*operation.table };
            // SAFETY: Every byte range was validated before the first mutation
            // and remains immutable for this synchronous call.
            let key = if operation.key_length == 0 {
                &[][..]
            } else {
                // SAFETY: The complete descriptor set was validated before
                // entering this loop and remains immutable for the call.
                unsafe { slice::from_raw_parts(operation.key, operation.key_length) }
            };
            let value = if operation.value_length == 0 {
                &[][..]
            } else {
                // SAFETY: The same prevalidation covers this value range.
                unsafe { slice::from_raw_parts(operation.value, operation.value_length) }
            };
            let access = {
                let native_worker = &handle.native_worker;
                let transaction = active_transaction(&mut handle.active)?;
                table
                    .state
                    .table
                    .insert_expected_absent(transaction, native_worker, key, value)
            };
            match access {
                Ok(true) => {
                    let Some(inserted) = output.inserted.checked_add(1) else {
                        let error = fatal(format_args!("insert-many inserted-row count overflows"));
                        abort_active_attempt_after_fatal(handle)?;
                        return Err(error);
                    };
                    output.inserted = inserted;
                    record_size_delta_after_staging(handle, &table.state, 1)?;
                }
                Ok(false) => {
                    if output.first_duplicate == usize::MAX {
                        output.first_duplicate = index;
                    }
                }
                Err(error) => return Ok(status_from_access("insert many", error)),
            }
        }

        Ok(if output.first_duplicate == usize::MAX {
            Status::Ok
        } else {
            Status::Duplicate
        })
    })
}

/// # Safety
/// Handles and the key byte range must be valid for the call.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_remove(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    key: *const u8,
    key_length: usize,
) -> i32 {
    boundary("sto_tpcc_remove", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let key = unsafe { bytes(key, key_length, "key")? };
        let cached = match table.cache_policy {
            ResolvedCachePolicy::Full => handle.resolved_cache.matching(&table.state.table, key),
            ResolvedCachePolicy::LastOnly | ResolvedCachePolicy::ReadThenWrite => handle
                .resolved_cache
                .matching_last_only(&table.state.table, key),
            ResolvedCachePolicy::None => None,
        };
        let native_worker = &handle.native_worker;
        let transaction = active_transaction(&mut handle.active)?;
        let access = match cached {
            Some(resolved) => table
                .state
                .table
                .remove_resolved_with_previous_presence(transaction, resolved),
            None => {
                table
                    .state
                    .table
                    .remove_with_previous_presence(transaction, native_worker, key)
            }
        };
        match access {
            Ok(true) => {
                record_size_delta_after_staging(handle, &table.state, -1)?;
                Ok(Status::Ok)
            }
            Ok(false) => Ok(Status::Miss),
            Err(error) => Ok(status_from_access("remove", error)),
        }
    })
}

pub type StoTpccScanCallback = unsafe extern "C" fn(
    context: *mut std::ffi::c_void,
    key: *const u8,
    key_length: usize,
    value: *const u8,
    value_length: usize,
) -> i32;

fn scan_bound<'a>(kind: i32, key: &'a [u8], name: &str) -> FfiResult<ScanBound<'a>> {
    match kind {
        0 => Ok(ScanBound::Unbounded),
        1 => Ok(ScanBound::Included(key)),
        2 => Ok(ScanBound::Excluded(key)),
        _ => Err(fatal(format_args!("invalid {name} bound kind {kind}"))),
    }
}

#[allow(clippy::too_many_arguments)]
#[inline(always)]
fn scan_impl(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    direction: ScanDirection,
    lower: ScanBound<'_>,
    upper: ScanBound<'_>,
    limit: usize,
    callback: StoTpccScanCallback,
    callback_context: *mut std::ffi::c_void,
    visited: &mut usize,
) -> FfiResult<Status> {
    let mut visited_count = 0;
    let request = ScanRequest::new(direction, limit)
        .with_lower(lower)
        .with_upper(upper);
    let cache_scan_rows =
        table.cache_policy == ResolvedCachePolicy::Full && limit <= RESOLVED_SCAN_CACHE_LIMIT;
    let native_worker = &handle.native_worker;
    let scratch = &mut handle.scan_scratch;
    let resolved_cache = &mut handle.resolved_cache;
    let result = match active_transaction(&mut handle.active) {
        Ok(transaction) => {
            let visit = |record: ScanBytesRef<'_>| {
                // SAFETY: The callback is a valid function pointer by the
                // ABI. Row slices remain alive for this invocation and
                // the contract forbids retaining them or unwinding through
                // Rust.
                let stop = unsafe {
                    callback(
                        callback_context,
                        record.key().as_ptr(),
                        record.key().len(),
                        record.value().as_ptr(),
                        record.value().len(),
                    )
                };
                visited_count += 1;
                if cache_scan_rows {
                    // Only callback-visible rows from small scans enter the
                    // cache. Exact key and table identity remain part of
                    // every later cache hit.
                    resolved_cache.remember(&table.state.table, record.key(), record.resolved());
                }
                if stop != 0 {
                    ScanControl::Stop
                } else {
                    ScanControl::Continue
                }
            };
            // SAFETY: Both the checked and wrapper-private scan ABIs require a
            // synchronous callback that neither retains row pointers nor
            // re-enters this thread handle. Every FFI table owns the private
            // direct-token directory created in `create_table`.
            let status = match unsafe {
                table.state.table.visit_scan_bytes_trusted_with_scratch(
                    transaction,
                    native_worker,
                    request,
                    scratch,
                    visit,
                )
            } {
                Ok(_) => Status::Ok,
                Err(error) => status_from_access("scan", error),
            };
            Ok(status)
        }
        Err(error) => Err(error),
    };
    *visited = visited_count;
    result
}

/// # Safety
/// All handles/ranges and `out_visited` must be valid. `callback` must not
/// unwind, retain row pointers, or re-enter operations on `thread_handle`.
/// Rows stream after their STO observation; if a later error occurs,
/// `out_visited` still includes earlier callback invocations and their external
/// side effects are not rolled back.
#[allow(clippy::too_many_arguments)]
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_scan(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    direction: i32,
    lower_kind: i32,
    lower_key: *const u8,
    lower_key_length: usize,
    upper_kind: i32,
    upper_key: *const u8,
    upper_key_length: usize,
    limit: usize,
    callback: Option<StoTpccScanCallback>,
    callback_context: *mut std::ffi::c_void,
    out_visited: *mut usize,
) -> i32 {
    boundary("sto_tpcc_scan", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        let table = unsafe { required_ref(table, "table")? };
        let lower_key = if lower_kind == 0 {
            &[][..]
        } else {
            unsafe { bytes(lower_key, lower_key_length, "lower_key")? }
        };
        let upper_key = if upper_kind == 0 {
            &[][..]
        } else {
            unsafe { bytes(upper_key, upper_key_length, "upper_key")? }
        };
        let lower = scan_bound(lower_kind, lower_key, "lower")?;
        let upper = scan_bound(upper_kind, upper_key, "upper")?;
        let direction = match direction {
            0 => ScanDirection::Forward,
            1 => ScanDirection::Reverse,
            other => {
                return Err(fatal(format_args!("invalid scan direction {other}")));
            }
        };
        let callback = callback.ok_or_else(|| fatal(format_args!("callback must not be null")))?;
        let visited = unsafe { required_mut(out_visited, "out_visited")? };
        scan_impl(
            handle,
            table,
            direction,
            lower,
            upper,
            limit,
            callback,
            callback_context,
            visited,
        )
    })
}

/// Wrapper-private scan. The C++ wrapper supplies only the two known
/// directions, three known bound kinds, a non-null callback, live key ranges,
/// and a uniquely writable visit count. The Rust panic boundary is retained.
///
/// # Safety
/// In addition to the trusted handle contract, the enum, callback, range, and
/// output conditions above must hold for the complete synchronous call.
#[allow(clippy::too_many_arguments)]
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_scan_trusted(
    thread_handle: *mut StoTpccThread,
    table: *const StoTpccTable,
    direction: i32,
    lower_kind: i32,
    lower_key: *const u8,
    lower_key_length: usize,
    upper_kind: i32,
    upper_key: *const u8,
    upper_key_length: usize,
    limit: usize,
    callback: Option<StoTpccScanCallback>,
    callback_context: *mut std::ffi::c_void,
    out_visited: *mut usize,
) -> i32 {
    boundary("mako_sto_tpcc_scan_trusted", || {
        // SAFETY: Every raw-pointer and enum precondition below is established
        // by rust_sto_tpcc_wrapper immediately before this private call.
        let handle = unsafe { &mut *thread_handle };
        let table = unsafe { &*table };
        let lower_key = if lower_kind == 0 || lower_key_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(lower_key, lower_key_length) }
        };
        let upper_key = if upper_kind == 0 || upper_key_length == 0 {
            &[][..]
        } else {
            unsafe { slice::from_raw_parts(upper_key, upper_key_length) }
        };
        let lower = match lower_kind {
            0 => ScanBound::Unbounded,
            1 => ScanBound::Included(lower_key),
            2 => ScanBound::Excluded(lower_key),
            other => return Err(fatal(format_args!("invalid trusted lower bound {other}"))),
        };
        let upper = match upper_kind {
            0 => ScanBound::Unbounded,
            1 => ScanBound::Included(upper_key),
            2 => ScanBound::Excluded(upper_key),
            other => return Err(fatal(format_args!("invalid trusted upper bound {other}"))),
        };
        let direction = match direction {
            0 => ScanDirection::Forward,
            1 => ScanDirection::Reverse,
            other => {
                return Err(fatal(format_args!(
                    "invalid trusted scan direction {other}"
                )))
            }
        };
        let callback = callback
            .ok_or_else(|| fatal(format_args!("trusted scan callback must not be null")))?;
        let visited = unsafe { &mut *out_visited };
        scan_impl(
            handle,
            table,
            direction,
            lower,
            upper,
            limit,
            callback,
            callback_context,
            visited,
        )
    })
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum PaymentCodecError {
    Truncated(&'static str),
    InvalidLength(&'static str),
    InvalidVarint(&'static str),
    TrailingBytes,
    NonFinite(&'static str),
    ArithmeticOverflow(&'static str),
    BufferTooSmall { required: usize, available: usize },
}

impl fmt::Display for PaymentCodecError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Truncated(field) => write!(formatter, "truncated {field}"),
            Self::InvalidLength(field) => write!(formatter, "invalid {field} length"),
            Self::InvalidVarint(field) => write!(formatter, "invalid {field} varint"),
            Self::TrailingBytes => formatter.write_str("trailing customer bytes"),
            Self::NonFinite(field) => write!(formatter, "nonfinite {field}"),
            Self::ArithmeticOverflow(field) => write!(formatter, "{field} overflow"),
            Self::BufferTooSmall {
                required,
                available,
            } => write!(
                formatter,
                "replacement needs {required} bytes but output has {available}"
            ),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct CustomerPaymentLayout {
    balance_offset: usize,
    ytd_payment_offset: usize,
    payment_count_start: usize,
    payment_count_end: usize,
    payment_count: i32,
}

#[inline]
fn payment_take(
    bytes: &[u8],
    cursor: &mut usize,
    length: usize,
    field: &'static str,
) -> Result<usize, PaymentCodecError> {
    let start = *cursor;
    let end = start
        .checked_add(length)
        .filter(|end| *end <= bytes.len())
        .ok_or(PaymentCodecError::Truncated(field))?;
    *cursor = end;
    Ok(start)
}

#[inline]
fn payment_read_f32(
    bytes: &[u8],
    cursor: &mut usize,
    field: &'static str,
) -> Result<(usize, f32), PaymentCodecError> {
    let offset = payment_take(bytes, cursor, mem::size_of::<f32>(), field)?;
    let value = f32::from_ne_bytes(
        bytes[offset..offset + mem::size_of::<f32>()]
            .try_into()
            .expect("the checked float range has four bytes"),
    );
    if !value.is_finite() {
        return Err(PaymentCodecError::NonFinite(field));
    }
    Ok((offset, value))
}

#[inline]
fn payment_skip_inline_u8(
    bytes: &[u8],
    cursor: &mut usize,
    maximum: usize,
    field: &'static str,
) -> Result<(), PaymentCodecError> {
    payment_read_inline_u8(bytes, cursor, maximum, field).map(|_| ())
}

#[inline]
fn payment_read_inline_u8<'bytes>(
    bytes: &'bytes [u8],
    cursor: &mut usize,
    maximum: usize,
    field: &'static str,
) -> Result<&'bytes [u8], PaymentCodecError> {
    let length_offset = payment_take(bytes, cursor, 1, field)?;
    let length = usize::from(bytes[length_offset]);
    if length > maximum {
        return Err(PaymentCodecError::InvalidLength(field));
    }
    // inline_str_8<N> derives from a packed base but is serialized through
    // the generic serializer as its complete in-memory representation:
    // one size byte followed by the fixed N+1 byte character array. Bytes
    // beyond `length` are unspecified and must only be preserved.
    let body_offset = payment_take(bytes, cursor, maximum + 1, field)?;
    Ok(&bytes[body_offset..body_offset + length])
}

#[inline]
fn payment_uvarint_length(value: u32) -> usize {
    match value {
        0..=0x7f => 1,
        0x80..=0x3fff => 2,
        0x4000..=0x1f_ffff => 3,
        0x20_0000..=0x0fff_ffff => 4,
        _ => 5,
    }
}

fn payment_decode_uvarint32(
    bytes: &[u8],
    cursor: &mut usize,
    field: &'static str,
) -> Result<(usize, usize, u32), PaymentCodecError> {
    let start = *cursor;
    let mut value = 0_u32;
    for index in 0..5 {
        let offset = payment_take(bytes, cursor, 1, field)?;
        let byte = bytes[offset];
        if index == 4 && byte > 0x0f {
            return Err(PaymentCodecError::InvalidVarint(field));
        }
        value |= u32::from(byte & 0x7f) << (index * 7);
        if byte < 0x80 {
            let end = *cursor;
            if end - start != payment_uvarint_length(value) {
                return Err(PaymentCodecError::InvalidVarint(field));
            }
            return Ok((start, end, value));
        }
    }
    Err(PaymentCodecError::InvalidVarint(field))
}

#[inline]
fn payment_decode_i32(
    bytes: &[u8],
    cursor: &mut usize,
    field: &'static str,
) -> Result<(usize, usize, i32), PaymentCodecError> {
    let (start, end, encoded) = payment_decode_uvarint32(bytes, cursor, field)?;
    let value = ((encoded >> 1) as i32) ^ -((encoded & 1) as i32);
    Ok((start, end, value))
}

fn payment_encode_i32(value: i32, output: &mut [u8; 5]) -> usize {
    let mut encoded = ((value as u32) << 1) ^ ((value >> 31) as u32);
    let mut length = 0;
    while encoded > 0x7f {
        output[length] = (encoded as u8 & 0x7f) | 0x80;
        encoded >>= 7;
        length += 1;
    }
    output[length] = encoded as u8;
    length + 1
}

fn payment_parse_customer(bytes: &[u8]) -> Result<CustomerPaymentLayout, PaymentCodecError> {
    let mut cursor = 0;
    payment_read_f32(bytes, &mut cursor, "customer discount")?;
    payment_take(bytes, &mut cursor, 2, "customer credit")?;
    payment_skip_inline_u8(bytes, &mut cursor, 16, "customer last name")?;
    payment_skip_inline_u8(bytes, &mut cursor, 16, "customer first name")?;
    payment_read_f32(bytes, &mut cursor, "customer credit limit")?;
    let (balance_offset, _) = payment_read_f32(bytes, &mut cursor, "customer balance")?;
    let (ytd_payment_offset, _) = payment_read_f32(bytes, &mut cursor, "customer YTD payment")?;
    let (payment_count_start, payment_count_end, payment_count) =
        payment_decode_i32(bytes, &mut cursor, "customer payment count")?;
    payment_decode_i32(bytes, &mut cursor, "customer delivery count")?;
    payment_skip_inline_u8(bytes, &mut cursor, 20, "customer street 1")?;
    payment_skip_inline_u8(bytes, &mut cursor, 20, "customer street 2")?;
    payment_skip_inline_u8(bytes, &mut cursor, 20, "customer city")?;
    payment_take(bytes, &mut cursor, 2, "customer state")?;
    payment_take(bytes, &mut cursor, 9, "customer ZIP")?;
    payment_take(bytes, &mut cursor, 16, "customer phone")?;
    payment_decode_uvarint32(bytes, &mut cursor, "customer since")?;
    payment_take(bytes, &mut cursor, 2, "customer middle name")?;
    if cursor != bytes.len() {
        return Err(PaymentCodecError::TrailingBytes);
    }
    Ok(CustomerPaymentLayout {
        balance_offset,
        ytd_payment_offset,
        payment_count_start,
        payment_count_end,
        payment_count,
    })
}

fn payment_patch_leading_ytd(
    output: &mut [u8],
    current_len: usize,
    payment_amount: f32,
    field: &'static str,
) -> Result<usize, PaymentCodecError> {
    let bytes = output
        .get(..current_len)
        .ok_or(PaymentCodecError::BufferTooSmall {
            required: current_len,
            available: output.len(),
        })?;
    let mut cursor = 0;
    let (offset, current) = payment_read_f32(bytes, &mut cursor, field)?;
    let replacement = current + payment_amount;
    if !replacement.is_finite() {
        return Err(PaymentCodecError::NonFinite(field));
    }
    output[offset..offset + mem::size_of::<f32>()].copy_from_slice(&replacement.to_ne_bytes());
    Ok(current_len)
}

fn payment_patch_customer(
    output: &mut [u8],
    current_len: usize,
    payment_amount: f32,
) -> Result<usize, PaymentCodecError> {
    let current = output
        .get(..current_len)
        .ok_or(PaymentCodecError::BufferTooSmall {
            required: current_len,
            available: output.len(),
        })?;
    let layout = payment_parse_customer(current)?;
    let balance = f32::from_ne_bytes(
        current[layout.balance_offset..layout.balance_offset + mem::size_of::<f32>()]
            .try_into()
            .expect("the parsed balance range has four bytes"),
    );
    let ytd_payment = f32::from_ne_bytes(
        current[layout.ytd_payment_offset..layout.ytd_payment_offset + mem::size_of::<f32>()]
            .try_into()
            .expect("the parsed YTD payment range has four bytes"),
    );
    let replacement_balance = balance - payment_amount;
    let replacement_ytd = ytd_payment + payment_amount;
    if !replacement_balance.is_finite() {
        return Err(PaymentCodecError::NonFinite("customer balance"));
    }
    if !replacement_ytd.is_finite() {
        return Err(PaymentCodecError::NonFinite("customer YTD payment"));
    }
    let replacement_count =
        layout
            .payment_count
            .checked_add(1)
            .ok_or(PaymentCodecError::ArithmeticOverflow(
                "customer payment count",
            ))?;
    let mut encoded_count = [0_u8; 5];
    let encoded_count_len = payment_encode_i32(replacement_count, &mut encoded_count);
    let tail_len = current_len - layout.payment_count_end;
    let replacement_count_end = layout
        .payment_count_start
        .checked_add(encoded_count_len)
        .ok_or(PaymentCodecError::ArithmeticOverflow(
            "customer replacement length",
        ))?;
    let replacement_len = replacement_count_end.checked_add(tail_len).ok_or(
        PaymentCodecError::ArithmeticOverflow("customer replacement length"),
    )?;
    if replacement_len > output.len() || replacement_len > PAYMENT_VALUE_CAPACITY {
        return Err(PaymentCodecError::BufferTooSmall {
            required: replacement_len,
            available: output.len().min(PAYMENT_VALUE_CAPACITY),
        });
    }

    output.copy_within(layout.payment_count_end..current_len, replacement_count_end);
    output[layout.payment_count_start..replacement_count_end]
        .copy_from_slice(&encoded_count[..encoded_count_len]);
    output[layout.balance_offset..layout.balance_offset + mem::size_of::<f32>()]
        .copy_from_slice(&replacement_balance.to_ne_bytes());
    output[layout.ytd_payment_offset..layout.ytd_payment_offset + mem::size_of::<f32>()]
        .copy_from_slice(&replacement_ytd.to_ne_bytes());
    Ok(replacement_len)
}

fn payment_decode_customer_id(bytes: &[u8]) -> Result<i32, PaymentCodecError> {
    let mut cursor = 0;
    let (_, _, customer_id) = payment_decode_i32(bytes, &mut cursor, "customer-name ID")?;
    // Mako's value encoder rounds a one-byte struct payload up to two bytes
    // before appending its metadata suffix. The wrapper strips that suffix,
    // leaving one canonical zero padding byte for customer IDs 1..=63.
    let mako_one_byte_padding = cursor == 1 && bytes.len() == 2 && bytes[1] == 0;
    if cursor != bytes.len() && !mako_one_byte_padding {
        return Err(PaymentCodecError::InvalidVarint("customer-name ID"));
    }
    if customer_id <= 0 {
        return Err(PaymentCodecError::InvalidLength(
            "positive customer-name ID",
        ));
    }
    Ok(customer_id)
}

fn payment_lower_median(customer_ids: &[i32]) -> Result<i32, PaymentCodecError> {
    if customer_ids.is_empty() {
        return Err(PaymentCodecError::InvalidLength(
            "customer-name result count",
        ));
    }
    // The C++ TPC-C path uses a 32-entry static callback and computes the
    // lower median from every row the callback retained, including the full
    // 32-entry case. Preserve that release-build behavior exactly. A larger
    // slice remains an internal contract violation even though the bounded
    // production scan cannot construct one.
    if customer_ids.len() > PAYMENT_NAME_SCAN_LIMIT {
        return Err(PaymentCodecError::InvalidLength(
            "customer-name result limit",
        ));
    }
    let selected = customer_ids[(customer_ids.len() - 1) / 2];
    if selected <= 0 {
        return Err(PaymentCodecError::InvalidLength(
            "positive selected customer ID",
        ));
    }
    Ok(selected)
}

fn payment_warehouse_name(bytes: &[u8]) -> Result<&[u8], PaymentCodecError> {
    let mut cursor = 0;
    payment_take(bytes, &mut cursor, 4, "warehouse YTD")?;
    payment_take(bytes, &mut cursor, 4, "warehouse tax")?;
    let name = payment_read_inline_u8(bytes, &mut cursor, 10, "warehouse name")?;
    payment_skip_inline_u8(bytes, &mut cursor, 20, "warehouse street 1")?;
    payment_skip_inline_u8(bytes, &mut cursor, 20, "warehouse street 2")?;
    payment_skip_inline_u8(bytes, &mut cursor, 20, "warehouse city")?;
    payment_take(bytes, &mut cursor, 2, "warehouse state")?;
    let zip = payment_take(bytes, &mut cursor, 9, "warehouse ZIP")?;
    if &bytes[zip..zip + 9] != b"123456789" || cursor != bytes.len() {
        return Err(PaymentCodecError::InvalidLength("warehouse value"));
    }
    Ok(name)
}

fn payment_district_name(bytes: &[u8]) -> Result<&[u8], PaymentCodecError> {
    let mut cursor = 0;
    payment_take(bytes, &mut cursor, 4, "district YTD")?;
    payment_take(bytes, &mut cursor, 4, "district tax")?;
    let (_, _, next_order_id) = payment_decode_i32(bytes, &mut cursor, "district next order ID")?;
    if next_order_id < 3_001 {
        return Err(PaymentCodecError::InvalidLength("district next order ID"));
    }
    let name = payment_read_inline_u8(bytes, &mut cursor, 10, "district name")?;
    payment_skip_inline_u8(bytes, &mut cursor, 20, "district street 1")?;
    payment_skip_inline_u8(bytes, &mut cursor, 20, "district street 2")?;
    payment_skip_inline_u8(bytes, &mut cursor, 20, "district city")?;
    payment_take(bytes, &mut cursor, 2, "district state")?;
    let zip = payment_take(bytes, &mut cursor, 9, "district ZIP")?;
    if &bytes[zip..zip + 9] != b"123456789" || cursor != bytes.len() {
        return Err(PaymentCodecError::InvalidLength("district value"));
    }
    Ok(name)
}

// `payment_patch_customer` has already parsed the complete row on this path.
// Recheck only the two scalar invariants that decide or guard the tail.
fn payment_customer_bad_credit_after_patch(bytes: &[u8]) -> Result<bool, PaymentCodecError> {
    let credit = bytes
        .get(4..6)
        .ok_or(PaymentCodecError::Truncated("customer credit"))?;
    let bad_credit = match credit {
        b"BC" => true,
        b"GC" => false,
        _ => return Err(PaymentCodecError::InvalidLength("customer credit")),
    };
    if bytes.get(bytes.len().saturating_sub(2)..) != Some(&b"OE"[..]) {
        return Err(PaymentCodecError::InvalidLength("customer middle name"));
    }
    Ok(bad_credit)
}

fn payment_history_key(
    customer_district_id: i32,
    customer_warehouse_id: i32,
    customer_id: i32,
    district_id: i32,
    warehouse_id: i32,
    timestamp: u32,
) -> [u8; 24] {
    let mut key = [0_u8; 24];
    for (index, field) in [
        customer_district_id,
        customer_warehouse_id,
        customer_id,
        district_id,
        warehouse_id,
    ]
    .into_iter()
    .enumerate()
    {
        key[index * 4..index * 4 + 4].copy_from_slice(&field.to_be_bytes());
    }
    key[20..24].copy_from_slice(&timestamp.to_be_bytes());
    key
}

fn payment_c_string_prefix(bytes: &[u8], maximum: usize) -> &[u8] {
    let limited = &bytes[..bytes.len().min(maximum)];
    &limited[..limited
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(limited.len())]
}

fn payment_history_value(
    payment_amount: f32,
    warehouse_name: &[u8],
    district_name: &[u8],
) -> [u8; PAYMENT_HISTORY_VALUE_LENGTH] {
    let mut value = [0_u8; PAYMENT_HISTORY_VALUE_LENGTH];
    value[..4].copy_from_slice(&payment_amount.to_ne_bytes());
    let warehouse_name = payment_c_string_prefix(warehouse_name, 10);
    let district_name = payment_c_string_prefix(district_name, 10);
    let data_length = warehouse_name.len() + 4 + district_name.len();
    debug_assert!(data_length <= 24);
    value[4] = data_length as u8;
    let mut cursor = 5;
    value[cursor..cursor + warehouse_name.len()].copy_from_slice(warehouse_name);
    cursor += warehouse_name.len();
    value[cursor..cursor + 4].copy_from_slice(b"    ");
    cursor += 4;
    value[cursor..cursor + district_name.len()].copy_from_slice(district_name);
    value
}

struct PaymentTextBuffer<'bytes> {
    bytes: &'bytes mut [u8],
    len: usize,
}

impl PaymentTextBuffer<'_> {
    fn push_bytes(&mut self, bytes: &[u8]) {
        let copied = bytes.len().min(self.bytes.len().saturating_sub(self.len));
        self.bytes[self.len..self.len + copied].copy_from_slice(&bytes[..copied]);
        self.len += copied;
    }
}

impl fmt::Write for PaymentTextBuffer<'_> {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        self.push_bytes(text.as_bytes());
        Ok(())
    }
}

#[derive(Clone, Copy)]
struct PaymentCustomerDataFields {
    customer_id: i32,
    adjusted_customer_district_id: i32,
    customer_warehouse_id: i32,
    district_id: i32,
    warehouse_id: i32,
    payment_amount: f32,
}

fn payment_patch_customer_data(
    output: &mut [u8],
    current_len: usize,
    fields: PaymentCustomerDataFields,
) -> Result<usize, PaymentCodecError> {
    if current_len != PAYMENT_CUSTOMER_DATA_VALUE_LENGTH
        || output.len() < PAYMENT_CUSTOMER_DATA_VALUE_LENGTH
    {
        return Err(PaymentCodecError::InvalidLength("customer data value"));
    }
    let declared = u16::from_ne_bytes(
        output[..2]
            .try_into()
            .expect("the fixed customer-data size field has two bytes"),
    );
    let source_length = usize::from(declared);
    if source_length > 300 {
        return Err(PaymentCodecError::InvalidLength("customer data string"));
    }
    let source = payment_c_string_prefix(&output[2..2 + source_length], source_length);
    let mut replacement = [0_u8; PAYMENT_CUSTOMER_DATA_VALUE_LENGTH];
    let mut text = PaymentTextBuffer {
        bytes: &mut replacement[2..302],
        len: 0,
    };
    write!(
        text,
        "{} {} {} {} {} {} | ",
        fields.customer_id,
        fields.adjusted_customer_district_id,
        fields.customer_warehouse_id,
        fields.district_id,
        fields.warehouse_id,
        fields.payment_amount as i32
    )
    .expect("the fixed Payment text writer cannot fail");
    text.push_bytes(source);
    let replacement_length = text.len;
    replacement[..2].copy_from_slice(&(replacement_length as u16).to_ne_bytes());
    output[..PAYMENT_CUSTOMER_DATA_VALUE_LENGTH].copy_from_slice(&replacement);
    Ok(PAYMENT_CUSTOMER_DATA_VALUE_LENGTH)
}

#[derive(Clone, Copy)]
struct PaymentByteRange {
    start: usize,
    end: usize,
}

impl PaymentByteRange {
    #[inline]
    fn overlaps(self, other: Self) -> bool {
        self.start < other.end && other.start < self.end
    }
}

fn payment_pointer_range<T>(
    pointer: *const T,
    length: usize,
    name: &str,
) -> FfiResult<PaymentByteRange> {
    if pointer.is_null() {
        return Err(fatal(format_args!("{name} must not be null")));
    }
    if !pointer.addr().is_multiple_of(mem::align_of::<T>()) {
        return Err(fatal(format_args!("{name} must be aligned")));
    }
    let byte_length = length
        .checked_mul(mem::size_of::<T>())
        .filter(|length| *length <= isize::MAX as usize)
        .ok_or_else(|| fatal(format_args!("{name} byte length overflows")))?;
    let start = pointer.addr();
    let end = start
        .checked_add(byte_length)
        .ok_or_else(|| fatal(format_args!("{name} address range overflows")))?;
    Ok(PaymentByteRange { start, end })
}

fn payment_output_range(
    pointer: *mut u8,
    capacity: usize,
    name: &str,
) -> FfiResult<PaymentByteRange> {
    payment_pointer_range(pointer.cast_const(), capacity, name)
}

unsafe fn payment_copy_key<const LENGTH: usize>(
    pointer: *const u8,
    name: &str,
) -> FfiResult<[u8; LENGTH]> {
    payment_pointer_range(pointer, LENGTH, name)?;
    let mut key = [0_u8; LENGTH];
    // SAFETY: The validated source names `LENGTH` readable bytes and the
    // stack-owned destination is disjoint and exactly sized.
    unsafe { ptr::copy_nonoverlapping(pointer, key.as_mut_ptr(), LENGTH) };
    Ok(key)
}

unsafe fn payment_table_owned<'a>(
    handle: &StoTpccThread,
    pointer: *const StoTpccTable,
    name: &str,
) -> FfiResult<&'a StoTpccTable> {
    payment_pointer_range(pointer, 1, name)?;
    // SAFETY: The private ABI requires a live table allocation. Nullness,
    // alignment, and address arithmetic were checked above.
    let table = unsafe { &*pointer };
    if table.state.runtime_id != handle.sto_worker.runtime().id() {
        return Err(fatal(format_args!(
            "{name} belongs to a different STO runtime"
        )));
    }
    Ok(table)
}

struct PaymentAttemptGuard<'handle> {
    handle: &'handle mut StoTpccThread,
    armed: bool,
}

struct PaymentFullStaging {
    warehouse: [u8; PAYMENT_VALUE_CAPACITY],
    district: [u8; PAYMENT_VALUE_CAPACITY],
    customer: [u8; PAYMENT_VALUE_CAPACITY],
    history: [u8; PAYMENT_HISTORY_VALUE_LENGTH],
    customer_data: mem::MaybeUninit<[u8; PAYMENT_CUSTOMER_DATA_VALUE_LENGTH]>,
}

impl PaymentFullStaging {
    fn new() -> Self {
        Self {
            warehouse: [0; PAYMENT_VALUE_CAPACITY],
            district: [0; PAYMENT_VALUE_CAPACITY],
            customer: [0; PAYMENT_VALUE_CAPACITY],
            history: [0; PAYMENT_HISTORY_VALUE_LENGTH],
            customer_data: mem::MaybeUninit::uninit(),
        }
    }
}

impl<'handle> PaymentAttemptGuard<'handle> {
    fn new(handle: &'handle mut StoTpccThread) -> Self {
        Self {
            handle,
            armed: true,
        }
    }

    fn disarm(&mut self) {
        self.armed = false;
    }
}

impl Drop for PaymentAttemptGuard<'_> {
    fn drop(&mut self) {
        if !self.armed {
            return;
        }
        payment_abort_attempt(self.handle);
    }
}

fn payment_abort_attempt(handle: &mut StoTpccThread) {
    handle.point_batch.clear();
    handle.scan_scratch = ScanScratch::default();
    if handle.active.is_some() {
        let _ = abort_active_attempt_after_fatal(handle);
    } else {
        handle.pending_size.clear();
    }
}

fn payment_boundary(
    thread_handle: *mut StoTpccThread,
    operation: &'static str,
    body: impl FnOnce() -> FfiResult<Status>,
) -> i32 {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(status) | Err(status)) => status.code(),
        Err(_) => {
            // The attempt guard normally performs this cleanup while the
            // panic unwinds. This fallback also covers a panic between
            // validating the trusted live handle and constructing the guard.
            // The private ABI promises pointer validity and exclusivity for
            // the complete call; null and misaligned handles cannot be
            // dereferenced even under that contract.
            if !thread_handle.is_null()
                && thread_handle
                    .addr()
                    .is_multiple_of(mem::align_of::<StoTpccThread>())
            {
                // SAFETY: The wrapper-private contract supplies a live,
                // exclusively accessed handle for the complete call.
                payment_abort_attempt(unsafe { &mut *thread_handle });
            }
            set_last_error(format_args!(
                "{operation}: contained an unexpected Rust panic"
            ));
            Status::Fatal.code()
        }
    }
}

#[allow(
    unsafe_code,
    reason = "the caller-owned output outlives the active transaction"
)]
unsafe fn payment_modify_full_cached_row<const KEY_LENGTH: usize>(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    key: &[u8; KEY_LENGTH],
    output: &mut [u8],
    operation: &'static str,
    patch: impl FnOnce(&mut [u8], usize) -> Result<usize, PaymentCodecError>,
) -> FfiResult<usize> {
    if table.cache_policy != ResolvedCachePolicy::Full {
        return Err(fatal(format_args!(
            "{operation}: table must use the full resolved cache policy"
        )));
    }

    let probe = handle.resolved_cache.probe(&table.state.table, key);
    let mut codec_error = None;
    let modify = |buffer: &mut [u8], current_len: usize| match patch(buffer, current_len) {
        Ok(length) => Ok(length),
        Err(error) => {
            codec_error = Some(error);
            Err(InvalidUse::IllegalItemState.into())
        }
    };
    let access = {
        let native_worker = &handle.native_worker;
        let transaction = active_transaction(&mut handle.active)?;
        match probe.record {
            Some(resolved) => {
                // SAFETY: The Payment request keeps this caller-owned output
                // allocation readable and immutable through transaction finish.
                unsafe {
                    table.state.table.try_modify_resolved_borrowed(
                        transaction,
                        resolved,
                        output,
                        modify,
                    )
                }
                .map(|length| (length, None))
            }
            None => {
                // SAFETY: The Payment request keeps this caller-owned output
                // allocation readable and immutable through transaction finish.
                unsafe {
                    table.state.table.try_modify_resolving_borrowed(
                        transaction,
                        native_worker,
                        key,
                        output,
                        modify,
                    )
                }
                .map(|(length, resolved)| (length, Some(resolved)))
            }
        }
    };
    match access {
        Ok((length, resolved)) => {
            if let Some(resolved) = resolved {
                handle.resolved_cache.remember_after_probe(
                    &table.state.table,
                    key,
                    resolved,
                    probe,
                );
            }
            length.ok_or_else(|| fatal(format_args!("{operation}: required row is missing")))
        }
        Err(_) if codec_error.is_some() => Err(fatal(format_args!(
            "{operation}: {}",
            codec_error.expect("the guarded branch proved a codec error")
        ))),
        Err(error) => Err(status_from_access(operation, error)),
    }
}

#[allow(
    unsafe_code,
    reason = "the trusted scan borrows rows only for the Rust callback"
)]
fn payment_select_customer_by_name(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    lower: &[u8; 40],
    upper: &[u8; 40],
) -> FfiResult<i32> {
    let mut customer_ids = [0_i32; PAYMENT_NAME_SCAN_LIMIT];
    let mut count = 0;
    let mut codec_error = None;
    let access = {
        let native_worker = &handle.native_worker;
        let scratch = &mut handle.scan_scratch;
        let transaction = active_transaction(&mut handle.active)?;
        // SAFETY: This table came from the closed direct-token FFI creator.
        // The callback copies each compressed ID and retains no row pointer.
        unsafe {
            table
                .state
                .table
                .visit_bounded_forward_values_trusted_with_scratch(
                    transaction,
                    native_worker,
                    lower,
                    upper,
                    PAYMENT_NAME_SCAN_LIMIT,
                    scratch,
                    |value, _resolved| match payment_decode_customer_id(value) {
                        Ok(customer_id) => {
                            debug_assert!(count < PAYMENT_NAME_SCAN_LIMIT);
                            customer_ids[count] = customer_id;
                            count += 1;
                            ScanControl::Continue
                        }
                        Err(error) => {
                            codec_error = Some(error);
                            ScanControl::Stop
                        }
                    },
                )
        }
    };
    if let Some(error) = codec_error {
        return Err(fatal(format_args!("customer-name scan: {error}")));
    }
    if let Err(error) = access {
        return Err(status_from_access("customer-name scan", error));
    }
    payment_lower_median(&customer_ids[..count])
        .map_err(|error| fatal(format_args!("customer-name scan: {error}")))
}

#[allow(
    unsafe_code,
    reason = "the private C++ wrapper owns every validated pointer and output lifetime"
)]
unsafe fn payment_prefix_body(
    guard: &mut PaymentAttemptGuard<'_>,
    request_pointer: *const MakoStoTpccPaymentPrefixRequest,
    result_pointer: *mut MakoStoTpccPaymentPrefixResult,
) -> FfiResult<Status> {
    payment_pointer_range(request_pointer, 1, "payment request")?;
    payment_pointer_range(result_pointer.cast_const(), 1, "payment result")?;
    // SAFETY: The request range is aligned and readable under the private ABI.
    // Copy it before constructing any mutable output slice, so an invalid C++
    // alias cannot leave simultaneous Rust references to the request.
    let request = unsafe { *request_pointer };
    if request.customer_by_name > 1 {
        return Err(fatal(format_args!(
            "payment customer_by_name must be exactly 0 or 1"
        )));
    }
    if request.output_capacity == 0 || request.output_capacity > PAYMENT_VALUE_CAPACITY {
        return Err(fatal(format_args!(
            "payment output capacity must be in 1..={PAYMENT_VALUE_CAPACITY}"
        )));
    }
    if !request.payment_amount.is_finite() {
        return Err(fatal(format_args!("payment amount must be finite")));
    }

    let warehouse_range = payment_output_range(
        request.warehouse_output,
        request.output_capacity,
        "warehouse output",
    )?;
    let district_range = payment_output_range(
        request.district_output,
        request.output_capacity,
        "district output",
    )?;
    let customer_range = payment_output_range(
        request.customer_output,
        request.output_capacity,
        "customer output",
    )?;
    let result_range = payment_pointer_range(result_pointer.cast_const(), 1, "payment result")?;
    for (left_name, left, right_name, right) in [
        (
            "warehouse output",
            warehouse_range,
            "district output",
            district_range,
        ),
        (
            "warehouse output",
            warehouse_range,
            "customer output",
            customer_range,
        ),
        (
            "district output",
            district_range,
            "customer output",
            customer_range,
        ),
        (
            "warehouse output",
            warehouse_range,
            "payment result",
            result_range,
        ),
        (
            "district output",
            district_range,
            "payment result",
            result_range,
        ),
        (
            "customer output",
            customer_range,
            "payment result",
            result_range,
        ),
    ] {
        if left.overlaps(right) {
            return Err(fatal(format_args!(
                "{left_name} must not overlap {right_name}"
            )));
        }
    }

    let handle = &mut *guard.handle;
    let warehouse_table =
        unsafe { payment_table_owned(handle, request.warehouse_table, "warehouse table")? };
    let district_table =
        unsafe { payment_table_owned(handle, request.district_table, "district table")? };
    let customer_table =
        unsafe { payment_table_owned(handle, request.customer_table, "customer table")? };
    let customer_name_table = if request.customer_by_name == 1 {
        Some(unsafe {
            payment_table_owned(handle, request.customer_name_table, "customer-name table")?
        })
    } else {
        None
    };

    let warehouse_key = unsafe { payment_copy_key::<4>(request.warehouse_key, "warehouse key")? };
    let district_key = unsafe { payment_copy_key::<8>(request.district_key, "district key")? };
    let customer_prefix =
        unsafe { payment_copy_key::<8>(request.customer_key_prefix, "customer key prefix")? };
    let (name_lower, name_upper) = if request.customer_by_name == 1 {
        let lower = unsafe {
            payment_copy_key::<40>(request.customer_name_lower_key, "customer-name lower key")?
        };
        let upper = unsafe {
            payment_copy_key::<40>(request.customer_name_upper_key, "customer-name upper key")?
        };
        if lower >= upper {
            return Err(fatal(format_args!(
                "customer-name lower key must precede its upper key"
            )));
        }
        (Some(lower), Some(upper))
    } else {
        (None, None)
    };

    // SAFETY: The three checked ranges are nonempty, non-overflowing, and
    // pairwise disjoint. The private wrapper keeps them alive through finish.
    let warehouse_output =
        unsafe { slice::from_raw_parts_mut(request.warehouse_output, request.output_capacity) };
    let district_output =
        unsafe { slice::from_raw_parts_mut(request.district_output, request.output_capacity) };
    let customer_output =
        unsafe { slice::from_raw_parts_mut(request.customer_output, request.output_capacity) };

    let warehouse_length = unsafe {
        payment_modify_full_cached_row(
            handle,
            warehouse_table,
            &warehouse_key,
            warehouse_output,
            "payment warehouse",
            |output, current_len| {
                payment_patch_leading_ytd(
                    output,
                    current_len,
                    request.payment_amount,
                    "warehouse YTD",
                )
            },
        )?
    };
    let district_length = unsafe {
        payment_modify_full_cached_row(
            handle,
            district_table,
            &district_key,
            district_output,
            "payment district",
            |output, current_len| {
                payment_patch_leading_ytd(
                    output,
                    current_len,
                    request.payment_amount,
                    "district YTD",
                )
            },
        )?
    };

    let selected_customer_id = if let Some(customer_name_table) = customer_name_table {
        payment_select_customer_by_name(
            handle,
            customer_name_table,
            name_lower
                .as_ref()
                .expect("the by-name branch copied a lower key"),
            name_upper
                .as_ref()
                .expect("the by-name branch copied an upper key"),
        )?
    } else {
        if request.customer_id <= 0 {
            return Err(fatal(format_args!("payment customer ID must be positive")));
        }
        request.customer_id
    };
    let mut customer_key = [0_u8; 12];
    customer_key[..8].copy_from_slice(&customer_prefix);
    customer_key[8..].copy_from_slice(&selected_customer_id.to_be_bytes());
    let customer_length = unsafe {
        payment_modify_full_cached_row(
            handle,
            customer_table,
            &customer_key,
            customer_output,
            "payment customer",
            |output, current_len| {
                payment_patch_customer(output, current_len, request.payment_amount)
            },
        )?
    };

    let result = MakoStoTpccPaymentPrefixResult {
        warehouse_length,
        district_length,
        customer_length,
        customer_id: selected_customer_id,
    };
    // SAFETY: The aligned result allocation was validated before mutation and
    // does not overlap any retained output prefix. Write it only on success.
    unsafe { ptr::write(result_pointer, result) };
    guard.disarm();
    Ok(Status::Ok)
}

#[allow(
    unsafe_code,
    reason = "the private C++ wrapper supplies live handles while local staging outlives commit"
)]
unsafe fn payment_full_body(
    guard: &mut PaymentAttemptGuard<'_>,
    request_pointer: *const MakoStoTpccPaymentFullRequest,
    result_pointer: *mut MakoStoTpccPaymentFullResult,
    staging: &mut PaymentFullStaging,
) -> FfiResult<Status> {
    let request_range = payment_pointer_range(request_pointer, 1, "full payment request")?;
    let result_range =
        payment_pointer_range(result_pointer.cast_const(), 1, "full payment result")?;
    if request_range.overlaps(result_range) {
        return Err(fatal(format_args!(
            "full payment request must not overlap its result"
        )));
    }
    // SAFETY: The aligned request allocation was validated above and the
    // private wrapper retains it for the call.
    let request = unsafe { *request_pointer };
    if request.customer_by_name > 1 {
        return Err(fatal(format_args!(
            "payment customer_by_name must be exactly 0 or 1"
        )));
    }
    if !request.payment_amount.is_finite() {
        return Err(fatal(format_args!("payment amount must be finite")));
    }
    for (name, value) in [
        ("warehouse ID", request.warehouse_id),
        ("district ID", request.district_id),
        ("customer warehouse ID", request.customer_warehouse_id),
        ("customer district ID", request.customer_district_id),
    ] {
        if value <= 0 {
            return Err(fatal(format_args!("payment {name} must be positive")));
        }
    }

    let handle = &mut *guard.handle;
    let warehouse_table =
        unsafe { payment_table_owned(handle, request.warehouse_table, "warehouse table")? };
    let district_table =
        unsafe { payment_table_owned(handle, request.district_table, "district table")? };
    let customer_table =
        unsafe { payment_table_owned(handle, request.customer_table, "customer table")? };
    let history_table =
        unsafe { payment_table_owned(handle, request.history_table, "history table")? };
    let customer_name_table = if request.customer_by_name == 1 {
        Some(unsafe {
            payment_table_owned(handle, request.customer_name_table, "customer-name table")?
        })
    } else {
        None
    };

    let warehouse_key = unsafe { payment_copy_key::<4>(request.warehouse_key, "warehouse key")? };
    let district_key = unsafe { payment_copy_key::<8>(request.district_key, "district key")? };
    let customer_prefix =
        unsafe { payment_copy_key::<8>(request.customer_key_prefix, "customer key prefix")? };
    let (name_lower, name_upper) = if request.customer_by_name == 1 {
        let lower = unsafe {
            payment_copy_key::<40>(request.customer_name_lower_key, "customer-name lower key")?
        };
        let upper = unsafe {
            payment_copy_key::<40>(request.customer_name_upper_key, "customer-name upper key")?
        };
        if lower >= upper {
            return Err(fatal(format_args!(
                "customer-name lower key must precede its upper key"
            )));
        }
        (Some(lower), Some(upper))
    } else {
        (None, None)
    };

    let PaymentFullStaging {
        warehouse: warehouse_output,
        district: district_output,
        customer: customer_output,
        history: history_value,
        customer_data: customer_data_output,
    } = staging;
    // Every borrowed replacement below belongs to the outer FFI frame. That
    // frame declares its attempt guard after this staging object, so commit or
    // guarded abort always resolves the transaction before a buffer drops.
    let warehouse_length = unsafe {
        payment_modify_full_cached_row(
            handle,
            warehouse_table,
            &warehouse_key,
            warehouse_output,
            "payment warehouse",
            |output, current_len| {
                payment_patch_leading_ytd(
                    output,
                    current_len,
                    request.payment_amount,
                    "warehouse YTD",
                )
            },
        )?
    };
    let district_length = unsafe {
        payment_modify_full_cached_row(
            handle,
            district_table,
            &district_key,
            district_output,
            "payment district",
            |output, current_len| {
                payment_patch_leading_ytd(
                    output,
                    current_len,
                    request.payment_amount,
                    "district YTD",
                )
            },
        )?
    };

    let selected_customer_id = if let Some(customer_name_table) = customer_name_table {
        payment_select_customer_by_name(
            handle,
            customer_name_table,
            name_lower
                .as_ref()
                .expect("the by-name branch copied a lower key"),
            name_upper
                .as_ref()
                .expect("the by-name branch copied an upper key"),
        )?
    } else {
        if request.customer_id <= 0 {
            return Err(fatal(format_args!("payment customer ID must be positive")));
        }
        request.customer_id
    };
    let mut customer_key = [0_u8; 12];
    customer_key[..8].copy_from_slice(&customer_prefix);
    customer_key[8..].copy_from_slice(&selected_customer_id.to_be_bytes());
    let customer_length = unsafe {
        payment_modify_full_cached_row(
            handle,
            customer_table,
            &customer_key,
            customer_output,
            "payment customer",
            |output, current_len| {
                payment_patch_customer(output, current_len, request.payment_amount)
            },
        )?
    };

    let warehouse_name = payment_warehouse_name(&warehouse_output[..warehouse_length])
        .map_err(|error| fatal(format_args!("payment warehouse: {error}")))?;
    let district_name = payment_district_name(&district_output[..district_length])
        .map_err(|error| fatal(format_args!("payment district: {error}")))?;
    let bad_credit = payment_customer_bad_credit_after_patch(&customer_output[..customer_length])
        .map_err(|error| fatal(format_args!("payment customer: {error}")))?;
    let history_key = payment_history_key(
        request.customer_district_id,
        request.customer_warehouse_id,
        selected_customer_id,
        request.district_id,
        request.warehouse_id,
        request.timestamp,
    );
    *history_value = payment_history_value(request.payment_amount, warehouse_name, district_name);
    // SAFETY: The outer staging frame keeps the history value immutable until
    // this call commits or its still-armed guard aborts the attempt.
    match unsafe { insert_borrowed_impl(handle, history_table, &history_key, history_value) }? {
        Status::Ok | Status::Duplicate => {}
        status => return Ok(status),
    }

    if bad_credit {
        let customer_data_output =
            customer_data_output.write([0; PAYMENT_CUSTOMER_DATA_VALUE_LENGTH]);
        let customer_data_district_id = request
            .customer_district_id
            .checked_add(100)
            .ok_or_else(|| fatal(format_args!("customer data district ID overflow")))?;
        let mut customer_data_key = customer_key;
        customer_data_key[4..8].copy_from_slice(&customer_data_district_id.to_be_bytes());
        let customer_data_length = unsafe {
            payment_modify_full_cached_row(
                handle,
                customer_table,
                &customer_data_key,
                customer_data_output,
                "payment customer data",
                |output, current_len| {
                    payment_patch_customer_data(
                        output,
                        current_len,
                        PaymentCustomerDataFields {
                            customer_id: selected_customer_id,
                            adjusted_customer_district_id: customer_data_district_id,
                            customer_warehouse_id: request.customer_warehouse_id,
                            district_id: request.district_id,
                            warehouse_id: request.warehouse_id,
                            payment_amount: request.payment_amount,
                        },
                    )
                },
            )?
        };
        if customer_data_length != PAYMENT_CUSTOMER_DATA_VALUE_LENGTH {
            return Err(fatal(format_args!(
                "payment customer data returned an invalid length"
            )));
        }
    }

    let commit_status = txn_commit_impl(handle)?;
    if commit_status != Status::Ok {
        return Ok(commit_status);
    }
    let result = MakoStoTpccPaymentFullResult {
        history_value_length: PAYMENT_HISTORY_VALUE_LENGTH,
        customer_id: selected_customer_id,
    };
    // SAFETY: Result validity and non-overlap were checked before any mutation.
    unsafe { ptr::write(result_pointer, result) };
    guard.disarm();
    Ok(Status::Ok)
}

/// Executes and commits one fully local TPC-C Payment transaction.
///
/// # Safety
/// The wrapper must provide live same-runtime handles and readable fixed-width
/// key allocations for the complete call. Request and result must be disjoint,
/// and the result allocation must not alias the thread handle, a table handle,
/// or any table object reachable through those handles.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_payment_full_trusted(
    thread_handle: *mut StoTpccThread,
    request: *const MakoStoTpccPaymentFullRequest,
    result: *mut MakoStoTpccPaymentFullResult,
) -> i32 {
    payment_boundary(thread_handle, "mako_sto_tpcc_payment_full_trusted", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        if handle.active.is_none() {
            return Err(fatal(format_args!(
                "full payment requires an active transaction"
            )));
        }
        // Declare retained staging before the guard. Rust drops locals in
        // reverse order, so both an ordinary error and an unwind abort the
        // active attempt while every borrowed value is still live.
        let mut staging = PaymentFullStaging::new();
        let mut guard = PaymentAttemptGuard::new(handle);
        // SAFETY: Raw validation and the commit-scoped staging contract are
        // centralized in the guarded body.
        unsafe { payment_full_body(&mut guard, request, result, &mut staging) }
    })
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum NewOrderCodecError {
    Truncated(&'static str),
    InvalidLength(&'static str),
    InvalidVarint(&'static str),
    TrailingBytes(&'static str),
    OutOfRange(&'static str),
}

impl fmt::Display for NewOrderCodecError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Truncated(field) => write!(formatter, "truncated {field}"),
            Self::InvalidLength(field) => write!(formatter, "invalid {field} length"),
            Self::InvalidVarint(field) => write!(formatter, "invalid {field} varint"),
            Self::TrailingBytes(value) => write!(formatter, "trailing {value} bytes"),
            Self::OutOfRange(field) => write!(formatter, "{field} is out of range"),
        }
    }
}

#[inline]
fn new_order_take(
    bytes: &[u8],
    cursor: &mut usize,
    length: usize,
    field: &'static str,
) -> Result<usize, NewOrderCodecError> {
    let start = *cursor;
    let end = start
        .checked_add(length)
        .filter(|end| *end <= bytes.len())
        .ok_or(NewOrderCodecError::Truncated(field))?;
    *cursor = end;
    Ok(start)
}

fn new_order_skip_inline_u8(
    bytes: &[u8],
    cursor: &mut usize,
    maximum: usize,
    field: &'static str,
) -> Result<(), NewOrderCodecError> {
    let length_offset = new_order_take(bytes, cursor, 1, field)?;
    if usize::from(bytes[length_offset]) > maximum {
        return Err(NewOrderCodecError::InvalidLength(field));
    }
    new_order_take(bytes, cursor, maximum + 1, field)?;
    Ok(())
}

fn new_order_decode_uvarint32(
    bytes: &[u8],
    cursor: &mut usize,
    field: &'static str,
) -> Result<u32, NewOrderCodecError> {
    let start = *cursor;
    let mut value = 0_u32;
    for index in 0..5 {
        let offset = new_order_take(bytes, cursor, 1, field)?;
        let byte = bytes[offset];
        if index == 4 && byte > 0x0f {
            return Err(NewOrderCodecError::InvalidVarint(field));
        }
        value |= u32::from(byte & 0x7f) << (index * 7);
        if byte < 0x80 {
            if *cursor - start != payment_uvarint_length(value) {
                return Err(NewOrderCodecError::InvalidVarint(field));
            }
            return Ok(value);
        }
    }
    Err(NewOrderCodecError::InvalidVarint(field))
}

#[inline]
fn new_order_decode_i32(
    bytes: &[u8],
    cursor: &mut usize,
    field: &'static str,
) -> Result<i32, NewOrderCodecError> {
    let encoded = new_order_decode_uvarint32(bytes, cursor, field)?;
    Ok(((encoded >> 1) as i32) ^ -((encoded & 1) as i32))
}

fn new_order_item_price(bytes: &[u8]) -> Result<f32, NewOrderCodecError> {
    if bytes.len() > NEW_ORDER_ITEM_VALUE_MAX {
        return Err(NewOrderCodecError::InvalidLength("item value"));
    }
    let mut cursor = 0;
    new_order_skip_inline_u8(bytes, &mut cursor, 24, "item name")?;
    let price_offset = new_order_take(bytes, &mut cursor, 4, "item price")?;
    let price = f32::from_ne_bytes(
        bytes[price_offset..price_offset + 4]
            .try_into()
            .expect("the checked item-price range has four bytes"),
    );
    new_order_skip_inline_u8(bytes, &mut cursor, 50, "item data")?;
    new_order_decode_i32(bytes, &mut cursor, "item image ID")?;
    if cursor != bytes.len() {
        return Err(NewOrderCodecError::TrailingBytes("item"));
    }
    if !price.is_finite() || !(1.0..=100.0).contains(&price) {
        return Err(NewOrderCodecError::OutOfRange("item price"));
    }
    Ok(price)
}

fn new_order_stock_replacement(
    bytes: &[u8],
    quantity: u32,
    output: &mut [u8; NEW_ORDER_STOCK_VALUE_MAX],
) -> Result<usize, NewOrderCodecError> {
    if bytes.len() > output.len() {
        return Err(NewOrderCodecError::InvalidLength("stock value"));
    }
    let mut cursor = 0;
    let quantity_offset = new_order_take(bytes, &mut cursor, 2, "stock quantity")?;
    let ytd_offset = new_order_take(bytes, &mut cursor, 4, "stock YTD")?;
    new_order_decode_i32(bytes, &mut cursor, "stock order count")?;
    new_order_decode_i32(bytes, &mut cursor, "stock remote count")?;
    if cursor != bytes.len() {
        return Err(NewOrderCodecError::TrailingBytes("stock"));
    }

    let current_quantity = i16::from_ne_bytes(
        bytes[quantity_offset..quantity_offset + 2]
            .try_into()
            .expect("the checked stock-quantity range has two bytes"),
    );
    let replacement_quantity = if i32::from(current_quantity) - quantity as i32 >= 10 {
        i32::from(current_quantity) - quantity as i32
    } else {
        i32::from(current_quantity) - quantity as i32 + 91
    } as i16;
    let current_ytd = f32::from_ne_bytes(
        bytes[ytd_offset..ytd_offset + 4]
            .try_into()
            .expect("the checked stock-YTD range has four bytes"),
    );
    let replacement_ytd = current_ytd + quantity as f32;

    output[..bytes.len()].copy_from_slice(bytes);
    output[quantity_offset..quantity_offset + 2]
        .copy_from_slice(&replacement_quantity.to_ne_bytes());
    output[ytd_offset..ytd_offset + 4].copy_from_slice(&replacement_ytd.to_ne_bytes());
    Ok(bytes.len())
}

fn new_order_append(
    output: &mut [u8],
    cursor: &mut usize,
    bytes: &[u8],
    field: &'static str,
) -> Result<(), NewOrderCodecError> {
    let end = cursor
        .checked_add(bytes.len())
        .filter(|end| *end <= output.len())
        .ok_or(NewOrderCodecError::InvalidLength(field))?;
    output[*cursor..end].copy_from_slice(bytes);
    *cursor = end;
    Ok(())
}

fn new_order_append_i32(
    output: &mut [u8],
    cursor: &mut usize,
    value: i32,
    field: &'static str,
) -> Result<(), NewOrderCodecError> {
    let mut encoded = [0_u8; 5];
    let length = payment_encode_i32(value, &mut encoded);
    new_order_append(output, cursor, &encoded[..length], field)
}

fn new_order_append_u32(
    output: &mut [u8],
    cursor: &mut usize,
    mut value: u32,
    field: &'static str,
) -> Result<(), NewOrderCodecError> {
    let mut encoded = [0_u8; 5];
    let mut length = 0;
    while value > 0x7f {
        encoded[length] = (value as u8 & 0x7f) | 0x80;
        value >>= 7;
        length += 1;
    }
    encoded[length] = value as u8;
    new_order_append(output, cursor, &encoded[..=length], field)
}

fn new_order_oorder_value(
    customer_id: i32,
    line_count: u8,
    entry_date: u32,
    output: &mut [u8; NEW_ORDER_OORDER_VALUE_MAX],
) -> Result<usize, NewOrderCodecError> {
    let mut cursor = 0;
    new_order_append_i32(output, &mut cursor, customer_id, "order customer ID")?;
    new_order_append_i32(output, &mut cursor, 0, "order carrier ID")?;
    new_order_append(output, &mut cursor, &[line_count], "order line count")?;
    new_order_append(output, &mut cursor, &[1], "order all-local flag")?;
    new_order_append_u32(output, &mut cursor, entry_date, "order entry date")?;
    Ok(cursor)
}

fn new_order_order_line_value(
    item_id: i32,
    amount: f32,
    warehouse_id: i32,
    quantity: u8,
    output: &mut [u8; NEW_ORDER_ORDER_LINE_VALUE_MAX],
) -> Result<usize, NewOrderCodecError> {
    let mut cursor = 0;
    new_order_append_i32(output, &mut cursor, item_id, "order-line item ID")?;
    new_order_append_u32(output, &mut cursor, 0, "order-line delivery date")?;
    new_order_append(
        output,
        &mut cursor,
        &amount.to_ne_bytes(),
        "order-line amount",
    )?;
    new_order_append_i32(
        output,
        &mut cursor,
        warehouse_id,
        "order-line supply warehouse",
    )?;
    new_order_append(output, &mut cursor, &[quantity], "order-line quantity")?;
    Ok(cursor)
}

fn new_order_key3(first: i32, second: i32, third: i32) -> [u8; 12] {
    let mut key = [0_u8; 12];
    key[..4].copy_from_slice(&first.to_be_bytes());
    key[4..8].copy_from_slice(&second.to_be_bytes());
    key[8..].copy_from_slice(&third.to_be_bytes());
    key
}

fn new_order_key4(first: i32, second: i32, third: i32, fourth: i32) -> [u8; 16] {
    let mut key = [0_u8; 16];
    key[..4].copy_from_slice(&first.to_be_bytes());
    key[4..8].copy_from_slice(&second.to_be_bytes());
    key[8..12].copy_from_slice(&third.to_be_bytes());
    key[12..].copy_from_slice(&fourth.to_be_bytes());
    key
}

#[derive(Clone, Copy)]
struct DeliverySelectedNewOrder {
    order_id: i32,
    resolved: ResolvedRecord,
}

struct DeliveryOrderHeader {
    customer_id: i32,
    resolved: ResolvedRecord,
    replacement: [u8; DELIVERY_OORDER_VALUE_MAX],
    replacement_length: usize,
}

struct DeliveryOrderLines {
    resolved: [Option<ResolvedRecord>; DELIVERY_MAX_LINES_PER_DISTRICT],
    replacements: [[u8; DELIVERY_ORDER_LINE_VALUE_MAX]; DELIVERY_MAX_LINES_PER_DISTRICT],
    replacement_lengths: [usize; DELIVERY_MAX_LINES_PER_DISTRICT],
    count: usize,
    total: f32,
}

impl DeliveryOrderLines {
    fn new() -> Self {
        Self {
            resolved: [None; DELIVERY_MAX_LINES_PER_DISTRICT],
            replacements: [[0; DELIVERY_ORDER_LINE_VALUE_MAX]; DELIVERY_MAX_LINES_PER_DISTRICT],
            replacement_lengths: [0; DELIVERY_MAX_LINES_PER_DISTRICT],
            count: 0,
            total: 0.0,
        }
    }
}

fn delivery_oorder_replacement(
    bytes: &[u8],
    carrier_id: i32,
    output: &mut [u8; DELIVERY_OORDER_VALUE_MAX],
) -> Result<(usize, i32), NewOrderCodecError> {
    let mut cursor = 0;
    let customer_id = new_order_decode_i32(bytes, &mut cursor, "order customer ID")?;
    new_order_decode_i32(bytes, &mut cursor, "order carrier ID")?;
    let line_count_offset = new_order_take(bytes, &mut cursor, 1, "order line count")?;
    let all_local_offset = new_order_take(bytes, &mut cursor, 1, "order all-local flag")?;
    let entry_date = new_order_decode_uvarint32(bytes, &mut cursor, "order entry date")?;
    if cursor != bytes.len() {
        return Err(NewOrderCodecError::TrailingBytes("order"));
    }
    if !(1..=3_000).contains(&customer_id) {
        return Err(NewOrderCodecError::OutOfRange("order customer ID"));
    }

    let mut replacement_cursor = 0;
    new_order_append_i32(
        output,
        &mut replacement_cursor,
        customer_id,
        "order customer ID",
    )?;
    new_order_append_i32(
        output,
        &mut replacement_cursor,
        carrier_id,
        "order carrier ID",
    )?;
    new_order_append(
        output,
        &mut replacement_cursor,
        &[bytes[line_count_offset]],
        "order line count",
    )?;
    new_order_append(
        output,
        &mut replacement_cursor,
        &[u8::from(bytes[all_local_offset] != 0)],
        "order all-local flag",
    )?;
    new_order_append_u32(
        output,
        &mut replacement_cursor,
        entry_date,
        "order entry date",
    )?;
    Ok((replacement_cursor, customer_id))
}

fn delivery_order_line_replacement(
    bytes: &[u8],
    timestamp: u32,
    output: &mut [u8; DELIVERY_ORDER_LINE_VALUE_MAX],
) -> Result<(usize, f32), NewOrderCodecError> {
    let mut cursor = 0;
    let item_id = new_order_decode_i32(bytes, &mut cursor, "order-line item ID")?;
    new_order_decode_uvarint32(bytes, &mut cursor, "order-line delivery date")?;
    let amount_offset = new_order_take(bytes, &mut cursor, 4, "order-line amount")?;
    let amount = f32::from_ne_bytes(
        bytes[amount_offset..amount_offset + 4]
            .try_into()
            .expect("the checked order-line amount has four bytes"),
    );
    let supply_warehouse = new_order_decode_i32(bytes, &mut cursor, "order-line supply warehouse")?;
    let quantity_offset = new_order_take(bytes, &mut cursor, 1, "order-line quantity")?;
    if cursor != bytes.len() {
        return Err(NewOrderCodecError::TrailingBytes("order-line"));
    }

    let mut replacement_cursor = 0;
    new_order_append_i32(
        output,
        &mut replacement_cursor,
        item_id,
        "order-line item ID",
    )?;
    new_order_append_u32(
        output,
        &mut replacement_cursor,
        timestamp,
        "order-line delivery date",
    )?;
    new_order_append(
        output,
        &mut replacement_cursor,
        &amount.to_ne_bytes(),
        "order-line amount",
    )?;
    new_order_append_i32(
        output,
        &mut replacement_cursor,
        supply_warehouse,
        "order-line supply warehouse",
    )?;
    new_order_append(
        output,
        &mut replacement_cursor,
        &[bytes[quantity_offset]],
        "order-line quantity",
    )?;
    Ok((replacement_cursor, amount))
}

#[allow(
    unsafe_code,
    reason = "the trusted scan exposes each row only for the synchronous Rust callback"
)]
fn delivery_select_new_order(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    warehouse_id: i32,
    district_id: i32,
    cursor: i32,
) -> FfiResult<Option<DeliverySelectedNewOrder>> {
    let lower = new_order_key3(warehouse_id, district_id, cursor);
    let upper = new_order_key3(warehouse_id, district_id, i32::MAX);
    let request = ScanRequest::new(ScanDirection::Forward, 1)
        .with_lower(ScanBound::Included(&lower))
        .with_upper(ScanBound::Excluded(&upper));
    let mut selected = None;
    let mut malformed_key = false;
    let access = {
        let native_worker = &handle.native_worker;
        let scratch = &mut handle.scan_scratch;
        let transaction = active_transaction(&mut handle.active)?;
        // SAFETY: Delivery retains no row bytes or pointers beyond the
        // callback, and every FFI table uses the private direct-token mode.
        unsafe {
            table.state.table.visit_scan_bytes_trusted_with_scratch(
                transaction,
                native_worker,
                request,
                scratch,
                |row| {
                    let key = row.key();
                    if key.len() != 12
                        || key[..4] != warehouse_id.to_be_bytes()
                        || key[4..8] != district_id.to_be_bytes()
                    {
                        malformed_key = true;
                    } else {
                        let order_id = i32::from_be_bytes(
                            key[8..]
                                .try_into()
                                .expect("the checked new-order key suffix has four bytes"),
                        );
                        selected = Some(DeliverySelectedNewOrder {
                            order_id,
                            resolved: row.resolved(),
                        });
                    }
                    ScanControl::Stop
                },
            )
        }
    };
    if let Err(error) = access {
        return Err(status_from_access("Delivery new-order scan", error));
    }
    if malformed_key {
        return Err(fatal(format_args!(
            "Delivery new-order scan returned a malformed key"
        )));
    }
    if selected.is_some_and(|row| row.order_id == i32::MAX) {
        return Err(fatal(format_args!(
            "Delivery new-order scan returned an excluded maximum order ID"
        )));
    }
    Ok(selected)
}

fn delivery_read_oorder(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    key: &[u8; 12],
    carrier_id: i32,
) -> FfiResult<DeliveryOrderHeader> {
    let mut replacement = [0_u8; DELIVERY_OORDER_VALUE_MAX];
    let mut decoded = None;
    let mut codec_error = None;
    let access = {
        let transaction = active_transaction(&mut handle.active)?;
        table.state.table.visit_get_resolving_bytes(
            transaction,
            &handle.native_worker,
            key,
            |current| {
                if let Some(bytes) = current {
                    match delivery_oorder_replacement(bytes, carrier_id, &mut replacement) {
                        Ok((replacement_length, customer_id)) => {
                            decoded = Some((replacement_length, customer_id));
                        }
                        Err(error) => codec_error = Some(error),
                    }
                }
            },
        )
    };
    let (_, resolved) = match access {
        Ok(value) => value,
        Err(error) => return Err(status_from_access("Delivery order read", error)),
    };
    if let Some(error) = codec_error {
        return Err(fatal(format_args!("Delivery order row: {error}")));
    }
    let Some((replacement_length, customer_id)) = decoded else {
        set_last_error(format_args!("Delivery order read: required row is missing"));
        return Err(Status::Retry);
    };
    Ok(DeliveryOrderHeader {
        customer_id,
        resolved,
        replacement,
        replacement_length,
    })
}

#[allow(
    unsafe_code,
    reason = "the private value-only scan retains only resolved tokens and copied replacements"
)]
fn delivery_read_order_lines(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    lower: &[u8; 16],
    upper: &[u8; 16],
    timestamp: u32,
) -> FfiResult<DeliveryOrderLines> {
    let mut lines = DeliveryOrderLines::new();
    let mut codec_error = None;
    let access = {
        let native_worker = &handle.native_worker;
        let scratch = &mut handle.scan_scratch;
        let transaction = active_transaction(&mut handle.active)?;
        // SAFETY: Values are parsed synchronously and copied to fixed stack
        // storage. ResolvedRecord is an owned, table-bound stable token.
        unsafe {
            table
                .state
                .table
                .visit_bounded_forward_values_trusted_with_scratch(
                    transaction,
                    native_worker,
                    lower,
                    upper,
                    DELIVERY_MAX_LINES_PER_DISTRICT,
                    scratch,
                    |value, resolved| {
                        let index = lines.count;
                        debug_assert!(index < DELIVERY_MAX_LINES_PER_DISTRICT);
                        match delivery_order_line_replacement(
                            value,
                            timestamp,
                            &mut lines.replacements[index],
                        ) {
                            Ok((length, amount)) => {
                                lines.resolved[index] = Some(resolved);
                                lines.replacement_lengths[index] = length;
                                lines.count += 1;
                                lines.total += amount;
                                ScanControl::Continue
                            }
                            Err(error) => {
                                codec_error = Some((index, error));
                                ScanControl::Stop
                            }
                        }
                    },
                )
        }
    };
    if let Err(error) = access {
        return Err(status_from_access("Delivery order-line scan", error));
    }
    if let Some((index, error)) = codec_error {
        return Err(fatal(format_args!(
            "Delivery order-line scan row {index}: {error}"
        )));
    }
    Ok(lines)
}

fn delivery_put_required_resolved(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    resolved: ResolvedRecord,
    value: &[u8],
    operation: &'static str,
) -> FfiResult<()> {
    let access = {
        let transaction = active_transaction(&mut handle.active)?;
        table
            .state
            .table
            .put_resolved_with_previous_presence(transaction, resolved, value)
    };
    match access {
        Ok(true) => Ok(()),
        Ok(false) => {
            set_last_error(format_args!(
                "{operation}: scanned row became absent before its update"
            ));
            Err(Status::Retry)
        }
        Err(error) => Err(status_from_access(operation, error)),
    }
}

fn delivery_remove_required_resolved(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    resolved: ResolvedRecord,
) -> FfiResult<()> {
    let access = {
        let transaction = active_transaction(&mut handle.active)?;
        table
            .state
            .table
            .remove_resolved_with_previous_presence(transaction, resolved)
    };
    match access {
        Ok(true) => {
            record_size_delta_after_staging(handle, &table.state, -1)?;
            Ok(())
        }
        Ok(false) => {
            set_last_error(format_args!(
                "Delivery new-order remove: scanned row became absent"
            ));
            Err(Status::Retry)
        }
        Err(error) => Err(status_from_access("Delivery new-order remove", error)),
    }
}

fn delivery_update_customer_balance(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    key: &[u8; 12],
    order_line_total: f32,
) -> FfiResult<()> {
    let mut replacement = [0_u8; mem::size_of::<f32>()];
    let mut found = false;
    let access = {
        let transaction = active_transaction(&mut handle.active)?;
        table.state.table.visit_get_resolving_bytes(
            transaction,
            &handle.native_worker,
            key,
            |current| {
                let Some(bytes) = current else {
                    return;
                };
                if bytes.len() != mem::size_of::<f32>() {
                    return;
                }
                let balance = f32::from_ne_bytes(
                    bytes
                        .try_into()
                        .expect("the checked customer balance has four bytes"),
                );
                replacement.copy_from_slice(&(balance + order_line_total).to_ne_bytes());
                found = true;
            },
        )
    };
    let (_, resolved) = match access {
        Ok(value) => value,
        Err(error) => return Err(status_from_access("Delivery customer-balance read", error)),
    };
    if !found {
        return Err(fatal(format_args!(
            "Delivery customer-balance read: required four-byte row is missing or malformed"
        )));
    }
    delivery_put_required_resolved(
        handle,
        table,
        resolved,
        &replacement,
        "Delivery customer-balance update",
    )
}

#[allow(
    unsafe_code,
    reason = "the private wrapper supplies live, same-runtime handles and a writable cursor array"
)]
unsafe fn delivery_full_body(
    guard: &mut PaymentAttemptGuard<'_>,
    request_pointer: *const MakoStoTpccDeliveryFullRequest,
    result_pointer: *mut MakoStoTpccDeliveryFullResult,
) -> FfiResult<Status> {
    let request_range = payment_pointer_range(request_pointer, 1, "full Delivery request")?;
    let result_range =
        payment_pointer_range(result_pointer.cast_const(), 1, "full Delivery result")?;
    if request_range.overlaps(result_range) {
        return Err(fatal(format_args!(
            "full Delivery request must not overlap its result"
        )));
    }
    // SAFETY: The private wrapper keeps this aligned request allocation live
    // and readable for the complete call.
    let request = unsafe { *request_pointer };
    if request.warehouse_id <= 0 {
        return Err(fatal(format_args!(
            "Delivery warehouse ID must be positive"
        )));
    }
    if !(1..=10).contains(&request.carrier_id) {
        return Err(fatal(format_args!(
            "Delivery carrier ID must be between 1 and 10"
        )));
    }

    let cursor_range = payment_pointer_range(
        request.last_no_o_ids.cast_const(),
        DELIVERY_DISTRICT_COUNT,
        "Delivery cursor array",
    )?;
    if cursor_range.overlaps(request_range) || cursor_range.overlaps(result_range) {
        return Err(fatal(format_args!(
            "Delivery cursor array must not overlap its request or result"
        )));
    }

    let handle = &mut *guard.handle;
    let new_order = unsafe {
        payment_table_owned(handle, request.new_order_table, "Delivery new-order table")?
    };
    let oorder =
        unsafe { payment_table_owned(handle, request.oorder_table, "Delivery order table")? };
    let order_line = unsafe {
        payment_table_owned(
            handle,
            request.order_line_table,
            "Delivery order-line table",
        )?
    };
    let customer = unsafe {
        payment_table_owned(
            handle,
            request.customer_table,
            "Delivery customer-balance table",
        )?
    };

    // SAFETY: The range validation proves that the wrapper-owned allocation
    // contains exactly ten aligned i32 slots. It is disjoint from the copied
    // request and result. Cursor writes intentionally outlive transaction
    // rollback, matching the scalar Delivery worker.
    let cursors =
        unsafe { slice::from_raw_parts_mut(request.last_no_o_ids, DELIVERY_DISTRICT_COUNT) };
    let mut delivered_districts = 0_u32;
    let mut updated_order_lines = 0_u32;
    for (district_index, cursor) in cursors.iter_mut().enumerate() {
        let district_id =
            i32::try_from(district_index + 1).expect("the fixed Delivery district count fits i32");
        let Some(selected) = delivery_select_new_order(
            handle,
            new_order,
            request.warehouse_id,
            district_id,
            *cursor,
        )?
        else {
            continue;
        };
        let next_cursor = selected.order_id.checked_add(1).ok_or_else(|| {
            fatal(format_args!(
                "Delivery selected an order ID whose cursor would overflow"
            ))
        })?;
        *cursor = next_cursor;

        let order_key = new_order_key3(request.warehouse_id, district_id, selected.order_id);
        let order = delivery_read_oorder(handle, oorder, &order_key, request.carrier_id)?;
        let order_line_lower =
            new_order_key4(request.warehouse_id, district_id, selected.order_id, 0);
        let order_line_upper = new_order_key4(
            request.warehouse_id,
            district_id,
            selected.order_id,
            i32::MAX,
        );
        let lines = delivery_read_order_lines(
            handle,
            order_line,
            &order_line_lower,
            &order_line_upper,
            request.timestamp,
        )?;
        for index in 0..lines.count {
            delivery_put_required_resolved(
                handle,
                order_line,
                lines.resolved[index]
                    .expect("every parsed Delivery order line retains its resolved token"),
                &lines.replacements[index][..lines.replacement_lengths[index]],
                "Delivery order-line update",
            )?;
        }

        delivery_remove_required_resolved(handle, new_order, selected.resolved)?;
        delivery_put_required_resolved(
            handle,
            oorder,
            order.resolved,
            &order.replacement[..order.replacement_length],
            "Delivery order update",
        )?;
        let customer_district_id = district_id
            .checked_add(200)
            .expect("the fixed Delivery district offset fits i32");
        let customer_key = new_order_key3(
            request.warehouse_id,
            customer_district_id,
            order.customer_id,
        );
        delivery_update_customer_balance(handle, customer, &customer_key, lines.total)?;

        delivered_districts = delivered_districts
            .checked_add(1)
            .ok_or_else(|| fatal(format_args!("Delivery district accounting overflow")))?;
        updated_order_lines = updated_order_lines
            .checked_add(
                u32::try_from(lines.count).expect("the bounded Delivery line count fits u32"),
            )
            .ok_or_else(|| fatal(format_args!("Delivery line accounting overflow")))?;
    }

    let commit_status = txn_commit_impl(handle)?;
    if commit_status != Status::Ok {
        return Ok(commit_status);
    }
    // SAFETY: The aligned result allocation was proven disjoint from the
    // request and externally mutated cursor array before transaction work.
    unsafe {
        ptr::write(
            result_pointer,
            MakoStoTpccDeliveryFullResult {
                reported_value_bytes: 0,
                delivered_districts,
                updated_order_lines,
            },
        )
    };
    guard.disarm();
    Ok(Status::Ok)
}

struct StockLevelItemSet {
    slots: [u32; STOCK_LEVEL_ITEM_SET_SLOTS],
    items: [u32; STOCK_LEVEL_MAX_ORDER_LINE_ROWS],
    len: usize,
}

impl StockLevelItemSet {
    fn new() -> Self {
        Self {
            slots: [0; STOCK_LEVEL_ITEM_SET_SLOTS],
            items: [0; STOCK_LEVEL_MAX_ORDER_LINE_ROWS],
            len: 0,
        }
    }

    fn insert(&mut self, item_id: u32) -> Result<bool, ()> {
        debug_assert_ne!(item_id, 0);
        debug_assert!(STOCK_LEVEL_ITEM_SET_SLOTS.is_power_of_two());
        let mut slot =
            item_id.wrapping_mul(0x9e37_79b1) as usize & (STOCK_LEVEL_ITEM_SET_SLOTS - 1);
        for _ in 0..STOCK_LEVEL_ITEM_SET_SLOTS {
            match self.slots[slot] {
                0 => {
                    if self.len == self.items.len() {
                        return Err(());
                    }
                    self.slots[slot] = item_id;
                    self.items[self.len] = item_id;
                    self.len += 1;
                    return Ok(true);
                }
                current if current == item_id => return Ok(false),
                _ => slot = (slot + 1) & (STOCK_LEVEL_ITEM_SET_SLOTS - 1),
            }
        }
        Err(())
    }

    fn as_slice(&self) -> &[u32] {
        &self.items[..self.len]
    }
}

#[allow(
    unsafe_code,
    reason = "the private keyless scan parses values synchronously and retains no native pointer"
)]
fn stock_level_scan_item_ids(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    lower: &[u8; 16],
    upper: &[u8; 16],
) -> FfiResult<(StockLevelItemSet, u32)> {
    let mut items = StockLevelItemSet::new();
    let mut scanned_rows = 0_u32;
    let mut codec_error = None;
    let mut set_exhausted = false;
    let access = {
        let native_worker = &handle.native_worker;
        let scratch = &mut handle.scan_scratch;
        let transaction = active_transaction(&mut handle.active)?;
        // SAFETY: The callback retains only a decoded integer in fixed Rust
        // storage. It neither retains value bytes nor re-enters the table.
        unsafe {
            table
                .state
                .table
                .visit_bounded_forward_values_trusted_with_scratch(
                    transaction,
                    native_worker,
                    lower,
                    upper,
                    STOCK_LEVEL_MAX_ORDER_LINE_ROWS,
                    scratch,
                    |value, _resolved| {
                        let mut cursor = 0;
                        match new_order_decode_i32(
                            value,
                            &mut cursor,
                            "StockLevel order-line item ID",
                        ) {
                            Ok(item_id) if (1..=100_000).contains(&item_id) => {
                                if items.insert(item_id as u32).is_err() {
                                    set_exhausted = true;
                                    return ScanControl::Stop;
                                }
                                scanned_rows += 1;
                                ScanControl::Continue
                            }
                            Ok(_) => {
                                codec_error = Some(NewOrderCodecError::OutOfRange(
                                    "StockLevel order-line item ID",
                                ));
                                ScanControl::Stop
                            }
                            Err(error) => {
                                codec_error = Some(error);
                                ScanControl::Stop
                            }
                        }
                    },
                )
        }
    };
    if let Err(error) = access {
        return Err(status_from_access("StockLevel order-line scan", error));
    }
    if set_exhausted {
        return Err(fatal(format_args!(
            "StockLevel item-ID set exhausted its fixed capacity"
        )));
    }
    if let Some(error) = codec_error {
        return Err(fatal(format_args!("StockLevel order-line scan: {error}")));
    }
    Ok((items, scanned_rows))
}

#[inline(always)]
fn stock_level_observe_quantity(
    current: Option<&[u8]>,
    threshold: i32,
) -> StockLevelQuantityObservation {
    let Some(bytes) = current else {
        return StockLevelQuantityObservation::Missing;
    };
    let Some(quantity_bytes) = bytes.get(..mem::size_of::<i16>()) else {
        return StockLevelQuantityObservation::Malformed;
    };
    let quantity = i16::from_ne_bytes(
        quantity_bytes
            .try_into()
            .expect("the checked StockLevel quantity has two bytes"),
    );
    if i32::from(quantity) < threshold {
        StockLevelQuantityObservation::Low
    } else {
        StockLevelQuantityObservation::Present
    }
}

#[derive(Clone, Copy)]
enum StockLevelQuantityObservation {
    Unvisited,
    Present,
    Low,
    Missing,
    Malformed,
}

#[inline(always)]
fn dense_cache_for_policy<'table>(
    table: &'table StoTpccTable,
    expected: DenseCachePolicy,
    operation: &'static str,
) -> FfiResult<Option<&'table DenseResolvedCache>> {
    if table.dense_policy != expected {
        return Ok(None);
    }
    table
        .dense_cache
        .as_ref()
        .map(Some)
        .ok_or_else(|| fatal(format_args!("{operation}: dense cache is missing")))
}

#[inline(always)]
fn dense_stock_cache_for_warehouse<'table>(
    table: &'table StoTpccTable,
    warehouse_id: i32,
    operation: &'static str,
) -> FfiResult<Option<&'table DenseResolvedCache>> {
    let Some(cache) = dense_cache_for_policy(table, DenseCachePolicy::Stock, operation)? else {
        return Ok(None);
    };
    if warehouse_id <= 0 {
        table.dense_stock_warehouse_id.store(-1, Ordering::Release);
        return Ok(None);
    }
    let mut bound = table.dense_stock_warehouse_id.load(Ordering::Acquire);
    loop {
        if bound == warehouse_id {
            return Ok(Some(cache));
        }
        if bound == -1 {
            return Ok(None);
        }
        let replacement = if bound == 0 { warehouse_id } else { -1 };
        match table.dense_stock_warehouse_id.compare_exchange_weak(
            bound,
            replacement,
            Ordering::AcqRel,
            Ordering::Acquire,
        ) {
            Ok(_) if replacement == warehouse_id => return Ok(Some(cache)),
            Ok(_) => return Ok(None),
            Err(observed) => bound = observed,
        }
    }
}

#[inline(always)]
fn dense_item_slot(item_id: u32) -> FfiResult<usize> {
    if !(1..=DENSE_TPCC_ITEM_SLOTS as u32).contains(&item_id) {
        return Err(fatal(format_args!(
            "dense TPC-C cache item ID {item_id} is out of range"
        )));
    }
    Ok(item_id as usize - 1)
}

fn stock_level_count_low_stock_worker_cache(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    warehouse_id: i32,
    items: &StockLevelItemSet,
    threshold: i32,
) -> FfiResult<u32> {
    if table.cache_policy != ResolvedCachePolicy::Full {
        return Err(fatal(format_args!(
            "StockLevel stock table must use the full resolved cache policy"
        )));
    }
    let item_ids = items.as_slice();
    if item_ids.is_empty() {
        #[cfg(test)]
        STOCK_LEVEL_CACHE_PARTITION.with(|partition| partition.set((0, 0)));
        return Ok(0);
    }
    let mut keys = [[0_u8; 8]; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    for (key, item_id) in keys.iter_mut().zip(item_ids.iter().copied()) {
        key[..4].copy_from_slice(&warehouse_id.to_be_bytes());
        key[4..].copy_from_slice(&item_id.to_be_bytes());
    }

    // StockLevel's set has already removed duplicate item IDs. Retain exact
    // cache hits as stable tokens and compact only misses into the native
    // fixed lookup. The miss batch runs first so it can still use the typed
    // unique-item lane before scalar resolved hits create stock items.
    let mut cached = [None; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    let mut miss_keys = [[0_u8; 8]; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    let mut miss_indices = [0_usize; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    let empty_probe = ResolvedCacheProbe {
        record: None,
        miss_slot: None,
    };
    let mut miss_probes = [empty_probe; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    let mut miss_count = 0_usize;
    for (index, key) in keys[..item_ids.len()].iter().enumerate() {
        let probe = handle.resolved_cache.probe(&table.state.table, key);
        if let Some(resolved) = probe.record {
            cached[index] = Some(resolved);
        } else {
            miss_keys[miss_count] = *key;
            miss_indices[miss_count] = index;
            miss_probes[miss_count] = probe;
            miss_count += 1;
        }
    }
    #[cfg(test)]
    STOCK_LEVEL_CACHE_PARTITION.with(|partition| {
        partition.set((item_ids.len() - miss_count, miss_count));
    });

    let mut observations =
        [StockLevelQuantityObservation::Unvisited; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    let native_worker = &handle.native_worker;
    let point_batch = &mut handle.point_batch;
    let resolved_cache = &mut handle.resolved_cache;
    let transaction = active_transaction(&mut handle.active)?;
    let access: Result<(), AccessError> = (|| {
        if miss_count != 0 {
            let mut session = table.state.table.point_session(transaction, native_worker);
            session.visit_fixed_resolving_bytes(
                &miss_keys[..miss_count],
                point_batch,
                |miss_index, current, resolved| {
                    let original_index = miss_indices[miss_index];
                    resolved_cache.remember_after_probe(
                        &table.state.table,
                        &miss_keys[miss_index],
                        resolved,
                        miss_probes[miss_index],
                    );
                    observations[original_index] = stock_level_observe_quantity(current, threshold);
                },
            )?;
        }

        for (index, resolved) in cached[..item_ids.len()].iter().copied().enumerate() {
            let Some(resolved) = resolved else {
                continue;
            };
            table
                .state
                .table
                .visit_get_resolved_bytes(transaction, resolved, |current| {
                    observations[index] = stock_level_observe_quantity(current, threshold);
                })?;
        }
        Ok(())
    })();
    if let Err(error) = access {
        return Err(status_from_access("StockLevel stock batch", error));
    }

    // The compact miss batch deliberately runs before cache hits. Reduce its
    // captured outcomes in original item order so mixed hit/miss failures
    // retain the scalar batch's first-error diagnostic while every item has
    // still entered the OCC read set.
    let mut low_stock_count = 0_u32;
    for (index, observation) in observations[..item_ids.len()].iter().copied().enumerate() {
        match observation {
            StockLevelQuantityObservation::Present => {}
            StockLevelQuantityObservation::Low => low_stock_count += 1,
            StockLevelQuantityObservation::Missing => {
                set_last_error(format_args!(
                    "StockLevel stock batch: required row {index} is missing"
                ));
                return Err(Status::Retry);
            }
            StockLevelQuantityObservation::Malformed => {
                set_last_error(format_args!(
                    "StockLevel stock batch: row {index} has a truncated quantity"
                ));
                return Err(Status::Retry);
            }
            StockLevelQuantityObservation::Unvisited => {
                return Err(fatal(format_args!(
                    "StockLevel stock batch did not visit row {index}"
                )));
            }
        }
    }
    Ok(low_stock_count)
}

fn stock_level_count_low_stock_dense(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    warehouse_id: i32,
    items: &StockLevelItemSet,
    threshold: i32,
) -> FfiResult<u32> {
    let item_ids = items.as_slice();
    if item_ids.is_empty() {
        #[cfg(test)]
        STOCK_LEVEL_CACHE_PARTITION.with(|partition| partition.set((0, 0)));
        return Ok(0);
    }
    let cache = dense_stock_cache_for_warehouse(table, warehouse_id, "StockLevel stock batch")?;
    let mut keys = [[0_u8; 8]; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    let mut hints = [None; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    let mut dense_slots = [0_usize; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    let mut missing_keys = [[0_u8; 8]; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    let mut missing_positions = [0_usize; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    let mut miss_count = 0_usize;
    for (index, item_id) in item_ids.iter().copied().enumerate() {
        keys[index][..4].copy_from_slice(&warehouse_id.to_be_bytes());
        keys[index][4..].copy_from_slice(&item_id.to_be_bytes());
        dense_slots[index] = dense_item_slot(item_id)?;
        hints[index] = match cache {
            Some(cache) => cache
                .get(dense_slots[index])
                .map_err(|error| status_from_access("StockLevel dense stock cache", error))?,
            None => None,
        };
        if hints[index].is_none() {
            missing_keys[miss_count] = keys[index];
            missing_positions[miss_count] = index;
            miss_count += 1;
        }
    }
    #[cfg(test)]
    STOCK_LEVEL_CACHE_PARTITION.with(|partition| {
        partition.set((item_ids.len() - miss_count, miss_count));
    });

    let mut observations =
        [StockLevelQuantityObservation::Unvisited; STOCK_LEVEL_MAX_ORDER_LINE_ROWS];
    let mut cache_error = None;
    let transaction = active_transaction(&mut handle.active)?;
    let mut session = table
        .state
        .table
        .point_session(transaction, &handle.native_worker);
    let access = session.visit_fixed_hinted_bytes(
        &keys[..item_ids.len()],
        &hints[..item_ids.len()],
        &missing_keys[..miss_count],
        &missing_positions[..miss_count],
        &mut handle.point_batch,
        |index, current, resolved| {
            if hints[index].is_none() {
                if let Some(cache) = cache {
                    if let Err(error) = cache.remember(dense_slots[index], resolved) {
                        cache_error.get_or_insert(error);
                    }
                }
            }
            observations[index] = stock_level_observe_quantity(current, threshold);
        },
    );
    if let Err(error) = access {
        return Err(status_from_access("StockLevel stock batch", error));
    }
    if let Some(error) = cache_error {
        return Err(status_from_access("StockLevel dense stock cache", error));
    }

    let mut low_stock_count = 0_u32;
    for (index, observation) in observations[..item_ids.len()].iter().copied().enumerate() {
        match observation {
            StockLevelQuantityObservation::Present => {}
            StockLevelQuantityObservation::Low => low_stock_count += 1,
            StockLevelQuantityObservation::Missing => {
                set_last_error(format_args!(
                    "StockLevel stock batch: required row {index} is missing"
                ));
                return Err(Status::Retry);
            }
            StockLevelQuantityObservation::Malformed => {
                set_last_error(format_args!(
                    "StockLevel stock batch: row {index} has a truncated quantity"
                ));
                return Err(Status::Retry);
            }
            StockLevelQuantityObservation::Unvisited => {
                return Err(fatal(format_args!(
                    "StockLevel stock batch did not visit row {index}"
                )));
            }
        }
    }
    Ok(low_stock_count)
}

fn stock_level_count_low_stock(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    warehouse_id: i32,
    items: &StockLevelItemSet,
    threshold: i32,
) -> FfiResult<u32> {
    match (table.cache_policy, table.dense_policy) {
        (ResolvedCachePolicy::Full, DenseCachePolicy::None) => {
            stock_level_count_low_stock_worker_cache(handle, table, warehouse_id, items, threshold)
        }
        (_, DenseCachePolicy::Stock) => {
            stock_level_count_low_stock_dense(handle, table, warehouse_id, items, threshold)
        }
        _ => Err(fatal(format_args!(
            "StockLevel stock table must use the full or dense-stock resolved cache policy"
        ))),
    }
}

#[allow(
    unsafe_code,
    reason = "the private wrapper supplies live same-runtime handles and disjoint request/result storage"
)]
unsafe fn stock_level_full_body(
    guard: &mut PaymentAttemptGuard<'_>,
    request_pointer: *const MakoStoTpccStockLevelFullRequest,
    result_pointer: *mut MakoStoTpccStockLevelFullResult,
) -> FfiResult<Status> {
    let request_range = payment_pointer_range(request_pointer, 1, "full StockLevel request")?;
    let result_range =
        payment_pointer_range(result_pointer.cast_const(), 1, "full StockLevel result")?;
    if request_range.overlaps(result_range) {
        return Err(fatal(format_args!(
            "full StockLevel request must not overlap its result"
        )));
    }
    // SAFETY: The private wrapper keeps the validated request live and
    // readable for this synchronous call.
    let request = unsafe { *request_pointer };
    if request.warehouse_id <= 0 {
        return Err(fatal(format_args!(
            "StockLevel warehouse ID must be positive"
        )));
    }
    if !(1..=10).contains(&request.district_id) {
        return Err(fatal(format_args!(
            "StockLevel district ID must be between 1 and 10"
        )));
    }
    let threshold = i32::try_from(request.threshold)
        .map_err(|_| fatal(format_args!("StockLevel threshold exceeds i32")))?;

    let handle = &mut *guard.handle;
    let order_line = unsafe {
        payment_table_owned(
            handle,
            request.order_line_table,
            "StockLevel order-line table",
        )?
    };
    let stock =
        unsafe { payment_table_owned(handle, request.stock_table, "StockLevel stock table")? };

    // Match the scalar C++ conditional subtraction and narrowing into the
    // signed order-line key fields. Valid TPC-C order IDs fit i32.
    let lower_order_id = request.current_next_order_id.saturating_sub(20) as i32;
    let upper_order_id = request.current_next_order_id as i32;
    let lower = new_order_key4(request.warehouse_id, request.district_id, lower_order_id, 0);
    let upper = new_order_key4(request.warehouse_id, request.district_id, upper_order_id, 0);
    let (item_ids, scanned_order_line_rows) =
        stock_level_scan_item_ids(handle, order_line, &lower, &upper)?;
    let low_stock_count =
        stock_level_count_low_stock(handle, stock, request.warehouse_id, &item_ids, threshold)?;
    let distinct_item_ids =
        u32::try_from(item_ids.len).expect("the bounded StockLevel distinct count fits u32");

    let commit_status = txn_commit_impl(handle)?;
    if commit_status != Status::Ok {
        return Ok(commit_status);
    }
    // SAFETY: The aligned result was validated and proven disjoint from the
    // request before transaction work, and is written only after commit.
    unsafe {
        ptr::write(
            result_pointer,
            MakoStoTpccStockLevelFullResult {
                reported_value_bytes: 0,
                scanned_order_line_rows,
                distinct_item_ids,
                low_stock_count,
            },
        )
    };
    guard.disarm();
    Ok(Status::Ok)
}

fn new_order_require_cached_present(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    key: &[u8],
    operation: &'static str,
) -> FfiResult<()> {
    if table.cache_policy != ResolvedCachePolicy::Full {
        return Err(fatal(format_args!(
            "{operation}: table must use the full resolved cache policy"
        )));
    }
    let probe = handle.resolved_cache.probe(&table.state.table, key);
    let access = {
        let transaction = active_transaction(&mut handle.active)?;
        match probe.record {
            Some(resolved) => table
                .state
                .table
                .contains_resolved(transaction, resolved)
                .map(|present| (present, None)),
            None => table
                .state
                .table
                .contains_resolving(transaction, &handle.native_worker, key)
                .map(|(present, resolved)| (present, Some(resolved))),
        }
    };
    let present = match access {
        Ok((present, Some(resolved))) => {
            handle
                .resolved_cache
                .remember_after_probe(&table.state.table, key, resolved, probe);
            present
        }
        Ok((present, None)) => present,
        Err(error) => return Err(status_from_access(operation, error)),
    };
    if !present {
        return Err(fatal(format_args!("{operation}: required row is missing")));
    }
    Ok(())
}

fn new_order_read_items(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    item_ids: &[u32],
) -> FfiResult<[f32; NEW_ORDER_MAX_LINES]> {
    let dense_cache =
        dense_cache_for_policy(table, DenseCachePolicy::Item, "new-order item batch")?;
    let mut keys = [[0_u8; 4]; NEW_ORDER_MAX_LINES];
    let mut hints = [None; NEW_ORDER_MAX_LINES];
    let mut dense_slots = [0_usize; NEW_ORDER_MAX_LINES];
    let mut missing_keys = [[0_u8; 4]; NEW_ORDER_MAX_LINES];
    let mut missing_positions = [0_usize; NEW_ORDER_MAX_LINES];
    let mut miss_count = 0_usize;
    for (index, item_id) in item_ids.iter().copied().enumerate() {
        keys[index] = item_id.to_be_bytes();
        dense_slots[index] = dense_item_slot(item_id)?;
        if let Some(cache) = dense_cache {
            hints[index] = cache
                .get(dense_slots[index])
                .map_err(|error| status_from_access("new-order dense item cache", error))?;
        }
        if hints[index].is_none() {
            missing_keys[miss_count] = keys[index];
            missing_positions[miss_count] = index;
            miss_count += 1;
        }
    }
    let mut prices = [0_f32; NEW_ORDER_MAX_LINES];
    let mut missing = None;
    let mut codec_error = None;
    let mut cache_error = None;
    let access = {
        let transaction = active_transaction(&mut handle.active)?;
        let mut session = table
            .state
            .table
            .point_session(transaction, &handle.native_worker);
        session.visit_fixed_hinted_bytes(
            &keys[..item_ids.len()],
            &hints[..item_ids.len()],
            &missing_keys[..miss_count],
            &missing_positions[..miss_count],
            &mut handle.point_batch,
            |index, current, resolved| {
                if hints[index].is_none() {
                    if let Some(cache) = dense_cache {
                        if let Err(error) = cache.remember(dense_slots[index], resolved) {
                            cache_error.get_or_insert(error);
                        }
                    }
                }
                if missing.is_some() || codec_error.is_some() {
                    return;
                }
                let Some(bytes) = current else {
                    missing = Some(index);
                    return;
                };
                match new_order_item_price(bytes) {
                    Ok(price) => prices[index] = price,
                    Err(error) => codec_error = Some((index, error)),
                }
            },
        )
    };
    if let Err(error) = access {
        return Err(status_from_access("new-order item batch", error));
    }
    if let Some(error) = cache_error {
        return Err(status_from_access("new-order dense item cache", error));
    }
    if let Some(index) = missing {
        set_last_error(format_args!(
            "new-order item batch: required row {index} is missing"
        ));
        return Err(Status::Retry);
    }
    if let Some((index, error)) = codec_error {
        set_last_error(format_args!("new-order item batch row {index}: {error}"));
        return Err(if matches!(error, NewOrderCodecError::OutOfRange(_)) {
            Status::Fatal
        } else {
            Status::Retry
        });
    }
    Ok(prices)
}

fn new_order_modify_stocks_worker_cache(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    warehouse_id: i32,
    item_ids: &[u32],
    quantities: &[u32],
) -> FfiResult<()> {
    if table.cache_policy != ResolvedCachePolicy::Full {
        return Err(fatal(format_args!(
            "new-order stock table must use the full resolved cache policy"
        )));
    }
    let mut keys = [[0_u8; 8]; NEW_ORDER_MAX_LINES];
    for (index, item_id) in item_ids.iter().copied().enumerate() {
        keys[index][..4].copy_from_slice(&warehouse_id.to_be_bytes());
        keys[index][4..].copy_from_slice(&item_id.to_be_bytes());
    }
    let mut missing = None;
    let mut codec_error = None;
    let access = {
        let resolved_cache = &mut handle.resolved_cache;
        let transaction = active_transaction(&mut handle.active)?;
        let mut session = table
            .state
            .table
            .point_session(transaction, &handle.native_worker);
        session.modify_fixed_resolving_visit(
            &keys[..item_ids.len()],
            &mut handle.point_batch,
            |index, current, resolved| {
                resolved_cache.remember(&table.state.table, &keys[index], resolved);
                if missing.is_some() || codec_error.is_some() {
                    return PointMutation::Keep;
                }
                let Some(current) = current else {
                    missing = Some(index);
                    return PointMutation::Keep;
                };
                let mut replacement = [0_u8; NEW_ORDER_STOCK_VALUE_MAX];
                match new_order_stock_replacement(
                    current.as_ref(),
                    quantities[index],
                    &mut replacement,
                ) {
                    Ok(length) => PointMutation::Put(Value::from(&replacement[..length])),
                    Err(error) => {
                        codec_error = Some((index, error));
                        PointMutation::Keep
                    }
                }
            },
        )
    };
    if let Err(error) = access {
        return Err(status_from_access("new-order stock batch", error));
    }
    if let Some(index) = missing {
        set_last_error(format_args!(
            "new-order stock batch: required row {index} is missing"
        ));
        return Err(Status::Retry);
    }
    if let Some((index, error)) = codec_error {
        set_last_error(format_args!("new-order stock batch row {index}: {error}"));
        return Err(Status::Retry);
    }
    Ok(())
}

fn new_order_modify_stocks_dense(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    warehouse_id: i32,
    item_ids: &[u32],
    quantities: &[u32],
) -> FfiResult<()> {
    let dense_cache =
        dense_stock_cache_for_warehouse(table, warehouse_id, "new-order stock batch")?;
    let mut keys = [[0_u8; 8]; NEW_ORDER_MAX_LINES];
    let mut hints = [None; NEW_ORDER_MAX_LINES];
    let mut dense_slots = [0_usize; NEW_ORDER_MAX_LINES];
    let mut missing_keys = [[0_u8; 8]; NEW_ORDER_MAX_LINES];
    let mut missing_positions = [0_usize; NEW_ORDER_MAX_LINES];
    let mut miss_count = 0_usize;
    for (index, item_id) in item_ids.iter().copied().enumerate() {
        keys[index][..4].copy_from_slice(&warehouse_id.to_be_bytes());
        keys[index][4..].copy_from_slice(&item_id.to_be_bytes());
        dense_slots[index] = dense_item_slot(item_id)?;
        if let Some(cache) = dense_cache {
            hints[index] = cache
                .get(dense_slots[index])
                .map_err(|error| status_from_access("new-order dense stock cache", error))?;
        }
        if hints[index].is_none() {
            missing_keys[miss_count] = keys[index];
            missing_positions[miss_count] = index;
            miss_count += 1;
        }
    }

    let mut missing = None;
    let mut codec_error = None;
    let mut cache_error = None;
    let transaction = active_transaction(&mut handle.active)?;
    let mut session = table
        .state
        .table
        .point_session(transaction, &handle.native_worker);
    let access = session.modify_fixed_hinted_visit(
        &keys[..item_ids.len()],
        &hints[..item_ids.len()],
        &missing_keys[..miss_count],
        &missing_positions[..miss_count],
        &mut handle.point_batch,
        |index, current, resolved| {
            if hints[index].is_none() {
                if let Some(cache) = dense_cache {
                    if let Err(error) = cache.remember(dense_slots[index], resolved) {
                        cache_error.get_or_insert(error);
                    }
                }
            }
            if missing.is_some() || codec_error.is_some() {
                return PointMutation::Keep;
            }
            let Some(current) = current else {
                missing = Some(index);
                return PointMutation::Keep;
            };
            let mut replacement = [0_u8; NEW_ORDER_STOCK_VALUE_MAX];
            match new_order_stock_replacement(current.as_ref(), quantities[index], &mut replacement)
            {
                Ok(length) => PointMutation::Put(Value::from(&replacement[..length])),
                Err(error) => {
                    codec_error = Some((index, error));
                    PointMutation::Keep
                }
            }
        },
    );
    if let Err(error) = access {
        return Err(status_from_access("new-order stock batch", error));
    }
    if let Some(error) = cache_error {
        return Err(status_from_access("new-order dense stock cache", error));
    }
    if let Some(index) = missing {
        set_last_error(format_args!(
            "new-order stock batch: required row {index} is missing"
        ));
        return Err(Status::Retry);
    }
    if let Some((index, error)) = codec_error {
        set_last_error(format_args!("new-order stock batch row {index}: {error}"));
        return Err(Status::Retry);
    }
    Ok(())
}

fn new_order_modify_stocks(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    warehouse_id: i32,
    item_ids: &[u32],
    quantities: &[u32],
) -> FfiResult<()> {
    match (table.cache_policy, table.dense_policy) {
        (ResolvedCachePolicy::Full, DenseCachePolicy::None) => {
            new_order_modify_stocks_worker_cache(handle, table, warehouse_id, item_ids, quantities)
        }
        (_, DenseCachePolicy::Stock) => {
            new_order_modify_stocks_dense(handle, table, warehouse_id, item_ids, quantities)
        }
        _ => Err(fatal(format_args!(
            "new-order stock table must use the full or dense-stock resolved cache policy"
        ))),
    }
}

fn new_order_insert_header(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    key: &[u8],
    value: &[u8],
    operation: &'static str,
) -> FfiResult<()> {
    match insert_impl(handle, table, key, value)? {
        Status::Ok | Status::Duplicate => Ok(()),
        Status::Retry => Err(Status::Retry),
        status => Err(fatal(format_args!(
            "{operation}: insert returned unexpected status {}",
            status.code()
        ))),
    }
}

struct NewOrderLineBatch<'a> {
    warehouse_id: i32,
    district_id: i32,
    order_id: i32,
    item_ids: &'a [u32],
    quantities: &'a [u32],
    prices: &'a [f32; NEW_ORDER_MAX_LINES],
}

fn new_order_insert_order_lines(
    handle: &mut StoTpccThread,
    table: &StoTpccTable,
    batch: NewOrderLineBatch<'_>,
) -> FfiResult<usize> {
    let mut keys = [[0_u8; 16]; NEW_ORDER_MAX_LINES];
    let mut values = [[0_u8; NEW_ORDER_ORDER_LINE_VALUE_MAX]; NEW_ORDER_MAX_LINES];
    let mut lengths = [0_usize; NEW_ORDER_MAX_LINES];
    let mut reported_value_bytes = 0_usize;
    for index in 0..batch.item_ids.len() {
        keys[index] = new_order_key4(
            batch.warehouse_id,
            batch.district_id,
            batch.order_id,
            i32::try_from(index + 1).expect("the bounded line number fits i32"),
        );
        let amount = batch.quantities[index] as f32 * batch.prices[index];
        lengths[index] = new_order_order_line_value(
            batch.item_ids[index] as i32,
            amount,
            batch.warehouse_id,
            batch.quantities[index] as u8,
            &mut values[index],
        )
        .map_err(|error| fatal(format_args!("new-order order line: {error}")))?;
        reported_value_bytes = reported_value_bytes
            .checked_add(lengths[index])
            .ok_or_else(|| fatal(format_args!("new-order byte accounting overflow")))?;
    }

    let mut inserted = 0_i64;
    let mut duplicate = None;
    let access = {
        let transaction = active_transaction(&mut handle.active)?;
        let mut session = table
            .state
            .table
            .point_session(transaction, &handle.native_worker);
        session.modify_fixed_expected_absent_visit(
            &keys[..batch.item_ids.len()],
            &mut handle.point_batch,
            |index, current| {
                if current.is_some() {
                    duplicate.get_or_insert(index);
                    PointMutation::Keep
                } else {
                    inserted += 1;
                    PointMutation::Put(Value::from(&values[index][..lengths[index]]))
                }
            },
        )
    };
    if let Err(error) = access {
        return Err(status_from_access("new-order order-line batch", error));
    }
    if let Some(index) = duplicate {
        set_last_error(format_args!(
            "new-order order-line batch: duplicate row at input {index}"
        ));
        return Err(Status::Retry);
    }
    if inserted != 0 {
        record_size_delta_after_staging(handle, &table.state, inserted)?;
    }
    Ok(reported_value_bytes)
}

unsafe fn new_order_full_body(
    guard: &mut PaymentAttemptGuard<'_>,
    request_pointer: *const MakoStoTpccNewOrderFullRequest,
    result_pointer: *mut MakoStoTpccNewOrderFullResult,
) -> FfiResult<Status> {
    let request_range = payment_pointer_range(request_pointer, 1, "full NewOrder request")?;
    let result_range =
        payment_pointer_range(result_pointer.cast_const(), 1, "full NewOrder result")?;
    if request_range.overlaps(result_range) {
        return Err(fatal(format_args!(
            "full NewOrder request must not overlap its result"
        )));
    }
    // SAFETY: The wrapper keeps the validated request allocation readable for
    // the complete call.
    let request = unsafe { *request_pointer };
    let line_count = usize::try_from(request.line_count)
        .map_err(|_| fatal(format_args!("NewOrder line count does not fit usize")))?;
    if !(5..=NEW_ORDER_MAX_LINES).contains(&line_count) {
        return Err(fatal(format_args!(
            "NewOrder line count must be between 5 and {NEW_ORDER_MAX_LINES}"
        )));
    }
    for (name, value) in [
        ("warehouse ID", request.warehouse_id),
        ("district ID", request.district_id),
        ("customer ID", request.customer_id),
        ("order ID", request.order_id),
    ] {
        if value <= 0 {
            return Err(fatal(format_args!("NewOrder {name} must be positive")));
        }
    }
    if request.district_id > 10 || request.customer_id > 3_000 {
        return Err(fatal(format_args!(
            "NewOrder district or customer ID is out of range"
        )));
    }

    let item_range = payment_pointer_range(request.item_ids, line_count, "NewOrder item IDs")?;
    let quantity_range =
        payment_pointer_range(request.quantities, line_count, "NewOrder quantities")?;
    if result_range.overlaps(item_range) || result_range.overlaps(quantity_range) {
        return Err(fatal(format_args!(
            "full NewOrder input arrays must not overlap its result"
        )));
    }
    let mut item_ids = [0_u32; NEW_ORDER_MAX_LINES];
    let mut quantities = [0_u32; NEW_ORDER_MAX_LINES];
    // SAFETY: The two validated source ranges contain line_count aligned u32
    // values. The stack arrays are disjoint and large enough.
    unsafe {
        ptr::copy_nonoverlapping(request.item_ids, item_ids.as_mut_ptr(), line_count);
        ptr::copy_nonoverlapping(request.quantities, quantities.as_mut_ptr(), line_count);
    }
    for index in 0..line_count {
        if item_ids[index] == 0 || item_ids[index] > 100_000 {
            return Err(fatal(format_args!(
                "NewOrder item ID at line {index} is out of range"
            )));
        }
        if !(1..=10).contains(&quantities[index]) {
            return Err(fatal(format_args!(
                "NewOrder quantity at line {index} is out of range"
            )));
        }
    }

    let handle = &mut *guard.handle;
    let warehouse = unsafe {
        payment_table_owned(handle, request.warehouse_table, "NewOrder warehouse table")?
    };
    let district =
        unsafe { payment_table_owned(handle, request.district_table, "NewOrder district table")? };
    let customer =
        unsafe { payment_table_owned(handle, request.customer_table, "NewOrder customer table")? };
    let item = unsafe { payment_table_owned(handle, request.item_table, "NewOrder item table")? };
    let stock =
        unsafe { payment_table_owned(handle, request.stock_table, "NewOrder stock table")? };
    let new_order = unsafe {
        payment_table_owned(handle, request.new_order_table, "NewOrder new-order table")?
    };
    let oorder =
        unsafe { payment_table_owned(handle, request.oorder_table, "NewOrder order table")? };
    let oorder_c_id_idx = unsafe {
        payment_table_owned(
            handle,
            request.oorder_c_id_idx_table,
            "NewOrder customer-order index table",
        )?
    };
    let order_line = unsafe {
        payment_table_owned(
            handle,
            request.order_line_table,
            "NewOrder order-line table",
        )?
    };

    for (name, table) in [
        ("warehouse", warehouse),
        ("district", district),
        ("customer", customer),
    ] {
        if table.cache_policy != ResolvedCachePolicy::Full {
            return Err(fatal(format_args!(
                "NewOrder {name} table must use the full resolved cache policy"
            )));
        }
    }

    let item_ids = &item_ids[..line_count];
    let quantities = &quantities[..line_count];
    let prices = new_order_read_items(handle, item, item_ids)?;
    new_order_modify_stocks(handle, stock, request.warehouse_id, item_ids, quantities)?;

    let warehouse_key = request.warehouse_id.to_be_bytes();
    let mut district_key = [0_u8; 8];
    district_key[..4].copy_from_slice(&warehouse_key);
    district_key[4..].copy_from_slice(&request.district_id.to_be_bytes());
    let customer_key = new_order_key3(
        request.warehouse_id,
        request.district_id,
        request.customer_id,
    );
    new_order_require_cached_present(handle, customer, &customer_key, "new-order customer")?;
    new_order_require_cached_present(handle, warehouse, &warehouse_key, "new-order warehouse")?;
    new_order_require_cached_present(handle, district, &district_key, "new-order district")?;

    let order_key = new_order_key3(request.warehouse_id, request.district_id, request.order_id);
    let new_order_value = [b' '; 12];
    new_order_insert_header(
        handle,
        new_order,
        &order_key,
        &new_order_value,
        "new-order header",
    )?;
    let mut oorder_value = [0_u8; NEW_ORDER_OORDER_VALUE_MAX];
    let oorder_length = new_order_oorder_value(
        request.customer_id,
        request.line_count as u8,
        request.entry_date,
        &mut oorder_value,
    )
    .map_err(|error| fatal(format_args!("new-order order header: {error}")))?;
    new_order_insert_header(
        handle,
        oorder,
        &order_key,
        &oorder_value[..oorder_length],
        "new-order order header",
    )?;
    let order_index_key = new_order_key4(
        request.warehouse_id,
        request.district_id,
        request.customer_id,
        request.order_id,
    );
    new_order_insert_header(
        handle,
        oorder_c_id_idx,
        &order_index_key,
        &[0, 0],
        "new-order customer-order index",
    )?;

    let order_line_bytes = new_order_insert_order_lines(
        handle,
        order_line,
        NewOrderLineBatch {
            warehouse_id: request.warehouse_id,
            district_id: request.district_id,
            order_id: request.order_id,
            item_ids,
            quantities,
            prices: &prices,
        },
    )?;
    let reported_value_bytes = 12_usize
        .checked_add(oorder_length)
        .and_then(|bytes| bytes.checked_add(order_line_bytes))
        .ok_or_else(|| fatal(format_args!("new-order byte accounting overflow")))?;

    let commit_status = txn_commit_impl(handle)?;
    if commit_status != Status::Ok {
        return Ok(commit_status);
    }
    // SAFETY: The result range was validated and proven disjoint from every
    // caller-owned input before the transaction began mutating records.
    unsafe {
        ptr::write(
            result_pointer,
            MakoStoTpccNewOrderFullResult {
                reported_value_bytes,
            },
        )
    };
    guard.disarm();
    Ok(Status::Ok)
}

/// Executes and commits one exact-home TPC-C NewOrder transaction.
///
/// # Safety
/// The wrapper must provide live same-runtime handles and two readable
/// line_count-element input arrays. Request, arrays, and result must obey the
/// private non-aliasing contract for the complete call.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_new_order_full_trusted(
    thread_handle: *mut StoTpccThread,
    request: *const MakoStoTpccNewOrderFullRequest,
    result: *mut MakoStoTpccNewOrderFullResult,
) -> i32 {
    payment_boundary(
        thread_handle,
        "mako_sto_tpcc_new_order_full_trusted",
        || {
            let handle = unsafe { required_mut(thread_handle, "thread")? };
            handle.ensure_owner()?;
            if handle.active.is_none() {
                return Err(fatal(format_args!(
                    "full NewOrder requires an active transaction"
                )));
            }
            let mut guard = PaymentAttemptGuard::new(handle);
            // SAFETY: The guarded body centralizes private request validation
            // and resolves the attempt before every return.
            unsafe { new_order_full_body(&mut guard, request, result) }
        },
    )
}

/// Executes and commits one local TPC-C Delivery transaction.
///
/// Cursor updates are deliberately not transactional. Once a new-order row is
/// selected, its district cursor advances even if a later operation aborts.
///
/// # Safety
/// The wrapper must provide a live, exclusively accessed active thread, four
/// same-runtime table handles, a readable request, a writable ten-element
/// cursor array, and a writable result. Request, cursor array, and result must
/// be mutually disjoint and remain live for the complete call.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_delivery_full_trusted(
    thread_handle: *mut StoTpccThread,
    request: *const MakoStoTpccDeliveryFullRequest,
    result: *mut MakoStoTpccDeliveryFullResult,
) -> i32 {
    payment_boundary(thread_handle, "mako_sto_tpcc_delivery_full_trusted", || {
        let handle = unsafe { required_mut(thread_handle, "thread")? };
        handle.ensure_owner()?;
        if handle.active.is_none() {
            return Err(fatal(format_args!(
                "full Delivery requires an active transaction"
            )));
        }
        let mut guard = PaymentAttemptGuard::new(handle);
        // SAFETY: The guarded body centralizes private pointer validation
        // and resolves the active transaction before every return.
        unsafe { delivery_full_body(&mut guard, request, result) }
    })
}

/// Executes and commits the scan-and-stock-join tail of one local TPC-C
/// StockLevel transaction. The caller has already read the district prefix in
/// the same active transaction.
///
/// # Safety
/// The wrapper must provide a live, exclusively accessed active thread, two
/// same-runtime table handles, and mutually disjoint live request/result
/// allocations for the complete call.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_stock_level_full_trusted(
    thread_handle: *mut StoTpccThread,
    request: *const MakoStoTpccStockLevelFullRequest,
    result: *mut MakoStoTpccStockLevelFullResult,
) -> i32 {
    payment_boundary(
        thread_handle,
        "mako_sto_tpcc_stock_level_full_trusted",
        || {
            let handle = unsafe { required_mut(thread_handle, "thread")? };
            handle.ensure_owner()?;
            if handle.active.is_none() {
                return Err(fatal(format_args!(
                    "full StockLevel requires an active transaction"
                )));
            }
            let mut guard = PaymentAttemptGuard::new(handle);
            // SAFETY: The guarded body validates the private request and
            // resolves the active attempt before every return.
            unsafe { stock_level_full_body(&mut guard, request, result) }
        },
    )
}

/// Executes the local W, D, optional customer-name lookup, and C prefix of a
/// TPC-C Payment transaction without callbacks across the C ABI.
///
/// # Safety
///
/// The wrapper must supply live same-runtime handles and valid request, key,
/// result, and output allocations. The output ranges must not alias the
/// thread or table handles, request, result, or key allocations. The three
/// output allocations must remain at fixed addresses and immutable after this
/// call succeeds until the active transaction commits or aborts. Those
/// liveness and non-aliasing rules are caller preconditions. The function
/// checks output-to-output and output-to-result overlap, and aborts the active
/// attempt on every reported failure or panic.
#[doc(hidden)]
#[no_mangle]
pub unsafe extern "C" fn mako_sto_tpcc_payment_prefix_trusted(
    thread_handle: *mut StoTpccThread,
    request: *const MakoStoTpccPaymentPrefixRequest,
    result: *mut MakoStoTpccPaymentPrefixResult,
) -> i32 {
    payment_boundary(
        thread_handle,
        "mako_sto_tpcc_payment_prefix_trusted",
        || {
            let handle = unsafe { required_mut(thread_handle, "thread")? };
            handle.ensure_owner()?;
            if handle.active.is_none() {
                return Err(fatal(format_args!(
                    "payment prefix requires an active transaction"
                )));
            }
            let mut guard = PaymentAttemptGuard::new(handle);
            // SAFETY: Raw range validation and the extended transaction-lifetime
            // contract are centralized in the guarded body.
            unsafe { payment_prefix_body(&mut guard, request, result) }
        },
    )
}

/// Returns the current thread's error-message byte length, excluding NUL.
#[no_mangle]
pub extern "C" fn sto_tpcc_last_error_length() -> usize {
    catch_unwind(AssertUnwindSafe(|| {
        LAST_ERROR.with(|slot| slot.try_borrow().map_or(0, |error| error.len))
    }))
    .unwrap_or(0)
}

/// # Safety
/// `out_actual` must be uniquely writable. A nonzero capacity requires that
/// `out_message` cover that many writable bytes.
#[no_mangle]
pub unsafe extern "C" fn sto_tpcc_last_error_copy(
    out_message: *mut std::ffi::c_char,
    message_capacity: usize,
    out_actual: *mut usize,
) -> i32 {
    boundary_preserving_error("sto_tpcc_last_error_copy", || {
        if out_actual.is_null() {
            return Err(fatal(format_args!("out_actual must not be null")));
        }
        let snapshot = LAST_ERROR.with(|slot| {
            let error = slot.try_borrow().map_err(|_| Status::Fatal)?;
            let mut snapshot = ErrorBuffer::new();
            snapshot.bytes[..error.len].copy_from_slice(error.as_bytes());
            snapshot.len = error.len;
            Ok::<_, Status>(snapshot)
        })?;
        // SAFETY: Validity and exclusivity are required by the C contract.
        unsafe { *out_actual = snapshot.len };
        let required = snapshot.len.saturating_add(1);
        if message_capacity < required {
            return Ok(Status::BufferTooSmall);
        }
        if out_message.is_null() {
            return Err(fatal(format_args!(
                "out_message must not be null when its capacity is nonzero"
            )));
        }
        // SAFETY: Capacity was checked and the caller supplies a uniquely
        // writable message buffer.
        unsafe {
            ptr::copy_nonoverlapping(snapshot.bytes.as_ptr(), out_message.cast(), snapshot.len);
            *out_message.add(snapshot.len) = 0;
        }
        Ok(Status::Ok)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    // The native ABI owns one process-wide runtime and requires every later
    // acquisition to repeat its exact native limits. Keep those two fields
    // uniform across native unit tests while varying the Rust STO capacities.
    #[cfg(mtree_native_integration)]
    const NATIVE_TEST_MAX_THREADS: u32 = 8;
    #[cfg(mtree_native_integration)]
    const NATIVE_TEST_MAX_KEY_LENGTH: u32 = 128;

    fn test_encode_u32(mut value: u32) -> Vec<u8> {
        let mut bytes = Vec::new();
        while value > 0x7f {
            bytes.push((value as u8 & 0x7f) | 0x80);
            value >>= 7;
        }
        bytes.push(value as u8);
        bytes
    }

    fn test_encode_i32(value: i32) -> Vec<u8> {
        test_encode_u32(((value as u32) << 1) ^ ((value >> 31) as u32))
    }

    fn test_inline(bytes: &mut Vec<u8>, value: &[u8], maximum: usize) {
        assert!(value.len() <= maximum);
        bytes.push(value.len() as u8);
        bytes.extend_from_slice(value);
        bytes.resize(bytes.len() + maximum + 1 - value.len(), 0);
    }

    fn payment_customer_fixture(payment_count: i32) -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&0.125_f32.to_ne_bytes());
        bytes.extend_from_slice(b"GC");
        test_inline(&mut bytes, b"BAR", 16);
        test_inline(&mut bytes, b"ANN", 16);
        bytes.extend_from_slice(&50_000_f32.to_ne_bytes());
        bytes.extend_from_slice(&(-10_f32).to_ne_bytes());
        bytes.extend_from_slice(&10_f32.to_ne_bytes());
        bytes.extend_from_slice(&test_encode_i32(payment_count));
        bytes.extend_from_slice(&test_encode_i32(4));
        test_inline(&mut bytes, b"A1", 20);
        test_inline(&mut bytes, b"B2", 20);
        test_inline(&mut bytes, b"NYC", 20);
        bytes.extend_from_slice(b"NY");
        bytes.extend_from_slice(b"123456789");
        bytes.extend_from_slice(b"0123456789012345");
        bytes.extend_from_slice(&test_encode_u32(123_456));
        bytes.extend_from_slice(b"OE");
        bytes
    }

    #[cfg(mtree_native_integration)]
    fn stock_fixture(quantity: i16) -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&quantity.to_ne_bytes());
        bytes.extend_from_slice(&0_f32.to_ne_bytes());
        bytes.extend_from_slice(&test_encode_i32(0));
        bytes.extend_from_slice(&test_encode_i32(0));
        bytes
    }

    #[cfg(mtree_native_integration)]
    fn item_fixture(price: f32) -> Vec<u8> {
        let mut bytes = Vec::new();
        test_inline(&mut bytes, b"ITEM", 24);
        bytes.extend_from_slice(&price.to_ne_bytes());
        test_inline(&mut bytes, b"DATA", 50);
        bytes.extend_from_slice(&test_encode_i32(1));
        bytes
    }

    fn fixture_f32(bytes: &[u8], offset: usize) -> f32 {
        f32::from_ne_bytes(bytes[offset..offset + 4].try_into().unwrap())
    }

    #[cfg(mtree_native_integration)]
    unsafe fn call_fixed_put<const KEY_LENGTH: usize>(
        thread: *mut StoTpccThread,
        table: *mut StoTpccTable,
        keys: &[[u8; KEY_LENGTH]],
        values: &[&[u8]],
        mode: StoTpccFixedPutMode,
    ) -> (i32, StoTpccFixedPutResult) {
        assert_eq!(keys.len(), values.len());
        let descriptors: Vec<_> = values
            .iter()
            .map(|value| StoTpccFixedValue {
                data: if value.is_empty() {
                    ptr::null()
                } else {
                    value.as_ptr()
                },
                length: value.len(),
            })
            .collect();
        let mut result = StoTpccFixedPutResult {
            inserted: usize::MAX,
            first_duplicate: 0,
        };
        let status = unsafe {
            sto_tpcc_put_fixed(
                thread,
                table,
                if keys.is_empty() {
                    ptr::null()
                } else {
                    keys.as_ptr().cast()
                },
                keys.len(),
                KEY_LENGTH,
                if keys.is_empty() {
                    ptr::null()
                } else {
                    descriptors.as_ptr()
                },
                mode,
                &mut result,
            )
        };
        (status, result)
    }

    #[cfg(mtree_native_integration)]
    unsafe extern "C" fn stop_after_first_scan_row(
        context: *mut std::ffi::c_void,
        key: *const u8,
        key_length: usize,
        _value: *const u8,
        _value_length: usize,
    ) -> i32 {
        // SAFETY: The test passes a live Vec for the synchronous scan call.
        let delivered = unsafe { &mut *context.cast::<Vec<Vec<u8>>>() };
        let key = if key_length == 0 {
            &[][..]
        } else {
            // SAFETY: The scan callback contract supplies this live row slice.
            unsafe { slice::from_raw_parts(key, key_length) }
        };
        delivered.push(key.to_vec());
        1
    }

    #[cfg(mtree_native_integration)]
    #[derive(Default)]
    struct FixedReadContext {
        observed: Vec<(usize, Option<Vec<u8>>)>,
        fail_at: Option<usize>,
    }

    #[cfg(mtree_native_integration)]
    unsafe extern "C" fn collect_fixed_read(
        context: *mut std::ffi::c_void,
        index: usize,
        value: *const u8,
        value_length: usize,
    ) -> i32 {
        // SAFETY: The test passes a live context for this synchronous call.
        let context = unsafe { &mut *context.cast::<FixedReadContext>() };
        let value = if value.is_null() {
            assert_eq!(value_length, 0);
            None
        } else {
            // SAFETY: The fixed-read callback contract supplies this live
            // value slice for the duration of the invocation.
            Some(unsafe { slice::from_raw_parts(value, value_length) }.to_vec())
        };
        context.observed.push((index, value));
        i32::from(context.fail_at == Some(index))
    }

    #[cfg(mtree_native_integration)]
    enum FixedModifyStep {
        Keep,
        Put(Vec<u8>),
        Remove,
        Failed,
        Unknown,
        NullNonzero,
        Oversize,
    }

    #[cfg(mtree_native_integration)]
    struct FixedModifyContext {
        steps: Vec<FixedModifyStep>,
        observed: Vec<(usize, Option<Vec<u8>>)>,
    }

    #[cfg(mtree_native_integration)]
    impl FixedModifyContext {
        fn new(steps: impl IntoIterator<Item = FixedModifyStep>) -> Self {
            Self {
                steps: steps.into_iter().collect(),
                observed: Vec::new(),
            }
        }
    }

    #[cfg(mtree_native_integration)]
    unsafe extern "C" fn apply_fixed_modify_step(
        context: *mut std::ffi::c_void,
        index: usize,
        value: *const u8,
        value_length: usize,
        out_replacement: *mut *const u8,
        out_replacement_length: *mut usize,
    ) -> StoTpccFixedModifyAction {
        // SAFETY: Tests supply a live context and uniquely writable outputs
        // for this synchronous callback.
        let context = unsafe { &mut *context.cast::<FixedModifyContext>() };
        let value = if value.is_null() {
            assert_eq!(value_length, 0);
            None
        } else {
            // SAFETY: The fixed-mutation contract leases this current value
            // for the callback invocation.
            Some(unsafe { slice::from_raw_parts(value, value_length) }.to_vec())
        };
        context.observed.push((index, value));
        // SAFETY: Both output pointers are supplied by the endpoint.
        unsafe {
            *out_replacement = ptr::null();
            *out_replacement_length = 0;
        }
        match &context.steps[index] {
            FixedModifyStep::Keep => STO_TPCC_FIXED_MODIFY_KEEP,
            FixedModifyStep::Put(replacement) => {
                // The action storage outlives the complete synchronous call.
                unsafe {
                    *out_replacement = replacement.as_ptr();
                    *out_replacement_length = replacement.len();
                }
                STO_TPCC_FIXED_MODIFY_PUT
            }
            FixedModifyStep::Remove => STO_TPCC_FIXED_MODIFY_REMOVE,
            FixedModifyStep::Failed => STO_TPCC_FIXED_MODIFY_FAILED,
            FixedModifyStep::Unknown => 99,
            FixedModifyStep::NullNonzero => {
                unsafe {
                    *out_replacement_length = 1;
                }
                STO_TPCC_FIXED_MODIFY_PUT
            }
            FixedModifyStep::Oversize => {
                unsafe {
                    *out_replacement = ptr::dangling();
                    *out_replacement_length = usize::MAX;
                }
                STO_TPCC_FIXED_MODIFY_PUT
            }
        }
    }

    #[cfg(mtree_native_integration)]
    unsafe fn get_with_capacity(
        thread: *mut StoTpccThread,
        table: *mut StoTpccTable,
        key: &[u8],
        capacity: usize,
    ) -> (i32, usize) {
        let mut output = [0_u8; 64];
        assert!(capacity <= output.len());
        let output_pointer = if capacity == 0 {
            ptr::null_mut()
        } else {
            output.as_mut_ptr()
        };
        let mut actual = usize::MAX;
        let status = unsafe {
            sto_tpcc_get(
                thread,
                table,
                key.as_ptr(),
                key.len(),
                output_pointer,
                capacity,
                &mut actual,
            )
        };
        (status, actual)
    }

    #[cfg(mtree_native_integration)]
    unsafe fn get_with_sentinel(
        thread: *mut StoTpccThread,
        table: *mut StoTpccTable,
        key: &[u8],
        capacity: usize,
        sentinel: u8,
    ) -> (i32, usize, Vec<u8>) {
        let mut output = vec![sentinel; capacity];
        let output_pointer = if output.is_empty() {
            ptr::null_mut()
        } else {
            output.as_mut_ptr()
        };
        let mut actual = usize::MAX;
        let status = unsafe {
            sto_tpcc_get(
                thread,
                table,
                key.as_ptr(),
                key.len(),
                output_pointer,
                output.len(),
                &mut actual,
            )
        };
        (status, actual, output)
    }

    #[cfg(mtree_native_integration)]
    fn reset_cache_slot_calls() {
        RESOLVED_CACHE_SLOT_CALLS.with(|calls| calls.set(0));
    }

    #[cfg(mtree_native_integration)]
    fn cache_slot_calls() -> usize {
        RESOLVED_CACHE_SLOT_CALLS.with(Cell::get)
    }

    #[cfg(mtree_native_integration)]
    fn stock_level_cache_partition() -> (usize, usize) {
        STOCK_LEVEL_CACHE_PARTITION.with(Cell::get)
    }

    #[test]
    fn stable_status_numbers_match_header() {
        assert_eq!(Status::Ok.code(), 0);
        assert_eq!(Status::Miss.code(), 1);
        assert_eq!(Status::Duplicate.code(), 2);
        assert_eq!(Status::Retry.code(), 3);
        assert_eq!(Status::BufferTooSmall.code(), 4);
        assert_eq!(Status::Fatal.code(), 5);
        assert_eq!(STO_TPCC_FIXED_MODIFY_KEEP, 0);
        assert_eq!(STO_TPCC_FIXED_MODIFY_PUT, 1);
        assert_eq!(STO_TPCC_FIXED_MODIFY_REMOVE, 2);
        assert_eq!(STO_TPCC_FIXED_MODIFY_FAILED, 3);
    }

    #[test]
    fn shared_ffi_boundary_contains_panics() {
        clear_last_error();
        let status = boundary("injected_boundary_panic", || -> FfiResult<Status> {
            panic!("injected panic must not cross extern C")
        });
        assert_eq!(status, Status::Fatal.code());
        let diagnostic = LAST_ERROR.with(|slot| {
            let error = slot.borrow();
            String::from_utf8(error.as_bytes().to_vec()).unwrap()
        });
        assert_eq!(
            diagnostic,
            "injected_boundary_panic: contained an unexpected Rust panic"
        );
    }

    #[test]
    fn checked_get_zeroes_actual_before_rejecting_an_invalid_handle() {
        let mut actual = usize::MAX;
        // SAFETY: `out_actual` is the only pointer dereferenced before the null
        // thread handle is rejected. The other null pointers describe empty
        // byte ranges and are never reached on this failure path.
        let status = unsafe {
            sto_tpcc_get(
                ptr::null_mut(),
                ptr::null(),
                ptr::null(),
                0,
                ptr::null_mut(),
                0,
                &mut actual,
            )
        };
        assert_eq!(status, Status::Fatal.code());
        assert_eq!(actual, 0);
    }

    #[test]
    fn payment_private_abi_layout_is_stable() {
        if cfg!(target_pointer_width = "64") {
            assert_eq!(mem::size_of::<MakoStoTpccPaymentPrefixRequest>(), 120);
            assert_eq!(
                mem::offset_of!(MakoStoTpccPaymentPrefixRequest, customer_id),
                72
            );
            assert_eq!(
                mem::offset_of!(MakoStoTpccPaymentPrefixRequest, warehouse_output),
                88
            );
            assert_eq!(mem::size_of::<MakoStoTpccPaymentPrefixResult>(), 32);
            assert_eq!(
                mem::offset_of!(MakoStoTpccPaymentPrefixResult, customer_id),
                24
            );
            assert_eq!(mem::size_of::<MakoStoTpccPaymentFullRequest>(), 112);
            assert_eq!(
                mem::offset_of!(MakoStoTpccPaymentFullRequest, history_table),
                32
            );
            assert_eq!(
                mem::offset_of!(MakoStoTpccPaymentFullRequest, customer_by_name),
                108
            );
            assert_eq!(mem::size_of::<MakoStoTpccPaymentFullResult>(), 16);
            assert_eq!(
                mem::offset_of!(MakoStoTpccPaymentFullResult, customer_id),
                8
            );
            assert_eq!(mem::size_of::<MakoStoTpccNewOrderFullRequest>(), 112);
            assert_eq!(
                mem::offset_of!(MakoStoTpccNewOrderFullRequest, order_line_table),
                64
            );
            assert_eq!(
                mem::offset_of!(MakoStoTpccNewOrderFullRequest, item_ids),
                72
            );
            assert_eq!(
                mem::offset_of!(MakoStoTpccNewOrderFullRequest, line_count),
                108
            );
            assert_eq!(mem::size_of::<MakoStoTpccNewOrderFullResult>(), 8);
        }
    }

    #[test]
    fn new_order_codecs_preserve_stock_tail_and_value_layouts() {
        let mut item = Vec::new();
        item.push(4);
        item.extend_from_slice(b"ITEM");
        item.resize(26, 0);
        item.extend_from_slice(&12.5_f32.to_ne_bytes());
        item.push(4);
        item.extend_from_slice(b"DATA");
        item.resize(82, 0);
        item.extend_from_slice(&test_encode_i32(123));
        assert_eq!(new_order_item_price(&item), Ok(12.5));

        let mut stock = Vec::new();
        stock.extend_from_slice(&15_i16.to_ne_bytes());
        stock.extend_from_slice(&3.5_f32.to_ne_bytes());
        stock.extend_from_slice(&test_encode_i32(8_192));
        stock.extend_from_slice(&test_encode_i32(63));
        let tail = stock[6..].to_vec();
        let mut replacement = [0_u8; NEW_ORDER_STOCK_VALUE_MAX];
        let length = new_order_stock_replacement(&stock, 6, &mut replacement).unwrap();
        assert_eq!(length, stock.len());
        assert_eq!(
            i16::from_ne_bytes(replacement[..2].try_into().unwrap()),
            100
        );
        assert_eq!(
            f32::from_ne_bytes(replacement[2..6].try_into().unwrap()),
            9.5
        );
        assert_eq!(&replacement[6..length], tail);

        let mut oorder = [0_u8; NEW_ORDER_OORDER_VALUE_MAX];
        let oorder_length = new_order_oorder_value(7, 5, 123, &mut oorder).unwrap();
        assert_eq!(&oorder[..oorder_length], &[14, 0, 5, 1, 123]);
        let mut order_line = [0_u8; NEW_ORDER_ORDER_LINE_VALUE_MAX];
        let order_line_length = new_order_order_line_value(1, 10.0, 1, 5, &mut order_line).unwrap();
        let mut expected = vec![2, 0];
        expected.extend_from_slice(&10_f32.to_ne_bytes());
        expected.extend_from_slice(&[2, 5]);
        assert_eq!(&order_line[..order_line_length], expected);
    }

    #[test]
    fn payment_zigzag_varints_cover_boundaries_and_reject_malformed_forms() {
        for value in [
            i32::MIN,
            -1_048_576,
            -64,
            -1,
            0,
            1,
            63,
            64,
            8_191,
            8_192,
            1_048_575,
            i32::MAX,
        ] {
            let expected = test_encode_i32(value);
            let mut encoded = [0_u8; 5];
            let length = payment_encode_i32(value, &mut encoded);
            assert_eq!(&encoded[..length], expected);
            let mut cursor = 0;
            let (_, end, decoded) =
                payment_decode_i32(&encoded[..length], &mut cursor, "fixture").unwrap();
            assert_eq!(decoded, value);
            assert_eq!(end, length);
            assert_eq!(cursor, length);
        }

        for malformed in [
            &[][..],
            &[0x80][..],
            &[0x80, 0x00][..],
            &[0xff, 0xff, 0xff, 0xff, 0x10][..],
            &[0xff, 0xff, 0xff, 0xff, 0x80][..],
        ] {
            let mut cursor = 0;
            assert!(payment_decode_i32(malformed, &mut cursor, "fixture").is_err());
        }
    }

    #[test]
    fn payment_customer_codec_matches_field_order_and_grows_varints() {
        for payment_count in [63, 8_191] {
            let original = payment_customer_fixture(payment_count);
            let layout = payment_parse_customer(&original).unwrap();
            assert_eq!(layout.payment_count, payment_count);
            assert_eq!(fixture_f32(&original, layout.balance_offset), -10.0);
            assert_eq!(fixture_f32(&original, layout.ytd_payment_offset), 10.0);

            let mut expected = original.clone();
            expected[layout.balance_offset..layout.balance_offset + 4]
                .copy_from_slice(&(-15.5_f32).to_ne_bytes());
            expected[layout.ytd_payment_offset..layout.ytd_payment_offset + 4]
                .copy_from_slice(&15.5_f32.to_ne_bytes());
            expected.splice(
                layout.payment_count_start..layout.payment_count_end,
                test_encode_i32(payment_count + 1),
            );

            let mut output = [0xa5_u8; PAYMENT_VALUE_CAPACITY];
            output[..original.len()].copy_from_slice(&original);
            let replacement_length =
                payment_patch_customer(&mut output, original.len(), 5.5).unwrap();
            assert_eq!(replacement_length, original.len() + 1);
            assert_eq!(&output[..replacement_length], expected);
            assert!(output[replacement_length..]
                .iter()
                .all(|byte| *byte == 0xa5));

            let replacement = payment_parse_customer(&output[..replacement_length]).unwrap();
            assert_eq!(replacement.payment_count, payment_count + 1);
            assert_eq!(fixture_f32(&output, replacement.balance_offset), -15.5);
            assert_eq!(fixture_f32(&output, replacement.ytd_payment_offset), 15.5);
        }
    }

    #[test]
    fn payment_customer_codec_rejects_truncation_malformed_values_and_overflow() {
        let valid = payment_customer_fixture(7);
        for length in 0..valid.len() {
            assert!(
                payment_parse_customer(&valid[..length]).is_err(),
                "{length}"
            );
        }

        let mut trailing = valid.clone();
        trailing.push(0);
        assert_eq!(
            payment_parse_customer(&trailing),
            Err(PaymentCodecError::TrailingBytes)
        );

        let mut oversized_last_name = valid.clone();
        oversized_last_name[6] = 17;
        assert_eq!(
            payment_parse_customer(&oversized_last_name),
            Err(PaymentCodecError::InvalidLength("customer last name"))
        );

        let mut nonfinite = valid.clone();
        nonfinite[..4].copy_from_slice(&f32::NAN.to_ne_bytes());
        assert_eq!(
            payment_parse_customer(&nonfinite),
            Err(PaymentCodecError::NonFinite("customer discount"))
        );

        let layout = payment_parse_customer(&valid).unwrap();
        let mut noncanonical = valid.clone();
        noncanonical.splice(
            layout.payment_count_start..layout.payment_count_end,
            [0x8e, 0x00],
        );
        assert_eq!(
            payment_parse_customer(&noncanonical),
            Err(PaymentCodecError::InvalidVarint("customer payment count"))
        );

        let needs_growth = payment_customer_fixture(63);
        let mut exact_capacity = needs_growth.clone();
        assert_eq!(
            payment_patch_customer(&mut exact_capacity, needs_growth.len(), 1.0),
            Err(PaymentCodecError::BufferTooSmall {
                required: needs_growth.len() + 1,
                available: needs_growth.len(),
            })
        );

        let maximum_count = payment_customer_fixture(i32::MAX);
        let mut output = [0_u8; PAYMENT_VALUE_CAPACITY];
        output[..maximum_count.len()].copy_from_slice(&maximum_count);
        assert_eq!(
            payment_patch_customer(&mut output, maximum_count.len(), 1.0),
            Err(PaymentCodecError::ArithmeticOverflow(
                "customer payment count"
            ))
        );
    }

    #[test]
    fn payment_tail_codecs_match_mako_packed_layouts() {
        let key = payment_history_key(3, 4, 5, 6, 7, 0x0102_0304);
        let expected_key = [
            3_i32.to_be_bytes().as_slice(),
            4_i32.to_be_bytes().as_slice(),
            5_i32.to_be_bytes().as_slice(),
            6_i32.to_be_bytes().as_slice(),
            7_i32.to_be_bytes().as_slice(),
            0x0102_0304_u32.to_be_bytes().as_slice(),
        ]
        .concat();
        assert_eq!(key.as_slice(), expected_key);

        let history = payment_history_value(12.5, b"WAREHOUSE12", b"DIST\0IGNORED");
        assert_eq!(history.len(), PAYMENT_HISTORY_VALUE_LENGTH);
        assert_eq!(&history[..4], &12.5_f32.to_ne_bytes());
        assert_eq!(history[4], 18);
        assert_eq!(&history[5..23], b"WAREHOUSE1    DIST");
        assert!(history[23..].iter().all(|byte| *byte == 0));

        let mut customer_data = [0_u8; PAYMENT_CUSTOMER_DATA_VALUE_LENGTH];
        customer_data[..2].copy_from_slice(&8_u16.to_ne_bytes());
        customer_data[2..10].copy_from_slice(b"OLD DATA");
        let customer_data_len = PAYMENT_CUSTOMER_DATA_VALUE_LENGTH;
        assert_eq!(
            payment_patch_customer_data(
                &mut customer_data,
                customer_data_len,
                PaymentCustomerDataFields {
                    customer_id: 5,
                    // The scalar path formats customer_district_id + 100.
                    adjusted_customer_district_id: 103,
                    customer_warehouse_id: 4,
                    district_id: 6,
                    warehouse_id: 7,
                    payment_amount: 12.5,
                },
            )
            .unwrap(),
            PAYMENT_CUSTOMER_DATA_VALUE_LENGTH
        );
        let expected = b"5 103 4 6 7 12 | OLD DATA";
        assert_eq!(
            u16::from_ne_bytes(customer_data[..2].try_into().unwrap()) as usize,
            expected.len()
        );
        assert_eq!(&customer_data[2..2 + expected.len()], expected);
        assert!(customer_data[2 + expected.len()..]
            .iter()
            .all(|byte| *byte == 0));

        customer_data[..2].copy_from_slice(&301_u16.to_ne_bytes());
        assert_eq!(
            payment_patch_customer_data(
                &mut customer_data,
                customer_data_len,
                PaymentCustomerDataFields {
                    customer_id: 5,
                    adjusted_customer_district_id: 103,
                    customer_warehouse_id: 4,
                    district_id: 6,
                    warehouse_id: 7,
                    payment_amount: 12.5,
                },
            ),
            Err(PaymentCodecError::InvalidLength("customer data string"))
        );
    }

    #[test]
    fn payment_name_selection_uses_lower_median_through_the_cpp_limit() {
        assert_eq!(payment_decode_customer_id(&[14]).unwrap(), 7);
        assert_eq!(payment_decode_customer_id(&[14, 0]).unwrap(), 7);
        assert!(payment_decode_customer_id(&[14, 1]).is_err());
        assert!(payment_decode_customer_id(&[14, 0, 0]).is_err());
        assert_eq!(payment_lower_median(&[7]).unwrap(), 7);
        assert_eq!(payment_lower_median(&[7, 11]).unwrap(), 7);
        assert_eq!(payment_lower_median(&[7, 11, 13]).unwrap(), 11);
        assert_eq!(payment_lower_median(&[7, 11, 13, 17]).unwrap(), 11);
        assert!(payment_lower_median(&[]).is_err());
        assert_eq!(
            payment_lower_median(&[1; PAYMENT_NAME_SCAN_LIMIT]).unwrap(),
            1
        );
        assert!(payment_lower_median(&[1; PAYMENT_NAME_SCAN_LIMIT + 1]).is_err());
        assert!(payment_lower_median(&[3, 0, 5]).is_err());
    }

    #[test]
    fn payment_output_ranges_detect_overlap_without_rejecting_adjacency() {
        let mut storage = [0_u8; PAYMENT_VALUE_CAPACITY * 3];
        let first =
            payment_output_range(storage.as_mut_ptr(), PAYMENT_VALUE_CAPACITY, "first output")
                .unwrap();
        let adjacent = payment_output_range(
            unsafe { storage.as_mut_ptr().add(PAYMENT_VALUE_CAPACITY) },
            PAYMENT_VALUE_CAPACITY,
            "adjacent output",
        )
        .unwrap();
        let overlapping = payment_output_range(
            unsafe { storage.as_mut_ptr().add(PAYMENT_VALUE_CAPACITY - 1) },
            PAYMENT_VALUE_CAPACITY,
            "overlapping output",
        )
        .unwrap();
        assert!(!first.overlaps(adjacent));
        assert!(first.overlaps(overlapping));
    }

    #[test]
    fn stable_resolved_cache_policies_match_header() {
        assert_eq!(STO_TPCC_RESOLVED_CACHE_FULL, 0);
        assert_eq!(STO_TPCC_RESOLVED_CACHE_LAST_ONLY, 1);
        assert_eq!(STO_TPCC_RESOLVED_CACHE_READ_THEN_WRITE, 2);
        assert_eq!(STO_TPCC_RESOLVED_CACHE_NONE, 3);
        assert_eq!(STO_TPCC_RESOLVED_CACHE_DENSE_ITEM, 4);
        assert_eq!(STO_TPCC_RESOLVED_CACHE_DENSE_STOCK, 5);
        assert_eq!(
            ResolvedCachePolicy::from_raw(STO_TPCC_RESOLVED_CACHE_FULL),
            Ok((ResolvedCachePolicy::Full, DenseCachePolicy::None))
        );
        assert_eq!(
            ResolvedCachePolicy::from_raw(STO_TPCC_RESOLVED_CACHE_LAST_ONLY),
            Ok((ResolvedCachePolicy::LastOnly, DenseCachePolicy::None))
        );
        assert_eq!(
            ResolvedCachePolicy::from_raw(STO_TPCC_RESOLVED_CACHE_READ_THEN_WRITE),
            Ok((ResolvedCachePolicy::ReadThenWrite, DenseCachePolicy::None))
        );
        assert_eq!(
            ResolvedCachePolicy::from_raw(STO_TPCC_RESOLVED_CACHE_NONE),
            Ok((ResolvedCachePolicy::None, DenseCachePolicy::None))
        );
        assert_eq!(
            ResolvedCachePolicy::from_raw(STO_TPCC_RESOLVED_CACHE_DENSE_ITEM),
            Ok((ResolvedCachePolicy::None, DenseCachePolicy::Item))
        );
        assert_eq!(
            ResolvedCachePolicy::from_raw(STO_TPCC_RESOLVED_CACHE_DENSE_STOCK),
            Ok((ResolvedCachePolicy::ReadThenWrite, DenseCachePolicy::Stock))
        );
        assert_eq!(ResolvedCachePolicy::from_raw(6), Err(Status::Fatal));
        assert_eq!(dense_item_slot(1), Ok(0));
        assert_eq!(dense_item_slot(100_000), Ok(99_999));
        assert_eq!(dense_item_slot(0), Err(Status::Fatal));
        assert_eq!(dense_item_slot(100_001), Err(Status::Fatal));
    }

    #[test]
    fn fixed_key_views_require_exact_nonoverflowing_storage() {
        let packed = [[1_u8, 2, 3, 4], [5, 6, 7, 8]];
        let keys = unsafe { fixed_keys::<4>(packed.as_ptr().cast(), packed.len()) }.unwrap();
        assert_eq!(keys, &packed);
        assert!(unsafe { fixed_keys::<4>(ptr::null(), 0) }
            .unwrap()
            .is_empty());
        assert_eq!(
            unsafe { fixed_keys::<8>(ptr::null(), usize::MAX) },
            Err(Status::Fatal)
        );
    }

    #[cfg(mtree_native_integration)]
    #[test]
    fn insert_many_accounting_failure_aborts_staged_transaction() {
        unsafe {
            let config = StoTpccDbConfig {
                max_threads: NATIVE_TEST_MAX_THREADS,
                max_key_length: NATIVE_TEST_MAX_KEY_LENGTH,
                max_items_per_txn: 16,
                max_locks_per_txn: 32,
            };
            let mut db = ptr::null_mut();
            let mut table = ptr::null_mut();
            let mut thread = ptr::null_mut();
            assert_eq!(sto_tpcc_db_create(&config, &mut db), Status::Ok.code());
            assert_eq!(
                sto_tpcc_table_create(db, ptr::null(), &mut table),
                Status::Ok.code()
            );
            assert_eq!(sto_tpcc_thread_create(db, &mut thread), Status::Ok.code());

            let key = b"accounting-overflow";
            let value = b"must-abort";
            let operation = StoTpccInsertOperation {
                table,
                key: key.as_ptr(),
                key_length: key.len(),
                value: value.as_ptr(),
                value_length: value.len(),
            };
            let mut result = StoTpccFixedPutResult::default();

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            (*thread).pending_size.push(PendingSizeDelta {
                table: Arc::clone(&(*table).state),
                delta: i64::MAX,
            });
            assert_eq!(
                sto_tpcc_insert_many(thread, &operation, 1, &mut result),
                Status::Fatal.code()
            );
            assert_eq!(result.inserted, 1);
            assert!((*thread).active.is_none());
            assert!((*thread).pending_size.is_empty());
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Fatal.code());

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            let (status, actual, output) = get_with_sentinel(thread, table, key, 32, 0xee);
            assert_eq!(status, Status::Miss.code());
            assert_eq!(actual, 0);
            assert_eq!(output, vec![0xee; 32]);
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());
            let mut rows = u64::MAX;
            assert_eq!(sto_tpcc_table_size(table, &mut rows), Status::Ok.code());
            assert_eq!(rows, 0);

            assert_eq!(sto_tpcc_thread_destroy(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(table), Status::Ok.code());
            assert_eq!(sto_tpcc_db_destroy(db), Status::Ok.code());
        }
    }

    #[cfg(mtree_native_integration)]
    #[test]
    fn fixed_read_visits_all_widths_and_aborts_on_callback_failure() {
        unsafe {
            let config = StoTpccDbConfig {
                max_threads: NATIVE_TEST_MAX_THREADS,
                max_key_length: NATIVE_TEST_MAX_KEY_LENGTH,
                max_items_per_txn: 64,
                max_locks_per_txn: 128,
            };
            let mut db = ptr::null_mut();
            let mut table = ptr::null_mut();
            let mut thread = ptr::null_mut();
            assert_eq!(sto_tpcc_db_create(&config, &mut db), Status::Ok.code());
            assert_eq!(
                sto_tpcc_table_create(db, ptr::null(), &mut table),
                Status::Ok.code()
            );
            assert_eq!(sto_tpcc_thread_create(db, &mut thread), Status::Ok.code());

            let key4_a = [0x10, 0x20, 0x30, 0x40];
            let key4_b = [0x50, 0x60, 0x70, 0x80];
            let key8 = [1, 2, 3, 4, 5, 6, 7, 8];
            let key12 = [0x12, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
            let key16 = [0x16, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            for (key, value) in [
                (&key4_a[..], &b"four-a"[..]),
                (&key4_b[..], &b"four-b"[..]),
                (&key8[..], &b"eight"[..]),
                (&key12[..], &b"twelve"[..]),
                (&key16[..], &b"sixteen"[..]),
            ] {
                assert_eq!(
                    sto_tpcc_insert(
                        thread,
                        table,
                        key.as_ptr(),
                        key.len(),
                        value.as_ptr(),
                        value.len(),
                    ),
                    Status::Ok.code()
                );
            }
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            let packed4 = [key4_b, key4_a, key4_a, [0xff; 4]];
            let mut context = FixedReadContext::default();
            let mut visited = usize::MAX;
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_put(
                    thread,
                    table,
                    key4_a.as_ptr(),
                    key4_a.len(),
                    b"staged".as_ptr(),
                    b"staged".len(),
                ),
                Status::Ok.code()
            );
            assert_eq!(
                sto_tpcc_visit_fixed(
                    thread,
                    table,
                    packed4.as_ptr().cast(),
                    packed4.len(),
                    4,
                    Some(collect_fixed_read),
                    (&mut context as *mut FixedReadContext).cast(),
                    &mut visited,
                ),
                Status::Ok.code()
            );
            assert_eq!(visited, packed4.len());
            assert_eq!(
                context.observed,
                [
                    (0, Some(b"four-b".to_vec())),
                    (1, Some(b"staged".to_vec())),
                    (2, Some(b"staged".to_vec())),
                    (3, None),
                ]
            );
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            context = FixedReadContext::default();
            visited = usize::MAX;
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_visit_fixed(
                    thread,
                    table,
                    key8.as_ptr(),
                    1,
                    8,
                    Some(collect_fixed_read),
                    (&mut context as *mut FixedReadContext).cast(),
                    &mut visited,
                ),
                Status::Ok.code()
            );
            assert_eq!(visited, 1);
            // The raw layer retains the complete encoded value; a
            // metadata-aware wrapper validates and subtracts its suffix
            // before applying the C++ capability's logical limit.
            assert_eq!(context.observed, [(0, Some(b"eight".to_vec()))]);
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            for (key, width, expected) in [
                (&key12[..], 12, &b"twelve"[..]),
                (&key16[..], 16, &b"sixteen"[..]),
            ] {
                context = FixedReadContext::default();
                visited = usize::MAX;
                assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
                assert_eq!(
                    sto_tpcc_visit_fixed(
                        thread,
                        table,
                        key.as_ptr(),
                        1,
                        width,
                        Some(collect_fixed_read),
                        (&mut context as *mut FixedReadContext).cast(),
                        &mut visited,
                    ),
                    Status::Ok.code()
                );
                assert_eq!(visited, 1);
                assert_eq!(context.observed, [(0, Some(expected.to_vec()))]);
                assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());
            }

            context = FixedReadContext {
                observed: Vec::new(),
                fail_at: Some(1),
            };
            visited = usize::MAX;
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_visit_fixed(
                    thread,
                    table,
                    packed4.as_ptr().cast(),
                    packed4.len(),
                    4,
                    Some(collect_fixed_read),
                    (&mut context as *mut FixedReadContext).cast(),
                    &mut visited,
                ),
                Status::Fatal.code()
            );
            assert_eq!(visited, 2);
            assert_eq!(context.observed.len(), 2);
            assert!((*thread).active.is_none());
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());

            context = FixedReadContext::default();
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            visited = usize::MAX;
            assert_eq!(
                sto_tpcc_visit_fixed(
                    thread,
                    table,
                    ptr::null(),
                    0,
                    4,
                    Some(collect_fixed_read),
                    (&mut context as *mut FixedReadContext).cast(),
                    &mut visited,
                ),
                Status::Ok.code()
            );
            assert_eq!(visited, 0);
            assert!(context.observed.is_empty());

            visited = usize::MAX;
            assert_eq!(
                sto_tpcc_visit_fixed(
                    thread,
                    table,
                    ptr::null(),
                    0,
                    4,
                    None,
                    (&mut context as *mut FixedReadContext).cast(),
                    &mut visited,
                ),
                Status::Fatal.code()
            );
            assert_eq!(visited, usize::MAX);

            assert_eq!(
                sto_tpcc_visit_fixed(
                    thread,
                    table,
                    ptr::null(),
                    0,
                    4,
                    Some(collect_fixed_read),
                    (&mut context as *mut FixedReadContext).cast(),
                    ptr::null_mut(),
                ),
                Status::Fatal.code()
            );

            visited = usize::MAX;
            assert_eq!(
                sto_tpcc_visit_fixed(
                    thread,
                    table,
                    key4_a.as_ptr(),
                    1,
                    3,
                    Some(collect_fixed_read),
                    (&mut context as *mut FixedReadContext).cast(),
                    &mut visited,
                ),
                Status::Fatal.code()
            );
            assert_eq!(visited, 0);
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            visited = usize::MAX;
            assert_eq!(
                sto_tpcc_visit_fixed(
                    thread,
                    table,
                    ptr::null(),
                    1,
                    4,
                    Some(collect_fixed_read),
                    (&mut context as *mut FixedReadContext).cast(),
                    &mut visited,
                ),
                Status::Fatal.code()
            );
            assert_eq!(visited, 0);
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());

            assert_eq!(sto_tpcc_thread_destroy(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(table), Status::Ok.code());
            assert_eq!(sto_tpcc_db_destroy(db), Status::Ok.code());
        }
    }

    #[cfg(mtree_native_integration)]
    #[test]
    fn fixed_mutation_preserves_actions_duplicates_size_and_failure_boundaries() {
        unsafe {
            let config = StoTpccDbConfig {
                max_threads: NATIVE_TEST_MAX_THREADS,
                max_key_length: NATIVE_TEST_MAX_KEY_LENGTH,
                max_items_per_txn: 128,
                max_locks_per_txn: 256,
            };
            let mut db = ptr::null_mut();
            let mut table = ptr::null_mut();
            let mut thread = ptr::null_mut();
            assert_eq!(sto_tpcc_db_create(&config, &mut db), Status::Ok.code());
            assert_eq!(
                sto_tpcc_table_create(db, ptr::null(), &mut table),
                Status::Ok.code()
            );
            assert_eq!(sto_tpcc_thread_create(db, &mut thread), Status::Ok.code());

            let key4_a = [0x10, 0x20, 0x30, 0x40];
            let key4_b = [0x11, 0x21, 0x31, 0x41];
            let key4_c = [0x12, 0x22, 0x32, 0x42];
            let key4_d = [0x13, 0x23, 0x33, 0x43];
            let key8_a = [1, 2, 3, 4, 5, 6, 7, 8];
            let key8_b = [8, 7, 6, 5, 4, 3, 2, 1];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            for (key, value) in [(&key4_a[..], &b"a"[..]), (&key8_a[..], &b"e"[..])] {
                assert_eq!(
                    sto_tpcc_insert(
                        thread,
                        table,
                        key.as_ptr(),
                        key.len(),
                        value.as_ptr(),
                        value.len(),
                    ),
                    Status::Ok.code()
                );
            }
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // Width four covers keep, put-on-miss, and remove-on-miss.
            let packed4 = [key4_a, key4_b, key4_c];
            let mut context = FixedModifyContext::new([
                FixedModifyStep::Keep,
                FixedModifyStep::Put(b"b".to_vec()),
                FixedModifyStep::Remove,
            ]);
            let mut visited = usize::MAX;
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_modify_fixed(
                    thread,
                    table,
                    packed4.as_ptr().cast(),
                    packed4.len(),
                    4,
                    Some(apply_fixed_modify_step),
                    (&mut context as *mut FixedModifyContext).cast(),
                    &mut visited,
                ),
                Status::Ok.code()
            );
            assert_eq!(visited, packed4.len());
            assert_eq!(
                context.observed,
                [(0, Some(b"a".to_vec())), (1, None), (2, None)]
            );
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // Width eight covers remove-on-hit and put-on-miss with zero net
            // size change.
            let packed8 = [key8_a, key8_b];
            context = FixedModifyContext::new([
                FixedModifyStep::Remove,
                FixedModifyStep::Put(b"f".to_vec()),
            ]);
            visited = usize::MAX;
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_modify_fixed(
                    thread,
                    table,
                    packed8.as_ptr().cast(),
                    packed8.len(),
                    8,
                    Some(apply_fixed_modify_step),
                    (&mut context as *mut FixedModifyContext).cast(),
                    &mut visited,
                ),
                Status::Ok.code()
            );
            assert_eq!(visited, packed8.len());
            assert_eq!(context.observed, [(0, Some(b"e".to_vec())), (1, None)]);
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // Duplicate positions observe earlier writes. Put->put creates
            // one row; put->remove ends absent with no size change.
            let repeated = [key4_d, key4_d, key4_c, key4_c];
            context = FixedModifyContext::new([
                FixedModifyStep::Put(b"d1".to_vec()),
                FixedModifyStep::Put(b"d2".to_vec()),
                FixedModifyStep::Put(b"temporary".to_vec()),
                FixedModifyStep::Remove,
            ]);
            visited = usize::MAX;
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_modify_fixed(
                    thread,
                    table,
                    repeated.as_ptr().cast(),
                    repeated.len(),
                    4,
                    Some(apply_fixed_modify_step),
                    (&mut context as *mut FixedModifyContext).cast(),
                    &mut visited,
                ),
                Status::Ok.code()
            );
            assert_eq!(visited, repeated.len());
            assert_eq!(
                context.observed,
                [
                    (0, None),
                    (1, Some(b"d1".to_vec())),
                    (2, None),
                    (3, Some(b"temporary".to_vec())),
                ]
            );
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            let mut rows = u64::MAX;
            assert_eq!(sto_tpcc_table_size(table, &mut rows), Status::Ok.code());
            assert_eq!(rows, 4);
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            let (status, actual, output) = get_with_sentinel(thread, table, &key4_d, 8, 0xee);
            assert_eq!(status, Status::Ok.code());
            assert_eq!(actual, 2);
            assert_eq!(&output[..actual], b"d2");
            let (status, actual) = get_with_capacity(thread, table, &key4_c, 8);
            assert_eq!(status, Status::Miss.code());
            assert_eq!(actual, 0);
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // A middle callback failure aborts both earlier batch mutations
            // and unrelated pending size accounting, then permits a clean
            // fresh attempt.
            let pending_key = [9, 9, 9, 9, 9, 9, 9, 9];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_insert(
                    thread,
                    table,
                    pending_key.as_ptr(),
                    pending_key.len(),
                    b"pending".as_ptr(),
                    b"pending".len(),
                ),
                Status::Ok.code()
            );
            assert!(!(*thread).pending_size.is_empty());
            context = FixedModifyContext::new([
                FixedModifyStep::Put(b"not-committed".to_vec()),
                FixedModifyStep::Failed,
                FixedModifyStep::Put(b"not-visited".to_vec()),
            ]);
            visited = usize::MAX;
            assert_eq!(
                sto_tpcc_modify_fixed(
                    thread,
                    table,
                    packed4.as_ptr().cast(),
                    packed4.len(),
                    4,
                    Some(apply_fixed_modify_step),
                    (&mut context as *mut FixedModifyContext).cast(),
                    &mut visited,
                ),
                Status::Fatal.code()
            );
            assert_eq!(visited, 2);
            assert_eq!(context.observed.len(), 2);
            assert!((*thread).active.is_none());
            assert!((*thread).pending_size.is_empty());

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            for key in [&pending_key[..], &key4_c[..]] {
                let (status, actual) = get_with_capacity(thread, table, key, 16);
                assert_eq!(status, Status::Miss.code());
                assert_eq!(actual, 0);
            }
            let (status, actual, output) = get_with_sentinel(thread, table, &key4_a, 8, 0xee);
            assert_eq!(status, Status::Ok.code());
            assert_eq!(&output[..actual], b"a");
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // Invalid actions and replacement ranges are callback failures:
            // each closes the attempt and leaves no pending accounting.
            for step in [
                FixedModifyStep::Unknown,
                FixedModifyStep::NullNonzero,
                FixedModifyStep::Oversize,
            ] {
                context = FixedModifyContext::new([step]);
                visited = usize::MAX;
                assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
                assert_eq!(
                    sto_tpcc_modify_fixed(
                        thread,
                        table,
                        key4_a.as_ptr(),
                        1,
                        4,
                        Some(apply_fixed_modify_step),
                        (&mut context as *mut FixedModifyContext).cast(),
                        &mut visited,
                    ),
                    Status::Fatal.code()
                );
                assert_eq!(visited, 1);
                assert!((*thread).active.is_none());
                assert!((*thread).pending_size.is_empty());
            }

            // Empty input is successful. Fully invalid key/callback/output
            // arguments leave out_visited untouched and do not consume the
            // still-active attempt.
            context = FixedModifyContext::new([]);
            visited = usize::MAX;
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_modify_fixed(
                    thread,
                    table,
                    ptr::null(),
                    0,
                    4,
                    Some(apply_fixed_modify_step),
                    (&mut context as *mut FixedModifyContext).cast(),
                    &mut visited,
                ),
                Status::Ok.code()
            );
            assert_eq!(visited, 0);

            for (key_pointer, key_count, key_width, callback) in [
                (
                    key4_a.as_ptr(),
                    1,
                    3,
                    Some(apply_fixed_modify_step as StoTpccFixedModifyCallback),
                ),
                (
                    ptr::null(),
                    1,
                    4,
                    Some(apply_fixed_modify_step as StoTpccFixedModifyCallback),
                ),
                (ptr::null(), 0, 4, None),
            ] {
                visited = usize::MAX;
                assert_eq!(
                    sto_tpcc_modify_fixed(
                        thread,
                        table,
                        key_pointer,
                        key_count,
                        key_width,
                        callback,
                        (&mut context as *mut FixedModifyContext).cast(),
                        &mut visited,
                    ),
                    Status::Fatal.code()
                );
                assert_eq!(visited, usize::MAX);
                assert!((*thread).active.is_some());
            }
            assert_eq!(
                sto_tpcc_modify_fixed(
                    thread,
                    table,
                    ptr::null(),
                    0,
                    4,
                    Some(apply_fixed_modify_step),
                    (&mut context as *mut FixedModifyContext).cast(),
                    ptr::null_mut(),
                ),
                Status::Fatal.code()
            );
            assert!((*thread).active.is_some());
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());

            assert_eq!(sto_tpcc_thread_destroy(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(table), Status::Ok.code());
            assert_eq!(sto_tpcc_db_destroy(db), Status::Ok.code());
        }
    }

    #[cfg(mtree_native_integration)]
    #[test]
    fn fixed_mutation_access_error_reports_only_the_delivered_prefix() {
        unsafe {
            let config = StoTpccDbConfig {
                max_threads: NATIVE_TEST_MAX_THREADS,
                max_key_length: NATIVE_TEST_MAX_KEY_LENGTH,
                max_items_per_txn: 16,
                max_locks_per_txn: 32,
            };
            let table_config = StoTpccTableConfig {
                max_retained_records: 1,
                max_retained_key_bytes: 16,
                max_consumed_record_ids: 1,
                ..StoTpccTableConfig::default()
            };
            let mut db = ptr::null_mut();
            let mut table = ptr::null_mut();
            let mut thread = ptr::null_mut();
            assert_eq!(sto_tpcc_db_create(&config, &mut db), Status::Ok.code());
            assert_eq!(
                sto_tpcc_table_create(db, &table_config, &mut table),
                Status::Ok.code()
            );
            assert_eq!(sto_tpcc_thread_create(db, &mut thread), Status::Ok.code());

            let present = [1, 2, 3, 4];
            let missing = [5, 6, 7, 8];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_insert(
                    thread,
                    table,
                    present.as_ptr(),
                    present.len(),
                    b"value".as_ptr(),
                    b"value".len(),
                ),
                Status::Ok.code()
            );
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            let keys = [present, missing];
            let mut context =
                FixedModifyContext::new([FixedModifyStep::Keep, FixedModifyStep::Keep]);
            let mut visited = usize::MAX;
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_modify_fixed(
                    thread,
                    table,
                    keys.as_ptr().cast(),
                    keys.len(),
                    4,
                    Some(apply_fixed_modify_step),
                    (&mut context as *mut FixedModifyContext).cast(),
                    &mut visited,
                ),
                Status::Fatal.code()
            );
            // Exact-unique fixed mutations pre-intern every miss before the
            // first callback. A reservation failure is therefore callback-free
            // even when an earlier key was already present.
            assert_eq!(visited, 0);
            assert!(context.observed.is_empty());
            assert!((*thread).active.as_ref().unwrap().transaction.is_doomed());
            assert!((*thread).pending_size.is_empty());
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());

            assert_eq!(sto_tpcc_thread_destroy(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(table), Status::Ok.code());
            assert_eq!(sto_tpcc_db_destroy(db), Status::Ok.code());
        }
    }

    #[cfg(mtree_native_integration)]
    #[test]
    fn fixed_put_supports_all_widths_mixed_rows_duplicates_abort_and_scratch_reuse() {
        unsafe {
            let config = StoTpccDbConfig {
                max_threads: NATIVE_TEST_MAX_THREADS,
                max_key_length: NATIVE_TEST_MAX_KEY_LENGTH,
                max_items_per_txn: 128,
                max_locks_per_txn: 256,
            };
            let mut db = ptr::null_mut();
            let mut table = ptr::null_mut();
            let mut bounded_table = ptr::null_mut();
            let mut thread = ptr::null_mut();
            assert_eq!(sto_tpcc_db_create(&config, &mut db), Status::Ok.code());
            assert_eq!(
                sto_tpcc_table_create(db, ptr::null(), &mut table),
                Status::Ok.code()
            );
            let bounded_config = StoTpccTableConfig {
                max_retained_records: 1,
                max_retained_key_bytes: 4,
                max_consumed_record_ids: 1,
                ..StoTpccTableConfig::default()
            };
            assert_eq!(
                sto_tpcc_table_create(db, &bounded_config, &mut bounded_table),
                Status::Ok.code()
            );
            assert_eq!(sto_tpcc_thread_create(db, &mut thread), Status::Ok.code());

            let retained_capacity = (*thread).point_batch.capacity();
            let keys4 = [[1, 2, 3, 4], [5, 6, 7, 8]];
            let keys8 = [[11; 8], [12; 8]];
            let keys12 = [[21; 12], [22; 12]];
            let keys16 = [[31; 16], [32; 16]];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            for (status, result) in [
                call_fixed_put(
                    thread,
                    table,
                    &keys4,
                    &[b"four-a", b""],
                    STO_TPCC_FIXED_PUT_UPSERT,
                ),
                call_fixed_put(
                    thread,
                    table,
                    &keys8,
                    &[b"eight-a", b"eight-b"],
                    STO_TPCC_FIXED_PUT_UPSERT,
                ),
                call_fixed_put(
                    thread,
                    table,
                    &keys12,
                    &[b"twelve-a", b"twelve-b"],
                    STO_TPCC_FIXED_PUT_UPSERT,
                ),
                call_fixed_put(
                    thread,
                    table,
                    &keys16,
                    &[b"sixteen-a", b"sixteen-b"],
                    STO_TPCC_FIXED_PUT_UPSERT,
                ),
            ] {
                assert_eq!(status, Status::Ok.code());
                assert_eq!(result.inserted, 2);
                assert_eq!(result.first_duplicate, usize::MAX);
            }
            assert_eq!((*thread).point_batch.capacity(), retained_capacity);
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            let mut rows = u64::MAX;
            assert_eq!(sto_tpcc_table_size(table, &mut rows), Status::Ok.code());
            assert_eq!(rows, 8);

            // A mixed hit/miss upsert counts only the absent transition.
            let mixed = [keys4[0], [9, 9, 9, 9]];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            let (status, result) = call_fixed_put(
                thread,
                table,
                &mixed,
                &[b"four-a2", b"new-nine"],
                STO_TPCC_FIXED_PUT_UPSERT,
            );
            assert_eq!(status, Status::Ok.code());
            assert_eq!(result.inserted, 1);
            assert_eq!(result.first_duplicate, usize::MAX);
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // INSERT processes sequential duplicates without overwriting the
            // earlier staged value. Other absent positions remain staged and
            // may be committed after the explicit DUPLICATE result.
            let duplicate_key = [41, 42, 43, 44];
            let insert_keys = [duplicate_key, duplicate_key, keys4[0], [51, 52, 53, 54]];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            let (status, result) = call_fixed_put(
                thread,
                table,
                &insert_keys,
                &[b"first", b"ignored", b"also-ignored", b"last"],
                STO_TPCC_FIXED_PUT_INSERT,
            );
            assert_eq!(status, Status::Duplicate.code());
            assert_eq!(result.inserted, 2);
            assert_eq!(result.first_duplicate, 1);
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_size(table, &mut rows), Status::Ok.code());
            assert_eq!(rows, 11);

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            let (status, actual, value) =
                get_with_sentinel(thread, table, &duplicate_key, 16, 0xee);
            assert_eq!(status, Status::Ok.code());
            assert_eq!(&value[..actual], b"first");
            let (status, actual, value) = get_with_sentinel(thread, table, &keys4[0], 16, 0xee);
            assert_eq!(status, Status::Ok.code());
            assert_eq!(&value[..actual], b"four-a2");
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // Aborting preserves physical directory bindings but no logical
            // size delta. A fresh insert attempt reuses those tombstones.
            let aborted = [[61; 12], [62; 12]];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            let (status, result) = call_fixed_put(
                thread,
                table,
                &aborted,
                &[b"abort-a", b"abort-b"],
                STO_TPCC_FIXED_PUT_INSERT,
            );
            assert_eq!(status, Status::Ok.code());
            assert_eq!(result.inserted, 2);
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_size(table, &mut rows), Status::Ok.code());
            assert_eq!(rows, 11);

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            let (status, result) = call_fixed_put(
                thread,
                table,
                &aborted,
                &[b"retry-a", b"retry-b"],
                STO_TPCC_FIXED_PUT_INSERT,
            );
            assert_eq!(status, Status::Ok.code());
            assert_eq!(result.inserted, 2);
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_size(table, &mut rows), Status::Ok.code());
            assert_eq!(rows, 13);
            assert_eq!((*thread).point_batch.capacity(), retained_capacity);

            exercise_fixed_put_hostile_inputs_and_capacity(thread, bounded_table);

            assert_eq!(sto_tpcc_thread_destroy(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(bounded_table), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(table), Status::Ok.code());
            assert_eq!(sto_tpcc_db_destroy(db), Status::Ok.code());
        }
    }

    #[cfg(mtree_native_integration)]
    unsafe fn exercise_fixed_put_hostile_inputs_and_capacity(
        thread: *mut StoTpccThread,
        table: *mut StoTpccTable,
    ) {
        unsafe {
            let retained_capacity = (*thread).point_batch.capacity();

            let keys = [[1, 2, 3, 4], [5, 6, 7, 8]];
            let values = *b"ab";
            let descriptors = [
                StoTpccFixedValue {
                    data: values.as_ptr(),
                    length: 1,
                },
                StoTpccFixedValue {
                    data: values.as_ptr().add(1),
                    length: 1,
                },
            ];
            let mut result = StoTpccFixedPutResult {
                inserted: 777,
                first_duplicate: 888,
            };
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_put_fixed(
                    thread,
                    table,
                    keys.as_ptr().cast(),
                    keys.len(),
                    4,
                    descriptors.as_ptr(),
                    STO_TPCC_FIXED_PUT_UPSERT,
                    &mut result,
                ),
                Status::Fatal.code()
            );
            assert_eq!(result, StoTpccFixedPutResult::default());
            assert!((*thread).active.as_ref().unwrap().transaction.is_doomed());
            assert!((*thread).pending_size.is_empty());
            assert_eq!((*thread).point_batch.capacity(), retained_capacity);
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());

            // The first pre-interned key is a reusable tombstone after the
            // second key exhausted the bounded registry.
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            let (status, retry_result) = call_fixed_put(
                thread,
                table,
                &keys[..1],
                &[b"retry"],
                STO_TPCC_FIXED_PUT_INSERT,
            );
            assert_eq!(status, Status::Ok.code());
            assert_eq!(retry_result.inserted, 1);
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // Validation failures leave both the output and active attempt
            // untouched. The misaligned descriptor pointer is rejected before it
            // can be dereferenced.
            let valid_key = keys[0];
            let valid_value = [StoTpccFixedValue {
                data: values.as_ptr(),
                length: 1,
            }];
            let null_nonzero = [StoTpccFixedValue {
                data: ptr::null(),
                length: 1,
            }];
            let oversized = [StoTpccFixedValue {
                data: values.as_ptr(),
                length: isize::MAX as usize + 1,
            }];
            let overflowing = [StoTpccFixedValue {
                data: usize::MAX as *const u8,
                length: 2,
            }];
            let descriptor_words = [StoTpccFixedValue {
                data: ptr::null(),
                length: 0,
            }; 2];
            let misaligned_values = descriptor_words
                .as_ptr()
                .cast::<u8>()
                .add(1)
                .cast::<StoTpccFixedValue>();
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());

            macro_rules! invalid {
                ($keys:expr, $count:expr, $width:expr, $values:expr, $mode:expr, $output:expr) => {{
                    result = StoTpccFixedPutResult {
                        inserted: 777,
                        first_duplicate: 888,
                    };
                    assert_eq!(
                        sto_tpcc_put_fixed(
                            thread, table, $keys, $count, $width, $values, $mode, $output,
                        ),
                        Status::Fatal.code()
                    );
                    assert_eq!(
                        result,
                        StoTpccFixedPutResult {
                            inserted: 777,
                            first_duplicate: 888,
                        }
                    );
                    assert!((*thread).active.is_some());
                    assert!(!(*thread).active.as_ref().unwrap().transaction.is_doomed());
                }};
            }

            invalid!(
                valid_key.as_ptr(),
                1,
                3,
                valid_value.as_ptr(),
                STO_TPCC_FIXED_PUT_UPSERT,
                &mut result
            );
            invalid!(
                ptr::null(),
                1,
                4,
                valid_value.as_ptr(),
                STO_TPCC_FIXED_PUT_UPSERT,
                &mut result
            );
            invalid!(
                valid_key.as_ptr(),
                1,
                4,
                ptr::null(),
                STO_TPCC_FIXED_PUT_UPSERT,
                &mut result
            );
            invalid!(
                valid_key.as_ptr(),
                1,
                4,
                null_nonzero.as_ptr(),
                STO_TPCC_FIXED_PUT_UPSERT,
                &mut result
            );
            invalid!(
                valid_key.as_ptr(),
                1,
                4,
                oversized.as_ptr(),
                STO_TPCC_FIXED_PUT_UPSERT,
                &mut result
            );
            invalid!(
                valid_key.as_ptr(),
                1,
                4,
                overflowing.as_ptr(),
                STO_TPCC_FIXED_PUT_UPSERT,
                &mut result
            );
            invalid!(
                valid_key.as_ptr(),
                1,
                4,
                misaligned_values,
                STO_TPCC_FIXED_PUT_UPSERT,
                &mut result
            );
            invalid!(
                valid_key.as_ptr(),
                1,
                4,
                valid_value.as_ptr(),
                99,
                &mut result
            );
            result = StoTpccFixedPutResult {
                inserted: 777,
                first_duplicate: 888,
            };
            assert_eq!(
                sto_tpcc_put_fixed(
                    thread,
                    table,
                    ptr::null(),
                    usize::MAX,
                    4,
                    ptr::null(),
                    STO_TPCC_FIXED_PUT_UPSERT,
                    &mut result,
                ),
                Status::Fatal.code()
            );
            assert_eq!(result.inserted, 777);
            assert!((*thread).active.is_some());
            assert_eq!(
                sto_tpcc_put_fixed(
                    thread,
                    table,
                    valid_key.as_ptr(),
                    1,
                    4,
                    valid_value.as_ptr(),
                    STO_TPCC_FIXED_PUT_UPSERT,
                    ptr::null_mut(),
                ),
                Status::Fatal.code()
            );
            assert!((*thread).active.is_some());

            // Empty batches accept all-null input and reset the output.
            result = StoTpccFixedPutResult {
                inserted: 777,
                first_duplicate: 888,
            };
            assert_eq!(
                sto_tpcc_put_fixed(
                    thread,
                    table,
                    ptr::null(),
                    0,
                    16,
                    ptr::null(),
                    STO_TPCC_FIXED_PUT_INSERT,
                    &mut result,
                ),
                Status::Ok.code()
            );
            assert_eq!(result, StoTpccFixedPutResult::default());
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());
        }
    }

    #[test]
    fn last_only_lane_does_not_grow_the_inline_cache_object() {
        assert_eq!(
            mem::size_of::<ResolvedCache>(),
            mem::size_of::<Vec<ResolvedCacheEntry>>() + mem::size_of::<Option<usize>>()
        );
        let cache = ResolvedCache::default();
        assert_eq!(cache.entries.len(), RESOLVED_CACHE_ENTRIES + 1);
        assert!(cache.entries.iter().all(|entry| entry.record.is_none()));
        assert_eq!(cache.last_slot, None);
    }

    #[test]
    fn resolved_cache_hash_distributes_big_endian_customer_ids() {
        let mut occupied = vec![false; RESOLVED_CACHE_ENTRIES];
        let table_hint = 0x1234_5678_9abc_def0;
        for district_id in 1_i32..=10 {
            for customer_id in 1_i32..=3_000 {
                let mut key = [0_u8; 12];
                key[..4].copy_from_slice(&1_i32.to_be_bytes());
                key[4..8].copy_from_slice(&district_id.to_be_bytes());
                key[8..].copy_from_slice(&customer_id.to_be_bytes());
                let slot =
                    ResolvedCache::hash(table_hint, &key) as usize & (RESOLVED_CACHE_ENTRIES - 1);
                occupied[slot] = true;
            }
        }
        let used = occupied.into_iter().filter(|used| *used).count();
        assert!(
            used > 3_800,
            "30,000 customer keys reached only {used} cache slots"
        );

        let district_slots = (1_i32..=10)
            .map(|district_id| {
                let mut key = [0_u8; 8];
                key[..4].copy_from_slice(&1_i32.to_be_bytes());
                key[4..].copy_from_slice(&district_id.to_be_bytes());
                ResolvedCache::hash(table_hint, &key) as usize & (RESOLVED_CACHE_ENTRIES - 1)
            })
            .collect::<std::collections::HashSet<_>>();
        assert_eq!(district_slots.len(), 10);

        let aliased_customer_data = (1_i32..=10)
            .flat_map(|district_id| {
                (1_i32..=3_000).map(move |customer_id| (district_id, customer_id))
            })
            .filter(|(district_id, customer_id)| {
                let mut customer = [0_u8; 12];
                customer[..4].copy_from_slice(&1_i32.to_be_bytes());
                customer[4..8].copy_from_slice(&district_id.to_be_bytes());
                customer[8..].copy_from_slice(&customer_id.to_be_bytes());
                let mut customer_data = customer;
                customer_data[4..8].copy_from_slice(&(district_id + 100).to_be_bytes());
                let customer_slot = ResolvedCache::hash(table_hint, &customer) as usize
                    & (RESOLVED_CACHE_ENTRIES - 1);
                let customer_data_slot = ResolvedCache::hash(table_hint, &customer_data) as usize
                    & (RESOLVED_CACHE_ENTRIES - 1);
                customer_slot == customer_data_slot
            })
            .count();
        assert!(
            aliased_customer_data < 100,
            "{aliased_customer_data} customer and customer-data keys alias"
        );
    }

    #[cfg(mtree_native_integration)]
    #[test]
    fn dense_item_and_stock_paths_warm_mix_and_disable_on_warehouse_mismatch() {
        unsafe {
            let config = StoTpccDbConfig {
                max_threads: NATIVE_TEST_MAX_THREADS,
                max_key_length: NATIVE_TEST_MAX_KEY_LENGTH,
                max_items_per_txn: 1_024,
                max_locks_per_txn: 2_048,
            };
            let mut db = ptr::null_mut();
            assert_eq!(sto_tpcc_db_create(&config, &mut db), Status::Ok.code());

            let mut item = ptr::null_mut();
            let mut stock = ptr::null_mut();
            assert_eq!(
                sto_tpcc_table_create_with_cache_policy(
                    db,
                    ptr::null(),
                    STO_TPCC_RESOLVED_CACHE_DENSE_ITEM,
                    &mut item,
                ),
                Status::Ok.code()
            );
            assert_eq!(
                sto_tpcc_table_create_with_cache_policy(
                    db,
                    ptr::null(),
                    STO_TPCC_RESOLVED_CACHE_DENSE_STOCK,
                    &mut stock,
                ),
                Status::Ok.code()
            );
            let item_handle = &*item;
            let stock_handle = &*stock;
            assert_eq!(item_handle.cache_policy, ResolvedCachePolicy::None);
            assert_eq!(item_handle.dense_policy, DenseCachePolicy::Item);
            assert_eq!(
                stock_handle.cache_policy,
                ResolvedCachePolicy::ReadThenWrite
            );
            assert_eq!(stock_handle.dense_policy, DenseCachePolicy::Stock);
            assert_eq!(item_handle.dense_cache.as_ref().unwrap().len(), 100_000);
            assert_eq!(stock_handle.dense_cache.as_ref().unwrap().len(), 100_000);

            let mut thread = ptr::null_mut();
            assert_eq!(sto_tpcc_thread_create(db, &mut thread), Status::Ok.code());
            let item_ids = [1_u32, 2, 3, 4];
            let item_values = [
                item_fixture(1.25),
                item_fixture(2.5),
                item_fixture(3.75),
                item_fixture(5.0),
            ];
            let stock_values = [
                stock_fixture(30),
                stock_fixture(15),
                stock_fixture(25),
                stock_fixture(40),
            ];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            for index in 0..item_ids.len() {
                let item_key = item_ids[index].to_be_bytes();
                assert_eq!(
                    sto_tpcc_insert(
                        thread,
                        item,
                        item_key.as_ptr(),
                        item_key.len(),
                        item_values[index].as_ptr(),
                        item_values[index].len(),
                    ),
                    Status::Ok.code()
                );
                for warehouse_id in [1_i32, 2] {
                    let mut stock_key = [0_u8; 8];
                    stock_key[..4].copy_from_slice(&warehouse_id.to_be_bytes());
                    stock_key[4..].copy_from_slice(&item_key);
                    assert_eq!(
                        sto_tpcc_insert(
                            thread,
                            stock,
                            stock_key.as_ptr(),
                            stock_key.len(),
                            stock_values[index].as_ptr(),
                            stock_values[index].len(),
                        ),
                        Status::Ok.code()
                    );
                }
            }
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_table_seal_directory_structure(item),
                Status::Ok.code()
            );
            assert_eq!(
                sto_tpcc_table_seal_directory_structure(stock),
                Status::Ok.code()
            );

            (*thread).resolved_cache = ResolvedCache::default();
            reset_cache_slot_calls();
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                &new_order_read_items(&mut *thread, item_handle, &item_ids[..2]).unwrap()[..2],
                &[1.25, 2.5]
            );
            new_order_modify_stocks(&mut *thread, stock_handle, 1, &item_ids[..2], &[1, 1])
                .unwrap();
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());

            let item_cache = item_handle.dense_cache.as_ref().unwrap();
            let stock_cache = stock_handle.dense_cache.as_ref().unwrap();
            for item_id in &item_ids[..2] {
                assert!(item_cache
                    .get(dense_item_slot(*item_id).unwrap())
                    .unwrap()
                    .is_some());
                assert!(stock_cache
                    .get(dense_item_slot(*item_id).unwrap())
                    .unwrap()
                    .is_some());
            }
            assert!(item_cache
                .get(dense_item_slot(3).unwrap())
                .unwrap()
                .is_none());
            assert!(stock_cache
                .get(dense_item_slot(3).unwrap())
                .unwrap()
                .is_none());

            let mixed_items = [item_ids[0], item_ids[2], item_ids[1], item_ids[0]];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                &new_order_read_items(&mut *thread, item_handle, &mixed_items).unwrap()[..4],
                &[1.25, 3.75, 2.5, 1.25]
            );
            let mut recent = StockLevelItemSet::new();
            assert_eq!(recent.insert(item_ids[0]), Ok(true));
            assert_eq!(recent.insert(item_ids[2]), Ok(true));
            assert_eq!(recent.insert(item_ids[1]), Ok(true));
            assert_eq!(
                stock_level_count_low_stock(&mut *thread, stock_handle, 1, &recent, 20),
                Ok(1)
            );
            assert_eq!(stock_level_cache_partition(), (2, 1));
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            assert!(item_cache
                .get(dense_item_slot(3).unwrap())
                .unwrap()
                .is_some());
            assert!(stock_cache
                .get(dense_item_slot(3).unwrap())
                .unwrap()
                .is_some());
            assert_eq!(
                stock_handle
                    .dense_stock_warehouse_id
                    .load(Ordering::Acquire),
                1
            );
            assert_eq!(cache_slot_calls(), 0);
            assert!((*thread)
                .resolved_cache
                .entries
                .iter()
                .all(|entry| entry.record.is_none()));
            assert_eq!((*thread).resolved_cache.last_slot, None);

            let mixed_stocks = [item_ids[0], item_ids[3], item_ids[1], item_ids[0]];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            new_order_modify_stocks(&mut *thread, stock_handle, 1, &mixed_stocks, &[3, 1, 2, 4])
                .unwrap();
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            for (item_id, expected_quantity) in [(1_u32, 23_i16), (2, 13), (4, 39)] {
                let resolved = stock_cache
                    .get(dense_item_slot(item_id).unwrap())
                    .unwrap()
                    .unwrap();
                let mut observed = None;
                stock_handle
                    .state
                    .table
                    .visit_get_resolved_bytes(
                        active_transaction(&mut (*thread).active).unwrap(),
                        resolved,
                        |current| {
                            observed = current
                                .map(|bytes| i16::from_ne_bytes(bytes[..2].try_into().unwrap()));
                        },
                    )
                    .unwrap();
                assert_eq!(observed, Some(expected_quantity));
            }
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            let mut other_warehouse = StockLevelItemSet::new();
            assert_eq!(other_warehouse.insert(1), Ok(true));
            assert_eq!(other_warehouse.insert(2), Ok(true));
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                stock_level_count_low_stock(&mut *thread, stock_handle, 2, &other_warehouse, 20,),
                Ok(1)
            );
            assert_eq!(stock_level_cache_partition(), (0, 2));
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());
            assert_eq!(
                stock_handle
                    .dense_stock_warehouse_id
                    .load(Ordering::Acquire),
                -1
            );

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                stock_level_count_low_stock(&mut *thread, stock_handle, 1, &other_warehouse, 20,),
                Ok(1)
            );
            assert_eq!(stock_level_cache_partition(), (0, 2));
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());
            assert_eq!(cache_slot_calls(), 0);
            assert!((*thread)
                .resolved_cache
                .entries
                .iter()
                .all(|entry| entry.record.is_none()));
            assert_eq!((*thread).resolved_cache.last_slot, None);

            assert_eq!(sto_tpcc_thread_destroy(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(item), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(stock), Status::Ok.code());
            assert_eq!(sto_tpcc_db_destroy(db), Status::Ok.code());
        }
    }

    #[cfg(mtree_native_integration)]
    #[test]
    fn new_order_stock_tokens_drive_stock_level_hit_and_compact_miss_paths() {
        unsafe {
            let config = StoTpccDbConfig {
                max_threads: NATIVE_TEST_MAX_THREADS,
                max_key_length: NATIVE_TEST_MAX_KEY_LENGTH,
                max_items_per_txn: 1_024,
                max_locks_per_txn: 2_048,
            };
            let mut db = ptr::null_mut();
            assert_eq!(sto_tpcc_db_create(&config, &mut db), Status::Ok.code());

            let mut stock = ptr::null_mut();
            assert_eq!(
                sto_tpcc_table_create_with_cache_policy(
                    db,
                    ptr::null(),
                    STO_TPCC_RESOLVED_CACHE_FULL,
                    &mut stock,
                ),
                Status::Ok.code()
            );
            let stock_handle = &*stock;
            let stock_table = &stock_handle.state.table;
            let mut thread = ptr::null_mut();
            assert_eq!(sto_tpcc_thread_create(db, &mut thread), Status::Ok.code());

            let warehouse_id = 1_i32;
            let mut item_ids = [0_u32; 4];
            let mut item_slots = [usize::MAX; 4];
            let mut item_count = 0_usize;
            for item_id in 1_u32..=100_000 {
                let mut key = [0_u8; 8];
                key[..4].copy_from_slice(&warehouse_id.to_be_bytes());
                key[4..].copy_from_slice(&item_id.to_be_bytes());
                let slot = ResolvedCache::slot(stock_table, &key);
                if item_slots[..item_count].contains(&slot) {
                    continue;
                }
                item_ids[item_count] = item_id;
                item_slots[item_count] = slot;
                item_count += 1;
                if item_count == item_ids.len() {
                    break;
                }
            }
            assert_eq!(item_count, item_ids.len());
            let values = [
                stock_fixture(5),
                stock_fixture(15),
                stock_fixture(25),
                vec![0_u8],
            ];
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            for (item_id, value) in item_ids.into_iter().zip(&values) {
                let mut key = [0_u8; 8];
                key[..4].copy_from_slice(&warehouse_id.to_be_bytes());
                key[4..].copy_from_slice(&item_id.to_be_bytes());
                assert_eq!(
                    sto_tpcc_insert(
                        thread,
                        stock,
                        key.as_ptr(),
                        key.len(),
                        value.as_ptr(),
                        value.len(),
                    ),
                    Status::Ok.code()
                );
            }
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_table_seal_directory_structure(stock),
                Status::Ok.code()
            );

            // NewOrder mints and retains each exact stock token before the
            // attempt finishes. Aborting the values does not invalidate those
            // stable identities.
            (*thread).resolved_cache = ResolvedCache::default();
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            new_order_modify_stocks(&mut *thread, &*stock, warehouse_id, &item_ids[..2], &[1, 1])
                .unwrap();
            for item_id in &item_ids[..2] {
                let mut key = [0_u8; 8];
                key[..4].copy_from_slice(&warehouse_id.to_be_bytes());
                key[4..].copy_from_slice(&item_id.to_be_bytes());
                assert!((*thread)
                    .resolved_cache
                    .matching(stock_table, &key)
                    .is_some());
            }
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());

            let mut recent = StockLevelItemSet::new();
            assert_eq!(recent.insert(item_ids[0]), Ok(true));
            assert_eq!(recent.insert(item_ids[1]), Ok(true));
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                stock_level_count_low_stock(&mut *thread, &*stock, warehouse_id, &recent, 20),
                Ok(2)
            );
            assert_eq!(stock_level_cache_partition(), (2, 0));
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // An empty cache produces one compact all-miss batch. Every
            // returned token is remembered, making the next attempt all-hit.
            (*thread).resolved_cache = ResolvedCache::default();
            let mut three = StockLevelItemSet::new();
            for item_id in &item_ids[..3] {
                assert_eq!(three.insert(*item_id), Ok(true));
            }
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                stock_level_count_low_stock(&mut *thread, &*stock, warehouse_id, &three, 20),
                Ok(2)
            );
            assert_eq!(stock_level_cache_partition(), (0, 3));
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                stock_level_count_low_stock(&mut *thread, &*stock, warehouse_id, &three, 20),
                Ok(2)
            );
            assert_eq!(stock_level_cache_partition(), (3, 0));
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // Evict only item 2. The next attempt must use two exact tokens
            // and one compact miss, then restore item 2's resolution.
            let mut second_key = [0_u8; 8];
            second_key[..4].copy_from_slice(&warehouse_id.to_be_bytes());
            second_key[4..].copy_from_slice(&item_ids[1].to_be_bytes());
            let second_slot = ResolvedCache::slot(stock_table, &second_key);
            (&mut (*thread).resolved_cache.entries)[second_slot] = ResolvedCacheEntry::default();
            if (*thread).resolved_cache.last_slot == Some(second_slot) {
                (*thread).resolved_cache.last_slot = None;
            }
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                stock_level_count_low_stock(&mut *thread, &*stock, warehouse_id, &three, 20),
                Ok(2)
            );
            assert_eq!(stock_level_cache_partition(), (2, 1));
            assert!((*thread)
                .resolved_cache
                .matching(stock_table, &second_key)
                .is_some());
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // Item 1 is cached and item 4 is a compact miss. Its malformed
            // row must still be reported at original input index 1.
            let mut fourth_key = [0_u8; 8];
            fourth_key[..4].copy_from_slice(&warehouse_id.to_be_bytes());
            fourth_key[4..].copy_from_slice(&item_ids[3].to_be_bytes());
            let fourth_slot = ResolvedCache::slot(stock_table, &fourth_key);
            (&mut (*thread).resolved_cache.entries)[fourth_slot] = ResolvedCacheEntry::default();
            if (*thread).resolved_cache.last_slot == Some(fourth_slot) {
                (*thread).resolved_cache.last_slot = None;
            }
            let mut malformed = StockLevelItemSet::new();
            assert_eq!(malformed.insert(item_ids[0]), Ok(true));
            assert_eq!(malformed.insert(item_ids[3]), Ok(true));
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                stock_level_count_low_stock(&mut *thread, &*stock, warehouse_id, &malformed, 20,),
                Err(Status::Retry)
            );
            assert_eq!(stock_level_cache_partition(), (1, 1));
            let diagnostic = LAST_ERROR.with(|slot| {
                let error = slot.borrow();
                String::from_utf8(error.as_bytes().to_vec()).unwrap()
            });
            assert_eq!(
                diagnostic,
                "StockLevel stock batch: row 1 has a truncated quantity"
            );
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());

            // Misses still run before hits. Tombstone the cached first row,
            // force the malformed second row through the miss batch, and
            // verify reduction restores the original batch's row-0 error
            // precedence after both records entered the OCC read set.
            let mut first_key = [0_u8; 8];
            first_key[..4].copy_from_slice(&warehouse_id.to_be_bytes());
            first_key[4..].copy_from_slice(&item_ids[0].to_be_bytes());
            assert!((*thread)
                .resolved_cache
                .matching(stock_table, &first_key)
                .is_some());
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                sto_tpcc_remove(thread, stock, first_key.as_ptr(), first_key.len()),
                Status::Ok.code()
            );
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());
            (&mut (*thread).resolved_cache.entries)[fourth_slot] = ResolvedCacheEntry::default();
            if (*thread).resolved_cache.last_slot == Some(fourth_slot) {
                (*thread).resolved_cache.last_slot = None;
            }
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                stock_level_count_low_stock(&mut *thread, &*stock, warehouse_id, &malformed, 20,),
                Err(Status::Retry)
            );
            assert_eq!(stock_level_cache_partition(), (1, 1));
            let diagnostic = LAST_ERROR.with(|slot| {
                let error = slot.borrow();
                String::from_utf8(error.as_bytes().to_vec()).unwrap()
            });
            assert_eq!(
                diagnostic,
                "StockLevel stock batch: required row 0 is missing"
            );
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());

            assert_eq!(sto_tpcc_thread_destroy(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(stock), Status::Ok.code());
            assert_eq!(sto_tpcc_db_destroy(db), Status::Ok.code());
        }
    }

    #[test]
    fn tpcc_table_configuration_always_selects_unique_lock_requests() {
        let defaults = table_config(StoTpccTableConfig::default());
        assert!(defaults.unique_lock_requests());
        assert!(!defaults.trusted_scan_value_generation());
        assert!(!defaults.bounded_atomic_values());

        let configured = table_config(StoTpccTableConfig {
            max_retained_records: 17,
            max_retained_key_bytes: 29,
            max_consumed_record_ids: 41,
            trusted_scan_value_generation: 1,
            bounded_atomic_values: 7,
            ..StoTpccTableConfig::default()
        });
        assert!(configured.unique_lock_requests());
        assert_eq!(configured.max_retained_records(), 17);
        assert_eq!(configured.max_retained_key_bytes(), 29);
        assert_eq!(configured.max_consumed_record_ids(), 41);
        assert!(configured.trusted_scan_value_generation());
        assert!(configured.bounded_atomic_values());
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn tpcc_table_config_c_layout_appends_the_bounded_value_flag() {
        assert_eq!(
            mem::offset_of!(StoTpccTableConfig, trusted_scan_value_generation),
            64
        );
        assert_eq!(
            mem::offset_of!(StoTpccTableConfig, bounded_atomic_values),
            68
        );
        assert_eq!(mem::size_of::<StoTpccTableConfig>(), 72);
        assert_eq!(mem::align_of::<StoTpccTableConfig>(), 8);
    }

    #[test]
    fn last_error_is_bounded_and_utf8() {
        clear_last_error();
        set_last_error(format_args!("{}", "é".repeat(ERROR_CAPACITY)));
        LAST_ERROR.with(|slot| {
            let error = slot.borrow();
            assert!(error.len <= ERROR_CAPACITY);
            assert!(std::str::from_utf8(error.as_bytes()).is_ok());
        });
    }

    #[test]
    fn thread_affinity_cookie_is_stable_and_distinct() {
        let owner = allocate_current_thread_cookie().unwrap();
        assert_eq!(owner, current_thread_cookie());
        let other = thread::spawn(|| allocate_current_thread_cookie().unwrap())
            .join()
            .unwrap();
        assert_ne!(owner, other);
    }

    #[cfg(mtree_native_integration)]
    #[test]
    fn last_only_policy_isolates_its_fixed_lane_from_full_cache() {
        unsafe {
            let config = StoTpccDbConfig {
                max_threads: NATIVE_TEST_MAX_THREADS,
                max_key_length: NATIVE_TEST_MAX_KEY_LENGTH,
                max_items_per_txn: 128,
                max_locks_per_txn: 256,
            };
            let mut db = ptr::null_mut();
            assert_eq!(sto_tpcc_db_create(&config, &mut db), Status::Ok.code());

            let mut full = ptr::null_mut();
            let mut last_only = ptr::null_mut();
            assert_eq!(
                sto_tpcc_table_create(db, ptr::null(), &mut full),
                Status::Ok.code()
            );
            assert_eq!(
                sto_tpcc_table_create_with_cache_policy(
                    db,
                    ptr::null(),
                    STO_TPCC_RESOLVED_CACHE_LAST_ONLY,
                    &mut last_only,
                ),
                Status::Ok.code()
            );
            assert_eq!((*full).cache_policy, ResolvedCachePolicy::Full);
            assert_eq!((*last_only).cache_policy, ResolvedCachePolicy::LastOnly);

            let mut invalid = full;
            assert_eq!(
                sto_tpcc_table_create_with_cache_policy(db, ptr::null(), 99, &mut invalid),
                Status::Fatal.code()
            );
            assert!(invalid.is_null());

            let full_handle = &*full;
            let last_only_handle = &*last_only;
            let full_table = &full_handle.state.table;
            let last_only_table = &last_only_handle.state.table;
            let full_key = &b"full-hot"[..];
            let full_slot = ResolvedCache::slot(full_table, full_key);
            let collision_key = (0_u64..)
                .map(u64::to_ne_bytes)
                .find(|key| ResolvedCache::slot(last_only_table, key) == full_slot)
                .expect("a LastOnly key must collide with the selected Full slot");
            let full_scan_key = (0_u64..)
                .map(|value| value.wrapping_add(1 << 32).to_ne_bytes())
                .find(|key| ResolvedCache::slot(full_table, key) != full_slot)
                .expect("a distinct Full scan slot must exist");
            let last_scan_key = &b"last-scan"[..];

            let mut thread = ptr::null_mut();
            assert_eq!(sto_tpcc_thread_create(db, &mut thread), Status::Ok.code());
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            for (table, key, value) in [
                (full, full_key, &b"full"[..]),
                (full, &full_scan_key[..], &b"scan-full"[..]),
                (last_only, &collision_key[..], &b"last"[..]),
                (last_only, last_scan_key, &b"scan-last"[..]),
            ] {
                assert_eq!(
                    sto_tpcc_insert(
                        thread,
                        table,
                        key.as_ptr(),
                        key.len(),
                        value.as_ptr(),
                        value.len(),
                    ),
                    Status::Ok.code()
                );
            }
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                get_with_capacity(thread, full, full_key, 64),
                (Status::Ok.code(), b"full".len())
            );
            let full_entry = {
                let cache = &(*thread).resolved_cache;
                assert_eq!(cache.last_slot, Some(full_slot));
                assert_eq!(
                    cache.entries[LAST_ONLY_CACHE_SLOT],
                    ResolvedCacheEntry::default()
                );
                cache.entries[full_slot]
            };

            // A BufferTooSmall result still resolves and remembers the exact
            // LastOnly row, but the colliding Full slot and its last pointer
            // remain untouched.
            assert_eq!(
                get_with_capacity(thread, last_only, &collision_key, 0),
                (Status::BufferTooSmall.code(), b"last".len())
            );
            {
                let cache = &(*thread).resolved_cache;
                assert_eq!(cache.last_slot, Some(full_slot));
                assert_eq!(cache.entries[full_slot], full_entry);
                assert!(ResolvedCache::entry_matches(
                    &cache.entries[LAST_ONLY_CACHE_SLOT],
                    last_only_table,
                    &collision_key,
                )
                .is_some());
            }
            assert_eq!(
                sto_tpcc_put(
                    thread,
                    last_only,
                    collision_key.as_ptr(),
                    collision_key.len(),
                    b"changed".as_ptr(),
                    b"changed".len(),
                ),
                Status::Ok.code()
            );

            // Logical misses also carry stable resolved tokens. Replacing the
            // LastOnly lane must not evict the colliding Full entry.
            assert_eq!(
                get_with_capacity(thread, last_only, b"missing", 0),
                (Status::Miss.code(), 0)
            );
            let last_only_miss = {
                let cache = &(*thread).resolved_cache;
                assert_eq!(cache.last_slot, Some(full_slot));
                assert_eq!(cache.entries[full_slot], full_entry);
                assert!(ResolvedCache::entry_matches(
                    &cache.entries[LAST_ONLY_CACHE_SLOT],
                    last_only_table,
                    b"missing",
                )
                .is_some());
                cache.entries[LAST_ONLY_CACHE_SLOT]
            };

            // Even a small LastOnly scan leaves both cache lanes byte-for-byte
            // unchanged.
            let mut delivered = Vec::<Vec<u8>>::new();
            let mut visited = usize::MAX;
            assert_eq!(
                sto_tpcc_scan(
                    thread,
                    last_only,
                    0,
                    1,
                    last_scan_key.as_ptr(),
                    last_scan_key.len(),
                    0,
                    ptr::null(),
                    0,
                    1,
                    Some(stop_after_first_scan_row),
                    (&mut delivered as *mut Vec<Vec<u8>>).cast(),
                    &mut visited,
                ),
                Status::Ok.code()
            );
            assert_eq!(visited, 1);
            assert_eq!(delivered, [last_scan_key.to_vec()]);
            {
                let cache = &(*thread).resolved_cache;
                assert_eq!(cache.last_slot, Some(full_slot));
                assert_eq!(cache.entries[full_slot], full_entry);
                assert_eq!(cache.entries[LAST_ONLY_CACHE_SLOT], last_only_miss);
            }

            // Full retains the original Delivery-style small-scan behavior:
            // the callback-visible row enters its hashed slot and becomes the
            // Full last slot.
            let full_scan_slot = ResolvedCache::slot(full_table, &full_scan_key);
            {
                let cache = &mut (*thread).resolved_cache;
                cache.entries[full_scan_slot] = ResolvedCacheEntry::default();
                cache.last_slot = Some(full_slot);
            }
            delivered.clear();
            visited = usize::MAX;
            assert_eq!(
                sto_tpcc_scan(
                    thread,
                    full,
                    0,
                    1,
                    full_scan_key.as_ptr(),
                    full_scan_key.len(),
                    1,
                    full_scan_key.as_ptr(),
                    full_scan_key.len(),
                    1,
                    Some(stop_after_first_scan_row),
                    (&mut delivered as *mut Vec<Vec<u8>>).cast(),
                    &mut visited,
                ),
                Status::Ok.code()
            );
            assert_eq!(visited, 1);
            assert_eq!(delivered, [full_scan_key.to_vec()]);
            let full_scan_entry = {
                let cache = &(*thread).resolved_cache;
                assert_eq!(cache.last_slot, Some(full_scan_slot));
                assert!(ResolvedCache::entry_matches(
                    &cache.entries[full_scan_slot],
                    full_table,
                    &full_scan_key,
                )
                .is_some());
                cache.entries[full_scan_slot]
            };

            // Further LastOnly point traffic cannot promote, overwrite, or
            // redirect the Full last slot. An oversized key safely clears only
            // the LastOnly lane.
            assert_eq!(
                get_with_capacity(thread, last_only, &collision_key, 64),
                (Status::Ok.code(), b"changed".len())
            );
            {
                let cache = &(*thread).resolved_cache;
                assert_eq!(cache.last_slot, Some(full_scan_slot));
                assert_eq!(cache.entries[full_scan_slot], full_scan_entry);
            }
            let oversized_key = [b'x'; RESOLVED_CACHE_KEY_BYTES + 1];
            assert_eq!(
                get_with_capacity(thread, last_only, &oversized_key, 0),
                (Status::Miss.code(), 0)
            );
            {
                let cache = &(*thread).resolved_cache;
                assert_eq!(cache.last_slot, Some(full_scan_slot));
                assert_eq!(cache.entries[full_scan_slot], full_scan_entry);
                assert!(cache.entries[LAST_ONLY_CACHE_SLOT].record.is_none());
            }
            assert_eq!(sto_tpcc_txn_abort(thread), Status::Ok.code());

            assert_eq!(sto_tpcc_thread_destroy(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(last_only), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(full), Status::Ok.code());
            assert_eq!(sto_tpcc_db_destroy(db), Status::Ok.code());
        }
    }

    #[cfg(mtree_native_integration)]
    #[test]
    fn resolved_cache_reuses_point_misses_and_only_small_scan_rows() {
        unsafe {
            let config = StoTpccDbConfig {
                max_threads: NATIVE_TEST_MAX_THREADS,
                max_key_length: NATIVE_TEST_MAX_KEY_LENGTH,
                max_items_per_txn: 64,
                max_locks_per_txn: 128,
            };
            let mut db = ptr::null_mut();
            assert_eq!(sto_tpcc_db_create(&config, &mut db), Status::Ok.code());

            let mut first = ptr::null_mut();
            let mut second = ptr::null_mut();
            assert_eq!(
                sto_tpcc_table_create(db, ptr::null(), &mut first),
                Status::Ok.code()
            );
            assert_eq!(
                sto_tpcc_table_create(db, ptr::null(), &mut second),
                Status::Ok.code()
            );
            let first_handle = &*first;
            let first_table = &first_handle.state.table;
            let hot_key = &b"full/hot"[..];
            let hot_value = &b"caller-buffer-value"[..];
            let hot_slot = ResolvedCache::slot(first_table, hot_key);
            let other_key = (0_u64..)
                .map(|value| {
                    let mut key = value.to_ne_bytes();
                    key[0] = b'z';
                    key
                })
                .find(|key| {
                    key.as_slice() != hot_key && ResolvedCache::slot(first_table, key) != hot_slot
                })
                .expect("a distinct Full cache slot must exist");
            let collision_a = &b"collision/a"[..];
            let collision_slot = ResolvedCache::slot(first_table, collision_a);
            let collision_b = (0_u64..)
                .map(|value| {
                    let mut key = value.to_ne_bytes();
                    key[0] = b'z';
                    key
                })
                .find(|key| {
                    *key != other_key && ResolvedCache::slot(first_table, key) == collision_slot
                })
                .expect("a colliding Full cache key must exist");
            let foreign_key = &b"foreign/key"[..];
            let mut thread = ptr::null_mut();
            assert_eq!(sto_tpcc_thread_create(db, &mut thread), Status::Ok.code());

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            for (table, key, value) in [
                (first, &b"a"[..], &b"first-a"[..]),
                (first, &b"b"[..], &b"first-b"[..]),
                (second, &b"a"[..], &b"second-a"[..]),
                (first, hot_key, hot_value),
                (first, &other_key[..], &b"other-value"[..]),
                (first, collision_a, &b"collision-a-value"[..]),
                (first, &collision_b[..], &b"collision-b-value"[..]),
                (first, foreign_key, &b"first-table-value"[..]),
                (second, foreign_key, &b"second-table-value"[..]),
            ] {
                assert_eq!(
                    sto_tpcc_insert(
                        thread,
                        table,
                        key.as_ptr(),
                        key.len(),
                        value.as_ptr(),
                        value.len(),
                    ),
                    Status::Ok.code()
                );
            }
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // Full probes test the last entry before hashing. Moving to a
            // different slot and back exercises the ordinary hashed hit. An
            // undersized hit reports its exact size without changing output.
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            assert_eq!(
                get_with_sentinel(thread, first, hot_key, hot_value.len(), 0xa1),
                (Status::Ok.code(), hot_value.len(), hot_value.to_vec())
            );
            reset_cache_slot_calls();
            assert_eq!(
                get_with_sentinel(thread, first, hot_key, hot_value.len(), 0xa2),
                (Status::Ok.code(), hot_value.len(), hot_value.to_vec())
            );
            assert_eq!(cache_slot_calls(), 0, "the last hit must not hash");
            assert_eq!(
                get_with_sentinel(thread, first, &other_key, 32, 0xa3).0,
                Status::Ok.code()
            );
            assert_ne!((*thread).resolved_cache.last_slot, Some(hot_slot));
            reset_cache_slot_calls();
            let undersized = hot_value.len() - 1;
            assert_eq!(
                get_with_sentinel(thread, first, hot_key, undersized, 0x9d),
                (
                    Status::BufferTooSmall.code(),
                    hot_value.len(),
                    vec![0x9d; undersized],
                )
            );
            assert_eq!(cache_slot_calls(), 1, "the hashed hit must hash once");
            assert_eq!((*thread).resolved_cache.last_slot, Some(hot_slot));

            // A direct-mapped collision is stale for the evicted key. A token
            // for the same key from another table is foreign. Both must fall
            // back to key resolution without dooming the active transaction.
            let (status, actual, output) = get_with_sentinel(thread, first, collision_a, 32, 0x71);
            assert_eq!(status, Status::Ok.code());
            assert_eq!(actual, b"collision-a-value".len());
            assert_eq!(&output[..actual], b"collision-a-value");
            assert_eq!(
                get_with_sentinel(thread, first, &collision_b, 32, 0x72).2
                    [..b"collision-b-value".len()],
                b"collision-b-value"[..]
            );
            let (status, actual, output) = get_with_sentinel(thread, first, collision_a, 32, 0x73);
            assert_eq!(status, Status::Ok.code());
            assert_eq!(actual, b"collision-a-value".len());
            assert_eq!(&output[..actual], b"collision-a-value");
            assert_eq!(
                get_with_sentinel(thread, first, foreign_key, 32, 0x74).2
                    [..b"first-table-value".len()],
                b"first-table-value"[..]
            );
            let (status, actual, output) = get_with_sentinel(thread, second, foreign_key, 32, 0x75);
            assert_eq!(status, Status::Ok.code());
            assert_eq!(actual, b"second-table-value".len());
            assert_eq!(&output[..actual], b"second-table-value");
            assert!(!(*thread)
                .active
                .as_ref()
                .expect("transaction remains active")
                .transaction
                .is_doomed());
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            // A point miss still resolves a stable tombstone token. The
            // follow-up cache lookup must find it without another directory
            // traversal, and normal miss semantics remain unchanged.
            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            let mut actual = usize::MAX;
            assert_eq!(
                sto_tpcc_get(
                    thread,
                    first,
                    b"missing".as_ptr(),
                    b"missing".len(),
                    ptr::null_mut(),
                    0,
                    &mut actual,
                ),
                Status::Miss.code()
            );
            assert_eq!(actual, 0);
            let first_handle = &*first;
            assert!((*thread)
                .resolved_cache
                .matching(&first_handle.state.table, b"missing")
                .is_some());
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());

            assert_eq!(sto_tpcc_txn_begin(thread), Status::Ok.code());
            let mut delivered = Vec::<Vec<u8>>::new();
            let mut visited = usize::MAX;
            assert_eq!(
                sto_tpcc_scan(
                    thread,
                    first,
                    0,
                    0,
                    ptr::null(),
                    0,
                    0,
                    ptr::null(),
                    0,
                    2,
                    Some(stop_after_first_scan_row),
                    (&mut delivered as *mut Vec<Vec<u8>>).cast(),
                    &mut visited,
                ),
                Status::Ok.code()
            );
            assert_eq!(visited, 1);
            assert_eq!(delivered, [b"a".to_vec()]);

            {
                let handle = &mut *thread;
                let first_handle = &*first;
                let second_handle = &*second;
                let first_table = &first_handle.state.table;
                let second_table = &second_handle.state.table;
                assert!(handle.resolved_cache.matching(first_table, b"a").is_some());
                assert!(handle.resolved_cache.matching(first_table, b"b").is_none());
                assert!(handle
                    .resolved_cache
                    .matching(first_table, b"a\0")
                    .is_none());
                assert!(handle.resolved_cache.matching(second_table, b"a").is_none());
            }

            // Large StockLevel-style scans must not evict useful point-cache
            // entries, even when their callback stops after the first row.
            delivered.clear();
            visited = usize::MAX;
            assert_eq!(
                sto_tpcc_scan(
                    thread,
                    second,
                    0,
                    0,
                    ptr::null(),
                    0,
                    0,
                    ptr::null(),
                    0,
                    RESOLVED_SCAN_CACHE_LIMIT + 1,
                    Some(stop_after_first_scan_row),
                    (&mut delivered as *mut Vec<Vec<u8>>).cast(),
                    &mut visited,
                ),
                Status::Ok.code()
            );
            assert_eq!(visited, 1);
            assert_eq!(delivered, [b"a".to_vec()]);
            let second_handle = &*second;
            assert!((*thread)
                .resolved_cache
                .matching(&second_handle.state.table, b"a")
                .is_none());

            // The stopping row remains reusable by remove; the stable token
            // survives logical deletion and retains normal size accounting.
            assert_eq!(
                sto_tpcc_remove(thread, first, b"a".as_ptr(), 1),
                Status::Ok.code()
            );
            assert_eq!(sto_tpcc_txn_commit(thread), Status::Ok.code());
            let mut first_rows = u64::MAX;
            assert_eq!(
                sto_tpcc_table_size(first, &mut first_rows),
                Status::Ok.code()
            );
            assert_eq!(first_rows, 6);

            // Error paths after validating the output pointer retain the ABI
            // guarantee that no rows were delivered.
            visited = usize::MAX;
            assert_eq!(
                sto_tpcc_scan(
                    thread,
                    first,
                    0,
                    0,
                    ptr::null(),
                    0,
                    0,
                    ptr::null(),
                    0,
                    1,
                    Some(stop_after_first_scan_row),
                    (&mut delivered as *mut Vec<Vec<u8>>).cast(),
                    &mut visited,
                ),
                Status::Fatal.code()
            );
            assert_eq!(visited, 0);

            assert_eq!(sto_tpcc_thread_destroy(thread), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(second), Status::Ok.code());
            assert_eq!(sto_tpcc_table_destroy(first), Status::Ok.code());
            assert_eq!(sto_tpcc_db_destroy(db), Status::Ok.code());
        }
    }
}
