// Real MBTA/STO + native Rust + srpc fixture. The stdin protocol is test control,
// never a replacement storage/RPC implementation. Production methods do all I/O.
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include "native_sharding_host.h"
#include "benchmarks/benchmark_config.h"
#include "benchmarks/tpcc_warehouse_bridge.h"
#include "storage/mbta_wrapper.hh"
#include "lib/table_registry.h"
#include "lib/server.h"
#include "lib/sharding_admission.h"
#include <rusty/cell.hpp>
#include <rusty/condvar.hpp>
#include <rusty/thread.hpp>

namespace {
constexpr uint64_t raw_id = 1001;
constexpr unsigned row_count = 40;
FullOrderedIndex* raw[2]{};
FullOrderedIndex* physical_warehouse[2][2]{};
uint64_t warehouse_id = 0;

// @unsafe - assertion diagnostics cross the standard exception/I/O boundary.
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
// @unsafe - bytes borrowed only for synchronous native ABI calls.
MakoShardBytes bytes(const std::string& value) {
    return {reinterpret_cast<const uint8_t*>(value.data()), value.size()};
}

// @unsafe - one thread-bound native read, paired with the real STO announcement.
struct EpochRead {
    const MakoShardRead* value = nullptr;
    Transaction::epoch_type epoch = 0;
    // @safe - no guard until acquire succeeds.
    EpochRead() = default;
    EpochRead(const EpochRead&) = delete;
    EpochRead& operator=(const EpochRead&) = delete;
    // @unsafe - must be destroyed on the worker that admitted it.
    ~EpochRead() { release(); }
    // @unsafe - failed admission still releases its already-announced STO guard.
    uint32_t acquire(unsigned owner) {
        require(value == nullptr && epoch == 0, "read context already active");
        epoch = Transaction::epoch_enter();
        require(epoch != 0, "STO epoch exhausted");
        const auto status = mako_sharding_read_begin(owner, epoch, &value);
        if (status != MAKO_SHARD_OK) {
            require(value == nullptr, "failed begin returned a read context");
            Transaction::epoch_leave();
            epoch = 0;
        }
        return status;
    }
    // @unsafe - no engine or native borrow may survive this terminal release.
    void release() {
        if (value) mako_sharding_read_end(value);
        value = nullptr;
        if (epoch) Transaction::epoch_leave();
        epoch = 0;
    }
};

// @unsafe - inspect the identity only while a real STO/native read protects it.
// Routing/admission assertions keep the opaque ID from substituting for authority.
uint64_t read_view(unsigned owner, uint64_t table, const std::string& point,
                   uint32_t admission = MAKO_SHARD_OK) {
    EpochRead read;
    require(read.acquire(owner) == MAKO_SHARD_OK, "view identity read begin");
    uint64_t identity = 0;
    MakoShardRoute route{};
    require(mako_sharding_read_view_id(read.value, &identity) == MAKO_SHARD_OK
            && identity != 0, "protected read has no cache incarnation");
    require(mako_sharding_read_route(read.value, table, bytes(point), &route) == MAKO_SHARD_OK
            && mako_sharding_read_point(read.value, table, bytes(point), route) == admission,
            "view identity did not retain expected route/admission");
    return identity;
}

// Dedicated physical workers keep retained contexts off the stdin thread's TLS.
// Every job completes before its borrowed captures return to the caller.
struct FixtureWorker {
    struct Mailbox {
        rusty::Function<void()> job;
        std::exception_ptr failure;
        bool ready = false;
        bool pending = false;
        bool stopping = false;
    };
    rusty::Mutex<Mailbox> mailbox{Mailbox{}};
    rusty::Condvar changed;
    rusty::Option<rusty::thread::JoinHandle<rusty::thread::Unit>> thread;
    EpochRead read;
    rusty::Option<rusty::Box<mako::ShardingOperation>> operation;
    int slot = -1;
    Transaction::epoch_type cut = 0;
    bool conflict = false;
    mako::ShardingPreparedRoute prepared{warehouse_id, 1};
    mako::ShardingPreparedRoute interleaved{warehouse_id, 2};
    // @unsafe - host outlives worker; native thread_enter assigns a unique STO slot.
    explicit FixtureWorker(mako::NativeShardingHost& host) {
        thread = rusty::Some(rusty::thread::spawn([this, &host]() {
            const auto callbacks = host.callbacks();
            bool entered = false;
            try {
                require(callbacks.thread_enter(&host, host.owner) == MAKO_SHARD_OK,
                        "fixture worker registration");
                entered = true;
                slot = TThread::id();
                {
                    auto state = mailbox.lock().unwrap();
                    state->ready = true;
                    changed.notify_all();
                }
                for (;;) {
                    rusty::Function<void()> job;
                    {
                        auto state = mailbox.lock().unwrap();
                        state = changed.wait_while(std::move(state), [](Mailbox& value) {
                            return !value.pending && !value.stopping;
                        }).unwrap();
                        if (state->stopping) break;
                        job = std::move(state->job);
                    }
                    std::exception_ptr failure;
                    try { job(); } catch (...) { failure = std::current_exception(); }
                    auto state = mailbox.lock().unwrap();
                    state->failure = failure;
                    state->pending = false;
                    changed.notify_all();
                }
            } catch (...) {
                auto state = mailbox.lock().unwrap();
                state->failure = std::current_exception();
                state->ready = true;
                state->pending = false;
                changed.notify_all();
            }
            if (entered) {
                if (TThread::txn && TThread::txn->in_progress()) TThread::txn->silent_abort();
                operation = rusty::None;
                read.release();
                callbacks.thread_leave(&host);
            }
        }));
        auto state = mailbox.lock().unwrap();
        state = changed.wait_while(std::move(state), [](Mailbox& value) {
            return !value.ready;
        }).unwrap();
        if (state->failure) {
            thread.as_mut().unwrap().join().unwrap();
            std::rethrow_exception(state->failure);
        }
    }
    // @unsafe - synchronous mailbox completion bounds all captured references.
    void run(rusty::Function<void()> job) {
        auto state = mailbox.lock().unwrap();
        require(!state->pending && !state->stopping, "worker mailbox already occupied");
        state->failure = {};
        state->job = std::move(job);
        state->pending = true;
        changed.notify_all();
        state = changed.wait_while(std::move(state), [](Mailbox& value) {
            return value.pending;
        }).unwrap();
        if (state->failure) std::rethrow_exception(state->failure);
    }
    // @unsafe - cleanup runs on the owning TLS before the native host is destroyed.
    ~FixtureWorker() {
        {
            auto state = mailbox.lock().unwrap();
            state->stopping = true;
            changed.notify_all();
        }
        if (thread.is_some()) thread.as_mut().unwrap().join().unwrap();
    }
};

struct IssuedControl {
    void* host = nullptr;
    uint32_t target = 0;
    uint32_t operation = 0;
    std::string payload;
};
struct FaultState {
    bool capture = false;
    bool armed = false;
    uint32_t source = 0;
    unsigned drops = 0;
    unsigned successful_retries = 0;
    IssuedControl dropped;
    std::string terminal_reply;
    rusty::Vec<IssuedControl> issued;
};
struct PrecopyBarrier {
    bool armed = false;
    bool complete = false;
    bool released = false;
    uint32_t status = MAKO_SHARD_IO;
    IssuedControl request;
};
rusty::Mutex<PrecopyBarrier> precopy{PrecopyBarrier{}};
rusty::Condvar precopy_changed;
struct ReleasePrecopyOnExit {
    // @unsafe - release only the fixture callback barrier before native jobs join.
    ~ReleasePrecopyOnExit() {
        auto state = precopy.lock().unwrap();
        state->released = true;
        precopy_changed.notify_all();
    }
};
rusty::Mutex<FaultState> faults{FaultState{}};

struct CapturedReply {
    unsigned calls = 0;
    uint32_t status = MAKO_SHARD_IO;
    std::string payload;
    bool copied = false;
    // @unsafe - copies only the actual synchronous srpc response; never fabricates one.
    static void receive(void* opaque, uint32_t status, MakoShardBytes reply) {
        auto& self = *static_cast<CapturedReply*>(opaque);
        ++self.calls;
        self.status = status;
        try {
            if (reply.len) self.payload.assign(reinterpret_cast<const char*>(reply.data), reply.len);
            self.copied = true;
        } catch (...) {
            self.copied = false;
        }
    }
};

// @unsafe - close an actual cached srpc connection, then require a new handshake.
// This exercises the disconnected cache state without faking an RPC result.
void check_peer_reconnect(mako::NativeShardingHost& host) {
    CapturedReply initial;
    require(mako::native_sharding_peer_call(&host, host.owner, 2, {},
                CapturedReply::receive, &initial) == MAKO_SHARD_OK
                && initial.calls == 1 && initial.status == MAKO_SHARD_OK,
            "initial real peer handshake");
    auto disconnected = [&]() {
        auto peers = host.peers.lock().unwrap();
        require((*peers)[host.owner].is_some(), "handshake did not cache its connection");
        return (*peers)[host.owner].as_ref().unwrap().clone();
    }();
    disconnected->close();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        CapturedReply reply;
        const auto status = mako::native_sharding_peer_call(&host, host.owner, 2, {},
            CapturedReply::receive, &reply);
        if (status == MAKO_SHARD_OK && reply.calls == 1
                && reply.status == MAKO_SHARD_OK && reply.payload == initial.payload)
            return;
        usleep(20000);
    }
    require(false, "disconnected cached peer never reconnected");
}

// @unsafe - fixture-only callback-boundary fault. The unchanged production RPC
// applies the real request first. One successful source Commit response is then
// lost before Rust's sink is called, so the production driver must retry it.
uint32_t fault_peer_call(void* host, uint32_t target, uint32_t operation,
                        MakoShardBytes payload, MakoShardReplySink sink, void* sink_context) {
    if (operation == 19) {
      try {
        CapturedReply reply;
        const auto transport = mako::native_sharding_peer_call(host, target, operation,
            payload, CapturedReply::receive, &reply);
        if (transport != MAKO_SHARD_OK) return transport;
        if (reply.calls != 1 || !reply.copied) return MAKO_SHARD_IO;
        {
            auto state = precopy.lock().unwrap();
            if (state->armed) {
                state->status = reply.status;
                state->armed = false;
                state->request = {host, target, operation, {}};
                if (payload.len) state->request.payload.assign(
                    reinterpret_cast<const char*>(payload.data), payload.len);
                state->complete = true;
                precopy_changed.notify_all();
                state = precopy_changed.wait_while(std::move(state), [](PrecopyBarrier& value) {
                    return !value.released;
                }).unwrap();
            }
        }
        sink(sink_context, reply.status, bytes(reply.payload));
        return MAKO_SHARD_OK;
      } catch (...) {
        return MAKO_SHARD_IO;
      }
    }
    if (operation != 10 && operation != 12 && operation != 14 && operation != 15)
        return mako::native_sharding_peer_call(host, target, operation, payload, sink, sink_context);
    try {
        IssuedControl current{host, target, operation, {}};
        if (payload.len) current.payload.assign(reinterpret_cast<const char*>(payload.data), payload.len);
        {
            auto state = faults.lock().unwrap();
            if (state->capture) {
                bool recorded = false;
                for (const auto& previous : state->issued)
                    if (previous.host == host && previous.target == target
                        && previous.operation == operation && previous.payload == current.payload)
                        recorded = true;
                if (!recorded) state->issued.push(current);
            }
        }
        CapturedReply reply;
        const auto transport = mako::native_sharding_peer_call(host, target, operation,
            payload, CapturedReply::receive, &reply);
        if (transport != MAKO_SHARD_OK) return transport;
        if (reply.calls != 1 || !reply.copied) return MAKO_SHARD_IO;
        {
            auto state = faults.lock().unwrap();
            if (operation == 14 && target == state->source && reply.status == MAKO_SHARD_OK) {
                if (state->armed) {
                    state->armed = false;
                    ++state->drops;
                    state->dropped = current;
                    state->terminal_reply = reply.payload;
                    return MAKO_SHARD_IO; // exactly zero Rust sink calls
                }
                if (state->drops == 1 && state->dropped.host == host
                    && state->dropped.target == target && state->dropped.payload == current.payload) {
                    require(state->terminal_reply == reply.payload,
                            "retried source Commit changed its real retained receipt");
                    ++state->successful_retries;
                }
            }
        }
        sink(sink_context, reply.status, bytes(reply.payload));
        return MAKO_SHARD_OK;
    } catch (...) {
        return MAKO_SHARD_IO;
    }
}

// @unsafe - retains only actually issued messages from one completed generation.
void begin_capture(unsigned source, bool drop_reply) {
    auto state = faults.lock().unwrap();
    state->capture = true;
    state->armed = drop_reply;
    state->source = source;
    state->drops = 0;
    state->successful_retries = 0;
    state->dropped = {};
    state->terminal_reply.clear();
    state->issued.clear();
}
// @unsafe - replay traverses the original real srpc callback, not the fault hook.
void replay_controls(bool aborted, int fenced_target = -1) {
    rusty::Vec<IssuedControl> issued;
    {
        auto state = faults.lock().unwrap();
        require(!state->capture, "stop capture before replaying old messages");
        for (const auto& record : state->issued) issued.push(record);
    }
    unsigned operations = 0, terminal_targets = 0;
    for (const auto& record : issued) {
        if (fenced_target >= 0 && record.target != unsigned(fenced_target)) continue;
        CapturedReply reply;
        uint32_t status = MAKO_SHARD_IO;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            reply = {};
            status = mako::native_sharding_peer_call(record.host, record.target,
                record.operation, bytes(record.payload), CapturedReply::receive, &reply);
            if (status == MAKO_SHARD_OK && reply.calls == 1 && reply.copied) break;
            usleep(50000);
        }
        require(status == MAKO_SHARD_OK && reply.calls == 1 && reply.copied,
                "old message replay must reach actual production handler");
        if (fenced_target >= 0)
            require(reply.status == MAKO_SHARD_RETRY,
                    "retained metadata admitted an old control, including terminal cleanup");
        else
            require(reply.status == MAKO_SHARD_OK || reply.status == MAKO_SHARD_RETRY
                    || reply.status == MAKO_SHARD_INVALID, "unexpected old message application status");
        operations |= 1u << (record.operation - 10);
        if (record.operation == (aborted ? 15u : 14u)) terminal_targets |= 1u << record.target;
    }
    require(terminal_targets == (fenced_target < 0 ? 3u : 1u << fenced_target),
            "old terminal message was not issued and replayed to every requested peer");
    if (!aborted)
        require((operations & 21u) == 21u, "missing actual old Start/Final/Commit messages");
}
// @unsafe - deterministic fixture bytes, including embedded NUL payloads.
std::string key(unsigned i) {
    char result[16];
    std::snprintf(result, sizeof(result), "m%04u", i);
    return result;
}
// @unsafe - engine boundary expects std::string values.
std::string value(unsigned i, unsigned changed) {
    if (i == 1 && changed) return std::string("updated\0value", 13);
    return std::string(4096, static_cast<char>('A' + i % 26));
}
// @unsafe - canonical BE4 coordinate at the ABI edge.
std::string coordinate(unsigned w) {
    const char data[4] = {char(w >> 24), char(w >> 16), char(w >> 8), char(w)};
    return std::string(data, 4);
}
// @unsafe - test commands select a real owner context, never an ownership grant.
void select_owner(unsigned owner) {
    require(owner < 2, "invalid fixture owner");
    BenchmarkConfig::setThreadLocalShardIndex(owner);
    TThread::set_shard_index(owner);
}
// @unsafe - physical aliases are preloaded; proxy selection uses the production
// opener so interleaved warehouse descriptors exercise real engine wrappers.
uint32_t open_warehouse(void* context, uint64_t table, uint32_t wh, uint32_t owner,
                        uint32_t proxy, uintptr_t* output) {
    if (table != warehouse_id || wh < 1 || wh > 2 || owner > 1 || !output)
        return MAKO_SHARD_INVALID;
    if (proxy) return mako::tpcc_open_warehouse(context, table, wh, owner, proxy, output);
    if (!physical_warehouse[owner][wh - 1]) return MAKO_SHARD_INVALID;
    *output = reinterpret_cast<uintptr_t>(physical_warehouse[owner][wh - 1]);
    return MAKO_SHARD_OK;
}

struct Hosts {
    rusty::Vec<rusty::Box<mako::NativeShardingHost>> items;
    bool stopped = false;
    // @unsafe - listeners fence before native jobs join, including assertion failures.
    void shutdown() {
        if (stopped) return;
        stopped = true;
        for (auto& host : items) host->stop_accepting();
        for (auto& host : items) mako_sharding_stop_node(host->owner);
    }
    // @unsafe - host engine borrows end before the enclosing DB is destroyed.
    ~Hosts() { shutdown(); }
};

// @unsafe - copy a real native-storage row while the synchronous sink owns bytes.
struct NativeRow {
    std::string coordinate, key, value;
    unsigned calls = 0;
    static void receive(void* opaque, MakoShardBytes coordinate, MakoShardBytes key,
                        MakoShardBytes value) {
        auto& row = *static_cast<NativeRow*>(opaque);
        ++row.calls;
        row.coordinate.assign(reinterpret_cast<const char*>(coordinate.data), coordinate.len);
        row.key.assign(reinterpret_cast<const char*>(key.data), key.len);
        row.value.assign(reinterpret_cast<const char*>(value.data), value.len);
    }
};

// @unsafe - inspect real private MBTA bytes only after the synchronous precopy
// callback has completed. This is not an ordinary-admission/storage substitute.
void check_private_precopy(FixtureWorker& worker, mako::NativeShardingHost& host,
                           uint64_t table, bool warehouse) {
    worker.run([&]() {
        const auto callbacks = host.callbacks();
        const auto lo = warehouse ? coordinate(1) : std::string("m");
        const auto hi = warehouse ? coordinate(2) : std::string("n");
        NativeRow previous;
        unsigned count = 0;
        bool updated_row = false, deleted_row = false, destination_only_row = false;
        for (;;) {
            NativeRow row;
            const auto status = callbacks.scan_next(&host, table, bytes(lo), 1, bytes(hi),
                count != 0, bytes(previous.coordinate), bytes(previous.key), NativeRow::receive, &row);
            if (status == MAKO_SHARD_NOT_FOUND) break;
            require(status == MAKO_SHARD_OK && row.calls == 1, "private physical precopy scan");
            require(row.key != "m7777", "private precopy contains a post-copy insertion");
            if (row.key == "m9999") {
                require(row.value == "stale-destination-only", "private destination-only row changed");
                destination_only_row = true;
            }
            if (row.key == key(1)) {
                require(row.value == value(1, 0), "precopy did not retain pre-update bytes");
                updated_row = true;
            }
            if (row.key == key(2)) deleted_row = true;
            previous = std::move(row);
            ++count;
        }
        require(count == row_count + 1 + (warehouse ? 2 : 0)
                    && updated_row && deleted_row && destination_only_row,
                "background precopy did not populate the actual private destination");
    });
    EpochRead read;
    require(read.acquire(host.owner) == MAKO_SHARD_OK, "private destination read begin");
    const auto lo = warehouse ? coordinate(1) : std::string("m");
    const auto hi = warehouse ? coordinate(2) : std::string("n");
    const auto point = warehouse ? coordinate(1) : key(1);
    MakoShardRoute route{};
    require(mako_sharding_read_route(read.value, table, bytes(point), &route) == MAKO_SHARD_OK
            && route.grant.owner != host.owner, "precopy published destination routing");
    route.grant.owner = host.owner;
    ++route.grant.ownership_version;
    require(mako_sharding_read_point(read.value, table, bytes(point), route) == MAKO_SHARD_RETRY
            && mako_sharding_read_range(read.value, table, bytes(lo), 1, bytes(hi), route)
                == MAKO_SHARD_RETRY, "private precopy admitted prospective point/range grant");
}

// @unsafe - no migration cuts here: observe the running production timer advance
// while a real write transaction stays open, then commit through normal cleanup.
void check_periodic_epoch_commit(FullOrderedIndex* index) {
    bool cleanup_pinned = false;
    {
        mako::ShardingOperation operation;
        Sto::start_transaction();
        const auto k = key(0);
        const auto encoded = mako::Encode("committed-across-timer-ticks");
        index->tx_put(nullptr, lcdf::Str(k), encoded);
        const auto start = Transaction::epoch_current();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (Transaction::epoch_current() < start + 2) {
            require(std::chrono::steady_clock::now() < deadline, "production epoch timer did not advance");
            rusty::thread::yield_now();
        }
        const auto cut = Transaction::epoch_current();
        require(!Transaction::epoch_drained(cut), "timer advanced past an active engine guard");
        auto& callback = Transaction::tinfo[TThread::id()].trans_end_callback;
        auto original = std::move(callback);
        callback = [&]() {
            if (original) original();
            cleanup_pinned = !Transaction::epoch_drained(cut);
        };
        bool committed = false;
        try {
            committed = Sto::try_commit_no_paxos();
        } catch (...) {
            callback = std::move(original);
            throw;
        }
        callback = std::move(original);
        require(committed && cleanup_pinned, "timer aborted old commit or cleanup lost its pin");
        require(!Transaction::epoch_drained(cut), "commit released retained logical context");
    }
    require(Transaction::tinfo[TThread::id()].epoch.load() == 0, "completed operation leaked epoch");
    std::string actual;
    require(index->get(lcdf::Str(key(0)), actual, SIZE_MAX)
                && actual == "committed-across-timer-ticks", "old commit lost its actual engine effect");
    index->put(lcdf::Str(key(0)), value(0, 0));
}

struct Rows final : oi_scan_callback {
    rusty::Vec<std::pair<std::string, std::string>> rows;
    // @unsafe - string-pair destruction cannot throw; match the callback ABI.
    ~Rows() noexcept override = default;
    // @unsafe - copies actual engine callback bytes before their borrow expires.
    bool invoke(const char* data, size_t size, const std::string& contents) override {
        rows.push(std::make_pair(std::string(data, size), contents));
        return true;
    }
};
// @unsafe - actual FullOrderedIndex reads and complete scan/rscan calls.
void check_rows(FullOrderedIndex* index, unsigned changed) {
    require(index != nullptr, "fixture does not host requested physical index");
    for (unsigned i = 0; i < row_count; ++i) {
        std::string actual;
        const auto k = key(i);
        require(index->get(lcdf::Str(k), actual, SIZE_MAX), "missing migrated row");
        require(actual == value(i, changed), "wrong migrated value");
    }
    std::string absent;
    require(!index->get(lcdf::Str("m9999"), absent, SIZE_MAX), "destination-only stale row survived");
    const std::string lo = "m", hi = "n", reverse_hi = "m9999";
    Rows forward, reverse;
    index->scan(lo, &hi, forward, nullptr);
    index->rscan(reverse_hi, &lo, reverse, nullptr);
    require(forward.rows.size() == row_count && reverse.rows.size() == row_count,
            "full scan lost or duplicated rows");
    for (unsigned i = 0; i < row_count; ++i) {
        require(forward.rows[i].first == key(i) && forward.rows[i].second == value(i, changed),
                "forward scan order/value mismatch");
        const auto j = row_count - i - 1;
        require(reverse.rows[i].first == key(j) && reverse.rows[i].second == value(j, changed),
                "reverse scan order/value mismatch");
    }
}

struct ScanCallbackFailure {};
struct ScanProbe final : oi_scan_callback {
    Rows observed;
    rusty::Vec<const std::string*> borrowed;
    size_t stop_after = SIZE_MAX;
    bool fail = false;
    bool retain = false;
    // @unsafe - callback ABI requires a nonthrowing destructor.
    ~ScanProbe() noexcept override = default;
    // @unsafe - values are borrowed only when backed by the caller's live arena.
    bool invoke(const char* data, size_t size, const std::string& contents) override {
        observed.invoke(data, size, contents);
        if (retain) borrowed.push(&contents);
        if (observed.rows.size() == stop_after) {
            if (fail) throw ScanCallbackFailure{};
            return false;
        }
        return true;
    }
};

// @unsafe - compares consumer-visible bytes from real forward/reverse scans.
void require_scan_rows(const Rows& actual, const Rows& expected, bool reverse,
                       size_t count) {
    require(actual.rows.size() == count, "scan callback count/early-stop mismatch");
    for (size_t i = 0; i < count; ++i) {
        const auto at = reverse ? expected.rows.size() - i - 1 : i;
        require(actual.rows[i] == expected.rows[at], "scan binary ordering/value mismatch");
    }
}

// @unsafe - inserts temporary real engine rows outside the migrating [m,n) range.
// One value exceeds the unchanged wire page capacity: a local scan cannot pass
// this assertion by silently serializing and consuming transport-sized pages.
void check_direct_scans(FullOrderedIndex* index, unsigned changed) {
    require(TThread::get_shard_index() == 0, "direct scan probe needs owner zero");
    const std::string empty, binary("b\0", 2), successor("b\0\0", 3), high("\xff\0\xff", 3);
    std::string oversized(mako::full_scan_page_capacity + 1024, 'L');
    oversized[0] = '\0';
    oversized.back() = '\xff';
    index->put(lcdf::Str(empty), "empty-key");
    index->put(lcdf::Str(binary), oversized);
    index->put(lcdf::Str(successor), "binary-successor");
    index->put(lcdf::Str(high), "high-key");
    Rows expected;
    expected.rows.push({empty, "empty-key"});
    expected.rows.push({"a", "unmoved-left"});
    expected.rows.push({binary, oversized});
    expected.rows.push({successor, "binary-successor"});
    for (unsigned i = 0; i < row_count; ++i) expected.rows.push({key(i), value(i, changed)});
    expected.rows.push({"z", "unmoved-right"});
    expected.rows.push({high, "high-key"});
    for (bool reverse : {false, true}) {
        str_arena arena;
        ScanProbe complete;
        complete.retain = true;
        if (reverse) index->rscan(high, nullptr, complete, &arena);
        else index->scan(empty, nullptr, complete, &arena);
        require_scan_rows(complete.observed, expected, reverse, expected.rows.size());
        // Reuse engine scratch storage before inspecting the borrowed values.
        Rows churn;
        if (reverse) index->rscan(high, nullptr, churn, nullptr);
        else index->scan(empty, nullptr, churn, nullptr);
        for (size_t i = 0; i < complete.borrowed.size(); ++i)
            require(*complete.borrowed[i] == complete.observed.rows[i].second,
                    "caller arena value changed after scan/transaction completion");
        // First local row, and first row after entering the migrated middle.
        for (size_t stop : {size_t(1), reverse ? size_t(3) : size_t(5)}) {
            for (bool fail : {false, true}) {
                ScanProbe short_scan;
                short_scan.stop_after = stop;
                short_scan.fail = fail;
                bool caught = false;
                try {
                    if (reverse) index->rscan(high, nullptr, short_scan, nullptr);
                    else index->scan(empty, nullptr, short_scan, nullptr);
                } catch (const ScanCallbackFailure&) { caught = true; }
                require(caught == fail, "scan swallowed/replaced callback exception");
                require_scan_rows(short_scan.observed, expected, reverse, stop);
                require(!TThread::txn || !TThread::txn->in_progress(),
                        "scan callback stop/exception left engine transaction live");
            }
        }
    }
    Rows forward_bound, reverse_bound, empty_bound, excluded_empty;
    index->scan(binary, &successor, forward_bound, nullptr);
    index->rscan(successor, &binary, reverse_bound, nullptr);
    index->rscan(empty, nullptr, empty_bound, nullptr);
    index->rscan(empty, &empty, excluded_empty, nullptr);
    require(forward_bound.rows.size() == 1 && forward_bound.rows[0].first == binary
            && forward_bound.rows[0].second == oversized, "forward binary [lo,hi) bounds");
    require(reverse_bound.rows.size() == 1 && reverse_bound.rows[0].first == successor,
            "reverse binary (end,start] bounds");
    require(empty_bound.rows.size() == 1 && empty_bound.rows[0].first.empty()
            && excluded_empty.rows.is_empty(), "reverse empty inclusive/exclusive boundary");
    for (const auto& k : {empty, binary, successor, high})
        require(index->remove(lcdf::Str(k)), "temporary scan row cleanup");
}

// @unsafe - replays a previously captured native segment route through the same
// production engine adapter. No new grant or fake storage response is supplied.
void check_scan_fenced(FullOrderedIndex* index, bool warehouse, MakoShardRoute route) {
    auto* concrete = dynamic_cast<mbta_ordered_index*>(index);
    require(concrete != nullptr, "fenced scan requires real MBTA");
    const std::string lo = "m", hi = "n";
    const auto segment_lo = warehouse ? coordinate(1) : lo;
    const auto segment_hi = warehouse ? coordinate(2) : hi;
    Rows observed;
    bool rejected = false;
    mako::ShardingOperation operation;
    try {
        Sto::start_transaction();
        oi_mbta_scan_delivery delivery{observed, nullptr, true};
        oi_mbta_scan_dispatch dispatch{concrete->mbta, lo, &hi, false, warehouse, delivery};
        const auto status = oi_mbta_scan_dispatch::segment(&dispatch, bytes(segment_lo),
            1, bytes(segment_hi), route);
        if (dispatch.exception) std::rethrow_exception(dispatch.exception);
        require(status == 0, "unexpected captured scan segment status");
        Sto::commit();
    } catch (const abstract_db::abstract_abort_exception&) {
        rejected = true;
        if (TThread::txn && TThread::txn->in_progress()) TThread::txn->silent_abort();
    } catch (...) {
        if (TThread::txn && TThread::txn->in_progress()) TThread::txn->silent_abort();
        throw;
    }
    require(rejected && observed.rows.is_empty(), "fenced scan delivered rows before full admission");
}

// @unsafe - a real mixed routing snapshot must stop before the unavailable RPC
// path. This checks local-to-remote dispatch stopping, not remote enumeration.
void check_stop_before_remote(FullOrderedIndex* index) {
    MakoShardRoute middle{};
    require(TThread::sclient == nullptr
            && mako_sharding_route(raw_id, bytes(key(0)), &middle) == MAKO_SHARD_OK
            && middle.grant.owner == 1, "stop probe requires unavailable remote middle");
    const std::string lo = "a", hi = "z";
    for (bool reverse : {false, true}) {
        for (bool fail : {false, true}) {
            ScanProbe probe;
            probe.stop_after = 1;
            probe.fail = fail;
            bool caught = false;
            try {
                if (reverse) index->rscan(hi, &lo, probe, nullptr);
                else index->scan(lo, &hi, probe, nullptr);
            } catch (const ScanCallbackFailure&) { caught = true; }
            require(caught == fail && probe.observed.rows.size() == 1,
                    "local stop/exception traversed unavailable remote segment");
            require(probe.observed.rows[0].first == (reverse ? hi : lo),
                    "mixed snapshot started from wrong local endpoint");
            require(!TThread::txn || !TThread::txn->in_progress(),
                    "local stop before remote retained engine transaction");
        }
    }
}

struct ConflictScan final : oi_scan_callback {
    mbta_wrapper& db;
    FullOrderedIndex* index;
    unsigned calls = 0;
    // @safe - caller retains DB/index ownership until the joined writer finishes.
    ConflictScan(mbta_wrapper& database, FullOrderedIndex* table) : db(database), index(table) {}
    // @unsafe - callback ABI requires a nonthrowing destructor.
    ~ConflictScan() noexcept override = default;
    // @unsafe - a real independent OCC writer commits after this scan observes
    // the tuple version, before the scan's own terminal validation. Join makes
    // the ordering deterministic and bounds every legacy pointer borrow.
    bool invoke(const char* data, size_t size, const std::string& contents) override {
        require(++calls == 1 && std::string(data, size) == "a"
                && contents == "unmoved-left", "conflict scan initial observation");
        auto* runtime = SiloRuntime::Current();
        std::exception_ptr failure;
        auto writer = rusty::thread::spawn([this, runtime, &failure]() {
            try {
                select_owner(0);
                runtime->BindToCurrentThread();
                db.thread_init(true, 0);
                TThread::in_loading_phase = false;
                try {
                    index->put(lcdf::Str("a"), "concurrent-replacement");
                } catch (...) {
                    db.thread_end();
                    throw;
                }
                db.thread_end();
            } catch (...) { failure = std::current_exception(); }
        });
        writer.join().unwrap();
        if (failure) std::rethrow_exception(failure);
        return false;
    }
};

// @unsafe - public scan must propagate OCC abort, not replay visible callbacks.
void check_scan_conflict(mbta_wrapper& db, FullOrderedIndex* index) {
    ConflictScan callback(db, index);
    const std::string lo = "a", hi = "b";
    bool aborted = false;
    try { index->scan(lo, &hi, callback, nullptr); }
    catch (const abstract_db::abstract_abort_exception&) { aborted = true; }
    require(aborted && callback.calls == 1, "scan OCC conflict was hidden or callbacks replayed");
    require(!TThread::txn || !TThread::txn->in_progress(), "conflicting scan left transaction live");
    std::string actual;
    require(index->get(lcdf::Str(lo), actual, SIZE_MAX) && actual == "concurrent-replacement",
            "conflicting writer did not actually commit");
    index->put(lcdf::Str(lo), "unmoved-left");
}

// @unsafe - drives the production normalized-range kernel because the legacy
// rscan(start,end) API has a finite inclusive start, not an infinity sentinel.
void check_warehouse_all(FullOrderedIndex* index, unsigned changed) {
    const std::string empty, high("\xff\0\xff", 3);
    Rows forward, reverse;
    index->scan(empty, nullptr, forward, nullptr);
    auto* concrete = dynamic_cast<mbta_ordered_index*>(index);
    require(concrete != nullptr, "unbounded scan requires real MBTA");
    {
        mako::ShardingOperation operation;
        try {
            Sto::start_transaction();
            oi_mbta_scan_delivery delivery{reverse, nullptr, true};
            oi_mbta_scan_dispatch dispatch{concrete->mbta, empty, nullptr, true, true, delivery};
            const auto lo = coordinate(1), hi = coordinate(2);
            const auto status = mako_sharding_scan_segments(warehouse_id, bytes(lo),
                1, bytes(hi), 1, oi_mbta_scan_dispatch::segment, &dispatch);
            if (dispatch.exception) std::rethrow_exception(dispatch.exception);
            require(status == MAKO_SHARD_OK, "unbounded reverse segment traversal");
            Sto::commit();
        } catch (...) {
            if (TThread::txn && TThread::txn->in_progress()) TThread::txn->silent_abort();
            throw;
        }
    }
    require(forward.rows.size() == row_count + 2 && reverse.rows.size() == row_count + 2,
            "unbounded scan lost empty or beyond-sentinel key");
    require(forward.rows[0].first.empty() && forward.rows[0].second == "empty-key",
            "unbounded forward empty key");
    require(reverse.rows[0].first == high && reverse.rows[0].second == "high-key",
            "unbounded reverse maximum key");
    require(forward.rows[row_count + 1].first == high
            && reverse.rows[row_count + 1].first.empty(), "unbounded scan endpoints");
    for (unsigned i = 0; i < row_count; ++i) {
        require(forward.rows[i + 1].first == key(i)
                && forward.rows[i + 1].second == value(i, changed), "unbounded forward rows");
        const auto j = row_count - i - 1;
        require(reverse.rows[i + 1].first == key(j)
                && reverse.rows[i + 1].second == value(j, changed), "unbounded reverse rows");
    }
}

// @unsafe - replay through the receiver boundary and the actual engine, never
// treating a successful canonical-to-physical lookup as an admission decision.
void check_received_point(FullOrderedIndex* index, mako::ShardingRequest request,
                          const std::string& point, bool admitted,
                          const std::string* expected = nullptr) {
    static rusty::Cell<uint64_t> sequence{0};
    sequence.set(sequence.get() + 1);
    request.transaction = {UINT64_C(0x80000000cace0000), sequence.get()};
    require(mako::sharding_bind_request(request), "receiver cache probe could not bind");
    bool rejected = false;
    bool found = false;
    std::string actual;
    try {
        const auto table = mako::sharding_request_table(request, 0);
        if (admitted)
            require(table == index->get_table_id(), "canonical receiver selected another physical table");
        found = index->get(lcdf::Str(point), actual, SIZE_MAX);
    } catch (const abstract_db::abstract_abort_exception&) {
        rejected = true;
    } catch (...) {
        mako::sharding_finish_request();
        throw;
    }
    mako::sharding_finish_request();
    require(rejected != admitted, "warm physical mapping substituted for receiver authorization");
    if (admitted)
        require(found == (expected != nullptr) && (!expected || actual == *expected),
                "canonical receiver read another binding's rows");
    require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
            "completed receiver cache probe retained an epoch");
}

// @unsafe - a warm descriptor must enter native admission again after Freeze,
// even though its routing labels and physical engine handle have not changed.
void check_prepared_frozen(FullOrderedIndex* index, unsigned owner,
                           mako::ShardingPreparedRoute& prepared, uint64_t serving_view) {
    require(read_view(owner, warehouse_id, coordinate(1), MAKO_SHARD_RETRY) != serving_view,
            "Frozen metadata reused the Serving cache incarnation");
    // A failed ownership check must not become a successful cached decision in
    // the next transaction, even though the Frozen view and handle are unchanged.
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        bool rejected = false;
        {
            mako::ShardingOperation operation;
            try {
                require(mako::sharding_warehouse_handle(prepared, owner) == index,
                        "Frozen routing labels changed the selected source");
                std::string actual;
                index->get(lcdf::Str(key(0)), actual, SIZE_MAX);
            } catch (const abstract_db::abstract_abort_exception&) { rejected = true; }
        }
        require(rejected, "failed Frozen authorization became a reusable success");
        require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
                "rejected Frozen prepared operation retained an idle pin");
    }
}

// @unsafe - real native context and engine-owned handles; no mocked admission.
void check_operation_resolution(FullOrderedIndex* index, bool warehouse, unsigned owner,
                                mako::ShardingPreparedRoute& prepared) {
    const auto first_key = key(0), second_key = key(1);
    mako::ShardingRequest first{}, second{};
    mako::ShardingPreparedRoute interleaved(warehouse_id, 2);
    MakoShardRoute current{};
    const auto routing_key = warehouse ? coordinate(1) : first_key;
    // The preceding replay command may have published a conservative new view
    // without changing routing labels. Refresh before establishing the stable
    // interval whose cross-epoch reuse this scenario measures.
    require(Transaction::epoch_cut() != 0, "refresh post-control admission view");
    const auto view = read_view(owner, warehouse ? warehouse_id : raw_id, routing_key);
    auto copied = prepared;
    const auto original = [&]() {
        std::string value;
        require(index->get(lcdf::Str(first_key), value, SIZE_MAX), "prepared baseline read");
        return value;
    }();
    require(mako_sharding_route(warehouse ? warehouse_id : raw_id,
                               bytes(routing_key), &current) == MAKO_SHARD_OK,
            "read current routing generation before operation");
    {
        mako::ShardingOperation operation;
        mako::ShardingRequest unstaged{};
        require(mako::sharding_batch_request(index->get_table_id(), first_key, unstaged) < 0,
                "previous transaction's coordinate counted as a staged batch row");
        require(mako::sharding_batch_request(raw[owner]->get_table_id(), key(95), unstaged) < 0,
                "previous raw-key capture survived logical completion");
        if (warehouse)
            require(mako::sharding_warehouse_handle(prepared, owner) == index,
                    "prepared descriptor retained another transaction's owner");
        const auto selected = mako::sharding_resolve_point(index->get_table_id(),
            first_key.data(), first_key.size());
        require(selected.handle() == index && selected.owner() == int(owner), "point selected wrong physical owner");
        const auto transaction = mako::sharding_transaction();
        std::string before;
        require(index->get(lcdf::Str(first_key), before, SIZE_MAX), "captured point engine read");
        // Distinct raw coordinates grow the capture vector even when all
        // warehouse keys share a single fixed-coordinate slot.
        for (unsigned i = 1; i < 96; ++i) {
            const auto k = key(i);
            mako::sharding_resolve_point(raw[owner]->get_table_id(), k.data(), k.size());
        }
        if (warehouse) {
            require(mako::sharding_warehouse_handle(prepared, owner) == index,
                    "capture growth invalidated prepared warehouse slot");
            mako::sharding_warehouse_handle(interleaved, owner);
        }
        // Materialize only after both capture sets have been interleaved; the
        // descriptor must not borrow a vector element or the last resolved row.
        selected.write_request(first);
        require(first.transaction.client == transaction.client
                && first.transaction.sequence == transaction.sequence
                && first.table == (warehouse ? warehouse_id : raw_id)
                && first.fixed_coordinate == warehouse
                && first.coordinate_length == routing_key.size()
                && std::memcmp(first.coordinate, routing_key.data(), routing_key.size()) == 0
                && first.grant.owner == current.grant.owner
                && first.grant.ownership_version == current.grant.ownership_version
                && first.snapshot_version == current.snapshot_version,
                "lazy point materialization lost its original scope, route or identity");
        std::string after;
        require(index->get(lcdf::Str(first_key), after, SIZE_MAX) && after == before,
                "interleaved prepared hint redirected the following storage verb");
        if (warehouse) {
            copied = prepared;
            auto moved = copied;
            auto relocated = std::move(moved);
            require(mako::sharding_warehouse_handle(relocated, owner) == index
                    && mako::sharding_warehouse_handle(prepared, owner) == index,
                    "copied or moved descriptor lost its immutable coordinate");
            require(index->get(lcdf::Str(first_key), after, SIZE_MAX) && after == before,
                    "repeated prepared access lost the captured engine row");
        }
        const auto next = mako::sharding_resolve_point(index->get_table_id(),
            second_key.data(), second_key.size());
        next.write_request(second);
        const auto& second_coordinate = warehouse ? routing_key : second_key;
        require(second.table == first.table && second.fixed_coordinate == warehouse
                && second.coordinate_length == second_coordinate.size()
                && std::memcmp(second.coordinate, second_coordinate.data(), second_coordinate.size()) == 0
                && second.grant.owner == current.grant.owner
                && second.grant.ownership_version == current.grant.ownership_version
                && second.snapshot_version == current.snapshot_version
                && second.transaction.client == transaction.client
                && second.transaction.sequence == transaction.sequence,
                "second lazy point substituted another capture's scope or route");
        mako::ShardingRequest row{};
        require(mako::sharding_batch_request(index->get_table_id(), first_key, row) == int(owner)
                && row.table == first.table && row.fixed_coordinate == first.fixed_coordinate
                && row.coordinate_length == first.coordinate_length
                && std::memcmp(row.coordinate, first.coordinate, row.coordinate_length) == 0
                && row.grant.owner == first.grant.owner
                && row.grant.ownership_version == first.grant.ownership_version
                && row.snapshot_version == first.snapshot_version
                && row.transaction.client == transaction.client
                && row.transaction.sequence == transaction.sequence,
                "batch substituted the final outgoing row for the first capture");
        require(mako::sharding_batch_request(index->get_table_id(), second_key, row) == int(owner)
                && row.table == second.table && row.fixed_coordinate == second.fixed_coordinate
                && row.coordinate_length == second.coordinate_length
                && std::memcmp(row.coordinate, second.coordinate, row.coordinate_length) == 0
                && row.grant.owner == second.grant.owner
                && row.grant.ownership_version == second.grant.ownership_version
                && row.snapshot_version == second.snapshot_version
                && row.transaction.client == transaction.client
                && row.transaction.sequence == transaction.sequence,
                "batch lost the second row capture");
        if (!warehouse)
            require(mako::sharding_batch_request(index->get_table_id(), key(999), row) < 0,
                    "batch performed a fresh route capture for an unresolved row");
    }
    require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
            "cached descriptor retained an idle engine pin");
    const auto cut = Transaction::epoch_cut();
    require(cut && Transaction::epoch_drained(cut),
            "persistent fixed cache prevented an idle engine epoch cut");
    require(read_view(owner, warehouse ? warehouse_id : raw_id, routing_key) == view,
            "unchanged metadata lost its cache incarnation across an engine epoch");
    {
        mako::ShardingOperation operation;
        mako::ShardingRequest unstaged{};
        require(mako::sharding_batch_request(index->get_table_id(), first_key, unstaged) < 0
                && mako::sharding_batch_request(raw[owner]->get_table_id(), key(95), unstaged) < 0,
                "fixed cache reused the preceding transaction's batch captures");
        if (warehouse)
            require(mako::sharding_warehouse_handle(copied, owner) == index
                    && mako::sharding_warehouse_handle(prepared, owner) == index,
                    "cross-transaction descriptor copy selected the wrong handle");
        const auto next = mako::sharding_resolve_point(index->get_table_id(),
            first_key.data(), first_key.size());
        mako::ShardingRequest fresh{};
        next.write_request(fresh);
        require(fresh.transaction.sequence != first.transaction.sequence,
                "terminal finish retained the previous operation identity");
        std::string actual;
        require(index->get(lcdf::Str(first_key), actual, SIZE_MAX),
                "fresh transaction point could not read the engine row");
        const auto encoded = mako::Encode("cross-transaction-prepared-commit");
        Sto::start_transaction();
        index->tx_put(nullptr, lcdf::Str(first_key), encoded);
        require(Sto::try_commit_no_paxos(), "persistent descriptor write could not commit");
        require(Transaction::tinfo[TThread::id()].epoch.load() != 0,
                "engine commit released the enclosing prepared operation pin");
    }
    require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
            "prepared commit retained an idle engine pin");
    std::string committed;
    require(index->get(lcdf::Str(first_key), committed, SIZE_MAX)
            && committed == "cross-transaction-prepared-commit",
            "persistent descriptor lost its actual committed write");
    {
        mako::ShardingOperation operation;
        if (warehouse) {
            mako::sharding_warehouse_handle(interleaved, owner);
            require(mako::sharding_warehouse_handle(copied, owner) == index,
                    "interleaved descriptor copy lost its cross-transaction coordinate");
        }
        const auto encoded = mako::Encode("cross-transaction-prepared-abort");
        Sto::start_transaction();
        index->tx_put(nullptr, lcdf::Str(first_key), encoded);
        Sto::silent_abort();
        require(Transaction::tinfo[TThread::id()].epoch.load() != 0,
                "engine abort released the enclosing prepared operation pin");
    }
    require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
            "prepared abort retained an idle engine pin");
    require(index->get(lcdf::Str(first_key), committed, SIZE_MAX)
            && committed == "cross-transaction-prepared-commit",
            "aborted persistent-descriptor write changed committed storage");
    index->put(lcdf::Str(first_key), original);
    require(read_view(owner, warehouse ? warehouse_id : raw_id, routing_key) == view,
            "logical transaction completion discarded unchanged cached metadata");
    // Receiver requests never use the sender's coordinate-only capture cache.
    static rusty::Cell<uint64_t> incoming_sequence{0};
    incoming_sequence.set(incoming_sequence.get() + 1);
    first.transaction = {UINT64_C(0x80000000abcd0000) + owner, incoming_sequence.get()};
    second.transaction = first.transaction;
    require(mako::sharding_bind_request(first), "bind captured receiver request");
    require(mako::sharding_request_table(first, 0) == index->get_table_id(),
            "receiver did not select this owner's physical table");
    auto* other = warehouse ? physical_warehouse[1 - owner][0] : raw[1 - owner];
    if (other) {
        bool rejected = false;
        try {
            std::string actual;
            other->get(lcdf::Str(first_key), actual, SIZE_MAX);
        } catch (const abstract_db::abstract_abort_exception&) { rejected = true; }
        require(rejected, "receiver admitted another participant's physical table");
    }
    if (!warehouse) {
        bool rejected = false;
        try {
            std::string actual;
            index->get(lcdf::Str(second_key), actual, SIZE_MAX);
        } catch (const abstract_db::abstract_abort_exception&) { rejected = true; }
        require(rejected, "incoming coordinate did not bind the actual point key");
    }
    std::string received_value;
    require(index->get(lcdf::Str(first_key), received_value, SIZE_MAX),
            "valid received request could not read the engine row");
    second.snapshot_version = MAKO_SHARD_NO_SNAPSHOT;
    mako::sharding_set_request(second);
    require(index->get(lcdf::Str(second_key), received_value, SIZE_MAX),
            "valid received second coordinate could not read the engine row");
    auto changed = second;
    ++changed.grant.ownership_version;
    mako::sharding_set_request(changed);
    bool stale_rejected = false;
    try {
        mako::sharding_resolve_point(index->get_table_id(),
            second_key.data(), second_key.size());
    } catch (const abstract_db::abstract_abort_exception&) { stale_rejected = true; }
    require(stale_rejected, "resolution authorized a changed transported grant");
    bool engine_rejected = false;
    try { index->get(lcdf::Str(second_key), received_value, SIZE_MAX); }
    catch (const abstract_db::abstract_abort_exception&) { engine_rejected = true; }
    require(engine_rejected, "cached physical handle authorized a changed grant");
    mako::sharding_finish_request();
    // New identities reuse the same canonical binding, while each transaction
    // alternates raw, fixed warehouse-one and fixed warehouse-two scopes. The
    // sentinels differ (including a genuinely absent row), so a wrong physical
    // address cannot pass merely because the migrating tables have equal rows.
    for (unsigned round = 0; round < 3; ++round) {
        check_received_point(index, first, first_key, true, &original);
        for (unsigned scope = 0; scope < 3; ++scope) {
            auto scoped = first;
            scoped.table = scope == 0 ? raw_id : warehouse_id;
            scoped.fixed_coordinate = scope != 0;
            const auto coord = scope == 0 ? std::string("a") : coordinate(scope);
            MakoShardRoute route{};
            require(mako_sharding_route(scoped.table, bytes(coord), &route) == MAKO_SHARD_OK,
                    "interleaved receiver scope routing");
            if (route.grant.owner != owner) continue;
            scoped.grant = route.grant;
            scoped.snapshot_version = route.snapshot_version;
            scoped.coordinate_length = coord.size();
            std::memcpy(scoped.coordinate, coord.data(), coord.size());
            auto* scoped_index = scope == 0 ? raw[owner] : physical_warehouse[owner][scope - 1];
            const auto scoped_key = scope == 0 ? std::string("a") :
                                    scope == 1 ? std::string() : first_key;
            const std::string sentinel = scope == 0 ? "unmoved-left" : "empty-key";
            check_received_point(scoped_index, scoped, scoped_key, true,
                                 scope == 2 ? nullptr : &sentinel);
            check_received_point(index, first, first_key, true, &original);
        }
        auto wrong_owner = first;
        wrong_owner.grant.owner = 1 - owner;
        check_received_point(index, wrong_owner, first_key, false);
        auto wrong_coordinate = first;
        wrong_coordinate.coordinate[wrong_coordinate.coordinate_length - 1] ^= 1;
        check_received_point(index, wrong_coordinate, first_key, false);
        check_received_point(index, first, first_key, true, &original);
    }
}

} // namespace

// @unsafe - dedicated real-engine fixture bootstrap, lifetime and stdin kernel.
int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: native_sharding_smoke CONFIG 0|1|both");
        const bool both = std::string(argv[2]) == "both";
        const unsigned first = both ? 0 : static_cast<unsigned>(std::stoul(argv[2]));
        require(first < 2, "invalid node");
        transport::Configuration config(argv[1]);
        config.multi_shard_mode = both;
        if (both) config.local_shard_indices = {0, 1};
        auto& bench = BenchmarkConfig::getInstance();
        bench.setConfig(&config);
        bench.setNshards(2);
        bench.setNthreads(1);
        bench.setShardIndex(first);
        bench.setIsReplicated(0);
        bench.setIsMicro(0);
        bench.setPaxosProcName("localhost");
        bench.setPinCpus(0);
        SiloRuntime::Current()->BindToCurrentThread();
        mbta_wrapper db;
        db.init();
        db.thread_init(true, 0);
        require(mako_sharding_catalog_tpcc(0) == MAKO_SHARD_OK, "TPC-C catalog init");
        require(mako_sharding_table_id(bytes("warehouse"), &warehouse_id) == MAKO_SHARD_OK,
                "canonical warehouse identity");
        require(mako_sharding_catalog_register(raw_id, bytes("smoke_raw"), 0, 0) == MAKO_SHARD_OK,
                "raw catalog init");
        require(mako_sharding_warehouse_init(1, 2) == MAKO_SHARD_OK, "warehouse dimensions");
        const unsigned limit = both ? 2 : first + 1;
        for (unsigned owner = first; owner < limit; ++owner) {
            select_owner(owner);
            raw[owner] = db.open_index("smoke_raw", int(owner));
            if (owner == first)
                require(mako::get_table_registry().bind_native("smoke_raw", raw_id, rusty::None),
                        "raw physical binding");
            require(mako_sharding_raw_register(raw_id, owner, 0,
                    reinterpret_cast<uintptr_t>(raw[owner])) == MAKO_SHARD_OK, "raw handle registration");
            for (unsigned wh = 1; wh <= 2; ++wh) {
                const auto name = "smoke_warehouse_" + std::to_string(wh);
                physical_warehouse[owner][wh - 1] = db.open_index(name, int(owner));
                if (owner == first)
                    require(mako::get_table_registry().bind_native(name, warehouse_id,
                            rusty::Some(coordinate(wh))), "warehouse physical binding");
                require(mako_sharding_warehouse_register(warehouse_id, wh, owner, 0,
                        reinterpret_cast<uintptr_t>(physical_warehouse[owner][wh - 1])) == MAKO_SHARD_OK,
                        "warehouse handle registration");
            }
            require(mako_sharding_warehouse_opener(owner, &db, open_warehouse) == MAKO_SHARD_OK,
                    "warehouse opener registration");
            if (owner == 0) {
                for (unsigned i = 0; i < row_count; ++i) {
                    const auto k = key(i), v = value(i, 0);
                    raw[owner]->put(lcdf::Str(k), v);
                    physical_warehouse[owner][0]->put(lcdf::Str(k), v);
                }
                raw[owner]->put(lcdf::Str("a"), "unmoved-left");
                raw[owner]->put(lcdf::Str("z"), "unmoved-right");
                physical_warehouse[owner][0]->put(lcdf::Str("", 0), "empty-key");
                const std::string high("\xff\0\xff", 3);
                physical_warehouse[owner][0]->put(lcdf::Str(high), "high-key");
            } else {
                raw[owner]->put(lcdf::Str("m9999"), "stale-destination-only");
                physical_warehouse[owner][0]->put(lcdf::Str("m9999"), "stale-destination-only");
            }
        }
        TThread::in_loading_phase = false;
        const auto descriptor_epoch = Transaction::tinfo[TThread::id()].epoch.load();
        mako::ShardingPreparedRoute prepared(warehouse_id, 1);
        require(Transaction::tinfo[TThread::id()].epoch.load() == descriptor_epoch,
                "constructing a workload descriptor announced an engine epoch");
        Hosts hosts;
        for (unsigned owner = first; owner < limit; ++owner) {
            auto host = rusty::make_box<mako::NativeShardingHost>(&db, owner, 2, SiloRuntime::Current());
            auto callbacks = host->callbacks();
            require(callbacks.peer_call == mako::native_sharding_peer_call, "unexpected production RPC callback");
            callbacks.peer_call = fault_peer_call;
            require(mako_sharding_start_node(owner, 2, 1, &callbacks) == MAKO_SHARD_OK, "start native node");
            const auto listening = host->start_service();
            if (listening != MAKO_SHARD_OK) {
                mako_sharding_stop_node(owner);
                require(false, "start actual srpc listener");
            }
            hosts.items.push(std::move(host));
        }
        ReleasePrecopyOnExit release_precopy_on_exit;
        rusty::Option<rusty::Box<FixtureWorker>> workers[2];
        for (unsigned owner = first; owner < limit; ++owner)
            workers[owner] = rusty::Some(rusty::make_box<FixtureWorker>(*hosts.items[owner - first]));
        std::cout << "SMOKE READY" << std::endl;
        std::string line;
        MakoShardRoute remembered[2]{};
        uint64_t remembered_table[2]{};
        uint64_t remembered_view[2]{};
        MakoShardRoute unmoved_route{};
        bool unmoved_remembered = false;
        while (std::getline(std::cin, line)) {
            std::istringstream input(line);
            std::string command, table;
            unsigned owner = 0, changed = 0;
            input >> command;
            if (command == "stop") break;
            input >> owner >> table;
            require(bool(input), "malformed fixture command");
            if (command == "ready") {
                for (unsigned attempt = 0; attempt < 200 && !mako_sharding_ready(owner); ++attempt)
                    usleep(50000);
                require(mako_sharding_ready(owner), "all-peer loader/catalog barrier did not open");
                std::cout << "SMOKE OK" << std::endl;
                continue;
            }
            if (command == "unready") {
                require(!mako_sharding_ready(owner), "missing peer incorrectly passed bootstrap");
                EpochRead read;
                require(read.acquire(owner) == MAKO_SHARD_RETRY,
                        "ordinary context admitted while another owner is still loading");
                std::cout << "SMOKE OK" << std::endl;
                continue;
            }
            if (command == "reconnect") {
                require(owner >= first && owner < limit, "reconnect targets nonlocal owner");
                check_peer_reconnect(*hosts.items[owner - first]);
                std::cout << "SMOKE OK" << std::endl;
                continue;
            }
            const uint64_t id = table == "raw" ? raw_id : warehouse_id;
            const auto point = table == "raw" ? key(0) : coordinate(1);
            if (command == "metadata-fenced") {
                // @unsafe - probes real native entrypoints; fixture genesis rows
                // are not recovered data and must never make this owner ready.
                require(owner >= first && owner < limit, "fence probe targets nonlocal owner");
                require(!mako_sharding_ready(owner), "retained metadata became data-ready");
                require(mako_sharding_wait_ready(owner) == MAKO_SHARD_RETRY,
                        "retained metadata wait_ready did not fail closed");
                EpochRead read;
                require(read.acquire(owner) == MAKO_SHARD_RETRY,
                        "retained metadata admitted an immutable context");
                uint64_t missing_view = 0;
                require(mako_sharding_read_view_id(nullptr, &missing_view) != MAKO_SHARD_OK,
                        "missing pre-death context exposed a view");
                std::cout << "SMOKE OK metadata retained; rows not recovered; all access fenced" << std::endl;
                continue;
            }
            if (command == "fault-arm" || command == "capture") {
                begin_capture(owner, command == "fault-arm");
                std::cout << "SMOKE OK" << std::endl;
                continue;
            }
            if (command == "capture-stop" || command == "fault-assert") {
                auto state = faults.lock().unwrap();
                state->capture = false;
                if (command == "fault-assert")
                    require(state->drops == 1 && state->successful_retries >= 1 && !state->armed,
                            "real source Commit response was not dropped and successfully retried");
                std::cout << "SMOKE OK drops=" << state->drops
                          << " retries=" << state->successful_retries << std::endl;
                continue;
            }
            if (command == "replay" || command == "replay-abort") {
                replay_controls(command == "replay-abort");
                std::cout << "SMOKE OK" << std::endl;
                continue;
            }
            if (command == "replay-target-fenced") {
                unsigned target = 0;
                input >> target;
                require(bool(input) && target < 2, "invalid fenced replay target");
                replay_controls(false, int(target));
                std::cout << "SMOKE OK captured Start/Final/Commit rejected by retained target" << std::endl;
                continue;
            }
            if (command == "route") {
                uint64_t minimum = 0;
                input >> minimum;
                MakoShardRoute route{};
                bool matched = false;
                for (unsigned attempt = 0; attempt < 200; ++attempt) {
                    EpochRead read;
                    require(read.acquire(owner) == MAKO_SHARD_OK, "route read begin");
                    const auto status = mako_sharding_read_route(read.value, id, bytes(point), &route);
                    if (status == MAKO_SHARD_OK && route.grant.owner == owner
                        && route.grant.ownership_version >= minimum
                        && mako_sharding_read_point(read.value, id, bytes(point), route) == MAKO_SHARD_OK) {
                        matched = true;
                        break;
                    }
                    usleep(50000);
                }
                require(matched, "captured route/Serving authority did not become available");
                std::cout << "SMOKE OK " << route.grant.ownership_version << std::endl;
                continue;
            }
            require(owner >= first && owner < limit, "command targets nonlocal owner");
            select_owner(owner);
            auto* index = table == "raw" ? raw[owner] : physical_warehouse[owner][0];
            if (command == "epoch-ticks") {
                check_periodic_epoch_commit(index);
                std::cout << "SMOKE OK timer ticks preserve old commit and cleanup pin" << std::endl;
                continue;
            }
            if (command == "epoch-idle") {
                auto& worker = *workers[owner].as_mut().unwrap();
                worker.run([&]() {
                    require(!mako::sharding_has_request(), "idle helper unexpectedly owns request");
                    TThread::set_mode(1);
                    Sto::start_transaction();
                    const auto cut = Transaction::epoch_cut();
                    require(cut && !TThread::txn->has_epoch() && Transaction::epoch_drained(cut),
                            "idle mode-1 reset blocked a process cut");
                    TThread::txn->silent_abort();
                    TThread::set_mode(0);
                });
                std::cout << "SMOKE OK idle helper leaves cut drained" << std::endl;
                continue;
            }
            if (command == "epoch-shared") {
                require(both, "shared pin slots require colocated owners");
                auto& left = *workers[0].as_mut().unwrap();
                auto& right = *workers[1].as_mut().unwrap();
                require(left.slot != right.slot && left.slot != TThread::id()
                        && right.slot != TThread::id(), "physical workers share a STO slot");
                uint64_t left_view = 0, right_view = 0;
                left.run([&]() {
                    require(left.read.acquire(0) == MAKO_SHARD_OK, "owner zero read begin");
                    require(mako_sharding_read_view_id(left.read.value, &left_view) == MAKO_SHARD_OK
                            && left_view != 0, "owner zero protected cache identity");
                    const MakoShardRead* nested = nullptr;
                    require(mako_sharding_read_begin(0, left.read.epoch, &nested) == MAKO_SHARD_BUSY
                            && nested == nullptr, "second context overwrote active worker TLS");
                });
                right.run([&]() {
                    require(right.read.acquire(1) == MAKO_SHARD_OK, "owner one read begin");
                    require(mako_sharding_read_view_id(right.read.value, &right_view) == MAKO_SHARD_OK
                            && right_view != 0, "owner one protected cache identity");
                    uint64_t foreign_view = 0;
                    require(mako_sharding_read_view_id(left.read.value, &foreign_view) != MAKO_SHARD_OK,
                            "foreign worker read handle exposed a view");
                });
                require(left_view != right_view, "independent worker caches share an incarnation");
                const auto zero = hosts.items[0]->callbacks();
                const auto one = hosts.items[1]->callbacks();
                const auto cut = zero.epoch_cut(hosts.items[0].get());
                require(cut && zero.epoch_drained(hosts.items[0].get(), cut) == MAKO_SHARD_BUSY
                        && one.epoch_drained(hosts.items[1].get(), cut) == MAKO_SHARD_BUSY,
                        "owner callbacks did not see process-wide pins");
                left.run([&]() { left.read.release(); });
                require(zero.epoch_drained(hosts.items[0].get(), cut) == MAKO_SHARD_BUSY,
                        "owner-zero cut ignored owner-one's older slot");
                right.run([&]() { right.read.release(); });
                require(zero.epoch_drained(hosts.items[0].get(), cut) == MAKO_SHARD_OK
                        && one.epoch_drained(hosts.items[1].get(), cut) == MAKO_SHARD_OK,
                        "released colocated worker left cut blocked");
                const auto before_switch = read_view(0, raw_id, "a");
                const auto switched = read_view(1, raw_id, "a", MAKO_SHARD_RETRY);
                const auto returned = read_view(0, raw_id, "a");
                require(before_switch != switched && switched != returned && before_switch != returned,
                        "owner cache A-B-A switch resurrected an old incarnation");
                std::cout << "SMOKE OK colocated owners share all process pin slots" << std::endl;
                continue;
            }
            if (command == "precopy-arm") {
                if (table == "warehouse") {
                    auto& worker = *workers[owner].as_mut().unwrap();
                    worker.run([&]() {
                        require(worker.operation.is_none(), "prepared precopy context already held");
                        require(mako::sharding_warehouse_handle(worker.prepared, owner) == index
                                && Transaction::tinfo[TThread::id()].epoch.load() == 0,
                                "idle prepared lookup pinned or redirected the source");
                        {
                            mako::ShardingOperation warmup;
                            require(mako::sharding_warehouse_handle(worker.prepared, owner) == index,
                                    "precopy warmup selected the wrong source");
                            std::string actual;
                            require(index->get(lcdf::Str(key(0)), actual, SIZE_MAX),
                                    "precopy warmup did not read the real source");
                        }
                        require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
                                "precopy cached descriptor retained an idle pin");
                        worker.operation = rusty::Some(rusty::make_box<mako::ShardingOperation>());
                        mako::ShardingRequest unstaged{};
                        require(mako::sharding_batch_request(index->get_table_id(), key(0), unstaged) < 0,
                                "precopy reused a batch coordinate from its completed warmup");
                        Sto::start_transaction();
                        require(mako::sharding_warehouse_handle(worker.prepared, owner) == index,
                                "precopy prepared access selected another owner");
                        std::string actual;
                        require(index->tx_get(nullptr, lcdf::Str(key(0)), actual, SIZE_MAX),
                                "prepared precopy source engine read");
                        worker.cut = Transaction::epoch_cut();
                        require(worker.cut && !Transaction::epoch_drained(worker.cut),
                                "prepared precopy read did not retain its logical epoch");
                    });
                }
                auto state = precopy.lock().unwrap();
                require(!state->armed && (!state->complete || state->released), "precopy barrier occupied");
                *state = {};
                state->armed = true;
                std::cout << "SMOKE OK" << std::endl;
                continue;
            }
            if (command == "precopy-wait") {
                auto state = precopy.lock().unwrap();
                state = precopy_changed.wait_while(std::move(state), [](PrecopyBarrier& value) {
                    return !value.complete;
                }).unwrap();
                require(state->status == MAKO_SHARD_OK && !state->released,
                        "background precopy did not complete successfully");
                std::cout << "SMOKE OK actual background copy complete; Freeze not yet issued" << std::endl;
                continue;
            }
            if (command == "precopy-release") {
                {
                    auto state = precopy.lock().unwrap();
                    require(state->complete && !state->released, "precopy barrier not held");
                    state->released = true;
                    precopy_changed.notify_all();
                }
                if (table == "warehouse") {
                    // The ordinary driver waits for commit next. Keep its real
                    // source read pinned until Freeze and the OCC retry below
                    // have exercised the prepared slot, then let commit proceed.
                    bool frozen = false;
                    for (unsigned attempt = 0; attempt < 200; ++attempt) {
                        EpochRead fresh;
                        require(fresh.acquire(owner) == MAKO_SHARD_OK, "prepared Freeze probe begin");
                        const auto status = mako_sharding_read_point(
                            fresh.value, id, bytes(point), remembered[owner]);
                        if (status == MAKO_SHARD_RETRY) {
                            frozen = true;
                            break;
                        }
                        require(status == MAKO_SHARD_OK, "prepared Freeze probe failed");
                        fresh.release();
                        usleep(50000);
                    }
                    require(frozen, "warehouse never reached Frozen with prepared view held");
                    check_prepared_frozen(index, owner, prepared, remembered_view[owner]);
                    auto& worker = *workers[owner].as_mut().unwrap();
                    worker.run([&]() {
                        require(worker.operation.is_some() && worker.conflict,
                                "prepared Frozen retry lost its real conflicting reader");
                        require(!Sto::try_commit_no_paxos(),
                                "prepared source read missed the real OCC version conflict");
                        require(!Transaction::epoch_drained(worker.cut),
                                "prepared OCC abort released the logical migration pin");
                        Sto::start_transaction();
                        require(mako::sharding_warehouse_handle(worker.prepared, owner) == index,
                                "prepared OCC retry discarded its captured Frozen owner");
                        mako::sharding_warehouse_handle(worker.interleaved, owner);
                        std::string actual;
                        require(index->tx_get(nullptr, lcdf::Str(key(1)), actual, SIZE_MAX)
                                && actual == value(1, 1),
                                "interleaved Frozen hint redirected the first retry engine read");
                        require(mako::sharding_warehouse_handle(worker.prepared, owner) == index,
                                "repeated prepared Frozen access changed the source handle");
                        require(index->tx_get(nullptr, lcdf::Str(key(1)), actual, SIZE_MAX)
                                && actual == value(1, 1),
                                "repeated prepared Frozen storage access lost the current tuple");
                        require(Sto::try_commit_no_paxos(), "prepared Frozen retry could not commit");
                        require(!Transaction::epoch_drained(worker.cut),
                                "prepared engine completion released its logical migration pin");
                        worker.operation = rusty::None;
                        worker.conflict = false;
                        require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
                                "prepared terminal cleanup leaked its source epoch");
                    });
                }
                std::cout << "SMOKE OK" << std::endl;
                continue;
            }
            if (command == "precopy-private") {
                check_private_precopy(*workers[owner].as_mut().unwrap(),
                    *hosts.items[owner - first], id, table == "warehouse");
                std::cout << "SMOKE OK real precopy bytes remain private" << std::endl;
                continue;
            }
            if (command == "precopy-mutate") {
                require(index->insert(lcdf::Str("m7777"), "inserted-after-precopy"), "live precopy insert");
                index->put(lcdf::Str(key(1)), value(1, 1));
                require(index->remove(lcdf::Str(key(2))), "live precopy delete");
                if (table == "warehouse") {
                    // Two committed versions force the retained engine reader
                    // to abort without changing the driver's expected final rows.
                    const auto k = key(0);
                    std::string before;
                    require(index->get(lcdf::Str(k), before, SIZE_MAX),
                            "prepared conflict source read");
                    index->put(lcdf::Str(k), "prepared-occ-conflict");
                    index->put(lcdf::Str(k), before);
                    auto& worker = *workers[owner].as_mut().unwrap();
                    worker.run([&]() { worker.conflict = true; });
                }
                std::cout << "SMOKE OK source insert update delete committed after precopy" << std::endl;
                continue;
            }
            if (command == "precopy-final") {
                std::string actual;
                require(index->get(lcdf::Str("m7777"), actual, SIZE_MAX)
                            && actual == "inserted-after-precopy", "final missed post-precopy insert");
                require(index->get(lcdf::Str(key(1)), actual, SIZE_MAX)
                            && actual == value(1, 1), "final missed post-precopy update");
                require(!index->get(lcdf::Str(key(2)), actual, SIZE_MAX),
                        "final retained deleted source row from private precopy");
                require(!index->get(lcdf::Str("m9999"), actual, SIZE_MAX),
                        "final retained destination-only stale row");
                // Restore the preexisting suite's exact data set after assertions.
                require(index->remove(lcdf::Str("m7777")), "final insert cleanup");
                index->put(lcdf::Str(key(1)), value(1, 0));
                require(index->insert(lcdf::Str(key(2)), value(2, 0)), "final delete cleanup");
                std::cout << "SMOKE OK final exact insert update delete destination-only reconciliation" << std::endl;
                continue;
            }
            if (command == "precopy-replay") {
                IssuedControl request;
                {
                    auto state = precopy.lock().unwrap();
                    require(state->complete && state->released, "no completed precopy to replay");
                    request = state->request;
                }
                CapturedReply reply;
                require(mako::native_sharding_peer_call(request.host, request.target,
                            request.operation, bytes(request.payload), CapturedReply::receive, &reply)
                            == MAKO_SHARD_OK && reply.calls == 1
                            && (reply.status == MAKO_SHARD_RETRY || reply.status == MAKO_SHARD_INVALID),
                        "late precopy crossed terminal/generation fence");
                std::cout << "SMOKE OK real late precopy job rejected" << std::endl;
                continue;
            }
            if (command == "gateway-forged") {
                require(owner == 0 && table == "raw", "gateway probe scope");
                MakoShardRoute route{};
                require(mako_sharding_route(raw_id, bytes("a"), &route) == MAKO_SHARD_OK
                        && route.grant.owner == owner && route.grant.ownership_version == 0
                        && route.snapshot_version > 0, "gateway probe needs changed snapshot");
                const auto physical = mako::get_table_registry().get_table_id("smoke_raw");
                require(physical.is_some(), "gateway physical table absent");
                mako::ShardReceiver receiver(argv[1]);
                // @unsafe - Register's legacy map signature is the engine boundary.
                receiver.Register(&db, {{physical.unwrap(), raw[owner]}});
                MakoGatewayRequest request{};
                request.version = MAKO_GATEWAY_VERSION;
                request.kind = MAKO_GATEWAY_GET;
                request.client = UINT64_C(0x80000000cafe0000) + owner;
                request.sequence = 1;
                request.table = raw_id;
                request.physical_table = physical.unwrap();
                request.owner = owner;
                request.ownership_version = 1; // wrong, but <= current snapshot
                request.snapshot_version = route.snapshot_version;
                request.route_known = 1;
                request.key[0] = 'a'; request.key_length = 1;
                request.coordinate[0] = 'a'; request.coordinate_length = 1;
                MakoGatewayResponse response{};
                receiver.ClientOperation(request, response);
                require(response.status != mako::ErrorCode::SUCCESS
                        && response.outcome == MAKO_GATEWAY_ABORTED,
                        "public current-version claim bypassed ownership validation");
                request.sequence = 2;
                request.ownership_version = route.grant.ownership_version;
                receiver.ClientOperation(request, response);
                const std::string expected = "unmoved-left";
                require(response.status == mako::ErrorCode::SUCCESS
                        && response.value_length == expected.size()
                        && std::memcmp(response.value, expected.data(), expected.size()) == 0
                        && response.snapshot_version == request.snapshot_version,
                        "legitimate gateway request/reply lost captured routing identity");
                std::cout << "SMOKE OK untrusted snapshot claim rejected; exact gateway read preserved" << std::endl;
                continue;
            }
            if (command == "remember") {
                require(mako_sharding_route(id, bytes(point), &remembered[owner]) == MAKO_SHARD_OK
                        && remembered[owner].grant.owner == owner, "remember actual routed request");
                remembered_table[owner] = id;
                remembered_view[owner] = read_view(owner, id, point);
                {
                    mako::ShardingOperation operation;
                    if (table == "warehouse")
                        require(mako::sharding_warehouse_handle(prepared, owner) == index,
                                "remember did not warm the actual prepared source handle");
                    std::string actual;
                    require(index->get(lcdf::Str(key(0)), actual, SIZE_MAX),
                            "remember could not read the source row");
                }
                mako::ShardingRequest request{};
                request.table = id;
                request.fixed_coordinate = table == "warehouse";
                request.coordinate_length = point.size();
                std::memcpy(request.coordinate, point.data(), point.size());
                request.grant = remembered[owner].grant;
                request.snapshot_version = remembered[owner].snapshot_version;
                std::string actual;
                require(index->get(lcdf::Str(key(0)), actual, SIZE_MAX), "remember receiver baseline");
                check_received_point(index, request, key(0), true, &actual);
                std::cout << "SMOKE OK" << std::endl;
                continue;
            }
            if (command == "remember-unmoved") {
                require(owner == 0 && table == "raw", "unmoved probe scope");
                require(mako_sharding_route(raw_id, bytes("a"), &unmoved_route) == MAKO_SHARD_OK
                        && unmoved_route.grant.owner == owner, "remember unaffected request");
                unmoved_remembered = true;
                std::cout << "SMOKE OK" << std::endl;
                continue;
            }
            if (command == "probe-unmoved") {
                MakoShardRoute current{};
                require(owner == 0 && table == "raw" && unmoved_remembered, "unmoved request absent");
                require(mako_sharding_route(raw_id, bytes("a"), &current) == MAKO_SHARD_OK
                        && current.snapshot_version > unmoved_route.snapshot_version
                        && current.grant.owner == unmoved_route.grant.owner
                        && current.grant.ownership_version == unmoved_route.grant.ownership_version,
                        "probe did not retain an older snapshot for unchanged ownership");
                EpochRead read;
                require(read.acquire(owner) == MAKO_SHARD_OK, "fallback begin");
                require(mako_sharding_read_point(read.value, raw_id, bytes("a"), unmoved_route)
                        == MAKO_SHARD_OK, "older snapshot rejected unchanged ownership");
                std::cout << "SMOKE OK old snapshot fallback" << std::endl;
                continue;
            }
            if (command == "probe-stale" || command == "probe-frozen") {
                require(remembered_table[owner] == id, "no captured request for probe");
                bool refused = false;
                for (unsigned attempt = 0; attempt < 200; ++attempt) {
                    EpochRead read;
                    require(read.acquire(owner) == MAKO_SHARD_OK, "captured probe begin");
                    const auto status = mako_sharding_read_point(read.value, id, bytes(point), remembered[owner]);
                    if (status == MAKO_SHARD_RETRY) {
                        const auto lo = table == "raw" ? std::string("m") : coordinate(1);
                        const auto hi = table == "raw" ? std::string("n") : coordinate(2);
                        require(mako_sharding_read_range(read.value, id, bytes(lo), 1, bytes(hi),
                                    remembered[owner]) == MAKO_SHARD_RETRY,
                                "fresh old-grant range entered after source cut");
                        refused = true;
                        break;
                    }
                    require(status == MAKO_SHARD_OK && command == "probe-frozen",
                            "stale captured request admitted or unexpected admission failure");
                    usleep(50000);
                }
                require(refused, "frozen range kept admitting matching-snapshot requests");
                mako::ShardingRequest stale{};
                stale.table = id;
                stale.fixed_coordinate = table == "warehouse";
                stale.coordinate_length = point.size();
                std::memcpy(stale.coordinate, point.data(), point.size());
                stale.grant = remembered[owner].grant;
                stale.snapshot_version = remembered[owner].snapshot_version;
                for (unsigned attempt = 0; attempt < 2; ++attempt)
                    check_received_point(index, stale, key(0), false);
                if (command == "probe-frozen") {
                    MakoShardRoute current{};
                    require(mako_sharding_route(id, bytes(point), &current) == MAKO_SHARD_OK
                            && current.snapshot_version == remembered[owner].snapshot_version
                            && current.grant.owner == remembered[owner].grant.owner
                            && current.grant.ownership_version == remembered[owner].grant.ownership_version,
                            "freeze probe did not exercise matching snapshot/ownership versions");
                    require(read_view(owner, id, point, MAKO_SHARD_RETRY) != remembered_view[owner],
                            "Frozen metadata reused the Serving cache incarnation");
                    if (table == "warehouse")
                        check_prepared_frozen(index, owner, prepared, remembered_view[owner]);
                    auto& worker = *workers[owner].as_mut().unwrap();
                    worker.run([&]() {
                        require(worker.operation.is_some(), "missing retained logical context");
                        require(!Transaction::epoch_drained(worker.cut), "old context lost its epoch");
                        if (worker.conflict) {
                            require(!Sto::try_commit_no_paxos(), "forced OCC conflict unexpectedly committed");
                            require(!Transaction::epoch_drained(worker.cut),
                                    "OCC abort/cleanup released logical epoch");
                            Sto::start_transaction();
                            worker.conflict = false;
                        }
                        // The captured view covers the whole partition, not just
                        // the point touched before Freeze. This is a real first
                        // access to key(1) through the production MBTA bridge.
                        std::string actual;
                        if (table == "warehouse") {
                            require(mako::sharding_warehouse_handle(worker.prepared, owner) == index,
                                    "Frozen or OCC retry revoked the prepared old-view owner");
                            mako::sharding_warehouse_handle(worker.interleaved, owner);
                        }
                        require(index->tx_get(nullptr, lcdf::Str(key(1)), actual, SIZE_MAX),
                                "Freeze revoked a newly accessed old-view coordinate");
                        if (table == "warehouse") {
                            const auto before = actual;
                            require(mako::sharding_warehouse_handle(worker.prepared, owner) == index,
                                    "repeated Frozen prepared access changed physical owner");
                            require(index->tx_get(nullptr, lcdf::Str(key(1)), actual, SIZE_MAX)
                                    && actual == before,
                                    "repeated Frozen prepared storage verb changed engine row");
                        }
                        const auto lo = table == "raw" ? std::string("m") : coordinate(1);
                        const auto hi = table == "raw" ? std::string("n") : coordinate(2);
                        mako::sharding_require_interval(index->get_table_id(),
                            bytes(lo), true, bytes(hi), remembered[owner]);
                        require(Sto::try_commit_no_paxos(), "old view could not commit after Freeze");
                        require(!Transaction::epoch_drained(worker.cut),
                                "engine completion released retained logical context");
                    });
                }
                check_scan_fenced(index, table == "warehouse", remembered[owner]);
                if (command == "probe-frozen") {
                    Rows observed;
                    const std::string lo = "m", hi = "n";
                    bool rejected = false;
                    try { index->scan(lo, &hi, observed, nullptr); }
                    catch (const abstract_db::abstract_abort_exception&) { rejected = true; }
                    require(rejected && observed.rows.is_empty(),
                            "public Frozen scan delivered rows before admission");
                }
                std::cout << "SMOKE OK scan " << command
                          << " rejected before callbacks" << std::endl;
                continue;
            }
            if (command == "direct-scan") {
                input >> changed;
                require(table == "raw" && owner == 0 && bool(input), "direct scan command scope");
                check_direct_scans(index, changed);
                check_scan_conflict(db, index);
                std::cout << "SMOKE OK direct local scan oversized binary arena stop exception OCC conflict" << std::endl;
                continue;
            }
            if (command == "scan-stop-before-remote") {
                require(table == "raw" && owner == 0, "mixed stop command scope");
                check_stop_before_remote(index);
                std::cout << "SMOKE OK local stop before unavailable remote segment" << std::endl;
                continue;
            }
            if (command == "check") {
                input >> changed;
                check_operation_resolution(index, table == "warehouse", owner, prepared);
                check_rows(index, changed);
                if (table == "warehouse") check_warehouse_all(index, changed);
            } else if (command == "write") {
                const auto k = key(1), v = value(1, 1);
                index->put(lcdf::Str(k), v);
                std::string actual;
                require(index->get(lcdf::Str(k), actual, SIZE_MAX) && actual == v, "post-migration write");
                require(index->insert(lcdf::Str("m8888"), "temporary"), "new insert");
                require(index->remove(lcdf::Str("m8888")), "delete inserted row");
                require(!index->get(lcdf::Str("m8888"), actual, SIZE_MAX), "deleted row still visible");
            } else if (command == "unmoved") {
                std::string actual;
                require(raw[owner]->get(lcdf::Str("a"), actual, SIZE_MAX) && actual == "unmoved-left",
                        "left unaffected row changed");
                require(raw[owner]->get(lcdf::Str("z"), actual, SIZE_MAX) && actual == "unmoved-right",
                        "right unaffected row changed");
            } else if (command == "hold") {
                auto& worker = *workers[owner].as_mut().unwrap();
                worker.run([&]() {
                    require(worker.operation.is_none(), "logical context already held");
                    if (table == "warehouse")
                        require(mako::sharding_warehouse_handle(worker.prepared, owner) == index,
                                "idle prepared lookup selected the wrong source");
                    require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
                            "idle prepared lookup pinned the migration source");
                    worker.operation = rusty::Some(rusty::make_box<mako::ShardingOperation>());
                    Sto::start_transaction();
                    if (table == "warehouse")
                        require(mako::sharding_warehouse_handle(worker.prepared, owner) == index,
                                "held prepared lookup selected the wrong source");
                    std::string actual;
                    require(index->tx_get(nullptr, lcdf::Str(key(0)), actual, SIZE_MAX), "hold engine read");
                    worker.cut = Transaction::epoch_cut();
                    require(worker.cut && !Transaction::epoch_drained(worker.cut), "hold did not pin cut");
                });
            } else if (command == "hold-conflict") {
                // Two real main-thread commits change then restore the bytes.
                // This forces a version conflict even with unchanged-row elision.
                const auto k = key(0);
                std::string before;
                require(index->get(lcdf::Str(k), before, SIZE_MAX), "conflict source read");
                index->put(lcdf::Str(k), "forced-occ-conflict");
                index->put(lcdf::Str(k), before);
                auto& worker = *workers[owner].as_mut().unwrap();
                worker.run([&]() { worker.conflict = true; });
            } else if (command == "release") {
                auto& worker = *workers[owner].as_mut().unwrap();
                worker.run([&]() {
                    require(worker.operation.is_some(), "no retained logical context");
                    if (TThread::txn && TThread::txn->in_progress()) TThread::txn->silent_abort();
                    require(!Transaction::epoch_drained(worker.cut), "cleanup lost logical pin");
                    worker.operation = rusty::None;
                    worker.conflict = false;
                    require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
                            "logical completion leaked the worker epoch");
                });
            } else if (command == "stale") {
                uint64_t ownership_version = 0;
                input >> ownership_version;
                EpochRead read;
                require(read.acquire(owner) == MAKO_SHARD_OK, "begin stale probe");
                const auto status = mako_sharding_read_point(read.value, id, bytes(point),
                    {{owner, ownership_version}, MAKO_SHARD_NO_SNAPSHOT});
                require(status != MAKO_SHARD_OK, "old owner/ownership-version admitted");
            } else {
                throw std::runtime_error("unknown fixture command");
            }
            std::cout << "SMOKE OK" << std::endl;
        }
        // @unsafe - each retained read stays active on its own physical worker.
        // Logical service stop must not revoke their protected immutable views.
        require(!TThread::txn || !TThread::txn->in_progress(),
                "normal stop with a live engine transaction");
        MakoShardRoute stopped_read_routes[2]{};
        uint64_t stopped_read_views[2]{};
        uint32_t stopped_point_status[2]{}, stopped_range_status[2]{};
        for (unsigned owner = first; owner < limit; ++owner) {
            auto& worker = *workers[owner].as_mut().unwrap();
            worker.run([&]() {
                require(worker.operation.is_none(), "normal stop with unsettled logical hold");
                require(!TThread::txn || !TThread::txn->in_progress(), "worker engine still active");
                const auto status = worker.read.acquire(owner);
                require(status == (mako_sharding_ready(owner) ? MAKO_SHARD_OK : MAKO_SHARD_RETRY),
                        "shutdown read readiness mismatch");
                if (status == MAKO_SHARD_OK) {
                    const auto* read = worker.read.value;
                    require(mako_sharding_read_view_id(read, &stopped_read_views[owner]) == MAKO_SHARD_OK
                            && stopped_read_views[owner] != 0, "shutdown protected view");
                    require(mako_sharding_read_route(read, raw_id, bytes("a"),
                                &stopped_read_routes[owner]) == MAKO_SHARD_OK,
                            "shutdown protected route");
                    stopped_point_status[owner] = mako_sharding_read_point(
                        read, raw_id, bytes("a"), stopped_read_routes[owner]);
                    stopped_range_status[owner] = mako_sharding_read_range(
                        read, raw_id, bytes("a"), 0, {}, stopped_read_routes[owner]);
                }
            });
        }
        // @unsafe - engine storage and hosts outlive the admitted transaction.
        // Metadata-reopen fixtures are deliberately unready and cannot warm it.
        rusty::Option<rusty::Box<mako::ShardingOperation>> stopped_operation;
        FullOrderedIndex* stopped_index = nullptr;
        const auto stopped_key = key(0);
        std::string stopped_before;
        bool stopped_found = false;
        Transaction::epoch_type stopped_cut = 0;
        mako::ShardingPreparedRoute stopped_route(warehouse_id, first + 1);
        if (mako_sharding_ready(first)) {
            select_owner(first);
            {
                mako::ShardingOperation warm;
                auto* index = mako::sharding_warehouse_handle(stopped_route, first);
                std::string actual;
                index->get(lcdf::Str(stopped_key), actual, SIZE_MAX);
            }
            require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
                    "warm ownership decision retained an idle pin");
            stopped_operation = rusty::Some(rusty::make_box<mako::ShardingOperation>());
            stopped_index = mako::sharding_warehouse_handle(stopped_route, first);
            Sto::start_transaction();
            stopped_found = stopped_index->tx_get(nullptr, lcdf::Str(stopped_key),
                                                  stopped_before, SIZE_MAX);
            stopped_cut = Transaction::epoch_cut();
            require(stopped_cut && !Transaction::epoch_drained(stopped_cut),
                    "admitted shutdown transaction lost its guard");
        }
        hosts.shutdown();
        if (stopped_operation.is_some()) {
            require(!Transaction::epoch_drained(stopped_cut),
                    "service stop revoked the admitted epoch guard");
            std::string actual;
            require(stopped_index->tx_get(nullptr, lcdf::Str(stopped_key), actual, SIZE_MAX)
                        == stopped_found && (!stopped_found || actual == stopped_before),
                    "service stop revoked the admitted engine read");
            const auto encoded = mako::Encode("committed-after-service-stop");
            stopped_index->tx_put(nullptr, lcdf::Str(stopped_key), encoded);
            require(Sto::try_commit_no_paxos(),
                    "service stop revoked the admitted engine write");
            require(stopped_index->get(lcdf::Str(stopped_key), actual, SIZE_MAX)
                        && actual == "committed-after-service-stop",
                    "admitted context lost its committed write after stop");
            require(Transaction::tinfo[TThread::id()].epoch.load() != 0,
                    "engine commit released the admitted logical guard");
            stopped_operation = rusty::None;
            require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
                    "stopped authorized context retained its terminal pin");
            bool rejected = false;
            try {
                mako::ShardingOperation fresh;
                stopped_index->get(lcdf::Str(stopped_key), actual, SIZE_MAX);
            } catch (const abstract_db::abstract_abort_exception&) { rejected = true; }
            require(rejected, "stopped service admitted fresh engine work");
            require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
                    "rejected fresh engine work leaked an idle pin");
        }
        for (unsigned owner = first; owner < limit; ++owner) {
            auto& worker = *workers[owner].as_mut().unwrap();
            worker.run([&]() {
                const auto* read = worker.read.value;
                if (read) {
                    uint64_t view = 0;
                    MakoShardRoute route{};
                    const auto& before = stopped_read_routes[owner];
                    require(mako_sharding_read_view_id(read, &view) == MAKO_SHARD_OK
                            && view == stopped_read_views[owner],
                            "service stop revoked the protected view");
                    require(mako_sharding_read_route(read, raw_id, bytes("a"), &route) == MAKO_SHARD_OK
                            && route.grant.owner == before.grant.owner
                            && route.grant.ownership_version == before.grant.ownership_version
                            && route.snapshot_version == before.snapshot_version,
                            "service stop changed the protected route");
                    require(mako_sharding_read_point(read, raw_id, bytes("a"), before)
                                == stopped_point_status[owner],
                            "service stop changed protected point ownership");
                    require(mako_sharding_read_range(read, raw_id, bytes("a"), 0, {}, before)
                                == stopped_range_status[owner],
                            "service stop changed protected range ownership");
                    worker.read.release();
                }
                require(worker.read.acquire(owner) != MAKO_SHARD_OK, "stopped node admitted fresh read");
                require(Transaction::tinfo[TThread::id()].epoch.load() == 0,
                        "stopped native read leaked its terminal pin");
            });
            workers[owner] = rusty::None;
        }
        if (stopped_cut)
            require(Transaction::epoch_drained(stopped_cut),
                    "shutdown contexts prevented terminal epoch drain");
        db.thread_end();
        std::cout << "SMOKE STOPPED" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SMOKE FAILED: " << error.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "SMOKE FAILED: engine/ABI exception" << std::endl;
        return 1;
    }
}
