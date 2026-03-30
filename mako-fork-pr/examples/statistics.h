//
// Statistics tracking for continuous transaction tests
//

#ifndef MAKO_EXAMPLES_STATISTICS_H
#define MAKO_EXAMPLES_STATISTICS_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <chrono>
#include <vector>

// Global statistics counters
struct Statistics {
    std::atomic<uint64_t> total_attempts{0};
    std::atomic<uint64_t> successful_commits{0};
    std::atomic<uint64_t> aborts{0};
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> cross_shard{0};
    std::atomic<uint64_t> single_shard{0};

    void reset() {
        total_attempts = 0;
        successful_commits = 0;
        aborts = 0;
        reads = 0;
        writes = 0;
        cross_shard = 0;
        single_shard = 0;
    }

    void print(int seconds) {
        uint64_t total = total_attempts.load();
        uint64_t commits = successful_commits.load();
        uint64_t abort_count = aborts.load();
        uint64_t read_count = reads.load();
        uint64_t write_count = writes.load();
        uint64_t cross = cross_shard.load();
        uint64_t single = single_shard.load();

        double abort_rate = total > 0 ? (100.0 * abort_count / total) : 0;
        double cross_rate = total > 0 ? (100.0 * cross / total) : 0;

        printf("[%3ds] TPS: %6lu | Commits: %6lu | Aborts: %5lu (%.1f%%) | R/W: %5lu/%5lu | Cross-shard: %5lu (%.1f%%) | Single-shard: %5lu\n",
               seconds, commits, commits, abort_count, abort_rate,
               read_count, write_count, cross, cross_rate, single);
        fflush(stdout);
    }
};

// Statistics printing thread
// Parameters:
//   stats: reference to global Statistics object
//   keep_running: reference to volatile bool controlling thread execution
//   worker_commits: vector of per-worker commit counters
inline void stats_printer_thread(Statistics& stats, volatile bool& keep_running,
                                   std::vector<std::atomic<uint64_t>>* worker_commits) {
    int seconds = 0;
    uint64_t last_total_attempts = 0;
    uint64_t last_successful_commits = 0;
    uint64_t last_aborts = 0;
    uint64_t last_reads = 0;
    uint64_t last_writes = 0;
    uint64_t last_cross_shard = 0;
    uint64_t last_single_shard = 0;

    // Track last per-worker commits
    std::vector<uint64_t> last_worker_commits(worker_commits->size(), 0);

    while (keep_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        seconds++;

        // Calculate and print per-second statistics
        uint64_t current_total = stats.total_attempts.load();
        uint64_t current_commits = stats.successful_commits.load();
        uint64_t current_aborts = stats.aborts.load();
        uint64_t current_reads = stats.reads.load();
        uint64_t current_writes = stats.writes.load();
        uint64_t current_cross = stats.cross_shard.load();
        uint64_t current_single = stats.single_shard.load();

        Statistics delta;
        delta.total_attempts = current_total - last_total_attempts;
        delta.successful_commits = current_commits - last_successful_commits;
        delta.aborts = current_aborts - last_aborts;
        delta.reads = current_reads - last_reads;
        delta.writes = current_writes - last_writes;
        delta.cross_shard = current_cross - last_cross_shard;
        delta.single_shard = current_single - last_single_shard;

        delta.print(seconds);

        // Print per-worker commits
        printf("    Worker commits: ");
        for (size_t i = 0; i < worker_commits->size(); i++) {
            uint64_t current_worker = (*worker_commits)[i].load();
            uint64_t delta_worker = current_worker - last_worker_commits[i];
            printf("%5lu", delta_worker);
            if (i < worker_commits->size() - 1) {
                printf(" | ");
            }
            last_worker_commits[i] = current_worker;
        }
        printf("\n");
        fflush(stdout);

        last_total_attempts = current_total;
        last_successful_commits = current_commits;
        last_aborts = current_aborts;
        last_reads = current_reads;
        last_writes = current_writes;
        last_cross_shard = current_cross;
        last_single_shard = current_single;
    }

    printf("\n--- Final Statistics ---\n");
    fflush(stdout);
    stats.print(seconds);
}

#endif // MAKO_EXAMPLES_STATISTICS_H
