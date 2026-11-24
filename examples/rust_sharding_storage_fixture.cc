// Rust-engine sharding storage fixture: one raw range migrates between two
// colocated owners of one process whose database is the Rust transaction
// engine, through the real sharding crate and the real srpc admin path.
//
//   rust_sharding_storage_fixture CONFIG SCENARIO
//
// One process runs one scenario and exits; examples/rust_sharding_storage.py
// starts it and compares the lines below with what the scenario must report.
//
// Standard output, one line each:
//   CHECK <name> ok
//   CHECK <name> FAILED <detail>
//   CHECK <name> SKIPPED
//   RUST_SHARD_STORAGE PASS <scenario>
//   RUST_SHARD_STORAGE FAIL <scenario>
// Standard error carries `rust_sharding_fixture: ...` lines: one per stage
// reached, the poll result, and per owner the calls the crate made through
// the callback table (`calls`, `epochs`, `opens`, and after an orderly stop
// `threads`).
//
// Exit code 0 follows PASS, which is printed only after both nodes have
// stopped, the hosts are destroyed, this thread has left the engine and the
// database is gone. Exit code 1 follows FAIL. Exit code 2 is a set-up error
// (bad arguments, a requirement that did not hold) and is never an expected
// result. FAIL and a set-up error leave through _Exit: after a node has
// started, the crate's threads call the callback table until the node is
// stopped, so nothing may unwind out of main() and destroy the hosts or the
// database under them.
//
// The crate never receives a callback table directly. It receives a copy in
// which the two thread slots, `open`, the three storage verbs and the two
// epoch slots are fixture functions that count the call and forward it
// unchanged. A
// scenario's checks read those counts, and read the rows of both owners with
// the adapter's own one-row verbs on the main thread once the migration has
// ended. While a migration is in flight the main thread only polls: a first
// access to a key a table has never held would make a storage lookahead of
// the same table answer RETRY.
//
// Scenarios:
//   migrate       ["", "n") moves from owner 0 to owner 1 on the Rust storage
//                 callbacks: 605 rows, two capture pages, one row replaced and
//                 one removed on the destination. The first put takes 1.5s,
//                 exceeding the generic RPC wait while retaining its effect.
//   pinned        An ordinary writer commits after freeze. The final copy waits
//                 for it, and a read at the new owner sees its committed value.
//   deleted-keys  ["c", "n") moves; on both owners 5,000 deleted keys lie in
//                 the range below the first live row.
//   aborted       As pinned, but the migration is aborted while it waits. The
//                 expected outcome is "aborted": the source keeps every row
//                 and the destination's stage range is emptied.
//   red-baseline  The C++ storage callback table, unchanged, on a Rust-engine
//                 database, with both table handles registered directly. This
//                 is the defect the Rust storage callbacks remove, and its
//                 expected result is the failure.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <exception>
#include <stdexcept>
#include <string>
#include "native_sharding_host.h"
#include "benchmarks/benchmark_config.h"
#include "lib/common.h"
#include "lib/configuration.h"
#include "silo_runtime.h"
#include "storage/mbta_wrapper.hh"
#include "storage/rust_sto_tpcc_wrapper.hh"
#include "storage/rust_sto_sharding_host.hh"
#include "rust_sharding_receiver_fixture.hh"
#include <rusty/cell.hpp>
#include <rusty/sync/atomic.hpp>
#include <rusty/thread.hpp>

namespace {

constexpr uint64_t kRawTable = 1001;
constexpr uint64_t kUnknownTable = 4242;
constexpr uint32_t kOwners = 2;
constexpr uint32_t kNoStatus = UINT32_MAX;
constexpr unsigned kPollMicros = 100000;
constexpr unsigned kPollsPerSecond = 1000000 / kPollMicros;
constexpr unsigned kTombstones = 5000;
constexpr uint64_t kDrainAnswersWanted = 5;
const char kRawName[] = "rs_raw";
const MakoShardTxn kNonce{0x5253545348415244ULL, 1};

enum class Scenario { red_baseline, migrate, pinned, deleted_keys, aborted, warehouses };

// What a scenario does differently from `migrate`.
struct Shape {
    // false keeps the C++ callback table: the defect of red-baseline.
    bool rust_callbacks;
    // 5,000 deleted keys below the first live row of the range, on both owners.
    bool tombstones;
    // An admitted writer keeps its epoch through the drain loop.
    bool pin;
    // The migration is aborted while the pin holds it; "aborted" is the success.
    bool abort;
    // One adapter scan of each whole table. Each walked directory entry is a
    // transaction item, so only a scenario without the tombstones can afford it.
    bool ordered_scan;
    std::string lo;
    std::string hi;
    // The longest wait for a terminal outcome.
    unsigned wait_seconds;
};

struct Row {
    std::string key;
    std::string value;
};

// Forwarded storage calls of one verb on one owner.
struct VerbCount {
    rusty::sync::atomic::AtomicU64 calls{0};
    rusty::sync::atomic::AtomicU64 ok{0};
    rusty::sync::atomic::AtomicU32 last{kNoStatus};
};
// Everything the crate did through one owner's callback table.
struct OwnerCounts {
    VerbCount scan_next;
    VerbCount put;
    VerbCount remove;
    rusty::sync::atomic::AtomicU64 enter{0};
    rusty::sync::atomic::AtomicU64 enter_ok{0};
    rusty::sync::atomic::AtomicU64 leave{0};
    // Returns of thread_leave after which the thread still held an engine handle.
    rusty::sync::atomic::AtomicU64 left_attached{0};
    rusty::sync::atomic::AtomicU64 opens{0};
    // The STO thread id of the owner's storage thread, -1 before it entered.
    rusty::sync::atomic::AtomicI32 slot{-1};
    rusty::sync::atomic::AtomicU64 cuts{0};
    rusty::sync::atomic::AtomicU64 last_cut{0};
    rusty::sync::atomic::AtomicU64 drained_busy{0};
    rusty::sync::atomic::AtomicU64 drained_ok{0};
    rusty::sync::atomic::AtomicU64 drained_other{0};
};

constexpr uint32_t kPinStarting = 0;
constexpr uint32_t kPinHeld = 1;
constexpr uint32_t kPinRelease = 2;
constexpr uint32_t kPinReleased = 3;
// The thread that holds an STO epoch for `pinned` and `aborted`.
struct Pin {
    rusty::sync::atomic::AtomicU32 state{kPinStarting};
    rusty::sync::atomic::AtomicU64 epoch{0};
    rusty::Option<rusty::thread::JoinHandle<rusty::thread::Unit>> thread;
};

// Main-thread state.
rusty::Cell<const char*> scenario_name{""};
rusty::Cell<bool> any_check_failed{false};
abstract_ordered_index* raw[kOwners]{};
mako::NativeShardingHost* host_of[kOwners]{};
Pin pin;
// The callback tables the fixture wraps; written before the nodes start.
MakoShardHost real[kOwners]{};
OwnerCounts counts[kOwners];
// Written before the storage threads start; immutable until they join.
bool delay_first_put = false;

// Checks that follow a committed migration, in reporting order.
const char* const kAfterCommitChecks[] = {
    "route-moved",
    "destination-image",
    "stale-collide-replaced",
    "stale-extra-removed",
    "destination-outside-untouched",
    "source-range-empty",
    "source-outside-untouched",
    "ordered-scan-agrees",
    "callbacks-ran",
    "no-pin-left",
};
// Checks that follow an aborted migration in scenario `aborted`.
const char* const kAfterAbortChecks[] = {
    "route-unmoved",
    "source-intact",
    "destination-stage-clean",
    "callbacks-ran",
    "no-pin-left",
};

// @safe - the scenario's differences from `migrate`.
Shape shape_of(Scenario scenario) {
    Shape shape{true, false, false, false, false, std::string(), "n", 60};
    switch (scenario) {
        case Scenario::red_baseline:
            shape.rust_callbacks = false;
            // Its expected result is "not committed".
            shape.wait_seconds = 15;
            break;
        case Scenario::warehouses:
            break;
        case Scenario::migrate:
            shape.ordered_scan = true;
            break;
        case Scenario::pinned:
            shape.pin = true;
            break;
        case Scenario::deleted_keys:
            shape.tombstones = true;
            shape.lo = "c";
            break;
        case Scenario::aborted:
            shape.pin = true;
            shape.abort = true;
            break;
    }
    return shape;
}

// @unsafe - stdio is not borrow-checked.
void stage(const char* text) {
    fprintf(stderr, "rust_sharding_fixture: stage %s\n", text);
}

// @unsafe - ends the process without unwinding (see the file comment).
[[noreturn]] void setup_error(const std::string& message) {
    fprintf(stderr, "rust_sharding_fixture: SETUP ERROR %s\n", message.c_str());
    fflush(stdout);
    fflush(stderr);
    _Exit(2);
}

// @unsafe - may end the process through setup_error.
void require(bool holds, const char* message) {
    if (!holds) setup_error(message);
}

// @unsafe - as require, with the status the crate or the engine answered.
void require_status(uint32_t status, uint32_t expected, const char* message) {
    if (status != expected)
        setup_error(std::string(message) + ": status " + std::to_string(status)
                    + ", expected " + std::to_string(expected));
}

// @unsafe - stdio is not borrow-checked.
void check_ok(const char* name) {
    printf("CHECK %s ok\n", name);
}

// @unsafe - stdio is not borrow-checked. The detail stays on the check's line.
void check_failed(const char* name, std::string detail) {
    any_check_failed.set(true);
    for (char& c : detail)
        if (c == '\n' || c == '\r') c = ' ';
    printf("CHECK %s FAILED %s\n", name, detail.c_str());
}

// @unsafe - stdio is not borrow-checked.
void check_skipped(const char* name) {
    printf("CHECK %s SKIPPED\n", name);
}

// What one check found wrong: the first three findings and how many more.
struct Problems {
    unsigned count = 0;
    std::string text;

    // @safe - records one finding.
    void add(const std::string& problem) {
        if (count < 3) {
            if (count != 0) text += "; ";
            text += problem;
        }
        ++count;
    }
    // @safe - the check's detail.
    std::string detail() const {
        if (count <= 3) return text;
        return text + "; and " + std::to_string(count - 3) + " more";
    }
};

// @unsafe - prints the result of one check.
void conclude(const char* name, const Problems& problems) {
    if (problems.count == 0) check_ok(name);
    else check_failed(name, problems.detail());
}

// @unsafe - one check. An exception from the adapter is a finding of this
// check; it must not unwind past the running nodes.
template <typename Body>
void check(const char* name, Body&& body) {
    Problems problems;
    try {
        body(problems);
    } catch (const std::exception& error) {
        problems.add(std::string("exception: ") + error.what());
    } catch (...) {
        problems.add("exception of an unknown type");
    }
    conclude(name, problems);
}

// @unsafe - bytes are borrowed for one synchronous native call.
MakoShardBytes bytes(const std::string& value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

// @unsafe - a key for a diagnostic line; every byte outside [0x21, 0x7e] and
// the backslash are written as \xNN. Formats through stdio.
std::string printable(const std::string& key) {
    std::string text;
    for (const char c : key) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte > 0x20 && byte < 0x7f && c != '\\') {
            text.push_back(c);
        } else {
            char escaped[8];
            snprintf(escaped, sizeof(escaped), "\\x%02x", byte);
            text += escaped;
        }
    }
    return text.empty() ? "(empty)" : text;
}

// @safe - 96 bytes that contain NUL and 0xff bytes and differ for every row.
std::string value_of(unsigned row) {
    std::string value(96, '\0');
    for (unsigned j = 0; j < value.size(); ++j)
        value[j] = static_cast<char>((row * 31 + j * 7) & 0xff);
    return value;
}

// @unsafe - the keys m0000, m0001, ... Formats through stdio.
std::string m_key(unsigned row) {
    char text[16];
    snprintf(text, sizeof(text), "m%04u", row);
    return text;
}

// @unsafe - the keys d00000, d00001, ... of the deleted rows of one owner.
// Formats through stdio.
std::string tombstone_key(char prefix, unsigned row) {
    char text[16];
    snprintf(text, sizeof(text), "%c%05u", prefix, row);
    return text;
}

// @safe - owner 0's rows after loading, in bytewise key order. Keys are byte
// strings: the empty key, embedded NUL bytes and 0xff are all present.
rusty::Vec<Row> source_rows() {
    rusty::Vec<Row> rows;
    rows.push(Row{std::string(), "empty-key"});
    rows.push(Row{"a", "left"});
    std::string large(60000, '\0');
    for (size_t j = 0; j < large.size(); ++j) large[j] = static_cast<char>(j & 0xff);
    rows.push(Row{std::string("b\0", 2), std::move(large)});
    rows.push(Row{std::string("b\0\0", 3), "binary-successor"});
    for (unsigned row = 0; row < 600; ++row) rows.push(Row{m_key(row), value_of(row)});
    rows.push(Row{std::string("m\xff", 2), std::string()});
    rows.push(Row{"n", "boundary-unmoved"});
    rows.push(Row{"z", "unmoved-right"});
    rows.push(Row{std::string("\xff\0\xff", 3), "high-key"});
    return rows;
}

// @safe - owner 1's rows before the migration. m0007 also exists on the source
// with other bytes and must be replaced; m9999 exists only here and must be
// removed; zz lies outside every migrated range.
rusty::Vec<Row> destination_rows() {
    rusty::Vec<Row> rows;
    rows.push(Row{m_key(7), "stale-collide"});
    rows.push(Row{m_key(9999), "stale-destination-only"});
    rows.push(Row{"zz", "destination-outside"});
    return rows;
}

// @safe - whether the migrated range [lo, hi) holds `key`. std::string orders
// bytewise, as the crate orders coordinates.
bool in_range(const Shape& shape, const std::string& key) {
    return key >= shape.lo && key < shape.hi;
}

// @safe - how many of `rows` lie in the migrated range.
size_t rows_in_range(const Shape& shape, const rusty::Vec<Row>& rows) {
    size_t inside = 0;
    for (const auto& row : rows)
        if (in_range(shape, row.key)) ++inside;
    return inside;
}

// @safe - how many of the destination's rows in the migrated range the source
// does not have: the rows a final copy or an abort must remove there beyond
// the copied ones.
size_t destination_only_in_range(const Shape& shape, const rusty::Vec<Row>& source,
                                 const rusty::Vec<Row>& destination) {
    size_t only = 0;
    for (const auto& row : destination) {
        if (!in_range(shape, row.key)) continue;
        bool on_source = false;
        for (const auto& other : source)
            if (other.key == row.key) on_source = true;
        if (!on_source) ++only;
    }
    return only;
}

// @unsafe - adapter one-row verbs on the attached loader thread. In a
// two-shard process each is a protocol commit, so every row carries
// distributed history.
void load(const rusty::Vec<Row>& source, const rusty::Vec<Row>& destination) {
    // m0003 gets two committed versions; the second one is in `source`.
    const std::string twice = m_key(3);
    require(raw[0]->put(lcdf::Str(twice), "superseded-version"),
            "the first version of m0003 was not a new row");
    for (const auto& row : source) {
        const bool created = raw[0]->put(lcdf::Str(row.key), row.value);
        if (created != (row.key != twice))
            setup_error("source row " + printable(row.key) + " loaded with created="
                        + std::to_string(created));
    }
    // m0600 lies inside every migrated range, keeps a value in its history and
    // must not reach the destination.
    const std::string deleted = m_key(600);
    require(raw[0]->put(lcdf::Str(deleted), "deleted-before-migration"),
            "m0600 was not a new row");
    require(raw[0]->remove(lcdf::Str(deleted)), "m0600 was not present when deleted");
    for (const auto& row : destination)
        if (!raw[1]->put(lcdf::Str(row.key), row.value))
            setup_error("destination row " + printable(row.key) + " was not a new row");
}

// @unsafe - writes and then deletes kTombstones keys on `owner` through the
// adapter verbs. The table's directory keeps a deleted key, so afterwards
// that many deleted entries lie between "c" and the first live row m0000.
void load_tombstones(unsigned owner, char prefix, size_t live_rows) {
    const std::string who = "owner " + std::to_string(owner);
    for (unsigned row = 0; row < kTombstones; ++row)
        if (!raw[owner]->put(lcdf::Str(tombstone_key(prefix, row)), "deleted-before-migration"))
            setup_error(who + " row " + tombstone_key(prefix, row) + " was not a new row");
    if (raw[owner]->size() != live_rows + kTombstones)
        setup_error(who + " holds " + std::to_string(raw[owner]->size())
                    + " rows with the rows to delete, expected "
                    + std::to_string(live_rows + kTombstones));
    for (unsigned row = 0; row < kTombstones; ++row)
        if (!raw[owner]->remove(lcdf::Str(tombstone_key(prefix, row))))
            setup_error(who + " row " + tombstone_key(prefix, row)
                        + " was not present when deleted");
}

// @unsafe - point reads through the adapter on the loader thread. The C++
// scan answers IO before it looks at any row, so without this the baseline
// would fail the same way over tables that were never loaded.
void require_loaded(unsigned owner, const rusty::Vec<Row>& rows) {
    const std::string who = "owner " + std::to_string(owner);
    if (raw[owner]->size() != rows.size())
        setup_error(who + " holds " + std::to_string(raw[owner]->size()) + " rows, expected "
                    + std::to_string(rows.size()));
    for (const auto& row : rows) {
        std::string value;
        if (!raw[owner]->get(lcdf::Str(row.key), value, std::string::npos))
            setup_error(who + " lacks row " + printable(row.key));
        if (value != row.value)
            setup_error(who + " row " + printable(row.key) + " has " + std::to_string(value.size())
                        + " bytes that differ from the " + std::to_string(row.value.size())
                        + " loaded");
    }
}

// @unsafe - the crate's context is the owner's NativeShardingHost.
uint32_t owner_of(void* context) {
    return static_cast<mako::NativeShardingHost*>(context)->owner;
}

// @safe - records one forwarded call and hands its status back.
uint32_t counted(VerbCount& count, uint32_t status) {
    count.calls.fetch_add(1);
    if (status == MAKO_SHARD_OK) count.ok.fetch_add(1);
    count.last.store(status);
    return status;
}

// @unsafe - forwards on the crate's storage thread and notes its STO slot.
uint32_t fx_thread_enter(void* context, uint32_t entering) {
    const uint32_t owner = owner_of(context);
    const uint32_t status = real[owner].thread_enter(context, entering);
    counts[owner].enter.fetch_add(1);
    if (status == MAKO_SHARD_OK) {
        counts[owner].enter_ok.fetch_add(1);
        counts[owner].slot.store(TThread::id());
    }
    return status;
}

// @unsafe - forwards on the crate's storage thread, then looks at that
// thread's own engine handle: thread_leave must have released it.
void fx_thread_leave(void* context) {
    const uint32_t owner = owner_of(context);
    real[owner].thread_leave(context);
    counts[owner].leave.fetch_add(1);
    if (rust_sto_tpcc_wrapper::sharding_host_idle_thread() != nullptr)
        counts[owner].left_attached.fetch_add(1);
}

// @unsafe - forwards. Both table handles are registered before the nodes
// start, so the crate has no reason to ask; the count shows whether it did.
uint32_t fx_open(void* context, uint64_t table, MakoShardBytes name, uint32_t asked,
                 uint32_t proxy, uint32_t kind, uintptr_t* handle) {
    const uint32_t owner = owner_of(context);
    counts[owner].opens.fetch_add(1);
    return real[owner].open(context, table, name, asked, proxy, kind, handle);
}

// @unsafe - forwards to the wrapped table on the crate's storage thread.
uint32_t fx_scan_next(void* context, uint64_t table, MakoShardBytes lo, uint32_t has_hi,
                      MakoShardBytes hi, uint32_t has_after, MakoShardBytes after_coordinate,
                      MakoShardBytes after_key, MakoShardRowSink sink, void* sink_context) {
    const uint32_t owner = owner_of(context);
    return counted(counts[owner].scan_next,
                   real[owner].scan_next(context, table, lo, has_hi, hi, has_after,
                                         after_coordinate, after_key, sink, sink_context));
}

// @unsafe - forwards to the wrapped table on the crate's storage thread.
uint32_t fx_put(void* context, uint64_t table, MakoShardBytes coordinate, MakoShardBytes key,
                MakoShardBytes value) {
    const uint32_t owner = owner_of(context);
    if (delay_first_put && owner == 1 && counts[owner].put.calls.load() == 0)
        usleep(1500000);
    return counted(counts[owner].put, real[owner].put(context, table, coordinate, key, value));
}

// @unsafe - forwards to the wrapped table on the crate's storage thread.
uint32_t fx_remove(void* context, uint64_t table, MakoShardBytes coordinate,
                   MakoShardBytes key) {
    const uint32_t owner = owner_of(context);
    return counted(counts[owner].remove, real[owner].remove(context, table, coordinate, key));
}

// @unsafe - forwards and keeps the cut the source took at Freeze.
uint64_t fx_epoch_cut(void* context) {
    const uint32_t owner = owner_of(context);
    const uint64_t cut = real[owner].epoch_cut(context);
    counts[owner].last_cut.store(cut);
    counts[owner].cuts.fetch_add(1);
    return cut;
}

// @unsafe - forwards and counts the answers by kind.
uint32_t fx_epoch_drained(void* context, uint64_t cut) {
    const uint32_t owner = owner_of(context);
    const uint32_t status = real[owner].epoch_drained(context, cut);
    if (status == MAKO_SHARD_OK) counts[owner].drained_ok.fetch_add(1);
    else if (status == MAKO_SHARD_BUSY) counts[owner].drained_busy.fetch_add(1);
    else counts[owner].drained_other.fetch_add(1);
    return status;
}

// @safe - the table the crate receives: `table` with every slot but
// `peer_call` counted. The crate never gets an unwrapped table.
MakoShardHost wrap(const MakoShardHost& table) {
    MakoShardHost wrapped = table;
    wrapped.thread_enter = fx_thread_enter;
    wrapped.thread_leave = fx_thread_leave;
    wrapped.open = fx_open;
    wrapped.scan_next = fx_scan_next;
    wrapped.put = fx_put;
    wrapped.remove = fx_remove;
    wrapped.epoch_cut = fx_epoch_cut;
    wrapped.epoch_drained = fx_epoch_drained;
    return wrapped;
}

// @safe - a status for a diagnostic line.
std::string status_text(uint32_t status) {
    return status == kNoStatus ? "none" : std::to_string(status);
}

// @unsafe - stdio is not borrow-checked.
void report_calls() {
    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        const auto& seen = counts[owner];
        fprintf(stderr,
                "rust_sharding_fixture: calls owner=%u"
                " scan_next=%llu scan_next_ok=%llu scan_next_last=%s"
                " put=%llu put_ok=%llu put_last=%s"
                " remove=%llu remove_ok=%llu remove_last=%s\n",
                owner,
                static_cast<unsigned long long>(seen.scan_next.calls.load()),
                static_cast<unsigned long long>(seen.scan_next.ok.load()),
                status_text(seen.scan_next.last.load()).c_str(),
                static_cast<unsigned long long>(seen.put.calls.load()),
                static_cast<unsigned long long>(seen.put.ok.load()),
                status_text(seen.put.last.load()).c_str(),
                static_cast<unsigned long long>(seen.remove.calls.load()),
                static_cast<unsigned long long>(seen.remove.ok.load()),
                status_text(seen.remove.last.load()).c_str());
    }
    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        const auto& seen = counts[owner];
        fprintf(stderr,
                "rust_sharding_fixture: epochs owner=%u cuts=%llu"
                " drained_busy=%llu drained_ok=%llu drained_other=%llu\n",
                owner, static_cast<unsigned long long>(seen.cuts.load()),
                static_cast<unsigned long long>(seen.drained_busy.load()),
                static_cast<unsigned long long>(seen.drained_ok.load()),
                static_cast<unsigned long long>(seen.drained_other.load()));
    }
    for (uint32_t owner = 0; owner < kOwners; ++owner)
        fprintf(stderr, "rust_sharding_fixture: opens owner=%u open=%llu\n", owner,
                static_cast<unsigned long long>(counts[owner].opens.load()));
}

// @unsafe - stdio is not borrow-checked.
void report_threads() {
    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        const auto& seen = counts[owner];
        fprintf(stderr,
                "rust_sharding_fixture: threads owner=%u enter=%llu enter_ok=%llu leave=%llu"
                " attached_after_leave=%llu\n",
                owner, static_cast<unsigned long long>(seen.enter.load()),
                static_cast<unsigned long long>(seen.enter_ok.load()),
                static_cast<unsigned long long>(seen.leave.load()),
                static_cast<unsigned long long>(seen.left_attached.load()));
    }
}

// @unsafe - the real connected native srpc sender, addressed to the
// coordinator (owner 0).
uint32_t admin_call(void* host, uint32_t operation, MakoShardBytes payload,
                    MakoShardReplySink sink, void* context) {
    return mako::native_sharding_peer_call(host, 0, operation, payload, sink, context);
}

// @safe - the admin result's outcome as the word the driver matches.
const char* outcome_text(uint32_t outcome) {
    switch (outcome) {
        case 0: return "pending";
        case 1: return "committed";
        case 2: return "aborted";
        default: return "unknown";
    }
}

// @unsafe - one poll of the migration's outcome through the admin path.
void poll(MakoShardAdminResult& result) {
    usleep(kPollMicros);
    require_status(mako_sharding_admin_poll(kNonce, host_of[0], admin_call, &result),
                   MAKO_SHARD_OK, "admin poll");
}

// Stage a write while the source is serving. Migration publishes Freeze
// while this attempt is still open. The adapter owns its epoch announcement.
constexpr char kWriterValue[] = "committed across freeze";
void hold_writer(rust_sto_tpcc_wrapper& db, bool commit) {
    BenchmarkConfig::setThreadLocalShardIndex(0);
    db.thread_init(true, 0);
    TThread::in_loading_phase = false;
    str_arena arena;
    auto* txn = db.new_txn(0, arena, nullptr);
    tx_put(raw[0], txn, lcdf::Str(m_key(3)), mako::Encode(kWriterValue));
    const auto announced = Transaction::tinfo[TThread::id()].epoch.load();
    pin.epoch.store(announced);
    require(announced != 0, "ordinary Rust writer has no epoch announcement");
    pin.state.store(kPinHeld);
    while (pin.state.load() != kPinRelease) usleep(1000);
    if (commit) require(db.commit_txn(txn), "ordinary write did not commit after freeze");
    else db.abort_txn(txn);
    require(Transaction::tinfo[TThread::id()].epoch.load() == 0, "terminal kept its epoch");
    db.thread_end();
    pin.state.store(kPinReleased);
}

void check_remote_scan(rust_sto_tpcc_wrapper& db) {
    struct Rows final : oi_scan_callback {
        std::vector<std::pair<std::string, std::string>> values;
        bool invoke(const char* key, size_t length, const std::string& value) override {
            values.emplace_back(std::string(key, length), value);
            return true;
        }
    } rows;
    str_arena arena;
    const std::string stop = "o";
    const std::string reverse_stop = m_key(524);
    const std::string empty_stop = m_key(9000);
    decltype(rows.values) expected;
    for (unsigned i = 525; i < 600; ++i) expected.emplace_back(m_key(i), value_of(i));
    expected.emplace_back(std::string("m\xff", 2), std::string());
    expected.emplace_back("n", "boundary-unmoved");

    auto* txn = db.new_txn(0, arena, nullptr);
    try {
        tx_scan(raw[0], txn, m_key(525), &stop, rows, &arena);
        require(rows.values == expected, "forward scan lost rows across the owner boundary");
        rows.values.clear();
        std::reverse(expected.begin(), expected.end());
        tx_rscan(raw[0], txn, "n", &reverse_stop, rows, &arena);
        require(rows.values == expected, "reverse scan lost rows at the page cursor");
        require(db.commit_txn(txn), "paged range transaction did not certify");
    } catch (...) {
        db.abort_txn(txn);
        throw;
    }
    txn = db.new_txn(0, arena, nullptr);
    try {
        rows.values.clear();
        tx_scan(raw[0], txn, m_key(600), &empty_stop, rows, &arena);
        require(rows.values.empty(), "empty migrated interval returned a row");
        require(db.commit_txn(txn), "empty range participant did not finish");
    } catch (...) {
        db.abort_txn(txn);
        throw;
    }
}

// Keep the coordinator's original index across cutover. Requests sent through
// the normal shard client must reach owner 1 and finish its real helper frame.
constexpr char kRemoteWrite[] = "written from the former owner";
void check_remote_transaction(rust_sto_tpcc_wrapper& db, transport::Configuration& config) {
    register_sync_util_sc([] { return 0; });
    register_sync_util_ss([] { return 0; });
    mako::HelperQueue requests(0, true);
    mako::HelperQueue responses(0, false);
    auto address = config.shard(1, 0).host;
    FastTransport transport(config.configFile, address, "localhost", 1, mako::fullScanReqType,
                            0, 0, 1, config.warehouses + 5);
    transport.SetHelperQueues({{0, &requests}});
    transport.SetHelperQueuesResponse({{0, &responses}});
    auto io = rusty::thread::spawn([&] { transport.Run(); });
    auto* runtime = SiloRuntime::Current();
    auto helper = rusty::thread::spawn([&] {
        BenchmarkConfig::setThreadLocalShardIndex(1);
        runtime->BindToCurrentThread();
        db.thread_init(true, 1);
        TThread::set_mode(1);
        TThread::in_loading_phase = false;
        mako::ShardServer server(config.configFile, 1, 0, 0);
        // A preopened migration destination can be absent from the helper's
        // startup catalog. The received canonical route selects it.
        server.Register(&db, &requests, &responses, {});
        server.Run();
        db.thread_end();
    });
    std::exception_ptr failure;
    try {
        mako::ShardClient client(config.configFile, "localhost", 0, 0, true);
        TThread::sclient = &client;
        struct ResetClient {
            ~ResetClient() { TThread::sclient = nullptr; }
        } reset;
        check_remote_scan(db);
        str_arena arena;
        auto* txn = db.new_txn(0, arena, nullptr);
        std::string value;
        require(tx_get(raw[0], txn, m_key(3), value) && value == kWriterValue,
                "former owner's remote read missed the committed writer");
        require(tx_get(raw[0], txn, lcdf::Str("z"), value) && value == "unmoved-right",
                "same transaction lost its local read");
        tx_put(raw[0], txn, m_key(4), mako::Encode(kRemoteWrite));
        require(db.commit_txn(txn), "remote write did not commit");
    } catch (...) {
        failure = std::current_exception();
    }
    transport.Stop();
    io.join().unwrap();
    helper.join().unwrap();
    if (failure) std::rethrow_exception(failure);
}

void start_pin(rust_sto_tpcc_wrapper& db, bool commit) {
    pin.thread = rusty::Some(rusty::thread::spawn([&db, commit]() { hold_writer(db, commit); }));
    for (unsigned attempt = 0; attempt < 5000 && pin.state.load() == kPinStarting; ++attempt)
        usleep(1000);
    require(pin.state.load() == kPinHeld && pin.epoch.load() != 0,
            "the pin thread did not announce an STO epoch");
    fprintf(stderr, "rust_sharding_fixture: pin epoch=%llu\n",
            static_cast<unsigned long long>(pin.epoch.load()));
}

// @unsafe - lets the pin thread leave its epoch and joins it.
void release_pin() {
    pin.state.store(kPinRelease);
    pin.thread.as_mut().unwrap().join().unwrap();
    pin.thread = rusty::None;
    require(pin.state.load() == kPinReleased, "the pin thread did not leave its epoch");
    stage("pin-released");
}

// @unsafe - Ordinary transactions intern keys outside the migrating range.
// Every transaction removes its own inserted value before commit, leaving
// directory tombstones without changing the fixture's expected live image.
class DirectoryChurn {
    rusty::sync::atomic::AtomicU32 stop_{0};
    rusty::sync::atomic::AtomicU64 committed_{0};
    rusty::Option<rusty::thread::JoinHandle<rusty::thread::Unit>> thread_;
    std::exception_ptr failure_;
    uint64_t before_ = 0;
public:
    DirectoryChurn(rust_sto_tpcc_wrapper& db, bool enabled) {
        if (!enabled) return;
        auto* runtime = SiloRuntime::Current();
        thread_ = rusty::Some(rusty::thread::spawn([this, &db, runtime] {
            try {
                BenchmarkConfig::setThreadLocalShardIndex(0);
                runtime->BindToCurrentThread();
                db.thread_init(true, 0);
                TThread::in_loading_phase = false;
                uint64_t sequence = 0;
                while (!stop_.load()) {
                    const std::string key = "z-churn-" + std::to_string(sequence++);
                    str_arena arena;
                    auto* txn = db.new_txn(0, arena, nullptr);
                    try {
                        tx_put(raw[0], txn, lcdf::Str(key), mako::Encode("temporary"));
                        raw[0]->tx_remove(txn, lcdf::Str(key));
                        if (db.commit_txn(txn)) committed_.fetch_add(1);
                    } catch (const abstract_db::abstract_abort_exception&) {
                        db.abort_txn(txn);
                    }
                }
                db.thread_end();
            } catch (...) {
                failure_ = std::current_exception();
                stop_.store(1);
            }
        }));
        for (unsigned attempt = 0; attempt < 10000 && committed_.load() < 100 && !stop_.load(); ++attempt)
            usleep(1000);
        before_ = committed_.load();
    }
    void finish(bool report) {
        if (thread_.is_none()) return;
        stop_.store(1);
        thread_.as_mut().unwrap().join().unwrap();
        thread_ = rusty::None;
        if (report) {
            if (failure_) std::rethrow_exception(failure_);
            require(before_ >= 100 && committed_.load() > before_,
                    "ordinary directory churn did not overlap migration");
            fprintf(stderr, "rust_sharding_fixture: concurrent directory commits=%llu before=%llu\n",
                static_cast<unsigned long long>(committed_.load()),
                static_cast<unsigned long long>(before_));
            check_ok("concurrent-directory-migration");
        }
    }
    ~DirectoryChurn() { finish(false); }
};

// Diagnostic reads inspect the physical owner, including rows an ordinary
// transaction can no longer address there after migration.
sto_tpcc_thread* begin_inspection() {
    auto* thread = rust_sto_tpcc_wrapper::sharding_host_idle_thread();
    require_status(sto_tpcc_protocol_begin(thread, 0), STO_TPCC_OK, "inspection begin");
    return thread;
}
void finish_inspection(sto_tpcc_thread* thread) {
    require_status(sto_tpcc_protocol_validate(thread), STO_TPCC_OK, "inspection validate");
    sto_tpcc_protocol_report report{};
    require_status(sto_tpcc_protocol_finish(thread, 1, &report), STO_TPCC_OK, "inspection finish");
}
const sto_tpcc_table* physical_table(unsigned owner) {
    return static_cast<rust_sto_tpcc_ordered_index*>(raw[owner])->sharding_host_table();
}
bool read_physical(const sto_tpcc_table* table, const std::string& key, std::string& value) {
    auto* thread = begin_inspection();
    size_t length = 0;
    uint32_t version = 0;
    value.resize(UINT16_MAX);
    const auto status = sto_tpcc_protocol_get(thread, table, bytes(key).data,
        key.size(), reinterpret_cast<uint8_t*>(value.data()), value.size(), &length, &version);
    if (status != STO_TPCC_MISS) require_status(status, STO_TPCC_OK, "physical read");
    value.resize(length);
    finish_inspection(thread);
    return status == STO_TPCC_OK;
}

bool read_row(unsigned owner, const std::string& key, std::string& value) {
    return read_physical(physical_table(owner), key, value);
}

// @unsafe - `row` must be on `owner` with exactly its bytes.
void expect_row(Problems& problems, unsigned owner, const Row& row) {
    std::string value;
    if (!read_row(owner, row.key, value))
        problems.add("owner " + std::to_string(owner) + " lacks row " + printable(row.key));
    else if (value != row.value)
        problems.add("owner " + std::to_string(owner) + " row " + printable(row.key) + " has "
                     + std::to_string(value.size()) + " bytes that differ from the "
                     + std::to_string(row.value.size()) + " expected");
}

// @unsafe - `key` must be absent on `owner`.
void expect_absent(Problems& problems, unsigned owner, const std::string& key) {
    std::string value;
    if (read_row(owner, key, value))
        problems.add("owner " + std::to_string(owner) + " holds row " + printable(key) + " ("
                     + std::to_string(value.size()) + " bytes)");
}

// @unsafe - the committed row count of `owner`'s table. With every expected
// row found present, an equal count means there is no other row.
void expect_size(Problems& problems, unsigned owner, size_t expected) {
    const size_t rows = raw[owner]->size();
    if (rows != expected)
        problems.add("owner " + std::to_string(owner) + " holds " + std::to_string(rows)
                     + " rows, expected " + std::to_string(expected));
}

// @unsafe - where the crate routes the range's bounds and every loaded source
// key. `moved` says whether the range must now belong to owner 1.
Problems route_problems(const Shape& shape, const rusty::Vec<Row>& source, bool moved) {
    Problems problems;
    const auto probe = [&](const char* what, const std::string& key, uint32_t expected) {
        MakoShardRoute route{};
        const uint32_t status = mako_sharding_route(kRawTable, bytes(key), &route);
        if (status != MAKO_SHARD_OK)
            problems.add(std::string("route of ") + what + printable(key) + ": status "
                         + std::to_string(status));
        else if (route.grant.owner != expected)
            problems.add(what + printable(key) + " routes to owner "
                         + std::to_string(route.grant.owner) + ", expected "
                         + std::to_string(expected));
    };
    probe("the lower bound ", shape.lo, moved ? 1 : 0);
    probe("the upper bound ", shape.hi, 0);
    for (const auto& row : source)
        probe("row ", row.key, moved && in_range(shape, row.key) ? 1 : 0);
    return problems;
}

void expect_scan(Problems& problems, unsigned owner, const rusty::Vec<Row>& expected) {
    struct Comparison {
        const rusty::Vec<Row>& rows;
        size_t at = 0;
        bool match = true;
    } comparison{expected};
    auto compare = [](void* context, const uint8_t* key, size_t klen, const uint8_t* value,
                      size_t vlen, uint32_t) -> int32_t {
        auto& state = *static_cast<Comparison*>(context);
        if (state.at >= state.rows.size()) { state.match = false; return 1; }
        const auto& row = state.rows[state.at++];
        state.match = row.key.size() == klen && row.value.size() == vlen
            && (!klen || !memcmp(key, row.key.data(), klen))
            && (!vlen || !memcmp(value, row.value.data(), vlen));
        return state.match ? 0 : 1;
    };
    auto* thread = begin_inspection();
    size_t visited = 0;
    require_status(sto_tpcc_protocol_scan(thread, physical_table(owner), STO_TPCC_SCAN_FORWARD,
        STO_TPCC_BOUND_UNBOUNDED, nullptr, 0, STO_TPCC_BOUND_UNBOUNDED, nullptr, 0,
        expected.size() + 1, compare, &comparison, &visited), STO_TPCC_OK, "physical scan");
    finish_inspection(thread);
    if (!comparison.match || comparison.at != expected.size())
        problems.add("physical scan differs on owner " + std::to_string(owner)
                     + " at row " + std::to_string(comparison.at));
}

// Asks one adapter scan for its first row only.
struct FirstRow final : oi_scan_callback {
    rusty::Option<std::string> key;
    // @safe - the base destructor does not throw.
    ~FirstRow() noexcept override {}
    // @unsafe - copies the key the adapter lends for this call and stops.
    bool invoke(const char* row_key, size_t key_length, const std::string&) override {
        key = rusty::Some(std::string(row_key, key_length));
        return false;
    }
    // @safe - one row is all the caller consumes.
    size_t max_records_hint() const override { return 1; }
};

// @unsafe - the premise of `deleted-keys`. The adapter's own scan is one
// transaction, every directory entry it walks is one of its items, and the
// adapter caps a transaction at 1,024 items; so with 5,000 deleted keys in
// front of the first live row it must fail, on both owners, even when it is
// asked for one row. If it ever succeeds, this scenario no longer shows that
// the storage lookahead crosses what one transaction cannot.
void tombstones_block_one_scan(Problems& problems, const Shape& shape) {
    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        FirstRow first;
        try {
            raw[owner]->scan(shape.lo, &shape.hi, first, nullptr);
            problems.add("one adapter scan of the range on owner " + std::to_string(owner)
                         + (first.key.is_some()
                                ? " reached row " + printable(first.key.as_ref().unwrap())
                                : std::string(" ended without a row"))
                         + " instead of failing at the item cap");
        } catch (const storage_resource_exhausted&) {
            // The expected result: the deleted keys alone exceed the cap.
        }
    }
}

// @unsafe - at least `least` forwarded calls of a verb answered OK.
void expect_ok_calls(Problems& problems, unsigned owner, const char* verb, const VerbCount& count,
                     uint64_t least) {
    const uint64_t ok = count.ok.load();
    if (ok < least)
        problems.add("owner " + std::to_string(owner) + " " + verb + " answered OK "
                     + std::to_string(ok) + " times, expected at least " + std::to_string(least));
}

// @unsafe - no forwarded call of a verb at all.
void expect_no_calls(Problems& problems, unsigned owner, const char* verb,
                     const VerbCount& count) {
    const uint64_t calls = count.calls.load();
    if (calls != 0)
        problems.add("owner " + std::to_string(owner) + " " + verb + " was called "
                     + std::to_string(calls) + " times, expected none");
}

// @unsafe - neither storage thread announces an STO epoch between callbacks,
// and a fresh cut of the process clock is drained at once.
void no_pin_left(Problems& problems) {
    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        const int slot = counts[owner].slot.load();
        if (slot < 0 || slot >= MAX_THREADS) {
            problems.add("owner " + std::to_string(owner) + " has no storage thread slot");
            continue;
        }
        const auto announced = Transaction::tinfo[slot].epoch.load();
        if (announced != 0)
            problems.add("owner " + std::to_string(owner) + " storage thread (slot "
                         + std::to_string(slot) + ") announces epoch "
                         + std::to_string(announced));
    }
    const auto cut = Transaction::epoch_cut();
    if (cut == 0 || !Transaction::epoch_drained(cut))
        problems.add("a new cut " + std::to_string(cut) + " is not drained");
}

// @unsafe - the checks that follow a committed migration of [lo, hi).
void committed_checks(const Shape& shape, const rusty::Vec<Row>& source,
                      const rusty::Vec<Row>& destination) {
    const size_t inside = rows_in_range(shape, source);
    const std::string collide = m_key(7);
    const std::string extra = m_key(9999);
    const std::string deleted = m_key(600);

    // The route is published after the commit; give it ten seconds.
    Problems routes = route_problems(shape, source, true);
    for (unsigned attempt = 0; attempt < 200 && routes.count != 0; ++attempt) {
        usleep(50000);
        routes = route_problems(shape, source, true);
    }
    conclude("route-moved", routes);

    check("destination-image", [&](Problems& problems) {
        for (const auto& row : source)
            if (in_range(shape, row.key)) expect_row(problems, 1, row);
        expect_absent(problems, 1, extra);
        size_t outside = 0;
        for (const auto& row : destination)
            if (!in_range(shape, row.key)) ++outside;
        expect_size(problems, 1, inside + outside);
    });
    check("stale-collide-replaced", [&](Problems& problems) {
        expect_row(problems, 1, Row{collide, value_of(7)});
    });
    check("stale-extra-removed", [&](Problems& problems) { expect_absent(problems, 1, extra); });
    check("destination-outside-untouched", [&](Problems& problems) {
        for (const auto& row : destination)
            if (!in_range(shape, row.key)) expect_row(problems, 1, row);
    });
    check("source-range-empty", [&](Problems& problems) {
        for (const auto& row : source)
            if (in_range(shape, row.key)) expect_absent(problems, 0, row.key);
        expect_absent(problems, 0, deleted);
        expect_size(problems, 0, source.size() - inside);
    });
    check("source-outside-untouched", [&](Problems& problems) {
        for (const auto& row : source)
            if (!in_range(shape, row.key)) expect_row(problems, 0, row);
    });
    if (shape.ordered_scan) {
        check("ordered-scan-agrees", [&](Problems& problems) {
            rusty::Vec<Row> left;
            rusty::Vec<Row> moved;
            for (const auto& row : source) {
                if (in_range(shape, row.key)) moved.push(Row{row.key, row.value});
                else left.push(Row{row.key, row.value});
            }
            // Every destination row outside the range sorts after the range.
            for (const auto& row : destination) {
                if (in_range(shape, row.key)) continue;
                require(row.key >= shape.hi, "a destination row sorts below the range");
                moved.push(Row{row.key, row.value});
            }
            expect_scan(problems, 0, left);
            expect_scan(problems, 1, moved);
        });
    }
    check("callbacks-ran", [&](Problems& problems) {
        // Every row of the range was read on the source at least once and
        // written on the destination at least once; the destination's stale
        // row that the source lacks was removed there; every row was removed
        // on the source. Nothing is ever put on the source.
        expect_ok_calls(problems, 0, "scan_next", counts[0].scan_next, inside);
        expect_ok_calls(problems, 1, "scan_next", counts[1].scan_next, 1);
        expect_ok_calls(problems, 1, "put", counts[1].put, inside);
        expect_ok_calls(problems, 0, "remove", counts[0].remove, inside);
        expect_ok_calls(problems, 1, "remove", counts[1].remove,
                        destination_only_in_range(shape, source, destination));
        expect_no_calls(problems, 0, "put", counts[0].put);
    });
    check("no-pin-left", no_pin_left);
}

// @unsafe - the checks that follow the aborted migration of scenario
// `aborted`: nothing moved, and the destination removed its stage range,
// stale rows included.
void aborted_checks(const Shape& shape, const rusty::Vec<Row>& source,
                    const rusty::Vec<Row>& destination) {
    const size_t inside = rows_in_range(shape, source);
    conclude("route-unmoved", route_problems(shape, source, false));
    check("source-intact", [&](Problems& problems) {
        for (const auto& row : source) expect_row(problems, 0, row);
        expect_absent(problems, 0, m_key(600));
        expect_size(problems, 0, source.size());
    });
    check("destination-stage-clean", [&](Problems& problems) {
        for (const auto& row : source)
            if (in_range(shape, row.key)) expect_absent(problems, 1, row.key);
        size_t outside = 0;
        for (const auto& row : destination) {
            if (in_range(shape, row.key)) {
                expect_absent(problems, 1, row.key);
            } else {
                expect_row(problems, 1, row);
                ++outside;
            }
        }
        expect_size(problems, 1, outside);
    });
    check("callbacks-ran", [&](Problems& problems) {
        // Precopy read every row on the source and wrote it on the
        // destination; the abort removed all of them there, and the stale
        // row that precopy did not overwrite. The source removed nothing.
        const size_t staged = inside + destination_only_in_range(shape, source, destination);
        expect_ok_calls(problems, 0, "scan_next", counts[0].scan_next, inside);
        expect_ok_calls(problems, 1, "scan_next", counts[1].scan_next, staged);
        expect_ok_calls(problems, 1, "put", counts[1].put, inside);
        expect_ok_calls(problems, 1, "remove", counts[1].remove, staged);
        expect_no_calls(problems, 0, "remove", counts[0].remove);
        expect_no_calls(problems, 0, "put", counts[0].put);
    });
    check("no-pin-left", no_pin_left);
}

// @unsafe - counts the rows a storage verb hands to its sink.
void counting_sink(void* context, MakoShardBytes, MakoShardBytes, MakoShardBytes) {
    ++*static_cast<unsigned*>(context);
}

// @unsafe - `open`, called on the main thread through the unwrapped table:
// it answers the registered handle and nothing else.
void open_registered_only(Problems& problems) {
    const std::string name(kRawName);
    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        const std::string who = "owner " + std::to_string(owner) + " open";
        void* context = host_of[owner];
        uintptr_t handle = 0;
        uint32_t status = real[owner].open(context, kRawTable, bytes(name), owner, 0, 0, &handle);
        if (status != MAKO_SHARD_OK || handle != reinterpret_cast<uintptr_t>(raw[owner]))
            problems.add(who + " of the registered table: status " + std::to_string(status)
                         + (handle == reinterpret_cast<uintptr_t>(raw[owner])
                                ? "" : ", not the registered handle"));
        const auto refused = [&](const char* what, uint64_t table, uint32_t asked, uint32_t proxy,
                                 uintptr_t* out) {
            handle = 0;
            status = real[owner].open(context, table, bytes(name), asked, proxy, 0, out);
            if (status != MAKO_SHARD_INVALID || handle != 0)
                problems.add(who + " " + what + ": status " + std::to_string(status)
                             + (handle != 0 ? " with a handle" : "") + ", expected 3");
        };
        uintptr_t proxy = 0;
        status = real[owner].open(context, kRawTable, bytes(name), owner, 1, 0, &proxy);
        auto* facade = reinterpret_cast<abstract_ordered_index*>(proxy);
        if (status != MAKO_SHARD_OK || !facade || facade == raw[owner]
            || !facade->get_is_remote() || facade->get_table_id() != raw[owner]->get_table_id())
            problems.add(who + " did not return the registered table's origin-side proxy");
        refused("of an invalid proxy flag", kRawTable, owner, 2, &handle);
        refused("of an unregistered table", kUnknownTable, owner, 0, &handle);
        refused("for another owner", kRawTable, 1 - owner, 0, &handle);
        refused("without a place for the handle", kRawTable, owner, 0, nullptr);
    }
}

// @unsafe - the three storage verbs, called on the main thread through the
// unwrapped table. This thread is attached to the engine but was never
// entered for sharding, so each must refuse and deliver or change nothing.
void unentered_thread_refused(Problems& problems) {
    const std::string empty;
    const std::string key("zzz-unentered");
    const std::string present("z");
    const std::string value("must-not-appear");
    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        const std::string who = "owner " + std::to_string(owner) + " ";
        void* context = host_of[owner];
        unsigned delivered = 0;
        const auto refused = [&](const char* verb, uint32_t status) {
            if (status != MAKO_SHARD_INVALID)
                problems.add(who + verb + " on an unentered thread: status "
                             + std::to_string(status) + ", expected 3");
        };
        refused("scan_next", real[owner].scan_next(context, kRawTable, bytes(empty), 0,
                                                   bytes(empty), 0, bytes(empty), bytes(empty),
                                                   counting_sink, &delivered));
        if (delivered != 0) problems.add(who + "scan_next delivered a row to an unentered thread");
        refused("put", real[owner].put(context, kRawTable, bytes(key), bytes(key), bytes(value)));
        refused("remove", real[owner].remove(context, kRawTable, bytes(present), bytes(present)));
    }
}

// @unsafe - registers `index` as owner's raw table with the Rust verbs and the
// crate, and notes whether the registration refuses what it must refuse.
void register_rust(Problems& guarded, uint32_t owner) {
    const auto refused = [&](const char* what, uint64_t table, uint32_t asked,
                             abstract_ordered_index* index) {
        const uint32_t status = mako::rust_sharding_register_raw(table, asked, index);
        if (status != MAKO_SHARD_INVALID)
            guarded.add(std::string("registration ") + what + " for owner "
                        + std::to_string(asked) + ": status " + std::to_string(status)
                        + ", expected 3");
    };
    refused("without an index", kRawTable, owner, nullptr);
    refused("of a table the catalog does not hold", kUnknownTable, owner, raw[owner]);
    refused("of another owner's index", kRawTable, 1 - owner, raw[owner]);
    require_status(mako::rust_sharding_register_raw(kRawTable, owner, raw[owner]), MAKO_SHARD_OK,
                   "Rust raw table registration");
    const uint32_t again = mako::rust_sharding_register_raw(kRawTable, owner, raw[owner]);
    if (again != MAKO_SHARD_OK)
        guarded.add("repeating owner " + std::to_string(owner) + "'s registration: status "
                    + std::to_string(again) + ", expected 0");
}

// @unsafe - rust_sharding_callbacks must refuse, with an exception, a host
// that has no Rust-engine database and a replicated process.
void callbacks_guarded(Problems& guarded, rust_sto_tpcc_wrapper& db) {
    const auto refuses = [](mako::NativeShardingHost& host) {
        try {
            (void)mako::rust_sharding_callbacks(host);
        } catch (const std::logic_error&) {
            return true;
        }
        return false;
    };
    {
        mako::NativeShardingHost without_engine(nullptr, 0, kOwners, SiloRuntime::Current());
        if (!refuses(without_engine))
            guarded.add("a callback table was handed out for a host without a Rust-engine "
                        "database");
    }
    {
        mako::NativeShardingHost replicated(&db, 0, kOwners, SiloRuntime::Current());
        auto& bench = BenchmarkConfig::getInstance();
        bench.setIsReplicated(1);
        const bool refused = refuses(replicated);
        bench.setIsReplicated(0);
        if (!refused) guarded.add("a callback table was handed out in a replicated process");
    }
}

// Move warehouses one and two together. Both contain the same physical key.
// Warehouse three stays outside the range. Other warehouses start absent and
// are materialized with workers attached, beyond the legacy 200-selector window.
void migrate_warehouses(rust_sto_tpcc_wrapper& db,
                        rusty::Option<rusty::Box<mako::NativeShardingHost>>* hosts) {
    constexpr unsigned count = 3;
    const std::string name("history");
    uint64_t table = 0;
    require_status(mako_sharding_catalog_tpcc(1), MAKO_SHARD_OK, "warehouse catalog");
    require_status(mako_sharding_warehouse_init(20, 40), MAKO_SHARD_OK, "dimensions");
    require_status(mako_sharding_table_id(bytes(name), &table), MAKO_SHARD_OK, "catalog ID");
    abstract_ordered_index* physical[kOwners][count]{};
    for (unsigned owner = 0; owner < kOwners; ++owner) {
        BenchmarkConfig::setThreadLocalShardIndex(owner);
        TThread::set_shard_index(owner);
        for (unsigned warehouse = 1; warehouse <= count; ++warehouse) {
            auto* index = db.open_index("fixture_history_" + std::to_string(owner) + "_"
                                        + std::to_string(warehouse), static_cast<int>(owner));
            require(index && !index->get_is_remote(), "physical warehouse open");
            require_status(mako::rust_sharding_register_warehouse(table, owner, warehouse, index),
                           MAKO_SHARD_OK, "warehouse registration");
            physical[owner][warehouse - 1] = index;
        }
    }
    BenchmarkConfig::setThreadLocalShardIndex(0);
    TThread::set_shard_index(0);
    db.thread_init(true, 0);
    for (unsigned i = 0; i < count; ++i) {
        require(physical[0][i]->put("same", "source" + std::to_string(i)), "load source");
        require(physical[1][i]->put("stale", "destination" + std::to_string(i)), "load destination");
    }
    require(physical[0][0]->put("second", "second-row"), "load a second key");
    for (unsigned owner = 0; owner < kOwners; ++owner) {
        hosts[owner] = rusty::Some(rusty::make_box<mako::NativeShardingHost>(
            &db, owner, kOwners, SiloRuntime::Current()));
        auto& host = *hosts[owner].as_mut().unwrap();
        host_of[owner] = &host;
        auto callbacks = host.callbacks();
        db.configure_native_sharding(host, callbacks);
        uintptr_t cold = 0;
        require_status(mako_sharding_warehouse_local(table, 1, owner, &cold), MAKO_SHARD_INVALID,
                       "warehouse open before node activation");
        require_status(mako_sharding_start_node(owner, kOwners, 1, &callbacks), MAKO_SHARD_OK,
                       "warehouse node start");
        require_status(host.start_service(), MAKO_SHARD_OK, "warehouse service start");
    }
    for (unsigned owner = 0; owner < kOwners; ++owner) {
        require_status(mako_sharding_wait_ready(owner), MAKO_SHARD_OK, "warehouse data ready");
        uintptr_t handle = 0;
        require_status(mako_sharding_warehouse_local(table, 41, owner, &handle), MAKO_SHARD_INVALID,
                       "out-of-range warehouse open");
        require_status(mako_sharding_warehouse_local(table, 1, owner, &handle), MAKO_SHARD_OK,
                       "registered warehouse open");
        require(handle == reinterpret_cast<uintptr_t>(physical[owner][0]), "wrong warehouse handle");
    }
    check_ok("warehouse-open");
    const char* governed[] = {"warehouse", "district", "customer", "customer_name_idx",
        "history", "new_order", "oorder", "oorder_c_id_idx", "order_line", "stock", "stock_data"};
    for (unsigned owner = 0; owner < kOwners; ++owner) {
        for (const char* logical : governed) {
            uint64_t identity = 0;
            require_status(mako_sharding_table_id(bytes(std::string(logical)), &identity),
                           MAKO_SHARD_OK, "lazy catalog identity");
            for (unsigned warehouse = 4; warehouse <= 24; ++warehouse) {
                uintptr_t first = 0, repeated = 0;
                require_status(mako_sharding_warehouse_local(identity, warehouse, owner, &first),
                               MAKO_SHARD_OK, "lazy warehouse open");
                require_status(mako_sharding_warehouse_local(identity, warehouse, owner, &repeated),
                               MAKO_SHARD_OK, "repeat lazy warehouse open");
                require(first && first == repeated, "lazy handle was not stable");
                auto* index = reinterpret_cast<abstract_ordered_index*>(first);
                require(!index->get_is_remote() && index->size() == 0, "new warehouse not empty");
            }
        }
    }
    check_ok("warehouse-lazy-catalog");
    TThread::in_loading_phase = false;
    const std::string lo("\0\0\0\1", 4), hi("\0\0\0\3", 4);
    MakoShardAdminResult result{};
    require_status(mako_sharding_admin_begin(kNonce, bytes(name), bytes(lo), 1, bytes(hi), 0, 1,
                                            host_of[0], admin_call, &result), MAKO_SHARD_OK,
                   "begin warehouse move");
    for (unsigned i = 0; i < 60 * kPollsPerSecond && !result.outcome; ++i) poll(result);
    require(result.outcome == 1, "warehouse move did not commit");
    check_ok("warehouse-committed");
    auto read = [](abstract_ordered_index* index, const char* key, std::string& value) {
        return read_physical(static_cast<rust_sto_tpcc_ordered_index*>(index)->sharding_host_table(), key, value);
    };
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t handle = 0;
        MakoShardGrant grant{};
        require_status(mako_sharding_warehouse_resolve(table, i + 1, 0, &handle, &grant),
                       MAKO_SHARD_OK, "published warehouse route");
        const bool moved = i < 2;
        require(grant.owner == static_cast<unsigned>(moved), "wrong route after move");
        auto* selected = reinterpret_cast<abstract_ordered_index*>(handle);
        require(selected && selected->get_is_remote() == moved &&
                selected->get_table_id() == physical[0][i]->get_table_id(), "wrong route handle");
        std::string value;
        auto* source = physical[0][i];
        auto* destination = physical[1][i];
        require(read(source, "same", value) != moved, "source cleanup");
        if (!moved) require(value == "source2", "outside-range source value");
        require(read(destination, "same", value) == moved, "copied key");
        if (moved) require(value == "source" + std::to_string(i), "copied value");
        require(read(destination, "stale", value) != moved, "destination cleanup");
    }
    std::string value;
    require(read(physical[1][0], "second", value) && value == "second-row",
            "second physical key was lost");
    require(!read(physical[0][0], "second", value), "second key cleanup");
    check_ok("warehouse-bytes");
    for (unsigned owner = 0; owner < kOwners; ++owner) host_of[owner]->stop_accepting();
    for (unsigned owner = 0; owner < kOwners; ++owner) mako_sharding_stop_node(owner);
    db.thread_end();
    for (unsigned owner = 0; owner < kOwners; ++owner) {
        db.native_sharding_stopped(owner);
        host_of[owner] = nullptr;
        hosts[owner] = rusty::None;
    }
}

// @unsafe - one scenario, on the main thread. `db`, `config` and `hosts`
// belong to main() so that an exception cannot destroy them on its way out.
// Returns with both nodes stopped, the hosts destroyed and this thread
// detached when no check failed, and with everything still running otherwise.
void run(Scenario scenario, const char* config_path, rust_sto_tpcc_wrapper& db,
         rusty::Option<rusty::Box<transport::Configuration>>& config,
         rusty::Option<rusty::Box<mako::NativeShardingHost>>* hosts) {
    const Shape shape = shape_of(scenario);
    delay_first_put = scenario == Scenario::migrate;
    config = rusty::Some(rusty::make_box<transport::Configuration>(config_path));
    auto& topology = *config.as_mut().unwrap();
    topology.multi_shard_mode = true;
    topology.local_shard_indices = {0, 1};
    auto& bench = BenchmarkConfig::getInstance();
    bench.setConfig(&topology);
    bench.setNshards(kOwners);
    bench.setNthreads(1);
    bench.setShardIndex(0);
    bench.setIsReplicated(0);
    bench.setIsMicro(0);
    bench.setPaxosProcName("localhost");
    bench.setPinCpus(0);
    SiloRuntime::Current()->BindToCurrentThread();
    db.init();
    if (scenario == Scenario::warehouses) return migrate_warehouses(db, hosts);

    // Both tables exist before this thread, or any other, attaches to the
    // engine: creating a table attaches a temporary engine worker.
    require_status(mako_sharding_catalog_register(kRawTable, bytes(kRawName), 0, 0),
                   MAKO_SHARD_OK, "raw catalog registration");
    Problems guarded;
    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        BenchmarkConfig::setThreadLocalShardIndex(static_cast<int>(owner));
        TThread::set_shard_index(static_cast<int>(owner));
        raw[owner] = db.open_index(kRawName, static_cast<int>(owner));
        require(raw[owner] != nullptr, "raw index open");
        require(!raw[owner]->get_is_remote(), "raw index is not owner-local");
        if (raw[owner]->get_table_id()
                != static_cast<int32_t>(owner) * mako::NUM_TABLES_PER_SHARD + 1)
            setup_error("owner " + std::to_string(owner) + " raw index has table id "
                        + std::to_string(raw[owner]->get_table_id()));
    }
    BenchmarkConfig::setThreadLocalShardIndex(0);
    TThread::set_shard_index(0);
    require(raw[0] != raw[1], "the two owners share one index");
    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        if (shape.rust_callbacks) {
            register_rust(guarded, owner);
        } else {
            // The baseline has no Rust registration to go through. Both
            // handles must be known to the crate, or the node start is
            // refused first.
            require_status(mako_sharding_raw_register(kRawTable, owner, 0,
                                                      reinterpret_cast<uintptr_t>(raw[owner])),
                           MAKO_SHARD_OK, "raw handle registration");
        }
    }

    db.thread_init(true, 0);
    auto source = source_rows();
    const auto destination = destination_rows();
    load(source, destination);
    if (shape.tombstones) {
        load_tombstones(0, 'd', source.size());
        load_tombstones(1, 'e', destination.size());
    }
    require_loaded(0, source);
    require_loaded(1, destination);
    {
        std::string value;
        require(!raw[0]->get(lcdf::Str(m_key(600)), value, std::string::npos),
                "m0600 is still present on owner 0");
    }
    stage("loaded");
    if (shape.tombstones) {
        check("tombstones-block-one-scan", [&](Problems& problems) {
            tombstones_block_one_scan(problems, shape);
        });
    }

    // The advancer alone, not ensure_epoch_runtime: this thread's Masstree
    // context must stay the one the Rust engine attached.
    Transaction::start_epoch_advancer();
    {
        const auto before = Transaction::epoch_current();
        bool grew = false;
        for (unsigned attempt = 0; attempt < 40 && !grew; ++attempt) {
            usleep(50000);
            grew = Transaction::epoch_current() > before;
        }
        if (grew) check_ok("tick-running");
        else check_failed("tick-running", "the STO epoch did not grow in 2 seconds");
    }

    if (shape.rust_callbacks) callbacks_guarded(guarded, db);
    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        hosts[owner] = rusty::Some(rusty::make_box<mako::NativeShardingHost>(
            &db, owner, kOwners, SiloRuntime::Current()));
        auto& host = *hosts[owner].as_mut().unwrap();
        host_of[owner] = &host;
        real[owner] = host.callbacks();
        if (shape.rust_callbacks) db.configure_native_sharding(host, real[owner]);
        if (shape.rust_callbacks) {
            // The first table handed out closed the registration.
            const uint32_t late = mako::rust_sharding_register_raw(kRawTable, owner, raw[owner]);
            if (late != MAKO_SHARD_INVALID)
                guarded.add("registration after the callback table was handed out: status "
                            + std::to_string(late) + ", expected 3");
        }
        const MakoShardHost table = wrap(real[owner]);
        require_status(mako_sharding_start_node(owner, kOwners, 1, &table), MAKO_SHARD_OK,
                       "node start");
        require_status(host.start_service(), MAKO_SHARD_OK, "srpc listener start");
    }
    stage("nodes-started");
    {
        bool ready = false;
        for (unsigned attempt = 0; attempt < 200 && !ready; ++attempt) {
            ready = mako_sharding_ready(0) && mako_sharding_ready(1);
            if (!ready) usleep(50000);
        }
        require(ready, "the two owners did not become ready in 10 seconds");
    }
    stage("ready");
    TThread::in_loading_phase = false;

    if (shape.rust_callbacks) {
        conclude("registration-guarded", guarded);
        check("open-registered-only", open_registered_only);
        check("unentered-thread-refused", unentered_thread_refused);
    } else {
        // These call the Rust registration and the installed Rust slots
        // directly; the baseline has neither.
        check_skipped("registration-guarded");
        check_skipped("open-registered-only");
        check_skipped("unentered-thread-refused");
    }

    if (scenario == Scenario::pinned) {
        check_rust_sharding_receiver(db, raw[0], kRawTable, config_path, "a", "n");
        check_ok("receiver-replayed-own-reply");
        mako::NativeShardingHost cpp_peer(&db, 0, kOwners, SiloRuntime::Current());
        uint32_t reply = kNoStatus;
        const auto receive = [](void* ctx, uint32_t status, MakoShardBytes) {
            *static_cast<uint32_t*>(ctx) = status;
        };
        require_status(mako::native_sharding_peer_call(&cpp_peer, 1, 2, {nullptr, 0}, receive,
            &reply), MAKO_SHARD_OK, "mixed peer HELLO transport");
        require_status(reply, MAKO_SHARD_INVALID, "mixed peer HELLO");
        check_ok("mixed-peer-refused");
    }
    if (shape.pin) start_pin(db, !shape.abort);
    DirectoryChurn churn(db, scenario == Scenario::deleted_keys);
    MakoShardAdminResult result{};
    require_status(mako_sharding_admin_begin(kNonce, bytes(kRawName), bytes(shape.lo), 1,
                                             bytes(shape.hi), 0, 1, host_of[0], admin_call,
                                             &result),
                   MAKO_SHARD_OK, "admin begin");
    stage("begun");
    unsigned polls = 0;
    uint64_t busy_before_release = 0;
    uint64_t ok_before_release = 0;
    if (shape.pin) {
        Problems held;
        // The coordinator must reach its drain loop and stay there: one cut
        // on the source, then BUSY from `epoch_drained` again and again.
        bool draining = false;
        for (unsigned attempt = 0; attempt < 30 * kPollsPerSecond && result.outcome == 0
                 && !draining; ++attempt) {
            poll(result);
            ++polls;
            draining = counts[0].cuts.load() >= 1
                && counts[0].drained_busy.load() >= kDrainAnswersWanted;
        }
        if (result.outcome != 0) {
            held.add(std::string("migration reached outcome=") + outcome_text(result.outcome)
                     + " while a pin older than the cut was held");
        } else if (!draining) {
            held.add("drain loop not reached");
        } else {
            const uint64_t cut = counts[0].last_cut.load();
            if (pin.epoch.load() >= cut)
                held.add("the pin announces epoch " + std::to_string(pin.epoch.load())
                         + ", which is not older than the cut " + std::to_string(cut));
            // One more second: still pending, and the cut still not drained.
            for (unsigned attempt = 0; attempt < kPollsPerSecond && held.count == 0; ++attempt) {
                poll(result);
                ++polls;
                if (result.outcome != 0)
                    held.add(std::string("migration reached outcome=")
                             + outcome_text(result.outcome)
                             + " while a pin older than the cut was held");
                else if (Transaction::epoch_drained(cut))
                    held.add("the STO clock reports the cut " + std::to_string(cut)
                             + " drained while the pin was held");
            }
        }
        conclude("pinned-stays-pending", held);
        stage("held");
        busy_before_release = counts[0].drained_busy.load();
        ok_before_release = counts[0].drained_ok.load();
        if (draining) {
            str_arena arena;
            auto* late = db.new_txn(0, arena, nullptr);
            bool refused = false;
            try {
                tx_put(raw[0], late, lcdf::Str(m_key(3)), mako::Encode("late stale write"));
            } catch (const abstract_db::abstract_abort_exception&) {
                refused = true;
            }
            db.abort_txn(late);
            require(refused, "post-freeze transaction staged a write on the old owner");
            check_ok("stale-write-refused");
        }
        if (shape.abort) {
            require_status(mako_sharding_admin_abort(kNonce, host_of[0], admin_call, &result),
                           MAKO_SHARD_OK, "admin abort");
            stage("abort-requested");
        }
        release_pin();
        if (!shape.abort) {
            for (auto& row : source) if (row.key == m_key(3)) row.value = kWriterValue;
        }
    }
    for (const unsigned limit = polls + shape.wait_seconds * kPollsPerSecond;
         result.outcome == 0 && polls < limit;) {
        poll(result);
        ++polls;
    }
    fprintf(stderr, "rust_sharding_fixture: outcome=%s generation=%llu polls=%u\n",
            outcome_text(result.outcome), static_cast<unsigned long long>(result.generation),
            polls);
    churn.finish(true);
    report_calls();

    if (scenario == Scenario::pinned && result.outcome == 1) {
        // Use a fresh coordinator on the new owner. Its captured route must
        // select the migrated physical table for an ordinary transaction.
        auto reader = rusty::thread::spawn([&db]() {
            BenchmarkConfig::setThreadLocalShardIndex(1);
            db.thread_init(true, 0);
            TThread::in_loading_phase = false;
            str_arena arena;
            auto* txn = db.new_txn(0, arena, nullptr);
            std::string value;
            require(tx_get(raw[1], txn, lcdf::Str(m_key(3)), value) && value == kWriterValue,
                    "new owner did not serve the pre-freeze writer's value");
            require(db.commit_txn(txn), "new owner's read did not certify");
            db.thread_end();
        });
        reader.join().unwrap();
        check_ok("new-owner-read");
        check_remote_transaction(db, topology);
        for (auto& row : source) if (row.key == m_key(4)) row.value = kRemoteWrite;
        check_ok("former-owner-transaction");
    }

    if (scenario == Scenario::pinned) {
        // The source answered BUSY while the pin was held and OK only after
        // its release.
        Problems drain;
        if (busy_before_release < kDrainAnswersWanted)
            drain.add("the source answered BUSY " + std::to_string(busy_before_release)
                      + " times before the release, expected at least "
                      + std::to_string(kDrainAnswersWanted));
        if (ok_before_release != 0)
            drain.add("the source answered OK " + std::to_string(ok_before_release)
                      + " times before the release");
        if (counts[0].drained_ok.load() == 0)
            drain.add("the source never answered OK after the release");
        conclude("drain-observed", drain);
    }

    const uint32_t expected_outcome = shape.abort ? 2 : 1;
    const char* outcome_check = shape.abort ? "outcome-aborted" : "outcome-committed";
    if (result.outcome == expected_outcome) {
        check_ok(outcome_check);
        if (shape.abort) aborted_checks(shape, source, destination);
        else committed_checks(shape, source, destination);
    } else {
        check_failed(outcome_check, std::string("outcome=") + outcome_text(result.outcome));
        if (shape.abort) {
            for (const char* name : kAfterAbortChecks) check_skipped(name);
        } else {
            for (const char* name : kAfterCommitChecks)
                if (shape.ordered_scan || strcmp(name, "ordered-scan-agrees") != 0)
                    check_skipped(name);
        }
    }
    // A failed check leaves through _Exit in main(): an injected or real
    // defect may have left the engine or the crate unable to stop.
    if (any_check_failed.get()) return;

    stage("stopping");
    for (uint32_t owner = 0; owner < kOwners; ++owner) host_of[owner]->stop_accepting();
    // Joins the owner's two threads; the storage thread runs thread_leave.
    for (uint32_t owner = 0; owner < kOwners; ++owner) mako_sharding_stop_node(owner);
    report_threads();
    check("threads-left", [&](Problems& problems) {
        for (uint32_t owner = 0; owner < kOwners; ++owner) {
            const auto& seen = counts[owner];
            if (seen.enter.load() != 1 || seen.enter_ok.load() != 1 || seen.leave.load() != 1)
                problems.add("owner " + std::to_string(owner) + " thread_enter was called "
                             + std::to_string(seen.enter.load()) + " times ("
                             + std::to_string(seen.enter_ok.load()) + " OK) and thread_leave "
                             + std::to_string(seen.leave.load()) + " times, expected one each");
            if (seen.left_attached.load() != 0)
                problems.add("owner " + std::to_string(owner)
                             + " storage thread still held its engine handle after thread_leave");
        }
    });
    if (any_check_failed.get()) return;
    // No callback runs any more. The hosts go first, then this thread's
    // engine handle, then the Rust verbs forget the tables, and the caller
    // destroys the database.
    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        host_of[owner] = nullptr;
        hosts[owner] = rusty::None;
    }
    db.thread_end();
    if (shape.rust_callbacks) mako::rust_sharding_release();
    stage("stopped");
}

// @safe - the scenario a command-line name selects.
rusty::Option<Scenario> parse_scenario(const char* name) {
    if (strcmp(name, "warehouses") == 0) return rusty::Some(Scenario::warehouses);
    if (strcmp(name, "migrate") == 0) return rusty::Some(Scenario::migrate);
    if (strcmp(name, "pinned") == 0) return rusty::Some(Scenario::pinned);
    if (strcmp(name, "deleted-keys") == 0) return rusty::Some(Scenario::deleted_keys);
    if (strcmp(name, "aborted") == 0) return rusty::Some(Scenario::aborted);
    if (strcmp(name, "red-baseline") == 0) return rusty::Some(Scenario::red_baseline);
    return rusty::None;
}

} // namespace

// @unsafe - real-engine fixture process: set-up, one scenario, then either an
// orderly end or _Exit.
int main(int argc, char** argv) {
    // Standard output is a file under the driver; _Exit does not flush it.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc != 3) {
        fprintf(stderr, "usage: rust_sharding_storage_fixture CONFIG SCENARIO\n");
        return 2;
    }
    const auto scenario = parse_scenario(argv[2]);
    if (scenario.is_none()) {
        fprintf(stderr, "rust_sharding_fixture: unknown scenario %s\n", argv[2]);
        return 2;
    }
    scenario_name.set(argv[2]);
    {
        // The database is declared after the configuration it reads and
        // before the hosts that borrow it, so it is destroyed between them.
        rusty::Option<rusty::Box<transport::Configuration>> config;
        rust_sto_tpcc_wrapper db;
        rusty::Option<rusty::Box<mako::NativeShardingHost>> hosts[kOwners];
        try {
            run(scenario.unwrap(), argv[1], db, config, hosts);
        } catch (const std::exception& error) {
            setup_error(std::string("exception: ") + error.what());
        } catch (...) {
            setup_error("exception of an unknown type");
        }
        if (any_check_failed.get()) {
            printf("RUST_SHARD_STORAGE FAIL %s\n", scenario_name.get());
            fflush(stdout);
            fflush(stderr);
            _Exit(1);
        }
        // run() stopped both nodes, destroyed the hosts and detached this
        // thread; the database goes at the end of this block.
    }
    stage("database-destroyed");
    printf("RUST_SHARD_STORAGE PASS %s\n", scenario_name.get());
    return 0;
}
