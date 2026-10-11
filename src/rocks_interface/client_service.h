#pragma once
#include <rusty/rusty.hpp>
#include "srpc/srpc.hpp"
#include "gateway_protocol.h"

namespace mako {
class ShardReceiver;
// The SRPC and raw TCP gateways carry the same versioned operation envelope.
// Session control does not promise multi-operation atomicity; every KV call is
// separately committed. No handler creates a client/operation identity.
class MakoClientService {
public:
    static constexpr srpc::i32 BEGIN_TXN = MAKO_GATEWAY_BEGIN;
    static constexpr srpc::i32 COMMIT = MAKO_GATEWAY_COMMIT;
    static constexpr srpc::i32 ROLLBACK = MAKO_GATEWAY_ROLLBACK;
    static constexpr srpc::i32 PUT = MAKO_GATEWAY_PUT;
    static constexpr srpc::i32 GET = MAKO_GATEWAY_GET;
    static constexpr srpc::i32 DELETE_KEY = MAKO_GATEWAY_DELETE;
    static constexpr srpc::i32 ROUTE = MAKO_GATEWAY_ROUTE;
    static constexpr srpc::i32 INSERT = MAKO_GATEWAY_INSERT;
    // @safe - receiver has server lifetime.
    explicit MakoClientService(ShardReceiver* receiver) : receiver_(receiver) {}
    // @unsafe - registers and dispatches the legacy SRPC transport boundary.
    int __reg_to__(srpc::Server& server, size_t svc_index);
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req,
                      srpc::WeakServerConnection sconn) const;
private:
    ShardReceiver* receiver_;
};
} // namespace mako
