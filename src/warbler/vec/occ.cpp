#include <iostream>
#include <vector>
#include <thread>
#include <random>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdlib> // For std::stoi
#include <csignal>
#include <coroutine>
#include <queue>
#include <functional>

// Signal handler function
void handleSignal(int signal) {
    if (signal == SIGINT) {
        std::cout << "\nCTRL+C (SIGINT) caught! Exiting gracefully..." << std::endl;
        std::exit(0); // Exit the program
    }
}

// g++ -std=c++20 -O2 -pthread occ.cpp -o occ
using namespace std;
using namespace std::chrono;

// Constants
const int NUM_KEYS_PER_SHARD = 1000000;

// Global variables
int N; // Number of shards
int VAL_SIZE; // Size of value
const int BASE_VAL_SIZE = 4; // Base size of value

atomic<bool> stopFlag(false); // Flag to stop threads

// Entry struct with a lock bit
struct Entry {
    string value;
    int lock; // 0 = unlocked, 1 = locked
    atomic<int> version; // increment version if a transaction update the value

    Entry(int val_size) : lock(0), version(0) {
        value = string(BASE_VAL_SIZE, 'a');
    }
};

// Shard class
class Shard {
public:
    vector<unique_ptr<Entry>> data;

    Shard(int val_size) {
        data.reserve(NUM_KEYS_PER_SHARD);
        for (int i = 0; i < NUM_KEYS_PER_SHARD; ++i) {
            data.emplace_back(unique_ptr<Entry>(new Entry(val_size)));
        }
    }
};

// Transaction class
class Transaction {
public:
    int txn_id;
    vector<tuple<int, int, string, int>> readSet;   // (shard_id, key, value, version)
    vector<tuple<int, int, string>> writeSet;  // (shard_id, key, value)
    bool success;

    Transaction(int id) : txn_id(id), success(false) {}
};

// Global shards
vector<Shard> shards;

// Function to simulate transaction processing
void workerThread(int thread_id, int& local_throughput, int& local_aborts) {
    pthread_t thread = pthread_self();

    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    int core_id = thread_id % 2? (thread_id / 2) % 64 : (thread_id / 2) % 64 + 64; // Our machine has 128 cores, and (i, i+64) are on the same physical core
    CPU_SET(core_id, &cpuSet);

    // Set CPU affinity for the thread
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuSet) != 0) {
        perror("pthread_setaffinity_np");
        return;
    }

    default_random_engine generator(random_device{}());
    uniform_int_distribution<int> distribution(0, NUM_KEYS_PER_SHARD - 1);
    uniform_int_distribution<int> shard_distribution(0, N - 1);
    uniform_real_distribution<double> prob_dist(0.0, 1.0);

    while (!stopFlag.load()) {
        Transaction txn(thread_id);

        // Read Phase
        for (int i = 0; i < 3; ++i) {
            int shard_id = thread_id; // shard_distribution(generator);
            int key = distribution(generator);
            txn.readSet.push_back({shard_id, key, shards[shard_id].data[key]->value, shards[shard_id].data[key]->version});
        }

        // Decide if write to same shard or different shard
        int write_shard_id = shard_distribution(generator);
        if (prob_dist(generator) >= 0.05) {
            write_shard_id = std::get<0>(txn.readSet[0]); // 95% chance to write to the same shard as the first read
        }

        int key = distribution(generator);
        string new_value = string(VAL_SIZE + BASE_VAL_SIZE, 'a'+VAL_SIZE%10);
                
        // Simulate latency if accessing a different shard
        bool shard_found_in_readSet = false;
        for (auto& t : txn.readSet) {
            if (std::get<0>(t) == write_shard_id) {
                shard_found_in_readSet = true;
                break;
            }
        }

        bool isRemote = false;
        if (!shard_found_in_readSet) {
            // Injected latency for remote access
            isRemote = true;
        }
        
        // dpdk read in execution phase
        if (isRemote) this_thread::sleep_for(microseconds(15));

        txn.writeSet.push_back({write_shard_id, key, new_value});

        // Commit Phase
        bool valid = true;

        // Lock the writeSet keys first using compare-and-swap on the lock bit
        vector<pair<int, int>> keys_to_lock; // (shard_id, key)
        for (auto& t : txn.writeSet) {
            int wsid = std::get<0>(t);
            int wkey = std::get<1>(t);
            keys_to_lock.emplace_back(wsid, wkey);
        }

        // Sort keys to prevent deadlocks
        sort(keys_to_lock.begin(), keys_to_lock.end());

        // Try to acquire all locks
        // dpdk clock
        if (isRemote) this_thread::sleep_for(microseconds(15));
        vector<pair<int, int>> acquired_locks;
        for (auto& p : keys_to_lock) {
            int sid = p.first;
            int key = p.second;

            // Perform compare-and-swap on the lock bit
            int expected = 0;
            if (!__sync_bool_compare_and_swap(&shards[sid].data[key]->lock, expected, 1)) {
                // Failed to acquire lock
                valid = false;
                break;
            } else {
                acquired_locks.emplace_back(sid, key);
            }
        }

        if (valid) {
            // Validate the ReadSet
            for (auto& t : txn.readSet) {
                int sid = std::get<0>(t);
                int key = std::get<1>(t);
                int version = std::get<3>(t);
                int current_ver = shards[sid].data[key]->version;
                // dpdk validate
                if (isRemote) this_thread::sleep_for(microseconds(15));
                if (current_ver != version) { // if version has changed
                    valid = false;
                    break;
                }
                // dpdk getClock
                if (isRemote) this_thread::sleep_for(microseconds(15));
            }

            if (valid) {
                // dpdk install
                if (isRemote) this_thread::sleep_for(microseconds(15));
                // Install the WriteSet
                for (auto& t : txn.writeSet) {
                    int wsid = std::get<0>(t);
                    int wkey = std::get<1>(t);
                    string wval = std::get<2>(t);
                    shards[wsid].data[wkey]->value.assign(wval);
                    shards[wsid].data[wkey]->version++;
                }
                txn.success = true;
                local_throughput++;
            }
        } 

        if (!valid) {
            local_aborts++;
        }

        // Release all acquired locks
        for (auto& p : acquired_locks) {
            int sid = p.first;
            int key = p.second;
            shards[sid].data[key]->lock = 0; // Unlock by setting lock bit to 0
        }

        // Transaction ends here
    }
}

void initThread(int thread_id, vector<Shard>& shards) {
    shards[thread_id] = Shard(VAL_SIZE);
}

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handleSignal);

    N = std::stoi(argv[1]);
    VAL_SIZE = std::stoi(argv[2]);
    cout << "Enter number of shards: " << N << std::endl;
    cout << "Enter size of value: " << VAL_SIZE << std::endl;

    shards.reserve(N);

    // Initialize shards with 1M key-value pairs
    int num_threads = N;
    cout << "Using " << num_threads << " worker threads." << endl;

    // for (int i = 0; i < N; ++i) {
    //     shards.emplace_back(Shard(VAL_SIZE));
    // }
    vector<thread> init_threads;
    for (int i = 0; i < num_threads; ++i) {
        init_threads.emplace_back(initThread, i, ref(shards));
    }
    for (auto& t : init_threads) {
        t.join();
    }
    std::cout<<"Load data complete"<<std::endl;

    // Create worker threads and per-thread throughput counters
    vector<thread> threads;
    vector<int> thread_throughput(num_threads, 0); // Per-thread throughput counters
    vector<int> thread_aborts(num_threads, 0); // Per-thread aborts counters

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(workerThread, i, ref(thread_throughput[i]), ref(thread_aborts[i]));
    }

    int run_time = 30;
    auto start_time = steady_clock::now();
    this_thread::sleep_for(seconds(run_time));
    stopFlag.store(true);

    // Join threads
    for (auto& t : threads) {
        t.join();
    }

    // Sum up throughput from all threads
    int total_throughput = 0;
    int total_aborts = 0;
    for (int i = 0; i < num_threads; ++i) {
        total_throughput += thread_throughput[i];
        total_aborts += thread_aborts[i];
    }

    // Output throughput
    cout << "Throughput: " << total_throughput / run_time << " txn/sec" << endl;
    cout << "Aborts: " << total_aborts / run_time << " txn/sec" << endl;

    return 0;
}

