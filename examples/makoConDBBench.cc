#include <stddef.h>

#include <mako.hh>
#include "storage/mbta_wrapper.hh"
#include <examples/common.h>
#include "storage/abstract_db.h"
#include "storage/abstract_ordered_index.h"
#include "sto/StringWrapper.hh"

import std;


struct Timer {
    std::chrono::steady_clock::time_point start;
    Timer() : start(std::chrono::steady_clock::now()) {}
    double elapsed() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - start).count();
    }
};

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    // Parameters (mirror your redis-benchmark setup)
    const size_t KEYS      = 20'000'000;   // keyspace size
    const size_t VAL_SIZE  = 8;            // 8-byte user values
    const size_t LOAD_OPS  = KEYS;         // preload one value per key
    const size_t GET_OPS   = 20'000'000;   // random GETs
    const size_t SET_OPS   = 20'000'000;   // random SETs (updates)

    auto& config = BenchmarkConfig::getInstance();
    config.setNthreads(1);

    std::cout << "== DB-only Benchmark (" << config.getNthreads() << "thread, no network, no RustWrapper) ==\n"
              << "KEYS="     << KEYS
              << " VAL_SIZE=" << VAL_SIZE
              << " LOAD_OPS=" << LOAD_OPS
              << " GET_OPS="  << GET_OPS
              << " SET_OPS="  << SET_OPS
              << "\n";

    // ---- DB setup ----
    abstract_db* db = new mbta_wrapper;
    db->init();

    abstract_ordered_index* customerTable = db->open_index("customer_0");
    if (!customerTable) {
        std::cerr << "Failed to open index customer_0\n";
        delete db;
        return 1;
    }

    // ---- Per-thread DB state (single thread) ----
    // Equivalent to RustWrapper::ensure_thread_info()
    mbta_ordered_index::mbta_type::thread_init();

    str_arena arena;
    std::string txn_obj_buf;
    txn_obj_buf.resize(db->sizeof_txn_object(0));

    auto make_user_key = [](size_t id) {
        return std::string("key_") + std::to_string(id);
    };

    auto build_db_key = [](const std::string& user_key) {
        std::string s;
        s.reserve(sizeof("table_key_") - 1 + user_key.size());
        s.append("table_key_", sizeof("table_key_") - 1);
        s.append(user_key);
        return s;
    };

    auto build_db_value = [](const std::string& user_val) {
        std::string s;
        s.reserve(sizeof("table_value_") - 1 + user_val.size() + mako::EXTRA_BITS_FOR_VALUE);
        s.append("table_value_", sizeof("table_value_") - 1);
        s.append(user_val);
        s.append(mako::EXTRA_BITS_FOR_VALUE, 'B');
        return s;
    };

    // ==============================
    // Phase 1: LOAD (preload values)
    // ==============================
    std::cout << "\n[Phase 1] Preloading " << LOAD_OPS
              << " keys with " << VAL_SIZE << "-byte values...\n";

    Timer load_timer;
    size_t load_succ = 0, load_aborts = 0;

    const std::string user_value_load(VAL_SIZE, 'V'); // base preload value

    for (size_t k = 0; k < LOAD_OPS; ++k) {
        std::string user_key = make_user_key(k);
        std::string db_key   = build_db_key(user_key);
        std::string db_val   = build_db_value(user_value_load);

        void* txn = db->new_txn(0, arena, txn_obj_buf.data());

        try {
            customerTable->put(txn, db_key, StringWrapper(db_val));
            db->commit_txn(txn);
            ++load_succ;
        } catch (abstract_db::abstract_abort_exception&) {
            db->abort_txn(txn);
            ++load_aborts;
        } catch (...) {
            db->abort_txn(txn);
        }
    }

    double load_sec = load_timer.elapsed();
    double load_rps = static_cast<double>(LOAD_OPS) / load_sec;

    std::cout << "[Phase 1] Preload summary:\n"
              << "  ops:     " << LOAD_OPS << "\n"
              << "  succ:    " << load_succ << "\n"
              << "  aborts:  " << load_aborts << "\n"
              << "  elapsed: " << load_sec << " sec\n"
              << "  thrpt:   " << load_rps << " req/s\n";

    // ==============================
    // Phase 2: GET workload
    // ==============================
    std::cout << "\n[Phase 2] GET workload: " << GET_OPS
              << " ops over " << KEYS << " keys (uniform random)...\n";

    std::mt19937_64 rng_get(123456);
    std::uniform_int_distribution<size_t> key_dist_get(0, KEYS - 1);

    Timer get_timer;
    size_t get_succ = 0, get_aborts = 0;

    for (size_t i = 0; i < GET_OPS; ++i) {
        size_t key_id   = key_dist_get(rng_get);
        std::string user_key = make_user_key(key_id);
        std::string db_key   = build_db_key(user_key);

        void* txn = db->new_txn(
            0,
            arena,
            txn_obj_buf.data(),
            abstract_db::HINT_TPCC_NEW_ORDER  // matches your RustWrapper GET hint
        );

        std::string val_out;
        try {
            customerTable->get(txn, db_key, val_out);
            db->commit_txn(txn);
            ++get_succ;
        } catch (abstract_db::abstract_abort_exception&) {
            db->abort_txn(txn);
            ++get_aborts;
        } catch (...) {
            db->abort_txn(txn);
        }
    }

    double get_sec = get_timer.elapsed();
    double get_rps = static_cast<double>(GET_OPS) / get_sec;

    std::cout << "[Phase 2] GET summary:\n"
              << "  ops:     " << GET_OPS << "\n"
              << "  succ:    " << get_succ << "\n"
              << "  aborts:  " << get_aborts << "\n"
              << "  elapsed: " << get_sec << " sec\n"
              << "  thrpt:   " << get_rps << " req/s\n";

    // ==============================
    // Phase 3: SET workload (updates)
    // ==============================
    std::cout << "\n[Phase 3] SET workload: " << SET_OPS
              << " ops over " << KEYS << " keys (uniform random)...\n";

    std::mt19937_64 rng_set(654321);
    std::uniform_int_distribution<size_t> key_dist_set(0, KEYS - 1);

    Timer set_timer;
    size_t set_succ = 0, set_aborts = 0;

    for (size_t i = 0; i < SET_OPS; ++i) {
        size_t key_id   = key_dist_set(rng_set);
        std::string user_key = make_user_key(key_id);

        // 8-byte user value, vary by key
        std::string user_val(VAL_SIZE, 'A' + (key_id % 26));
        std::string db_key = build_db_key(user_key);
        std::string db_val = build_db_value(user_val);

        void* txn = db->new_txn(0, arena, txn_obj_buf.data());

        try {
            customerTable->put(txn, db_key, StringWrapper(db_val));
            db->commit_txn(txn);
            ++set_succ;
        } catch (abstract_db::abstract_abort_exception&) {
            db->abort_txn(txn);
            ++set_aborts;
        } catch (...) {
            db->abort_txn(txn);
        }
    }

    double set_sec = set_timer.elapsed();
    double set_rps = static_cast<double>(SET_OPS) / set_sec;

    std::cout << "[Phase 3] SET summary:\n"
              << "  ops:     " << SET_OPS << "\n"
              << "  succ:    " << set_succ << "\n"
              << "  aborts:  " << set_aborts << "\n"
              << "  elapsed: " << set_sec << " sec\n"
              << "  thrpt:   " << set_rps << " req/s\n";

    // ==============================
    // Final summary
    // ==============================
    std::cout << "\n=== Final DB-only summary (single thread) ===\n"
              << "Preload: " << load_rps << " req/s\n"
              << "GET:     " << get_rps  << " req/s\n"
              << "SET:     " << set_rps  << " req/s\n";

    delete db;
    return 0;
}