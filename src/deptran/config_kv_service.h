#pragma once

// ConfigKvService — the transport for RemoteKvStore's ReadFn. A
// non-shard-0 node reads a single key from shard 0's __mako_config__
// system table through this RPC; the server handler is backed by a
// KvStore (on shard 0's leader, an OrderedIndexKvStore over the config
// FullOrderedIndex). Distinct from ConfigService, which serves the
// c-node's ConfigStore — this one is bound to shard 0's config index,
// resolving the c-node-vs-shard-0 split by giving shard-0 config reads
// their own focused service.

#include "rcc_rpc.h"                    // ConfigKvServiceService / Proxy
#include "cluster/kv_store.h"           // KvStore
#include "cluster/remote_kv_store.h"    // RemoteKvStoreReadFn

#include <string>
#include <utility>

namespace janus {

/**
 * Server side. Serves ReadConfigKey from a KvStore. The KvStore is
 * injected (non-owning) — on shard 0's leader it's an OrderedIndexKvStore
 * over the __mako_config__ index; in tests an InMemoryKvStore.
 */
// @safe - thin handler over an injected KvStore.
class ConfigKvServiceImpl : public ConfigKvServiceService {
public:
    explicit ConfigKvServiceImpl(KvStore* kv) : kv_(kv) {}

    // Read logic, factored out of the RPC handler so it can be unit
    // tested without the RPC machinery (no DeferredReply / socket).
    // @unsafe - KvStore read (port returns Option; adapt to found + out)
    bool DoReadConfigKey(const std::string& key, std::string* value) {
        if (kv_ == nullptr || value == nullptr) return false;
        auto found = kv_->get(key);
        if (found.is_none()) return false;
        *value = found.unwrap();
        return true;
    }

    // @unsafe - RPC handler; delegates to DoReadConfigKey then replies.
    void ReadConfigKey(const RpcReadConfigKeyRequest& req,
                       RpcReadConfigKeyResponse& resp,
                       rrr::DeferredReply defer) override {
        std::string value;
        const bool found = DoReadConfigKey(req.key, &value);
        resp.found = found ? 1 : 0;
        if (found) resp.value = std::move(value);
        defer.reply();
    }

private:
    KvStore* kv_;  // non-owning; shard 0's config store via the port.
};

/**
 * Client side. Build a RemoteKvStoreReadFn from a connected
 * ConfigKvServiceProxy. The proxy must outlive the returned function.
 * Production wires this on a non-shard-0 node, pointed at shard 0's
 * leader; the resulting ReadFn goes into a RemoteKvStore, which a
 * ConfigManager/ConfigWatcher reads through.
 */
// @unsafe - issues an RPC per get.
inline RemoteKvStoreReadFn make_config_read_fn(ConfigKvServiceProxy* proxy) {
    return [proxy](const std::string& key) -> rusty::Option<std::string> {
        if (proxy == nullptr) return rusty::None;
        ConfigKvServiceProxy::RpcReadConfigKeyRequest req;
        req.key = key;
        auto r = proxy->ReadConfigKey(req);   // synchronous
        if (r.is_err()) return rusty::None;
        auto resp = r.unwrap();
        if (!resp.found) return rusty::None;
        return rusty::Some(std::move(resp.value));
    };
}

}  // namespace janus
