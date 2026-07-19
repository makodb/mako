#pragma once

// Deliberately a MICRO-header: the migration data plane's RPC service must
// catch this type, but pulling the full storage seam header (or abstract_db.h)
// into that service header trips a known clang-22 silent frontend hang in
// gtest TUs that also import the cluster module. Keep this file free of any
// includes.

namespace mako {
// Thrown by the migration data plane's chunked scan when its leaked-lock
// deadline expires: a row lock whose releasing 2PC message was lost pins the
// scanned window forever (observed live: one stock row locked 24+ minutes).
// The thrower is ordered_index_shard_data.h; catchers convert it to a clean
// per-chunk failure (the DoScanRange wedged sentinel, a failed pull, or a
// migration abort) so no service thread is ever captured.
struct oi_scan_wedged {};
}  // namespace mako
