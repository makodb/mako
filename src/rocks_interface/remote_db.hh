#pragma once
#include <rusty/rusty.hpp>
#include "status.hh"
#include "idb.hh"
#include "db.hh"
#include "gateway_protocol.h"
#include <atomic>
#include <cstring>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
import rusty;

namespace mako {
struct RemoteOptions {
    std::string server_host = "localhost";
    int server_port = 31000;
    int shard_index = 0;
    int num_shards = 1;
    uint32_t timeout_ms = 5000;
    // Administratively unique external namespace (high bit set). Never reuse an
    // ID for a restarted client: outstanding outcomes could still be unknown.
    uint64_t client_id = 0;
    uint64_t first_sequence = 1;
    // @safe - configuration value conversion.
    ClientConfig to_client_config() const {
        ClientConfig config;
        config.server_hosts.push_back(server_host);
        config.server_ports.push_back(server_port);
        config.enabled = true;
        config.timeout_ms = timeout_ms;
        config.client_id = client_id;
        config.first_sequence = first_sequence;
        return config;
    }
};

class RemoteDB;
// Gateway operations, with or without a session handle, are individually
// committed. A session is NOT a multi-operation OCC transaction, and Rollback
// cannot undo prior Put/Delete/Insert operations.
class RemoteTable : public ITable {
public:
    // @safe - borrows the parent, which owns every table proxy.
    RemoteTable(RemoteDB* db, const std::string& name, uint16_t table)
        : db_(db), name_(name), table_(table) {}
    // @unsafe - typed adapters to the native pending-operation stream.
    Status Put(void* session, const std::string& key, const std::string& value) override;
    Status Get(void* session, const std::string& key, std::string& value) override;
    Status Delete(void* session, const std::string& key) override;
    Status Insert(void* session, const std::string& key, const std::string& value) override;
    Status Exists(void* session, const std::string& key, bool* exists) override;
    Status Put(const std::string& key, const std::string& value) override;
    Status Get(const std::string& key, std::string& value) override;
    Status Delete(const std::string& key) override;
    Status Insert(const std::string& key, const std::string& value) override;
    Status Exists(const std::string& key, bool* exists) override;
    // @safe - read-only accessors and explicitly unsupported preexisting surface.
    const std::string& GetName() const override { return name_; }
    uint16_t GetTableId() const { return table_; }
    Status Scan(void*, const std::string&, const std::string*,
                std::function<bool(const std::string&, const std::string&)>) override {
        return Status::NotSupported("remote gateway Scan");
    }
    Status ReverseScan(void*, const std::string&, const std::string*,
                       std::function<bool(const std::string&, const std::string&)>) override {
        return Status::NotSupported("remote gateway ReverseScan");
    }
    Status GetApproximateSize(size_t*) override {
        return Status::NotSupported("remote gateway approximate size");
    }
private:
    // @unsafe - one gateway operation, whether session-scoped or independent.
    Status Operation(uint32_t kind, uint64_t session, const std::string& key,
                     const std::string& value, std::string* output, bool* result);
    RemoteDB* db_;
    std::string name_;
    uint16_t table_; // zero means resolve the canonical name, not a guessed ID
};

class RemoteDB : public IDatabase {
    friend class RemoteTable;
public:
    // @unsafe - establishes the actual ClientTcpServer gateway connection.
    static Status Connect(const Options& options, int shard, RemoteDB** output) {
        if (!options.client.enabled || shard < 0
            || size_t(shard) >= options.client.server_hosts.size()
            || options.client.server_hosts.size() != options.client.server_ports.size())
            return Status::InvalidArgument("invalid remote client configuration");
        RemoteOptions remote;
        remote.server_host = options.client.server_hosts[shard];
        remote.server_port = options.client.server_ports[shard];
        remote.shard_index = shard;
        remote.num_shards = int(options.client.server_hosts.size());
        remote.timeout_ms = options.client.timeout_ms;
        remote.client_id = options.client.client_id;
        remote.first_sequence = options.client.first_sequence;
        return Connect(remote, output);
    }
    // @unsafe - Rust validates the unique client namespace and nonzero sequence.
    static Status Connect(const RemoteOptions& options, RemoteDB** output) {
        if (!output) return Status::InvalidArgument("null database output");
        *output = nullptr;
        auto* client = mako_gateway_client_new(options.client_id, options.first_sequence);
        if (!client) return Status::InvalidArgument("client_id must be unique with high bit set; sequence must be nonzero");
        auto db = rusty::make_box<RemoteDB>();
        db->options_ = options;
        Status status = Status::OK();
        {
            auto state = db->connection_.lock().unwrap();
            state->client = client;
            db->connected_.store(true);
            status = db->EnsureSocket(*state);
        }
        if (!status.ok()) return status;
        *output = db.release();
        return Status::OK();
    }
    // @unsafe - nontransactional calls use the SAME identity-bearing gateway.
    static Status ConnectNontxn(const std::string& host, int port, uint64_t client_id,
                                RemoteDB** output) {
        RemoteOptions options;
        options.server_host = host;
        options.server_port = port;
        options.client_id = client_id;
        return Connect(options, output);
    }
    // @safe - synchronization/data-owner initialization.
    RemoteDB() = default;
    RemoteDB(const RemoteDB&) = delete;
    RemoteDB& operator=(const RemoteDB&) = delete;
    // @unsafe - release the socket and opaque Rust-owned client stream.
    ~RemoteDB() noexcept override {
        Disconnect();
        auto state = connection_.lock().unwrap();
        mako_gateway_client_free(state->client);
    }
    // @unsafe - borrowed table handles remain valid until database destruction.
    ITable* GetTable(const std::string& name) override { return GetTable(name, 0); }
    ITable* GetTable(const std::string& name, uint16_t table) {
        auto tables = tables_.lock().unwrap();
        for (const auto& entry : *tables)
            if (entry->GetName() == name && entry->GetTableId() == table) return entry.get();
        auto entry = rusty::make_box<RemoteTable>(this, name, table);
        auto* result = entry.get();
        tables->push(std::move(entry));
        return result;
    }
    // @unsafe - begin retries retain the caller's original identity in Rust.
    void* BeginTransaction() override {
        MakoGatewayResponse response{};
        const auto status = Operation(MAKO_GATEWAY_BEGIN, 0, 0, {}, {}, {}, response);
        return status.ok() ? reinterpret_cast<void*>(response.sequence) : nullptr;
    }
    // The inherited void API cannot return a status. LastOperationStatus and the
    // status-returning methods expose failures/unknown outcomes without pretending
    // a commit succeeded. Retry the SAME method/session while outcome is unknown.
    // @unsafe - legacy interface adapters; Operation retains its real status.
    void Commit(void* session) override { CommitStatus(session); }
    void Rollback(void* session) override { RollbackStatus(session); }
    Status CommitStatus(void* session) {
        if (!session) return Status::InvalidArgument("null session");
        MakoGatewayResponse response{};
        return Operation(MAKO_GATEWAY_COMMIT, reinterpret_cast<uint64_t>(session),
                         0, {}, {}, {}, response);
    }
    Status RollbackStatus(void* session) {
        if (!session) return Status::InvalidArgument("null session");
        MakoGatewayResponse response{};
        return Operation(MAKO_GATEWAY_ROLLBACK, reinterpret_cast<uint64_t>(session),
                         0, {}, {}, {}, response);
    }
    // @safe - synchronized copy of the actual last operation status.
    Status LastOperationStatus() const {
        auto state = connection_.lock().unwrap();
        return state->last_status;
    }
    bool IsConnected() const override { return connected_.load(); }
    // @unsafe - reconnect retains the SAME native pending-operation state.
    Status Connect() override {
        auto state = connection_.lock().unwrap();
        connected_.store(true);
        return EnsureSocket(*state);
    }
    void Disconnect() override {
        auto state = connection_.lock().unwrap();
        connected_.store(false);
        CloseSocket(*state);
    }
    // @safe - engine thread registration belongs to the server worker.
    void InitThread() override {}
private:
    struct Connection {
        int fd = -1;
        MakoGatewayClient* client = nullptr; // opaque Rust allocation, owned by DB
        Status last_status = Status::OK();
    };
    // @unsafe - socket lifetime kernel; caller holds the connection guard.
    static void CloseSocket(Connection& state) {
        if (state.fd >= 0) ::close(state.fd);
        state.fd = -1;
    }
    // @unsafe - bounded POSIX stream I/O; partial writes preserve ambiguity.
    static bool WriteAll(int fd, const void* input, size_t length) {
        auto* bytes = static_cast<const char*>(input);
        while (length) {
            const auto written = ::send(fd, bytes, length, MSG_NOSIGNAL);
            if (written <= 0) return false;
            bytes += written; length -= size_t(written);
        }
        return true;
    }
    static bool ReadAll(int fd, void* output, size_t length) {
        auto* bytes = static_cast<char*>(output);
        while (length) {
            const auto received = ::read(fd, bytes, length);
            if (received <= 0) return false;
            bytes += received; length -= size_t(received);
        }
        return true;
    }
    // @unsafe - resolve/connect a legacy TCP endpoint, no protocol state changes.
    Status EnsureSocket(Connection& state) {
        if (state.fd >= 0) return Status::OK();
        if (!connected_.load()) return Status::IOError("disconnected");
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        const auto port = std::to_string(options_.server_port);
        if (::getaddrinfo(options_.server_host.c_str(), port.c_str(), &hints, &addresses))
            return Status::IOError("gateway name resolution failed");
        for (auto* address = addresses; address; address = address->ai_next) {
            const int fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (fd < 0) continue;
            timeval timeout{};
            timeout.tv_sec = options_.timeout_ms / 1000;
            timeout.tv_usec = (options_.timeout_ms % 1000) * 1000;
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
                state.fd = fd;
                break;
            }
            ::close(fd);
        }
        ::freeaddrinfo(addresses);
        return state.fd >= 0 ? Status::OK() : Status::IOError("gateway connection failed");
    }
    // @unsafe - transport marshalling only. A lost reply never changes identity.
    Status RoundTrip(Connection& state, const MakoGatewayRequest& request,
                     MakoGatewayResponse& response) {
        auto status = EnsureSocket(state);
        if (!status.ok()) return status;
        const uint8_t kind = static_cast<uint8_t>(request.kind);
        const uint32_t length = offsetof(MakoGatewayRequest, value) + request.value_length;
        response = {};
        bool ok = WriteAll(state.fd, &kind, sizeof(kind))
            && WriteAll(state.fd, &length, sizeof(length))
            && WriteAll(state.fd, &request, length)
            && ReadAll(state.fd, &response, offsetof(MakoGatewayResponse, value));
        ok = ok && response.client == request.client && response.sequence == request.sequence
            && response.value_length <= sizeof(response.value);
        if (ok && response.value_length)
            ok = ReadAll(state.fd, response.value, response.value_length);
        if (!ok) {
            CloseSocket(state);
            return Status::IOError("gateway outcome unknown; retry identical operation");
        }
        return Status::OK();
    }
    // @unsafe - marshals intent; Rust owns allocation, pending retry and result
    // fences. This per-client I/O guard is never a server/lease/engine mutex.
    Status Operation(uint32_t kind, uint64_t session, uint16_t table,
                     const std::string& name, const std::string& key,
                     const std::string& value, MakoGatewayResponse& response) {
        auto state = connection_.lock().unwrap();
        if (key.size() > 64 || value.size() > MAKO_GATEWAY_VALUE_LIMIT || name.size() > 128)
            return state->last_status = Status::InvalidArgument("gateway field too large");
        MakoGatewayRequest intent{};
        intent.version = MAKO_GATEWAY_VERSION;
        intent.kind = kind; intent.session = session; intent.physical_table = table;
        intent.key_length = uint32_t(key.size());
        intent.value_length = uint32_t(value.size());
        intent.name_length = table == 0 ? uint32_t(name.size()) : 0;
        std::memcpy(intent.key, key.data(), key.size());
        std::memcpy(intent.value, value.data(), value.size());
        if (!table) std::memcpy(intent.name, name.data(), name.size());
        MakoGatewayRequest request{};
        if (mako_gateway_prepare(state->client, &intent, &request) != 0)
            return state->last_status = Status::InvalidArgument(
                "different operation still pending, or nonwrapping sequence exhausted");
        const bool storage = kind == MAKO_GATEWAY_PUT || kind == MAKO_GATEWAY_GET
            || kind == MAKO_GATEWAY_DELETE || kind == MAKO_GATEWAY_INSERT;
        if (storage && !request.route_known) {
            auto discovery = request;
            discovery.kind = MAKO_GATEWAY_ROUTE;
            auto status = RoundTrip(*state, discovery, response);
            if (!status.ok()) return state->last_status = status;
            if (response.status != 0) {
                mako_gateway_complete(state->client, &response);
                return state->last_status = Status::IOError("gateway table/route unavailable");
            }
            if (mako_gateway_set_route(state->client, &response, &request) != 0)
                return state->last_status = Status::IOError("invalid gateway route response");
        }
        // One reconnect/retry uses the SAME retained request and selected epoch.
        auto status = RoundTrip(*state, request, response);
        if (!status.ok()) status = RoundTrip(*state, request, response);
        if (!status.ok()) return state->last_status = status;
        if (mako_gateway_complete(state->client, &response) != 0)
            return state->last_status = Status::IOError("gateway operation outcome unknown or still in flight");
        if (response.status == 0) return state->last_status = Status::OK();
        if (response.status == 3 && kind == MAKO_GATEWAY_GET)
            return state->last_status = Status::NotFound();
        if (response.status == 4) return state->last_status = Status::Busy("gateway engine busy");
        return state->last_status = Status::IOError("gateway rejected/fenced operation");
    }
    RemoteOptions options_;
    std::atomic<bool> connected_{false};
    mutable rusty::Mutex<Connection> connection_{Connection{}};
    rusty::Mutex<rusty::Vec<rusty::Box<RemoteTable>>> tables_{rusty::Vec<rusty::Box<RemoteTable>>{}};
};

// @unsafe - legacy interface marshalling, with separately committed semantics.
inline Status RemoteTable::Operation(uint32_t kind, uint64_t session, const std::string& key,
                                    const std::string& value, std::string* output, bool* result) {
    if (!db_) return Status::InvalidArgument("invalid database");
    MakoGatewayResponse response{};
    auto status = db_->Operation(kind, session, table_, name_, key, value, response);
    if (status.ok()) {
        if (output) output->assign(reinterpret_cast<const char*>(response.value), response.value_length);
        if (result) *result = response.op_result != 0;
    }
    return status;
}
// @unsafe - session-scoped operations still commit independently.
inline Status RemoteTable::Put(void* session, const std::string& key, const std::string& value) {
    if (!session) return Status::InvalidArgument("null session");
    return Operation(MAKO_GATEWAY_PUT, reinterpret_cast<uint64_t>(session), key, value, nullptr, nullptr);
}
inline Status RemoteTable::Get(void* session, const std::string& key, std::string& value) {
    if (!session) return Status::InvalidArgument("null session");
    return Operation(MAKO_GATEWAY_GET, reinterpret_cast<uint64_t>(session), key, {}, &value, nullptr);
}
inline Status RemoteTable::Delete(void* session, const std::string& key) {
    if (!session) return Status::InvalidArgument("null session");
    return Operation(MAKO_GATEWAY_DELETE, reinterpret_cast<uint64_t>(session), key, {}, nullptr, nullptr);
}
inline Status RemoteTable::Insert(void* session, const std::string& key, const std::string& value) {
    if (!session) return Status::InvalidArgument("null session");
    bool inserted = false;
    auto status = Operation(MAKO_GATEWAY_INSERT, reinterpret_cast<uint64_t>(session), key, value, nullptr, &inserted);
    return !status.ok() || inserted ? status : Status::InvalidArgument("key already exists");
}
// @unsafe - no-session operations use the same sequential identity stream.
inline Status RemoteTable::Put(const std::string& key, const std::string& value) {
    return Operation(MAKO_GATEWAY_PUT, 0, key, value, nullptr, nullptr);
}
inline Status RemoteTable::Get(const std::string& key, std::string& value) {
    return Operation(MAKO_GATEWAY_GET, 0, key, {}, &value, nullptr);
}
inline Status RemoteTable::Insert(const std::string& key, const std::string& value) {
    bool inserted = false;
    auto status = Operation(MAKO_GATEWAY_INSERT, 0, key, value, nullptr, &inserted);
    return !status.ok() || inserted ? status : Status::InvalidArgument("key already exists");
}
inline Status RemoteTable::Delete(const std::string& key) {
    bool removed = false;
    auto status = Operation(MAKO_GATEWAY_DELETE, 0, key, {}, nullptr, &removed);
    return !status.ok() || removed ? status : Status::NotFound();
}
// @safe - existence preserves every error except a genuine absent-key result.
inline Status RemoteTable::Exists(void* session, const std::string& key, bool* exists) {
    if (!exists) return Status::InvalidArgument("null exists output");
    std::string value;
    auto status = Get(session, key, value);
    if (!status.ok() && !status.IsNotFound()) return status;
    *exists = status.ok();
    return Status::OK();
}
inline Status RemoteTable::Exists(const std::string& key, bool* exists) {
    if (!exists) return Status::InvalidArgument("null exists output");
    std::string value;
    auto status = Get(key, value);
    if (!status.ok() && !status.IsNotFound()) return status;
    *exists = status.ok();
    return Status::OK();
}
} // namespace mako
