#pragma once
#include "storage/shard_scan_page.hh"
#include "lib/shardClient.h"
#include "runtime/worker_context.hh"
#include <exception>
#include <string>

namespace mako {
inline MakoShardBytes scan_bytes(const std::string& value) {
  return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}
struct FullScanState {
  MakoFullScan* scan = nullptr;
  ~FullScanState() { mako_full_scan_free(scan); }
  FullScanState() = default;
  FullScanState(const FullScanState&) = delete;
  FullScanState& operator=(const FullScanState&) = delete;
};
template<typename Index, typename Delivery>
struct ShardedScanDispatch {
  Index* table;
  const std::string& lo;
  const std::string* hi;
  bool reverse;
  bool fixed;
  Delivery& delivery;
  std::exception_ptr exception;

  // @unsafe - one immutable native snapshot calls this bridge for each segment.
  static uint32_t segment(void* opaque, MakoShardBytes segment_lo,
                          uint32_t has_hi, MakoShardBytes segment_hi,
                          MakoShardRoute route) {
    auto& self = *static_cast<ShardedScanDispatch*>(opaque);
    try {
      const auto low = self.fixed ? scan_bytes(self.lo) : segment_lo;
      const auto high = self.fixed
          ? (self.hi ? scan_bytes(*self.hi) : MakoShardBytes{}) : segment_hi;
      const uint32_t bounded = self.fixed ? self.hi != nullptr : has_hi;
      auto request = mako::sharding_request_with_route(
          self.table->get_table_id(), std::string(), route);
      // Disabled routing still needs the physical destination on the wire.
      request.grant = route.grant;
      request.snapshot_version = route.snapshot_version;
      if (route.grant.owner == uint32_t(TThread::get_shard_index())) {
        self.delivery.local(self.table, request, {low, high, {}, bounded, 0, self.reverse}, route);
        return self.delivery.stopped ? 1 : 0;
      }
      self.delivery.begin_remote(route.grant.owner);
      const auto identity = scan_identity(request);
      FullScanState state;
      require_scan_status(mako_full_scan_new(
          {low, high, {}, bounded, 0, self.reverse}, &identity, &state.scan));
      uint8_t input[mako::full_scan_request_capacity];
      std::string response;
      uint32_t done = 0;
      while (!done) {
        size_t length = 0;
        require_scan_status(mako_full_scan_request(state.scan, input,
                                                    sizeof(input), &length));
        if (!TThread::sclient || TThread::sclient->fullScanPage(
            self.table->get_table_id(), request, input, length, response)
                != mako::ErrorCode::SUCCESS)
          throw abstract_db::abstract_abort_exception();
        require_scan_status(mako_full_scan_consume(state.scan,
            scan_bytes(response), Delivery::row,
            &self.delivery, &done));
        if (self.delivery.exception) std::rethrow_exception(self.delivery.exception);
      }
      return self.delivery.stopped ? 1 : 0;
    } catch (...) {
      self.exception = std::current_exception();
      return 2;
    }
  }
};

// @unsafe - snapshot traversal borrows no native mutex across RPC/user callbacks.
template<typename Index, typename Delivery>
inline void sharded_full_scan(Index* table, const std::string& start,
                                 const std::string* end, bool reverse, Delivery& delivery) {
  using Dispatch = ShardedScanDispatch<Index, Delivery>;
  // Engine reverse bounds are (end,start]. The appended NULs normalize bounds
  // only; every page cursor remains the exact last binary key, exclusively.
  std::string lower = reverse ? (end ? *end : std::string()) : start;
  std::string upper = reverse ? start : (end ? *end : std::string());
  if (reverse) {
    if (end) lower.push_back('\0');
    upper.push_back('\0');
  }
  const bool has_upper = reverse || end != nullptr;
  if (has_upper && lower >= upper) return;
  // Genuinely disabled local scans need no native admission. Native-enabled
  // scans below select captured segments and admit each complete local scope.
  // Only remote segments require transport pages.
  if (!mako_sharding_enabled() && !table->get_is_remote()) {
    delivery.local(table, {}, {scan_bytes(lower), scan_bytes(upper),
                               {}, has_upper, 0, reverse}, {});
    return;
  }
  Dispatch dispatch{table, lower, has_upper ? &upper : nullptr,
                                reverse, false, delivery};
  uint32_t status = MAKO_SHARD_OK;
  if (mako::sharding_admission_enabled()) {
    const auto* binding = mako::sharding_table_binding(table->get_table_id());
    if (binding->kind == 0) {
      dispatch.fixed = binding->fixed_coordinate.is_some();
      if (dispatch.fixed) {
        const auto& coordinate = binding->fixed_coordinate.as_ref().unwrap();
        std::string coordinate_end = coordinate;
        coordinate_end.push_back('\0');
        status = mako::sharding_scan_segments(binding->table,
            scan_bytes(coordinate), 1, scan_bytes(coordinate_end),
            reverse, Dispatch::segment, &dispatch);
      } else {
        status = mako::sharding_scan_segments(binding->table,
            scan_bytes(lower), has_upper, scan_bytes(upper),
            reverse, Dispatch::segment, &dispatch);
      }
      if (dispatch.exception) std::rethrow_exception(dispatch.exception);
      require_scan_status(status);
      return;
    }
  }
  mako::ShardingRequest request{};
  const int owner = table->get_is_remote()
      ? mako::sharding_scan_request(table->get_table_id(), start, request)
      : TThread::get_shard_index();
  if (owner < 0) throw abstract_db::abstract_abort_exception();
  status = Dispatch::segment(&dispatch, scan_bytes(lower),
      has_upper, scan_bytes(upper), {{uint32_t(owner), 0}, MAKO_SHARD_NO_SNAPSHOT});
  if (dispatch.exception) std::rethrow_exception(dispatch.exception);
  if (status == 2) throw abstract_db::abstract_abort_exception();
}

} // namespace mako
