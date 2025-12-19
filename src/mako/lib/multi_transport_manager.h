// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * multi_transport_manager.h:
 *   Manages multiple FastTransport instances for multi-shard mode
 *   Each shard gets its own transport running in a separate thread
 *
 **********************************************************************/

#ifndef _MAKO_MULTI_TRANSPORT_MANAGER_H_
#define _MAKO_MULTI_TRANSPORT_MANAGER_H_

#include <map>
#include <vector>
#include <thread>
#include <string>
#include <atomic>
#include "lib/fasttransport.h"
#include "lib/configuration.h"

namespace mako {

/**
 * MultiTransportManager - Manages multiple transport instances for multi-shard mode
 *
 * This class creates and manages one FastTransport per shard running in the same process.
 * Each transport listens on its own port and runs its event loop in a dedicated thread.
 *
 * Thread Safety:
 * - InitializeAll() and RunAll() should be called from the main thread
 * - Once started, each transport runs independently in its own thread
 * - StopAll() is thread-safe and can be called from any thread
 *
 * Usage:
 *   MultiTransportManager manager;
 *   manager.InitializeAll(config, local_shards, cluster, ...);
 *   manager.RunAll();  // Spawns threads for each transport
 *   // ... do work ...
 *   manager.StopAll();  // Stops all transports and joins threads
 */
class MultiTransportManager {
public:
    MultiTransportManager();
    ~MultiTransportManager();

    /**
     * Initialize all transports for the specified local shards
     *
     * @param config Transport configuration (contains shard addresses/ports)
     * @param local_shard_indices Which shards to run in this process
     * @param ip Local IP address to bind to
     * @param cluster Cluster name (localhost, p1, p2, learner)
     * @param st_nr_req_types Start of request type range
     * @param end_nr_req_types End of request type range
     * @param phy_port Physical port for RDMA (0 for TCP)
     * @param numa_node NUMA node for memory allocation
     * @return true on success, false on failure
     */
    bool InitializeAll(
        const std::string& config_file,
        const std::vector<int>& local_shard_indices,
        const std::string& ip,
        const std::string& cluster,
        uint8_t st_nr_req_types,
        uint8_t end_nr_req_types,
        uint8_t phy_port = 0,
        uint8_t numa_node = 0);

    /**
     * Start all transport event loops in separate threads
     *
     * Each transport's Run() method is called in a dedicated thread.
     * This method returns immediately after spawning all threads.
     */
    void RunAll();

    /**
     * Stop all transports and wait for threads to finish
     *
     * Signals all transports to stop, then joins all event loop threads.
     * Safe to call multiple times.
     */
    void StopAll();

    /**
     * Get transport for a specific shard
     *
     * @param shard_idx Shard index
     * @return Pointer to FastTransport, or nullptr if not found
     */
    FastTransport* GetTransport(int shard_idx);

    /**
     * Get all transports
     *
     * @return Map of shard_index -> FastTransport*
     */
    const std::map<int, FastTransport*>& GetAllTransports() const {
        return transports_;
    }

    /**
     * Check if transports are running
     *
     * @return true if event loop threads are active
     */
    bool IsRunning() const {
        return running_.load();
    }

    /**
     * Get number of managed transports
     */
    size_t GetTransportCount() const {
        return transports_.size();
    }

private:
    // Map of shard_index -> FastTransport instance
    std::map<int, FastTransport*> transports_;

    // Event loop threads, one per transport
    std::vector<std::thread> event_loop_threads_;

    // Running state
    std::atomic<bool> running_;

    // Prevent copy/move
    MultiTransportManager(const MultiTransportManager&) = delete;
    MultiTransportManager& operator=(const MultiTransportManager&) = delete;
};

} // namespace mako

#endif  /* _MAKO_MULTI_TRANSPORT_MANAGER_H_ */
