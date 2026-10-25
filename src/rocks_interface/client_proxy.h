#pragma once
#include <rusty/arc.hpp>
#include "srpc/srpc.hpp"
#include "gateway_protocol.h"

namespace mako {
// Low-level SRPC transport. The caller supplies an already-created full operation
// envelope and resends exactly that envelope after an ambiguous failure. RemoteDB
// uses the native Rust pending-stream API to enforce this contract automatically.
class MakoClientProxy {
public:
    // @safe - shares the SRPC connection owner.
    explicit MakoClientProxy(rusty::Arc<srpc::Client> client) : client_(std::move(client)) {}
    // @unsafe - bounded transport wait; nonzero return leaves outcome unknown.
    srpc::i32 Call(const MakoGatewayRequest& request, MakoGatewayResponse* response,
                   uint32_t timeout_ms);
    srpc::FutureResult async_Call(const MakoGatewayRequest& request,
                                  const srpc::FutureAttr& attr = srpc::FutureAttr());
    // @unsafe - existing SRPC connection boundary.
    bool connected() const { return client_->connected(); }
    void close() { client_->close(); }
private:
    rusty::Arc<srpc::Client> client_;
};
} // namespace mako
