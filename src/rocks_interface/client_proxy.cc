// @safe - SRPC RPC client proxy implementation for Mako client API
#include <std_compat.hpp>  // textual STL before `import std` (mixed with module → abi_tag clash)
#include "client_proxy.h"

namespace mako {

// ============================================================================
// Synchronous API Implementation
// ============================================================================

// @safe - Synchronous RPC call
srpc::i32 MakoClientProxy::BeginTxn(srpc::i64 client_id, srpc::i64* txn_id) {
    auto fu_result = async_BeginTxn(client_id);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    srpc::i32 ret = fu->get_error_code();
    if (ret == 0) {
        srpc::i32 status;
        srpc::deserialize_from(fu->get_reply(), *txn_id);
        srpc::deserialize_from(fu->get_reply(), status);
    }
    return ret;
}

// @safe - Synchronous RPC call
srpc::i32 MakoClientProxy::Commit(srpc::i64 txn_id) {
    auto fu_result = async_Commit(txn_id);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    srpc::i32 ret = fu->get_error_code();
    if (ret == 0) {
        srpc::i32 status;
        srpc::deserialize_from(fu->get_reply(), status);
    }
    return ret;
}

// @safe - Synchronous RPC call
srpc::i32 MakoClientProxy::Rollback(srpc::i64 txn_id) {
    auto fu_result = async_Rollback(txn_id);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    srpc::i32 ret = fu->get_error_code();
    if (ret == 0) {
        srpc::i32 status;
        srpc::deserialize_from(fu->get_reply(), status);
    }
    return ret;
}

// @safe - Synchronous RPC call
srpc::i32 MakoClientProxy::Put(srpc::i64 txn_id, srpc::i32 table_id,
                               const std::string& key, const std::string& value) {
    auto fu_result = async_Put(txn_id, table_id, key, value);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    srpc::i32 ret = fu->get_error_code();
    if (ret == 0) {
        srpc::i32 status;
        srpc::deserialize_from(fu->get_reply(), status);
    }
    return ret;
}

// @safe - Synchronous RPC call
srpc::i32 MakoClientProxy::Get(srpc::i64 txn_id, srpc::i32 table_id,
                               const std::string& key, std::string* value) {
    auto fu_result = async_Get(txn_id, table_id, key);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    srpc::i32 ret = fu->get_error_code();
    if (ret == 0) {
        srpc::i32 status;
        srpc::deserialize_from(fu->get_reply(), status);
        srpc::deserialize_from(fu->get_reply(), *value);
    }
    return ret;
}

// @safe - Synchronous RPC call
srpc::i32 MakoClientProxy::Delete(srpc::i64 txn_id, srpc::i32 table_id,
                                  const std::string& key) {
    auto fu_result = async_Delete(txn_id, table_id, key);
    if (fu_result.is_err()) {
        return fu_result.unwrap_err();
    }
    auto fu = fu_result.unwrap();
    srpc::i32 ret = fu->get_error_code();
    if (ret == 0) {
        srpc::i32 status;
        srpc::deserialize_from(fu->get_reply(), status);
    }
    return ret;
}

// ============================================================================
// Asynchronous API Implementation
// ============================================================================

// @safe - Returns Future for async handling
srpc::FutureResult MakoClientProxy::async_BeginTxn(srpc::i64 client_id,
                                                   const srpc::FutureAttr& attr) {
    return client_->request(MakoClientService::BEGIN_TXN, attr,
                           [&](srpc::BinaryWriteArchive& m) {
                               srpc::Serialize_::serialize(client_id, m);
                           });
}

// @safe - Returns Future for async handling
srpc::FutureResult MakoClientProxy::async_Commit(srpc::i64 txn_id,
                                                 const srpc::FutureAttr& attr) {
    return client_->request(MakoClientService::COMMIT, attr,
                           [&](srpc::BinaryWriteArchive& m) {
                               srpc::Serialize_::serialize(txn_id, m);
                           });
}

// @safe - Returns Future for async handling
srpc::FutureResult MakoClientProxy::async_Rollback(srpc::i64 txn_id,
                                                   const srpc::FutureAttr& attr) {
    return client_->request(MakoClientService::ROLLBACK, attr,
                           [&](srpc::BinaryWriteArchive& m) {
                               srpc::Serialize_::serialize(txn_id, m);
                           });
}

// @safe - Returns Future for async handling
srpc::FutureResult MakoClientProxy::async_Put(srpc::i64 txn_id, srpc::i32 table_id,
                                              const std::string& key, const std::string& value,
                                              const srpc::FutureAttr& attr) {
    return client_->request(MakoClientService::PUT, attr,
                           [&](srpc::BinaryWriteArchive& m) {
                               srpc::Serialize_::serialize(txn_id, m);
                               srpc::Serialize_::serialize(table_id, m);
                               srpc::Serialize_::serialize(key, m);
                               srpc::Serialize_::serialize(value, m);
                           });
}

// @safe - Returns Future for async handling
srpc::FutureResult MakoClientProxy::async_Get(srpc::i64 txn_id, srpc::i32 table_id,
                                              const std::string& key,
                                              const srpc::FutureAttr& attr) {
    return client_->request(MakoClientService::GET, attr,
                           [&](srpc::BinaryWriteArchive& m) {
                               srpc::Serialize_::serialize(txn_id, m);
                               srpc::Serialize_::serialize(table_id, m);
                               srpc::Serialize_::serialize(key, m);
                           });
}

// @safe - Returns Future for async handling
srpc::FutureResult MakoClientProxy::async_Delete(srpc::i64 txn_id, srpc::i32 table_id,
                                                 const std::string& key,
                                                 const srpc::FutureAttr& attr) {
    return client_->request(MakoClientService::DELETE_KEY, attr,
                           [&](srpc::BinaryWriteArchive& m) {
                               srpc::Serialize_::serialize(txn_id, m);
                               srpc::Serialize_::serialize(table_id, m);
                               srpc::Serialize_::serialize(key, m);
                           });
}

} // namespace mako
