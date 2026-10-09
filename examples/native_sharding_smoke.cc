// Real MBTA/STO + native Rust + srpc fixture. The stdin protocol is test control,
// never a replacement storage/RPC implementation. Production methods do all I/O.
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include "native_sharding_host.h"
#include "benchmarks/benchmark_config.h"
#include "storage/mbta_wrapper.hh"
#include "lib/table_registry.h"

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
void replay_controls(bool aborted) {
    rusty::Vec<IssuedControl> issued;
    {
        auto state = faults.lock().unwrap();
        require(!state->capture, "stop capture before replaying old messages");
        for (const auto& record : state->issued) issued.push(record);
    }
    unsigned operations = 0, terminal_targets = 0;
    for (const auto& record : issued) {
        CapturedReply reply;
        const auto status = mako::native_sharding_peer_call(record.host, record.target,
            record.operation, bytes(record.payload), CapturedReply::receive, &reply);
        require(status == MAKO_SHARD_OK && reply.calls == 1 && reply.copied,
                "old message replay must reach actual production handler");
        require(reply.status == MAKO_SHARD_OK || reply.status == MAKO_SHARD_RETRY
                || reply.status == MAKO_SHARD_INVALID, "unexpected old message application status");
        operations |= 1u << (record.operation - 10);
        if (record.operation == (aborted ? 15u : 14u)) terminal_targets |= 1u << record.target;
    }
    require(terminal_targets == 3, "old terminal message was not issued and replayed to both peers");
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
// @unsafe - already materialized real aliases; no fake index or storage fallback.
uint32_t open_warehouse(void*, uint64_t table, uint32_t wh, uint32_t owner,
                        uint32_t proxy, uintptr_t* output) {
    if (table != warehouse_id || wh < 1 || wh > 2 || owner > 1 || proxy
        || !output || !physical_warehouse[owner][wh - 1]) return MAKO_SHARD_INVALID;
    *output = reinterpret_cast<uintptr_t>(physical_warehouse[owner][wh - 1]);
    return MAKO_SHARD_OK;
}

struct Hosts {
    rusty::Vec<rusty::Box<mako::NativeShardingHost>> items;
    // @unsafe - listeners fence before native jobs join, including assertion failures.
    void shutdown() {
        for (auto& host : items) host->stop_accepting();
        for (auto& host : items) mako_sharding_stop_node(host->owner);
        items.clear();
    }
    // @unsafe - host engine borrows end before the enclosing DB is destroyed.
    ~Hosts() { shutdown(); }
};

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
// @unsafe - actual FullOrderedIndex reads and full paginated scan/rscan calls.
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
            require(mako_sharding_warehouse_opener(owner, nullptr, open_warehouse) == MAKO_SHARD_OK,
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
        std::cout << "SMOKE READY" << std::endl;
        std::string line;
        uint64_t lease_sequence = 0;
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
                require(mako_sharding_lease_begin(owner, {0x7fffffffffff0001ULL, 1})
                            == MAKO_SHARD_RETRY,
                        "ordinary lease admitted while another owner is still loading");
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
            if (command == "route") {
                uint64_t minimum = 0;
                input >> minimum;
                MakoShardGrant grant{};
                bool matched = false;
                for (unsigned attempt = 0; attempt < 200; ++attempt) {
                    const auto status = mako_sharding_route(id, bytes(point), &grant);
                    if (status == MAKO_SHARD_OK && grant.owner == owner && grant.epoch >= minimum) {
                        matched = true;
                        break;
                    }
                    usleep(50000);
                }
                require(matched, "route owner/epoch mismatch after snapshot refresh");
                std::cout << "SMOKE OK " << grant.epoch << std::endl;
                continue;
            }
            require(owner >= first && owner < limit, "command targets nonlocal owner");
            select_owner(owner);
            auto* index = table == "raw" ? raw[owner] : physical_warehouse[owner][0];
            if (command == "check") {
                input >> changed;
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
                MakoShardGrant grant{};
                const MakoShardTxn txn{0x484f4c44ULL + owner, 1};
                require(mako_sharding_route(id, bytes(point), &grant) == MAKO_SHARD_OK
                        && grant.owner == owner, "hold current grant");
                require(mako_sharding_lease_begin(owner, txn) == MAKO_SHARD_OK, "hold begin");
                require(mako_sharding_lease_acquire(owner, txn, id, bytes(point), grant)
                        == MAKO_SHARD_OK, "hold acquire");
            } else if (command == "release") {
                require(mako_sharding_lease_finish(owner, {0x484f4c44ULL + owner, 1})
                        == MAKO_SHARD_OK, "hold release");
            } else if (command == "stale") {
                uint64_t epoch = 0;
                input >> epoch;
                const MakoShardTxn txn{0x534d4f4b45ULL + owner, ++lease_sequence};
                require(mako_sharding_lease_begin(owner, txn) == MAKO_SHARD_OK, "begin stale probe");
                const auto status = mako_sharding_lease_acquire(owner, txn, id, bytes(point), {owner, epoch});
                require(mako_sharding_lease_finish(owner, txn) == MAKO_SHARD_OK, "finish stale probe");
                require(status != MAKO_SHARD_OK, "old owner/epoch admitted");
            } else {
                throw std::runtime_error("unknown fixture command");
            }
            std::cout << "SMOKE OK" << std::endl;
        }
        hosts.shutdown();
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
