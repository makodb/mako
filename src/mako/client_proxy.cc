// @safe - RRR RPC client proxy implementation for Mako client API
#include <std_compat.hpp>  // textual STL before `import std` (mixed with module → abi_tag clash)
#include "client_proxy.h"

namespace mako {

// ============================================================================
// Synchronous API Implementation
// ============================================================================

// @safe - Synchronous RPC call
rrr::i32 MakoClientProxy::BeginTxn(rrr::i64 client_id, rrr::i64* txn_id) {
    auto fu_result = async_BeginTxn(client_id);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    rrr::i32 ret = fu->get_error_code();
    if (ret == 0) {
        rrr::i32 status;
        rrr::deserialize_from(fu->get_reply(), *txn_id, status);
    }
    return ret;
}

// @safe - Synchronous RPC call
rrr::i32 MakoClientProxy::Commit(rrr::i64 txn_id) {
    auto fu_result = async_Commit(txn_id);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    rrr::i32 ret = fu->get_error_code();
    if (ret == 0) {
        rrr::i32 status;
        rrr::deserialize_from(fu->get_reply(), status);
    }
    return ret;
}

// @safe - Synchronous RPC call
rrr::i32 MakoClientProxy::Rollback(rrr::i64 txn_id) {
    auto fu_result = async_Rollback(txn_id);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    rrr::i32 ret = fu->get_error_code();
    if (ret == 0) {
        rrr::i32 status;
        rrr::deserialize_from(fu->get_reply(), status);
    }
    return ret;
}

// @safe - Synchronous RPC call
rrr::i32 MakoClientProxy::Put(rrr::i64 txn_id, rrr::i32 table_id,
                               const std::string& key, const std::string& value) {
    auto fu_result = async_Put(txn_id, table_id, key, value);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    rrr::i32 ret = fu->get_error_code();
    if (ret == 0) {
        rrr::i32 status;
        rrr::deserialize_from(fu->get_reply(), status);
    }
    return ret;
}

// @safe - Synchronous RPC call
rrr::i32 MakoClientProxy::Get(rrr::i64 txn_id, rrr::i32 table_id,
                               const std::string& key, std::string* value) {
    auto fu_result = async_Get(txn_id, table_id, key);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    rrr::i32 ret = fu->get_error_code();
    if (ret == 0) {
        rrr::i32 status;
        rrr::deserialize_from(fu->get_reply(), status, *value);
    }
    return ret;
}

// @safe - Synchronous RPC call
rrr::i32 MakoClientProxy::Delete(rrr::i64 txn_id, rrr::i32 table_id,
                                  const std::string& key) {
    auto fu_result = async_Delete(txn_id, table_id, key);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    rrr::i32 ret = fu->get_error_code();
    if (ret == 0) {
        rrr::i32 status;
        rrr::deserialize_from(fu->get_reply(), status);
    }
    return ret;
}

// ============================================================================
// Asynchronous API Implementation
// ============================================================================

// @safe - Returns Future for async handling
rrr::FutureResult MakoClientProxy::async_BeginTxn(rrr::i64 client_id,
                                                   const rrr::FutureAttr& attr) {
    return client_->request(MakoClientService::BEGIN_TXN, attr,
                           [&](rrr::BinaryWriteArchive& m) {
                               rrr::Serialize_::serialize(client_id, m);
                           });
}

// @safe - Returns Future for async handling
rrr::FutureResult MakoClientProxy::async_Commit(rrr::i64 txn_id,
                                                 const rrr::FutureAttr& attr) {
    return client_->request(MakoClientService::COMMIT, attr,
                           [&](rrr::BinaryWriteArchive& m) {
                               rrr::Serialize_::serialize(txn_id, m);
                           });
}

// @safe - Returns Future for async handling
rrr::FutureResult MakoClientProxy::async_Rollback(rrr::i64 txn_id,
                                                   const rrr::FutureAttr& attr) {
    return client_->request(MakoClientService::ROLLBACK, attr,
                           [&](rrr::BinaryWriteArchive& m) {
                               rrr::Serialize_::serialize(txn_id, m);
                           });
}

// @safe - Returns Future for async handling
rrr::FutureResult MakoClientProxy::async_Put(rrr::i64 txn_id, rrr::i32 table_id,
                                              const std::string& key, const std::string& value,
                                              const rrr::FutureAttr& attr) {
    return client_->request(MakoClientService::PUT, attr,
                           [&](rrr::BinaryWriteArchive& m) {
                               rrr::Serialize_::serialize(txn_id, m);
                               rrr::Serialize_::serialize(table_id, m);
                               rrr::Serialize_::serialize(key, m);
                               rrr::Serialize_::serialize(value, m);
                           });
}

// @safe - Returns Future for async handling
rrr::FutureResult MakoClientProxy::async_Get(rrr::i64 txn_id, rrr::i32 table_id,
                                              const std::string& key,
                                              const rrr::FutureAttr& attr) {
    return client_->request(MakoClientService::GET, attr,
                           [&](rrr::BinaryWriteArchive& m) {
                               rrr::Serialize_::serialize(txn_id, m);
                               rrr::Serialize_::serialize(table_id, m);
                               rrr::Serialize_::serialize(key, m);
                           });
}

// @safe - Returns Future for async handling
rrr::FutureResult MakoClientProxy::async_Delete(rrr::i64 txn_id, rrr::i32 table_id,
                                                 const std::string& key,
                                                 const rrr::FutureAttr& attr) {
    return client_->request(MakoClientService::DELETE_KEY, attr,
                           [&](rrr::BinaryWriteArchive& m) {
                               rrr::Serialize_::serialize(txn_id, m);
                               rrr::Serialize_::serialize(table_id, m);
                               rrr::Serialize_::serialize(key, m);
                           });
}

} // namespace mako
