#pragma once

#include "storage/mako_timestamp.h"

#include <cstdint>
#include <limits>

namespace mako {

using MakoTimestampStamp = uint64_t;

inline constexpr uint32_t kMakoTimestampOrigin = 1;
inline constexpr uint32_t kMakoTimestampLogicalBits = 19;
inline constexpr uint64_t kMakoTimestampLogicalMask =
    (UINT64_C(1) << kMakoTimestampLogicalBits) - 1;
inline constexpr uint32_t kMakoTimestampPhysicalMsBits = 44;
inline constexpr uint64_t kMakoTimestampPhysicalMsMax =
    (UINT64_C(1) << kMakoTimestampPhysicalMsBits) - 1;
inline constexpr MakoTimestampStamp kMakoTimestampStampMax =
    std::numeric_limits<uint64_t>::max() >> 1;

static_assert(kMakoTimestampLogicalBits + kMakoTimestampPhysicalMsBits == 63);
static_assert(kMakoTimestampLogicalMask == MAKO_TIMESTAMP_V1_LOGICAL_MAX);

constexpr bool valid_mako_timestamp_stamp(MakoTimestampStamp stamp) noexcept {
  return stamp != 0 && stamp <= kMakoTimestampStampMax;
}

constexpr uint64_t mako_timestamp_stamp_physical_ms(
    MakoTimestampStamp stamp) noexcept {
  return stamp >> kMakoTimestampLogicalBits;
}

constexpr uint32_t mako_timestamp_stamp_logical(
    MakoTimestampStamp stamp) noexcept {
  return static_cast<uint32_t>(stamp & kMakoTimestampLogicalMask);
}

constexpr mako_timestamp_v1 expand_mako_timestamp_stamp(
    MakoTimestampStamp stamp) noexcept {
  return mako_timestamp_v1{
      mako_timestamp_stamp_physical_ms(stamp) *
          MAKO_TIMESTAMP_V1_PHYSICAL_UNIT_US,
      mako_timestamp_stamp_logical(stamp), kMakoTimestampOrigin};
}

constexpr bool valid_mako_timestamp_v1(
    const mako_timestamp_v1& timestamp) noexcept {
  return timestamp.origin != 0;
}

constexpr int compare_mako_timestamps(const mako_timestamp_v1& left,
                                      const mako_timestamp_v1& right) noexcept {
  if (left.physical_us != right.physical_us)
    return left.physical_us < right.physical_us ? -1 : 1;
  if (left.logical != right.logical)
    return left.logical < right.logical ? -1 : 1;
  if (left.origin != right.origin)
    return left.origin < right.origin ? -1 : 1;
  return 0;
}

/* Fast approximate Unix-millisecond input for the HLC. RDTSCP is used only
 * after runtime capability and calibration checks. Any unsupported or suspect
 * platform falls back to CLOCK_REALTIME. HLC monotonicity never depends on
 * either source being monotonic. */
class MakoTimestampPhysicalClock {
 public:
  static void initialize() noexcept;
  static bool read_unix_ms(uint64_t& unix_ms) noexcept;
  static bool using_rdtscp() noexcept;
  static uint64_t fallback_count() noexcept;

#if defined(MAKO_LOCAL_TEST_HOOKS)
  static bool set_test_unix_ms(uint64_t unix_ms) noexcept;
  static void clear_test_unix_ms() noexcept;
  static bool test_scale_elapsed_cycles(
      uint64_t elapsed_cycles, uint64_t ns_per_cycle_q32,
      uint64_t& elapsed_ns) noexcept;
#endif
};

}  // namespace mako
