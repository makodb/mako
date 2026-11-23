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
// reached, the poll result, and per owner the storage calls the crate made.
//
// Exit code 1 follows FAIL. Exit code 2 is a set-up error (bad arguments, a
// requirement that did not hold) and is never an expected result. Once the
// database exists, both leave through _Exit: after a node has started, the
// crate's threads call the callback table until the node is stopped, so
// nothing may unwind out of main() and destroy the hosts or the database
// under them.
//
// Scenarios:
//   red-baseline  The C++ storage callback table, unchanged, on a Rust-engine
//                 database, with both table handles registered directly. This
//                 is the defect the Rust storage callbacks remove, and its
//                 expected result is the failure. No scenario can pass yet:
//                 the checks that follow a committed migration are not
//                 written, so each reports FAILED if a migration commits.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <exception>
#include <string>
#include "native_sharding_host.h"
#include "benchmarks/benchmark_config.h"
#include "lib/common.h"
#include "lib/configuration.h"
#include "silo_runtime.h"
#include "storage/mbta_wrapper.hh"
#include "storage/rust_sto_tpcc_wrapper.hh"
#include <rusty/cell.hpp>
#include <rusty/sync/atomic.hpp>

namespace {

constexpr uint64_t kRawTable = 1001;
constexpr uint32_t kOwners = 2;
constexpr uint32_t kNoStatus = UINT32_MAX;
constexpr unsigned kPollMicros = 100000;
const char kRawName[] = "rs_raw";

enum class Scenario { red_baseline };

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
struct OwnerCounts {
    VerbCount scan_next;
    VerbCount put;
    VerbCount remove;
};

// Main-thread state.
rusty::Cell<const char*> scenario_name{""};
rusty::Cell<bool> any_check_failed{false};
abstract_ordered_index* raw[kOwners]{};
// The callback tables the fixture wraps; written before the nodes start.
MakoShardHost real[kOwners]{};
OwnerCounts counts[kOwners];

// Checks that presuppose a committed migration, in reporting order.
const char* const kAfterCommitChecks[] = {
    "route-moved",
    "destination-image",
    "stale-collide-replaced",
    "stale-extra-removed",
    "destination-outside-untouched",
    "source-range-empty",
    "source-outside-untouched",
    "callbacks-ran",
    "no-pin-left",
};

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

// @unsafe - stdio is not borrow-checked.
void check_failed(const char* name, const std::string& detail) {
    any_check_failed.set(true);
    printf("CHECK %s FAILED %s\n", name, detail.c_str());
}

// @unsafe - stdio is not borrow-checked.
void check_skipped(const char* name) {
    printf("CHECK %s SKIPPED\n", name);
}

// @unsafe - bytes are borrowed for one synchronous native call.
MakoShardBytes bytes(const std::string& value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

// @safe - a key for a diagnostic line; every byte outside [0x21, 0x7e] and the
// backslash are written as \xNN.
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
    return text;
}

// @safe - 96 bytes that contain NUL and 0xff bytes and differ for every row.
std::string value_of(unsigned row) {
    std::string value(96, '\0');
    for (unsigned j = 0; j < value.size(); ++j)
        value[j] = static_cast<char>((row * 31 + j * 7) & 0xff);
    return value;
}

// @safe - the keys m0000, m0001, ...
std::string m_key(unsigned row) {
    char text[16];
    snprintf(text, sizeof(text), "m%04u", row);
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
    return counted(counts[owner].put, real[owner].put(context, table, coordinate, key, value));
}

// @unsafe - forwards to the wrapped table on the crate's storage thread.
uint32_t fx_remove(void* context, uint64_t table, MakoShardBytes coordinate,
                   MakoShardBytes key) {
    const uint32_t owner = owner_of(context);
    return counted(counts[owner].remove, real[owner].remove(context, table, coordinate, key));
}

// @safe - the table the crate receives: `table` with its three storage verbs
// counted. The crate never gets an unwrapped table.
MakoShardHost wrap(const MakoShardHost& table) {
    MakoShardHost wrapped = table;
    wrapped.scan_next = fx_scan_next;
    wrapped.put = fx_put;
    wrapped.remove = fx_remove;
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

// @unsafe - begins the migration of [lo, hi) from owner 0 to owner 1 through
// the admin path and polls until a terminal outcome or `seconds` have passed.
MakoShardAdminResult migrate(mako::NativeShardingHost* coordinator, const std::string& lo,
                             const std::string& hi, unsigned seconds) {
    const MakoShardTxn nonce{0x5253545348415244ULL, 1};
    MakoShardAdminResult result{};
    require_status(mako_sharding_admin_begin(nonce, bytes(kRawName), bytes(lo), 1, bytes(hi), 0, 1,
                                             coordinator, admin_call, &result),
                   MAKO_SHARD_OK, "admin begin");
    stage("begun");
    const unsigned limit = seconds * (1000000 / kPollMicros);
    unsigned polls = 0;
    while (result.outcome == 0 && polls < limit) {
        usleep(kPollMicros);
        ++polls;
        require_status(mako_sharding_admin_poll(nonce, coordinator, admin_call, &result),
                       MAKO_SHARD_OK, "admin poll");
    }
    fprintf(stderr, "rust_sharding_fixture: outcome=%s generation=%llu polls=%u\n",
            outcome_text(result.outcome), static_cast<unsigned long long>(result.generation),
            polls);
    return result;
}

// @unsafe - one scenario, on the main thread. `db`, `config` and `hosts`
// belong to main() so that an exception cannot destroy them on its way out.
void run(Scenario scenario, const char* config_path, rust_sto_tpcc_wrapper& db,
         rusty::Option<rusty::Box<transport::Configuration>>& config,
         rusty::Option<rusty::Box<mako::NativeShardingHost>>* hosts) {
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

    // Both tables exist before this thread, or any other, attaches to the
    // engine: creating a table attaches a temporary engine worker.
    require_status(mako_sharding_catalog_register(kRawTable, bytes(kRawName), 0, 0),
                   MAKO_SHARD_OK, "raw catalog registration");
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
        // The baseline has no Rust registration to go through. Both handles
        // must be known to the crate, or the node start is refused first.
        require_status(mako_sharding_raw_register(kRawTable, owner, 0,
                                                  reinterpret_cast<uintptr_t>(raw[owner])),
                       MAKO_SHARD_OK, "raw handle registration");
    }
    BenchmarkConfig::setThreadLocalShardIndex(0);
    TThread::set_shard_index(0);
    require(raw[0] != raw[1], "the two owners share one index");

    db.thread_init(true, 0);
    const auto source = source_rows();
    const auto destination = destination_rows();
    load(source, destination);
    TThread::in_loading_phase = false;
    require_loaded(0, source);
    require_loaded(1, destination);
    {
        std::string value;
        require(!raw[0]->get(lcdf::Str(m_key(600)), value, std::string::npos),
                "m0600 is still present on owner 0");
    }
    stage("loaded");

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

    for (uint32_t owner = 0; owner < kOwners; ++owner) {
        hosts[owner] = rusty::Some(rusty::make_box<mako::NativeShardingHost>(
            &db, owner, kOwners, SiloRuntime::Current()));
        auto& host = *hosts[owner].as_mut().unwrap();
        switch (scenario) {
            case Scenario::red_baseline: real[owner] = host.callbacks(); break;
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

    // These two call the installed Rust slots directly; the baseline has none.
    check_skipped("open-registered-only");
    check_skipped("unentered-thread-refused");

    // A scenario whose expected result is "not committed" waits 15 seconds.
    const unsigned wait_seconds = scenario == Scenario::red_baseline ? 15 : 60;
    const auto result = migrate(hosts[0].as_ref().unwrap().get(), std::string(), "n", wait_seconds);
    report_calls();
    if (result.outcome == 1) {
        check_ok("outcome-committed");
        for (const char* name : kAfterCommitChecks)
            check_failed(name, "this check is not written yet");
    } else {
        check_failed("outcome-committed", std::string("outcome=") + outcome_text(result.outcome));
        for (const char* name : kAfterCommitChecks) check_skipped(name);
    }
}

// @safe - the scenario a command-line name selects.
rusty::Option<Scenario> parse_scenario(const char* name) {
    if (strcmp(name, "red-baseline") == 0) return rusty::Some(Scenario::red_baseline);
    return rusty::None;
}

} // namespace

// @unsafe - real-engine fixture process: set-up, one scenario, _Exit.
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
    rust_sto_tpcc_wrapper db;
    rusty::Option<rusty::Box<transport::Configuration>> config;
    rusty::Option<rusty::Box<mako::NativeShardingHost>> hosts[kOwners];
    try {
        run(scenario.unwrap(), argv[1], db, config, hosts);
    } catch (const std::exception& error) {
        setup_error(std::string("exception: ") + error.what());
    } catch (...) {
        setup_error("exception of an unknown type");
    }
    // Every scenario reports at least one FAILED check so far, so there is no
    // passing exit yet. The orderly shutdown (stop accepting, stop both nodes,
    // destroy the hosts, detach this thread) belongs here once one can pass.
    require(any_check_failed.get(), "no check failed, and the passing exit is not written yet");
    printf("RUST_SHARD_STORAGE FAIL %s\n", scenario_name.get());
    fflush(stdout);
    fflush(stderr);
    _Exit(1);
}
