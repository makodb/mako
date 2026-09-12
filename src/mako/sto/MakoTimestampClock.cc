#include "sto/MakoTimestampClock.hh"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <mutex>
#include <time.h>

#if defined(__linux__)
#include <sched.h>
#endif

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#endif

namespace mako {
namespace {

constexpr uint64_t kNanosecondsPerMillisecond = UINT64_C(1000000);
constexpr uint64_t kCalibrationWindowNanoseconds = UINT64_C(5000000);
constexpr uint64_t kMaximumRealtimeBracketNanoseconds = UINT64_C(1000000);
constexpr uint64_t kMaximumDriftNanoseconds = UINT64_C(10000000);
constexpr uint64_t kMaximumUncheckedTscStepNanoseconds = UINT64_C(1000000000);
constexpr uint64_t kDriftCheckMask = (UINT64_C(1) << 16) - 1;
constexpr uint64_t kNoTestOverride = std::numeric_limits<uint64_t>::max();
#if defined(__i386__) || defined(__x86_64__)
constexpr unsigned int kCpuidHypervisorBit = UINT32_C(1) << 31;
constexpr unsigned int kCpuidRdtscpBit = UINT32_C(1) << 27;
constexpr unsigned int kCpuidInvariantTscBit = UINT32_C(1) << 8;
#endif

bool read_clock_nanoseconds(clockid_t clock, uint64_t& result) noexcept {
  timespec value{};
  if (clock_gettime(clock, &value) != 0 || value.tv_sec < 0 ||
      value.tv_nsec < 0 || value.tv_nsec >= 1000000000L)
    return false;
  const uint64_t seconds = static_cast<uint64_t>(value.tv_sec);
  if (seconds >
      (std::numeric_limits<uint64_t>::max() -
       static_cast<uint64_t>(value.tv_nsec)) /
          UINT64_C(1000000000))
    return false;
  result = seconds * UINT64_C(1000000000) +
      static_cast<uint64_t>(value.tv_nsec);
  return true;
}

bool read_realtime_ms(uint64_t& result) noexcept {
  uint64_t nanoseconds = 0;
  if (!read_clock_nanoseconds(CLOCK_REALTIME, nanoseconds))
    return false;
  result = nanoseconds / kNanosecondsPerMillisecond;
  return result <= kMakoTimestampPhysicalMsMax;
}

bool scale_elapsed_cycles(uint64_t elapsed_cycles,
                          uint64_t ns_per_cycle_q32,
                          uint64_t& elapsed_ns) noexcept {
  const __uint128_t scaled =
      static_cast<__uint128_t>(elapsed_cycles) * ns_per_cycle_q32;
  const __uint128_t shifted = scaled >> 32;
  if (shifted > std::numeric_limits<uint64_t>::max())
    return false;
  elapsed_ns = static_cast<uint64_t>(shifted);
  return true;
}

bool realtime_estimate_is_trusted(uint64_t estimated_ns) noexcept {
  uint64_t realtime_ns = 0;
  if (!read_clock_nanoseconds(CLOCK_REALTIME, realtime_ns))
    return false;
  const uint64_t difference = estimated_ns > realtime_ns
      ? estimated_ns - realtime_ns
      : realtime_ns - estimated_ns;
  return difference <= kMaximumDriftNanoseconds;
}

#if defined(__i386__) || defined(__x86_64__)
bool rdtscp_is_supported_and_trusted() noexcept {
  const unsigned int maximum_extended = __get_cpuid_max(0x80000000, nullptr);
  if (maximum_extended < 0x80000007)
    return false;

  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;
  if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
    return false;
  // A hypervisor may virtualize TSC correctly, but without an explicit trust
  // contract its migration and pause behavior is unknown. Use the safe clock.
  if ((ecx & kCpuidHypervisorBit) != 0)
    return false;
  if (!__get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx) ||
      (edx & kCpuidRdtscpBit) == 0)
    return false;
  if (!__get_cpuid(0x80000007, &eax, &ebx, &ecx, &edx) ||
      (edx & kCpuidInvariantTscBit) == 0)
    return false;
  return true;
}

uint64_t read_rdtscp(uint32_t& auxiliary) noexcept {
  unsigned int raw_auxiliary = 0;
  const uint64_t tsc = __rdtscp(&raw_auxiliary);
  auxiliary = raw_auxiliary;
  return tsc;
}
#endif

struct tsc_calibration {
  uint64_t base_tsc = 0;
  uint64_t base_unix_ns = 0;
  uint64_t ns_per_cycle_q32 = 0;
  uint32_t base_auxiliary = 0;
  bool enabled = false;
};

struct thread_tsc_observation {
  uint64_t tsc = 0;
  uint64_t estimated_ns = 0;
  uint64_t calls = 0;
  uint32_t auxiliary = 0;
  bool initialized = false;
};

tsc_calibration calibrate_tsc() noexcept {
  tsc_calibration result{};
#if defined(__i386__) || defined(__x86_64__)
  if (!rdtscp_is_supported_and_trusted())
    return result;

  uint64_t monotonic_start = 0;
  uint64_t monotonic_end = 0;
  if (!read_clock_nanoseconds(CLOCK_MONOTONIC_RAW, monotonic_start))
    return result;
  uint32_t auxiliary_start = 0;
  const uint64_t tsc_start = read_rdtscp(auxiliary_start);
  do {
    if (!read_clock_nanoseconds(CLOCK_MONOTONIC_RAW, monotonic_end))
      return result;
  } while (monotonic_end - monotonic_start <
           kCalibrationWindowNanoseconds);
  uint32_t auxiliary_end = 0;
  const uint64_t tsc_end = read_rdtscp(auxiliary_end);
  if (tsc_end <= tsc_start || monotonic_end <= monotonic_start ||
      auxiliary_start != auxiliary_end)
    return result;

  const uint64_t elapsed_tsc = tsc_end - tsc_start;
  const uint64_t elapsed_ns = monotonic_end - monotonic_start;
  // Reject frequencies outside 100 MHz through 10 GHz.
  if (elapsed_tsc < elapsed_ns / 10 || elapsed_tsc > elapsed_ns * 10)
    return result;
  const __uint128_t scaled_ns =
      static_cast<__uint128_t>(elapsed_ns) << 32;
  const uint64_t ns_per_cycle_q32 =
      static_cast<uint64_t>(scaled_ns / elapsed_tsc);
  if (ns_per_cycle_q32 == 0)
    return result;

  uint64_t realtime_before = 0;
  uint64_t realtime_after = 0;
  if (!read_clock_nanoseconds(CLOCK_REALTIME, realtime_before))
    return result;
  uint32_t base_auxiliary = 0;
  const uint64_t base_tsc = read_rdtscp(base_auxiliary);
  if (!read_clock_nanoseconds(CLOCK_REALTIME, realtime_after) ||
      realtime_after < realtime_before ||
      realtime_after - realtime_before > kMaximumRealtimeBracketNanoseconds)
    return result;

  result.base_tsc = base_tsc;
  result.base_unix_ns =
      realtime_before + (realtime_after - realtime_before) / 2;
  result.ns_per_cycle_q32 = ns_per_cycle_q32;
  result.base_auxiliary = base_auxiliary;
  result.enabled = true;
#endif
  return result;
}

class physical_clock_state {
 public:
  physical_clock_state() noexcept : calibration_(calibrate_tsc()) {}

  bool read(uint64_t& unix_ms) noexcept {
#if defined(MAKO_LOCAL_TEST_HOOKS)
    const uint64_t overridden =
        test_unix_ms_.load(std::memory_order_acquire);
    if (overridden != kNoTestOverride) {
      unix_ms = overridden;
      return true;
    }
#endif

#if defined(__i386__) || defined(__x86_64__)
    if (rdtscp_enabled_.load(std::memory_order_acquire) &&
        calibration_.enabled) {
      uint32_t auxiliary = 0;
      uint64_t tsc = read_rdtscp(auxiliary);
      thread_local thread_tsc_observation observation{};
      const uint32_t expected_auxiliary = observation.initialized
          ? observation.auxiliary
          : calibration_.base_auxiliary;
      const bool auxiliary_changed = auxiliary != expected_auxiliary;
      if (auxiliary_changed) {
        uint32_t retried_auxiliary = 0;
        tsc = read_rdtscp(retried_auxiliary);
        if (retried_auxiliary != auxiliary) {
          disable_rdtscp();
          return fallback(unix_ms);
        }
      }
      if (tsc < calibration_.base_tsc ||
          (observation.initialized && tsc < observation.tsc)) {
        disable_rdtscp();
        return fallback(unix_ms);
      }
      const uint64_t elapsed_tsc = tsc - calibration_.base_tsc;
      uint64_t elapsed_ns = 0;
      if (!scale_elapsed_cycles(elapsed_tsc,
                                calibration_.ns_per_cycle_q32,
                                elapsed_ns)) {
        disable_rdtscp();
        return fallback(unix_ms);
      }
      if (elapsed_ns >
          std::numeric_limits<uint64_t>::max() -
              calibration_.base_unix_ns) {
        disable_rdtscp();
        return fallback(unix_ms);
      }
      const uint64_t estimated_ns = calibration_.base_unix_ns + elapsed_ns;
      if (observation.initialized &&
          estimated_ns < observation.estimated_ns) {
        disable_rdtscp();
        return fallback(unix_ms);
      }
      // Drift sampling is worker-local. A shared fetch_add here would put a
      // second contended atomic on every timestamp allocation.
      const uint64_t call = ++observation.calls;
      const bool large_step = observation.initialized &&
          estimated_ns - observation.estimated_ns >
              kMaximumUncheckedTscStepNanoseconds;
      if (!observation.initialized || auxiliary_changed || large_step ||
          (call & kDriftCheckMask) == 0) {
        if (!realtime_estimate_is_trusted(estimated_ns)) {
          disable_rdtscp();
          return fallback(unix_ms);
        }
      }
      observation.tsc = tsc;
      observation.estimated_ns = estimated_ns;
      observation.auxiliary = auxiliary;
      observation.initialized = true;
      unix_ms = estimated_ns / kNanosecondsPerMillisecond;
      if (unix_ms <= kMakoTimestampPhysicalMsMax)
        return true;
      return false;
    }
#endif
    return fallback(unix_ms);
  }

  bool using_rdtscp() const noexcept {
    return calibration_.enabled &&
        rdtscp_enabled_.load(std::memory_order_acquire);
  }

  uint64_t fallback_count() const noexcept {
    return fallback_count_.load(std::memory_order_relaxed);
  }

#if defined(MAKO_LOCAL_TEST_HOOKS)
  bool set_test_unix_ms(uint64_t unix_ms) noexcept {
    if (unix_ms > kMakoTimestampPhysicalMsMax)
      return false;
    test_unix_ms_.store(unix_ms, std::memory_order_release);
    return true;
  }

  void clear_test_unix_ms() noexcept {
    test_unix_ms_.store(kNoTestOverride, std::memory_order_release);
  }
#endif

 private:
  void disable_rdtscp() noexcept {
    bool expected = true;
    if (rdtscp_enabled_.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel,
            std::memory_order_acquire))
      fallback_count_.fetch_add(1, std::memory_order_relaxed);
  }

  bool fallback(uint64_t& unix_ms) noexcept {
    return read_realtime_ms(unix_ms);
  }

  const tsc_calibration calibration_;
  std::atomic<bool> rdtscp_enabled_{calibration_.enabled};
  std::atomic<uint64_t> fallback_count_{
      calibration_.enabled ? UINT64_C(0) : UINT64_C(1)};
#if defined(MAKO_LOCAL_TEST_HOOKS)
  std::atomic<uint64_t> test_unix_ms_{kNoTestOverride};
#endif
};

physical_clock_state& clock_state() noexcept {
  static physical_clock_state state;
  return state;
}

}  // namespace

void MakoTimestampPhysicalClock::initialize() noexcept {
  (void)clock_state();
}

bool MakoTimestampPhysicalClock::read_unix_ms(uint64_t& unix_ms) noexcept {
  return clock_state().read(unix_ms);
}

bool MakoTimestampPhysicalClock::using_rdtscp() noexcept {
  return clock_state().using_rdtscp();
}

uint64_t MakoTimestampPhysicalClock::fallback_count() noexcept {
  return clock_state().fallback_count();
}

#if defined(MAKO_LOCAL_TEST_HOOKS)
bool MakoTimestampPhysicalClock::set_test_unix_ms(uint64_t unix_ms) noexcept {
  return clock_state().set_test_unix_ms(unix_ms);
}

void MakoTimestampPhysicalClock::clear_test_unix_ms() noexcept {
  clock_state().clear_test_unix_ms();
}

bool MakoTimestampPhysicalClock::test_scale_elapsed_cycles(
    uint64_t elapsed_cycles, uint64_t ns_per_cycle_q32,
    uint64_t& elapsed_ns) noexcept {
  return scale_elapsed_cycles(elapsed_cycles, ns_per_cycle_q32, elapsed_ns);
}
#endif

}  // namespace mako
