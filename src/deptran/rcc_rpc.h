#pragma once

#include "rrr/rrr.hpp"
#include <rusty/async.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/result.hpp>

#include <errno.h>
#include <memory>

#include "procedure.h"
namespace janus {

struct ValueTimesPair {
    rrr::i64 value;
    rrr::i64 times;
};

inline void serialize(const ValueTimesPair& o, rrr::BinaryWriteArchive& ar) {
    rrr::Serialize_::serialize(o.value, ar);
    rrr::Serialize_::serialize(o.times, ar);
}

inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const ValueTimesPair& o) { serialize(o, ar); return ar; }

inline void deserialize(ValueTimesPair& o, rrr::BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(o.value, ar);
    rrr::Deserialize_::deserialize(o.times, ar);
}

inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, ValueTimesPair& o) { deserialize(o, ar); return ar; }

struct DepId {
    std::string str;
    rrr::i64 id;
};

inline void serialize(const DepId& o, rrr::BinaryWriteArchive& ar) {
    rrr::Serialize_::serialize(o.str, ar);
    rrr::Serialize_::serialize(o.id, ar);
}

inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const DepId& o) { serialize(o, ar); return ar; }

inline void deserialize(DepId& o, rrr::BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(o.str, ar);
    rrr::Deserialize_::deserialize(o.id, ar);
}

inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, DepId& o) { deserialize(o, ar); return ar; }

struct TxnInfoRes {
    rrr::i32 start_txn;
    rrr::i32 total_txn;
    rrr::i32 total_try;
    rrr::i32 commit_txn;
    rrr::i32 num_exhausted;
    std::vector<double> this_latency;
    std::vector<double> last_latency;
    std::vector<double> attempt_latency;
    std::vector<double> interval_latency;
    std::vector<double> all_interval_latency;
    std::vector<rrr::i32> num_try;
};

inline void serialize(const TxnInfoRes& o, rrr::BinaryWriteArchive& ar) {
    rrr::Serialize_::serialize(o.start_txn, ar);
    rrr::Serialize_::serialize(o.total_txn, ar);
    rrr::Serialize_::serialize(o.total_try, ar);
    rrr::Serialize_::serialize(o.commit_txn, ar);
    rrr::Serialize_::serialize(o.num_exhausted, ar);
    rrr::Serialize_::serialize(o.this_latency, ar);
    rrr::Serialize_::serialize(o.last_latency, ar);
    rrr::Serialize_::serialize(o.attempt_latency, ar);
    rrr::Serialize_::serialize(o.interval_latency, ar);
    rrr::Serialize_::serialize(o.all_interval_latency, ar);
    rrr::Serialize_::serialize(o.num_try, ar);
}

inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const TxnInfoRes& o) { serialize(o, ar); return ar; }

inline void deserialize(TxnInfoRes& o, rrr::BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(o.start_txn, ar);
    rrr::Deserialize_::deserialize(o.total_txn, ar);
    rrr::Deserialize_::deserialize(o.total_try, ar);
    rrr::Deserialize_::deserialize(o.commit_txn, ar);
    rrr::Deserialize_::deserialize(o.num_exhausted, ar);
    rrr::Deserialize_::deserialize(o.this_latency, ar);
    rrr::Deserialize_::deserialize(o.last_latency, ar);
    rrr::Deserialize_::deserialize(o.attempt_latency, ar);
    rrr::Deserialize_::deserialize(o.interval_latency, ar);
    rrr::Deserialize_::deserialize(o.all_interval_latency, ar);
    rrr::Deserialize_::deserialize(o.num_try, ar);
}

inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, TxnInfoRes& o) { deserialize(o, ar); return ar; }

struct ServerResponse {
    std::map<std::string, ValueTimesPair> statistics;
    rrr::i64 r_cnt_sum;
    rrr::i64 r_cnt_num;
    rrr::i64 r_sz_sum;
    rrr::i64 r_sz_num;
};

inline void serialize(const ServerResponse& o, rrr::BinaryWriteArchive& ar) {
    rrr::Serialize_::serialize(o.statistics, ar);
    rrr::Serialize_::serialize(o.r_cnt_sum, ar);
    rrr::Serialize_::serialize(o.r_cnt_num, ar);
    rrr::Serialize_::serialize(o.r_sz_sum, ar);
    rrr::Serialize_::serialize(o.r_sz_num, ar);
}

inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const ServerResponse& o) { serialize(o, ar); return ar; }

inline void deserialize(ServerResponse& o, rrr::BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(o.statistics, ar);
    rrr::Deserialize_::deserialize(o.r_cnt_sum, ar);
    rrr::Deserialize_::deserialize(o.r_cnt_num, ar);
    rrr::Deserialize_::deserialize(o.r_sz_sum, ar);
    rrr::Deserialize_::deserialize(o.r_sz_num, ar);
}

inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, ServerResponse& o) { deserialize(o, ar); return ar; }

struct ClientResponse {
    std::map<rrr::i32, TxnInfoRes> txn_info;
    rrr::i64 run_sec;
    rrr::i64 run_nsec;
    rrr::i64 period_sec;
    rrr::i64 period_nsec;
    rrr::i32 is_finish;
};

inline void serialize(const ClientResponse& o, rrr::BinaryWriteArchive& ar) {
    rrr::Serialize_::serialize(o.txn_info, ar);
    rrr::Serialize_::serialize(o.run_sec, ar);
    rrr::Serialize_::serialize(o.run_nsec, ar);
    rrr::Serialize_::serialize(o.period_sec, ar);
    rrr::Serialize_::serialize(o.period_nsec, ar);
    rrr::Serialize_::serialize(o.is_finish, ar);
}

inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const ClientResponse& o) { serialize(o, ar); return ar; }

inline void deserialize(ClientResponse& o, rrr::BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(o.txn_info, ar);
    rrr::Deserialize_::deserialize(o.run_sec, ar);
    rrr::Deserialize_::deserialize(o.run_nsec, ar);
    rrr::Deserialize_::deserialize(o.period_sec, ar);
    rrr::Deserialize_::deserialize(o.period_nsec, ar);
    rrr::Deserialize_::deserialize(o.is_finish, ar);
}

inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, ClientResponse& o) { deserialize(o, ar); return ar; }

struct TxDispatchRequest {
    rrr::i32 id;
    rrr::i32 tx_type;
    std::vector<Value> input;
};

inline void serialize(const TxDispatchRequest& o, rrr::BinaryWriteArchive& ar) {
    rrr::Serialize_::serialize(o.id, ar);
    rrr::Serialize_::serialize(o.tx_type, ar);
    rrr::Serialize_::serialize(o.input, ar);
}

inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const TxDispatchRequest& o) { serialize(o, ar); return ar; }

inline void deserialize(TxDispatchRequest& o, rrr::BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(o.id, ar);
    rrr::Deserialize_::deserialize(o.tx_type, ar);
    rrr::Deserialize_::deserialize(o.input, ar);
}

inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, TxDispatchRequest& o) { deserialize(o, ar); return ar; }

struct TxnDispatchResponse {
};

inline void serialize(const TxnDispatchResponse& o, rrr::BinaryWriteArchive& ar) {
}

inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const TxnDispatchResponse& o) { serialize(o, ar); return ar; }

inline void deserialize(TxnDispatchResponse& o, rrr::BinaryReadArchive& ar) {
}

inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, TxnDispatchResponse& o) { deserialize(o, ar); return ar; }

class MultiPaxosService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcPrepareRequest {
        uint64_t slot;
        ballot_t ballot;
    };
    friend inline void serialize(const RpcPrepareRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcPrepareRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcPrepareRequest& o) { deserialize(o, ar); return ar; }

    struct RpcPrepareResponse {
        ballot_t max_ballot;
        uint64_t coro_id;
    };
    friend inline void serialize(const RpcPrepareResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.max_ballot, ar);
        rrr::Serialize_::serialize(o.coro_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcPrepareResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.max_ballot, ar);
        rrr::Deserialize_::deserialize(o.coro_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcPrepareResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAcceptRequest {
        uint64_t slot;
        uint64_t time;
        ballot_t ballot;
        Command cmd;
    };
    friend inline void serialize(const RpcAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.time, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.time, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAcceptResponse {
        ballot_t max_ballot;
        uint64_t coro_id;
    };
    friend inline void serialize(const RpcAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.max_ballot, ar);
        rrr::Serialize_::serialize(o.coro_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAcceptResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.max_ballot, ar);
        rrr::Deserialize_::deserialize(o.coro_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcDecideRequest {
        uint64_t slot;
        ballot_t ballot;
        Command cmd;
    };
    friend inline void serialize(const RpcDecideRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcDecideRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDecideRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcDecideRequest& o) { deserialize(o, ar); return ar; }

    struct RpcDecideResponse {
    };
    friend inline void serialize(const RpcDecideResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcDecideResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDecideResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcDecideResponse& o) { deserialize(o, ar); return ar; }

    struct RpcForwardToLearnerServerRequest {
        rrr::i32 par_id;
        uint64_t slot;
        ballot_t ballot;
        Command cmd;
    };
    friend inline void serialize(const RpcForwardToLearnerServerRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.par_id, ar);
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcForwardToLearnerServerRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcForwardToLearnerServerRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.par_id, ar);
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcForwardToLearnerServerRequest& o) { deserialize(o, ar); return ar; }

    struct RpcForwardToLearnerServerResponse {
        uint64_t ret_slot;
        ballot_t ret_ballot;
    };
    friend inline void serialize(const RpcForwardToLearnerServerResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ret_slot, ar);
        rrr::Serialize_::serialize(o.ret_ballot, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcForwardToLearnerServerResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcForwardToLearnerServerResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ret_slot, ar);
        rrr::Deserialize_::deserialize(o.ret_ballot, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcForwardToLearnerServerResponse& o) { deserialize(o, ar); return ar; }

    struct RpcBulkAcceptRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcBulkAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcBulkAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcBulkAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcBulkAcceptResponse {
        rrr::i32 ballot;
        rrr::i32 val;
    };
    friend inline void serialize(const RpcBulkAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.val, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcBulkAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkAcceptResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.val, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcBulkAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcSyncLogRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcSyncLogRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSyncLogRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncLogRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSyncLogRequest& o) { deserialize(o, ar); return ar; }

    struct RpcSyncLogResponse {
        rrr::i32 ballot;
        rrr::i32 val;
        Command ret;
    };
    friend inline void serialize(const RpcSyncLogResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.val, ar);
        rrr::Serialize_::serialize(o.ret, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSyncLogResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncLogResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.val, ar);
        rrr::Deserialize_::deserialize(o.ret, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSyncLogResponse& o) { deserialize(o, ar); return ar; }

    struct RpcSyncCommitRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcSyncCommitRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSyncCommitRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncCommitRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSyncCommitRequest& o) { deserialize(o, ar); return ar; }

    struct RpcSyncCommitResponse {
        rrr::i32 ballot;
        rrr::i32 val;
    };
    friend inline void serialize(const RpcSyncCommitResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.val, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSyncCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSyncCommitResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.val, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSyncCommitResponse& o) { deserialize(o, ar); return ar; }

    struct RpcBulkDecideRequest {
        Command cmd;
    };
    friend inline void serialize(const RpcBulkDecideRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcBulkDecideRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkDecideRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcBulkDecideRequest& o) { deserialize(o, ar); return ar; }

    struct RpcBulkDecideResponse {
        rrr::i32 ballot;
        rrr::i32 val;
    };
    friend inline void serialize(const RpcBulkDecideResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.val, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcBulkDecideResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcBulkDecideResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.val, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcBulkDecideResponse& o) { deserialize(o, ar); return ar; }

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
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
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
    virtual void Prepare(const RpcPrepareRequest& req, RpcPrepareResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Accept(const RpcAcceptRequest& req, RpcAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Decide(const RpcDecideRequest& req, RpcDecideResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req, RpcForwardToLearnerServerResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void BulkAccept(const RpcBulkAcceptRequest& req, RpcBulkAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void SyncLog(const RpcSyncLogRequest& req, RpcSyncLogResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void SyncCommit(const RpcSyncCommitRequest& req, RpcSyncCommitResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void BulkDecide(const RpcBulkDecideRequest& req, RpcBulkDecideResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Prepare__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPrepareRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcPrepareResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->coro_id, m);
                },
                []() {});
            this->Prepare(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Accept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAcceptRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.time, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->coro_id, m);
                },
                []() {});
            this->Accept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Decide__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDecideRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcDecideResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->Decide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __ForwardToLearnerServer__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcForwardToLearnerServerRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.par_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcForwardToLearnerServerResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ret_slot, m);
                    rrr::Serialize_::serialize(__typed_resp__->ret_ballot, m);
                },
                []() {});
            this->ForwardToLearnerServer(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcBulkAcceptRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcBulkAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->val, m);
                },
                []() {});
            this->BulkAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SyncLog__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSyncLogRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcSyncLogResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->val, m);
                    rrr::Serialize_::serialize(__typed_resp__->ret, m);
                },
                []() {});
            this->SyncLog(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SyncCommit__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSyncCommitRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcSyncCommitResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->val, m);
                },
                []() {});
            this->SyncCommit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkDecide__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcBulkDecideRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcBulkDecideResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->val, m);
                },
                []() {});
            this->BulkDecide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class MultiPaxosProxy {
protected:
    rrr::Client* __cl__;
public:
    MultiPaxosProxy(rrr::Client* cl): __cl__(cl) { }
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
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit PrepareTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcPrepareResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcPrepareResponse, rrr::i32>::Err(__ret__);
            }
            RpcPrepareResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<PrepareTypedFuture, rrr::i32> async_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::PREPARE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<PrepareTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<PrepareTypedFuture, rrr::i32>::Ok(PrepareTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcPrepareResponse, rrr::i32> Prepare(const RpcPrepareRequest& req) {
        auto __typed_fu_result__ = this->async_Prepare(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit AcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            return rusty::Result<RpcAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AcceptTypedFuture, rrr::i32> async_Accept(const RpcAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::ACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.time, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AcceptTypedFuture, rrr::i32>::Ok(AcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAcceptResponse, rrr::i32> Accept(const RpcAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_Accept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class DecideTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit DecideTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcDecideResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcDecideResponse, rrr::i32>::Err(__ret__);
            }
            RpcDecideResponse __typed_resp__;
            return rusty::Result<RpcDecideResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<DecideTypedFuture, rrr::i32> async_Decide(const RpcDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::DECIDE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<DecideTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<DecideTypedFuture, rrr::i32>::Ok(DecideTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcDecideResponse, rrr::i32> Decide(const RpcDecideRequest& req) {
        auto __typed_fu_result__ = this->async_Decide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDecideResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class ForwardToLearnerServerTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit ForwardToLearnerServerTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32>::Err(__ret__);
            }
            RpcForwardToLearnerServerResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ret_slot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.ret_ballot, __reply_ar__);
            return rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ForwardToLearnerServerTypedFuture, rrr::i32> async_ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::FORWARDTOLEARNERSERVER, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.par_id, __m__);
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<ForwardToLearnerServerTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<ForwardToLearnerServerTypedFuture, rrr::i32>::Ok(ForwardToLearnerServerTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32> ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req) {
        auto __typed_fu_result__ = this->async_ForwardToLearnerServer(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class BulkAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit BulkAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcBulkAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcBulkAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcBulkAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcBulkAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkAcceptTypedFuture, rrr::i32> async_BulkAccept(const RpcBulkAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<BulkAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<BulkAcceptTypedFuture, rrr::i32>::Ok(BulkAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcBulkAcceptResponse, rrr::i32> BulkAccept(const RpcBulkAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_BulkAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcBulkAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class SyncLogTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit SyncLogTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcSyncLogResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSyncLogResponse, rrr::i32>::Err(__ret__);
            }
            RpcSyncLogResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.ret, __reply_ar__);
            return rusty::Result<RpcSyncLogResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SyncLogTypedFuture, rrr::i32> async_SyncLog(const RpcSyncLogRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::SYNCLOG, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SyncLogTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SyncLogTypedFuture, rrr::i32>::Ok(SyncLogTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSyncLogResponse, rrr::i32> SyncLog(const RpcSyncLogRequest& req) {
        auto __typed_fu_result__ = this->async_SyncLog(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSyncLogResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class SyncCommitTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit SyncCommitTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcSyncCommitResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSyncCommitResponse, rrr::i32>::Err(__ret__);
            }
            RpcSyncCommitResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcSyncCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SyncCommitTypedFuture, rrr::i32> async_SyncCommit(const RpcSyncCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::SYNCCOMMIT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SyncCommitTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SyncCommitTypedFuture, rrr::i32>::Ok(SyncCommitTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSyncCommitResponse, rrr::i32> SyncCommit(const RpcSyncCommitRequest& req) {
        auto __typed_fu_result__ = this->async_SyncCommit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSyncCommitResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class BulkDecideTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit BulkDecideTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcBulkDecideResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcBulkDecideResponse, rrr::i32>::Err(__ret__);
            }
            RpcBulkDecideResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcBulkDecideResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkDecideTypedFuture, rrr::i32> async_BulkDecide(const RpcBulkDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKDECIDE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<BulkDecideTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<BulkDecideTypedFuture, rrr::i32>::Ok(BulkDecideTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcBulkDecideResponse, rrr::i32> BulkDecide(const RpcBulkDecideRequest& req) {
        auto __typed_fu_result__ = this->async_BulkDecide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcBulkDecideResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
    friend inline void serialize(const RpcVoteRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.lst_log_idx, ar);
        rrr::Serialize_::serialize(o.lst_log_term, ar);
        rrr::Serialize_::serialize(o.site_id, ar);
        rrr::Serialize_::serialize(o.cur_term, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcVoteRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.lst_log_idx, ar);
        rrr::Deserialize_::deserialize(o.lst_log_term, ar);
        rrr::Deserialize_::deserialize(o.site_id, ar);
        rrr::Deserialize_::deserialize(o.cur_term, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcVoteRequest& o) { deserialize(o, ar); return ar; }

    struct RpcVoteResponse {
        ballot_t max_ballot;
        bool_t vote_granted;
    };
    friend inline void serialize(const RpcVoteResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.max_ballot, ar);
        rrr::Serialize_::serialize(o.vote_granted, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcVoteResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.max_ballot, ar);
        rrr::Deserialize_::deserialize(o.vote_granted, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcVoteResponse& o) { deserialize(o, ar); return ar; }

    struct RpcVoteDurableRequest {
        ballot_t term;
        siteid_t voter_id;
    };
    friend inline void serialize(const RpcVoteDurableRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term, ar);
        rrr::Serialize_::serialize(o.voter_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcVoteDurableRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteDurableRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term, ar);
        rrr::Deserialize_::deserialize(o.voter_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcVoteDurableRequest& o) { deserialize(o, ar); return ar; }

    struct RpcVoteDurableResponse {
        bool_t acknowledged;
    };
    friend inline void serialize(const RpcVoteDurableResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcVoteDurableResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteDurableResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcVoteDurableResponse& o) { deserialize(o, ar); return ar; }

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
    friend inline void serialize(const RpcAppendEntriesRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.leaderCurrentTerm, ar);
        rrr::Serialize_::serialize(o.leaderSiteId, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        rrr::Serialize_::serialize(o.leaderCommitIndex, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
        rrr::Serialize_::serialize(o.leaderNextLogTerm, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntriesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderSiteId, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderCommitIndex, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
        rrr::Deserialize_::deserialize(o.leaderNextLogTerm, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntriesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
        uint64_t followerAckType;
    };
    friend inline void serialize(const RpcAppendEntriesResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.followerAppendOK, ar);
        rrr::Serialize_::serialize(o.followerCurrentTerm, ar);
        rrr::Serialize_::serialize(o.followerLastLogIndex, ar);
        rrr::Serialize_::serialize(o.followerAckType, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntriesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.followerAppendOK, ar);
        rrr::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.followerLastLogIndex, ar);
        rrr::Deserialize_::deserialize(o.followerAckType, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntriesResponse& o) { deserialize(o, ar); return ar; }

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
    friend inline void serialize(const RpcEmptyAppendEntriesRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.leaderCurrentTerm, ar);
        rrr::Serialize_::serialize(o.leaderSiteId, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        rrr::Serialize_::serialize(o.leaderCommitIndex, ar);
        rrr::Serialize_::serialize(o.trigger_election_now, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcEmptyAppendEntriesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEmptyAppendEntriesRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderSiteId, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderCommitIndex, ar);
        rrr::Deserialize_::deserialize(o.trigger_election_now, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcEmptyAppendEntriesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcEmptyAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
        uint64_t followerAckType;
    };
    friend inline void serialize(const RpcEmptyAppendEntriesResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.followerAppendOK, ar);
        rrr::Serialize_::serialize(o.followerCurrentTerm, ar);
        rrr::Serialize_::serialize(o.followerLastLogIndex, ar);
        rrr::Serialize_::serialize(o.followerAckType, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcEmptyAppendEntriesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEmptyAppendEntriesResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.followerAppendOK, ar);
        rrr::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.followerLastLogIndex, ar);
        rrr::Deserialize_::deserialize(o.followerAckType, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcEmptyAppendEntriesResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesDurableRequest {
        ballot_t term;
        siteid_t follower_id;
        uint64_t lastLogIndex;
    };
    friend inline void serialize(const RpcAppendEntriesDurableRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term, ar);
        rrr::Serialize_::serialize(o.follower_id, ar);
        rrr::Serialize_::serialize(o.lastLogIndex, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntriesDurableRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesDurableRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term, ar);
        rrr::Deserialize_::deserialize(o.follower_id, ar);
        rrr::Deserialize_::deserialize(o.lastLogIndex, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntriesDurableRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesDurableResponse {
        bool_t acknowledged;
    };
    friend inline void serialize(const RpcAppendEntriesDurableResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntriesDurableResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesDurableResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntriesDurableResponse& o) { deserialize(o, ar); return ar; }

    struct RpcTimeoutNowRequest {
        uint64_t leaderTerm;
        siteid_t leaderSiteId;
    };
    friend inline void serialize(const RpcTimeoutNowRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.leaderTerm, ar);
        rrr::Serialize_::serialize(o.leaderSiteId, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTimeoutNowRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTimeoutNowRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.leaderTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderSiteId, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTimeoutNowRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTimeoutNowResponse {
        uint64_t followerTerm;
        bool_t success;
    };
    friend inline void serialize(const RpcTimeoutNowResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.followerTerm, ar);
        rrr::Serialize_::serialize(o.success, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTimeoutNowResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTimeoutNowResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.followerTerm, ar);
        rrr::Deserialize_::deserialize(o.success, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTimeoutNowResponse& o) { deserialize(o, ar); return ar; }

    struct RpcNotifyRestartRequest {
        siteid_t restartedSiteId;
    };
    friend inline void serialize(const RpcNotifyRestartRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.restartedSiteId, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcNotifyRestartRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcNotifyRestartRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.restartedSiteId, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcNotifyRestartRequest& o) { deserialize(o, ar); return ar; }

    struct RpcNotifyRestartResponse {
        bool_t acknowledged;
    };
    friend inline void serialize(const RpcNotifyRestartResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcNotifyRestartResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcNotifyRestartResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.acknowledged, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcNotifyRestartResponse& o) { deserialize(o, ar); return ar; }

    struct RpcInstallSnapshotRequest {
        uint64_t term;
        uint64_t leader_id;
        uint64_t last_included_index;
        uint64_t last_included_term;
        std::string data;
    };
    friend inline void serialize(const RpcInstallSnapshotRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term, ar);
        rrr::Serialize_::serialize(o.leader_id, ar);
        rrr::Serialize_::serialize(o.last_included_index, ar);
        rrr::Serialize_::serialize(o.last_included_term, ar);
        rrr::Serialize_::serialize(o.data, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcInstallSnapshotRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcInstallSnapshotRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term, ar);
        rrr::Deserialize_::deserialize(o.leader_id, ar);
        rrr::Deserialize_::deserialize(o.last_included_index, ar);
        rrr::Deserialize_::deserialize(o.last_included_term, ar);
        rrr::Deserialize_::deserialize(o.data, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcInstallSnapshotRequest& o) { deserialize(o, ar); return ar; }

    struct RpcInstallSnapshotResponse {
        uint64_t term_out;
    };
    friend inline void serialize(const RpcInstallSnapshotResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term_out, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcInstallSnapshotResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcInstallSnapshotResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term_out, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcInstallSnapshotResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAddServerRequest {
        uint64_t term;
        uint64_t new_server_id;
        std::string new_server_addr;
    };
    friend inline void serialize(const RpcAddServerRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term, ar);
        rrr::Serialize_::serialize(o.new_server_id, ar);
        rrr::Serialize_::serialize(o.new_server_addr, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAddServerRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAddServerRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term, ar);
        rrr::Deserialize_::deserialize(o.new_server_id, ar);
        rrr::Deserialize_::deserialize(o.new_server_addr, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAddServerRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAddServerResponse {
        bool_t success;
        std::string error_msg;
        uint64_t leader_hint;
    };
    friend inline void serialize(const RpcAddServerResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.success, ar);
        rrr::Serialize_::serialize(o.error_msg, ar);
        rrr::Serialize_::serialize(o.leader_hint, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAddServerResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAddServerResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.success, ar);
        rrr::Deserialize_::deserialize(o.error_msg, ar);
        rrr::Deserialize_::deserialize(o.leader_hint, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAddServerResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRemoveServerRequest {
        uint64_t term;
        uint64_t server_id;
    };
    friend inline void serialize(const RpcRemoveServerRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.term, ar);
        rrr::Serialize_::serialize(o.server_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRemoveServerRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRemoveServerRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.term, ar);
        rrr::Deserialize_::deserialize(o.server_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRemoveServerRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRemoveServerResponse {
        bool_t success;
        std::string error_msg;
        uint64_t leader_hint;
    };
    friend inline void serialize(const RpcRemoveServerResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.success, ar);
        rrr::Serialize_::serialize(o.error_msg, ar);
        rrr::Serialize_::serialize(o.leader_hint, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRemoveServerResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRemoveServerResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.success, ar);
        rrr::Deserialize_::deserialize(o.error_msg, ar);
        rrr::Deserialize_::deserialize(o.leader_hint, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRemoveServerResponse& o) { deserialize(o, ar); return ar; }

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
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
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
    virtual rusty::Result<RpcVoteResponse, rrr::i32> Vote(const RpcVoteRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcVoteDurableResponse, rrr::i32> VoteDurable(const RpcVoteDurableRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcAppendEntriesResponse, rrr::i32> AppendEntries(const RpcAppendEntriesRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32> AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcTimeoutNowResponse, rrr::i32> TimeoutNow(const RpcTimeoutNowRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcNotifyRestartResponse, rrr::i32> NotifyRestart(const RpcNotifyRestartRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcInstallSnapshotResponse, rrr::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcAddServerResponse, rrr::i32> AddServer(const RpcAddServerRequest& req) = 0;
    // @safe
    virtual rusty::Result<RpcRemoveServerResponse, rrr::i32> RemoveServer(const RpcRemoveServerRequest& req) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Vote__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVoteRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.lst_log_idx, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.lst_log_term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.site_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cur_term, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->Vote(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.max_ballot, m);
                            rrr::Serialize_::serialize(__typed_resp__.vote_granted, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __VoteDurable__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVoteDurableRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.voter_id, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->VoteDurable(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.acknowledged, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __AppendEntries__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntriesRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderSiteId, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderNextLogTerm, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->AppendEntries(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.followerAppendOK, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerCurrentTerm, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerLastLogIndex, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerAckType, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __EmptyAppendEntries__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcEmptyAppendEntriesRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderSiteId, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.trigger_election_now, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->EmptyAppendEntries(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.followerAppendOK, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerCurrentTerm, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerLastLogIndex, m);
                            rrr::Serialize_::serialize(__typed_resp__.followerAckType, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __AppendEntriesDurable__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntriesDurableRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.follower_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.lastLogIndex, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->AppendEntriesDurable(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.acknowledged, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __TimeoutNow__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTimeoutNowRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.leaderTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderSiteId, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->TimeoutNow(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.followerTerm, m);
                            rrr::Serialize_::serialize(__typed_resp__.success, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __NotifyRestart__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcNotifyRestartRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.restartedSiteId, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->NotifyRestart(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.acknowledged, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __InstallSnapshot__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcInstallSnapshotRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leader_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.last_included_index, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.last_included_term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.data, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->InstallSnapshot(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.term_out, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __AddServer__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAddServerRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.new_server_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.new_server_addr, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->AddServer(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.success, m);
                            rrr::Serialize_::serialize(__typed_resp__.error_msg, m);
                            rrr::Serialize_::serialize(__typed_resp__.leader_hint, m);
                        });
                    }
                }
            });
            (void)__fiber__;
        }
    }
    // @safe
    void __RemoveServer__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRemoveServerRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.server_id, __req_ar__);
            auto __fiber_req__ = std::move(req);
            auto __fiber_weak_sconn__ = weak_sconn;
            auto __fiber__ = Fiber::create_run([this, __typed_req__ = std::move(__typed_req__), __fiber_req__ = std::move(__fiber_req__), __fiber_weak_sconn__]() mutable {
                auto __typed_result__ = this->RemoveServer(__typed_req__);
                auto sconn_opt = __fiber_weak_sconn__.upgrade();
                if (sconn_opt.is_some()) {
                    auto sconn = sconn_opt.unwrap();
                    if (__typed_result__.is_err()) {
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, __typed_result__.unwrap_err(), rrr::ServerReplyFn{});
                    } else {
                        auto __typed_resp__ = __typed_result__.unwrap();
                        const_cast<rrr::ServerConnection&>(*sconn).reply(*__fiber_req__, 0, [&](rrr::BinaryWriteArchive& m) {
                            rrr::Serialize_::serialize(__typed_resp__.success, m);
                            rrr::Serialize_::serialize(__typed_resp__.error_msg, m);
                            rrr::Serialize_::serialize(__typed_resp__.leader_hint, m);
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
    rrr::Client* __cl__;
public:
    RaftProxy(rrr::Client* cl): __cl__(cl) { }
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
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit VoteTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcVoteResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcVoteResponse, rrr::i32>::Err(__ret__);
            }
            RpcVoteResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.vote_granted, __reply_ar__);
            return rusty::Result<RpcVoteResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<VoteTypedFuture, rrr::i32> async_Vote(const RpcVoteRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::VOTE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.lst_log_idx, __m__);
            rrr::Serialize_::serialize(req.lst_log_term, __m__);
            rrr::Serialize_::serialize(req.site_id, __m__);
            rrr::Serialize_::serialize(req.cur_term, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<VoteTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<VoteTypedFuture, rrr::i32>::Ok(VoteTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcVoteResponse, rrr::i32> Vote(const RpcVoteRequest& req) {
        auto __typed_fu_result__ = this->async_Vote(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVoteResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class VoteDurableTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit VoteDurableTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcVoteDurableResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcVoteDurableResponse, rrr::i32>::Err(__ret__);
            }
            RpcVoteDurableResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcVoteDurableResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<VoteDurableTypedFuture, rrr::i32> async_VoteDurable(const RpcVoteDurableRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::VOTEDURABLE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.term, __m__);
            rrr::Serialize_::serialize(req.voter_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<VoteDurableTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<VoteDurableTypedFuture, rrr::i32>::Ok(VoteDurableTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcVoteDurableResponse, rrr::i32> VoteDurable(const RpcVoteDurableRequest& req) {
        auto __typed_fu_result__ = this->async_VoteDurable(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVoteDurableResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AppendEntriesTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit AppendEntriesTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAppendEntriesResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAppendEntriesResponse, rrr::i32>::Err(__ret__);
            }
            RpcAppendEntriesResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerAckType, __reply_ar__);
            return rusty::Result<RpcAppendEntriesResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntriesTypedFuture, rrr::i32> async_AppendEntries(const RpcAppendEntriesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::APPENDENTRIES, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            rrr::Serialize_::serialize(req.leaderSiteId, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            rrr::Serialize_::serialize(req.leaderCommitIndex, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
            rrr::Serialize_::serialize(req.leaderNextLogTerm, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntriesTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntriesTypedFuture, rrr::i32>::Ok(AppendEntriesTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAppendEntriesResponse, rrr::i32> AppendEntries(const RpcAppendEntriesRequest& req) {
        auto __typed_fu_result__ = this->async_AppendEntries(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntriesResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class EmptyAppendEntriesTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit EmptyAppendEntriesTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Err(__ret__);
            }
            RpcEmptyAppendEntriesResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerAckType, __reply_ar__);
            return rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<EmptyAppendEntriesTypedFuture, rrr::i32> async_EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::EMPTYAPPENDENTRIES, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            rrr::Serialize_::serialize(req.leaderSiteId, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            rrr::Serialize_::serialize(req.leaderCommitIndex, __m__);
            rrr::Serialize_::serialize(req.trigger_election_now, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<EmptyAppendEntriesTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<EmptyAppendEntriesTypedFuture, rrr::i32>::Ok(EmptyAppendEntriesTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32> EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req) {
        auto __typed_fu_result__ = this->async_EmptyAppendEntries(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AppendEntriesDurableTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit AppendEntriesDurableTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32>::Err(__ret__);
            }
            RpcAppendEntriesDurableResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntriesDurableTypedFuture, rrr::i32> async_AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::APPENDENTRIESDURABLE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.term, __m__);
            rrr::Serialize_::serialize(req.follower_id, __m__);
            rrr::Serialize_::serialize(req.lastLogIndex, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntriesDurableTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntriesDurableTypedFuture, rrr::i32>::Ok(AppendEntriesDurableTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32> AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req) {
        auto __typed_fu_result__ = this->async_AppendEntriesDurable(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class TimeoutNowTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit TimeoutNowTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTimeoutNowResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTimeoutNowResponse, rrr::i32>::Err(__ret__);
            }
            RpcTimeoutNowResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.followerTerm, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            return rusty::Result<RpcTimeoutNowResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TimeoutNowTypedFuture, rrr::i32> async_TimeoutNow(const RpcTimeoutNowRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::TIMEOUTNOW, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.leaderTerm, __m__);
            rrr::Serialize_::serialize(req.leaderSiteId, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TimeoutNowTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TimeoutNowTypedFuture, rrr::i32>::Ok(TimeoutNowTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTimeoutNowResponse, rrr::i32> TimeoutNow(const RpcTimeoutNowRequest& req) {
        auto __typed_fu_result__ = this->async_TimeoutNow(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTimeoutNowResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class NotifyRestartTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit NotifyRestartTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcNotifyRestartResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcNotifyRestartResponse, rrr::i32>::Err(__ret__);
            }
            RpcNotifyRestartResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcNotifyRestartResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<NotifyRestartTypedFuture, rrr::i32> async_NotifyRestart(const RpcNotifyRestartRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::NOTIFYRESTART, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.restartedSiteId, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<NotifyRestartTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<NotifyRestartTypedFuture, rrr::i32>::Ok(NotifyRestartTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcNotifyRestartResponse, rrr::i32> NotifyRestart(const RpcNotifyRestartRequest& req) {
        auto __typed_fu_result__ = this->async_NotifyRestart(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcNotifyRestartResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class InstallSnapshotTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit InstallSnapshotTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcInstallSnapshotResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcInstallSnapshotResponse, rrr::i32>::Err(__ret__);
            }
            RpcInstallSnapshotResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.term_out, __reply_ar__);
            return rusty::Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<InstallSnapshotTypedFuture, rrr::i32> async_InstallSnapshot(const RpcInstallSnapshotRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::INSTALLSNAPSHOT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.term, __m__);
            rrr::Serialize_::serialize(req.leader_id, __m__);
            rrr::Serialize_::serialize(req.last_included_index, __m__);
            rrr::Serialize_::serialize(req.last_included_term, __m__);
            rrr::Serialize_::serialize(req.data, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<InstallSnapshotTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<InstallSnapshotTypedFuture, rrr::i32>::Ok(InstallSnapshotTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcInstallSnapshotResponse, rrr::i32> InstallSnapshot(const RpcInstallSnapshotRequest& req) {
        auto __typed_fu_result__ = this->async_InstallSnapshot(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcInstallSnapshotResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AddServerTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit AddServerTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAddServerResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAddServerResponse, rrr::i32>::Err(__ret__);
            }
            RpcAddServerResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.error_msg, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.leader_hint, __reply_ar__);
            return rusty::Result<RpcAddServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AddServerTypedFuture, rrr::i32> async_AddServer(const RpcAddServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::ADDSERVER, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.term, __m__);
            rrr::Serialize_::serialize(req.new_server_id, __m__);
            rrr::Serialize_::serialize(req.new_server_addr, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AddServerTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AddServerTypedFuture, rrr::i32>::Ok(AddServerTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAddServerResponse, rrr::i32> AddServer(const RpcAddServerRequest& req) {
        auto __typed_fu_result__ = this->async_AddServer(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAddServerResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class RemoveServerTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RemoveServerTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcRemoveServerResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRemoveServerResponse, rrr::i32>::Err(__ret__);
            }
            RpcRemoveServerResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.error_msg, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.leader_hint, __reply_ar__);
            return rusty::Result<RpcRemoveServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RemoveServerTypedFuture, rrr::i32> async_RemoveServer(const RpcRemoveServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::REMOVESERVER, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.term, __m__);
            rrr::Serialize_::serialize(req.server_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RemoveServerTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RemoveServerTypedFuture, rrr::i32>::Ok(RemoveServerTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRemoveServerResponse, rrr::i32> RemoveServer(const RpcRemoveServerRequest& req) {
        auto __typed_fu_result__ = this->async_RemoveServer(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRemoveServerResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
    friend inline void serialize(const RpcMsgStringRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.arg, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcMsgStringRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcMsgStringRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.arg, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcMsgStringRequest& o) { deserialize(o, ar); return ar; }

    struct RpcMsgStringResponse {
        std::string ret;
    };
    friend inline void serialize(const RpcMsgStringResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ret, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcMsgStringResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcMsgStringResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ret, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcMsgStringResponse& o) { deserialize(o, ar); return ar; }

    struct RpcMsgMarshallRequest {
        Command arg;
    };
    friend inline void serialize(const RpcMsgMarshallRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.arg, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcMsgMarshallRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcMsgMarshallRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.arg, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcMsgMarshallRequest& o) { deserialize(o, ar); return ar; }

    struct RpcMsgMarshallResponse {
        Command ret;
    };
    friend inline void serialize(const RpcMsgMarshallResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ret, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcMsgMarshallResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcMsgMarshallResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ret, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcMsgMarshallResponse& o) { deserialize(o, ar); return ar; }

    struct RpcReElectRequest {
    };
    friend inline void serialize(const RpcReElectRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcReElectRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReElectRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcReElectRequest& o) { deserialize(o, ar); return ar; }

    struct RpcReElectResponse {
        bool_t success;
    };
    friend inline void serialize(const RpcReElectResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.success, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcReElectResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReElectResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.success, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcReElectResponse& o) { deserialize(o, ar); return ar; }

    struct RpcDispatchRequest {
        rrr::i64 tid;
        DepId dep_id;
        Command cmd;
    };
    friend inline void serialize(const RpcDispatchRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.tid, ar);
        rrr::Serialize_::serialize(o.dep_id, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcDispatchRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDispatchRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.tid, ar);
        rrr::Deserialize_::deserialize(o.dep_id, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcDispatchRequest& o) { deserialize(o, ar); return ar; }

    struct RpcDispatchResponse {
        rrr::i32 res;
        TxnOutput output;
        uint64_t coro_id;
        Command view_data;
    };
    friend inline void serialize(const RpcDispatchResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.output, ar);
        rrr::Serialize_::serialize(o.coro_id, ar);
        rrr::Serialize_::serialize(o.view_data, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcDispatchResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDispatchResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.output, ar);
        rrr::Deserialize_::deserialize(o.coro_id, ar);
        rrr::Deserialize_::deserialize(o.view_data, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcDispatchResponse& o) { deserialize(o, ar); return ar; }

    struct RpcPrepareRequest {
        rrr::i64 tid;
        std::vector<rrr::i32> sids;
        DepId dep_id;
    };
    friend inline void serialize(const RpcPrepareRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.tid, ar);
        rrr::Serialize_::serialize(o.sids, ar);
        rrr::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcPrepareRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.tid, ar);
        rrr::Deserialize_::deserialize(o.sids, ar);
        rrr::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcPrepareRequest& o) { deserialize(o, ar); return ar; }

    struct RpcPrepareResponse {
        rrr::i32 res;
        bool_t slow;
        uint64_t coro_id;
    };
    friend inline void serialize(const RpcPrepareResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.slow, ar);
        rrr::Serialize_::serialize(o.coro_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcPrepareResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.slow, ar);
        rrr::Deserialize_::deserialize(o.coro_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcPrepareResponse& o) { deserialize(o, ar); return ar; }

    struct RpcCommitRequest {
        rrr::i64 tid;
        DepId dep_id;
    };
    friend inline void serialize(const RpcCommitRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.tid, ar);
        rrr::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcCommitRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcCommitRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.tid, ar);
        rrr::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcCommitRequest& o) { deserialize(o, ar); return ar; }

    struct RpcCommitResponse {
        rrr::i32 res;
        bool_t slow;
        uint64_t coro_id;
        Command view_data;
    };
    friend inline void serialize(const RpcCommitResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.slow, ar);
        rrr::Serialize_::serialize(o.coro_id, ar);
        rrr::Serialize_::serialize(o.view_data, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcCommitResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.slow, ar);
        rrr::Deserialize_::deserialize(o.coro_id, ar);
        rrr::Deserialize_::deserialize(o.view_data, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcCommitResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAbortRequest {
        rrr::i64 tid;
        DepId dep_id;
    };
    friend inline void serialize(const RpcAbortRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.tid, ar);
        rrr::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAbortRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAbortRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.tid, ar);
        rrr::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAbortRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAbortResponse {
        rrr::i32 res;
        bool_t slow;
        uint64_t coro_id;
        Command view_data;
    };
    friend inline void serialize(const RpcAbortResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.slow, ar);
        rrr::Serialize_::serialize(o.coro_id, ar);
        rrr::Serialize_::serialize(o.view_data, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAbortResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAbortResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.slow, ar);
        rrr::Deserialize_::deserialize(o.coro_id, ar);
        rrr::Deserialize_::deserialize(o.view_data, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAbortResponse& o) { deserialize(o, ar); return ar; }

    struct RpcEarlyAbortRequest {
        rrr::i64 tid;
    };
    friend inline void serialize(const RpcEarlyAbortRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.tid, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcEarlyAbortRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEarlyAbortRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.tid, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcEarlyAbortRequest& o) { deserialize(o, ar); return ar; }

    struct RpcEarlyAbortResponse {
        rrr::i32 res;
    };
    friend inline void serialize(const RpcEarlyAbortResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcEarlyAbortResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcEarlyAbortResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcEarlyAbortResponse& o) { deserialize(o, ar); return ar; }

    struct RpcUpgradeEpochRequest {
        uint32_t curr_epoch;
    };
    friend inline void serialize(const RpcUpgradeEpochRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.curr_epoch, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcUpgradeEpochRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcUpgradeEpochRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.curr_epoch, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcUpgradeEpochRequest& o) { deserialize(o, ar); return ar; }

    struct RpcUpgradeEpochResponse {
        int32_t res;
    };
    friend inline void serialize(const RpcUpgradeEpochResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcUpgradeEpochResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcUpgradeEpochResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcUpgradeEpochResponse& o) { deserialize(o, ar); return ar; }

    struct RpcTruncateEpochRequest {
        uint32_t old_epoch;
    };
    friend inline void serialize(const RpcTruncateEpochRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.old_epoch, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTruncateEpochRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTruncateEpochRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.old_epoch, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTruncateEpochRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTruncateEpochResponse {
    };
    friend inline void serialize(const RpcTruncateEpochResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTruncateEpochResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTruncateEpochResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTruncateEpochResponse& o) { deserialize(o, ar); return ar; }

    struct RpcIsLeaderRequest {
        locid_t cur_pause;
    };
    friend inline void serialize(const RpcIsLeaderRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cur_pause, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcIsLeaderRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcIsLeaderRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cur_pause, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcIsLeaderRequest& o) { deserialize(o, ar); return ar; }

    struct RpcIsLeaderResponse {
        bool_t is_leader;
    };
    friend inline void serialize(const RpcIsLeaderResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.is_leader, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcIsLeaderResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcIsLeaderResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.is_leader, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcIsLeaderResponse& o) { deserialize(o, ar); return ar; }

    struct RpcSimpleCmdRequest {
        SimpleCommand cmd;
    };
    friend inline void serialize(const RpcSimpleCmdRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSimpleCmdRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSimpleCmdRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSimpleCmdRequest& o) { deserialize(o, ar); return ar; }

    struct RpcSimpleCmdResponse {
        rrr::i32 res;
    };
    friend inline void serialize(const RpcSimpleCmdResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSimpleCmdResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSimpleCmdResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSimpleCmdResponse& o) { deserialize(o, ar); return ar; }

    struct RpcFailoverPauseSocketOutRequest {
    };
    friend inline void serialize(const RpcFailoverPauseSocketOutRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFailoverPauseSocketOutRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFailoverPauseSocketOutRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFailoverPauseSocketOutRequest& o) { deserialize(o, ar); return ar; }

    struct RpcFailoverPauseSocketOutResponse {
        rrr::i32 res;
    };
    friend inline void serialize(const RpcFailoverPauseSocketOutResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFailoverPauseSocketOutResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFailoverPauseSocketOutResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFailoverPauseSocketOutResponse& o) { deserialize(o, ar); return ar; }

    struct RpcFailoverResumeSocketOutRequest {
    };
    friend inline void serialize(const RpcFailoverResumeSocketOutRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFailoverResumeSocketOutRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFailoverResumeSocketOutRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFailoverResumeSocketOutRequest& o) { deserialize(o, ar); return ar; }

    struct RpcFailoverResumeSocketOutResponse {
        rrr::i32 res;
    };
    friend inline void serialize(const RpcFailoverResumeSocketOutResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFailoverResumeSocketOutResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFailoverResumeSocketOutResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFailoverResumeSocketOutResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackBeginRecoveryRequest {
        Command old_view;
        Command new_view;
        epoch_t new_view_id;
    };
    friend inline void serialize(const RpcJetpackBeginRecoveryRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.old_view, ar);
        rrr::Serialize_::serialize(o.new_view, ar);
        rrr::Serialize_::serialize(o.new_view_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackBeginRecoveryRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackBeginRecoveryRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.old_view, ar);
        rrr::Deserialize_::deserialize(o.new_view, ar);
        rrr::Deserialize_::deserialize(o.new_view_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackBeginRecoveryRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackBeginRecoveryResponse {
    };
    friend inline void serialize(const RpcJetpackBeginRecoveryResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackBeginRecoveryResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackBeginRecoveryResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackBeginRecoveryResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullIdSetRequest {
        epoch_t jepoch;
        epoch_t oepoch;
    };
    friend inline void serialize(const RpcJetpackPullIdSetRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.jepoch, ar);
        rrr::Serialize_::serialize(o.oepoch, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackPullIdSetRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullIdSetRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.jepoch, ar);
        rrr::Deserialize_::deserialize(o.oepoch, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackPullIdSetRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullIdSetResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        Command reply_old_view;
        Command reply_new_view;
        Command id_set;
    };
    friend inline void serialize(const RpcJetpackPullIdSetResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ok, ar);
        rrr::Serialize_::serialize(o.reply_jepoch, ar);
        rrr::Serialize_::serialize(o.reply_oepoch, ar);
        rrr::Serialize_::serialize(o.reply_old_view, ar);
        rrr::Serialize_::serialize(o.reply_new_view, ar);
        rrr::Serialize_::serialize(o.id_set, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackPullIdSetResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullIdSetResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ok, ar);
        rrr::Deserialize_::deserialize(o.reply_jepoch, ar);
        rrr::Deserialize_::deserialize(o.reply_oepoch, ar);
        rrr::Deserialize_::deserialize(o.reply_old_view, ar);
        rrr::Deserialize_::deserialize(o.reply_new_view, ar);
        rrr::Deserialize_::deserialize(o.id_set, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackPullIdSetResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullCmdRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        Command key_batch;
    };
    friend inline void serialize(const RpcJetpackPullCmdRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.jepoch, ar);
        rrr::Serialize_::serialize(o.oepoch, ar);
        rrr::Serialize_::serialize(o.key_batch, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackPullCmdRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullCmdRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.jepoch, ar);
        rrr::Deserialize_::deserialize(o.oepoch, ar);
        rrr::Deserialize_::deserialize(o.key_batch, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackPullCmdRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullCmdResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        Command reply_old_view;
        Command reply_new_view;
        Command cmd_batch;
    };
    friend inline void serialize(const RpcJetpackPullCmdResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ok, ar);
        rrr::Serialize_::serialize(o.reply_jepoch, ar);
        rrr::Serialize_::serialize(o.reply_oepoch, ar);
        rrr::Serialize_::serialize(o.reply_old_view, ar);
        rrr::Serialize_::serialize(o.reply_new_view, ar);
        rrr::Serialize_::serialize(o.cmd_batch, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackPullCmdResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullCmdResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ok, ar);
        rrr::Deserialize_::deserialize(o.reply_jepoch, ar);
        rrr::Deserialize_::deserialize(o.reply_oepoch, ar);
        rrr::Deserialize_::deserialize(o.reply_old_view, ar);
        rrr::Deserialize_::deserialize(o.reply_new_view, ar);
        rrr::Deserialize_::deserialize(o.cmd_batch, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackPullCmdResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackRecordCmdRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        int32_t sid;
        int32_t rid;
        Command cmd_batch;
    };
    friend inline void serialize(const RpcJetpackRecordCmdRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.jepoch, ar);
        rrr::Serialize_::serialize(o.oepoch, ar);
        rrr::Serialize_::serialize(o.sid, ar);
        rrr::Serialize_::serialize(o.rid, ar);
        rrr::Serialize_::serialize(o.cmd_batch, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackRecordCmdRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackRecordCmdRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.jepoch, ar);
        rrr::Deserialize_::deserialize(o.oepoch, ar);
        rrr::Deserialize_::deserialize(o.sid, ar);
        rrr::Deserialize_::deserialize(o.rid, ar);
        rrr::Deserialize_::deserialize(o.cmd_batch, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackRecordCmdRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackRecordCmdResponse {
    };
    friend inline void serialize(const RpcJetpackRecordCmdResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackRecordCmdResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackRecordCmdResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackRecordCmdResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPrepareRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        ballot_t max_seen_ballot;
    };
    friend inline void serialize(const RpcJetpackPrepareRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.jepoch, ar);
        rrr::Serialize_::serialize(o.oepoch, ar);
        rrr::Serialize_::serialize(o.max_seen_ballot, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackPrepareRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPrepareRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.jepoch, ar);
        rrr::Deserialize_::deserialize(o.oepoch, ar);
        rrr::Deserialize_::deserialize(o.max_seen_ballot, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackPrepareRequest& o) { deserialize(o, ar); return ar; }

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
    friend inline void serialize(const RpcJetpackPrepareResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ok, ar);
        rrr::Serialize_::serialize(o.reply_jepoch, ar);
        rrr::Serialize_::serialize(o.reply_oepoch, ar);
        rrr::Serialize_::serialize(o.reply_old_view, ar);
        rrr::Serialize_::serialize(o.reply_new_view, ar);
        rrr::Serialize_::serialize(o.reply_max_seen_ballot, ar);
        rrr::Serialize_::serialize(o.accepted_ballot, ar);
        rrr::Serialize_::serialize(o.replied_sid, ar);
        rrr::Serialize_::serialize(o.replied_set_size, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackPrepareResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPrepareResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ok, ar);
        rrr::Deserialize_::deserialize(o.reply_jepoch, ar);
        rrr::Deserialize_::deserialize(o.reply_oepoch, ar);
        rrr::Deserialize_::deserialize(o.reply_old_view, ar);
        rrr::Deserialize_::deserialize(o.reply_new_view, ar);
        rrr::Deserialize_::deserialize(o.reply_max_seen_ballot, ar);
        rrr::Deserialize_::deserialize(o.accepted_ballot, ar);
        rrr::Deserialize_::deserialize(o.replied_sid, ar);
        rrr::Deserialize_::deserialize(o.replied_set_size, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackPrepareResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackAcceptRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        ballot_t max_seen_ballot;
        int32_t sid;
        int32_t set_size;
    };
    friend inline void serialize(const RpcJetpackAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.jepoch, ar);
        rrr::Serialize_::serialize(o.oepoch, ar);
        rrr::Serialize_::serialize(o.max_seen_ballot, ar);
        rrr::Serialize_::serialize(o.sid, ar);
        rrr::Serialize_::serialize(o.set_size, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.jepoch, ar);
        rrr::Deserialize_::deserialize(o.oepoch, ar);
        rrr::Deserialize_::deserialize(o.max_seen_ballot, ar);
        rrr::Deserialize_::deserialize(o.sid, ar);
        rrr::Deserialize_::deserialize(o.set_size, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackAcceptResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        Command reply_old_view;
        Command reply_new_view;
        ballot_t reply_max_seen_ballot;
    };
    friend inline void serialize(const RpcJetpackAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ok, ar);
        rrr::Serialize_::serialize(o.reply_jepoch, ar);
        rrr::Serialize_::serialize(o.reply_oepoch, ar);
        rrr::Serialize_::serialize(o.reply_old_view, ar);
        rrr::Serialize_::serialize(o.reply_new_view, ar);
        rrr::Serialize_::serialize(o.reply_max_seen_ballot, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackAcceptResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ok, ar);
        rrr::Deserialize_::deserialize(o.reply_jepoch, ar);
        rrr::Deserialize_::deserialize(o.reply_oepoch, ar);
        rrr::Deserialize_::deserialize(o.reply_old_view, ar);
        rrr::Deserialize_::deserialize(o.reply_new_view, ar);
        rrr::Deserialize_::deserialize(o.reply_max_seen_ballot, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackCommitRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        int32_t sid;
        int32_t set_size;
    };
    friend inline void serialize(const RpcJetpackCommitRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.jepoch, ar);
        rrr::Serialize_::serialize(o.oepoch, ar);
        rrr::Serialize_::serialize(o.sid, ar);
        rrr::Serialize_::serialize(o.set_size, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackCommitRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackCommitRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.jepoch, ar);
        rrr::Deserialize_::deserialize(o.oepoch, ar);
        rrr::Deserialize_::deserialize(o.sid, ar);
        rrr::Deserialize_::deserialize(o.set_size, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackCommitRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackCommitResponse {
    };
    friend inline void serialize(const RpcJetpackCommitResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackCommitResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackCommitResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullRecSetInsRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        int32_t sid;
        int32_t rid;
    };
    friend inline void serialize(const RpcJetpackPullRecSetInsRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.jepoch, ar);
        rrr::Serialize_::serialize(o.oepoch, ar);
        rrr::Serialize_::serialize(o.sid, ar);
        rrr::Serialize_::serialize(o.rid, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackPullRecSetInsRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullRecSetInsRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.jepoch, ar);
        rrr::Deserialize_::deserialize(o.oepoch, ar);
        rrr::Deserialize_::deserialize(o.sid, ar);
        rrr::Deserialize_::deserialize(o.rid, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackPullRecSetInsRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackPullRecSetInsResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        Command reply_old_view;
        Command reply_new_view;
        Command cmd;
    };
    friend inline void serialize(const RpcJetpackPullRecSetInsResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ok, ar);
        rrr::Serialize_::serialize(o.reply_jepoch, ar);
        rrr::Serialize_::serialize(o.reply_oepoch, ar);
        rrr::Serialize_::serialize(o.reply_old_view, ar);
        rrr::Serialize_::serialize(o.reply_new_view, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackPullRecSetInsResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackPullRecSetInsResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ok, ar);
        rrr::Deserialize_::deserialize(o.reply_jepoch, ar);
        rrr::Deserialize_::deserialize(o.reply_oepoch, ar);
        rrr::Deserialize_::deserialize(o.reply_old_view, ar);
        rrr::Deserialize_::deserialize(o.reply_new_view, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackPullRecSetInsResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackFinishRecoveryRequest {
        epoch_t oepoch;
    };
    friend inline void serialize(const RpcJetpackFinishRecoveryRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.oepoch, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackFinishRecoveryRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackFinishRecoveryRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.oepoch, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackFinishRecoveryRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJetpackFinishRecoveryResponse {
    };
    friend inline void serialize(const RpcJetpackFinishRecoveryResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJetpackFinishRecoveryResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJetpackFinishRecoveryResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJetpackFinishRecoveryResponse& o) { deserialize(o, ar); return ar; }

    enum {
        MSGSTRING = 0x4075aa22,
        MSGMARSHALL = 0x6602ec53,
        REELECT = 0x2bcd0e52,
        DISPATCH = 0x63b62f50,
        PREPARE = 0x5ef5071b,
        COMMIT = 0x1b3bdc7d,
        ABORT = 0x4d934a81,
        EARLYABORT = 0x4a31f986,
        UPGRADEEPOCH = 0x63d5a0e1,
        TRUNCATEEPOCH = 0x2c8a9f38,
        ISLEADER = 0x4b803f58,
        SIMPLECMD = 0x40161224,
        FAILOVERPAUSESOCKETOUT = 0x566789af,
        FAILOVERRESUMESOCKETOUT = 0x61f54de5,
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
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
        if ((ret = svr.reg_rpc(SIMPLECMD, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FAILOVERPAUSESOCKETOUT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FAILOVERRESUMESOCKETOUT, svc_index)) != 0) {
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
        svr.unreg(DISPATCH);
        svr.unreg(PREPARE);
        svr.unreg(COMMIT);
        svr.unreg(ABORT);
        svr.unreg(EARLYABORT);
        svr.unreg(UPGRADEEPOCH);
        svr.unreg(TRUNCATEEPOCH);
        svr.unreg(ISLEADER);
        svr.unreg(SIMPLECMD);
        svr.unreg(FAILOVERPAUSESOCKETOUT);
        svr.unreg(FAILOVERRESUMESOCKETOUT);
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case MSGSTRING: __MsgString__wrapper__(std::move(req), weak_sconn); break;
        case MSGMARSHALL: __MsgMarshall__wrapper__(std::move(req), weak_sconn); break;
        case REELECT: __ReElect__wrapper__(std::move(req), weak_sconn); break;
        case DISPATCH: __Dispatch__wrapper__(std::move(req), weak_sconn); break;
        case PREPARE: __Prepare__wrapper__(std::move(req), weak_sconn); break;
        case COMMIT: __Commit__wrapper__(std::move(req), weak_sconn); break;
        case ABORT: __Abort__wrapper__(std::move(req), weak_sconn); break;
        case EARLYABORT: __EarlyAbort__wrapper__(std::move(req), weak_sconn); break;
        case UPGRADEEPOCH: __UpgradeEpoch__wrapper__(std::move(req), weak_sconn); break;
        case TRUNCATEEPOCH: __TruncateEpoch__wrapper__(std::move(req), weak_sconn); break;
        case ISLEADER: __IsLeader__wrapper__(std::move(req), weak_sconn); break;
        case SIMPLECMD: __SimpleCmd__wrapper__(std::move(req), weak_sconn); break;
        case FAILOVERPAUSESOCKETOUT: __FailoverPauseSocketOut__wrapper__(std::move(req), weak_sconn); break;
        case FAILOVERRESUMESOCKETOUT: __FailoverResumeSocketOut__wrapper__(std::move(req), weak_sconn); break;
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
    virtual void MsgString(const RpcMsgStringRequest& req, RpcMsgStringResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void MsgMarshall(const RpcMsgMarshallRequest& req, RpcMsgMarshallResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void ReElect(const RpcReElectRequest& req, RpcReElectResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Dispatch(const RpcDispatchRequest& req, RpcDispatchResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Prepare(const RpcPrepareRequest& req, RpcPrepareResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Commit(const RpcCommitRequest& req, RpcCommitResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Abort(const RpcAbortRequest& req, RpcAbortResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void EarlyAbort(const RpcEarlyAbortRequest& req, RpcEarlyAbortResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void UpgradeEpoch(const RpcUpgradeEpochRequest& req, RpcUpgradeEpochResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void TruncateEpoch(const RpcTruncateEpochRequest& req, RpcTruncateEpochResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void IsLeader(const RpcIsLeaderRequest& req, RpcIsLeaderResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void SimpleCmd(const RpcSimpleCmdRequest& req, RpcSimpleCmdResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void FailoverPauseSocketOut(const RpcFailoverPauseSocketOutRequest& req, RpcFailoverPauseSocketOutResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest& req, RpcFailoverResumeSocketOutResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackBeginRecovery(const RpcJetpackBeginRecoveryRequest& req, RpcJetpackBeginRecoveryResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackPullIdSet(const RpcJetpackPullIdSetRequest& req, RpcJetpackPullIdSetResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackPullCmd(const RpcJetpackPullCmdRequest& req, RpcJetpackPullCmdResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackRecordCmd(const RpcJetpackRecordCmdRequest& req, RpcJetpackRecordCmdResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackPrepare(const RpcJetpackPrepareRequest& req, RpcJetpackPrepareResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackAccept(const RpcJetpackAcceptRequest& req, RpcJetpackAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackCommit(const RpcJetpackCommitRequest& req, RpcJetpackCommitResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackPullRecSetIns(const RpcJetpackPullRecSetInsRequest& req, RpcJetpackPullRecSetInsResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JetpackFinishRecovery(const RpcJetpackFinishRecoveryRequest& req, RpcJetpackFinishRecoveryResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __MsgString__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcMsgStringRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.arg, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcMsgStringResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ret, m);
                },
                []() {});
            this->MsgString(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __MsgMarshall__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcMsgMarshallRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.arg, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcMsgMarshallResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ret, m);
                },
                []() {});
            this->MsgMarshall(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __ReElect__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcReElectRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcReElectResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->success, m);
                },
                []() {});
            this->ReElect(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Dispatch__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDispatchRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.tid, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcDispatchResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->output, m);
                    rrr::Serialize_::serialize(__typed_resp__->coro_id, m);
                    rrr::Serialize_::serialize(__typed_resp__->view_data, m);
                },
                []() {});
            this->Dispatch(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Prepare__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPrepareRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.tid, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.sids, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcPrepareResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->slow, m);
                    rrr::Serialize_::serialize(__typed_resp__->coro_id, m);
                },
                []() {});
            this->Prepare(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Commit__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcCommitRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.tid, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcCommitResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->slow, m);
                    rrr::Serialize_::serialize(__typed_resp__->coro_id, m);
                    rrr::Serialize_::serialize(__typed_resp__->view_data, m);
                },
                []() {});
            this->Commit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Abort__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAbortRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.tid, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcAbortResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->slow, m);
                    rrr::Serialize_::serialize(__typed_resp__->coro_id, m);
                    rrr::Serialize_::serialize(__typed_resp__->view_data, m);
                },
                []() {});
            this->Abort(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __EarlyAbort__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcEarlyAbortRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.tid, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcEarlyAbortResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->EarlyAbort(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __UpgradeEpoch__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcUpgradeEpochRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.curr_epoch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcUpgradeEpochResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->UpgradeEpoch(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __TruncateEpoch__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTruncateEpochRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.old_epoch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcTruncateEpochResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->TruncateEpoch(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __IsLeader__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcIsLeaderRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.cur_pause, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcIsLeaderResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->is_leader, m);
                },
                []() {});
            this->IsLeader(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SimpleCmd__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSimpleCmdRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcSimpleCmdResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->SimpleCmd(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __FailoverPauseSocketOut__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFailoverPauseSocketOutRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcFailoverPauseSocketOutResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->FailoverPauseSocketOut(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __FailoverResumeSocketOut__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFailoverResumeSocketOutRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcFailoverResumeSocketOutResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->FailoverResumeSocketOut(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackBeginRecovery__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackBeginRecoveryRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.old_view, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.new_view, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.new_view_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackBeginRecoveryResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->JetpackBeginRecovery(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackPullIdSet__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackPullIdSetRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackPullIdSetResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ok, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_jepoch, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_oepoch, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_old_view, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_new_view, m);
                    rrr::Serialize_::serialize(__typed_resp__->id_set, m);
                },
                []() {});
            this->JetpackPullIdSet(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackPullCmd__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackPullCmdRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.key_batch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackPullCmdResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ok, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_jepoch, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_oepoch, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_old_view, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_new_view, m);
                    rrr::Serialize_::serialize(__typed_resp__->cmd_batch, m);
                },
                []() {});
            this->JetpackPullCmd(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackRecordCmd__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackRecordCmdRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.sid, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rid, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd_batch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackRecordCmdResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->JetpackRecordCmd(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackPrepare__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackPrepareRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.max_seen_ballot, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackPrepareResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ok, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_jepoch, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_oepoch, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_old_view, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_new_view, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_max_seen_ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->accepted_ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->replied_sid, m);
                    rrr::Serialize_::serialize(__typed_resp__->replied_set_size, m);
                },
                []() {});
            this->JetpackPrepare(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackAcceptRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.max_seen_ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.sid, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.set_size, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ok, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_jepoch, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_oepoch, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_old_view, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_new_view, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_max_seen_ballot, m);
                },
                []() {});
            this->JetpackAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackCommit__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackCommitRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.sid, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.set_size, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackCommitResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->JetpackCommit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackPullRecSetIns__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackPullRecSetInsRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.jepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.sid, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rid, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackPullRecSetInsResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ok, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_jepoch, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_oepoch, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_old_view, m);
                    rrr::Serialize_::serialize(__typed_resp__->reply_new_view, m);
                    rrr::Serialize_::serialize(__typed_resp__->cmd, m);
                },
                []() {});
            this->JetpackPullRecSetIns(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackFinishRecovery__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackFinishRecoveryRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.oepoch, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJetpackFinishRecoveryResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->JetpackFinishRecovery(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ClassicProxy {
protected:
    rrr::Client* __cl__;
public:
    ClassicProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcMsgStringRequest = ClassicService::RpcMsgStringRequest;
    using RpcMsgStringResponse = ClassicService::RpcMsgStringResponse;
    using RpcMsgMarshallRequest = ClassicService::RpcMsgMarshallRequest;
    using RpcMsgMarshallResponse = ClassicService::RpcMsgMarshallResponse;
    using RpcReElectRequest = ClassicService::RpcReElectRequest;
    using RpcReElectResponse = ClassicService::RpcReElectResponse;
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
    using RpcSimpleCmdRequest = ClassicService::RpcSimpleCmdRequest;
    using RpcSimpleCmdResponse = ClassicService::RpcSimpleCmdResponse;
    using RpcFailoverPauseSocketOutRequest = ClassicService::RpcFailoverPauseSocketOutRequest;
    using RpcFailoverPauseSocketOutResponse = ClassicService::RpcFailoverPauseSocketOutResponse;
    using RpcFailoverResumeSocketOutRequest = ClassicService::RpcFailoverResumeSocketOutRequest;
    using RpcFailoverResumeSocketOutResponse = ClassicService::RpcFailoverResumeSocketOutResponse;
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
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit MsgStringTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcMsgStringResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcMsgStringResponse, rrr::i32>::Err(__ret__);
            }
            RpcMsgStringResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ret, __reply_ar__);
            return rusty::Result<RpcMsgStringResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<MsgStringTypedFuture, rrr::i32> async_MsgString(const RpcMsgStringRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::MSGSTRING, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.arg, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<MsgStringTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<MsgStringTypedFuture, rrr::i32>::Ok(MsgStringTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcMsgStringResponse, rrr::i32> MsgString(const RpcMsgStringRequest& req) {
        auto __typed_fu_result__ = this->async_MsgString(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcMsgStringResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class MsgMarshallTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit MsgMarshallTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcMsgMarshallResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcMsgMarshallResponse, rrr::i32>::Err(__ret__);
            }
            RpcMsgMarshallResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ret, __reply_ar__);
            return rusty::Result<RpcMsgMarshallResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<MsgMarshallTypedFuture, rrr::i32> async_MsgMarshall(const RpcMsgMarshallRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::MSGMARSHALL, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.arg, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<MsgMarshallTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<MsgMarshallTypedFuture, rrr::i32>::Ok(MsgMarshallTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcMsgMarshallResponse, rrr::i32> MsgMarshall(const RpcMsgMarshallRequest& req) {
        auto __typed_fu_result__ = this->async_MsgMarshall(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcMsgMarshallResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class ReElectTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit ReElectTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcReElectResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcReElectResponse, rrr::i32>::Err(__ret__);
            }
            RpcReElectResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            return rusty::Result<RpcReElectResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ReElectTypedFuture, rrr::i32> async_ReElect(const RpcReElectRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::REELECT, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<ReElectTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<ReElectTypedFuture, rrr::i32>::Ok(ReElectTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcReElectResponse, rrr::i32> ReElect(const RpcReElectRequest& req) {
        auto __typed_fu_result__ = this->async_ReElect(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcReElectResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class DispatchTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit DispatchTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcDispatchResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcDispatchResponse, rrr::i32>::Err(__ret__);
            }
            RpcDispatchResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.view_data, __reply_ar__);
            return rusty::Result<RpcDispatchResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<DispatchTypedFuture, rrr::i32> async_Dispatch(const RpcDispatchRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::DISPATCH, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.tid, __m__);
            rrr::Serialize_::serialize(req.dep_id, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<DispatchTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<DispatchTypedFuture, rrr::i32>::Ok(DispatchTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcDispatchResponse, rrr::i32> Dispatch(const RpcDispatchRequest& req) {
        auto __typed_fu_result__ = this->async_Dispatch(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDispatchResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class PrepareTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit PrepareTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcPrepareResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcPrepareResponse, rrr::i32>::Err(__ret__);
            }
            RpcPrepareResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.slow, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<PrepareTypedFuture, rrr::i32> async_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::PREPARE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.tid, __m__);
            rrr::Serialize_::serialize(req.sids, __m__);
            rrr::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<PrepareTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<PrepareTypedFuture, rrr::i32>::Ok(PrepareTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcPrepareResponse, rrr::i32> Prepare(const RpcPrepareRequest& req) {
        auto __typed_fu_result__ = this->async_Prepare(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class CommitTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit CommitTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcCommitResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcCommitResponse, rrr::i32>::Err(__ret__);
            }
            RpcCommitResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.slow, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.view_data, __reply_ar__);
            return rusty::Result<RpcCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<CommitTypedFuture, rrr::i32> async_Commit(const RpcCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::COMMIT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.tid, __m__);
            rrr::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<CommitTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<CommitTypedFuture, rrr::i32>::Ok(CommitTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcCommitResponse, rrr::i32> Commit(const RpcCommitRequest& req) {
        auto __typed_fu_result__ = this->async_Commit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcCommitResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class AbortTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit AbortTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcAbortResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAbortResponse, rrr::i32>::Err(__ret__);
            }
            RpcAbortResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.slow, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.view_data, __reply_ar__);
            return rusty::Result<RpcAbortResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AbortTypedFuture, rrr::i32> async_Abort(const RpcAbortRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::ABORT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.tid, __m__);
            rrr::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AbortTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AbortTypedFuture, rrr::i32>::Ok(AbortTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAbortResponse, rrr::i32> Abort(const RpcAbortRequest& req) {
        auto __typed_fu_result__ = this->async_Abort(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAbortResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class EarlyAbortTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit EarlyAbortTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcEarlyAbortResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcEarlyAbortResponse, rrr::i32>::Err(__ret__);
            }
            RpcEarlyAbortResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcEarlyAbortResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<EarlyAbortTypedFuture, rrr::i32> async_EarlyAbort(const RpcEarlyAbortRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::EARLYABORT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.tid, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<EarlyAbortTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<EarlyAbortTypedFuture, rrr::i32>::Ok(EarlyAbortTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcEarlyAbortResponse, rrr::i32> EarlyAbort(const RpcEarlyAbortRequest& req) {
        auto __typed_fu_result__ = this->async_EarlyAbort(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcEarlyAbortResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class UpgradeEpochTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit UpgradeEpochTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcUpgradeEpochResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcUpgradeEpochResponse, rrr::i32>::Err(__ret__);
            }
            RpcUpgradeEpochResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcUpgradeEpochResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<UpgradeEpochTypedFuture, rrr::i32> async_UpgradeEpoch(const RpcUpgradeEpochRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::UPGRADEEPOCH, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.curr_epoch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<UpgradeEpochTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<UpgradeEpochTypedFuture, rrr::i32>::Ok(UpgradeEpochTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcUpgradeEpochResponse, rrr::i32> UpgradeEpoch(const RpcUpgradeEpochRequest& req) {
        auto __typed_fu_result__ = this->async_UpgradeEpoch(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcUpgradeEpochResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class TruncateEpochTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit TruncateEpochTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcTruncateEpochResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTruncateEpochResponse, rrr::i32>::Err(__ret__);
            }
            RpcTruncateEpochResponse __typed_resp__;
            return rusty::Result<RpcTruncateEpochResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TruncateEpochTypedFuture, rrr::i32> async_TruncateEpoch(const RpcTruncateEpochRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::TRUNCATEEPOCH, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.old_epoch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TruncateEpochTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TruncateEpochTypedFuture, rrr::i32>::Ok(TruncateEpochTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTruncateEpochResponse, rrr::i32> TruncateEpoch(const RpcTruncateEpochRequest& req) {
        auto __typed_fu_result__ = this->async_TruncateEpoch(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTruncateEpochResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class IsLeaderTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit IsLeaderTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcIsLeaderResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcIsLeaderResponse, rrr::i32>::Err(__ret__);
            }
            RpcIsLeaderResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.is_leader, __reply_ar__);
            return rusty::Result<RpcIsLeaderResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<IsLeaderTypedFuture, rrr::i32> async_IsLeader(const RpcIsLeaderRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::ISLEADER, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cur_pause, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<IsLeaderTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<IsLeaderTypedFuture, rrr::i32>::Ok(IsLeaderTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcIsLeaderResponse, rrr::i32> IsLeader(const RpcIsLeaderRequest& req) {
        auto __typed_fu_result__ = this->async_IsLeader(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcIsLeaderResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class SimpleCmdTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit SimpleCmdTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcSimpleCmdResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSimpleCmdResponse, rrr::i32>::Err(__ret__);
            }
            RpcSimpleCmdResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcSimpleCmdResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SimpleCmdTypedFuture, rrr::i32> async_SimpleCmd(const RpcSimpleCmdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::SIMPLECMD, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SimpleCmdTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SimpleCmdTypedFuture, rrr::i32>::Ok(SimpleCmdTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSimpleCmdResponse, rrr::i32> SimpleCmd(const RpcSimpleCmdRequest& req) {
        auto __typed_fu_result__ = this->async_SimpleCmd(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSimpleCmdResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class FailoverPauseSocketOutTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit FailoverPauseSocketOutTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcFailoverPauseSocketOutResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFailoverPauseSocketOutResponse, rrr::i32>::Err(__ret__);
            }
            RpcFailoverPauseSocketOutResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcFailoverPauseSocketOutResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<FailoverPauseSocketOutTypedFuture, rrr::i32> async_FailoverPauseSocketOut(const RpcFailoverPauseSocketOutRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::FAILOVERPAUSESOCKETOUT, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<FailoverPauseSocketOutTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<FailoverPauseSocketOutTypedFuture, rrr::i32>::Ok(FailoverPauseSocketOutTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcFailoverPauseSocketOutResponse, rrr::i32> FailoverPauseSocketOut(const RpcFailoverPauseSocketOutRequest& req) {
        auto __typed_fu_result__ = this->async_FailoverPauseSocketOut(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFailoverPauseSocketOutResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class FailoverResumeSocketOutTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit FailoverResumeSocketOutTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcFailoverResumeSocketOutResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFailoverResumeSocketOutResponse, rrr::i32>::Err(__ret__);
            }
            RpcFailoverResumeSocketOutResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcFailoverResumeSocketOutResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<FailoverResumeSocketOutTypedFuture, rrr::i32> async_FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::FAILOVERRESUMESOCKETOUT, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<FailoverResumeSocketOutTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<FailoverResumeSocketOutTypedFuture, rrr::i32>::Ok(FailoverResumeSocketOutTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcFailoverResumeSocketOutResponse, rrr::i32> FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest& req) {
        auto __typed_fu_result__ = this->async_FailoverResumeSocketOut(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFailoverResumeSocketOutResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackBeginRecoveryTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JetpackBeginRecoveryTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackBeginRecoveryResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackBeginRecoveryResponse, rrr::i32>::Err(__ret__);
            }
            RpcJetpackBeginRecoveryResponse __typed_resp__;
            return rusty::Result<RpcJetpackBeginRecoveryResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackBeginRecoveryTypedFuture, rrr::i32> async_JetpackBeginRecovery(const RpcJetpackBeginRecoveryRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKBEGINRECOVERY, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.old_view, __m__);
            rrr::Serialize_::serialize(req.new_view, __m__);
            rrr::Serialize_::serialize(req.new_view_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackBeginRecoveryTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackBeginRecoveryTypedFuture, rrr::i32>::Ok(JetpackBeginRecoveryTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackBeginRecoveryResponse, rrr::i32> JetpackBeginRecovery(const RpcJetpackBeginRecoveryRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackBeginRecovery(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackBeginRecoveryResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackPullIdSetTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JetpackPullIdSetTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackPullIdSetResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackPullIdSetResponse, rrr::i32>::Err(__ret__);
            }
            RpcJetpackPullIdSetResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.id_set, __reply_ar__);
            return rusty::Result<RpcJetpackPullIdSetResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPullIdSetTypedFuture, rrr::i32> async_JetpackPullIdSet(const RpcJetpackPullIdSetRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPULLIDSET, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.jepoch, __m__);
            rrr::Serialize_::serialize(req.oepoch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackPullIdSetTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackPullIdSetTypedFuture, rrr::i32>::Ok(JetpackPullIdSetTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackPullIdSetResponse, rrr::i32> JetpackPullIdSet(const RpcJetpackPullIdSetRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackPullIdSet(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackPullIdSetResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackPullCmdTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JetpackPullCmdTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackPullCmdResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackPullCmdResponse, rrr::i32>::Err(__ret__);
            }
            RpcJetpackPullCmdResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.cmd_batch, __reply_ar__);
            return rusty::Result<RpcJetpackPullCmdResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPullCmdTypedFuture, rrr::i32> async_JetpackPullCmd(const RpcJetpackPullCmdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPULLCMD, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.jepoch, __m__);
            rrr::Serialize_::serialize(req.oepoch, __m__);
            rrr::Serialize_::serialize(req.key_batch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackPullCmdTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackPullCmdTypedFuture, rrr::i32>::Ok(JetpackPullCmdTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackPullCmdResponse, rrr::i32> JetpackPullCmd(const RpcJetpackPullCmdRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackPullCmd(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackPullCmdResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackRecordCmdTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JetpackRecordCmdTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackRecordCmdResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackRecordCmdResponse, rrr::i32>::Err(__ret__);
            }
            RpcJetpackRecordCmdResponse __typed_resp__;
            return rusty::Result<RpcJetpackRecordCmdResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackRecordCmdTypedFuture, rrr::i32> async_JetpackRecordCmd(const RpcJetpackRecordCmdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKRECORDCMD, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.jepoch, __m__);
            rrr::Serialize_::serialize(req.oepoch, __m__);
            rrr::Serialize_::serialize(req.sid, __m__);
            rrr::Serialize_::serialize(req.rid, __m__);
            rrr::Serialize_::serialize(req.cmd_batch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackRecordCmdTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackRecordCmdTypedFuture, rrr::i32>::Ok(JetpackRecordCmdTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackRecordCmdResponse, rrr::i32> JetpackRecordCmd(const RpcJetpackRecordCmdRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackRecordCmd(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackRecordCmdResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackPrepareTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JetpackPrepareTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackPrepareResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackPrepareResponse, rrr::i32>::Err(__ret__);
            }
            RpcJetpackPrepareResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_max_seen_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.accepted_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.replied_sid, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.replied_set_size, __reply_ar__);
            return rusty::Result<RpcJetpackPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPrepareTypedFuture, rrr::i32> async_JetpackPrepare(const RpcJetpackPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPREPARE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.jepoch, __m__);
            rrr::Serialize_::serialize(req.oepoch, __m__);
            rrr::Serialize_::serialize(req.max_seen_ballot, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackPrepareTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackPrepareTypedFuture, rrr::i32>::Ok(JetpackPrepareTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackPrepareResponse, rrr::i32> JetpackPrepare(const RpcJetpackPrepareRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackPrepare(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackPrepareResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JetpackAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcJetpackAcceptResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_max_seen_ballot, __reply_ar__);
            return rusty::Result<RpcJetpackAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackAcceptTypedFuture, rrr::i32> async_JetpackAccept(const RpcJetpackAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.jepoch, __m__);
            rrr::Serialize_::serialize(req.oepoch, __m__);
            rrr::Serialize_::serialize(req.max_seen_ballot, __m__);
            rrr::Serialize_::serialize(req.sid, __m__);
            rrr::Serialize_::serialize(req.set_size, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackAcceptTypedFuture, rrr::i32>::Ok(JetpackAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackAcceptResponse, rrr::i32> JetpackAccept(const RpcJetpackAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackCommitTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JetpackCommitTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackCommitResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackCommitResponse, rrr::i32>::Err(__ret__);
            }
            RpcJetpackCommitResponse __typed_resp__;
            return rusty::Result<RpcJetpackCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackCommitTypedFuture, rrr::i32> async_JetpackCommit(const RpcJetpackCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKCOMMIT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.jepoch, __m__);
            rrr::Serialize_::serialize(req.oepoch, __m__);
            rrr::Serialize_::serialize(req.sid, __m__);
            rrr::Serialize_::serialize(req.set_size, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackCommitTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackCommitTypedFuture, rrr::i32>::Ok(JetpackCommitTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackCommitResponse, rrr::i32> JetpackCommit(const RpcJetpackCommitRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackCommit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackCommitResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackPullRecSetInsTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JetpackPullRecSetInsTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackPullRecSetInsResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackPullRecSetInsResponse, rrr::i32>::Err(__ret__);
            }
            RpcJetpackPullRecSetInsResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.cmd, __reply_ar__);
            return rusty::Result<RpcJetpackPullRecSetInsResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPullRecSetInsTypedFuture, rrr::i32> async_JetpackPullRecSetIns(const RpcJetpackPullRecSetInsRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPULLRECSETINS, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.jepoch, __m__);
            rrr::Serialize_::serialize(req.oepoch, __m__);
            rrr::Serialize_::serialize(req.sid, __m__);
            rrr::Serialize_::serialize(req.rid, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackPullRecSetInsTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackPullRecSetInsTypedFuture, rrr::i32>::Ok(JetpackPullRecSetInsTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackPullRecSetInsResponse, rrr::i32> JetpackPullRecSetIns(const RpcJetpackPullRecSetInsRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackPullRecSetIns(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackPullRecSetInsResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class JetpackFinishRecoveryTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JetpackFinishRecoveryTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcJetpackFinishRecoveryResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJetpackFinishRecoveryResponse, rrr::i32>::Err(__ret__);
            }
            RpcJetpackFinishRecoveryResponse __typed_resp__;
            return rusty::Result<RpcJetpackFinishRecoveryResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackFinishRecoveryTypedFuture, rrr::i32> async_JetpackFinishRecovery(const RpcJetpackFinishRecoveryRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKFINISHRECOVERY, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.oepoch, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JetpackFinishRecoveryTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JetpackFinishRecoveryTypedFuture, rrr::i32>::Ok(JetpackFinishRecoveryTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJetpackFinishRecoveryResponse, rrr::i32> JetpackFinishRecovery(const RpcJetpackFinishRecoveryRequest& req) {
        auto __typed_fu_result__ = this->async_JetpackFinishRecovery(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJetpackFinishRecoveryResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class ServerControlService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcServerShutdownRequest {
    };
    friend inline void serialize(const RpcServerShutdownRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerShutdownRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerShutdownRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerShutdownRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerShutdownResponse {
    };
    friend inline void serialize(const RpcServerShutdownResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerShutdownResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerShutdownResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerShutdownResponse& o) { deserialize(o, ar); return ar; }

    struct RpcServerReadyRequest {
    };
    friend inline void serialize(const RpcServerReadyRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerReadyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerReadyRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerReadyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerReadyResponse {
        rrr::i32 res;
    };
    friend inline void serialize(const RpcServerReadyResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerReadyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerReadyResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerReadyResponse& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatWithDataRequest {
    };
    friend inline void serialize(const RpcServerHeartBeatWithDataRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerHeartBeatWithDataRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatWithDataRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerHeartBeatWithDataRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatWithDataResponse {
        ServerResponse res;
    };
    friend inline void serialize(const RpcServerHeartBeatWithDataResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerHeartBeatWithDataResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatWithDataResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerHeartBeatWithDataResponse& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatRequest {
    };
    friend inline void serialize(const RpcServerHeartBeatRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerHeartBeatRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerHeartBeatRequest& o) { deserialize(o, ar); return ar; }

    struct RpcServerHeartBeatResponse {
    };
    friend inline void serialize(const RpcServerHeartBeatResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcServerHeartBeatResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcServerHeartBeatResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcServerHeartBeatResponse& o) { deserialize(o, ar); return ar; }

    enum {
        SERVER_SHUTDOWN = 0x10af16ed,
        SERVER_READY = 0x4780016f,
        SERVER_HEART_BEAT_WITH_DATA = 0x46c9fd6b,
        SERVER_HEART_BEAT = 0x174c78b8,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
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
    virtual void server_shutdown(const RpcServerShutdownRequest& req, RpcServerShutdownResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void server_ready(const RpcServerReadyRequest& req, RpcServerReadyResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void server_heart_beat_with_data(const RpcServerHeartBeatWithDataRequest& req, RpcServerHeartBeatWithDataResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void server_heart_beat(const RpcServerHeartBeatRequest& req, RpcServerHeartBeatResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __server_shutdown__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcServerShutdownRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerShutdownResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->server_shutdown(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __server_ready__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcServerReadyRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerReadyResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->server_ready(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __server_heart_beat_with_data__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcServerHeartBeatWithDataRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerHeartBeatWithDataResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->server_heart_beat_with_data(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __server_heart_beat__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcServerHeartBeatRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcServerHeartBeatResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->server_heart_beat(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ServerControlProxy {
protected:
    rrr::Client* __cl__;
public:
    ServerControlProxy(rrr::Client* cl): __cl__(cl) { }
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
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit server_shutdownTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcServerShutdownResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerShutdownResponse, rrr::i32>::Err(__ret__);
            }
            RpcServerShutdownResponse __typed_resp__;
            return rusty::Result<RpcServerShutdownResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_shutdownTypedFuture, rrr::i32> async_server_shutdown(const RpcServerShutdownRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_SHUTDOWN, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_shutdownTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_shutdownTypedFuture, rrr::i32>::Ok(server_shutdownTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerShutdownResponse, rrr::i32> server_shutdown(const RpcServerShutdownRequest& req) {
        auto __typed_fu_result__ = this->async_server_shutdown(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerShutdownResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class server_readyTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit server_readyTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcServerReadyResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerReadyResponse, rrr::i32>::Err(__ret__);
            }
            RpcServerReadyResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcServerReadyResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_readyTypedFuture, rrr::i32> async_server_ready(const RpcServerReadyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_READY, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_readyTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_readyTypedFuture, rrr::i32>::Ok(server_readyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerReadyResponse, rrr::i32> server_ready(const RpcServerReadyRequest& req) {
        auto __typed_fu_result__ = this->async_server_ready(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerReadyResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class server_heart_beat_with_dataTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit server_heart_beat_with_dataTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcServerHeartBeatWithDataResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerHeartBeatWithDataResponse, rrr::i32>::Err(__ret__);
            }
            RpcServerHeartBeatWithDataResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcServerHeartBeatWithDataResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_heart_beat_with_dataTypedFuture, rrr::i32> async_server_heart_beat_with_data(const RpcServerHeartBeatWithDataRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_HEART_BEAT_WITH_DATA, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_heart_beat_with_dataTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_heart_beat_with_dataTypedFuture, rrr::i32>::Ok(server_heart_beat_with_dataTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerHeartBeatWithDataResponse, rrr::i32> server_heart_beat_with_data(const RpcServerHeartBeatWithDataRequest& req) {
        auto __typed_fu_result__ = this->async_server_heart_beat_with_data(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerHeartBeatWithDataResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class server_heart_beatTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit server_heart_beatTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcServerHeartBeatResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcServerHeartBeatResponse, rrr::i32>::Err(__ret__);
            }
            RpcServerHeartBeatResponse __typed_resp__;
            return rusty::Result<RpcServerHeartBeatResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_heart_beatTypedFuture, rrr::i32> async_server_heart_beat(const RpcServerHeartBeatRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_HEART_BEAT, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<server_heart_beatTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<server_heart_beatTypedFuture, rrr::i32>::Ok(server_heart_beatTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcServerHeartBeatResponse, rrr::i32> server_heart_beat(const RpcServerHeartBeatRequest& req) {
        auto __typed_fu_result__ = this->async_server_heart_beat(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcServerHeartBeatResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class ClientControlService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcClientGetTxnNamesRequest {
    };
    friend inline void serialize(const RpcClientGetTxnNamesRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientGetTxnNamesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientGetTxnNamesRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientGetTxnNamesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientGetTxnNamesResponse {
        std::map<rrr::i32, std::string> txn_names;
    };
    friend inline void serialize(const RpcClientGetTxnNamesResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.txn_names, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientGetTxnNamesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientGetTxnNamesResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.txn_names, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientGetTxnNamesResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientShutdownRequest {
    };
    friend inline void serialize(const RpcClientShutdownRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientShutdownRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientShutdownRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientShutdownRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientShutdownResponse {
    };
    friend inline void serialize(const RpcClientShutdownResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientShutdownResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientShutdownResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientShutdownResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientForceStopRequest {
    };
    friend inline void serialize(const RpcClientForceStopRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientForceStopRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientForceStopRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientForceStopRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientForceStopResponse {
    };
    friend inline void serialize(const RpcClientForceStopResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientForceStopResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientForceStopResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientForceStopResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientResponseRequest {
        DepId dep_id;
    };
    friend inline void serialize(const RpcClientResponseRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientResponseRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientResponseRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientResponseRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientResponseResponse {
        ClientResponse res;
    };
    friend inline void serialize(const RpcClientResponseResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientResponseResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientResponseResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientResponseResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientReadyRequest {
    };
    friend inline void serialize(const RpcClientReadyRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientReadyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientReadyRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientReadyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientReadyResponse {
        rrr::i32 res;
    };
    friend inline void serialize(const RpcClientReadyResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientReadyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientReadyResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientReadyResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientReadyBlockRequest {
    };
    friend inline void serialize(const RpcClientReadyBlockRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientReadyBlockRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientReadyBlockRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientReadyBlockRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientReadyBlockResponse {
        rrr::i32 res;
    };
    friend inline void serialize(const RpcClientReadyBlockResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientReadyBlockResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientReadyBlockResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientReadyBlockResponse& o) { deserialize(o, ar); return ar; }

    struct RpcClientStartRequest {
    };
    friend inline void serialize(const RpcClientStartRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientStartRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientStartRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientStartRequest& o) { deserialize(o, ar); return ar; }

    struct RpcClientStartResponse {
    };
    friend inline void serialize(const RpcClientStartResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcClientStartResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcClientStartResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcClientStartResponse& o) { deserialize(o, ar); return ar; }

    struct RpcDispatchTxnRequest {
        TxDispatchRequest req;
    };
    friend inline void serialize(const RpcDispatchTxnRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.req, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcDispatchTxnRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDispatchTxnRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.req, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcDispatchTxnRequest& o) { deserialize(o, ar); return ar; }

    struct RpcDispatchTxnResponse {
        TxReply result;
    };
    friend inline void serialize(const RpcDispatchTxnResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.result, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcDispatchTxnResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDispatchTxnResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.result, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcDispatchTxnResponse& o) { deserialize(o, ar); return ar; }

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
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
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
    virtual void client_get_txn_names(const RpcClientGetTxnNamesRequest& req, RpcClientGetTxnNamesResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void client_shutdown(const RpcClientShutdownRequest& req, RpcClientShutdownResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void client_force_stop(const RpcClientForceStopRequest& req, RpcClientForceStopResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void client_response(const RpcClientResponseRequest& req, RpcClientResponseResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void client_ready(const RpcClientReadyRequest& req, RpcClientReadyResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void client_ready_block(const RpcClientReadyBlockRequest& req, RpcClientReadyBlockResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void client_start(const RpcClientStartRequest& req, RpcClientStartResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void DispatchTxn(const RpcDispatchTxnRequest& req, RpcDispatchTxnResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __client_get_txn_names__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientGetTxnNamesRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientGetTxnNamesResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->txn_names, m);
                },
                []() {});
            this->client_get_txn_names(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_shutdown__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientShutdownRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientShutdownResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->client_shutdown(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_force_stop__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientForceStopRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientForceStopResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->client_force_stop(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_response__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientResponseRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcClientResponseResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->client_response(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_ready__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientReadyRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientReadyResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->client_ready(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_ready_block__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientReadyBlockRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientReadyBlockResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->client_ready_block(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __client_start__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcClientStartRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcClientStartResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->client_start(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __DispatchTxn__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDispatchTxnRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.req, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcDispatchTxnResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->result, m);
                },
                []() {});
            this->DispatchTxn(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ClientControlProxy {
protected:
    rrr::Client* __cl__;
public:
    ClientControlProxy(rrr::Client* cl): __cl__(cl) { }
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
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit client_get_txn_namesTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientGetTxnNamesResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientGetTxnNamesResponse, rrr::i32>::Err(__ret__);
            }
            RpcClientGetTxnNamesResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.txn_names, __reply_ar__);
            return rusty::Result<RpcClientGetTxnNamesResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_get_txn_namesTypedFuture, rrr::i32> async_client_get_txn_names(const RpcClientGetTxnNamesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_GET_TXN_NAMES, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_get_txn_namesTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_get_txn_namesTypedFuture, rrr::i32>::Ok(client_get_txn_namesTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientGetTxnNamesResponse, rrr::i32> client_get_txn_names(const RpcClientGetTxnNamesRequest& req) {
        auto __typed_fu_result__ = this->async_client_get_txn_names(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientGetTxnNamesResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_shutdownTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit client_shutdownTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientShutdownResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientShutdownResponse, rrr::i32>::Err(__ret__);
            }
            RpcClientShutdownResponse __typed_resp__;
            return rusty::Result<RpcClientShutdownResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_shutdownTypedFuture, rrr::i32> async_client_shutdown(const RpcClientShutdownRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_SHUTDOWN, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_shutdownTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_shutdownTypedFuture, rrr::i32>::Ok(client_shutdownTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientShutdownResponse, rrr::i32> client_shutdown(const RpcClientShutdownRequest& req) {
        auto __typed_fu_result__ = this->async_client_shutdown(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientShutdownResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_force_stopTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit client_force_stopTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientForceStopResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientForceStopResponse, rrr::i32>::Err(__ret__);
            }
            RpcClientForceStopResponse __typed_resp__;
            return rusty::Result<RpcClientForceStopResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_force_stopTypedFuture, rrr::i32> async_client_force_stop(const RpcClientForceStopRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_FORCE_STOP, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_force_stopTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_force_stopTypedFuture, rrr::i32>::Ok(client_force_stopTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientForceStopResponse, rrr::i32> client_force_stop(const RpcClientForceStopRequest& req) {
        auto __typed_fu_result__ = this->async_client_force_stop(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientForceStopResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_responseTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit client_responseTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientResponseResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientResponseResponse, rrr::i32>::Err(__ret__);
            }
            RpcClientResponseResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcClientResponseResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_responseTypedFuture, rrr::i32> async_client_response(const RpcClientResponseRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_RESPONSE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<client_responseTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<client_responseTypedFuture, rrr::i32>::Ok(client_responseTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientResponseResponse, rrr::i32> client_response(const RpcClientResponseRequest& req) {
        auto __typed_fu_result__ = this->async_client_response(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientResponseResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_readyTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit client_readyTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientReadyResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientReadyResponse, rrr::i32>::Err(__ret__);
            }
            RpcClientReadyResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcClientReadyResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_readyTypedFuture, rrr::i32> async_client_ready(const RpcClientReadyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_READY, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_readyTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_readyTypedFuture, rrr::i32>::Ok(client_readyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientReadyResponse, rrr::i32> client_ready(const RpcClientReadyRequest& req) {
        auto __typed_fu_result__ = this->async_client_ready(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientReadyResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_ready_blockTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit client_ready_blockTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientReadyBlockResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientReadyBlockResponse, rrr::i32>::Err(__ret__);
            }
            RpcClientReadyBlockResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcClientReadyBlockResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_ready_blockTypedFuture, rrr::i32> async_client_ready_block(const RpcClientReadyBlockRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_READY_BLOCK, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_ready_blockTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_ready_blockTypedFuture, rrr::i32>::Ok(client_ready_blockTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientReadyBlockResponse, rrr::i32> client_ready_block(const RpcClientReadyBlockRequest& req) {
        auto __typed_fu_result__ = this->async_client_ready_block(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientReadyBlockResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class client_startTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit client_startTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcClientStartResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcClientStartResponse, rrr::i32>::Err(__ret__);
            }
            RpcClientStartResponse __typed_resp__;
            return rusty::Result<RpcClientStartResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_startTypedFuture, rrr::i32> async_client_start(const RpcClientStartRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_START, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<client_startTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<client_startTypedFuture, rrr::i32>::Ok(client_startTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcClientStartResponse, rrr::i32> client_start(const RpcClientStartRequest& req) {
        auto __typed_fu_result__ = this->async_client_start(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcClientStartResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    class DispatchTxnTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit DispatchTxnTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcDispatchTxnResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcDispatchTxnResponse, rrr::i32>::Err(__ret__);
            }
            RpcDispatchTxnResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.result, __reply_ar__);
            return rusty::Result<RpcDispatchTxnResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<DispatchTxnTypedFuture, rrr::i32> async_DispatchTxn(const RpcDispatchTxnRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::DISPATCHTXN, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.req, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<DispatchTxnTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<DispatchTxnTypedFuture, rrr::i32>::Ok(DispatchTxnTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcDispatchTxnResponse, rrr::i32> DispatchTxn(const RpcDispatchTxnRequest& req) {
        auto __typed_fu_result__ = this->async_DispatchTxn(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDispatchTxnResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
    friend inline void serialize(const RpcReadConfigKeyRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.key, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcReadConfigKeyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReadConfigKeyRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.key, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcReadConfigKeyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcReadConfigKeyResponse {
        rrr::i32 found;
        std::string value;
    };
    friend inline void serialize(const RpcReadConfigKeyResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.found, ar);
        rrr::Serialize_::serialize(o.value, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcReadConfigKeyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcReadConfigKeyResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.found, ar);
        rrr::Deserialize_::deserialize(o.value, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcReadConfigKeyResponse& o) { deserialize(o, ar); return ar; }

    enum {
        READCONFIGKEY = 0x584cdad1,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        switch (rpc_id) {
        case READCONFIGKEY: __ReadConfigKey__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void ReadConfigKey(const RpcReadConfigKeyRequest& req, RpcReadConfigKeyResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __ReadConfigKey__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcReadConfigKeyRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy_buffer(&req->src));
            rrr::Deserialize_::deserialize(__typed_req__.key, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcReadConfigKeyResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->found, m);
                    rrr::Serialize_::serialize(__typed_resp__->value, m);
                },
                []() {});
            this->ReadConfigKey(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ConfigKvServiceProxy {
protected:
    rrr::Client* __cl__;
public:
    ConfigKvServiceProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcReadConfigKeyRequest = ConfigKvServiceService::RpcReadConfigKeyRequest;
    using RpcReadConfigKeyResponse = ConfigKvServiceService::RpcReadConfigKeyResponse;
    class ReadConfigKeyTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit ReadConfigKeyTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
        bool ready() const {
            return __fu__->ready();
        }
        void wait() const {
            __fu__->wait();
        }
        rrr::i32 get_error_code() const {
            return __fu__->get_error_code();
        }
        rusty::Arc<rrr::Future> raw_future() const {
            return __fu__;
        }
        rusty::Result<RpcReadConfigKeyResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcReadConfigKeyResponse, rrr::i32>::Err(__ret__);
            }
            RpcReadConfigKeyResponse __typed_resp__;
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy_buffer(&__reply_guard__->src));
            rrr::Deserialize_::deserialize(__typed_resp__.found, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.value, __reply_ar__);
            return rusty::Result<RpcReadConfigKeyResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ReadConfigKeyTypedFuture, rrr::i32> async_ReadConfigKey(const RpcReadConfigKeyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigKvServiceService::READCONFIGKEY, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.key, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<ReadConfigKeyTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<ReadConfigKeyTypedFuture, rrr::i32>::Ok(ReadConfigKeyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcReadConfigKeyResponse, rrr::i32> ReadConfigKey(const RpcReadConfigKeyRequest& req) {
        auto __typed_fu_result__ = this->async_ReadConfigKey(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcReadConfigKeyResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

} // namespace janus



