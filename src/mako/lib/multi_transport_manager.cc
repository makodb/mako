// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * multi_transport_manager.cc:
 *   Implementation of MultiTransportManager
 *
 **********************************************************************/

#include "lib/multi_transport_manager.h"
#include "lib/assert.h"
#include "lib/common.h"

namespace mako {

MultiTransportManager::MultiTransportManager()
    : running_(false)
{
}

MultiTransportManager::~MultiTransportManager()
{
    StopAll();

    // Clean up transport instances
    for (auto& pair : transports_) {
        if (pair.second) {
            delete pair.second;
        }
    }
    transports_.clear();
}

bool MultiTransportManager::InitializeAll(
    const std::string& config_file,
    const std::vector<int>& local_shard_indices,
    const std::string& ip,
    const std::string& cluster,
    uint8_t st_nr_req_types,
    uint8_t end_nr_req_types,
    uint8_t phy_port,
    uint8_t numa_node)
{
    if (local_shard_indices.empty()) {
        Warning("MultiTransportManager::InitializeAll: No shards specified");
        return false;
    }

    Notice("MultiTransportManager: Initializing %zu transports", local_shard_indices.size());

    for (int shard_idx : local_shard_indices) {
        // Create FastTransport for this shard
        // Each shard uses a unique server ID (0 for now, can be parameterized later)
        uint16_t server_id = 0;

        FastTransport* transport = new FastTransport(
            config_file,
            const_cast<std::string&>(ip),  // FastTransport takes non-const reference
            cluster,
            st_nr_req_types,
            end_nr_req_types,
            phy_port,
            numa_node,
            shard_idx,
            server_id);

        if (!transport) {
            Warning("MultiTransportManager: Failed to create transport for shard %d", shard_idx);
            return false;
        }

        transports_[shard_idx] = transport;
        Notice("MultiTransportManager: Initialized transport for shard %d", shard_idx);
    }

    return true;
}

void MultiTransportManager::RunAll()
{
    if (running_.load()) {
        Warning("MultiTransportManager::RunAll: Already running");
        return;
    }

    if (transports_.empty()) {
        Warning("MultiTransportManager::RunAll: No transports to run");
        return;
    }

    Notice("MultiTransportManager: Starting %zu transport event loops", transports_.size());

    running_.store(true);

    // Spawn a thread for each transport's event loop
    for (auto& pair : transports_) {
        int shard_idx = pair.first;
        FastTransport* transport = pair.second;

        event_loop_threads_.emplace_back([this, shard_idx, transport]() {
            Notice("MultiTransportManager: Event loop thread started for shard %d", shard_idx);

            // Run the transport event loop (blocks until Stop() is called)
            transport->Run();

            Notice("MultiTransportManager: Event loop thread exited for shard %d", shard_idx);
        });
    }

    Notice("MultiTransportManager: All %zu event loop threads spawned", event_loop_threads_.size());
}

void MultiTransportManager::StopAll()
{
    if (!running_.load()) {
        return;  // Already stopped
    }

    Notice("MultiTransportManager: Stopping all transports");

    // Signal all transports to stop
    for (auto& pair : transports_) {
        if (pair.second) {
            pair.second->Stop();
        }
    }

    running_.store(false);

    // Wait for all event loop threads to finish
    Notice("MultiTransportManager: Waiting for %zu threads to finish", event_loop_threads_.size());
    for (auto& thread : event_loop_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    event_loop_threads_.clear();

    Notice("MultiTransportManager: All transports stopped");
}

FastTransport* MultiTransportManager::GetTransport(int shard_idx)
{
    auto it = transports_.find(shard_idx);
    if (it != transports_.end()) {
        return it->second;
    }
    return nullptr;
}

} // namespace mako
