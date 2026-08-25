#pragma once

#include "srpc/srpc.hpp"
#include <rusty/async.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/result.hpp>

#include <errno.h>
#include <memory>

#include "procedure.h"
#include "rcc/tx.h"
#include "srpc/misc/any_message.hpp"  // graph fields are AnyMessage
namespace janus {

struct ValueTimesPair {
    srpc::i64 value;
    srpc::i64 times;
};

inline void serialize(const ValueTimesPair& o, srpc::BinaryWriteArchive& ar) {
    srpc::Serialize_::serialize(o.value, ar);
    srpc::Serialize_::serialize(o.times, ar);
}

inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const ValueTimesPair& o) { serialize(o, ar); return ar; }

inline void deserialize(ValueTimesPair& o, srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(o.value, ar);
    srpc::Deserialize_::deserialize(o.times, ar);
}

inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, ValueTimesPair& o) { deserialize(o, ar); return ar; }

struct DepId {
    std::string str;
    srpc::i64 id;
};

inline void serialize(const DepId& o, srpc::BinaryWriteArchive& ar) {
    srpc::Serialize_::serialize(o.str, ar);
    srpc::Serialize_::serialize(o.id, ar);
}

inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const DepId& o) { serialize(o, ar); return ar; }

inline void deserialize(DepId& o, srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(o.str, ar);
    srpc::Deserialize_::deserialize(o.id, ar);
}

inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, DepId& o) { deserialize(o, ar); return ar; }

struct TxnInfoRes {
    srpc::i32 start_txn;
    srpc::i32 total_txn;
    srpc::i32 total_try;
    srpc::i32 commit_txn;
    srpc::i32 num_exhausted;
    std::vector<double> this_latency;
    std::vector<double> last_latency;
    std::vector<double> attempt_latency;
    std::vector<double> interval_latency;
    std::vector<double> all_interval_latency;
    std::vector<srpc::i32> num_try;
};

inline void serialize(const TxnInfoRes& o, srpc::BinaryWriteArchive& ar) {
    srpc::Serialize_::serialize(o.start_txn, ar);
    srpc::Serialize_::serialize(o.total_txn, ar);
    srpc::Serialize_::serialize(o.total_try, ar);
    srpc::Serialize_::serialize(o.commit_txn, ar);
    srpc::Serialize_::serialize(o.num_exhausted, ar);
    srpc::Serialize_::serialize(o.this_latency, ar);
    srpc::Serialize_::serialize(o.last_latency, ar);
    srpc::Serialize_::serialize(o.attempt_latency, ar);
    srpc::Serialize_::serialize(o.interval_latency, ar);
    srpc::Serialize_::serialize(o.all_interval_latency, ar);
    srpc::Serialize_::serialize(o.num_try, ar);
}

inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const TxnInfoRes& o) { serialize(o, ar); return ar; }

inline void deserialize(TxnInfoRes& o, srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(o.start_txn, ar);
    srpc::Deserialize_::deserialize(o.total_txn, ar);
    srpc::Deserialize_::deserialize(o.total_try, ar);
    srpc::Deserialize_::deserialize(o.commit_txn, ar);
    srpc::Deserialize_::deserialize(o.num_exhausted, ar);
    srpc::Deserialize_::deserialize(o.this_latency, ar);
    srpc::Deserialize_::deserialize(o.last_latency, ar);
    srpc::Deserialize_::deserialize(o.attempt_latency, ar);
    srpc::Deserialize_::deserialize(o.interval_latency, ar);
    srpc::Deserialize_::deserialize(o.all_interval_latency, ar);
    srpc::Deserialize_::deserialize(o.num_try, ar);
}

inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, TxnInfoRes& o) { deserialize(o, ar); return ar; }

struct ServerResponse {
    std::map<std::string, ValueTimesPair> statistics;
    srpc::i64 r_cnt_sum;
    srpc::i64 r_cnt_num;
    srpc::i64 r_sz_sum;
    srpc::i64 r_sz_num;
};

inline void serialize(const ServerResponse& o, srpc::BinaryWriteArchive& ar) {
    srpc::Serialize_::serialize(o.statistics, ar);
    srpc::Serialize_::serialize(o.r_cnt_sum, ar);
    srpc::Serialize_::serialize(o.r_cnt_num, ar);
    srpc::Serialize_::serialize(o.r_sz_sum, ar);
    srpc::Serialize_::serialize(o.r_sz_num, ar);
}

inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const ServerResponse& o) { serialize(o, ar); return ar; }

inline void deserialize(ServerResponse& o, srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(o.statistics, ar);
    srpc::Deserialize_::deserialize(o.r_cnt_sum, ar);
    srpc::Deserialize_::deserialize(o.r_cnt_num, ar);
    srpc::Deserialize_::deserialize(o.r_sz_sum, ar);
    srpc::Deserialize_::deserialize(o.r_sz_num, ar);
}

inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, ServerResponse& o) { deserialize(o, ar); return ar; }

struct ClientResponse {
    std::map<srpc::i32, TxnInfoRes> txn_info;
    srpc::i64 run_sec;
    srpc::i64 run_nsec;
    srpc::i64 period_sec;
    srpc::i64 period_nsec;
    srpc::i32 is_finish;
    srpc::i64 n_asking;
};

inline void serialize(const ClientResponse& o, srpc::BinaryWriteArchive& ar) {
    srpc::Serialize_::serialize(o.txn_info, ar);
    srpc::Serialize_::serialize(o.run_sec, ar);
    srpc::Serialize_::serialize(o.run_nsec, ar);
    srpc::Serialize_::serialize(o.period_sec, ar);
    srpc::Serialize_::serialize(o.period_nsec, ar);
    srpc::Serialize_::serialize(o.is_finish, ar);
    srpc::Serialize_::serialize(o.n_asking, ar);
}

inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const ClientResponse& o) { serialize(o, ar); return ar; }

inline void deserialize(ClientResponse& o, srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(o.txn_info, ar);
    srpc::Deserialize_::deserialize(o.run_sec, ar);
    srpc::Deserialize_::deserialize(o.run_nsec, ar);
    srpc::Deserialize_::deserialize(o.period_sec, ar);
    srpc::Deserialize_::deserialize(o.period_nsec, ar);
    srpc::Deserialize_::deserialize(o.is_finish, ar);
    srpc::Deserialize_::deserialize(o.n_asking, ar);
}

inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, ClientResponse& o) { deserialize(o, ar); return ar; }

struct TxDispatchRequest {
    srpc::i32 id;
    srpc::i32 tx_type;
    std::vector<Value> input;
};

inline void serialize(const TxDispatchRequest& o, srpc::BinaryWriteArchive& ar) {
    srpc::Serialize_::serialize(o.id, ar);
    srpc::Serialize_::serialize(o.tx_type, ar);
    srpc::Serialize_::serialize(o.input, ar);
}

inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const TxDispatchRequest& o) { serialize(o, ar); return ar; }

inline void deserialize(TxDispatchRequest& o, srpc::BinaryReadArchive& ar) {
    srpc::Deserialize_::deserialize(o.id, ar);
    srpc::Deserialize_::deserialize(o.tx_type, ar);
    srpc::Deserialize_::deserialize(o.input, ar);
}

inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, TxDispatchRequest& o) { deserialize(o, ar); return ar; }

struct TxnDispatchResponse {
};

inline void serialize(const TxnDispatchResponse& o, srpc::BinaryWriteArchive& ar) {
}

inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const TxnDispatchResponse& o) { serialize(o, ar); return ar; }

inline void deserialize(TxnDispatchResponse& o, srpc::BinaryReadArchive& ar) {
}

inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, TxnDispatchResponse& o) { deserialize(o, ar); return ar; }

class MultiPaxosService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcPrepareRequest {
        uint64_t slot;
        ballot_t ballot;
    };
    friend inline void serialize(const RpcPrepareRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcPrepareRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcPrepareRequest& o) { deserialize(o, ar); return ar; }

    struct RpcPrepareResponse {
        ballot_t max_ballot;
        uint64_t coro_id;
    };
    friend inline void serialize(const RpcPrepareResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.max_ballot, ar);
        srpc::Serialize_::serialize(o.coro_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcPrepareResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.max_ballot, ar);
        srpc::Deserialize_::deserialize(o.coro_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcPrepareResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAcceptRequest {
        uint64_t slot;
        uint64_t time;
        ballot_t ballot;
        Command cmd;
    };
    friend inline void serialize(const RpcAcceptRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.time, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAcceptRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.time, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAcceptResponse {
        ballot_t max_ballot;
        uint64_t coro_id;
    };
    friend inline void serialize(const RpcAcceptResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.max_ballot, ar);
        srpc::Serialize_::serialize(o.coro_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAcceptResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.max_ballot, ar);
        srpc::Deserialize_::deserialize(o.coro_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcDecideRequest {
        uint64_t slot;
        ballot_t ballot;
        Command cmd;
    };
    friend inline void serialize(const RpcDecideRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDecideRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDecideRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDecideRequest& o) { deserialize(o, ar); return ar; }

    struct RpcDecideResponse {
    };
    friend inline void serialize(const RpcDecideResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDecideResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDecideResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDecideResponse& o) { deserialize(o, ar); return ar; }

    struct RpcForwardToLearnerServerRequest {
        srpc::i32 par_id;
        uint64_t slot;
        ballot_t ballot;
        Command cmd;
    };
    friend inline void serialize(const RpcForwardToLearnerServerRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.par_id, ar);
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcForwardToLearnerServerRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcForwardToLearnerServerRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.par_id, ar);
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcForwardToLearnerServerRequest& o) { deserialize(o, ar); return ar; }

    struct RpcForwardToLearnerServerResponse {
        uint64_t ret_slot;
        ballot_t ret_ballot;
    };
    friend inline void serialize(const RpcForwardToLearnerServerResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ret_slot, ar);
        srpc::Serialize_::serialize(o.ret_ballot, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcForwardToLearnerServerResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcForwardToLearnerServerResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ret_slot, ar);
        srpc::Deserialize_::deserialize(o.ret_ballot, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcForwardToLearnerServerResponse& o) { deserialize(o, ar); return ar; }

    struct RpcBulkAcceptRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcBulkAcceptRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcBulkAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkAcceptRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcBulkAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcBulkAcceptResponse {
        srpc::i32 ballot;
        srpc::i32 val;
    };
    friend inline void serialize(const RpcBulkAcceptResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.val, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcBulkAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkAcceptResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.val, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcBulkAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcSyncLogRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcSyncLogRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcSyncLogRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncLogRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcSyncLogRequest& o) { deserialize(o, ar); return ar; }

    struct RpcSyncLogResponse {
        srpc::i32 ballot;
        srpc::i32 val;
        Command ret;
    };
    friend inline void serialize(const RpcSyncLogResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.val, ar);
        srpc::Serialize_::serialize(o.ret, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcSyncLogResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncLogResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.val, ar);
        srpc::Deserialize_::deserialize(o.ret, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcSyncLogResponse& o) { deserialize(o, ar); return ar; }

    struct RpcSyncCommitRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcSyncCommitRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcSyncCommitRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncCommitRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcSyncCommitRequest& o) { deserialize(o, ar); return ar; }

    struct RpcSyncCommitResponse {
        srpc::i32 ballot;
        srpc::i32 val;
    };
    friend inline void serialize(const RpcSyncCommitResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.val, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcSyncCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncCommitResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.val, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcSyncCommitResponse& o) { deserialize(o, ar); return ar; }

    struct RpcBulkDecideRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcBulkDecideRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcBulkDecideRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkDecideRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcBulkDecideRequest& o) { deserialize(o, ar); return ar; }

    struct RpcBulkDecideResponse {
        srpc::i32 ballot;
        srpc::i32 val;
    };
    friend inline void serialize(const RpcBulkDecideResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.val, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcBulkDecideResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkDecideResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.val, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcBulkDecideResponse& o) { deserialize(o, ar); return ar; }

    enum {
        PREPARE = 0x6f478870,
        ACCEPT = 0x20887b5b,
        DECIDE = 0x6ccaea21,
        FORWARDTOLEARNERSERVER = 0x4aedcaeb,
        BULKACCEPT = 0x18c1d1f9,
        SYNCLOG = 0x510c8e22,
        SYNCCOMMIT = 0x311f9a47,
        BULKDECIDE = 0x11c83c3a,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(PREPARE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(ACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(DECIDE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FORWARDTOLEARNERSERVER, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(BULKACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SYNCLOG, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SYNCCOMMIT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(BULKDECIDE, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(PREPARE);
        svr.unreg(ACCEPT);
        svr.unreg(DECIDE);
        svr.unreg(FORWARDTOLEARNERSERVER);
        svr.unreg(BULKACCEPT);
        svr.unreg(SYNCLOG);
        svr.unreg(SYNCCOMMIT);
        svr.unreg(BULKDECIDE);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case PREPARE: __Prepare__wrapper__(std::move(req), weak_sconn); break;
        case ACCEPT: __Accept__wrapper__(std::move(req), weak_sconn); break;
        case DECIDE: __Decide__wrapper__(std::move(req), weak_sconn); break;
        case FORWARDTOLEARNERSERVER: __ForwardToLearnerServer__wrapper__(std::move(req), weak_sconn); break;
        case BULKACCEPT: __BulkAccept__wrapper__(std::move(req), weak_sconn); break;
        case SYNCLOG: __SyncLog__wrapper__(std::move(req), weak_sconn); break;
        case SYNCCOMMIT: __SyncCommit__wrapper__(std::move(req), weak_sconn); break;
        case BULKDECIDE: __BulkDecide__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void Prepare(const RpcPrepareRequest& req, RpcPrepareResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void Accept(const RpcAcceptRequest& req, RpcAcceptResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void Decide(const RpcDecideRequest& req, RpcDecideResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req, RpcForwardToLearnerServerResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void BulkAccept(const RpcBulkAcceptRequest& req, RpcBulkAcceptResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void SyncLog(const RpcSyncLogRequest& req, RpcSyncLogResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void SyncCommit(const RpcSyncCommitRequest& req, RpcSyncCommitResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void BulkDecide(const RpcBulkDecideRequest& req, RpcBulkDecideResponse& resp, srpc::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Prepare__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPrepareRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcPrepareResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->coro_id, m);
                },
                []() {});
            this->Prepare(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Accept__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAcceptRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.time, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcAcceptResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->coro_id, m);
                },
                []() {});
            this->Accept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Decide__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDecideRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcDecideResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->Decide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __ForwardToLearnerServer__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcForwardToLearnerServerRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.par_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcForwardToLearnerServerResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ret_slot, m);
                    srpc::Serialize_::serialize(__typed_resp__->ret_ballot, m);
                },
                []() {});
            this->ForwardToLearnerServer(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkAccept__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcBulkAcceptRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcBulkAcceptResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->val, m);
                },
                []() {});
            this->BulkAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SyncLog__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSyncLogRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcSyncLogResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->val, m);
                    srpc::Serialize_::serialize(__typed_resp__->ret, m);
                },
                []() {});
            this->SyncLog(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SyncCommit__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSyncCommitRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcSyncCommitResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->val, m);
                },
                []() {});
            this->SyncCommit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkDecide__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcBulkDecideRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcBulkDecideResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->val, m);
                },
                []() {});
            this->BulkDecide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class MultiPaxosProxy {
protected:
    srpc::Client* __cl__;
public:
    MultiPaxosProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcPrepareRequest = MultiPaxosService::RpcPrepareRequest;
    using RpcPrepareResponse = MultiPaxosService::RpcPrepareResponse;
    using RpcAcceptRequest = MultiPaxosService::RpcAcceptRequest;
    using RpcAcceptResponse = MultiPaxosService::RpcAcceptResponse;
    using RpcDecideRequest = MultiPaxosService::RpcDecideRequest;
    using RpcDecideResponse = MultiPaxosService::RpcDecideResponse;
    using RpcForwardToLearnerServerRequest = MultiPaxosService::RpcForwardToLearnerServerRequest;
    using RpcForwardToLearnerServerResponse = MultiPaxosService::RpcForwardToLearnerServerResponse;
    using RpcBulkAcceptRequest = MultiPaxosService::RpcBulkAcceptRequest;
    using RpcBulkAcceptResponse = MultiPaxosService::RpcBulkAcceptResponse;
    using RpcSyncLogRequest = MultiPaxosService::RpcSyncLogRequest;
    using RpcSyncLogResponse = MultiPaxosService::RpcSyncLogResponse;
    using RpcSyncCommitRequest = MultiPaxosService::RpcSyncCommitRequest;
    using RpcSyncCommitResponse = MultiPaxosService::RpcSyncCommitResponse;
    using RpcBulkDecideRequest = MultiPaxosService::RpcBulkDecideRequest;
    using RpcBulkDecideResponse = MultiPaxosService::RpcBulkDecideResponse;
    class PrepareTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit PrepareTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcPrepareResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcPrepareResponse, srpc::i32>::Err(__ret__);
            }
            RpcPrepareResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            return rusty::Result<RpcPrepareResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<PrepareTypedFuture, srpc::i32> async_Prepare(const RpcPrepareRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::PREPARE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<PrepareTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<PrepareTypedFuture, srpc::i32>::Ok(PrepareTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcPrepareResponse, srpc::i32> Prepare(const RpcPrepareRequest& req) {
        auto __typed_fu_result__ = this->async_Prepare(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcPrepareResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AcceptTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit AcceptTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAcceptResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAcceptResponse, srpc::i32>::Err(__ret__);
            }
            RpcAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            return rusty::Result<RpcAcceptResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AcceptTypedFuture, srpc::i32> async_Accept(const RpcAcceptRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::ACCEPT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.time, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AcceptTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AcceptTypedFuture, srpc::i32>::Ok(AcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAcceptResponse, srpc::i32> Accept(const RpcAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_Accept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAcceptResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class DecideTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit DecideTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcDecideResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcDecideResponse, srpc::i32>::Err(__ret__);
            }
            RpcDecideResponse __typed_resp__;
            return rusty::Result<RpcDecideResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<DecideTypedFuture, srpc::i32> async_Decide(const RpcDecideRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::DECIDE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<DecideTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<DecideTypedFuture, srpc::i32>::Ok(DecideTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcDecideResponse, srpc::i32> Decide(const RpcDecideRequest& req) {
        auto __typed_fu_result__ = this->async_Decide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDecideResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class ForwardToLearnerServerTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit ForwardToLearnerServerTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcForwardToLearnerServerResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcForwardToLearnerServerResponse, srpc::i32>::Err(__ret__);
            }
            RpcForwardToLearnerServerResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ret_slot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.ret_ballot, __reply_ar__);
            return rusty::Result<RpcForwardToLearnerServerResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ForwardToLearnerServerTypedFuture, srpc::i32> async_ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::FORWARDTOLEARNERSERVER, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.par_id, __m__);
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<ForwardToLearnerServerTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<ForwardToLearnerServerTypedFuture, srpc::i32>::Ok(ForwardToLearnerServerTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcForwardToLearnerServerResponse, srpc::i32> ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req) {
        auto __typed_fu_result__ = this->async_ForwardToLearnerServer(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcForwardToLearnerServerResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class BulkAcceptTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit BulkAcceptTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcBulkAcceptResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcBulkAcceptResponse, srpc::i32>::Err(__ret__);
            }
            RpcBulkAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcBulkAcceptResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkAcceptTypedFuture, srpc::i32> async_BulkAccept(const RpcBulkAcceptRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKACCEPT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<BulkAcceptTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<BulkAcceptTypedFuture, srpc::i32>::Ok(BulkAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcBulkAcceptResponse, srpc::i32> BulkAccept(const RpcBulkAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_BulkAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcBulkAcceptResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class SyncLogTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit SyncLogTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcSyncLogResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSyncLogResponse, srpc::i32>::Err(__ret__);
            }
            RpcSyncLogResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.ret, __reply_ar__);
            return rusty::Result<RpcSyncLogResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SyncLogTypedFuture, srpc::i32> async_SyncLog(const RpcSyncLogRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::SYNCLOG, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SyncLogTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SyncLogTypedFuture, srpc::i32>::Ok(SyncLogTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSyncLogResponse, srpc::i32> SyncLog(const RpcSyncLogRequest& req) {
        auto __typed_fu_result__ = this->async_SyncLog(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSyncLogResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class SyncCommitTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit SyncCommitTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcSyncCommitResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSyncCommitResponse, srpc::i32>::Err(__ret__);
            }
            RpcSyncCommitResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcSyncCommitResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SyncCommitTypedFuture, srpc::i32> async_SyncCommit(const RpcSyncCommitRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::SYNCCOMMIT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SyncCommitTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SyncCommitTypedFuture, srpc::i32>::Ok(SyncCommitTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSyncCommitResponse, srpc::i32> SyncCommit(const RpcSyncCommitRequest& req) {
        auto __typed_fu_result__ = this->async_SyncCommit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSyncCommitResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class BulkDecideTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit BulkDecideTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcBulkDecideResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcBulkDecideResponse, srpc::i32>::Err(__ret__);
            }
            RpcBulkDecideResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcBulkDecideResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkDecideTypedFuture, srpc::i32> async_BulkDecide(const RpcBulkDecideRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKDECIDE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<BulkDecideTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<BulkDecideTypedFuture, srpc::i32>::Ok(BulkDecideTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcBulkDecideResponse, srpc::i32> BulkDecide(const RpcBulkDecideRequest& req) {
        auto __typed_fu_result__ = this->async_BulkDecide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcBulkDecideResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class FpgaRaftService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcHeartbeatRequest {
        uint64_t leaderPrevLogIndex;
        DepId dep_id;
    };
    friend inline void serialize(const RpcHeartbeatRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        srpc::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcHeartbeatRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcHeartbeatRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        srpc::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcHeartbeatRequest& o) { deserialize(o, ar); return ar; }

    struct RpcHeartbeatResponse {
        uint64_t followerPrevLogIndex;
    };
    friend inline void serialize(const RpcHeartbeatResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.followerPrevLogIndex, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcHeartbeatResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcHeartbeatResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.followerPrevLogIndex, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcHeartbeatResponse& o) { deserialize(o, ar); return ar; }

    struct RpcVoteRequest {
        uint64_t lst_log_idx;
        ballot_t lst_log_term;
        parid_t par_id;
        ballot_t cur_term;
    };
    friend inline void serialize(const RpcVoteRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.lst_log_idx, ar);
        srpc::Serialize_::serialize(o.lst_log_term, ar);
        srpc::Serialize_::serialize(o.par_id, ar);
        srpc::Serialize_::serialize(o.cur_term, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcVoteRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.lst_log_idx, ar);
        srpc::Deserialize_::deserialize(o.lst_log_term, ar);
        srpc::Deserialize_::deserialize(o.par_id, ar);
        srpc::Deserialize_::deserialize(o.cur_term, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcVoteRequest& o) { deserialize(o, ar); return ar; }

    struct RpcVoteResponse {
        ballot_t max_ballot;
        bool_t vote_granted;
    };
    friend inline void serialize(const RpcVoteResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.max_ballot, ar);
        srpc::Serialize_::serialize(o.vote_granted, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcVoteResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.max_ballot, ar);
        srpc::Deserialize_::deserialize(o.vote_granted, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcVoteResponse& o) { deserialize(o, ar); return ar; }

    struct RpcVote2FPGARequest {
        uint64_t lst_log_idx;
        ballot_t lst_log_term;
        parid_t par_id;
        ballot_t cur_term;
    };
    friend inline void serialize(const RpcVote2FPGARequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.lst_log_idx, ar);
        srpc::Serialize_::serialize(o.lst_log_term, ar);
        srpc::Serialize_::serialize(o.par_id, ar);
        srpc::Serialize_::serialize(o.cur_term, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcVote2FPGARequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVote2FPGARequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.lst_log_idx, ar);
        srpc::Deserialize_::deserialize(o.lst_log_term, ar);
        srpc::Deserialize_::deserialize(o.par_id, ar);
        srpc::Deserialize_::deserialize(o.cur_term, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcVote2FPGARequest& o) { deserialize(o, ar); return ar; }

    struct RpcVote2FPGAResponse {
        ballot_t max_ballot;
        bool_t vote_granted;
    };
    friend inline void serialize(const RpcVote2FPGAResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.max_ballot, ar);
        srpc::Serialize_::serialize(o.vote_granted, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcVote2FPGAResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVote2FPGAResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.max_ballot, ar);
        srpc::Deserialize_::deserialize(o.vote_granted, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcVote2FPGAResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesRequest {
        uint64_t slot;
        ballot_t ballot;
        uint64_t leaderCurrentTerm;
        uint64_t leaderPrevLogIndex;
        uint64_t leaderPrevLogTerm;
        uint64_t leaderCommitIndex;
        DepId dep_id;
        Command cmd;
    };
    friend inline void serialize(const RpcAppendEntriesRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.leaderCurrentTerm, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        srpc::Serialize_::serialize(o.leaderCommitIndex, ar);
        srpc::Serialize_::serialize(o.dep_id, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAppendEntriesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderCommitIndex, ar);
        srpc::Deserialize_::deserialize(o.dep_id, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAppendEntriesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
    };
    friend inline void serialize(const RpcAppendEntriesResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.followerAppendOK, ar);
        srpc::Serialize_::serialize(o.followerCurrentTerm, ar);
        srpc::Serialize_::serialize(o.followerLastLogIndex, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAppendEntriesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.followerAppendOK, ar);
        srpc::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.followerLastLogIndex, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAppendEntriesResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntries2Request {
        uint64_t slot;
        ballot_t ballot;
        uint64_t leaderCurrentTerm;
        uint64_t leaderPrevLogIndex;
        uint64_t leaderPrevLogTerm;
        uint64_t leaderCommitIndex;
        DepId dep_id;
        Command cmd;
    };
    friend inline void serialize(const RpcAppendEntries2Request& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.leaderCurrentTerm, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        srpc::Serialize_::serialize(o.leaderCommitIndex, ar);
        srpc::Serialize_::serialize(o.dep_id, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAppendEntries2Request& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntries2Request& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderCommitIndex, ar);
        srpc::Deserialize_::deserialize(o.dep_id, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAppendEntries2Request& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntries2Response {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
    };
    friend inline void serialize(const RpcAppendEntries2Response& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.followerAppendOK, ar);
        srpc::Serialize_::serialize(o.followerCurrentTerm, ar);
        srpc::Serialize_::serialize(o.followerLastLogIndex, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAppendEntries2Response& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntries2Response& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.followerAppendOK, ar);
        srpc::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.followerLastLogIndex, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAppendEntries2Response& o) { deserialize(o, ar); return ar; }

    struct RpcDecideRequest {
        uint64_t slot;
        ballot_t ballot;
        DepId dep_id;
        Command cmd;
    };
    friend inline void serialize(const RpcDecideRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.dep_id, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDecideRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDecideRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.dep_id, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDecideRequest& o) { deserialize(o, ar); return ar; }

    struct RpcDecideResponse {
    };
    friend inline void serialize(const RpcDecideResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDecideResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDecideResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDecideResponse& o) { deserialize(o, ar); return ar; }

    enum {
        HEARTBEAT = 0x58d23c6d,
        VOTE = 0x66092887,
        VOTE2FPGA = 0x50690d2a,
        APPENDENTRIES = 0x50d44f21,
        APPENDENTRIES2 = 0x56e3d907,
        DECIDE = 0x6b7e272c,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(HEARTBEAT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(VOTE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(VOTE2FPGA, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(APPENDENTRIES, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(APPENDENTRIES2, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(DECIDE, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(HEARTBEAT);
        svr.unreg(VOTE);
        svr.unreg(VOTE2FPGA);
        svr.unreg(APPENDENTRIES);
        svr.unreg(APPENDENTRIES2);
        svr.unreg(DECIDE);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case HEARTBEAT: __Heartbeat__wrapper__(std::move(req), weak_sconn); break;
        case VOTE: __Vote__wrapper__(std::move(req), weak_sconn); break;
        case VOTE2FPGA: __Vote2FPGA__wrapper__(std::move(req), weak_sconn); break;
        case APPENDENTRIES: __AppendEntries__wrapper__(std::move(req), weak_sconn); break;
        case APPENDENTRIES2: __AppendEntries2__wrapper__(std::move(req), weak_sconn); break;
        case DECIDE: __Decide__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void Heartbeat(const RpcHeartbeatRequest& req, RpcHeartbeatResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void Vote(const RpcVoteRequest& req, RpcVoteResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void Vote2FPGA(const RpcVote2FPGARequest& req, RpcVote2FPGAResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void AppendEntries(const RpcAppendEntriesRequest& req, RpcAppendEntriesResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void AppendEntries2(const RpcAppendEntries2Request& req, RpcAppendEntries2Response& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void Decide(const RpcDecideRequest& req, RpcDecideResponse& resp, srpc::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Heartbeat__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcHeartbeatRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcHeartbeatResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->followerPrevLogIndex, m);
                },
                []() {});
            this->Heartbeat(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Vote__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVoteRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.lst_log_idx, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.lst_log_term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.par_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cur_term, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcVoteResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->vote_granted, m);
                },
                []() {});
            this->Vote(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Vote2FPGA__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVote2FPGARequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.lst_log_idx, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.lst_log_term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.par_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cur_term, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcVote2FPGAResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->vote_granted, m);
                },
                []() {});
            this->Vote2FPGA(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __AppendEntries__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntriesRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcAppendEntriesResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->followerAppendOK, m);
                    srpc::Serialize_::serialize(__typed_resp__->followerCurrentTerm, m);
                    srpc::Serialize_::serialize(__typed_resp__->followerLastLogIndex, m);
                },
                []() {});
            this->AppendEntries(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __AppendEntries2__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntries2Request __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcAppendEntries2Response>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->followerAppendOK, m);
                    srpc::Serialize_::serialize(__typed_resp__->followerCurrentTerm, m);
                    srpc::Serialize_::serialize(__typed_resp__->followerLastLogIndex, m);
                },
                []() {});
            this->AppendEntries2(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Decide__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDecideRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcDecideResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->Decide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class FpgaRaftProxy {
protected:
    srpc::Client* __cl__;
public:
    FpgaRaftProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcHeartbeatRequest = FpgaRaftService::RpcHeartbeatRequest;
    using RpcHeartbeatResponse = FpgaRaftService::RpcHeartbeatResponse;
    using RpcVoteRequest = FpgaRaftService::RpcVoteRequest;
    using RpcVoteResponse = FpgaRaftService::RpcVoteResponse;
    using RpcVote2FPGARequest = FpgaRaftService::RpcVote2FPGARequest;
    using RpcVote2FPGAResponse = FpgaRaftService::RpcVote2FPGAResponse;
    using RpcAppendEntriesRequest = FpgaRaftService::RpcAppendEntriesRequest;
    using RpcAppendEntriesResponse = FpgaRaftService::RpcAppendEntriesResponse;
    using RpcAppendEntries2Request = FpgaRaftService::RpcAppendEntries2Request;
    using RpcAppendEntries2Response = FpgaRaftService::RpcAppendEntries2Response;
    using RpcDecideRequest = FpgaRaftService::RpcDecideRequest;
    using RpcDecideResponse = FpgaRaftService::RpcDecideResponse;
    class HeartbeatTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit HeartbeatTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcHeartbeatResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcHeartbeatResponse, srpc::i32>::Err(__ret__);
            }
            RpcHeartbeatResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.followerPrevLogIndex, __reply_ar__);
            return rusty::Result<RpcHeartbeatResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<HeartbeatTypedFuture, srpc::i32> async_Heartbeat(const RpcHeartbeatRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::HEARTBEAT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            srpc::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<HeartbeatTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<HeartbeatTypedFuture, srpc::i32>::Ok(HeartbeatTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcHeartbeatResponse, srpc::i32> Heartbeat(const RpcHeartbeatRequest& req) {
        auto __typed_fu_result__ = this->async_Heartbeat(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcHeartbeatResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class VoteTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit VoteTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcVoteResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcVoteResponse, srpc::i32>::Err(__ret__);
            }
            RpcVoteResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.vote_granted, __reply_ar__);
            return rusty::Result<RpcVoteResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<VoteTypedFuture, srpc::i32> async_Vote(const RpcVoteRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::VOTE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.lst_log_idx, __m__);
            srpc::Serialize_::serialize(req.lst_log_term, __m__);
            srpc::Serialize_::serialize(req.par_id, __m__);
            srpc::Serialize_::serialize(req.cur_term, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<VoteTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<VoteTypedFuture, srpc::i32>::Ok(VoteTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcVoteResponse, srpc::i32> Vote(const RpcVoteRequest& req) {
        auto __typed_fu_result__ = this->async_Vote(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVoteResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class Vote2FPGATypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit Vote2FPGATypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcVote2FPGAResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcVote2FPGAResponse, srpc::i32>::Err(__ret__);
            }
            RpcVote2FPGAResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.vote_granted, __reply_ar__);
            return rusty::Result<RpcVote2FPGAResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<Vote2FPGATypedFuture, srpc::i32> async_Vote2FPGA(const RpcVote2FPGARequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::VOTE2FPGA, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.lst_log_idx, __m__);
            srpc::Serialize_::serialize(req.lst_log_term, __m__);
            srpc::Serialize_::serialize(req.par_id, __m__);
            srpc::Serialize_::serialize(req.cur_term, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<Vote2FPGATypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<Vote2FPGATypedFuture, srpc::i32>::Ok(Vote2FPGATypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcVote2FPGAResponse, srpc::i32> Vote2FPGA(const RpcVote2FPGARequest& req) {
        auto __typed_fu_result__ = this->async_Vote2FPGA(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVote2FPGAResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AppendEntriesTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit AppendEntriesTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAppendEntriesResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAppendEntriesResponse, srpc::i32>::Err(__ret__);
            }
            RpcAppendEntriesResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            return rusty::Result<RpcAppendEntriesResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntriesTypedFuture, srpc::i32> async_AppendEntries(const RpcAppendEntriesRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::APPENDENTRIES, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            srpc::Serialize_::serialize(req.leaderCommitIndex, __m__);
            srpc::Serialize_::serialize(req.dep_id, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntriesTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntriesTypedFuture, srpc::i32>::Ok(AppendEntriesTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAppendEntriesResponse, srpc::i32> AppendEntries(const RpcAppendEntriesRequest& req) {
        auto __typed_fu_result__ = this->async_AppendEntries(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntriesResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AppendEntries2TypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit AppendEntries2TypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAppendEntries2Response, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAppendEntries2Response, srpc::i32>::Err(__ret__);
            }
            RpcAppendEntries2Response __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            return rusty::Result<RpcAppendEntries2Response, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntries2TypedFuture, srpc::i32> async_AppendEntries2(const RpcAppendEntries2Request& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::APPENDENTRIES2, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            srpc::Serialize_::serialize(req.leaderCommitIndex, __m__);
            srpc::Serialize_::serialize(req.dep_id, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntries2TypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntries2TypedFuture, srpc::i32>::Ok(AppendEntries2TypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAppendEntries2Response, srpc::i32> AppendEntries2(const RpcAppendEntries2Request& req) {
        auto __typed_fu_result__ = this->async_AppendEntries2(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntries2Response, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class DecideTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit DecideTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcDecideResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcDecideResponse, srpc::i32>::Err(__ret__);
            }
            RpcDecideResponse __typed_resp__;
            return rusty::Result<RpcDecideResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<DecideTypedFuture, srpc::i32> async_Decide(const RpcDecideRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::DECIDE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.dep_id, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<DecideTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<DecideTypedFuture, srpc::i32>::Ok(DecideTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcDecideResponse, srpc::i32> Decide(const RpcDecideRequest& req) {
        auto __typed_fu_result__ = this->async_Decide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDecideResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class RaftService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcVoteRequest {
        uint64_t lst_log_idx;
        ballot_t lst_log_term;
        siteid_t site_id;
        ballot_t cur_term;
    };
    friend inline void serialize(const RpcVoteRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.lst_log_idx, ar);
        srpc::Serialize_::serialize(o.lst_log_term, ar);
        srpc::Serialize_::serialize(o.site_id, ar);
        srpc::Serialize_::serialize(o.cur_term, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcVoteRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.lst_log_idx, ar);
        srpc::Deserialize_::deserialize(o.lst_log_term, ar);
        srpc::Deserialize_::deserialize(o.site_id, ar);
        srpc::Deserialize_::deserialize(o.cur_term, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcVoteRequest& o) { deserialize(o, ar); return ar; }

    struct RpcVoteResponse {
        ballot_t max_ballot;
        bool_t vote_granted;
    };
    friend inline void serialize(const RpcVoteResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.max_ballot, ar);
        srpc::Serialize_::serialize(o.vote_granted, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcVoteResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.max_ballot, ar);
        srpc::Deserialize_::deserialize(o.vote_granted, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcVoteResponse& o) { deserialize(o, ar); return ar; }

    struct RpcVoteDurableRequest {
        ballot_t term;
        siteid_t voter_id;
    };
    friend inline void serialize(const RpcVoteDurableRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.term, ar);
        srpc::Serialize_::serialize(o.voter_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcVoteDurableRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteDurableRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.term, ar);
        srpc::Deserialize_::deserialize(o.voter_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcVoteDurableRequest& o) { deserialize(o, ar); return ar; }

    struct RpcVoteDurableResponse {
        bool_t acknowledged;
    };
    friend inline void serialize(const RpcVoteDurableResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.acknowledged, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcVoteDurableResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteDurableResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.acknowledged, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcVoteDurableResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesRequest {
        uint64_t slot;
        ballot_t ballot;
        uint64_t leaderCurrentTerm;
        siteid_t leaderSiteId;
        uint64_t leaderPrevLogIndex;
        uint64_t leaderPrevLogTerm;
        uint64_t leaderCommitIndex;
        Command cmd;
        uint64_t leaderNextLogTerm;
    };
    friend inline void serialize(const RpcAppendEntriesRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.leaderCurrentTerm, ar);
        srpc::Serialize_::serialize(o.leaderSiteId, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        srpc::Serialize_::serialize(o.leaderCommitIndex, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
        srpc::Serialize_::serialize(o.leaderNextLogTerm, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAppendEntriesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderSiteId, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderCommitIndex, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
        srpc::Deserialize_::deserialize(o.leaderNextLogTerm, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAppendEntriesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
        uint64_t followerAckType;
    };
    friend inline void serialize(const RpcAppendEntriesResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.followerAppendOK, ar);
        srpc::Serialize_::serialize(o.followerCurrentTerm, ar);
        srpc::Serialize_::serialize(o.followerLastLogIndex, ar);
        srpc::Serialize_::serialize(o.followerAckType, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAppendEntriesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.followerAppendOK, ar);
        srpc::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.followerLastLogIndex, ar);
        srpc::Deserialize_::deserialize(o.followerAckType, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAppendEntriesResponse& o) { deserialize(o, ar); return ar; }

    struct RpcEmptyAppendEntriesRequest {
        uint64_t slot;
        ballot_t ballot;
        uint64_t leaderCurrentTerm;
        siteid_t leaderSiteId;
        uint64_t leaderPrevLogIndex;
        uint64_t leaderPrevLogTerm;
        uint64_t leaderCommitIndex;
        bool_t trigger_election_now;
    };
    friend inline void serialize(const RpcEmptyAppendEntriesRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.leaderCurrentTerm, ar);
        srpc::Serialize_::serialize(o.leaderSiteId, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        srpc::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        srpc::Serialize_::serialize(o.leaderCommitIndex, ar);
        srpc::Serialize_::serialize(o.trigger_election_now, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcEmptyAppendEntriesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEmptyAppendEntriesRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderSiteId, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        srpc::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderCommitIndex, ar);
        srpc::Deserialize_::deserialize(o.trigger_election_now, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcEmptyAppendEntriesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcEmptyAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
        uint64_t followerAckType;
    };
    friend inline void serialize(const RpcEmptyAppendEntriesResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.followerAppendOK, ar);
        srpc::Serialize_::serialize(o.followerCurrentTerm, ar);
        srpc::Serialize_::serialize(o.followerLastLogIndex, ar);
        srpc::Serialize_::serialize(o.followerAckType, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcEmptyAppendEntriesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEmptyAppendEntriesResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.followerAppendOK, ar);
        srpc::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        srpc::Deserialize_::deserialize(o.followerLastLogIndex, ar);
        srpc::Deserialize_::deserialize(o.followerAckType, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcEmptyAppendEntriesResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesDurableRequest {
        ballot_t term;
        siteid_t follower_id;
        uint64_t lastLogIndex;
    };
    friend inline void serialize(const RpcAppendEntriesDurableRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.term, ar);
        srpc::Serialize_::serialize(o.follower_id, ar);
        srpc::Serialize_::serialize(o.lastLogIndex, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAppendEntriesDurableRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesDurableRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.term, ar);
        srpc::Deserialize_::deserialize(o.follower_id, ar);
        srpc::Deserialize_::deserialize(o.lastLogIndex, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAppendEntriesDurableRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesDurableResponse {
        bool_t acknowledged;
    };
    friend inline void serialize(const RpcAppendEntriesDurableResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.acknowledged, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAppendEntriesDurableResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesDurableResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.acknowledged, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAppendEntriesDurableResponse& o) { deserialize(o, ar); return ar; }

    struct RpcTimeoutNowRequest {
        uint64_t leaderTerm;
        siteid_t leaderSiteId;
    };
    friend inline void serialize(const RpcTimeoutNowRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.leaderTerm, ar);
        srpc::Serialize_::serialize(o.leaderSiteId, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTimeoutNowRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTimeoutNowRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.leaderTerm, ar);
        srpc::Deserialize_::deserialize(o.leaderSiteId, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTimeoutNowRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTimeoutNowResponse {
        uint64_t followerTerm;
        bool_t success;
    };
    friend inline void serialize(const RpcTimeoutNowResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.followerTerm, ar);
        srpc::Serialize_::serialize(o.success, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTimeoutNowResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTimeoutNowResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.followerTerm, ar);
        srpc::Deserialize_::deserialize(o.success, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTimeoutNowResponse& o) { deserialize(o, ar); return ar; }

    struct RpcNotifyRestartRequest {
        siteid_t restartedSiteId;
    };
    friend inline void serialize(const RpcNotifyRestartRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.restartedSiteId, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcNotifyRestartRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcNotifyRestartRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.restartedSiteId, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcNotifyRestartRequest& o) { deserialize(o, ar); return ar; }

    struct RpcNotifyRestartResponse {
        bool_t acknowledged;
    };
    friend inline void serialize(const RpcNotifyRestartResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.acknowledged, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcNotifyRestartResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcNotifyRestartResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.acknowledged, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcNotifyRestartResponse& o) { deserialize(o, ar); return ar; }

    struct RpcInstallSnapshotRequest {
        uint64_t term;
        uint64_t leader_id;
        uint64_t last_included_index;
        uint64_t last_included_term;
        std::string data;
    };
    friend inline void serialize(const RpcInstallSnapshotRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.term, ar);
        srpc::Serialize_::serialize(o.leader_id, ar);
        srpc::Serialize_::serialize(o.last_included_index, ar);
        srpc::Serialize_::serialize(o.last_included_term, ar);
        srpc::Serialize_::serialize(o.data, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcInstallSnapshotRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcInstallSnapshotRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.term, ar);
        srpc::Deserialize_::deserialize(o.leader_id, ar);
        srpc::Deserialize_::deserialize(o.last_included_index, ar);
        srpc::Deserialize_::deserialize(o.last_included_term, ar);
        srpc::Deserialize_::deserialize(o.data, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcInstallSnapshotRequest& o) { deserialize(o, ar); return ar; }

    struct RpcInstallSnapshotResponse {
        uint64_t term_out;
    };
    friend inline void serialize(const RpcInstallSnapshotResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.term_out, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcInstallSnapshotResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcInstallSnapshotResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.term_out, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcInstallSnapshotResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAddServerRequest {
        uint64_t term;
        uint64_t new_server_id;
        std::string new_server_addr;
    };
    friend inline void serialize(const RpcAddServerRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.term, ar);
        srpc::Serialize_::serialize(o.new_server_id, ar);
        srpc::Serialize_::serialize(o.new_server_addr, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAddServerRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAddServerRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.term, ar);
        srpc::Deserialize_::deserialize(o.new_server_id, ar);
        srpc::Deserialize_::deserialize(o.new_server_addr, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAddServerRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAddServerResponse {
        bool_t success;
        std::string error_msg;
        uint64_t leader_hint;
    };
    friend inline void serialize(const RpcAddServerResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.success, ar);
        srpc::Serialize_::serialize(o.error_msg, ar);
        srpc::Serialize_::serialize(o.leader_hint, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAddServerResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAddServerResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.success, ar);
        srpc::Deserialize_::deserialize(o.error_msg, ar);
        srpc::Deserialize_::deserialize(o.leader_hint, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAddServerResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRemoveServerRequest {
        uint64_t term;
        uint64_t server_id;
    };
    friend inline void serialize(const RpcRemoveServerRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.term, ar);
        srpc::Serialize_::serialize(o.server_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRemoveServerRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRemoveServerRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.term, ar);
        srpc::Deserialize_::deserialize(o.server_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRemoveServerRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRemoveServerResponse {
        bool_t success;
        std::string error_msg;
        uint64_t leader_hint;
    };
    friend inline void serialize(const RpcRemoveServerResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.success, ar);
        srpc::Serialize_::serialize(o.error_msg, ar);
        srpc::Serialize_::serialize(o.leader_hint, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRemoveServerResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRemoveServerResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.success, ar);
        srpc::Deserialize_::deserialize(o.error_msg, ar);
        srpc::Deserialize_::deserialize(o.leader_hint, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRemoveServerResponse& o) { deserialize(o, ar); return ar; }

    enum {
        VOTE = 0x2802b911,
        VOTEDURABLE = 0x538739a2,
        APPENDENTRIES = 0x3935326f,
        EMPTYAPPENDENTRIES = 0x6e089268,
        APPENDENTRIESDURABLE = 0x1e8b9027,
        TIMEOUTNOW = 0x59a6a5f9,
        NOTIFYRESTART = 0x3df03452,
        INSTALLSNAPSHOT = 0x5276442f,
        ADDSERVER = 0x10e10b20,
        REMOVESERVER = 0x68ea2fc0,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(VOTE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(VOTEDURABLE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(APPENDENTRIES, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(EMPTYAPPENDENTRIES, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(APPENDENTRIESDURABLE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TIMEOUTNOW, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(NOTIFYRESTART, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(INSTALLSNAPSHOT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(ADDSERVER, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(REMOVESERVER, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(VOTE);
        svr.unreg(VOTEDURABLE);
        svr.unreg(APPENDENTRIES);
        svr.unreg(EMPTYAPPENDENTRIES);
        svr.unreg(APPENDENTRIESDURABLE);
        svr.unreg(TIMEOUTNOW);
        svr.unreg(NOTIFYRESTART);
        svr.unreg(INSTALLSNAPSHOT);
        svr.unreg(ADDSERVER);
        svr.unreg(REMOVESERVER);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case VOTE: __Vote__wrapper__(std::move(req), weak_sconn); break;
        case VOTEDURABLE: __VoteDurable__wrapper__(std::move(req), weak_sconn); break;
        case APPENDENTRIES: __AppendEntries__wrapper__(std::move(req), weak_sconn); break;
        case EMPTYAPPENDENTRIES: __EmptyAppendEntries__wrapper__(std::move(req), weak_sconn); break;
        case APPENDENTRIESDURABLE: __AppendEntriesDurable__wrapper__(std::move(req), weak_sconn); break;
        case TIMEOUTNOW: __TimeoutNow__wrapper__(std::move(req), weak_sconn); break;
        case NOTIFYRESTART: __NotifyRestart__wrapper__(std::move(req), weak_sconn); break;
        case INSTALLSNAPSHOT: __InstallSnapshot__wrapper__(std::move(req), weak_sconn); break;
        case ADDSERVER: __AddServer__wrapper__(std::move(req), weak_sconn); break;
        case REMOVESERVER: __RemoveServer__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual rusty::Result<RpcVoteResponse, srpc::i32> Vote(const RpcVoteRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcVoteDurableResponse, srpc::i32> VoteDurable(const RpcVoteDurableRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcAppendEntriesResponse, srpc::i32> AppendEntries(const RpcAppendEntriesRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcAppendEntriesDurableResponse, srpc::i32> AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcTimeoutNowResponse, srpc::i32> TimeoutNow(const RpcTimeoutNowRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcNotifyRestartResponse, srpc::i32> NotifyRestart(const RpcNotifyRestartRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcInstallSnapshotResponse, srpc::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcAddServerResponse, srpc::i32> AddServer(const RpcAddServerRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcRemoveServerResponse, srpc::i32> RemoveServer(const RpcRemoveServerRequest& req) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Vote__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVoteRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.lst_log_idx, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.lst_log_term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.site_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cur_term, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->Vote(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.max_ballot, m);
                            srpc::Serialize_::serialize(__typed_resp__.vote_granted, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __VoteDurable__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVoteDurableRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.voter_id, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->VoteDurable(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.acknowledged, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __AppendEntries__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntriesRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderSiteId, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderNextLogTerm, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->AppendEntries(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.followerAppendOK, m);
                            srpc::Serialize_::serialize(__typed_resp__.followerCurrentTerm, m);
                            srpc::Serialize_::serialize(__typed_resp__.followerLastLogIndex, m);
                            srpc::Serialize_::serialize(__typed_resp__.followerAckType, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __EmptyAppendEntries__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcEmptyAppendEntriesRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderSiteId, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.trigger_election_now, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->EmptyAppendEntries(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.followerAppendOK, m);
                            srpc::Serialize_::serialize(__typed_resp__.followerCurrentTerm, m);
                            srpc::Serialize_::serialize(__typed_resp__.followerLastLogIndex, m);
                            srpc::Serialize_::serialize(__typed_resp__.followerAckType, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __AppendEntriesDurable__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntriesDurableRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.follower_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.lastLogIndex, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->AppendEntriesDurable(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.acknowledged, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __TimeoutNow__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTimeoutNowRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.leaderTerm, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leaderSiteId, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->TimeoutNow(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.followerTerm, m);
                            srpc::Serialize_::serialize(__typed_resp__.success, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __NotifyRestart__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcNotifyRestartRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.restartedSiteId, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->NotifyRestart(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.acknowledged, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __InstallSnapshot__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcInstallSnapshotRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.leader_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.last_included_index, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.last_included_term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.data, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->InstallSnapshot(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.term_out, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __AddServer__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAddServerRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.new_server_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.new_server_addr, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->AddServer(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.success, m);
                            srpc::Serialize_::serialize(__typed_resp__.error_msg, m);
                            srpc::Serialize_::serialize(__typed_resp__.leader_hint, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __RemoveServer__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRemoveServerRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.server_id, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->RemoveServer(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), srpc::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<srpc::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](srpc::BinaryWriteArchive& m) {
                            srpc::Serialize_::serialize(__typed_resp__.success, m);
                            srpc::Serialize_::serialize(__typed_resp__.error_msg, m);
                            srpc::Serialize_::serialize(__typed_resp__.leader_hint, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
};

class RaftProxy {
protected:
    srpc::Client* __cl__;
public:
    RaftProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcVoteRequest = RaftService::RpcVoteRequest;
    using RpcVoteResponse = RaftService::RpcVoteResponse;
    using RpcVoteDurableRequest = RaftService::RpcVoteDurableRequest;
    using RpcVoteDurableResponse = RaftService::RpcVoteDurableResponse;
    using RpcAppendEntriesRequest = RaftService::RpcAppendEntriesRequest;
    using RpcAppendEntriesResponse = RaftService::RpcAppendEntriesResponse;
    using RpcEmptyAppendEntriesRequest = RaftService::RpcEmptyAppendEntriesRequest;
    using RpcEmptyAppendEntriesResponse = RaftService::RpcEmptyAppendEntriesResponse;
    using RpcAppendEntriesDurableRequest = RaftService::RpcAppendEntriesDurableRequest;
    using RpcAppendEntriesDurableResponse = RaftService::RpcAppendEntriesDurableResponse;
    using RpcTimeoutNowRequest = RaftService::RpcTimeoutNowRequest;
    using RpcTimeoutNowResponse = RaftService::RpcTimeoutNowResponse;
    using RpcNotifyRestartRequest = RaftService::RpcNotifyRestartRequest;
    using RpcNotifyRestartResponse = RaftService::RpcNotifyRestartResponse;
    using RpcInstallSnapshotRequest = RaftService::RpcInstallSnapshotRequest;
    using RpcInstallSnapshotResponse = RaftService::RpcInstallSnapshotResponse;
    using RpcAddServerRequest = RaftService::RpcAddServerRequest;
    using RpcAddServerResponse = RaftService::RpcAddServerResponse;
    using RpcRemoveServerRequest = RaftService::RpcRemoveServerRequest;
    using RpcRemoveServerResponse = RaftService::RpcRemoveServerResponse;
    class VoteTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit VoteTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcVoteResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcVoteResponse, srpc::i32>::Err(__ret__);
            }
            RpcVoteResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.vote_granted, __reply_ar__);
            return rusty::Result<RpcVoteResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<VoteTypedFuture, srpc::i32> async_Vote(const RpcVoteRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::VOTE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.lst_log_idx, __m__);
            srpc::Serialize_::serialize(req.lst_log_term, __m__);
            srpc::Serialize_::serialize(req.site_id, __m__);
            srpc::Serialize_::serialize(req.cur_term, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<VoteTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<VoteTypedFuture, srpc::i32>::Ok(VoteTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcVoteResponse, srpc::i32> Vote(const RpcVoteRequest& req) {
        auto __typed_fu_result__ = this->async_Vote(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVoteResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class VoteDurableTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit VoteDurableTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcVoteDurableResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcVoteDurableResponse, srpc::i32>::Err(__ret__);
            }
            RpcVoteDurableResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcVoteDurableResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<VoteDurableTypedFuture, srpc::i32> async_VoteDurable(const RpcVoteDurableRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::VOTEDURABLE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.term, __m__);
            srpc::Serialize_::serialize(req.voter_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<VoteDurableTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<VoteDurableTypedFuture, srpc::i32>::Ok(VoteDurableTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcVoteDurableResponse, srpc::i32> VoteDurable(const RpcVoteDurableRequest& req) {
        auto __typed_fu_result__ = this->async_VoteDurable(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVoteDurableResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AppendEntriesTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit AppendEntriesTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAppendEntriesResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAppendEntriesResponse, srpc::i32>::Err(__ret__);
            }
            RpcAppendEntriesResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerAckType, __reply_ar__);
            return rusty::Result<RpcAppendEntriesResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntriesTypedFuture, srpc::i32> async_AppendEntries(const RpcAppendEntriesRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::APPENDENTRIES, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            srpc::Serialize_::serialize(req.leaderSiteId, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            srpc::Serialize_::serialize(req.leaderCommitIndex, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
            srpc::Serialize_::serialize(req.leaderNextLogTerm, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntriesTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntriesTypedFuture, srpc::i32>::Ok(AppendEntriesTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAppendEntriesResponse, srpc::i32> AppendEntries(const RpcAppendEntriesRequest& req) {
        auto __typed_fu_result__ = this->async_AppendEntries(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntriesResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class EmptyAppendEntriesTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit EmptyAppendEntriesTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32>::Err(__ret__);
            }
            RpcEmptyAppendEntriesResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.followerAckType, __reply_ar__);
            return rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<EmptyAppendEntriesTypedFuture, srpc::i32> async_EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::EMPTYAPPENDENTRIES, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            srpc::Serialize_::serialize(req.leaderSiteId, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            srpc::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            srpc::Serialize_::serialize(req.leaderCommitIndex, __m__);
            srpc::Serialize_::serialize(req.trigger_election_now, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<EmptyAppendEntriesTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<EmptyAppendEntriesTypedFuture, srpc::i32>::Ok(EmptyAppendEntriesTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) {
        auto __typed_fu_result__ = this->async_EmptyAppendEntries(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcEmptyAppendEntriesResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AppendEntriesDurableTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit AppendEntriesDurableTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAppendEntriesDurableResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAppendEntriesDurableResponse, srpc::i32>::Err(__ret__);
            }
            RpcAppendEntriesDurableResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcAppendEntriesDurableResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntriesDurableTypedFuture, srpc::i32> async_AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::APPENDENTRIESDURABLE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.term, __m__);
            srpc::Serialize_::serialize(req.follower_id, __m__);
            srpc::Serialize_::serialize(req.lastLogIndex, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntriesDurableTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntriesDurableTypedFuture, srpc::i32>::Ok(AppendEntriesDurableTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAppendEntriesDurableResponse, srpc::i32> AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) {
        auto __typed_fu_result__ = this->async_AppendEntriesDurable(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntriesDurableResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class TimeoutNowTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit TimeoutNowTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTimeoutNowResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTimeoutNowResponse, srpc::i32>::Err(__ret__);
            }
            RpcTimeoutNowResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.followerTerm, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            return rusty::Result<RpcTimeoutNowResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TimeoutNowTypedFuture, srpc::i32> async_TimeoutNow(const RpcTimeoutNowRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::TIMEOUTNOW, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.leaderTerm, __m__);
            srpc::Serialize_::serialize(req.leaderSiteId, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TimeoutNowTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TimeoutNowTypedFuture, srpc::i32>::Ok(TimeoutNowTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTimeoutNowResponse, srpc::i32> TimeoutNow(const RpcTimeoutNowRequest& req) {
        auto __typed_fu_result__ = this->async_TimeoutNow(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTimeoutNowResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class NotifyRestartTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit NotifyRestartTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcNotifyRestartResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcNotifyRestartResponse, srpc::i32>::Err(__ret__);
            }
            RpcNotifyRestartResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcNotifyRestartResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<NotifyRestartTypedFuture, srpc::i32> async_NotifyRestart(const RpcNotifyRestartRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::NOTIFYRESTART, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.restartedSiteId, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<NotifyRestartTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<NotifyRestartTypedFuture, srpc::i32>::Ok(NotifyRestartTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcNotifyRestartResponse, srpc::i32> NotifyRestart(const RpcNotifyRestartRequest& req) {
        auto __typed_fu_result__ = this->async_NotifyRestart(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcNotifyRestartResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class InstallSnapshotTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit InstallSnapshotTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcInstallSnapshotResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcInstallSnapshotResponse, srpc::i32>::Err(__ret__);
            }
            RpcInstallSnapshotResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.term_out, __reply_ar__);
            return rusty::Result<RpcInstallSnapshotResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<InstallSnapshotTypedFuture, srpc::i32> async_InstallSnapshot(const RpcInstallSnapshotRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::INSTALLSNAPSHOT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.term, __m__);
            srpc::Serialize_::serialize(req.leader_id, __m__);
            srpc::Serialize_::serialize(req.last_included_index, __m__);
            srpc::Serialize_::serialize(req.last_included_term, __m__);
            srpc::Serialize_::serialize(req.data, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<InstallSnapshotTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<InstallSnapshotTypedFuture, srpc::i32>::Ok(InstallSnapshotTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcInstallSnapshotResponse, srpc::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) {
        auto __typed_fu_result__ = this->async_InstallSnapshot(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcInstallSnapshotResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AddServerTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit AddServerTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAddServerResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAddServerResponse, srpc::i32>::Err(__ret__);
            }
            RpcAddServerResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.error_msg, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.leader_hint, __reply_ar__);
            return rusty::Result<RpcAddServerResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AddServerTypedFuture, srpc::i32> async_AddServer(const RpcAddServerRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::ADDSERVER, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.term, __m__);
            srpc::Serialize_::serialize(req.new_server_id, __m__);
            srpc::Serialize_::serialize(req.new_server_addr, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AddServerTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AddServerTypedFuture, srpc::i32>::Ok(AddServerTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAddServerResponse, srpc::i32> AddServer(const RpcAddServerRequest& req) {
        auto __typed_fu_result__ = this->async_AddServer(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAddServerResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RemoveServerTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit RemoveServerTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRemoveServerResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRemoveServerResponse, srpc::i32>::Err(__ret__);
            }
            RpcRemoveServerResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.error_msg, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.leader_hint, __reply_ar__);
            return rusty::Result<RpcRemoveServerResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RemoveServerTypedFuture, srpc::i32> async_RemoveServer(const RpcRemoveServerRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::REMOVESERVER, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.term, __m__);
            srpc::Serialize_::serialize(req.server_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RemoveServerTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RemoveServerTypedFuture, srpc::i32>::Ok(RemoveServerTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRemoveServerResponse, srpc::i32> RemoveServer(const RpcRemoveServerRequest& req) {
        auto __typed_fu_result__ = this->async_RemoveServer(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRemoveServerResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class CopilotService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcPrepareRequest {
        uint8_t is_pilot;
        uint64_t slot;
        ballot_t ballot;
        DepId dep_id;
    };
    friend inline void serialize(const RpcPrepareRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.is_pilot, ar);
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcPrepareRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.is_pilot, ar);
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcPrepareRequest& o) { deserialize(o, ar); return ar; }

    struct RpcPrepareResponse {
        Command ret_cmd;
        ballot_t max_ballot;
        uint64_t dep;
        status_t status;
    };
    friend inline void serialize(const RpcPrepareResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ret_cmd, ar);
        srpc::Serialize_::serialize(o.max_ballot, ar);
        srpc::Serialize_::serialize(o.dep, ar);
        srpc::Serialize_::serialize(o.status, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcPrepareResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ret_cmd, ar);
        srpc::Deserialize_::deserialize(o.max_ballot, ar);
        srpc::Deserialize_::deserialize(o.dep, ar);
        srpc::Deserialize_::deserialize(o.status, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcPrepareResponse& o) { deserialize(o, ar); return ar; }

    struct RpcFastAcceptRequest {
        uint8_t is_pilot;
        uint64_t slot;
        ballot_t ballot;
        uint64_t dep;
        Command cmd;
        DepId dep_id;
    };
    friend inline void serialize(const RpcFastAcceptRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.is_pilot, ar);
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.dep, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
        srpc::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastAcceptRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.is_pilot, ar);
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.dep, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
        srpc::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcFastAcceptResponse {
        ballot_t max_ballot;
        uint64_t ret_dep;
    };
    friend inline void serialize(const RpcFastAcceptResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.max_ballot, ar);
        srpc::Serialize_::serialize(o.ret_dep, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFastAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastAcceptResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.max_ballot, ar);
        srpc::Deserialize_::deserialize(o.ret_dep, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFastAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAcceptRequest {
        uint8_t is_pilot;
        uint64_t slot;
        ballot_t ballot;
        uint64_t dep;
        Command cmd;
        DepId dep_id;
    };
    friend inline void serialize(const RpcAcceptRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.is_pilot, ar);
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.dep, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
        srpc::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAcceptRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.is_pilot, ar);
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.dep, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
        srpc::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAcceptResponse {
        ballot_t max_ballot;
    };
    friend inline void serialize(const RpcAcceptResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.max_ballot, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAcceptResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.max_ballot, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcCommitRequest {
        uint8_t is_pilot;
        uint64_t slot;
        uint64_t dep;
        Command cmd;
    };
    friend inline void serialize(const RpcCommitRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.is_pilot, ar);
        srpc::Serialize_::serialize(o.slot, ar);
        srpc::Serialize_::serialize(o.dep, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcCommitRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcCommitRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.is_pilot, ar);
        srpc::Deserialize_::deserialize(o.slot, ar);
        srpc::Deserialize_::deserialize(o.dep, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcCommitRequest& o) { deserialize(o, ar); return ar; }

    struct RpcCommitResponse {
    };
    friend inline void serialize(const RpcCommitResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcCommitResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcCommitResponse& o) { deserialize(o, ar); return ar; }

    enum {
        PREPARE = 0x1466367d,
        FASTACCEPT = 0x35412f49,
        ACCEPT = 0x6f2ec138,
        COMMIT = 0x4424ed87,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(PREPARE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FASTACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(ACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(COMMIT, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(PREPARE);
        svr.unreg(FASTACCEPT);
        svr.unreg(ACCEPT);
        svr.unreg(COMMIT);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case PREPARE: __Prepare__wrapper__(std::move(req), weak_sconn); break;
        case FASTACCEPT: __FastAccept__wrapper__(std::move(req), weak_sconn); break;
        case ACCEPT: __Accept__wrapper__(std::move(req), weak_sconn); break;
        case COMMIT: __Commit__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void Prepare(const RpcPrepareRequest& req, RpcPrepareResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void FastAccept(const RpcFastAcceptRequest& req, RpcFastAcceptResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void Accept(const RpcAcceptRequest& req, RpcAcceptResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void Commit(const RpcCommitRequest& req, RpcCommitResponse& resp, srpc::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Prepare__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPrepareRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.is_pilot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcPrepareResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ret_cmd, m);
                    srpc::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->dep, m);
                    srpc::Serialize_::serialize(__typed_resp__->status, m);
                },
                []() {});
            this->Prepare(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __FastAccept__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastAcceptRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.is_pilot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcFastAcceptResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->ret_dep, m);
                },
                []() {});
            this->FastAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Accept__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAcceptRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.is_pilot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcAcceptResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->max_ballot, m);
                },
                []() {});
            this->Accept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Commit__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcCommitRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.is_pilot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcCommitResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->Commit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class CopilotProxy {
protected:
    srpc::Client* __cl__;
public:
    CopilotProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcPrepareRequest = CopilotService::RpcPrepareRequest;
    using RpcPrepareResponse = CopilotService::RpcPrepareResponse;
    using RpcFastAcceptRequest = CopilotService::RpcFastAcceptRequest;
    using RpcFastAcceptResponse = CopilotService::RpcFastAcceptResponse;
    using RpcAcceptRequest = CopilotService::RpcAcceptRequest;
    using RpcAcceptResponse = CopilotService::RpcAcceptResponse;
    using RpcCommitRequest = CopilotService::RpcCommitRequest;
    using RpcCommitResponse = CopilotService::RpcCommitResponse;
    class PrepareTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit PrepareTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcPrepareResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcPrepareResponse, srpc::i32>::Err(__ret__);
            }
            RpcPrepareResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ret_cmd, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.dep, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.status, __reply_ar__);
            return rusty::Result<RpcPrepareResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<PrepareTypedFuture, srpc::i32> async_Prepare(const RpcPrepareRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::PREPARE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.is_pilot, __m__);
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<PrepareTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<PrepareTypedFuture, srpc::i32>::Ok(PrepareTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcPrepareResponse, srpc::i32> Prepare(const RpcPrepareRequest& req) {
        auto __typed_fu_result__ = this->async_Prepare(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcPrepareResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class FastAcceptTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit FastAcceptTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcFastAcceptResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastAcceptResponse, srpc::i32>::Err(__ret__);
            }
            RpcFastAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.ret_dep, __reply_ar__);
            return rusty::Result<RpcFastAcceptResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<FastAcceptTypedFuture, srpc::i32> async_FastAccept(const RpcFastAcceptRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::FASTACCEPT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.is_pilot, __m__);
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.dep, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
            srpc::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<FastAcceptTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<FastAcceptTypedFuture, srpc::i32>::Ok(FastAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcFastAcceptResponse, srpc::i32> FastAccept(const RpcFastAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_FastAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastAcceptResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AcceptTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit AcceptTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAcceptResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAcceptResponse, srpc::i32>::Err(__ret__);
            }
            RpcAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            return rusty::Result<RpcAcceptResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AcceptTypedFuture, srpc::i32> async_Accept(const RpcAcceptRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::ACCEPT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.is_pilot, __m__);
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.dep, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
            srpc::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AcceptTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AcceptTypedFuture, srpc::i32>::Ok(AcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAcceptResponse, srpc::i32> Accept(const RpcAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_Accept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAcceptResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class CommitTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit CommitTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcCommitResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcCommitResponse, srpc::i32>::Err(__ret__);
            }
            RpcCommitResponse __typed_resp__;
            return rusty::Result<RpcCommitResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<CommitTypedFuture, srpc::i32> async_Commit(const RpcCommitRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::COMMIT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.is_pilot, __m__);
            srpc::Serialize_::serialize(req.slot, __m__);
            srpc::Serialize_::serialize(req.dep, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<CommitTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<CommitTypedFuture, srpc::i32>::Ok(CommitTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcCommitResponse, srpc::i32> Commit(const RpcCommitRequest& req) {
        auto __typed_fu_result__ = this->async_Commit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcCommitResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class ClassicService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcMsgStringRequest {
        std::string arg;
    };
    friend inline void serialize(const RpcMsgStringRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.arg, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcMsgStringRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcMsgStringRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.arg, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcMsgStringRequest& o) { deserialize(o, ar); return ar; }

    struct RpcMsgStringResponse {
        std::string ret;
    };
    friend inline void serialize(const RpcMsgStringResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ret, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcMsgStringResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcMsgStringResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ret, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcMsgStringResponse& o) { deserialize(o, ar); return ar; }

    struct RpcMsgMarshallRequest {
        Command arg;
    };
    friend inline void serialize(const RpcMsgMarshallRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.arg, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcMsgMarshallRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcMsgMarshallRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.arg, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcMsgMarshallRequest& o) { deserialize(o, ar); return ar; }

    struct RpcMsgMarshallResponse {
        Command ret;
    };
    friend inline void serialize(const RpcMsgMarshallResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ret, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcMsgMarshallResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcMsgMarshallResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ret, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcMsgMarshallResponse& o) { deserialize(o, ar); return ar; }

    struct RpcReElectRequest {
    };
    friend inline void serialize(const RpcReElectRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcReElectRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReElectRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcReElectRequest& o) { deserialize(o, ar); return ar; }

    struct RpcReElectResponse {
        bool_t success;
    };
    friend inline void serialize(const RpcReElectResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.success, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcReElectResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReElectResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.success, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcReElectResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRuleSpeculativeExecuteRequest {
        Command md;
    };
    friend inline void serialize(const RpcRuleSpeculativeExecuteRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.md, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRuleSpeculativeExecuteRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRuleSpeculativeExecuteRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.md, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRuleSpeculativeExecuteRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRuleSpeculativeExecuteResponse {
        bool_t accepted;
        int32_t result;
        bool_t is_leader;
    };
    friend inline void serialize(const RpcRuleSpeculativeExecuteResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.accepted, ar);
        srpc::Serialize_::serialize(o.result, ar);
        srpc::Serialize_::serialize(o.is_leader, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRuleSpeculativeExecuteResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRuleSpeculativeExecuteResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.accepted, ar);
        srpc::Deserialize_::deserialize(o.result, ar);
        srpc::Deserialize_::deserialize(o.is_leader, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRuleSpeculativeExecuteResponse& o) { deserialize(o, ar); return ar; }

    struct RpcDispatchRequest {
        srpc::i64 tid;
        DepId dep_id;
        Command cmd;
    };
    friend inline void serialize(const RpcDispatchRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.tid, ar);
        srpc::Serialize_::serialize(o.dep_id, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDispatchRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDispatchRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.tid, ar);
        srpc::Deserialize_::deserialize(o.dep_id, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDispatchRequest& o) { deserialize(o, ar); return ar; }

    struct RpcDispatchResponse {
        srpc::i32 res;
        TxnOutput output;
        uint64_t coro_id;
        Command view_data;
    };
    friend inline void serialize(const RpcDispatchResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
        srpc::Serialize_::serialize(o.output, ar);
        srpc::Serialize_::serialize(o.coro_id, ar);
        srpc::Serialize_::serialize(o.view_data, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDispatchResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDispatchResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
        srpc::Deserialize_::deserialize(o.output, ar);
        srpc::Deserialize_::deserialize(o.coro_id, ar);
        srpc::Deserialize_::deserialize(o.view_data, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDispatchResponse& o) { deserialize(o, ar); return ar; }

    struct RpcPrepareRequest {
        srpc::i64 tid;
        std::vector<srpc::i32> sids;
        DepId dep_id;
    };
    friend inline void serialize(const RpcPrepareRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.tid, ar);
        srpc::Serialize_::serialize(o.sids, ar);
        srpc::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcPrepareRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.tid, ar);
        srpc::Deserialize_::deserialize(o.sids, ar);
        srpc::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcPrepareRequest& o) { deserialize(o, ar); return ar; }

    struct RpcPrepareResponse {
        srpc::i32 res;
        bool_t slow;
        uint64_t coro_id;
    };
    friend inline void serialize(const RpcPrepareResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
        srpc::Serialize_::serialize(o.slow, ar);
        srpc::Serialize_::serialize(o.coro_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcPrepareResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
        srpc::Deserialize_::deserialize(o.slow, ar);
        srpc::Deserialize_::deserialize(o.coro_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcPrepareResponse& o) { deserialize(o, ar); return ar; }

    struct RpcCommitRequest {
        srpc::i64 tid;
        DepId dep_id;
    };
    friend inline void serialize(const RpcCommitRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.tid, ar);
        srpc::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcCommitRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcCommitRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.tid, ar);
        srpc::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcCommitRequest& o) { deserialize(o, ar); return ar; }

    struct RpcCommitResponse {
        srpc::i32 res;
        bool_t slow;
        uint64_t coro_id;
        Command view_data;
    };
    friend inline void serialize(const RpcCommitResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
        srpc::Serialize_::serialize(o.slow, ar);
        srpc::Serialize_::serialize(o.coro_id, ar);
        srpc::Serialize_::serialize(o.view_data, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcCommitResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
        srpc::Deserialize_::deserialize(o.slow, ar);
        srpc::Deserialize_::deserialize(o.coro_id, ar);
        srpc::Deserialize_::deserialize(o.view_data, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcCommitResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAbortRequest {
        srpc::i64 tid;
        DepId dep_id;
    };
    friend inline void serialize(const RpcAbortRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.tid, ar);
        srpc::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAbortRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAbortRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.tid, ar);
        srpc::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAbortRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAbortResponse {
        srpc::i32 res;
        bool_t slow;
        uint64_t coro_id;
        Command view_data;
    };
    friend inline void serialize(const RpcAbortResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
        srpc::Serialize_::serialize(o.slow, ar);
        srpc::Serialize_::serialize(o.coro_id, ar);
        srpc::Serialize_::serialize(o.view_data, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcAbortResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAbortResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
        srpc::Deserialize_::deserialize(o.slow, ar);
        srpc::Deserialize_::deserialize(o.coro_id, ar);
        srpc::Deserialize_::deserialize(o.view_data, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcAbortResponse& o) { deserialize(o, ar); return ar; }

    struct RpcEarlyAbortRequest {
        srpc::i64 tid;
    };
    friend inline void serialize(const RpcEarlyAbortRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.tid, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcEarlyAbortRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEarlyAbortRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.tid, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcEarlyAbortRequest& o) { deserialize(o, ar); return ar; }

    struct RpcEarlyAbortResponse {
        srpc::i32 res;
    };
    friend inline void serialize(const RpcEarlyAbortResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcEarlyAbortResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEarlyAbortResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcEarlyAbortResponse& o) { deserialize(o, ar); return ar; }

    struct RpcUpgradeEpochRequest {
        uint32_t curr_epoch;
    };
    friend inline void serialize(const RpcUpgradeEpochRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.curr_epoch, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcUpgradeEpochRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcUpgradeEpochRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.curr_epoch, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcUpgradeEpochRequest& o) { deserialize(o, ar); return ar; }

    struct RpcUpgradeEpochResponse {
        int32_t res;
    };
    friend inline void serialize(const RpcUpgradeEpochResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcUpgradeEpochResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcUpgradeEpochResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcUpgradeEpochResponse& o) { deserialize(o, ar); return ar; }

    struct RpcTruncateEpochRequest {
        uint32_t old_epoch;
    };
    friend inline void serialize(const RpcTruncateEpochRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.old_epoch, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTruncateEpochRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTruncateEpochRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.old_epoch, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTruncateEpochRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTruncateEpochResponse {
    };
    friend inline void serialize(const RpcTruncateEpochResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTruncateEpochResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTruncateEpochResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTruncateEpochResponse& o) { deserialize(o, ar); return ar; }

    struct RpcIsLeaderRequest {
        locid_t cur_pause;
    };
    friend inline void serialize(const RpcIsLeaderRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cur_pause, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcIsLeaderRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcIsLeaderRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cur_pause, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcIsLeaderRequest& o) { deserialize(o, ar); return ar; }

    struct RpcIsLeaderResponse {
        bool_t is_leader;
    };
    friend inline void serialize(const RpcIsLeaderResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.is_leader, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcIsLeaderResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcIsLeaderResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.is_leader, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcIsLeaderResponse& o) { deserialize(o, ar); return ar; }

    struct RpcIsFPGALeaderRequest {
        locid_t cur_pause;
    };
    friend inline void serialize(const RpcIsFPGALeaderRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cur_pause, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcIsFPGALeaderRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcIsFPGALeaderRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cur_pause, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcIsFPGALeaderRequest& o) { deserialize(o, ar); return ar; }

    struct RpcIsFPGALeaderResponse {
        bool_t is_leader;
    };
    friend inline void serialize(const RpcIsFPGALeaderResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.is_leader, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcIsFPGALeaderResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcIsFPGALeaderResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.is_leader, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcIsFPGALeaderResponse& o) { deserialize(o, ar); return ar; }

    struct RpcSimpleCmdRequest {
        SimpleCommand cmd;
    };
    friend inline void serialize(const RpcSimpleCmdRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcSimpleCmdRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSimpleCmdRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcSimpleCmdRequest& o) { deserialize(o, ar); return ar; }

    struct RpcSimpleCmdResponse {
        srpc::i32 res;
    };
    friend inline void serialize(const RpcSimpleCmdResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcSimpleCmdResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSimpleCmdResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcSimpleCmdResponse& o) { deserialize(o, ar); return ar; }

    struct RpcFailoverPauseSocketOutRequest {
    };
    friend inline void serialize(const RpcFailoverPauseSocketOutRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFailoverPauseSocketOutRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFailoverPauseSocketOutRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFailoverPauseSocketOutRequest& o) { deserialize(o, ar); return ar; }

    struct RpcFailoverPauseSocketOutResponse {
        srpc::i32 res;
    };
    friend inline void serialize(const RpcFailoverPauseSocketOutResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFailoverPauseSocketOutResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFailoverPauseSocketOutResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFailoverPauseSocketOutResponse& o) { deserialize(o, ar); return ar; }

    struct RpcFailoverResumeSocketOutRequest {
    };
    friend inline void serialize(const RpcFailoverResumeSocketOutRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFailoverResumeSocketOutRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFailoverResumeSocketOutRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFailoverResumeSocketOutRequest& o) { deserialize(o, ar); return ar; }

    struct RpcFailoverResumeSocketOutResponse {
        srpc::i32 res;
    };
    friend inline void serialize(const RpcFailoverResumeSocketOutResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcFailoverResumeSocketOutResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFailoverResumeSocketOutResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcFailoverResumeSocketOutResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRpcNullRequest {
    };
    friend inline void serialize(const RpcRpcNullRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRpcNullRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRpcNullRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRpcNullRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRpcNullResponse {
    };
    friend inline void serialize(const RpcRpcNullResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRpcNullResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRpcNullResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRpcNullResponse& o) { deserialize(o, ar); return ar; }

    struct RpcTapirAcceptRequest {
        uint64_t cmd_id;
        int64_t ballot;
        int32_t decision;
    };
    friend inline void serialize(const RpcTapirAcceptRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd_id, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.decision, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTapirAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirAcceptRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd_id, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.decision, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTapirAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTapirAcceptResponse {
    };
    friend inline void serialize(const RpcTapirAcceptResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTapirAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirAcceptResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTapirAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcTapirFastAcceptRequest {
        uint64_t cmd_id;
        std::vector<SimpleCommand> txn_cmds;
    };
    friend inline void serialize(const RpcTapirFastAcceptRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd_id, ar);
        srpc::Serialize_::serialize(o.txn_cmds, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTapirFastAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirFastAcceptRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd_id, ar);
        srpc::Deserialize_::deserialize(o.txn_cmds, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTapirFastAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTapirFastAcceptResponse {
        srpc::i32 res;
    };
    friend inline void serialize(const RpcTapirFastAcceptResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTapirFastAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirFastAcceptResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTapirFastAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcTapirDecideRequest {
        uint64_t cmd_id;
        srpc::i32 commit;
    };
    friend inline void serialize(const RpcTapirDecideRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd_id, ar);
        srpc::Serialize_::serialize(o.commit, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTapirDecideRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirDecideRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd_id, ar);
        srpc::Deserialize_::deserialize(o.commit, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTapirDecideRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTapirDecideResponse {
    };
    friend inline void serialize(const RpcTapirDecideResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcTapirDecideResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirDecideResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcTapirDecideResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccDispatchRequest {
        std::vector<SimpleCommand> cmd;
    };
    friend inline void serialize(const RpcRccDispatchRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccDispatchRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccDispatchRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccDispatchRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccDispatchResponse {
        srpc::i32 res;
        TxnOutput output;
        AnyMessage md_graph;
    };
    friend inline void serialize(const RpcRccDispatchResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
        srpc::Serialize_::serialize(o.output, ar);
        srpc::Serialize_::serialize(o.md_graph, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccDispatchResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccDispatchResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
        srpc::Deserialize_::deserialize(o.output, ar);
        srpc::Deserialize_::deserialize(o.md_graph, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccDispatchResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccFinishRequest {
        cmdid_t id;
        AnyMessage md_graph;
    };
    friend inline void serialize(const RpcRccFinishRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.id, ar);
        srpc::Serialize_::serialize(o.md_graph, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccFinishRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccFinishRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.id, ar);
        srpc::Deserialize_::deserialize(o.md_graph, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccFinishRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccFinishResponse {
        std::map<uint32_t, std::map<int32_t, Value>> outputs;
    };
    friend inline void serialize(const RpcRccFinishResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.outputs, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccFinishResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccFinishResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.outputs, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccFinishResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccInquireRequest {
        txnid_t txn_id;
        int32_t rank;
    };
    friend inline void serialize(const RpcRccInquireRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.txn_id, ar);
        srpc::Serialize_::serialize(o.rank, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccInquireRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccInquireRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.txn_id, ar);
        srpc::Deserialize_::deserialize(o.rank, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccInquireRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccInquireResponse {
        std::map<uint64_t, parent_set_t> out_0;
    };
    friend inline void serialize(const RpcRccInquireResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.out_0, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccInquireResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccInquireResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.out_0, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccInquireResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccDispatchRoRequest {
        SimpleCommand cmd;
    };
    friend inline void serialize(const RpcRccDispatchRoRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccDispatchRoRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccDispatchRoRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccDispatchRoRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccDispatchRoResponse {
        std::map<srpc::i32, Value> output;
    };
    friend inline void serialize(const RpcRccDispatchRoResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.output, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccDispatchRoResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccDispatchRoResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.output, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccDispatchRoResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccInquireValidationRequest {
        txid_t tx_id;
        int32_t rank;
    };
    friend inline void serialize(const RpcRccInquireValidationRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.tx_id, ar);
        srpc::Serialize_::serialize(o.rank, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccInquireValidationRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccInquireValidationRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.tx_id, ar);
        srpc::Deserialize_::deserialize(o.rank, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccInquireValidationRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccInquireValidationResponse {
        int32_t res;
    };
    friend inline void serialize(const RpcRccInquireValidationResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccInquireValidationResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccInquireValidationResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccInquireValidationResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccNotifyGlobalValidationRequest {
        txid_t tx_id;
        int32_t rank;
        int32_t res;
    };
    friend inline void serialize(const RpcRccNotifyGlobalValidationRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.tx_id, ar);
        srpc::Serialize_::serialize(o.rank, ar);
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccNotifyGlobalValidationRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccNotifyGlobalValidationRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.tx_id, ar);
        srpc::Deserialize_::deserialize(o.rank, ar);
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccNotifyGlobalValidationRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccNotifyGlobalValidationResponse {
    };
    friend inline void serialize(const RpcRccNotifyGlobalValidationResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccNotifyGlobalValidationResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccNotifyGlobalValidationResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccNotifyGlobalValidationResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccCommitRequest {
        cmdid_t id;
        rank_t rank;
        int32_t need_validation;
        parent_set_t parents;
    };
    friend inline void serialize(const RpcRccCommitRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.id, ar);
        srpc::Serialize_::serialize(o.rank, ar);
        srpc::Serialize_::serialize(o.need_validation, ar);
        srpc::Serialize_::serialize(o.parents, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccCommitRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccCommitRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.id, ar);
        srpc::Deserialize_::deserialize(o.rank, ar);
        srpc::Deserialize_::deserialize(o.need_validation, ar);
        srpc::Deserialize_::deserialize(o.parents, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccCommitRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccCommitResponse {
        int32_t res;
        TxnOutput output;
    };
    friend inline void serialize(const RpcRccCommitResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
        srpc::Serialize_::serialize(o.output, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccCommitResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
        srpc::Deserialize_::deserialize(o.output, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccCommitResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccPreAcceptRequest {
        cmdid_t txn_id;
        rank_t rank;
        std::vector<SimpleCommand> cmd;
    };
    friend inline void serialize(const RpcRccPreAcceptRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.txn_id, ar);
        srpc::Serialize_::serialize(o.rank, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccPreAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccPreAcceptRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.txn_id, ar);
        srpc::Deserialize_::deserialize(o.rank, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccPreAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccPreAcceptResponse {
        srpc::i32 res;
        parent_set_t x;
    };
    friend inline void serialize(const RpcRccPreAcceptResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
        srpc::Serialize_::serialize(o.x, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccPreAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccPreAcceptResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
        srpc::Deserialize_::deserialize(o.x, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccPreAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccAcceptRequest {
        cmdid_t txn_id;
        srpc::i32 rank;
        ballot_t ballot;
        parent_set_t p;
    };
    friend inline void serialize(const RpcRccAcceptRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.txn_id, ar);
        srpc::Serialize_::serialize(o.rank, ar);
        srpc::Serialize_::serialize(o.ballot, ar);
        srpc::Serialize_::serialize(o.p, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccAcceptRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.txn_id, ar);
        srpc::Deserialize_::deserialize(o.rank, ar);
        srpc::Deserialize_::deserialize(o.ballot, ar);
        srpc::Deserialize_::deserialize(o.p, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccAcceptResponse {
        srpc::i32 res;
    };
    friend inline void serialize(const RpcRccAcceptResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcRccAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccAcceptResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcRccAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackBeginRecoveryRequest {
        Command old_view;
        Command new_view;
        epoch_t new_view_id;
    };
    friend inline void serialize(const RpcJetpackBeginRecoveryRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.old_view, ar);
        srpc::Serialize_::serialize(o.new_view, ar);
        srpc::Serialize_::serialize(o.new_view_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackBeginRecoveryRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackBeginRecoveryRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.old_view, ar);
        srpc::Deserialize_::deserialize(o.new_view, ar);
        srpc::Deserialize_::deserialize(o.new_view_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackBeginRecoveryRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackBeginRecoveryResponse {
    };
    friend inline void serialize(const RpcJetpackBeginRecoveryResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackBeginRecoveryResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackBeginRecoveryResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackBeginRecoveryResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullIdSetRequest {
        epoch_t jepoch;
        epoch_t oepoch;
    };
    friend inline void serialize(const RpcJetpackPullIdSetRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.jepoch, ar);
        srpc::Serialize_::serialize(o.oepoch, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackPullIdSetRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullIdSetRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.jepoch, ar);
        srpc::Deserialize_::deserialize(o.oepoch, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackPullIdSetRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullIdSetResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        Command reply_old_view;
        Command reply_new_view;
        Command id_set;
    };
    friend inline void serialize(const RpcJetpackPullIdSetResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ok, ar);
        srpc::Serialize_::serialize(o.reply_jepoch, ar);
        srpc::Serialize_::serialize(o.reply_oepoch, ar);
        srpc::Serialize_::serialize(o.reply_old_view, ar);
        srpc::Serialize_::serialize(o.reply_new_view, ar);
        srpc::Serialize_::serialize(o.id_set, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackPullIdSetResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullIdSetResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ok, ar);
        srpc::Deserialize_::deserialize(o.reply_jepoch, ar);
        srpc::Deserialize_::deserialize(o.reply_oepoch, ar);
        srpc::Deserialize_::deserialize(o.reply_old_view, ar);
        srpc::Deserialize_::deserialize(o.reply_new_view, ar);
        srpc::Deserialize_::deserialize(o.id_set, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackPullIdSetResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullCmdRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        Command key_batch;
    };
    friend inline void serialize(const RpcJetpackPullCmdRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.jepoch, ar);
        srpc::Serialize_::serialize(o.oepoch, ar);
        srpc::Serialize_::serialize(o.key_batch, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackPullCmdRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullCmdRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.jepoch, ar);
        srpc::Deserialize_::deserialize(o.oepoch, ar);
        srpc::Deserialize_::deserialize(o.key_batch, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackPullCmdRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullCmdResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        Command reply_old_view;
        Command reply_new_view;
        Command cmd_batch;
    };
    friend inline void serialize(const RpcJetpackPullCmdResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ok, ar);
        srpc::Serialize_::serialize(o.reply_jepoch, ar);
        srpc::Serialize_::serialize(o.reply_oepoch, ar);
        srpc::Serialize_::serialize(o.reply_old_view, ar);
        srpc::Serialize_::serialize(o.reply_new_view, ar);
        srpc::Serialize_::serialize(o.cmd_batch, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackPullCmdResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullCmdResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ok, ar);
        srpc::Deserialize_::deserialize(o.reply_jepoch, ar);
        srpc::Deserialize_::deserialize(o.reply_oepoch, ar);
        srpc::Deserialize_::deserialize(o.reply_old_view, ar);
        srpc::Deserialize_::deserialize(o.reply_new_view, ar);
        srpc::Deserialize_::deserialize(o.cmd_batch, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackPullCmdResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackRecordCmdRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        int32_t sid;
        int32_t rid;
        Command cmd_batch;
    };
    friend inline void serialize(const RpcJetpackRecordCmdRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.jepoch, ar);
        srpc::Serialize_::serialize(o.oepoch, ar);
        srpc::Serialize_::serialize(o.sid, ar);
        srpc::Serialize_::serialize(o.rid, ar);
        srpc::Serialize_::serialize(o.cmd_batch, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackRecordCmdRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackRecordCmdRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.jepoch, ar);
        srpc::Deserialize_::deserialize(o.oepoch, ar);
        srpc::Deserialize_::deserialize(o.sid, ar);
        srpc::Deserialize_::deserialize(o.rid, ar);
        srpc::Deserialize_::deserialize(o.cmd_batch, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackRecordCmdRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackRecordCmdResponse {
    };
    friend inline void serialize(const RpcJetpackRecordCmdResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackRecordCmdResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackRecordCmdResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackRecordCmdResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPrepareRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        ballot_t max_seen_ballot;
    };
    friend inline void serialize(const RpcJetpackPrepareRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.jepoch, ar);
        srpc::Serialize_::serialize(o.oepoch, ar);
        srpc::Serialize_::serialize(o.max_seen_ballot, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackPrepareRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPrepareRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.jepoch, ar);
        srpc::Deserialize_::deserialize(o.oepoch, ar);
        srpc::Deserialize_::deserialize(o.max_seen_ballot, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackPrepareRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPrepareResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        Command reply_old_view;
        Command reply_new_view;
        ballot_t reply_max_seen_ballot;
        ballot_t accepted_ballot;
        int32_t replied_sid;
        int32_t replied_set_size;
    };
    friend inline void serialize(const RpcJetpackPrepareResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ok, ar);
        srpc::Serialize_::serialize(o.reply_jepoch, ar);
        srpc::Serialize_::serialize(o.reply_oepoch, ar);
        srpc::Serialize_::serialize(o.reply_old_view, ar);
        srpc::Serialize_::serialize(o.reply_new_view, ar);
        srpc::Serialize_::serialize(o.reply_max_seen_ballot, ar);
        srpc::Serialize_::serialize(o.accepted_ballot, ar);
        srpc::Serialize_::serialize(o.replied_sid, ar);
        srpc::Serialize_::serialize(o.replied_set_size, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackPrepareResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPrepareResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ok, ar);
        srpc::Deserialize_::deserialize(o.reply_jepoch, ar);
        srpc::Deserialize_::deserialize(o.reply_oepoch, ar);
        srpc::Deserialize_::deserialize(o.reply_old_view, ar);
        srpc::Deserialize_::deserialize(o.reply_new_view, ar);
        srpc::Deserialize_::deserialize(o.reply_max_seen_ballot, ar);
        srpc::Deserialize_::deserialize(o.accepted_ballot, ar);
        srpc::Deserialize_::deserialize(o.replied_sid, ar);
        srpc::Deserialize_::deserialize(o.replied_set_size, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackPrepareResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackAcceptRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        ballot_t max_seen_ballot;
        int32_t sid;
        int32_t set_size;
    };
    friend inline void serialize(const RpcJetpackAcceptRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.jepoch, ar);
        srpc::Serialize_::serialize(o.oepoch, ar);
        srpc::Serialize_::serialize(o.max_seen_ballot, ar);
        srpc::Serialize_::serialize(o.sid, ar);
        srpc::Serialize_::serialize(o.set_size, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackAcceptRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.jepoch, ar);
        srpc::Deserialize_::deserialize(o.oepoch, ar);
        srpc::Deserialize_::deserialize(o.max_seen_ballot, ar);
        srpc::Deserialize_::deserialize(o.sid, ar);
        srpc::Deserialize_::deserialize(o.set_size, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackAcceptResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        Command reply_old_view;
        Command reply_new_view;
        ballot_t reply_max_seen_ballot;
    };
    friend inline void serialize(const RpcJetpackAcceptResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ok, ar);
        srpc::Serialize_::serialize(o.reply_jepoch, ar);
        srpc::Serialize_::serialize(o.reply_oepoch, ar);
        srpc::Serialize_::serialize(o.reply_old_view, ar);
        srpc::Serialize_::serialize(o.reply_new_view, ar);
        srpc::Serialize_::serialize(o.reply_max_seen_ballot, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackAcceptResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ok, ar);
        srpc::Deserialize_::deserialize(o.reply_jepoch, ar);
        srpc::Deserialize_::deserialize(o.reply_oepoch, ar);
        srpc::Deserialize_::deserialize(o.reply_old_view, ar);
        srpc::Deserialize_::deserialize(o.reply_new_view, ar);
        srpc::Deserialize_::deserialize(o.reply_max_seen_ballot, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackCommitRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        int32_t sid;
        int32_t set_size;
    };
    friend inline void serialize(const RpcJetpackCommitRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.jepoch, ar);
        srpc::Serialize_::serialize(o.oepoch, ar);
        srpc::Serialize_::serialize(o.sid, ar);
        srpc::Serialize_::serialize(o.set_size, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackCommitRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackCommitRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.jepoch, ar);
        srpc::Deserialize_::deserialize(o.oepoch, ar);
        srpc::Deserialize_::deserialize(o.sid, ar);
        srpc::Deserialize_::deserialize(o.set_size, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackCommitRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackCommitResponse {
    };
    friend inline void serialize(const RpcJetpackCommitResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackCommitResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackCommitResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullRecSetInsRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        int32_t sid;
        int32_t rid;
    };
    friend inline void serialize(const RpcJetpackPullRecSetInsRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.jepoch, ar);
        srpc::Serialize_::serialize(o.oepoch, ar);
        srpc::Serialize_::serialize(o.sid, ar);
        srpc::Serialize_::serialize(o.rid, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackPullRecSetInsRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullRecSetInsRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.jepoch, ar);
        srpc::Deserialize_::deserialize(o.oepoch, ar);
        srpc::Deserialize_::deserialize(o.sid, ar);
        srpc::Deserialize_::deserialize(o.rid, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackPullRecSetInsRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullRecSetInsResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        Command reply_old_view;
        Command reply_new_view;
        Command cmd;
    };
    friend inline void serialize(const RpcJetpackPullRecSetInsResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.ok, ar);
        srpc::Serialize_::serialize(o.reply_jepoch, ar);
        srpc::Serialize_::serialize(o.reply_oepoch, ar);
        srpc::Serialize_::serialize(o.reply_old_view, ar);
        srpc::Serialize_::serialize(o.reply_new_view, ar);
        srpc::Serialize_::serialize(o.cmd, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackPullRecSetInsResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullRecSetInsResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.ok, ar);
        srpc::Deserialize_::deserialize(o.reply_jepoch, ar);
        srpc::Deserialize_::deserialize(o.reply_oepoch, ar);
        srpc::Deserialize_::deserialize(o.reply_old_view, ar);
        srpc::Deserialize_::deserialize(o.reply_new_view, ar);
        srpc::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackPullRecSetInsResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackFinishRecoveryRequest {
        epoch_t oepoch;
    };
    friend inline void serialize(const RpcJetpackFinishRecoveryRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.oepoch, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackFinishRecoveryRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackFinishRecoveryRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.oepoch, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackFinishRecoveryRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackFinishRecoveryResponse {
    };
    friend inline void serialize(const RpcJetpackFinishRecoveryResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcJetpackFinishRecoveryResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackFinishRecoveryResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcJetpackFinishRecoveryResponse& o) { deserialize(o, ar); return ar; }

    enum {
        MSGSTRING = 0x4075aa22,
        MSGMARSHALL = 0x6602ec53,
        REELECT = 0x2bcd0e52,
        RULESPECULATIVEEXECUTE = 0x15bd499e,
        DISPATCH = 0x63b62f50,
        PREPARE = 0x5ef5071b,
        COMMIT = 0x1b3bdc7d,
        ABORT = 0x4d934a81,
        EARLYABORT = 0x4a31f986,
        UPGRADEEPOCH = 0x63d5a0e1,
        TRUNCATEEPOCH = 0x2c8a9f38,
        ISLEADER = 0x4b803f58,
        ISFPGALEADER = 0x3a601bea,
        SIMPLECMD = 0x40161224,
        FAILOVERPAUSESOCKETOUT = 0x566789af,
        FAILOVERRESUMESOCKETOUT = 0x61f54de5,
        RPC_NULL = 0x6315d00c,
        TAPIRACCEPT = 0x6afb4b6c,
        TAPIRFASTACCEPT = 0x3942ae5a,
        TAPIRDECIDE = 0x53c67705,
        RCCDISPATCH = 0x55256154,
        RCCFINISH = 0x644485ff,
        RCCINQUIRE = 0x48ef4f11,
        RCCDISPATCHRO = 0x6e560eac,
        RCCINQUIREVALIDATION = 0x1d5efebb,
        RCCNOTIFYGLOBALVALIDATION = 0x53ba2d5e,
        RCCCOMMIT = 0x4e47267b,
        RCCPREACCEPT = 0x21d3c639,
        RCCACCEPT = 0x6fc6c306,
        JETPACKBEGINRECOVERY = 0x50c73c47,
        JETPACKPULLIDSET = 0x6f8bb3e1,
        JETPACKPULLCMD = 0x32301751,
        JETPACKRECORDCMD = 0x467071a3,
        JETPACKPREPARE = 0x6042d72f,
        JETPACKACCEPT = 0x4d33cd93,
        JETPACKCOMMIT = 0x38189bf8,
        JETPACKPULLRECSETINS = 0x184ee0eb,
        JETPACKFINISHRECOVERY = 0x545f28a6,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(MSGSTRING, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(MSGMARSHALL, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(REELECT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RULESPECULATIVEEXECUTE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(DISPATCH, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(PREPARE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(COMMIT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(ABORT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(EARLYABORT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(UPGRADEEPOCH, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TRUNCATEEPOCH, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(ISLEADER, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(ISFPGALEADER, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SIMPLECMD, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FAILOVERPAUSESOCKETOUT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FAILOVERRESUMESOCKETOUT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RPC_NULL, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TAPIRACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TAPIRFASTACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(TAPIRDECIDE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCDISPATCH, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCFINISH, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCINQUIRE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCDISPATCHRO, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCINQUIREVALIDATION, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCNOTIFYGLOBALVALIDATION, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCCOMMIT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCPREACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JETPACKBEGINRECOVERY, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JETPACKPULLIDSET, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JETPACKPULLCMD, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JETPACKRECORDCMD, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JETPACKPREPARE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JETPACKACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JETPACKCOMMIT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JETPACKPULLRECSETINS, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JETPACKFINISHRECOVERY, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(MSGSTRING);
        svr.unreg(MSGMARSHALL);
        svr.unreg(REELECT);
        svr.unreg(RULESPECULATIVEEXECUTE);
        svr.unreg(DISPATCH);
        svr.unreg(PREPARE);
        svr.unreg(COMMIT);
        svr.unreg(ABORT);
        svr.unreg(EARLYABORT);
        svr.unreg(UPGRADEEPOCH);
        svr.unreg(TRUNCATEEPOCH);
        svr.unreg(ISLEADER);
        svr.unreg(ISFPGALEADER);
        svr.unreg(SIMPLECMD);
        svr.unreg(FAILOVERPAUSESOCKETOUT);
        svr.unreg(FAILOVERRESUMESOCKETOUT);
        svr.unreg(RPC_NULL);
        svr.unreg(TAPIRACCEPT);
        svr.unreg(TAPIRFASTACCEPT);
        svr.unreg(TAPIRDECIDE);
        svr.unreg(RCCDISPATCH);
        svr.unreg(RCCFINISH);
        svr.unreg(RCCINQUIRE);
        svr.unreg(RCCDISPATCHRO);
        svr.unreg(RCCINQUIREVALIDATION);
        svr.unreg(RCCNOTIFYGLOBALVALIDATION);
        svr.unreg(RCCCOMMIT);
        svr.unreg(RCCPREACCEPT);
        svr.unreg(RCCACCEPT);
        svr.unreg(JETPACKBEGINRECOVERY);
        svr.unreg(JETPACKPULLIDSET);
        svr.unreg(JETPACKPULLCMD);
        svr.unreg(JETPACKRECORDCMD);
        svr.unreg(JETPACKPREPARE);
        svr.unreg(JETPACKACCEPT);
        svr.unreg(JETPACKCOMMIT);
        svr.unreg(JETPACKPULLRECSETINS);
        svr.unreg(JETPACKFINISHRECOVERY);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case MSGSTRING: __MsgString__wrapper__(std::move(req), weak_sconn); break;
        case MSGMARSHALL: __MsgMarshall__wrapper__(std::move(req), weak_sconn); break;
        case REELECT: __ReElect__wrapper__(std::move(req), weak_sconn); break;
        case RULESPECULATIVEEXECUTE: __RuleSpeculativeExecute__wrapper__(std::move(req), weak_sconn); break;
        case DISPATCH: __Dispatch__wrapper__(std::move(req), weak_sconn); break;
        case PREPARE: __Prepare__wrapper__(std::move(req), weak_sconn); break;
        case COMMIT: __Commit__wrapper__(std::move(req), weak_sconn); break;
        case ABORT: __Abort__wrapper__(std::move(req), weak_sconn); break;
        case EARLYABORT: __EarlyAbort__wrapper__(std::move(req), weak_sconn); break;
        case UPGRADEEPOCH: __UpgradeEpoch__wrapper__(std::move(req), weak_sconn); break;
        case TRUNCATEEPOCH: __TruncateEpoch__wrapper__(std::move(req), weak_sconn); break;
        case ISLEADER: __IsLeader__wrapper__(std::move(req), weak_sconn); break;
        case ISFPGALEADER: __IsFPGALeader__wrapper__(std::move(req), weak_sconn); break;
        case SIMPLECMD: __SimpleCmd__wrapper__(std::move(req), weak_sconn); break;
        case FAILOVERPAUSESOCKETOUT: __FailoverPauseSocketOut__wrapper__(std::move(req), weak_sconn); break;
        case FAILOVERRESUMESOCKETOUT: __FailoverResumeSocketOut__wrapper__(std::move(req), weak_sconn); break;
        case RPC_NULL: __rpc_null__wrapper__(std::move(req), weak_sconn); break;
        case TAPIRACCEPT: __TapirAccept__wrapper__(std::move(req), weak_sconn); break;
        case TAPIRFASTACCEPT: __TapirFastAccept__wrapper__(std::move(req), weak_sconn); break;
        case TAPIRDECIDE: __TapirDecide__wrapper__(std::move(req), weak_sconn); break;
        case RCCDISPATCH: __RccDispatch__wrapper__(std::move(req), weak_sconn); break;
        case RCCFINISH: __RccFinish__wrapper__(std::move(req), weak_sconn); break;
        case RCCINQUIRE: __RccInquire__wrapper__(std::move(req), weak_sconn); break;
        case RCCDISPATCHRO: __RccDispatchRo__wrapper__(std::move(req), weak_sconn); break;
        case RCCINQUIREVALIDATION: __RccInquireValidation__wrapper__(std::move(req), weak_sconn); break;
        case RCCNOTIFYGLOBALVALIDATION: __RccNotifyGlobalValidation__wrapper__(std::move(req), weak_sconn); break;
        case RCCCOMMIT: __RccCommit__wrapper__(std::move(req), weak_sconn); break;
        case RCCPREACCEPT: __RccPreAccept__wrapper__(std::move(req), weak_sconn); break;
        case RCCACCEPT: __RccAccept__wrapper__(std::move(req), weak_sconn); break;
        case JETPACKBEGINRECOVERY: __JetpackBeginRecovery__wrapper__(std::move(req), weak_sconn); break;
        case JETPACKPULLIDSET: __JetpackPullIdSet__wrapper__(std::move(req), weak_sconn); break;
        case JETPACKPULLCMD: __JetpackPullCmd__wrapper__(std::move(req), weak_sconn); break;
        case JETPACKRECORDCMD: __JetpackRecordCmd__wrapper__(std::move(req), weak_sconn); break;
        case JETPACKPREPARE: __JetpackPrepare__wrapper__(std::move(req), weak_sconn); break;
        case JETPACKACCEPT: __JetpackAccept__wrapper__(std::move(req), weak_sconn); break;
        case JETPACKCOMMIT: __JetpackCommit__wrapper__(std::move(req), weak_sconn); break;
        case JETPACKPULLRECSETINS: __JetpackPullRecSetIns__wrapper__(std::move(req), weak_sconn); break;
        case JETPACKFINISHRECOVERY: __JetpackFinishRecovery__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void MsgString(const RpcMsgStringRequest& req, RpcMsgStringResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void MsgMarshall(const RpcMsgMarshallRequest& req, RpcMsgMarshallResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void ReElect(const RpcReElectRequest& req, RpcReElectResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void RuleSpeculativeExecute(const RpcRuleSpeculativeExecuteRequest& req, RpcRuleSpeculativeExecuteResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void Dispatch(const RpcDispatchRequest& req, RpcDispatchResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void Prepare(const RpcPrepareRequest& req, RpcPrepareResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void Commit(const RpcCommitRequest& req, RpcCommitResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void Abort(const RpcAbortRequest& req, RpcAbortResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void EarlyAbort(const RpcEarlyAbortRequest& req, RpcEarlyAbortResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void UpgradeEpoch(const RpcUpgradeEpochRequest& req, RpcUpgradeEpochResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void TruncateEpoch(const RpcTruncateEpochRequest& req, RpcTruncateEpochResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void IsLeader(const RpcIsLeaderRequest& req, RpcIsLeaderResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void IsFPGALeader(const RpcIsFPGALeaderRequest& req, RpcIsFPGALeaderResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void SimpleCmd(const RpcSimpleCmdRequest& req, RpcSimpleCmdResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void FailoverPauseSocketOut(const RpcFailoverPauseSocketOutRequest& req, RpcFailoverPauseSocketOutResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest& req, RpcFailoverResumeSocketOutResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void rpc_null(const RpcRpcNullRequest& req, RpcRpcNullResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void TapirAccept(const RpcTapirAcceptRequest& req, RpcTapirAcceptResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void TapirFastAccept(const RpcTapirFastAcceptRequest& req, RpcTapirFastAcceptResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void TapirDecide(const RpcTapirDecideRequest& req, RpcTapirDecideResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void RccDispatch(const RpcRccDispatchRequest& req, RpcRccDispatchResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void RccFinish(const RpcRccFinishRequest& req, RpcRccFinishResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void RccInquire(const RpcRccInquireRequest& req, RpcRccInquireResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void RccDispatchRo(const RpcRccDispatchRoRequest& req, RpcRccDispatchRoResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void RccInquireValidation(const RpcRccInquireValidationRequest& req, RpcRccInquireValidationResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void RccNotifyGlobalValidation(const RpcRccNotifyGlobalValidationRequest& req, RpcRccNotifyGlobalValidationResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void RccCommit(const RpcRccCommitRequest& req, RpcRccCommitResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void RccPreAccept(const RpcRccPreAcceptRequest& req, RpcRccPreAcceptResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void RccAccept(const RpcRccAcceptRequest& req, RpcRccAcceptResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackBeginRecovery(const RpcJetpackBeginRecoveryRequest& req, RpcJetpackBeginRecoveryResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackPullIdSet(const RpcJetpackPullIdSetRequest& req, RpcJetpackPullIdSetResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackPullCmd(const RpcJetpackPullCmdRequest& req, RpcJetpackPullCmdResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackRecordCmd(const RpcJetpackRecordCmdRequest& req, RpcJetpackRecordCmdResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackPrepare(const RpcJetpackPrepareRequest& req, RpcJetpackPrepareResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackAccept(const RpcJetpackAcceptRequest& req, RpcJetpackAcceptResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackCommit(const RpcJetpackCommitRequest& req, RpcJetpackCommitResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackPullRecSetIns(const RpcJetpackPullRecSetInsRequest& req, RpcJetpackPullRecSetInsResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackFinishRecovery(const RpcJetpackFinishRecoveryRequest& req, RpcJetpackFinishRecoveryResponse& resp, srpc::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __MsgString__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcMsgStringRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.arg, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcMsgStringResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ret, m);
                },
                []() {});
            this->MsgString(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __MsgMarshall__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcMsgMarshallRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.arg, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcMsgMarshallResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ret, m);
                },
                []() {});
            this->MsgMarshall(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __ReElect__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcReElectRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcReElectResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->success, m);
                },
                []() {});
            this->ReElect(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RuleSpeculativeExecute__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRuleSpeculativeExecuteRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.md, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRuleSpeculativeExecuteResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->accepted, m);
                    srpc::Serialize_::serialize(__typed_resp__->result, m);
                    srpc::Serialize_::serialize(__typed_resp__->is_leader, m);
                },
                []() {});
            this->RuleSpeculativeExecute(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Dispatch__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDispatchRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.tid, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcDispatchResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                    srpc::Serialize_::serialize(__typed_resp__->output, m);
                    srpc::Serialize_::serialize(__typed_resp__->coro_id, m);
                    srpc::Serialize_::serialize(__typed_resp__->view_data, m);
                },
                []() {});
            this->Dispatch(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Prepare__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPrepareRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.tid, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.sids, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcPrepareResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                    srpc::Serialize_::serialize(__typed_resp__->slow, m);
                    srpc::Serialize_::serialize(__typed_resp__->coro_id, m);
                },
                []() {});
            this->Prepare(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Commit__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcCommitRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.tid, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcCommitResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                    srpc::Serialize_::serialize(__typed_resp__->slow, m);
                    srpc::Serialize_::serialize(__typed_resp__->coro_id, m);
                    srpc::Serialize_::serialize(__typed_resp__->view_data, m);
                },
                []() {});
            this->Commit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Abort__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAbortRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.tid, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcAbortResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                    srpc::Serialize_::serialize(__typed_resp__->slow, m);
                    srpc::Serialize_::serialize(__typed_resp__->coro_id, m);
                    srpc::Serialize_::serialize(__typed_resp__->view_data, m);
                },
                []() {});
            this->Abort(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __EarlyAbort__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcEarlyAbortRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.tid, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcEarlyAbortResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->EarlyAbort(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __UpgradeEpoch__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcUpgradeEpochRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.curr_epoch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcUpgradeEpochResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->UpgradeEpoch(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __TruncateEpoch__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTruncateEpochRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.old_epoch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcTruncateEpochResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->TruncateEpoch(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __IsLeader__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcIsLeaderRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cur_pause, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcIsLeaderResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->is_leader, m);
                },
                []() {});
            this->IsLeader(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __IsFPGALeader__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcIsFPGALeaderRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cur_pause, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcIsFPGALeaderResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->is_leader, m);
                },
                []() {});
            this->IsFPGALeader(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SimpleCmd__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSimpleCmdRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcSimpleCmdResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->SimpleCmd(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __FailoverPauseSocketOut__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFailoverPauseSocketOutRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcFailoverPauseSocketOutResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->FailoverPauseSocketOut(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __FailoverResumeSocketOut__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFailoverResumeSocketOutRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcFailoverResumeSocketOutResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->FailoverResumeSocketOut(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __rpc_null__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRpcNullRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcRpcNullResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->rpc_null(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __TapirAccept__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTapirAcceptRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.decision, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcTapirAcceptResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->TapirAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __TapirFastAccept__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTapirFastAcceptRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.txn_cmds, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcTapirFastAcceptResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->TapirFastAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __TapirDecide__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTapirDecideRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.commit, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcTapirDecideResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->TapirDecide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccDispatch__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccDispatchRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccDispatchResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                    srpc::Serialize_::serialize(__typed_resp__->output, m);
                    srpc::Serialize_::serialize(__typed_resp__->md_graph, m);
                },
                []() {});
            this->RccDispatch(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccFinish__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccFinishRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.md_graph, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccFinishResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->outputs, m);
                },
                []() {});
            this->RccFinish(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccInquire__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccInquireRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.txn_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccInquireResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->out_0, m);
                },
                []() {});
            this->RccInquire(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccDispatchRo__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccDispatchRoRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccDispatchRoResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->output, m);
                },
                []() {});
            this->RccDispatchRo(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccInquireValidation__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccInquireValidationRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.tx_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccInquireValidationResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->RccInquireValidation(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccNotifyGlobalValidation__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccNotifyGlobalValidationRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.tx_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.res, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccNotifyGlobalValidationResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->RccNotifyGlobalValidation(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccCommit__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccCommitRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.need_validation, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.parents, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccCommitResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                    srpc::Serialize_::serialize(__typed_resp__->output, m);
                },
                []() {});
            this->RccCommit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccPreAccept__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccPreAcceptRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.txn_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccPreAcceptResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                    srpc::Serialize_::serialize(__typed_resp__->x, m);
                },
                []() {});
            this->RccPreAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccAccept__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccAcceptRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.txn_id, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.p, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccAcceptResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->RccAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackBeginRecovery__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackBeginRecoveryRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.old_view, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.new_view, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.new_view_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackBeginRecoveryResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->JetpackBeginRecovery(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackPullIdSet__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackPullIdSetRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackPullIdSetResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ok, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_jepoch, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_oepoch, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_old_view, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_new_view, m);
                    srpc::Serialize_::serialize(__typed_resp__->id_set, m);
                },
                []() {});
            this->JetpackPullIdSet(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackPullCmd__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackPullCmdRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.key_batch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackPullCmdResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ok, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_jepoch, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_oepoch, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_old_view, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_new_view, m);
                    srpc::Serialize_::serialize(__typed_resp__->cmd_batch, m);
                },
                []() {});
            this->JetpackPullCmd(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackRecordCmd__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackRecordCmdRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.sid, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.rid, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.cmd_batch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackRecordCmdResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->JetpackRecordCmd(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackPrepare__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackPrepareRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.max_seen_ballot, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackPrepareResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ok, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_jepoch, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_oepoch, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_old_view, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_new_view, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_max_seen_ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->accepted_ballot, m);
                    srpc::Serialize_::serialize(__typed_resp__->replied_sid, m);
                    srpc::Serialize_::serialize(__typed_resp__->replied_set_size, m);
                },
                []() {});
            this->JetpackPrepare(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackAccept__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackAcceptRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.max_seen_ballot, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.sid, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.set_size, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackAcceptResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ok, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_jepoch, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_oepoch, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_old_view, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_new_view, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_max_seen_ballot, m);
                },
                []() {});
            this->JetpackAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackCommit__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackCommitRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.sid, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.set_size, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackCommitResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->JetpackCommit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackPullRecSetIns__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackPullRecSetInsRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.sid, __req_ar__);
            srpc::Deserialize_::deserialize(__typed_req__.rid, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackPullRecSetInsResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->ok, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_jepoch, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_oepoch, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_old_view, m);
                    srpc::Serialize_::serialize(__typed_resp__->reply_new_view, m);
                    srpc::Serialize_::serialize(__typed_resp__->cmd, m);
                },
                []() {});
            this->JetpackPullRecSetIns(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackFinishRecovery__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackFinishRecoveryRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackFinishRecoveryResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->JetpackFinishRecovery(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ClassicProxy {
protected:
    srpc::Client* __cl__;
public:
    ClassicProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcMsgStringRequest = ClassicService::RpcMsgStringRequest;
    using RpcMsgStringResponse = ClassicService::RpcMsgStringResponse;
    using RpcMsgMarshallRequest = ClassicService::RpcMsgMarshallRequest;
    using RpcMsgMarshallResponse = ClassicService::RpcMsgMarshallResponse;
    using RpcReElectRequest = ClassicService::RpcReElectRequest;
    using RpcReElectResponse = ClassicService::RpcReElectResponse;
    using RpcRuleSpeculativeExecuteRequest = ClassicService::RpcRuleSpeculativeExecuteRequest;
    using RpcRuleSpeculativeExecuteResponse = ClassicService::RpcRuleSpeculativeExecuteResponse;
    using RpcDispatchRequest = ClassicService::RpcDispatchRequest;
    using RpcDispatchResponse = ClassicService::RpcDispatchResponse;
    using RpcPrepareRequest = ClassicService::RpcPrepareRequest;
    using RpcPrepareResponse = ClassicService::RpcPrepareResponse;
    using RpcCommitRequest = ClassicService::RpcCommitRequest;
    using RpcCommitResponse = ClassicService::RpcCommitResponse;
    using RpcAbortRequest = ClassicService::RpcAbortRequest;
    using RpcAbortResponse = ClassicService::RpcAbortResponse;
    using RpcEarlyAbortRequest = ClassicService::RpcEarlyAbortRequest;
    using RpcEarlyAbortResponse = ClassicService::RpcEarlyAbortResponse;
    using RpcUpgradeEpochRequest = ClassicService::RpcUpgradeEpochRequest;
    using RpcUpgradeEpochResponse = ClassicService::RpcUpgradeEpochResponse;
    using RpcTruncateEpochRequest = ClassicService::RpcTruncateEpochRequest;
    using RpcTruncateEpochResponse = ClassicService::RpcTruncateEpochResponse;
    using RpcIsLeaderRequest = ClassicService::RpcIsLeaderRequest;
    using RpcIsLeaderResponse = ClassicService::RpcIsLeaderResponse;
    using RpcIsFPGALeaderRequest = ClassicService::RpcIsFPGALeaderRequest;
    using RpcIsFPGALeaderResponse = ClassicService::RpcIsFPGALeaderResponse;
    using RpcSimpleCmdRequest = ClassicService::RpcSimpleCmdRequest;
    using RpcSimpleCmdResponse = ClassicService::RpcSimpleCmdResponse;
    using RpcFailoverPauseSocketOutRequest = ClassicService::RpcFailoverPauseSocketOutRequest;
    using RpcFailoverPauseSocketOutResponse = ClassicService::RpcFailoverPauseSocketOutResponse;
    using RpcFailoverResumeSocketOutRequest = ClassicService::RpcFailoverResumeSocketOutRequest;
    using RpcFailoverResumeSocketOutResponse = ClassicService::RpcFailoverResumeSocketOutResponse;
    using RpcRpcNullRequest = ClassicService::RpcRpcNullRequest;
    using RpcRpcNullResponse = ClassicService::RpcRpcNullResponse;
    using RpcTapirAcceptRequest = ClassicService::RpcTapirAcceptRequest;
    using RpcTapirAcceptResponse = ClassicService::RpcTapirAcceptResponse;
    using RpcTapirFastAcceptRequest = ClassicService::RpcTapirFastAcceptRequest;
    using RpcTapirFastAcceptResponse = ClassicService::RpcTapirFastAcceptResponse;
    using RpcTapirDecideRequest = ClassicService::RpcTapirDecideRequest;
    using RpcTapirDecideResponse = ClassicService::RpcTapirDecideResponse;
    using RpcRccDispatchRequest = ClassicService::RpcRccDispatchRequest;
    using RpcRccDispatchResponse = ClassicService::RpcRccDispatchResponse;
    using RpcRccFinishRequest = ClassicService::RpcRccFinishRequest;
    using RpcRccFinishResponse = ClassicService::RpcRccFinishResponse;
    using RpcRccInquireRequest = ClassicService::RpcRccInquireRequest;
    using RpcRccInquireResponse = ClassicService::RpcRccInquireResponse;
    using RpcRccDispatchRoRequest = ClassicService::RpcRccDispatchRoRequest;
    using RpcRccDispatchRoResponse = ClassicService::RpcRccDispatchRoResponse;
    using RpcRccInquireValidationRequest = ClassicService::RpcRccInquireValidationRequest;
    using RpcRccInquireValidationResponse = ClassicService::RpcRccInquireValidationResponse;
    using RpcRccNotifyGlobalValidationRequest = ClassicService::RpcRccNotifyGlobalValidationRequest;
    using RpcRccNotifyGlobalValidationResponse = ClassicService::RpcRccNotifyGlobalValidationResponse;
    using RpcRccCommitRequest = ClassicService::RpcRccCommitRequest;
    using RpcRccCommitResponse = ClassicService::RpcRccCommitResponse;
    using RpcRccPreAcceptRequest = ClassicService::RpcRccPreAcceptRequest;
    using RpcRccPreAcceptResponse = ClassicService::RpcRccPreAcceptResponse;
    using RpcRccAcceptRequest = ClassicService::RpcRccAcceptRequest;
    using RpcRccAcceptResponse = ClassicService::RpcRccAcceptResponse;
    using RpcJetpackBeginRecoveryRequest = ClassicService::RpcJetpackBeginRecoveryRequest;
    using RpcJetpackBeginRecoveryResponse = ClassicService::RpcJetpackBeginRecoveryResponse;
    using RpcJetpackPullIdSetRequest = ClassicService::RpcJetpackPullIdSetRequest;
    using RpcJetpackPullIdSetResponse = ClassicService::RpcJetpackPullIdSetResponse;
    using RpcJetpackPullCmdRequest = ClassicService::RpcJetpackPullCmdRequest;
    using RpcJetpackPullCmdResponse = ClassicService::RpcJetpackPullCmdResponse;
    using RpcJetpackRecordCmdRequest = ClassicService::RpcJetpackRecordCmdRequest;
    using RpcJetpackRecordCmdResponse = ClassicService::RpcJetpackRecordCmdResponse;
    using RpcJetpackPrepareRequest = ClassicService::RpcJetpackPrepareRequest;
    using RpcJetpackPrepareResponse = ClassicService::RpcJetpackPrepareResponse;
    using RpcJetpackAcceptRequest = ClassicService::RpcJetpackAcceptRequest;
    using RpcJetpackAcceptResponse = ClassicService::RpcJetpackAcceptResponse;
    using RpcJetpackCommitRequest = ClassicService::RpcJetpackCommitRequest;
    using RpcJetpackCommitResponse = ClassicService::RpcJetpackCommitResponse;
    using RpcJetpackPullRecSetInsRequest = ClassicService::RpcJetpackPullRecSetInsRequest;
    using RpcJetpackPullRecSetInsResponse = ClassicService::RpcJetpackPullRecSetInsResponse;
    using RpcJetpackFinishRecoveryRequest = ClassicService::RpcJetpackFinishRecoveryRequest;
    using RpcJetpackFinishRecoveryResponse = ClassicService::RpcJetpackFinishRecoveryResponse;
    class MsgStringTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit MsgStringTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcMsgStringResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcMsgStringResponse, srpc::i32>::Err(__ret__);
            }
            RpcMsgStringResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ret, __reply_ar__);
            return rusty::Result<RpcMsgStringResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<MsgStringTypedFuture, srpc::i32> async_MsgString(const RpcMsgStringRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::MSGSTRING, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.arg, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<MsgStringTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<MsgStringTypedFuture, srpc::i32>::Ok(MsgStringTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcMsgStringResponse, srpc::i32> MsgString(const RpcMsgStringRequest& req) {
        auto __typed_fu_result__ = this->async_MsgString(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcMsgStringResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class MsgMarshallTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit MsgMarshallTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcMsgMarshallResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcMsgMarshallResponse, srpc::i32>::Err(__ret__);
            }
            RpcMsgMarshallResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ret, __reply_ar__);
            return rusty::Result<RpcMsgMarshallResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<MsgMarshallTypedFuture, srpc::i32> async_MsgMarshall(const RpcMsgMarshallRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::MSGMARSHALL, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.arg, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<MsgMarshallTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<MsgMarshallTypedFuture, srpc::i32>::Ok(MsgMarshallTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcMsgMarshallResponse, srpc::i32> MsgMarshall(const RpcMsgMarshallRequest& req) {
        auto __typed_fu_result__ = this->async_MsgMarshall(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcMsgMarshallResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class ReElectTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit ReElectTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcReElectResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcReElectResponse, srpc::i32>::Err(__ret__);
            }
            RpcReElectResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            return rusty::Result<RpcReElectResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ReElectTypedFuture, srpc::i32> async_ReElect(const RpcReElectRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::REELECT, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<ReElectTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<ReElectTypedFuture, srpc::i32>::Ok(ReElectTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcReElectResponse, srpc::i32> ReElect(const RpcReElectRequest& req) {
        auto __typed_fu_result__ = this->async_ReElect(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcReElectResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RuleSpeculativeExecuteTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit RuleSpeculativeExecuteTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRuleSpeculativeExecuteResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRuleSpeculativeExecuteResponse, srpc::i32>::Err(__ret__);
            }
            RpcRuleSpeculativeExecuteResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.accepted, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.result, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.is_leader, __reply_ar__);
            return rusty::Result<RpcRuleSpeculativeExecuteResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RuleSpeculativeExecuteTypedFuture, srpc::i32> async_RuleSpeculativeExecute(const RpcRuleSpeculativeExecuteRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RULESPECULATIVEEXECUTE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.md, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RuleSpeculativeExecuteTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RuleSpeculativeExecuteTypedFuture, srpc::i32>::Ok(RuleSpeculativeExecuteTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRuleSpeculativeExecuteResponse, srpc::i32> RuleSpeculativeExecute(const RpcRuleSpeculativeExecuteRequest& req) {
        auto __typed_fu_result__ = this->async_RuleSpeculativeExecute(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRuleSpeculativeExecuteResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class DispatchTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit DispatchTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcDispatchResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcDispatchResponse, srpc::i32>::Err(__ret__);
            }
            RpcDispatchResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.view_data, __reply_ar__);
            return rusty::Result<RpcDispatchResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<DispatchTypedFuture, srpc::i32> async_Dispatch(const RpcDispatchRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::DISPATCH, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.tid, __m__);
            srpc::Serialize_::serialize(req.dep_id, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<DispatchTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<DispatchTypedFuture, srpc::i32>::Ok(DispatchTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcDispatchResponse, srpc::i32> Dispatch(const RpcDispatchRequest& req) {
        auto __typed_fu_result__ = this->async_Dispatch(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDispatchResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class PrepareTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit PrepareTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcPrepareResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcPrepareResponse, srpc::i32>::Err(__ret__);
            }
            RpcPrepareResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.slow, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            return rusty::Result<RpcPrepareResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<PrepareTypedFuture, srpc::i32> async_Prepare(const RpcPrepareRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::PREPARE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.tid, __m__);
            srpc::Serialize_::serialize(req.sids, __m__);
            srpc::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<PrepareTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<PrepareTypedFuture, srpc::i32>::Ok(PrepareTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcPrepareResponse, srpc::i32> Prepare(const RpcPrepareRequest& req) {
        auto __typed_fu_result__ = this->async_Prepare(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcPrepareResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class CommitTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit CommitTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcCommitResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcCommitResponse, srpc::i32>::Err(__ret__);
            }
            RpcCommitResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.slow, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.view_data, __reply_ar__);
            return rusty::Result<RpcCommitResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<CommitTypedFuture, srpc::i32> async_Commit(const RpcCommitRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::COMMIT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.tid, __m__);
            srpc::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<CommitTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<CommitTypedFuture, srpc::i32>::Ok(CommitTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcCommitResponse, srpc::i32> Commit(const RpcCommitRequest& req) {
        auto __typed_fu_result__ = this->async_Commit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcCommitResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AbortTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit AbortTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAbortResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAbortResponse, srpc::i32>::Err(__ret__);
            }
            RpcAbortResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.slow, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.view_data, __reply_ar__);
            return rusty::Result<RpcAbortResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AbortTypedFuture, srpc::i32> async_Abort(const RpcAbortRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::ABORT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.tid, __m__);
            srpc::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AbortTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AbortTypedFuture, srpc::i32>::Ok(AbortTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAbortResponse, srpc::i32> Abort(const RpcAbortRequest& req) {
        auto __typed_fu_result__ = this->async_Abort(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAbortResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class EarlyAbortTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit EarlyAbortTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcEarlyAbortResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcEarlyAbortResponse, srpc::i32>::Err(__ret__);
            }
            RpcEarlyAbortResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcEarlyAbortResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<EarlyAbortTypedFuture, srpc::i32> async_EarlyAbort(const RpcEarlyAbortRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::EARLYABORT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.tid, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<EarlyAbortTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<EarlyAbortTypedFuture, srpc::i32>::Ok(EarlyAbortTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcEarlyAbortResponse, srpc::i32> EarlyAbort(const RpcEarlyAbortRequest& req) {
        auto __typed_fu_result__ = this->async_EarlyAbort(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcEarlyAbortResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class UpgradeEpochTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit UpgradeEpochTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcUpgradeEpochResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcUpgradeEpochResponse, srpc::i32>::Err(__ret__);
            }
            RpcUpgradeEpochResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcUpgradeEpochResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<UpgradeEpochTypedFuture, srpc::i32> async_UpgradeEpoch(const RpcUpgradeEpochRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::UPGRADEEPOCH, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.curr_epoch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<UpgradeEpochTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<UpgradeEpochTypedFuture, srpc::i32>::Ok(UpgradeEpochTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcUpgradeEpochResponse, srpc::i32> UpgradeEpoch(const RpcUpgradeEpochRequest& req) {
        auto __typed_fu_result__ = this->async_UpgradeEpoch(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcUpgradeEpochResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class TruncateEpochTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit TruncateEpochTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTruncateEpochResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTruncateEpochResponse, srpc::i32>::Err(__ret__);
            }
            RpcTruncateEpochResponse __typed_resp__;
            return rusty::Result<RpcTruncateEpochResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TruncateEpochTypedFuture, srpc::i32> async_TruncateEpoch(const RpcTruncateEpochRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::TRUNCATEEPOCH, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.old_epoch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TruncateEpochTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TruncateEpochTypedFuture, srpc::i32>::Ok(TruncateEpochTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTruncateEpochResponse, srpc::i32> TruncateEpoch(const RpcTruncateEpochRequest& req) {
        auto __typed_fu_result__ = this->async_TruncateEpoch(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTruncateEpochResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class IsLeaderTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit IsLeaderTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcIsLeaderResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcIsLeaderResponse, srpc::i32>::Err(__ret__);
            }
            RpcIsLeaderResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.is_leader, __reply_ar__);
            return rusty::Result<RpcIsLeaderResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<IsLeaderTypedFuture, srpc::i32> async_IsLeader(const RpcIsLeaderRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::ISLEADER, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cur_pause, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<IsLeaderTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<IsLeaderTypedFuture, srpc::i32>::Ok(IsLeaderTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcIsLeaderResponse, srpc::i32> IsLeader(const RpcIsLeaderRequest& req) {
        auto __typed_fu_result__ = this->async_IsLeader(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcIsLeaderResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class IsFPGALeaderTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit IsFPGALeaderTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcIsFPGALeaderResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcIsFPGALeaderResponse, srpc::i32>::Err(__ret__);
            }
            RpcIsFPGALeaderResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.is_leader, __reply_ar__);
            return rusty::Result<RpcIsFPGALeaderResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<IsFPGALeaderTypedFuture, srpc::i32> async_IsFPGALeader(const RpcIsFPGALeaderRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::ISFPGALEADER, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cur_pause, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<IsFPGALeaderTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<IsFPGALeaderTypedFuture, srpc::i32>::Ok(IsFPGALeaderTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcIsFPGALeaderResponse, srpc::i32> IsFPGALeader(const RpcIsFPGALeaderRequest& req) {
        auto __typed_fu_result__ = this->async_IsFPGALeader(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcIsFPGALeaderResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class SimpleCmdTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit SimpleCmdTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcSimpleCmdResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSimpleCmdResponse, srpc::i32>::Err(__ret__);
            }
            RpcSimpleCmdResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcSimpleCmdResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SimpleCmdTypedFuture, srpc::i32> async_SimpleCmd(const RpcSimpleCmdRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::SIMPLECMD, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SimpleCmdTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SimpleCmdTypedFuture, srpc::i32>::Ok(SimpleCmdTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSimpleCmdResponse, srpc::i32> SimpleCmd(const RpcSimpleCmdRequest& req) {
        auto __typed_fu_result__ = this->async_SimpleCmd(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSimpleCmdResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class FailoverPauseSocketOutTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit FailoverPauseSocketOutTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcFailoverPauseSocketOutResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFailoverPauseSocketOutResponse, srpc::i32>::Err(__ret__);
            }
            RpcFailoverPauseSocketOutResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcFailoverPauseSocketOutResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<FailoverPauseSocketOutTypedFuture, srpc::i32> async_FailoverPauseSocketOut(const RpcFailoverPauseSocketOutRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::FAILOVERPAUSESOCKETOUT, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<FailoverPauseSocketOutTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<FailoverPauseSocketOutTypedFuture, srpc::i32>::Ok(FailoverPauseSocketOutTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcFailoverPauseSocketOutResponse, srpc::i32> FailoverPauseSocketOut(const RpcFailoverPauseSocketOutRequest& req) {
        auto __typed_fu_result__ = this->async_FailoverPauseSocketOut(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFailoverPauseSocketOutResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class FailoverResumeSocketOutTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit FailoverResumeSocketOutTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcFailoverResumeSocketOutResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFailoverResumeSocketOutResponse, srpc::i32>::Err(__ret__);
            }
            RpcFailoverResumeSocketOutResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcFailoverResumeSocketOutResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<FailoverResumeSocketOutTypedFuture, srpc::i32> async_FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::FAILOVERRESUMESOCKETOUT, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<FailoverResumeSocketOutTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<FailoverResumeSocketOutTypedFuture, srpc::i32>::Ok(FailoverResumeSocketOutTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcFailoverResumeSocketOutResponse, srpc::i32> FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest& req) {
        auto __typed_fu_result__ = this->async_FailoverResumeSocketOut(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFailoverResumeSocketOutResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class rpc_nullTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit rpc_nullTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRpcNullResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRpcNullResponse, srpc::i32>::Err(__ret__);
            }
            RpcRpcNullResponse __typed_resp__;
            return rusty::Result<RpcRpcNullResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<rpc_nullTypedFuture, srpc::i32> async_rpc_null(const RpcRpcNullRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RPC_NULL, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<rpc_nullTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<rpc_nullTypedFuture, srpc::i32>::Ok(rpc_nullTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRpcNullResponse, srpc::i32> rpc_null(const RpcRpcNullRequest& req) {
        auto __typed_fu_result__ = this->async_rpc_null(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRpcNullResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class TapirAcceptTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit TapirAcceptTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTapirAcceptResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTapirAcceptResponse, srpc::i32>::Err(__ret__);
            }
            RpcTapirAcceptResponse __typed_resp__;
            return rusty::Result<RpcTapirAcceptResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TapirAcceptTypedFuture, srpc::i32> async_TapirAccept(const RpcTapirAcceptRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::TAPIRACCEPT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd_id, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.decision, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TapirAcceptTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TapirAcceptTypedFuture, srpc::i32>::Ok(TapirAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTapirAcceptResponse, srpc::i32> TapirAccept(const RpcTapirAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_TapirAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTapirAcceptResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class TapirFastAcceptTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit TapirFastAcceptTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTapirFastAcceptResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTapirFastAcceptResponse, srpc::i32>::Err(__ret__);
            }
            RpcTapirFastAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcTapirFastAcceptResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TapirFastAcceptTypedFuture, srpc::i32> async_TapirFastAccept(const RpcTapirFastAcceptRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::TAPIRFASTACCEPT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd_id, __m__);
            srpc::Serialize_::serialize(req.txn_cmds, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TapirFastAcceptTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TapirFastAcceptTypedFuture, srpc::i32>::Ok(TapirFastAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTapirFastAcceptResponse, srpc::i32> TapirFastAccept(const RpcTapirFastAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_TapirFastAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTapirFastAcceptResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class TapirDecideTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit TapirDecideTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTapirDecideResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTapirDecideResponse, srpc::i32>::Err(__ret__);
            }
            RpcTapirDecideResponse __typed_resp__;
            return rusty::Result<RpcTapirDecideResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TapirDecideTypedFuture, srpc::i32> async_TapirDecide(const RpcTapirDecideRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::TAPIRDECIDE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd_id, __m__);
            srpc::Serialize_::serialize(req.commit, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TapirDecideTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TapirDecideTypedFuture, srpc::i32>::Ok(TapirDecideTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTapirDecideResponse, srpc::i32> TapirDecide(const RpcTapirDecideRequest& req) {
        auto __typed_fu_result__ = this->async_TapirDecide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTapirDecideResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RccDispatchTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit RccDispatchTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRccDispatchResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccDispatchResponse, srpc::i32>::Err(__ret__);
            }
            RpcRccDispatchResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.md_graph, __reply_ar__);
            return rusty::Result<RpcRccDispatchResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccDispatchTypedFuture, srpc::i32> async_RccDispatch(const RpcRccDispatchRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCDISPATCH, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccDispatchTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccDispatchTypedFuture, srpc::i32>::Ok(RccDispatchTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccDispatchResponse, srpc::i32> RccDispatch(const RpcRccDispatchRequest& req) {
        auto __typed_fu_result__ = this->async_RccDispatch(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccDispatchResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RccFinishTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit RccFinishTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRccFinishResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccFinishResponse, srpc::i32>::Err(__ret__);
            }
            RpcRccFinishResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.outputs, __reply_ar__);
            return rusty::Result<RpcRccFinishResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccFinishTypedFuture, srpc::i32> async_RccFinish(const RpcRccFinishRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCFINISH, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.id, __m__);
            srpc::Serialize_::serialize(req.md_graph, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccFinishTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccFinishTypedFuture, srpc::i32>::Ok(RccFinishTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccFinishResponse, srpc::i32> RccFinish(const RpcRccFinishRequest& req) {
        auto __typed_fu_result__ = this->async_RccFinish(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccFinishResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RccInquireTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit RccInquireTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRccInquireResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccInquireResponse, srpc::i32>::Err(__ret__);
            }
            RpcRccInquireResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.out_0, __reply_ar__);
            return rusty::Result<RpcRccInquireResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccInquireTypedFuture, srpc::i32> async_RccInquire(const RpcRccInquireRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCINQUIRE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.txn_id, __m__);
            srpc::Serialize_::serialize(req.rank, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccInquireTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccInquireTypedFuture, srpc::i32>::Ok(RccInquireTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccInquireResponse, srpc::i32> RccInquire(const RpcRccInquireRequest& req) {
        auto __typed_fu_result__ = this->async_RccInquire(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccInquireResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RccDispatchRoTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit RccDispatchRoTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRccDispatchRoResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccDispatchRoResponse, srpc::i32>::Err(__ret__);
            }
            RpcRccDispatchRoResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            return rusty::Result<RpcRccDispatchRoResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccDispatchRoTypedFuture, srpc::i32> async_RccDispatchRo(const RpcRccDispatchRoRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCDISPATCHRO, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccDispatchRoTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccDispatchRoTypedFuture, srpc::i32>::Ok(RccDispatchRoTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccDispatchRoResponse, srpc::i32> RccDispatchRo(const RpcRccDispatchRoRequest& req) {
        auto __typed_fu_result__ = this->async_RccDispatchRo(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccDispatchRoResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RccInquireValidationTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit RccInquireValidationTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRccInquireValidationResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccInquireValidationResponse, srpc::i32>::Err(__ret__);
            }
            RpcRccInquireValidationResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcRccInquireValidationResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccInquireValidationTypedFuture, srpc::i32> async_RccInquireValidation(const RpcRccInquireValidationRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCINQUIREVALIDATION, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.tx_id, __m__);
            srpc::Serialize_::serialize(req.rank, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccInquireValidationTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccInquireValidationTypedFuture, srpc::i32>::Ok(RccInquireValidationTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccInquireValidationResponse, srpc::i32> RccInquireValidation(const RpcRccInquireValidationRequest& req) {
        auto __typed_fu_result__ = this->async_RccInquireValidation(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccInquireValidationResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RccNotifyGlobalValidationTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit RccNotifyGlobalValidationTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRccNotifyGlobalValidationResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccNotifyGlobalValidationResponse, srpc::i32>::Err(__ret__);
            }
            RpcRccNotifyGlobalValidationResponse __typed_resp__;
            return rusty::Result<RpcRccNotifyGlobalValidationResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccNotifyGlobalValidationTypedFuture, srpc::i32> async_RccNotifyGlobalValidation(const RpcRccNotifyGlobalValidationRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCNOTIFYGLOBALVALIDATION, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.tx_id, __m__);
            srpc::Serialize_::serialize(req.rank, __m__);
            srpc::Serialize_::serialize(req.res, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccNotifyGlobalValidationTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccNotifyGlobalValidationTypedFuture, srpc::i32>::Ok(RccNotifyGlobalValidationTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccNotifyGlobalValidationResponse, srpc::i32> RccNotifyGlobalValidation(const RpcRccNotifyGlobalValidationRequest& req) {
        auto __typed_fu_result__ = this->async_RccNotifyGlobalValidation(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccNotifyGlobalValidationResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RccCommitTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit RccCommitTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRccCommitResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccCommitResponse, srpc::i32>::Err(__ret__);
            }
            RpcRccCommitResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            return rusty::Result<RpcRccCommitResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccCommitTypedFuture, srpc::i32> async_RccCommit(const RpcRccCommitRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCCOMMIT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.id, __m__);
            srpc::Serialize_::serialize(req.rank, __m__);
            srpc::Serialize_::serialize(req.need_validation, __m__);
            srpc::Serialize_::serialize(req.parents, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccCommitTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccCommitTypedFuture, srpc::i32>::Ok(RccCommitTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccCommitResponse, srpc::i32> RccCommit(const RpcRccCommitRequest& req) {
        auto __typed_fu_result__ = this->async_RccCommit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccCommitResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RccPreAcceptTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit RccPreAcceptTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRccPreAcceptResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccPreAcceptResponse, srpc::i32>::Err(__ret__);
            }
            RpcRccPreAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.x, __reply_ar__);
            return rusty::Result<RpcRccPreAcceptResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccPreAcceptTypedFuture, srpc::i32> async_RccPreAccept(const RpcRccPreAcceptRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCPREACCEPT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.txn_id, __m__);
            srpc::Serialize_::serialize(req.rank, __m__);
            srpc::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccPreAcceptTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccPreAcceptTypedFuture, srpc::i32>::Ok(RccPreAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccPreAcceptResponse, srpc::i32> RccPreAccept(const RpcRccPreAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_RccPreAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccPreAcceptResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RccAcceptTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit RccAcceptTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRccAcceptResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccAcceptResponse, srpc::i32>::Err(__ret__);
            }
            RpcRccAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcRccAcceptResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccAcceptTypedFuture, srpc::i32> async_RccAccept(const RpcRccAcceptRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCACCEPT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.txn_id, __m__);
            srpc::Serialize_::serialize(req.rank, __m__);
            srpc::Serialize_::serialize(req.ballot, __m__);
            srpc::Serialize_::serialize(req.p, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccAcceptTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccAcceptTypedFuture, srpc::i32>::Ok(RccAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccAcceptResponse, srpc::i32> RccAccept(const RpcRccAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_RccAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccAcceptResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackBeginRecoveryTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit JetpackBeginRecoveryTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackBeginRecoveryResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackBeginRecoveryResponse, srpc::i32>::Err(__ret__);
            }
            RpcJetpackBeginRecoveryResponse __typed_resp__;
            return rusty::Result<RpcJetpackBeginRecoveryResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackBeginRecoveryTypedFuture, srpc::i32> async_JetpackBeginRecovery(const RpcJetpackBeginRecoveryRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKBEGINRECOVERY, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.old_view, __m__);
            srpc::Serialize_::serialize(req.new_view, __m__);
            srpc::Serialize_::serialize(req.new_view_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackBeginRecoveryTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackBeginRecoveryTypedFuture, srpc::i32>::Ok(JetpackBeginRecoveryTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackBeginRecoveryResponse, srpc::i32> JetpackBeginRecovery(const RpcJetpackBeginRecoveryRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackBeginRecovery(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackBeginRecoveryResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackPullIdSetTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit JetpackPullIdSetTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackPullIdSetResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackPullIdSetResponse, srpc::i32>::Err(__ret__);
            }
            RpcJetpackPullIdSetResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.id_set, __reply_ar__);
            return rusty::Result<RpcJetpackPullIdSetResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPullIdSetTypedFuture, srpc::i32> async_JetpackPullIdSet(const RpcJetpackPullIdSetRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPULLIDSET, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.jepoch, __m__);
            srpc::Serialize_::serialize(req.oepoch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackPullIdSetTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackPullIdSetTypedFuture, srpc::i32>::Ok(JetpackPullIdSetTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackPullIdSetResponse, srpc::i32> JetpackPullIdSet(const RpcJetpackPullIdSetRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackPullIdSet(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackPullIdSetResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackPullCmdTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit JetpackPullCmdTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackPullCmdResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackPullCmdResponse, srpc::i32>::Err(__ret__);
            }
            RpcJetpackPullCmdResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.cmd_batch, __reply_ar__);
            return rusty::Result<RpcJetpackPullCmdResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPullCmdTypedFuture, srpc::i32> async_JetpackPullCmd(const RpcJetpackPullCmdRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPULLCMD, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.jepoch, __m__);
            srpc::Serialize_::serialize(req.oepoch, __m__);
            srpc::Serialize_::serialize(req.key_batch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackPullCmdTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackPullCmdTypedFuture, srpc::i32>::Ok(JetpackPullCmdTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackPullCmdResponse, srpc::i32> JetpackPullCmd(const RpcJetpackPullCmdRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackPullCmd(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackPullCmdResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackRecordCmdTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit JetpackRecordCmdTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackRecordCmdResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackRecordCmdResponse, srpc::i32>::Err(__ret__);
            }
            RpcJetpackRecordCmdResponse __typed_resp__;
            return rusty::Result<RpcJetpackRecordCmdResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackRecordCmdTypedFuture, srpc::i32> async_JetpackRecordCmd(const RpcJetpackRecordCmdRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKRECORDCMD, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.jepoch, __m__);
            srpc::Serialize_::serialize(req.oepoch, __m__);
            srpc::Serialize_::serialize(req.sid, __m__);
            srpc::Serialize_::serialize(req.rid, __m__);
            srpc::Serialize_::serialize(req.cmd_batch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackRecordCmdTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackRecordCmdTypedFuture, srpc::i32>::Ok(JetpackRecordCmdTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackRecordCmdResponse, srpc::i32> JetpackRecordCmd(const RpcJetpackRecordCmdRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackRecordCmd(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackRecordCmdResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackPrepareTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit JetpackPrepareTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackPrepareResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackPrepareResponse, srpc::i32>::Err(__ret__);
            }
            RpcJetpackPrepareResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_max_seen_ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.accepted_ballot, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.replied_sid, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.replied_set_size, __reply_ar__);
            return rusty::Result<RpcJetpackPrepareResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPrepareTypedFuture, srpc::i32> async_JetpackPrepare(const RpcJetpackPrepareRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPREPARE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.jepoch, __m__);
            srpc::Serialize_::serialize(req.oepoch, __m__);
            srpc::Serialize_::serialize(req.max_seen_ballot, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackPrepareTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackPrepareTypedFuture, srpc::i32>::Ok(JetpackPrepareTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackPrepareResponse, srpc::i32> JetpackPrepare(const RpcJetpackPrepareRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackPrepare(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackPrepareResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackAcceptTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit JetpackAcceptTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackAcceptResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackAcceptResponse, srpc::i32>::Err(__ret__);
            }
            RpcJetpackAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_max_seen_ballot, __reply_ar__);
            return rusty::Result<RpcJetpackAcceptResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackAcceptTypedFuture, srpc::i32> async_JetpackAccept(const RpcJetpackAcceptRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKACCEPT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.jepoch, __m__);
            srpc::Serialize_::serialize(req.oepoch, __m__);
            srpc::Serialize_::serialize(req.max_seen_ballot, __m__);
            srpc::Serialize_::serialize(req.sid, __m__);
            srpc::Serialize_::serialize(req.set_size, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackAcceptTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackAcceptTypedFuture, srpc::i32>::Ok(JetpackAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackAcceptResponse, srpc::i32> JetpackAccept(const RpcJetpackAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackAcceptResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackCommitTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit JetpackCommitTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackCommitResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackCommitResponse, srpc::i32>::Err(__ret__);
            }
            RpcJetpackCommitResponse __typed_resp__;
            return rusty::Result<RpcJetpackCommitResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackCommitTypedFuture, srpc::i32> async_JetpackCommit(const RpcJetpackCommitRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKCOMMIT, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.jepoch, __m__);
            srpc::Serialize_::serialize(req.oepoch, __m__);
            srpc::Serialize_::serialize(req.sid, __m__);
            srpc::Serialize_::serialize(req.set_size, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackCommitTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackCommitTypedFuture, srpc::i32>::Ok(JetpackCommitTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackCommitResponse, srpc::i32> JetpackCommit(const RpcJetpackCommitRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackCommit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackCommitResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackPullRecSetInsTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit JetpackPullRecSetInsTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackPullRecSetInsResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackPullRecSetInsResponse, srpc::i32>::Err(__ret__);
            }
            RpcJetpackPullRecSetInsResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.cmd, __reply_ar__);
            return rusty::Result<RpcJetpackPullRecSetInsResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPullRecSetInsTypedFuture, srpc::i32> async_JetpackPullRecSetIns(const RpcJetpackPullRecSetInsRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPULLRECSETINS, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.jepoch, __m__);
            srpc::Serialize_::serialize(req.oepoch, __m__);
            srpc::Serialize_::serialize(req.sid, __m__);
            srpc::Serialize_::serialize(req.rid, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackPullRecSetInsTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackPullRecSetInsTypedFuture, srpc::i32>::Ok(JetpackPullRecSetInsTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackPullRecSetInsResponse, srpc::i32> JetpackPullRecSetIns(const RpcJetpackPullRecSetInsRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackPullRecSetIns(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackPullRecSetInsResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackFinishRecoveryTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit JetpackFinishRecoveryTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackFinishRecoveryResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackFinishRecoveryResponse, srpc::i32>::Err(__ret__);
            }
            RpcJetpackFinishRecoveryResponse __typed_resp__;
            return rusty::Result<RpcJetpackFinishRecoveryResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackFinishRecoveryTypedFuture, srpc::i32> async_JetpackFinishRecovery(const RpcJetpackFinishRecoveryRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKFINISHRECOVERY, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.oepoch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackFinishRecoveryTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackFinishRecoveryTypedFuture, srpc::i32>::Ok(JetpackFinishRecoveryTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackFinishRecoveryResponse, srpc::i32> JetpackFinishRecovery(const RpcJetpackFinishRecoveryRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackFinishRecovery(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackFinishRecoveryResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class ServerControlService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcServerShutdownRequest {
    };
    friend inline void serialize(const RpcServerShutdownRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerShutdownRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerShutdownRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerShutdownRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerShutdownResponse {
    };
    friend inline void serialize(const RpcServerShutdownResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerShutdownResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerShutdownResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerShutdownResponse& o) { deserialize(o, ar); return ar; }

    struct RpcServerReadyRequest {
    };
    friend inline void serialize(const RpcServerReadyRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerReadyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerReadyRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerReadyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerReadyResponse {
        srpc::i32 res;
    };
    friend inline void serialize(const RpcServerReadyResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerReadyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerReadyResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerReadyResponse& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatWithDataRequest {
    };
    friend inline void serialize(const RpcServerHeartBeatWithDataRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerHeartBeatWithDataRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatWithDataRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerHeartBeatWithDataRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatWithDataResponse {
        ServerResponse res;
    };
    friend inline void serialize(const RpcServerHeartBeatWithDataResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerHeartBeatWithDataResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatWithDataResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerHeartBeatWithDataResponse& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatRequest {
    };
    friend inline void serialize(const RpcServerHeartBeatRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerHeartBeatRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerHeartBeatRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatResponse {
    };
    friend inline void serialize(const RpcServerHeartBeatResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcServerHeartBeatResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcServerHeartBeatResponse& o) { deserialize(o, ar); return ar; }

    enum {
        SERVER_SHUTDOWN = 0x10af16ed,
        SERVER_READY = 0x4780016f,
        SERVER_HEART_BEAT_WITH_DATA = 0x46c9fd6b,
        SERVER_HEART_BEAT = 0x174c78b8,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(SERVER_SHUTDOWN, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SERVER_READY, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SERVER_HEART_BEAT_WITH_DATA, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SERVER_HEART_BEAT, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(SERVER_SHUTDOWN);
        svr.unreg(SERVER_READY);
        svr.unreg(SERVER_HEART_BEAT_WITH_DATA);
        svr.unreg(SERVER_HEART_BEAT);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case SERVER_SHUTDOWN: __server_shutdown__wrapper__(std::move(req), weak_sconn); break;
        case SERVER_READY: __server_ready__wrapper__(std::move(req), weak_sconn); break;
        case SERVER_HEART_BEAT_WITH_DATA: __server_heart_beat_with_data__wrapper__(std::move(req), weak_sconn); break;
        case SERVER_HEART_BEAT: __server_heart_beat__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void server_shutdown(const RpcServerShutdownRequest& req, RpcServerShutdownResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void server_ready(const RpcServerReadyRequest& req, RpcServerReadyResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void server_heart_beat_with_data(const RpcServerHeartBeatWithDataRequest& req, RpcServerHeartBeatWithDataResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void server_heart_beat(const RpcServerHeartBeatRequest& req, RpcServerHeartBeatResponse& resp, srpc::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __server_shutdown__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcServerShutdownRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerShutdownResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->server_shutdown(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __server_ready__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcServerReadyRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerReadyResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->server_ready(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __server_heart_beat_with_data__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcServerHeartBeatWithDataRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerHeartBeatWithDataResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->server_heart_beat_with_data(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __server_heart_beat__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcServerHeartBeatRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerHeartBeatResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->server_heart_beat(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ServerControlProxy {
protected:
    srpc::Client* __cl__;
public:
    ServerControlProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcServerShutdownRequest = ServerControlService::RpcServerShutdownRequest;
    using RpcServerShutdownResponse = ServerControlService::RpcServerShutdownResponse;
    using RpcServerReadyRequest = ServerControlService::RpcServerReadyRequest;
    using RpcServerReadyResponse = ServerControlService::RpcServerReadyResponse;
    using RpcServerHeartBeatWithDataRequest = ServerControlService::RpcServerHeartBeatWithDataRequest;
    using RpcServerHeartBeatWithDataResponse = ServerControlService::RpcServerHeartBeatWithDataResponse;
    using RpcServerHeartBeatRequest = ServerControlService::RpcServerHeartBeatRequest;
    using RpcServerHeartBeatResponse = ServerControlService::RpcServerHeartBeatResponse;
    class server_shutdownTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit server_shutdownTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcServerShutdownResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerShutdownResponse, srpc::i32>::Err(__ret__);
            }
            RpcServerShutdownResponse __typed_resp__;
            return rusty::Result<RpcServerShutdownResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_shutdownTypedFuture, srpc::i32> async_server_shutdown(const RpcServerShutdownRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_SHUTDOWN, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_shutdownTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_shutdownTypedFuture, srpc::i32>::Ok(server_shutdownTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerShutdownResponse, srpc::i32> server_shutdown(const RpcServerShutdownRequest& req) {
        auto __typed_fu_result__ = this->async_server_shutdown(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerShutdownResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class server_readyTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit server_readyTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcServerReadyResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerReadyResponse, srpc::i32>::Err(__ret__);
            }
            RpcServerReadyResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcServerReadyResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_readyTypedFuture, srpc::i32> async_server_ready(const RpcServerReadyRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_READY, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_readyTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_readyTypedFuture, srpc::i32>::Ok(server_readyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerReadyResponse, srpc::i32> server_ready(const RpcServerReadyRequest& req) {
        auto __typed_fu_result__ = this->async_server_ready(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerReadyResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class server_heart_beat_with_dataTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit server_heart_beat_with_dataTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcServerHeartBeatWithDataResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerHeartBeatWithDataResponse, srpc::i32>::Err(__ret__);
            }
            RpcServerHeartBeatWithDataResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcServerHeartBeatWithDataResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_heart_beat_with_dataTypedFuture, srpc::i32> async_server_heart_beat_with_data(const RpcServerHeartBeatWithDataRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_HEART_BEAT_WITH_DATA, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_heart_beat_with_dataTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_heart_beat_with_dataTypedFuture, srpc::i32>::Ok(server_heart_beat_with_dataTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerHeartBeatWithDataResponse, srpc::i32> server_heart_beat_with_data(const RpcServerHeartBeatWithDataRequest& req) {
        auto __typed_fu_result__ = this->async_server_heart_beat_with_data(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerHeartBeatWithDataResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class server_heart_beatTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit server_heart_beatTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcServerHeartBeatResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerHeartBeatResponse, srpc::i32>::Err(__ret__);
            }
            RpcServerHeartBeatResponse __typed_resp__;
            return rusty::Result<RpcServerHeartBeatResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_heart_beatTypedFuture, srpc::i32> async_server_heart_beat(const RpcServerHeartBeatRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_HEART_BEAT, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_heart_beatTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_heart_beatTypedFuture, srpc::i32>::Ok(server_heart_beatTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerHeartBeatResponse, srpc::i32> server_heart_beat(const RpcServerHeartBeatRequest& req) {
        auto __typed_fu_result__ = this->async_server_heart_beat(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerHeartBeatResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class ClientControlService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcClientGetTxnNamesRequest {
    };
    friend inline void serialize(const RpcClientGetTxnNamesRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientGetTxnNamesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientGetTxnNamesRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientGetTxnNamesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientGetTxnNamesResponse {
        std::map<srpc::i32, std::string> txn_names;
    };
    friend inline void serialize(const RpcClientGetTxnNamesResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.txn_names, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientGetTxnNamesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientGetTxnNamesResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.txn_names, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientGetTxnNamesResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientShutdownRequest {
    };
    friend inline void serialize(const RpcClientShutdownRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientShutdownRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientShutdownRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientShutdownRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientShutdownResponse {
    };
    friend inline void serialize(const RpcClientShutdownResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientShutdownResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientShutdownResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientShutdownResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientForceStopRequest {
    };
    friend inline void serialize(const RpcClientForceStopRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientForceStopRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientForceStopRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientForceStopRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientForceStopResponse {
    };
    friend inline void serialize(const RpcClientForceStopResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientForceStopResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientForceStopResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientForceStopResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientResponseRequest {
        DepId dep_id;
    };
    friend inline void serialize(const RpcClientResponseRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientResponseRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientResponseRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientResponseRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientResponseResponse {
        ClientResponse res;
    };
    friend inline void serialize(const RpcClientResponseResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientResponseResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientResponseResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientResponseResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientReadyRequest {
    };
    friend inline void serialize(const RpcClientReadyRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientReadyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientReadyRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientReadyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientReadyResponse {
        srpc::i32 res;
    };
    friend inline void serialize(const RpcClientReadyResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientReadyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientReadyResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientReadyResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientReadyBlockRequest {
    };
    friend inline void serialize(const RpcClientReadyBlockRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientReadyBlockRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientReadyBlockRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientReadyBlockRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientReadyBlockResponse {
        srpc::i32 res;
    };
    friend inline void serialize(const RpcClientReadyBlockResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.res, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientReadyBlockResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientReadyBlockResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.res, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientReadyBlockResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientStartRequest {
    };
    friend inline void serialize(const RpcClientStartRequest& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientStartRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientStartRequest& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientStartRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientStartResponse {
    };
    friend inline void serialize(const RpcClientStartResponse& o, srpc::BinaryWriteArchive& ar) {
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcClientStartResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientStartResponse& o, srpc::BinaryReadArchive& ar) {
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcClientStartResponse& o) { deserialize(o, ar); return ar; }

    struct RpcDispatchTxnRequest {
        TxDispatchRequest req;
    };
    friend inline void serialize(const RpcDispatchTxnRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.req, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDispatchTxnRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDispatchTxnRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.req, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDispatchTxnRequest& o) { deserialize(o, ar); return ar; }

    struct RpcDispatchTxnResponse {
        TxReply result;
    };
    friend inline void serialize(const RpcDispatchTxnResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.result, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcDispatchTxnResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDispatchTxnResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.result, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcDispatchTxnResponse& o) { deserialize(o, ar); return ar; }

    enum {
        CLIENT_GET_TXN_NAMES = 0x5f6fbcfc,
        CLIENT_SHUTDOWN = 0x137d1d5e,
        CLIENT_FORCE_STOP = 0x56a23a61,
        CLIENT_RESPONSE = 0x181b3a96,
        CLIENT_READY = 0x3ff8b812,
        CLIENT_READY_BLOCK = 0x545f5490,
        CLIENT_START = 0x22d1ab87,
        DISPATCHTXN = 0x48796373,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(CLIENT_GET_TXN_NAMES, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(CLIENT_SHUTDOWN, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(CLIENT_FORCE_STOP, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(CLIENT_RESPONSE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(CLIENT_READY, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(CLIENT_READY_BLOCK, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(CLIENT_START, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(DISPATCHTXN, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(CLIENT_GET_TXN_NAMES);
        svr.unreg(CLIENT_SHUTDOWN);
        svr.unreg(CLIENT_FORCE_STOP);
        svr.unreg(CLIENT_RESPONSE);
        svr.unreg(CLIENT_READY);
        svr.unreg(CLIENT_READY_BLOCK);
        svr.unreg(CLIENT_START);
        svr.unreg(DISPATCHTXN);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case CLIENT_GET_TXN_NAMES: __client_get_txn_names__wrapper__(std::move(req), weak_sconn); break;
        case CLIENT_SHUTDOWN: __client_shutdown__wrapper__(std::move(req), weak_sconn); break;
        case CLIENT_FORCE_STOP: __client_force_stop__wrapper__(std::move(req), weak_sconn); break;
        case CLIENT_RESPONSE: __client_response__wrapper__(std::move(req), weak_sconn); break;
        case CLIENT_READY: __client_ready__wrapper__(std::move(req), weak_sconn); break;
        case CLIENT_READY_BLOCK: __client_ready_block__wrapper__(std::move(req), weak_sconn); break;
        case CLIENT_START: __client_start__wrapper__(std::move(req), weak_sconn); break;
        case DISPATCHTXN: __DispatchTxn__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void client_get_txn_names(const RpcClientGetTxnNamesRequest& req, RpcClientGetTxnNamesResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void client_shutdown(const RpcClientShutdownRequest& req, RpcClientShutdownResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void client_force_stop(const RpcClientForceStopRequest& req, RpcClientForceStopResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void client_response(const RpcClientResponseRequest& req, RpcClientResponseResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void client_ready(const RpcClientReadyRequest& req, RpcClientReadyResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void client_ready_block(const RpcClientReadyBlockRequest& req, RpcClientReadyBlockResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void client_start(const RpcClientStartRequest& req, RpcClientStartResponse& resp, srpc::DeferredReply defer) = 0;
    // @safe
    virtual void DispatchTxn(const RpcDispatchTxnRequest& req, RpcDispatchTxnResponse& resp, srpc::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __client_get_txn_names__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientGetTxnNamesRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientGetTxnNamesResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->txn_names, m);
                },
                []() {});
            this->client_get_txn_names(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_shutdown__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientShutdownRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientShutdownResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->client_shutdown(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_force_stop__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientForceStopRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientForceStopResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->client_force_stop(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_response__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientResponseRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcClientResponseResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->client_response(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_ready__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientReadyRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientReadyResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->client_ready(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_ready_block__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientReadyBlockRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientReadyBlockResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->client_ready_block(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_start__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientStartRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientStartResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                },
                []() {});
            this->client_start(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __DispatchTxn__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDispatchTxnRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.req, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcDispatchTxnResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->result, m);
                },
                []() {});
            this->DispatchTxn(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ClientControlProxy {
protected:
    srpc::Client* __cl__;
public:
    ClientControlProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcClientGetTxnNamesRequest = ClientControlService::RpcClientGetTxnNamesRequest;
    using RpcClientGetTxnNamesResponse = ClientControlService::RpcClientGetTxnNamesResponse;
    using RpcClientShutdownRequest = ClientControlService::RpcClientShutdownRequest;
    using RpcClientShutdownResponse = ClientControlService::RpcClientShutdownResponse;
    using RpcClientForceStopRequest = ClientControlService::RpcClientForceStopRequest;
    using RpcClientForceStopResponse = ClientControlService::RpcClientForceStopResponse;
    using RpcClientResponseRequest = ClientControlService::RpcClientResponseRequest;
    using RpcClientResponseResponse = ClientControlService::RpcClientResponseResponse;
    using RpcClientReadyRequest = ClientControlService::RpcClientReadyRequest;
    using RpcClientReadyResponse = ClientControlService::RpcClientReadyResponse;
    using RpcClientReadyBlockRequest = ClientControlService::RpcClientReadyBlockRequest;
    using RpcClientReadyBlockResponse = ClientControlService::RpcClientReadyBlockResponse;
    using RpcClientStartRequest = ClientControlService::RpcClientStartRequest;
    using RpcClientStartResponse = ClientControlService::RpcClientStartResponse;
    using RpcDispatchTxnRequest = ClientControlService::RpcDispatchTxnRequest;
    using RpcDispatchTxnResponse = ClientControlService::RpcDispatchTxnResponse;
    class client_get_txn_namesTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit client_get_txn_namesTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientGetTxnNamesResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientGetTxnNamesResponse, srpc::i32>::Err(__ret__);
            }
            RpcClientGetTxnNamesResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.txn_names, __reply_ar__);
            return rusty::Result<RpcClientGetTxnNamesResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_get_txn_namesTypedFuture, srpc::i32> async_client_get_txn_names(const RpcClientGetTxnNamesRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_GET_TXN_NAMES, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_get_txn_namesTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_get_txn_namesTypedFuture, srpc::i32>::Ok(client_get_txn_namesTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientGetTxnNamesResponse, srpc::i32> client_get_txn_names(const RpcClientGetTxnNamesRequest& req) {
        auto __typed_fu_result__ = this->async_client_get_txn_names(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientGetTxnNamesResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_shutdownTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit client_shutdownTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientShutdownResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientShutdownResponse, srpc::i32>::Err(__ret__);
            }
            RpcClientShutdownResponse __typed_resp__;
            return rusty::Result<RpcClientShutdownResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_shutdownTypedFuture, srpc::i32> async_client_shutdown(const RpcClientShutdownRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_SHUTDOWN, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_shutdownTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_shutdownTypedFuture, srpc::i32>::Ok(client_shutdownTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientShutdownResponse, srpc::i32> client_shutdown(const RpcClientShutdownRequest& req) {
        auto __typed_fu_result__ = this->async_client_shutdown(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientShutdownResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_force_stopTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit client_force_stopTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientForceStopResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientForceStopResponse, srpc::i32>::Err(__ret__);
            }
            RpcClientForceStopResponse __typed_resp__;
            return rusty::Result<RpcClientForceStopResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_force_stopTypedFuture, srpc::i32> async_client_force_stop(const RpcClientForceStopRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_FORCE_STOP, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_force_stopTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_force_stopTypedFuture, srpc::i32>::Ok(client_force_stopTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientForceStopResponse, srpc::i32> client_force_stop(const RpcClientForceStopRequest& req) {
        auto __typed_fu_result__ = this->async_client_force_stop(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientForceStopResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_responseTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit client_responseTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientResponseResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientResponseResponse, srpc::i32>::Err(__ret__);
            }
            RpcClientResponseResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcClientResponseResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_responseTypedFuture, srpc::i32> async_client_response(const RpcClientResponseRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_RESPONSE, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<client_responseTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<client_responseTypedFuture, srpc::i32>::Ok(client_responseTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientResponseResponse, srpc::i32> client_response(const RpcClientResponseRequest& req) {
        auto __typed_fu_result__ = this->async_client_response(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientResponseResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_readyTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit client_readyTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientReadyResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientReadyResponse, srpc::i32>::Err(__ret__);
            }
            RpcClientReadyResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcClientReadyResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_readyTypedFuture, srpc::i32> async_client_ready(const RpcClientReadyRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_READY, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_readyTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_readyTypedFuture, srpc::i32>::Ok(client_readyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientReadyResponse, srpc::i32> client_ready(const RpcClientReadyRequest& req) {
        auto __typed_fu_result__ = this->async_client_ready(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientReadyResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_ready_blockTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit client_ready_blockTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientReadyBlockResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientReadyBlockResponse, srpc::i32>::Err(__ret__);
            }
            RpcClientReadyBlockResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcClientReadyBlockResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_ready_blockTypedFuture, srpc::i32> async_client_ready_block(const RpcClientReadyBlockRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_READY_BLOCK, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_ready_blockTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_ready_blockTypedFuture, srpc::i32>::Ok(client_ready_blockTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientReadyBlockResponse, srpc::i32> client_ready_block(const RpcClientReadyBlockRequest& req) {
        auto __typed_fu_result__ = this->async_client_ready_block(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientReadyBlockResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_startTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit client_startTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientStartResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientStartResponse, srpc::i32>::Err(__ret__);
            }
            RpcClientStartResponse __typed_resp__;
            return rusty::Result<RpcClientStartResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_startTypedFuture, srpc::i32> async_client_start(const RpcClientStartRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_START, __fu_attr__, [](srpc::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_startTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_startTypedFuture, srpc::i32>::Ok(client_startTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientStartResponse, srpc::i32> client_start(const RpcClientStartRequest& req) {
        auto __typed_fu_result__ = this->async_client_start(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientStartResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class DispatchTxnTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit DispatchTxnTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcDispatchTxnResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcDispatchTxnResponse, srpc::i32>::Err(__ret__);
            }
            RpcDispatchTxnResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.result, __reply_ar__);
            return rusty::Result<RpcDispatchTxnResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<DispatchTxnTypedFuture, srpc::i32> async_DispatchTxn(const RpcDispatchTxnRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::DISPATCHTXN, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.req, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<DispatchTxnTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<DispatchTxnTypedFuture, srpc::i32>::Ok(DispatchTxnTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcDispatchTxnResponse, srpc::i32> DispatchTxn(const RpcDispatchTxnRequest& req) {
        auto __typed_fu_result__ = this->async_DispatchTxn(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDispatchTxnResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class ConfigKvServiceService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcReadConfigKeyRequest {
        std::string key;
    };
    friend inline void serialize(const RpcReadConfigKeyRequest& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.key, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcReadConfigKeyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReadConfigKeyRequest& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.key, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcReadConfigKeyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcReadConfigKeyResponse {
        srpc::i32 found;
        std::string value;
    };
    friend inline void serialize(const RpcReadConfigKeyResponse& o, srpc::BinaryWriteArchive& ar) {
        srpc::Serialize_::serialize(o.found, ar);
        srpc::Serialize_::serialize(o.value, ar);
    }
    friend inline srpc::BinaryWriteArchive& operator <<(srpc::BinaryWriteArchive& ar, const RpcReadConfigKeyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReadConfigKeyResponse& o, srpc::BinaryReadArchive& ar) {
        srpc::Deserialize_::deserialize(o.found, ar);
        srpc::Deserialize_::deserialize(o.value, ar);
    }
    friend inline srpc::BinaryReadArchive& operator >>(srpc::BinaryReadArchive& ar, RpcReadConfigKeyResponse& o) { deserialize(o, ar); return ar; }

    enum {
        READCONFIGKEY = 0x584cdad1,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls srpc::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(srpc::Server& svr, size_t svc_index) {
        int ret = 0;
        if ((ret = svr.reg_rpc(READCONFIGKEY, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(READCONFIGKEY);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(srpc::i32 rpc_id, rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case READCONFIGKEY: __ReadConfigKey__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void ReadConfigKey(const RpcReadConfigKeyRequest& req, RpcReadConfigKeyResponse& resp, srpc::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __ReadConfigKey__wrapper__(rusty::Box<srpc::Request> req, srpc::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcReadConfigKeyRequest __typed_req__;
            srpc::BinaryReadArchive __req_ar__(srpc::make_source_proxy_buffer(&req->src));
            srpc::Deserialize_::deserialize(__typed_req__.key, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcReadConfigKeyResponse>();
            auto __defer__ = srpc::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](srpc::BinaryWriteArchive& m) {
                    srpc::Serialize_::serialize(__typed_resp__->found, m);
                    srpc::Serialize_::serialize(__typed_resp__->value, m);
                },
                []() {});
            this->ReadConfigKey(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ConfigKvServiceProxy {
protected:
    srpc::Client* __cl__;
public:
    ConfigKvServiceProxy(srpc::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcReadConfigKeyRequest = ConfigKvServiceService::RpcReadConfigKeyRequest;
    using RpcReadConfigKeyResponse = ConfigKvServiceService::RpcReadConfigKeyResponse;
    class ReadConfigKeyTypedFuture {
    private:
        rusty::Arc<srpc::Future> __fu__;
    public:
        explicit ReadConfigKeyTypedFuture(rusty::Arc<srpc::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        srpc::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<srpc::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcReadConfigKeyResponse, srpc::i32> resolve() const {
            srpc::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcReadConfigKeyResponse, srpc::i32>::Err(__ret__);
            }
            RpcReadConfigKeyResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            srpc::BinaryReadArchive __reply_ar__(srpc::make_source_proxy_buffer(&__reply_guard__->src));
            srpc::Deserialize_::deserialize(__typed_resp__.found, __reply_ar__);
            srpc::Deserialize_::deserialize(__typed_resp__.value, __reply_ar__);
            return rusty::Result<RpcReadConfigKeyResponse, srpc::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ReadConfigKeyTypedFuture, srpc::i32> async_ReadConfigKey(const RpcReadConfigKeyRequest& req, const srpc::FutureAttr& __fu_attr__ = srpc::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigKvServiceService::READCONFIGKEY, __fu_attr__, [&](srpc::BinaryWriteArchive& __m__) {
            srpc::Serialize_::serialize(req.key, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<ReadConfigKeyTypedFuture, srpc::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<ReadConfigKeyTypedFuture, srpc::i32>::Ok(ReadConfigKeyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcReadConfigKeyResponse, srpc::i32> ReadConfigKey(const RpcReadConfigKeyRequest& req) {
        auto __typed_fu_result__ = this->async_ReadConfigKey(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcReadConfigKeyResponse, srpc::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

} // namespace janus



