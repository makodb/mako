#pragma once

/**
 * @file config_node_init.h
 * @brief Config node initialization functions for Mako.
 *
 * This header is separated from mako.hh to avoid include conflicts between
 * rrr headers and mako lib headers. Include this only in files that need
 * config node functionality.
 *
 * NOTE: The rrr headers (through config_*.h) define macros like 'verify' that
 * may conflict with other code. Include this header last in your file.
 */

#include "cluster/config_store.h"
#include "deptran/config_service.h"
#include "deptran/config_client.h"
#include "benchmark_config.h"

namespace mako {

// Global config node components (for c-node mode)
extern janus::ConfigStore* g_config_store;
extern rusty::Option<rusty::Arc<rrr::PollThread>> g_config_poll_thread;
extern rrr::Server* g_config_rpc_server;

/**
 * @brief Initialize config node components for c-node mode.
 * @return true if initialization succeeded, false otherwise
 */
bool init_config_node();

/**
 * @brief Fetch configuration from a remote c-node.
 * @return true if config was fetched and applied, false otherwise
 */
bool fetch_config_from_cnode();

/**
 * @brief Fetch sharding policy from a remote c-node.
 *
 * Data nodes call this at startup to get the sharding policy from the c-node.
 * If no policy is set on the c-node yet, returns true (uses fallback routing).
 * Initializes the global sharding policy cache on success.
 *
 * @return true if policy was fetched (or no policy exists), false on error
 */
bool fetch_sharding_policy_from_cnode();

/**
 * @brief Shutdown config node components.
 */
void shutdown_config_node();

}  // namespace mako
