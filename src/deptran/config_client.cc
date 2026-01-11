#include "config_client.h"
#include <thread>
#include <chrono>

namespace janus {

ConfigClient::ConfigClient(const std::string& c_node_addr)
    : c_node_addr_(c_node_addr),
      poll_thread_(rusty::None),
      rpc_client_(rusty::None),
      proxy_(rusty::None) {
}

ConfigClient::~ConfigClient() {
    disconnect();
}

// @unsafe - Creates poll thread and client connection
bool ConfigClient::try_connect() {
    // Create poll thread if not already created
    if (poll_thread_.is_none()) {
        poll_thread_ = rusty::Some(rrr::PollThread::create());
    }

    // Create client if not already created
    if (rpc_client_.is_none()) {
        auto& poll = poll_thread_.as_ref().unwrap();
        rpc_client_ = rusty::Some(rrr::Client::create(poll.clone()));
    }

    // Try to connect
    auto& client = rpc_client_.as_ref().unwrap();
    int result = client->connect(c_node_addr_.c_str());  // @unsafe

    if (result == 0) {
        // Create proxy
        if (proxy_.is_none()) {
            // Get raw pointer for proxy (it doesn't own the client)
            // ConfigServiceProxy takes non-const Client* but we have Arc which gives const
            auto proxy = new ConfigServiceProxy(const_cast<rrr::Client*>(client.get()));  // @unsafe
            proxy_ = rusty::Some(proxy);
        }
        Log_info("ConfigClient: Connected to c-node at %s", c_node_addr_.c_str());  // @unsafe
        return true;
    }

    Log_warn("ConfigClient: Failed to connect to c-node at %s (error: %d)",  // @unsafe
             c_node_addr_.c_str(), result);
    return false;
}

// @unsafe - Establishes network connection with retry
bool ConfigClient::connect() {
    uint32_t retries = 0;
    uint32_t delay_ms = retry_delay_ms_.get();
    uint32_t max_retries = max_retries_.get();
    uint32_t max_delay = max_retry_delay_ms_.get();

    while (retries < max_retries) {
        if (try_connect()) {
            return true;
        }

        retries++;
        if (retries < max_retries) {
            Log_info("ConfigClient: Retrying connection (%u/%u) in %u ms...",  // @unsafe
                     retries, max_retries, delay_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));  // @unsafe

            // Exponential backoff with cap
            delay_ms = std::min(delay_ms * 2, max_delay);
        }
    }

    Log_error("ConfigClient: Failed to connect after %u retries", max_retries);  // @unsafe
    return false;
}

// @safe - Cleanup only
void ConfigClient::disconnect() {
    // Delete proxy first (it holds raw pointer to client)
    if (proxy_.is_some()) {
        delete proxy_.take().unwrap();  // @unsafe
    }

    // Close client connection
    if (rpc_client_.is_some()) {
        rpc_client_.as_ref().unwrap()->close();
        rpc_client_ = rusty::None;
    }

    // Note: We keep poll_thread_ alive - it can be reused for reconnection
}

// @safe - Read-only check
bool ConfigClient::is_connected() const {
    if (rpc_client_.is_none()) {
        return false;
    }
    // Check if client is in connected state
    auto& client = rpc_client_.as_ref().unwrap();
    return client->connection_state() == rrr::ConnectionState::CONNECTED;
}

// @unsafe - Makes RPC call
rusty::Option<PersistentConfig> ConfigClient::fetch_config() {
    if (proxy_.is_none()) {
        Log_warn("ConfigClient: Not connected, cannot fetch config");  // @unsafe
        return rusty::None;
    }

    uint64_t current_version = 0;
    rrr::i32 has_update = 0;
    std::string config_data;

    // Call RPC with client_version=0 to get full config
    auto proxy = proxy_.as_ref().unwrap();
    rrr::i32 result = proxy->GetConfig(0, &current_version, &has_update, &config_data);  // @unsafe

    if (result != 0) {
        Log_warn("ConfigClient: GetConfig RPC failed with error %d", result);  // @unsafe
        return rusty::None;
    }

    if (has_update == 0 || config_data.empty()) {
        Log_warn("ConfigClient: C-node has no configuration");  // @unsafe
        return rusty::None;
    }

    // Deserialize config_data to PersistentConfig
    rrr::Marshal m;
    m.write(config_data.data(), config_data.size());  // @unsafe

    PersistentConfig config;
    m >> config;  // @unsafe

    Log_info("ConfigClient: Fetched configuration version %lu with %zu sites",  // @unsafe
             config.version, config.sites.size());

    return rusty::Some(std::move(config));
}

// @unsafe - Makes RPC call
rusty::Option<uint64_t> ConfigClient::fetch_version() {
    if (proxy_.is_none()) {
        Log_warn("ConfigClient: Not connected, cannot fetch version");  // @unsafe
        return rusty::None;
    }

    uint64_t version = 0;
    auto proxy = proxy_.as_ref().unwrap();
    rrr::i32 result = proxy->GetConfigVersion(&version);  // @unsafe

    if (result != 0) {
        Log_warn("ConfigClient: GetConfigVersion RPC failed with error %d", result);  // @unsafe
        return rusty::None;
    }

    return rusty::Some(version);
}

// @unsafe - Makes RPC call
rusty::Option<bool> ConfigClient::has_config() {
    if (proxy_.is_none()) {
        Log_warn("ConfigClient: Not connected, cannot check has_config");  // @unsafe
        return rusty::None;
    }

    rrr::i32 has_config_result = 0;
    auto proxy = proxy_.as_ref().unwrap();
    rrr::i32 result = proxy->HasConfig(&has_config_result);  // @unsafe

    if (result != 0) {
        Log_warn("ConfigClient: HasConfig RPC failed with error %d", result);  // @unsafe
        return rusty::None;
    }

    return rusty::Some(has_config_result != 0);
}

}  // namespace janus
