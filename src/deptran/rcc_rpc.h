#pragma once

#include "rrr.hpp"

#include <errno.h>
#include <memory>

#include "procedure.h"
#include "rcc/tx.h"
namespace janus {

struct ValueTimesPair {
    rrr::i64 value;
    rrr::i64 times;
};

inline rrr::Marshal& operator <<(rrr::Marshal& m, const ValueTimesPair& o) {
    m << o.value;
    m << o.times;
    return m;
}

inline rrr::Marshal& operator >>(rrr::Marshal& m, ValueTimesPair& o) {
    m >> o.value;
    m >> o.times;
    return m;
}

struct DepId {
    std::string str;
    rrr::i64 id;
};

inline rrr::Marshal& operator <<(rrr::Marshal& m, const DepId& o) {
    m << o.str;
    m << o.id;
    return m;
}

inline rrr::Marshal& operator >>(rrr::Marshal& m, DepId& o) {
    m >> o.str;
    m >> o.id;
    return m;
}

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

inline rrr::Marshal& operator <<(rrr::Marshal& m, const TxnInfoRes& o) {
    m << o.start_txn;
    m << o.total_txn;
    m << o.total_try;
    m << o.commit_txn;
    m << o.num_exhausted;
    m << o.this_latency;
    m << o.last_latency;
    m << o.attempt_latency;
    m << o.interval_latency;
    m << o.all_interval_latency;
    m << o.num_try;
    return m;
}

inline rrr::Marshal& operator >>(rrr::Marshal& m, TxnInfoRes& o) {
    m >> o.start_txn;
    m >> o.total_txn;
    m >> o.total_try;
    m >> o.commit_txn;
    m >> o.num_exhausted;
    m >> o.this_latency;
    m >> o.last_latency;
    m >> o.attempt_latency;
    m >> o.interval_latency;
    m >> o.all_interval_latency;
    m >> o.num_try;
    return m;
}

struct ServerResponse {
    std::map<std::string, ValueTimesPair> statistics;
    double cpu_util;
    rrr::i64 r_cnt_sum;
    rrr::i64 r_cnt_num;
    rrr::i64 r_sz_sum;
    rrr::i64 r_sz_num;
};

inline rrr::Marshal& operator <<(rrr::Marshal& m, const ServerResponse& o) {
    m << o.statistics;
    m << o.cpu_util;
    m << o.r_cnt_sum;
    m << o.r_cnt_num;
    m << o.r_sz_sum;
    m << o.r_sz_num;
    return m;
}

inline rrr::Marshal& operator >>(rrr::Marshal& m, ServerResponse& o) {
    m >> o.statistics;
    m >> o.cpu_util;
    m >> o.r_cnt_sum;
    m >> o.r_cnt_num;
    m >> o.r_sz_sum;
    m >> o.r_sz_num;
    return m;
}

struct ClientResponse {
    std::map<rrr::i32, TxnInfoRes> txn_info;
    rrr::i64 run_sec;
    rrr::i64 run_nsec;
    rrr::i64 period_sec;
    rrr::i64 period_nsec;
    rrr::i32 is_finish;
    rrr::i64 n_asking;
};

inline rrr::Marshal& operator <<(rrr::Marshal& m, const ClientResponse& o) {
    m << o.txn_info;
    m << o.run_sec;
    m << o.run_nsec;
    m << o.period_sec;
    m << o.period_nsec;
    m << o.is_finish;
    m << o.n_asking;
    return m;
}

inline rrr::Marshal& operator >>(rrr::Marshal& m, ClientResponse& o) {
    m >> o.txn_info;
    m >> o.run_sec;
    m >> o.run_nsec;
    m >> o.period_sec;
    m >> o.period_nsec;
    m >> o.is_finish;
    m >> o.n_asking;
    return m;
}

struct Profiling {
    double cpu_util;
    double tx_util;
    double rx_util;
    double mem_util;
};

inline rrr::Marshal& operator <<(rrr::Marshal& m, const Profiling& o) {
    m << o.cpu_util;
    m << o.tx_util;
    m << o.rx_util;
    m << o.mem_util;
    return m;
}

inline rrr::Marshal& operator >>(rrr::Marshal& m, Profiling& o) {
    m >> o.cpu_util;
    m >> o.tx_util;
    m >> o.rx_util;
    m >> o.mem_util;
    return m;
}

struct TxDispatchRequest {
    rrr::i32 id;
    rrr::i32 tx_type;
    std::vector<Value> input;
};

inline rrr::Marshal& operator <<(rrr::Marshal& m, const TxDispatchRequest& o) {
    m << o.id;
    m << o.tx_type;
    m << o.input;
    return m;
}

inline rrr::Marshal& operator >>(rrr::Marshal& m, TxDispatchRequest& o) {
    m >> o.id;
    m >> o.tx_type;
    m >> o.input;
    return m;
}

struct TxnDispatchResponse {
};

inline rrr::Marshal& operator <<(rrr::Marshal& m, const TxnDispatchResponse& o) {
    return m;
}

inline rrr::Marshal& operator >>(rrr::Marshal& m, TxnDispatchResponse& o) {
    return m;
}

class MultiPaxosService : public rrr::Service {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcForwardRequest {
        MarshallDeputy cmd;
        uint64_t dep_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcForwardRequest& o) {
        m << o.cmd;
        m << o.dep_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcForwardRequest& o) {
        m >> o.cmd;
        m >> o.dep_id;
        return m;
    }

    struct RpcForwardResponse {
        uint64_t coro_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcForwardResponse& o) {
        m << o.coro_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcForwardResponse& o) {
        m >> o.coro_id;
        return m;
    }

    struct RpcPrepareRequest {
        uint64_t slot;
        ballot_t ballot;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPrepareRequest& o) {
        m << o.slot;
        m << o.ballot;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPrepareRequest& o) {
        m >> o.slot;
        m >> o.ballot;
        return m;
    }

    struct RpcPrepareResponse {
        ballot_t max_ballot;
        uint64_t coro_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPrepareResponse& o) {
        m << o.max_ballot;
        m << o.coro_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPrepareResponse& o) {
        m >> o.max_ballot;
        m >> o.coro_id;
        return m;
    }

    struct RpcAcceptRequest {
        uint64_t slot;
        uint64_t time;
        ballot_t ballot;
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAcceptRequest& o) {
        m << o.slot;
        m << o.time;
        m << o.ballot;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAcceptRequest& o) {
        m >> o.slot;
        m >> o.time;
        m >> o.ballot;
        m >> o.cmd;
        return m;
    }

    struct RpcAcceptResponse {
        ballot_t max_ballot;
        uint64_t coro_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAcceptResponse& o) {
        m << o.max_ballot;
        m << o.coro_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAcceptResponse& o) {
        m >> o.max_ballot;
        m >> o.coro_id;
        return m;
    }

    struct RpcDecideRequest {
        uint64_t slot;
        ballot_t ballot;
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDecideRequest& o) {
        m << o.slot;
        m << o.ballot;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDecideRequest& o) {
        m >> o.slot;
        m >> o.ballot;
        m >> o.cmd;
        return m;
    }

    struct RpcDecideResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDecideResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDecideResponse& o) {
        return m;
    }

    struct RpcHeartbeatRequest {
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcHeartbeatRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcHeartbeatRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcHeartbeatResponse {
        rrr::i32 ballot;
        rrr::i32 val;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcHeartbeatResponse& o) {
        m << o.ballot;
        m << o.val;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcHeartbeatResponse& o) {
        m >> o.ballot;
        m >> o.val;
        return m;
    }

    struct RpcForwardToLearnerServerRequest {
        rrr::i32 par_id;
        uint64_t slot;
        ballot_t ballot;
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcForwardToLearnerServerRequest& o) {
        m << o.par_id;
        m << o.slot;
        m << o.ballot;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcForwardToLearnerServerRequest& o) {
        m >> o.par_id;
        m >> o.slot;
        m >> o.ballot;
        m >> o.cmd;
        return m;
    }

    struct RpcForwardToLearnerServerResponse {
        uint64_t ret_slot;
        ballot_t ret_ballot;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcForwardToLearnerServerResponse& o) {
        m << o.ret_slot;
        m << o.ret_ballot;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcForwardToLearnerServerResponse& o) {
        m >> o.ret_slot;
        m >> o.ret_ballot;
        return m;
    }

    struct RpcBulkPrepareRequest {
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcBulkPrepareRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcBulkPrepareRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcBulkPrepareResponse {
        rrr::i32 ballot;
        rrr::i32 val;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcBulkPrepareResponse& o) {
        m << o.ballot;
        m << o.val;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcBulkPrepareResponse& o) {
        m >> o.ballot;
        m >> o.val;
        return m;
    }

    struct RpcBulkAcceptRequest {
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcBulkAcceptRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcBulkAcceptRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcBulkAcceptResponse {
        rrr::i32 ballot;
        rrr::i32 val;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcBulkAcceptResponse& o) {
        m << o.ballot;
        m << o.val;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcBulkAcceptResponse& o) {
        m >> o.ballot;
        m >> o.val;
        return m;
    }

    struct RpcBulkPrepare2Request {
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcBulkPrepare2Request& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcBulkPrepare2Request& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcBulkPrepare2Response {
        rrr::i32 ballot;
        rrr::i32 val;
        MarshallDeputy ret;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcBulkPrepare2Response& o) {
        m << o.ballot;
        m << o.val;
        m << o.ret;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcBulkPrepare2Response& o) {
        m >> o.ballot;
        m >> o.val;
        m >> o.ret;
        return m;
    }

    struct RpcSyncLogRequest {
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSyncLogRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSyncLogRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcSyncLogResponse {
        rrr::i32 ballot;
        rrr::i32 val;
        MarshallDeputy ret;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSyncLogResponse& o) {
        m << o.ballot;
        m << o.val;
        m << o.ret;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSyncLogResponse& o) {
        m >> o.ballot;
        m >> o.val;
        m >> o.ret;
        return m;
    }

    struct RpcSyncCommitRequest {
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSyncCommitRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSyncCommitRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcSyncCommitResponse {
        rrr::i32 ballot;
        rrr::i32 val;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSyncCommitResponse& o) {
        m << o.ballot;
        m << o.val;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSyncCommitResponse& o) {
        m >> o.ballot;
        m >> o.val;
        return m;
    }

    struct RpcSyncNoOpsRequest {
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSyncNoOpsRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSyncNoOpsRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcSyncNoOpsResponse {
        rrr::i32 ballot;
        rrr::i32 val;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSyncNoOpsResponse& o) {
        m << o.ballot;
        m << o.val;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSyncNoOpsResponse& o) {
        m >> o.ballot;
        m >> o.val;
        return m;
    }

    struct RpcBulkDecideRequest {
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcBulkDecideRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcBulkDecideRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcBulkDecideResponse {
        rrr::i32 ballot;
        rrr::i32 val;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcBulkDecideResponse& o) {
        m << o.ballot;
        m << o.val;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcBulkDecideResponse& o) {
        m >> o.ballot;
        m >> o.val;
        return m;
    }

    enum {
        FORWARD = 0x59f9f2b7,
        PREPARE = 0x5686a451,
        ACCEPT = 0x6aa3abed,
        DECIDE = 0x616ddafc,
        HEARTBEAT = 0x6c35c93b,
        FORWARDTOLEARNERSERVER = 0x33295296,
        BULKPREPARE = 0x65d6e27e,
        BULKACCEPT = 0x67704f29,
        BULKPREPARE2 = 0x5dac6871,
        SYNCLOG = 0x6ec9dd8e,
        SYNCCOMMIT = 0x2dc149f6,
        SYNCNOOPS = 0x6d3159a9,
        BULKDECIDE = 0x67f0731b,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        int ret = 0;
        if ((ret = svr.reg_rpc(FORWARD, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(PREPARE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(ACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(DECIDE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(HEARTBEAT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FORWARDTOLEARNERSERVER, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(BULKPREPARE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(BULKACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(BULKPREPARE2, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SYNCLOG, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SYNCCOMMIT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SYNCNOOPS, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(BULKDECIDE, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(FORWARD);
        svr.unreg(PREPARE);
        svr.unreg(ACCEPT);
        svr.unreg(DECIDE);
        svr.unreg(HEARTBEAT);
        svr.unreg(FORWARDTOLEARNERSERVER);
        svr.unreg(BULKPREPARE);
        svr.unreg(BULKACCEPT);
        svr.unreg(BULKPREPARE2);
        svr.unreg(SYNCLOG);
        svr.unreg(SYNCCOMMIT);
        svr.unreg(SYNCNOOPS);
        svr.unreg(BULKDECIDE);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
        switch (rpc_id) {
        case FORWARD: __Forward__wrapper__(std::move(req), weak_sconn); break;
        case PREPARE: __Prepare__wrapper__(std::move(req), weak_sconn); break;
        case ACCEPT: __Accept__wrapper__(std::move(req), weak_sconn); break;
        case DECIDE: __Decide__wrapper__(std::move(req), weak_sconn); break;
        case HEARTBEAT: __Heartbeat__wrapper__(std::move(req), weak_sconn); break;
        case FORWARDTOLEARNERSERVER: __ForwardToLearnerServer__wrapper__(std::move(req), weak_sconn); break;
        case BULKPREPARE: __BulkPrepare__wrapper__(std::move(req), weak_sconn); break;
        case BULKACCEPT: __BulkAccept__wrapper__(std::move(req), weak_sconn); break;
        case BULKPREPARE2: __BulkPrepare2__wrapper__(std::move(req), weak_sconn); break;
        case SYNCLOG: __SyncLog__wrapper__(std::move(req), weak_sconn); break;
        case SYNCCOMMIT: __SyncCommit__wrapper__(std::move(req), weak_sconn); break;
        case SYNCNOOPS: __SyncNoOps__wrapper__(std::move(req), weak_sconn); break;
        case BULKDECIDE: __BulkDecide__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void Forward(const RpcForwardRequest& req, RpcForwardResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Prepare(const RpcPrepareRequest& req, RpcPrepareResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Accept(const RpcAcceptRequest& req, RpcAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Decide(const RpcDecideRequest& req, RpcDecideResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Heartbeat(const RpcHeartbeatRequest& req, RpcHeartbeatResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req, RpcForwardToLearnerServerResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void BulkPrepare(const RpcBulkPrepareRequest& req, RpcBulkPrepareResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void BulkAccept(const RpcBulkAcceptRequest& req, RpcBulkAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void BulkPrepare2(const RpcBulkPrepare2Request& req, RpcBulkPrepare2Response& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void SyncLog(const RpcSyncLogRequest& req, RpcSyncLogResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void SyncCommit(const RpcSyncCommitRequest& req, RpcSyncCommitResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void SyncNoOps(const RpcSyncNoOpsRequest& req, RpcSyncNoOpsResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void BulkDecide(const RpcBulkDecideRequest& req, RpcBulkDecideResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Forward__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcForwardRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            req->m >> __typed_req__.dep_id;
            auto __typed_resp__ = std::make_shared<RpcForwardResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->coro_id;
                },
                []() {});
            this->Forward(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Prepare__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPrepareRequest __typed_req__;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            auto __typed_resp__ = std::make_shared<RpcPrepareResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->max_ballot;
                    m << __typed_resp__->coro_id;
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
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.time;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->max_ballot;
                    m << __typed_resp__->coro_id;
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
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcDecideResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->Decide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Heartbeat__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcHeartbeatRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcHeartbeatResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ballot;
                    m << __typed_resp__->val;
                },
                []() {});
            this->Heartbeat(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __ForwardToLearnerServer__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcForwardToLearnerServerRequest __typed_req__;
            req->m >> __typed_req__.par_id;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcForwardToLearnerServerResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ret_slot;
                    m << __typed_resp__->ret_ballot;
                },
                []() {});
            this->ForwardToLearnerServer(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkPrepare__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcBulkPrepareRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcBulkPrepareResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ballot;
                    m << __typed_resp__->val;
                },
                []() {});
            this->BulkPrepare(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcBulkAcceptRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcBulkAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ballot;
                    m << __typed_resp__->val;
                },
                []() {});
            this->BulkAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkPrepare2__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcBulkPrepare2Request __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcBulkPrepare2Response>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ballot;
                    m << __typed_resp__->val;
                    m << __typed_resp__->ret;
                },
                []() {});
            this->BulkPrepare2(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SyncLog__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSyncLogRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcSyncLogResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ballot;
                    m << __typed_resp__->val;
                    m << __typed_resp__->ret;
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
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcSyncCommitResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ballot;
                    m << __typed_resp__->val;
                },
                []() {});
            this->SyncCommit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SyncNoOps__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSyncNoOpsRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcSyncNoOpsResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ballot;
                    m << __typed_resp__->val;
                },
                []() {});
            this->SyncNoOps(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __BulkDecide__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcBulkDecideRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcBulkDecideResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ballot;
                    m << __typed_resp__->val;
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
    using RpcForwardRequest = MultiPaxosService::RpcForwardRequest;
    using RpcForwardResponse = MultiPaxosService::RpcForwardResponse;
    using RpcPrepareRequest = MultiPaxosService::RpcPrepareRequest;
    using RpcPrepareResponse = MultiPaxosService::RpcPrepareResponse;
    using RpcAcceptRequest = MultiPaxosService::RpcAcceptRequest;
    using RpcAcceptResponse = MultiPaxosService::RpcAcceptResponse;
    using RpcDecideRequest = MultiPaxosService::RpcDecideRequest;
    using RpcDecideResponse = MultiPaxosService::RpcDecideResponse;
    using RpcHeartbeatRequest = MultiPaxosService::RpcHeartbeatRequest;
    using RpcHeartbeatResponse = MultiPaxosService::RpcHeartbeatResponse;
    using RpcForwardToLearnerServerRequest = MultiPaxosService::RpcForwardToLearnerServerRequest;
    using RpcForwardToLearnerServerResponse = MultiPaxosService::RpcForwardToLearnerServerResponse;
    using RpcBulkPrepareRequest = MultiPaxosService::RpcBulkPrepareRequest;
    using RpcBulkPrepareResponse = MultiPaxosService::RpcBulkPrepareResponse;
    using RpcBulkAcceptRequest = MultiPaxosService::RpcBulkAcceptRequest;
    using RpcBulkAcceptResponse = MultiPaxosService::RpcBulkAcceptResponse;
    using RpcBulkPrepare2Request = MultiPaxosService::RpcBulkPrepare2Request;
    using RpcBulkPrepare2Response = MultiPaxosService::RpcBulkPrepare2Response;
    using RpcSyncLogRequest = MultiPaxosService::RpcSyncLogRequest;
    using RpcSyncLogResponse = MultiPaxosService::RpcSyncLogResponse;
    using RpcSyncCommitRequest = MultiPaxosService::RpcSyncCommitRequest;
    using RpcSyncCommitResponse = MultiPaxosService::RpcSyncCommitResponse;
    using RpcSyncNoOpsRequest = MultiPaxosService::RpcSyncNoOpsRequest;
    using RpcSyncNoOpsResponse = MultiPaxosService::RpcSyncNoOpsResponse;
    using RpcBulkDecideRequest = MultiPaxosService::RpcBulkDecideRequest;
    using RpcBulkDecideResponse = MultiPaxosService::RpcBulkDecideResponse;
    class ForwardTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit ForwardTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcForwardResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcForwardResponse, rrr::i32>::Err(__ret__);
            }
            RpcForwardResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.coro_id;
            return rusty::Result<RpcForwardResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ForwardTypedFuture, rrr::i32> async_Forward(const RpcForwardRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::FORWARD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
            __m__ << req.dep_id;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<ForwardTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<ForwardTypedFuture, rrr::i32>::Ok(ForwardTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcForwardResponse, rrr::i32> Forward(const RpcForwardRequest& req) {
        auto __typed_fu_result__ = this->async_Forward(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcForwardResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_Forward(const RpcForwardRequest&) instead")]]
    rrr::FutureResult async_Forward(const MarshallDeputy& cmd, const uint64_t& dep_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcForwardRequest __req__;
        __req__.cmd = cmd;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->async_Forward(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Forward(const RpcForwardRequest&) instead")]]
    rrr::i32 Forward(const MarshallDeputy& cmd, const uint64_t& dep_id, uint64_t* coro_id) {
        RpcForwardRequest __req__;
        __req__.cmd = cmd;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->Forward(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (coro_id) *coro_id = __resp__.coro_id;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.max_ballot;
            __fu__->get_reply() >> __typed_resp__.coro_id;
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<PrepareTypedFuture, rrr::i32> async_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::PREPARE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.slot;
            __m__ << req.ballot;
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
    [[deprecated("use typed async_Prepare(const RpcPrepareRequest&) instead")]]
    rrr::FutureResult async_Prepare(const uint64_t& slot, const ballot_t& ballot, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcPrepareRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        auto __typed_result__ = this->async_Prepare(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Prepare(const RpcPrepareRequest&) instead")]]
    rrr::i32 Prepare(const uint64_t& slot, const ballot_t& ballot, ballot_t* max_ballot, uint64_t* coro_id) {
        RpcPrepareRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        auto __typed_result__ = this->Prepare(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (max_ballot) *max_ballot = __resp__.max_ballot;
        if (coro_id) *coro_id = __resp__.coro_id;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.max_ballot;
            __fu__->get_reply() >> __typed_resp__.coro_id;
            return rusty::Result<RpcAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AcceptTypedFuture, rrr::i32> async_Accept(const RpcAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::ACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.slot;
            __m__ << req.time;
            __m__ << req.ballot;
            __m__ << req.cmd;
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
    [[deprecated("use typed async_Accept(const RpcAcceptRequest&) instead")]]
    rrr::FutureResult async_Accept(const uint64_t& slot, const uint64_t& time, const ballot_t& ballot, const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcAcceptRequest __req__;
        __req__.slot = slot;
        __req__.time = time;
        __req__.ballot = ballot;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_Accept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Accept(const RpcAcceptRequest&) instead")]]
    rrr::i32 Accept(const uint64_t& slot, const uint64_t& time, const ballot_t& ballot, const MarshallDeputy& cmd, ballot_t* max_ballot, uint64_t* coro_id) {
        RpcAcceptRequest __req__;
        __req__.slot = slot;
        __req__.time = time;
        __req__.ballot = ballot;
        __req__.cmd = cmd;
        auto __typed_result__ = this->Accept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (max_ballot) *max_ballot = __resp__.max_ballot;
        if (coro_id) *coro_id = __resp__.coro_id;
        return 0;
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
        auto __fu_result__ = __cl__->request(MultiPaxosService::DECIDE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.slot;
            __m__ << req.ballot;
            __m__ << req.cmd;
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
    [[deprecated("use typed async_Decide(const RpcDecideRequest&) instead")]]
    rrr::FutureResult async_Decide(const uint64_t& slot, const ballot_t& ballot, const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcDecideRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_Decide(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Decide(const RpcDecideRequest&) instead")]]
    rrr::i32 Decide(const uint64_t& slot, const ballot_t& ballot, const MarshallDeputy& cmd) {
        RpcDecideRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.cmd = cmd;
        auto __typed_result__ = this->Decide(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
    class HeartbeatTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit HeartbeatTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcHeartbeatResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcHeartbeatResponse, rrr::i32>::Err(__ret__);
            }
            RpcHeartbeatResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.ballot;
            __fu__->get_reply() >> __typed_resp__.val;
            return rusty::Result<RpcHeartbeatResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<HeartbeatTypedFuture, rrr::i32> async_Heartbeat(const RpcHeartbeatRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::HEARTBEAT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<HeartbeatTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<HeartbeatTypedFuture, rrr::i32>::Ok(HeartbeatTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcHeartbeatResponse, rrr::i32> Heartbeat(const RpcHeartbeatRequest& req) {
        auto __typed_fu_result__ = this->async_Heartbeat(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcHeartbeatResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_Heartbeat(const RpcHeartbeatRequest&) instead")]]
    rrr::FutureResult async_Heartbeat(const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcHeartbeatRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_Heartbeat(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Heartbeat(const RpcHeartbeatRequest&) instead")]]
    rrr::i32 Heartbeat(const MarshallDeputy& cmd, rrr::i32* ballot, rrr::i32* val) {
        RpcHeartbeatRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->Heartbeat(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ballot) *ballot = __resp__.ballot;
        if (val) *val = __resp__.val;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ret_slot;
            __fu__->get_reply() >> __typed_resp__.ret_ballot;
            return rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ForwardToLearnerServerTypedFuture, rrr::i32> async_ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::FORWARDTOLEARNERSERVER, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.par_id;
            __m__ << req.slot;
            __m__ << req.ballot;
            __m__ << req.cmd;
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
    [[deprecated("use typed async_ForwardToLearnerServer(const RpcForwardToLearnerServerRequest&) instead")]]
    rrr::FutureResult async_ForwardToLearnerServer(const rrr::i32& par_id, const uint64_t& slot, const ballot_t& ballot, const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcForwardToLearnerServerRequest __req__;
        __req__.par_id = par_id;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_ForwardToLearnerServer(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed ForwardToLearnerServer(const RpcForwardToLearnerServerRequest&) instead")]]
    rrr::i32 ForwardToLearnerServer(const rrr::i32& par_id, const uint64_t& slot, const ballot_t& ballot, const MarshallDeputy& cmd, uint64_t* ret_slot, ballot_t* ret_ballot) {
        RpcForwardToLearnerServerRequest __req__;
        __req__.par_id = par_id;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.cmd = cmd;
        auto __typed_result__ = this->ForwardToLearnerServer(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ret_slot) *ret_slot = __resp__.ret_slot;
        if (ret_ballot) *ret_ballot = __resp__.ret_ballot;
        return 0;
    }
    class BulkPrepareTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit BulkPrepareTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcBulkPrepareResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcBulkPrepareResponse, rrr::i32>::Err(__ret__);
            }
            RpcBulkPrepareResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.ballot;
            __fu__->get_reply() >> __typed_resp__.val;
            return rusty::Result<RpcBulkPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkPrepareTypedFuture, rrr::i32> async_BulkPrepare(const RpcBulkPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKPREPARE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<BulkPrepareTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<BulkPrepareTypedFuture, rrr::i32>::Ok(BulkPrepareTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcBulkPrepareResponse, rrr::i32> BulkPrepare(const RpcBulkPrepareRequest& req) {
        auto __typed_fu_result__ = this->async_BulkPrepare(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcBulkPrepareResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_BulkPrepare(const RpcBulkPrepareRequest&) instead")]]
    rrr::FutureResult async_BulkPrepare(const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcBulkPrepareRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_BulkPrepare(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed BulkPrepare(const RpcBulkPrepareRequest&) instead")]]
    rrr::i32 BulkPrepare(const MarshallDeputy& cmd, rrr::i32* ballot, rrr::i32* val) {
        RpcBulkPrepareRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->BulkPrepare(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ballot) *ballot = __resp__.ballot;
        if (val) *val = __resp__.val;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ballot;
            __fu__->get_reply() >> __typed_resp__.val;
            return rusty::Result<RpcBulkAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkAcceptTypedFuture, rrr::i32> async_BulkAccept(const RpcBulkAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
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
    [[deprecated("use typed async_BulkAccept(const RpcBulkAcceptRequest&) instead")]]
    rrr::FutureResult async_BulkAccept(const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcBulkAcceptRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_BulkAccept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed BulkAccept(const RpcBulkAcceptRequest&) instead")]]
    rrr::i32 BulkAccept(const MarshallDeputy& cmd, rrr::i32* ballot, rrr::i32* val) {
        RpcBulkAcceptRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->BulkAccept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ballot) *ballot = __resp__.ballot;
        if (val) *val = __resp__.val;
        return 0;
    }
    class BulkPrepare2TypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit BulkPrepare2TypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcBulkPrepare2Response, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcBulkPrepare2Response, rrr::i32>::Err(__ret__);
            }
            RpcBulkPrepare2Response __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.ballot;
            __fu__->get_reply() >> __typed_resp__.val;
            __fu__->get_reply() >> __typed_resp__.ret;
            return rusty::Result<RpcBulkPrepare2Response, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkPrepare2TypedFuture, rrr::i32> async_BulkPrepare2(const RpcBulkPrepare2Request& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKPREPARE2, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<BulkPrepare2TypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<BulkPrepare2TypedFuture, rrr::i32>::Ok(BulkPrepare2TypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcBulkPrepare2Response, rrr::i32> BulkPrepare2(const RpcBulkPrepare2Request& req) {
        auto __typed_fu_result__ = this->async_BulkPrepare2(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcBulkPrepare2Response, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_BulkPrepare2(const RpcBulkPrepare2Request&) instead")]]
    rrr::FutureResult async_BulkPrepare2(const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcBulkPrepare2Request __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_BulkPrepare2(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed BulkPrepare2(const RpcBulkPrepare2Request&) instead")]]
    rrr::i32 BulkPrepare2(const MarshallDeputy& cmd, rrr::i32* ballot, rrr::i32* val, MarshallDeputy* ret) {
        RpcBulkPrepare2Request __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->BulkPrepare2(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ballot) *ballot = __resp__.ballot;
        if (val) *val = __resp__.val;
        if (ret) *ret = __resp__.ret;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ballot;
            __fu__->get_reply() >> __typed_resp__.val;
            __fu__->get_reply() >> __typed_resp__.ret;
            return rusty::Result<RpcSyncLogResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SyncLogTypedFuture, rrr::i32> async_SyncLog(const RpcSyncLogRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::SYNCLOG, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
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
    [[deprecated("use typed async_SyncLog(const RpcSyncLogRequest&) instead")]]
    rrr::FutureResult async_SyncLog(const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcSyncLogRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_SyncLog(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed SyncLog(const RpcSyncLogRequest&) instead")]]
    rrr::i32 SyncLog(const MarshallDeputy& cmd, rrr::i32* ballot, rrr::i32* val, MarshallDeputy* ret) {
        RpcSyncLogRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->SyncLog(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ballot) *ballot = __resp__.ballot;
        if (val) *val = __resp__.val;
        if (ret) *ret = __resp__.ret;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ballot;
            __fu__->get_reply() >> __typed_resp__.val;
            return rusty::Result<RpcSyncCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SyncCommitTypedFuture, rrr::i32> async_SyncCommit(const RpcSyncCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::SYNCCOMMIT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
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
    [[deprecated("use typed async_SyncCommit(const RpcSyncCommitRequest&) instead")]]
    rrr::FutureResult async_SyncCommit(const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcSyncCommitRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_SyncCommit(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed SyncCommit(const RpcSyncCommitRequest&) instead")]]
    rrr::i32 SyncCommit(const MarshallDeputy& cmd, rrr::i32* ballot, rrr::i32* val) {
        RpcSyncCommitRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->SyncCommit(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ballot) *ballot = __resp__.ballot;
        if (val) *val = __resp__.val;
        return 0;
    }
    class SyncNoOpsTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit SyncNoOpsTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcSyncNoOpsResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSyncNoOpsResponse, rrr::i32>::Err(__ret__);
            }
            RpcSyncNoOpsResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.ballot;
            __fu__->get_reply() >> __typed_resp__.val;
            return rusty::Result<RpcSyncNoOpsResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SyncNoOpsTypedFuture, rrr::i32> async_SyncNoOps(const RpcSyncNoOpsRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::SYNCNOOPS, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SyncNoOpsTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SyncNoOpsTypedFuture, rrr::i32>::Ok(SyncNoOpsTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSyncNoOpsResponse, rrr::i32> SyncNoOps(const RpcSyncNoOpsRequest& req) {
        auto __typed_fu_result__ = this->async_SyncNoOps(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSyncNoOpsResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_SyncNoOps(const RpcSyncNoOpsRequest&) instead")]]
    rrr::FutureResult async_SyncNoOps(const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcSyncNoOpsRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_SyncNoOps(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed SyncNoOps(const RpcSyncNoOpsRequest&) instead")]]
    rrr::i32 SyncNoOps(const MarshallDeputy& cmd, rrr::i32* ballot, rrr::i32* val) {
        RpcSyncNoOpsRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->SyncNoOps(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ballot) *ballot = __resp__.ballot;
        if (val) *val = __resp__.val;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ballot;
            __fu__->get_reply() >> __typed_resp__.val;
            return rusty::Result<RpcBulkDecideResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<BulkDecideTypedFuture, rrr::i32> async_BulkDecide(const RpcBulkDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MultiPaxosService::BULKDECIDE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
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
    [[deprecated("use typed async_BulkDecide(const RpcBulkDecideRequest&) instead")]]
    rrr::FutureResult async_BulkDecide(const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcBulkDecideRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_BulkDecide(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed BulkDecide(const RpcBulkDecideRequest&) instead")]]
    rrr::i32 BulkDecide(const MarshallDeputy& cmd, rrr::i32* ballot, rrr::i32* val) {
        RpcBulkDecideRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->BulkDecide(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ballot) *ballot = __resp__.ballot;
        if (val) *val = __resp__.val;
        return 0;
    }
};

class MongodbService : public rrr::Service {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcCommitRequest {
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCommitRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCommitRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcCommitResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCommitResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCommitResponse& o) {
        return m;
    }

    enum {
        COMMIT = 0x2a6ce678,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        int ret = 0;
        if ((ret = svr.reg_rpc(COMMIT, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(COMMIT);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
        switch (rpc_id) {
        case COMMIT: __Commit__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void Commit(const RpcCommitRequest& req, RpcCommitResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Commit__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcCommitRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcCommitResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->Commit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class MongodbProxy {
protected:
    rrr::Client* __cl__;
public:
    MongodbProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcCommitRequest = MongodbService::RpcCommitRequest;
    using RpcCommitResponse = MongodbService::RpcCommitResponse;
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
            return rusty::Result<RpcCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<CommitTypedFuture, rrr::i32> async_Commit(const RpcCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MongodbService::COMMIT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
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
    [[deprecated("use typed async_Commit(const RpcCommitRequest&) instead")]]
    rrr::FutureResult async_Commit(const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcCommitRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_Commit(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Commit(const RpcCommitRequest&) instead")]]
    rrr::i32 Commit(const MarshallDeputy& cmd) {
        RpcCommitRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->Commit(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
};

class MenciusService : public rrr::Service {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcPrepareRequest {
        uint64_t slot;
        ballot_t ballot;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPrepareRequest& o) {
        m << o.slot;
        m << o.ballot;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPrepareRequest& o) {
        m >> o.slot;
        m >> o.ballot;
        return m;
    }

    struct RpcPrepareResponse {
        ballot_t max_ballot;
        uint64_t coro_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPrepareResponse& o) {
        m << o.max_ballot;
        m << o.coro_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPrepareResponse& o) {
        m >> o.max_ballot;
        m >> o.coro_id;
        return m;
    }

    struct RpcSuggestRequest {
        uint64_t slot;
        uint64_t time;
        ballot_t ballot;
        uint64_t sender;
        std::vector<uint64_t> skip_commits;
        std::vector<uint64_t> skip_potentials;
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSuggestRequest& o) {
        m << o.slot;
        m << o.time;
        m << o.ballot;
        m << o.sender;
        m << o.skip_commits;
        m << o.skip_potentials;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSuggestRequest& o) {
        m >> o.slot;
        m >> o.time;
        m >> o.ballot;
        m >> o.sender;
        m >> o.skip_commits;
        m >> o.skip_potentials;
        m >> o.cmd;
        return m;
    }

    struct RpcSuggestResponse {
        ballot_t max_ballot;
        uint64_t coro_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSuggestResponse& o) {
        m << o.max_ballot;
        m << o.coro_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSuggestResponse& o) {
        m >> o.max_ballot;
        m >> o.coro_id;
        return m;
    }

    struct RpcDecideRequest {
        uint64_t slot;
        ballot_t ballot;
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDecideRequest& o) {
        m << o.slot;
        m << o.ballot;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDecideRequest& o) {
        m >> o.slot;
        m >> o.ballot;
        m >> o.cmd;
        return m;
    }

    struct RpcDecideResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDecideResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDecideResponse& o) {
        return m;
    }

    enum {
        PREPARE = 0x12807923,
        SUGGEST = 0x5311ef4c,
        DECIDE = 0x6b1a26b8,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        int ret = 0;
        if ((ret = svr.reg_rpc(PREPARE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SUGGEST, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(DECIDE, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(PREPARE);
        svr.unreg(SUGGEST);
        svr.unreg(DECIDE);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
        switch (rpc_id) {
        case PREPARE: __Prepare__wrapper__(std::move(req), weak_sconn); break;
        case SUGGEST: __Suggest__wrapper__(std::move(req), weak_sconn); break;
        case DECIDE: __Decide__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void Prepare(const RpcPrepareRequest& req, RpcPrepareResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Suggest(const RpcSuggestRequest& req, RpcSuggestResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Decide(const RpcDecideRequest& req, RpcDecideResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Prepare__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPrepareRequest __typed_req__;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            auto __typed_resp__ = std::make_shared<RpcPrepareResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->max_ballot;
                    m << __typed_resp__->coro_id;
                },
                []() {});
            this->Prepare(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Suggest__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSuggestRequest __typed_req__;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.time;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.sender;
            req->m >> __typed_req__.skip_commits;
            req->m >> __typed_req__.skip_potentials;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcSuggestResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->max_ballot;
                    m << __typed_resp__->coro_id;
                },
                []() {});
            this->Suggest(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Decide__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDecideRequest __typed_req__;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcDecideResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->Decide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class MenciusProxy {
protected:
    rrr::Client* __cl__;
public:
    MenciusProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcPrepareRequest = MenciusService::RpcPrepareRequest;
    using RpcPrepareResponse = MenciusService::RpcPrepareResponse;
    using RpcSuggestRequest = MenciusService::RpcSuggestRequest;
    using RpcSuggestResponse = MenciusService::RpcSuggestResponse;
    using RpcDecideRequest = MenciusService::RpcDecideRequest;
    using RpcDecideResponse = MenciusService::RpcDecideResponse;
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
            __fu__->get_reply() >> __typed_resp__.max_ballot;
            __fu__->get_reply() >> __typed_resp__.coro_id;
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<PrepareTypedFuture, rrr::i32> async_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MenciusService::PREPARE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.slot;
            __m__ << req.ballot;
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
    [[deprecated("use typed async_Prepare(const RpcPrepareRequest&) instead")]]
    rrr::FutureResult async_Prepare(const uint64_t& slot, const ballot_t& ballot, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcPrepareRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        auto __typed_result__ = this->async_Prepare(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Prepare(const RpcPrepareRequest&) instead")]]
    rrr::i32 Prepare(const uint64_t& slot, const ballot_t& ballot, ballot_t* max_ballot, uint64_t* coro_id) {
        RpcPrepareRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        auto __typed_result__ = this->Prepare(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (max_ballot) *max_ballot = __resp__.max_ballot;
        if (coro_id) *coro_id = __resp__.coro_id;
        return 0;
    }
    class SuggestTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit SuggestTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcSuggestResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSuggestResponse, rrr::i32>::Err(__ret__);
            }
            RpcSuggestResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.max_ballot;
            __fu__->get_reply() >> __typed_resp__.coro_id;
            return rusty::Result<RpcSuggestResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SuggestTypedFuture, rrr::i32> async_Suggest(const RpcSuggestRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MenciusService::SUGGEST, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.slot;
            __m__ << req.time;
            __m__ << req.ballot;
            __m__ << req.sender;
            __m__ << req.skip_commits;
            __m__ << req.skip_potentials;
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SuggestTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SuggestTypedFuture, rrr::i32>::Ok(SuggestTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSuggestResponse, rrr::i32> Suggest(const RpcSuggestRequest& req) {
        auto __typed_fu_result__ = this->async_Suggest(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSuggestResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_Suggest(const RpcSuggestRequest&) instead")]]
    rrr::FutureResult async_Suggest(const uint64_t& slot, const uint64_t& time, const ballot_t& ballot, const uint64_t& sender, const std::vector<uint64_t>& skip_commits, const std::vector<uint64_t>& skip_potentials, const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcSuggestRequest __req__;
        __req__.slot = slot;
        __req__.time = time;
        __req__.ballot = ballot;
        __req__.sender = sender;
        __req__.skip_commits = skip_commits;
        __req__.skip_potentials = skip_potentials;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_Suggest(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Suggest(const RpcSuggestRequest&) instead")]]
    rrr::i32 Suggest(const uint64_t& slot, const uint64_t& time, const ballot_t& ballot, const uint64_t& sender, const std::vector<uint64_t>& skip_commits, const std::vector<uint64_t>& skip_potentials, const MarshallDeputy& cmd, ballot_t* max_ballot, uint64_t* coro_id) {
        RpcSuggestRequest __req__;
        __req__.slot = slot;
        __req__.time = time;
        __req__.ballot = ballot;
        __req__.sender = sender;
        __req__.skip_commits = skip_commits;
        __req__.skip_potentials = skip_potentials;
        __req__.cmd = cmd;
        auto __typed_result__ = this->Suggest(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (max_ballot) *max_ballot = __resp__.max_ballot;
        if (coro_id) *coro_id = __resp__.coro_id;
        return 0;
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
        auto __fu_result__ = __cl__->request(MenciusService::DECIDE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.slot;
            __m__ << req.ballot;
            __m__ << req.cmd;
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
    [[deprecated("use typed async_Decide(const RpcDecideRequest&) instead")]]
    rrr::FutureResult async_Decide(const uint64_t& slot, const ballot_t& ballot, const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcDecideRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_Decide(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Decide(const RpcDecideRequest&) instead")]]
    rrr::i32 Decide(const uint64_t& slot, const ballot_t& ballot, const MarshallDeputy& cmd) {
        RpcDecideRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.cmd = cmd;
        auto __typed_result__ = this->Decide(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
};

class FpgaRaftService : public rrr::Service {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcHeartbeatRequest {
        uint64_t leaderPrevLogIndex;
        DepId dep_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcHeartbeatRequest& o) {
        m << o.leaderPrevLogIndex;
        m << o.dep_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcHeartbeatRequest& o) {
        m >> o.leaderPrevLogIndex;
        m >> o.dep_id;
        return m;
    }

    struct RpcHeartbeatResponse {
        uint64_t followerPrevLogIndex;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcHeartbeatResponse& o) {
        m << o.followerPrevLogIndex;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcHeartbeatResponse& o) {
        m >> o.followerPrevLogIndex;
        return m;
    }

    struct RpcForwardRequest {
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcForwardRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcForwardRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcForwardResponse {
        uint64_t cmt_idx;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcForwardResponse& o) {
        m << o.cmt_idx;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcForwardResponse& o) {
        m >> o.cmt_idx;
        return m;
    }

    struct RpcVoteRequest {
        uint64_t lst_log_idx;
        ballot_t lst_log_term;
        parid_t par_id;
        ballot_t cur_term;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcVoteRequest& o) {
        m << o.lst_log_idx;
        m << o.lst_log_term;
        m << o.par_id;
        m << o.cur_term;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcVoteRequest& o) {
        m >> o.lst_log_idx;
        m >> o.lst_log_term;
        m >> o.par_id;
        m >> o.cur_term;
        return m;
    }

    struct RpcVoteResponse {
        ballot_t max_ballot;
        bool_t vote_granted;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcVoteResponse& o) {
        m << o.max_ballot;
        m << o.vote_granted;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcVoteResponse& o) {
        m >> o.max_ballot;
        m >> o.vote_granted;
        return m;
    }

    struct RpcVote2FPGARequest {
        uint64_t lst_log_idx;
        ballot_t lst_log_term;
        parid_t par_id;
        ballot_t cur_term;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcVote2FPGARequest& o) {
        m << o.lst_log_idx;
        m << o.lst_log_term;
        m << o.par_id;
        m << o.cur_term;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcVote2FPGARequest& o) {
        m >> o.lst_log_idx;
        m >> o.lst_log_term;
        m >> o.par_id;
        m >> o.cur_term;
        return m;
    }

    struct RpcVote2FPGAResponse {
        ballot_t max_ballot;
        bool_t vote_granted;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcVote2FPGAResponse& o) {
        m << o.max_ballot;
        m << o.vote_granted;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcVote2FPGAResponse& o) {
        m >> o.max_ballot;
        m >> o.vote_granted;
        return m;
    }

    struct RpcAppendEntriesRequest {
        uint64_t slot;
        ballot_t ballot;
        uint64_t leaderCurrentTerm;
        uint64_t leaderPrevLogIndex;
        uint64_t leaderPrevLogTerm;
        uint64_t leaderCommitIndex;
        DepId dep_id;
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAppendEntriesRequest& o) {
        m << o.slot;
        m << o.ballot;
        m << o.leaderCurrentTerm;
        m << o.leaderPrevLogIndex;
        m << o.leaderPrevLogTerm;
        m << o.leaderCommitIndex;
        m << o.dep_id;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAppendEntriesRequest& o) {
        m >> o.slot;
        m >> o.ballot;
        m >> o.leaderCurrentTerm;
        m >> o.leaderPrevLogIndex;
        m >> o.leaderPrevLogTerm;
        m >> o.leaderCommitIndex;
        m >> o.dep_id;
        m >> o.cmd;
        return m;
    }

    struct RpcAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAppendEntriesResponse& o) {
        m << o.followerAppendOK;
        m << o.followerCurrentTerm;
        m << o.followerLastLogIndex;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAppendEntriesResponse& o) {
        m >> o.followerAppendOK;
        m >> o.followerCurrentTerm;
        m >> o.followerLastLogIndex;
        return m;
    }

    struct RpcAppendEntries2Request {
        uint64_t slot;
        ballot_t ballot;
        uint64_t leaderCurrentTerm;
        uint64_t leaderPrevLogIndex;
        uint64_t leaderPrevLogTerm;
        uint64_t leaderCommitIndex;
        DepId dep_id;
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAppendEntries2Request& o) {
        m << o.slot;
        m << o.ballot;
        m << o.leaderCurrentTerm;
        m << o.leaderPrevLogIndex;
        m << o.leaderPrevLogTerm;
        m << o.leaderCommitIndex;
        m << o.dep_id;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAppendEntries2Request& o) {
        m >> o.slot;
        m >> o.ballot;
        m >> o.leaderCurrentTerm;
        m >> o.leaderPrevLogIndex;
        m >> o.leaderPrevLogTerm;
        m >> o.leaderCommitIndex;
        m >> o.dep_id;
        m >> o.cmd;
        return m;
    }

    struct RpcAppendEntries2Response {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAppendEntries2Response& o) {
        m << o.followerAppendOK;
        m << o.followerCurrentTerm;
        m << o.followerLastLogIndex;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAppendEntries2Response& o) {
        m >> o.followerAppendOK;
        m >> o.followerCurrentTerm;
        m >> o.followerLastLogIndex;
        return m;
    }

    struct RpcDecideRequest {
        uint64_t slot;
        ballot_t ballot;
        DepId dep_id;
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDecideRequest& o) {
        m << o.slot;
        m << o.ballot;
        m << o.dep_id;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDecideRequest& o) {
        m >> o.slot;
        m >> o.ballot;
        m >> o.dep_id;
        m >> o.cmd;
        return m;
    }

    struct RpcDecideResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDecideResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDecideResponse& o) {
        return m;
    }

    enum {
        HEARTBEAT = 0x1fac8043,
        FORWARD = 0x41694497,
        VOTE = 0x52ce2808,
        VOTE2FPGA = 0x2134a8ea,
        APPENDENTRIES = 0x4052eadc,
        APPENDENTRIES2 = 0x20d8d7fa,
        DECIDE = 0x28480d53,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        int ret = 0;
        if ((ret = svr.reg_rpc(HEARTBEAT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(FORWARD, svc_index)) != 0) {
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
        svr.unreg(FORWARD);
        svr.unreg(VOTE);
        svr.unreg(VOTE2FPGA);
        svr.unreg(APPENDENTRIES);
        svr.unreg(APPENDENTRIES2);
        svr.unreg(DECIDE);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
        switch (rpc_id) {
        case HEARTBEAT: __Heartbeat__wrapper__(std::move(req), weak_sconn); break;
        case FORWARD: __Forward__wrapper__(std::move(req), weak_sconn); break;
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
    virtual void Heartbeat(const RpcHeartbeatRequest& req, RpcHeartbeatResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Forward(const RpcForwardRequest& req, RpcForwardResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Vote(const RpcVoteRequest& req, RpcVoteResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Vote2FPGA(const RpcVote2FPGARequest& req, RpcVote2FPGAResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void AppendEntries(const RpcAppendEntriesRequest& req, RpcAppendEntriesResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void AppendEntries2(const RpcAppendEntries2Request& req, RpcAppendEntries2Response& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Decide(const RpcDecideRequest& req, RpcDecideResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Heartbeat__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcHeartbeatRequest __typed_req__;
            req->m >> __typed_req__.leaderPrevLogIndex;
            req->m >> __typed_req__.dep_id;
            auto __typed_resp__ = std::make_shared<RpcHeartbeatResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->followerPrevLogIndex;
                },
                []() {});
            this->Heartbeat(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Forward__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcForwardRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcForwardResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->cmt_idx;
                },
                []() {});
            this->Forward(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Vote__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVoteRequest __typed_req__;
            req->m >> __typed_req__.lst_log_idx;
            req->m >> __typed_req__.lst_log_term;
            req->m >> __typed_req__.par_id;
            req->m >> __typed_req__.cur_term;
            auto __typed_resp__ = std::make_shared<RpcVoteResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->max_ballot;
                    m << __typed_resp__->vote_granted;
                },
                []() {});
            this->Vote(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Vote2FPGA__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVote2FPGARequest __typed_req__;
            req->m >> __typed_req__.lst_log_idx;
            req->m >> __typed_req__.lst_log_term;
            req->m >> __typed_req__.par_id;
            req->m >> __typed_req__.cur_term;
            auto __typed_resp__ = std::make_shared<RpcVote2FPGAResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->max_ballot;
                    m << __typed_resp__->vote_granted;
                },
                []() {});
            this->Vote2FPGA(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __AppendEntries__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntriesRequest __typed_req__;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.leaderCurrentTerm;
            req->m >> __typed_req__.leaderPrevLogIndex;
            req->m >> __typed_req__.leaderPrevLogTerm;
            req->m >> __typed_req__.leaderCommitIndex;
            req->m >> __typed_req__.dep_id;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcAppendEntriesResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->followerAppendOK;
                    m << __typed_resp__->followerCurrentTerm;
                    m << __typed_resp__->followerLastLogIndex;
                },
                []() {});
            this->AppendEntries(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __AppendEntries2__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntries2Request __typed_req__;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.leaderCurrentTerm;
            req->m >> __typed_req__.leaderPrevLogIndex;
            req->m >> __typed_req__.leaderPrevLogTerm;
            req->m >> __typed_req__.leaderCommitIndex;
            req->m >> __typed_req__.dep_id;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcAppendEntries2Response>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->followerAppendOK;
                    m << __typed_resp__->followerCurrentTerm;
                    m << __typed_resp__->followerLastLogIndex;
                },
                []() {});
            this->AppendEntries2(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Decide__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDecideRequest __typed_req__;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.dep_id;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcDecideResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->Decide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class FpgaRaftProxy {
protected:
    rrr::Client* __cl__;
public:
    FpgaRaftProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcHeartbeatRequest = FpgaRaftService::RpcHeartbeatRequest;
    using RpcHeartbeatResponse = FpgaRaftService::RpcHeartbeatResponse;
    using RpcForwardRequest = FpgaRaftService::RpcForwardRequest;
    using RpcForwardResponse = FpgaRaftService::RpcForwardResponse;
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
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit HeartbeatTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcHeartbeatResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcHeartbeatResponse, rrr::i32>::Err(__ret__);
            }
            RpcHeartbeatResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.followerPrevLogIndex;
            return rusty::Result<RpcHeartbeatResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<HeartbeatTypedFuture, rrr::i32> async_Heartbeat(const RpcHeartbeatRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::HEARTBEAT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.leaderPrevLogIndex;
            __m__ << req.dep_id;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<HeartbeatTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<HeartbeatTypedFuture, rrr::i32>::Ok(HeartbeatTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcHeartbeatResponse, rrr::i32> Heartbeat(const RpcHeartbeatRequest& req) {
        auto __typed_fu_result__ = this->async_Heartbeat(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcHeartbeatResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_Heartbeat(const RpcHeartbeatRequest&) instead")]]
    rrr::FutureResult async_Heartbeat(const uint64_t& leaderPrevLogIndex, const DepId& dep_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcHeartbeatRequest __req__;
        __req__.leaderPrevLogIndex = leaderPrevLogIndex;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->async_Heartbeat(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Heartbeat(const RpcHeartbeatRequest&) instead")]]
    rrr::i32 Heartbeat(const uint64_t& leaderPrevLogIndex, const DepId& dep_id, uint64_t* followerPrevLogIndex) {
        RpcHeartbeatRequest __req__;
        __req__.leaderPrevLogIndex = leaderPrevLogIndex;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->Heartbeat(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (followerPrevLogIndex) *followerPrevLogIndex = __resp__.followerPrevLogIndex;
        return 0;
    }
    class ForwardTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit ForwardTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcForwardResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcForwardResponse, rrr::i32>::Err(__ret__);
            }
            RpcForwardResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.cmt_idx;
            return rusty::Result<RpcForwardResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ForwardTypedFuture, rrr::i32> async_Forward(const RpcForwardRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::FORWARD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<ForwardTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<ForwardTypedFuture, rrr::i32>::Ok(ForwardTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcForwardResponse, rrr::i32> Forward(const RpcForwardRequest& req) {
        auto __typed_fu_result__ = this->async_Forward(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcForwardResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_Forward(const RpcForwardRequest&) instead")]]
    rrr::FutureResult async_Forward(const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcForwardRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_Forward(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Forward(const RpcForwardRequest&) instead")]]
    rrr::i32 Forward(const MarshallDeputy& cmd, uint64_t* cmt_idx) {
        RpcForwardRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->Forward(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (cmt_idx) *cmt_idx = __resp__.cmt_idx;
        return 0;
    }
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
            __fu__->get_reply() >> __typed_resp__.max_ballot;
            __fu__->get_reply() >> __typed_resp__.vote_granted;
            return rusty::Result<RpcVoteResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<VoteTypedFuture, rrr::i32> async_Vote(const RpcVoteRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::VOTE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.lst_log_idx;
            __m__ << req.lst_log_term;
            __m__ << req.par_id;
            __m__ << req.cur_term;
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
    [[deprecated("use typed async_Vote(const RpcVoteRequest&) instead")]]
    rrr::FutureResult async_Vote(const uint64_t& lst_log_idx, const ballot_t& lst_log_term, const parid_t& par_id, const ballot_t& cur_term, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcVoteRequest __req__;
        __req__.lst_log_idx = lst_log_idx;
        __req__.lst_log_term = lst_log_term;
        __req__.par_id = par_id;
        __req__.cur_term = cur_term;
        auto __typed_result__ = this->async_Vote(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Vote(const RpcVoteRequest&) instead")]]
    rrr::i32 Vote(const uint64_t& lst_log_idx, const ballot_t& lst_log_term, const parid_t& par_id, const ballot_t& cur_term, ballot_t* max_ballot, bool_t* vote_granted) {
        RpcVoteRequest __req__;
        __req__.lst_log_idx = lst_log_idx;
        __req__.lst_log_term = lst_log_term;
        __req__.par_id = par_id;
        __req__.cur_term = cur_term;
        auto __typed_result__ = this->Vote(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (max_ballot) *max_ballot = __resp__.max_ballot;
        if (vote_granted) *vote_granted = __resp__.vote_granted;
        return 0;
    }
    class Vote2FPGATypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit Vote2FPGATypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcVote2FPGAResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcVote2FPGAResponse, rrr::i32>::Err(__ret__);
            }
            RpcVote2FPGAResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.max_ballot;
            __fu__->get_reply() >> __typed_resp__.vote_granted;
            return rusty::Result<RpcVote2FPGAResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<Vote2FPGATypedFuture, rrr::i32> async_Vote2FPGA(const RpcVote2FPGARequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::VOTE2FPGA, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.lst_log_idx;
            __m__ << req.lst_log_term;
            __m__ << req.par_id;
            __m__ << req.cur_term;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<Vote2FPGATypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<Vote2FPGATypedFuture, rrr::i32>::Ok(Vote2FPGATypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcVote2FPGAResponse, rrr::i32> Vote2FPGA(const RpcVote2FPGARequest& req) {
        auto __typed_fu_result__ = this->async_Vote2FPGA(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVote2FPGAResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_Vote2FPGA(const RpcVote2FPGARequest&) instead")]]
    rrr::FutureResult async_Vote2FPGA(const uint64_t& lst_log_idx, const ballot_t& lst_log_term, const parid_t& par_id, const ballot_t& cur_term, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcVote2FPGARequest __req__;
        __req__.lst_log_idx = lst_log_idx;
        __req__.lst_log_term = lst_log_term;
        __req__.par_id = par_id;
        __req__.cur_term = cur_term;
        auto __typed_result__ = this->async_Vote2FPGA(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Vote2FPGA(const RpcVote2FPGARequest&) instead")]]
    rrr::i32 Vote2FPGA(const uint64_t& lst_log_idx, const ballot_t& lst_log_term, const parid_t& par_id, const ballot_t& cur_term, ballot_t* max_ballot, bool_t* vote_granted) {
        RpcVote2FPGARequest __req__;
        __req__.lst_log_idx = lst_log_idx;
        __req__.lst_log_term = lst_log_term;
        __req__.par_id = par_id;
        __req__.cur_term = cur_term;
        auto __typed_result__ = this->Vote2FPGA(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (max_ballot) *max_ballot = __resp__.max_ballot;
        if (vote_granted) *vote_granted = __resp__.vote_granted;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.followerAppendOK;
            __fu__->get_reply() >> __typed_resp__.followerCurrentTerm;
            __fu__->get_reply() >> __typed_resp__.followerLastLogIndex;
            return rusty::Result<RpcAppendEntriesResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntriesTypedFuture, rrr::i32> async_AppendEntries(const RpcAppendEntriesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::APPENDENTRIES, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.slot;
            __m__ << req.ballot;
            __m__ << req.leaderCurrentTerm;
            __m__ << req.leaderPrevLogIndex;
            __m__ << req.leaderPrevLogTerm;
            __m__ << req.leaderCommitIndex;
            __m__ << req.dep_id;
            __m__ << req.cmd;
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
    [[deprecated("use typed async_AppendEntries(const RpcAppendEntriesRequest&) instead")]]
    rrr::FutureResult async_AppendEntries(const uint64_t& slot, const ballot_t& ballot, const uint64_t& leaderCurrentTerm, const uint64_t& leaderPrevLogIndex, const uint64_t& leaderPrevLogTerm, const uint64_t& leaderCommitIndex, const DepId& dep_id, const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcAppendEntriesRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.leaderCurrentTerm = leaderCurrentTerm;
        __req__.leaderPrevLogIndex = leaderPrevLogIndex;
        __req__.leaderPrevLogTerm = leaderPrevLogTerm;
        __req__.leaderCommitIndex = leaderCommitIndex;
        __req__.dep_id = dep_id;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_AppendEntries(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed AppendEntries(const RpcAppendEntriesRequest&) instead")]]
    rrr::i32 AppendEntries(const uint64_t& slot, const ballot_t& ballot, const uint64_t& leaderCurrentTerm, const uint64_t& leaderPrevLogIndex, const uint64_t& leaderPrevLogTerm, const uint64_t& leaderCommitIndex, const DepId& dep_id, const MarshallDeputy& cmd, uint64_t* followerAppendOK, uint64_t* followerCurrentTerm, uint64_t* followerLastLogIndex) {
        RpcAppendEntriesRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.leaderCurrentTerm = leaderCurrentTerm;
        __req__.leaderPrevLogIndex = leaderPrevLogIndex;
        __req__.leaderPrevLogTerm = leaderPrevLogTerm;
        __req__.leaderCommitIndex = leaderCommitIndex;
        __req__.dep_id = dep_id;
        __req__.cmd = cmd;
        auto __typed_result__ = this->AppendEntries(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (followerAppendOK) *followerAppendOK = __resp__.followerAppendOK;
        if (followerCurrentTerm) *followerCurrentTerm = __resp__.followerCurrentTerm;
        if (followerLastLogIndex) *followerLastLogIndex = __resp__.followerLastLogIndex;
        return 0;
    }
    class AppendEntries2TypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit AppendEntries2TypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcAppendEntries2Response, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAppendEntries2Response, rrr::i32>::Err(__ret__);
            }
            RpcAppendEntries2Response __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.followerAppendOK;
            __fu__->get_reply() >> __typed_resp__.followerCurrentTerm;
            __fu__->get_reply() >> __typed_resp__.followerLastLogIndex;
            return rusty::Result<RpcAppendEntries2Response, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntries2TypedFuture, rrr::i32> async_AppendEntries2(const RpcAppendEntries2Request& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::APPENDENTRIES2, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.slot;
            __m__ << req.ballot;
            __m__ << req.leaderCurrentTerm;
            __m__ << req.leaderPrevLogIndex;
            __m__ << req.leaderPrevLogTerm;
            __m__ << req.leaderCommitIndex;
            __m__ << req.dep_id;
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntries2TypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntries2TypedFuture, rrr::i32>::Ok(AppendEntries2TypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAppendEntries2Response, rrr::i32> AppendEntries2(const RpcAppendEntries2Request& req) {
        auto __typed_fu_result__ = this->async_AppendEntries2(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntries2Response, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_AppendEntries2(const RpcAppendEntries2Request&) instead")]]
    rrr::FutureResult async_AppendEntries2(const uint64_t& slot, const ballot_t& ballot, const uint64_t& leaderCurrentTerm, const uint64_t& leaderPrevLogIndex, const uint64_t& leaderPrevLogTerm, const uint64_t& leaderCommitIndex, const DepId& dep_id, const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcAppendEntries2Request __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.leaderCurrentTerm = leaderCurrentTerm;
        __req__.leaderPrevLogIndex = leaderPrevLogIndex;
        __req__.leaderPrevLogTerm = leaderPrevLogTerm;
        __req__.leaderCommitIndex = leaderCommitIndex;
        __req__.dep_id = dep_id;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_AppendEntries2(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed AppendEntries2(const RpcAppendEntries2Request&) instead")]]
    rrr::i32 AppendEntries2(const uint64_t& slot, const ballot_t& ballot, const uint64_t& leaderCurrentTerm, const uint64_t& leaderPrevLogIndex, const uint64_t& leaderPrevLogTerm, const uint64_t& leaderCommitIndex, const DepId& dep_id, const MarshallDeputy& cmd, uint64_t* followerAppendOK, uint64_t* followerCurrentTerm, uint64_t* followerLastLogIndex) {
        RpcAppendEntries2Request __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.leaderCurrentTerm = leaderCurrentTerm;
        __req__.leaderPrevLogIndex = leaderPrevLogIndex;
        __req__.leaderPrevLogTerm = leaderPrevLogTerm;
        __req__.leaderCommitIndex = leaderCommitIndex;
        __req__.dep_id = dep_id;
        __req__.cmd = cmd;
        auto __typed_result__ = this->AppendEntries2(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (followerAppendOK) *followerAppendOK = __resp__.followerAppendOK;
        if (followerCurrentTerm) *followerCurrentTerm = __resp__.followerCurrentTerm;
        if (followerLastLogIndex) *followerLastLogIndex = __resp__.followerLastLogIndex;
        return 0;
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
        auto __fu_result__ = __cl__->request(FpgaRaftService::DECIDE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.slot;
            __m__ << req.ballot;
            __m__ << req.dep_id;
            __m__ << req.cmd;
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
    [[deprecated("use typed async_Decide(const RpcDecideRequest&) instead")]]
    rrr::FutureResult async_Decide(const uint64_t& slot, const ballot_t& ballot, const DepId& dep_id, const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcDecideRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.dep_id = dep_id;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_Decide(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Decide(const RpcDecideRequest&) instead")]]
    rrr::i32 Decide(const uint64_t& slot, const ballot_t& ballot, const DepId& dep_id, const MarshallDeputy& cmd) {
        RpcDecideRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.dep_id = dep_id;
        __req__.cmd = cmd;
        auto __typed_result__ = this->Decide(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
};

class RaftService : public rrr::Service {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcVoteRequest {
        uint64_t lst_log_idx;
        ballot_t lst_log_term;
        siteid_t site_id;
        ballot_t cur_term;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcVoteRequest& o) {
        m << o.lst_log_idx;
        m << o.lst_log_term;
        m << o.site_id;
        m << o.cur_term;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcVoteRequest& o) {
        m >> o.lst_log_idx;
        m >> o.lst_log_term;
        m >> o.site_id;
        m >> o.cur_term;
        return m;
    }

    struct RpcVoteResponse {
        ballot_t max_ballot;
        bool_t vote_granted;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcVoteResponse& o) {
        m << o.max_ballot;
        m << o.vote_granted;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcVoteResponse& o) {
        m >> o.max_ballot;
        m >> o.vote_granted;
        return m;
    }

    struct RpcVoteDurableRequest {
        ballot_t term;
        siteid_t voter_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcVoteDurableRequest& o) {
        m << o.term;
        m << o.voter_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcVoteDurableRequest& o) {
        m >> o.term;
        m >> o.voter_id;
        return m;
    }

    struct RpcVoteDurableResponse {
        bool_t acknowledged;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcVoteDurableResponse& o) {
        m << o.acknowledged;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcVoteDurableResponse& o) {
        m >> o.acknowledged;
        return m;
    }

    struct RpcAppendEntriesRequest {
        uint64_t slot;
        ballot_t ballot;
        uint64_t leaderCurrentTerm;
        siteid_t leaderSiteId;
        uint64_t leaderPrevLogIndex;
        uint64_t leaderPrevLogTerm;
        uint64_t leaderCommitIndex;
        MarshallDeputy cmd;
        uint64_t leaderNextLogTerm;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAppendEntriesRequest& o) {
        m << o.slot;
        m << o.ballot;
        m << o.leaderCurrentTerm;
        m << o.leaderSiteId;
        m << o.leaderPrevLogIndex;
        m << o.leaderPrevLogTerm;
        m << o.leaderCommitIndex;
        m << o.cmd;
        m << o.leaderNextLogTerm;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAppendEntriesRequest& o) {
        m >> o.slot;
        m >> o.ballot;
        m >> o.leaderCurrentTerm;
        m >> o.leaderSiteId;
        m >> o.leaderPrevLogIndex;
        m >> o.leaderPrevLogTerm;
        m >> o.leaderCommitIndex;
        m >> o.cmd;
        m >> o.leaderNextLogTerm;
        return m;
    }

    struct RpcAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
        uint64_t followerAckType;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAppendEntriesResponse& o) {
        m << o.followerAppendOK;
        m << o.followerCurrentTerm;
        m << o.followerLastLogIndex;
        m << o.followerAckType;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAppendEntriesResponse& o) {
        m >> o.followerAppendOK;
        m >> o.followerCurrentTerm;
        m >> o.followerLastLogIndex;
        m >> o.followerAckType;
        return m;
    }

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
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcEmptyAppendEntriesRequest& o) {
        m << o.slot;
        m << o.ballot;
        m << o.leaderCurrentTerm;
        m << o.leaderSiteId;
        m << o.leaderPrevLogIndex;
        m << o.leaderPrevLogTerm;
        m << o.leaderCommitIndex;
        m << o.trigger_election_now;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcEmptyAppendEntriesRequest& o) {
        m >> o.slot;
        m >> o.ballot;
        m >> o.leaderCurrentTerm;
        m >> o.leaderSiteId;
        m >> o.leaderPrevLogIndex;
        m >> o.leaderPrevLogTerm;
        m >> o.leaderCommitIndex;
        m >> o.trigger_election_now;
        return m;
    }

    struct RpcEmptyAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
        uint64_t followerAckType;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcEmptyAppendEntriesResponse& o) {
        m << o.followerAppendOK;
        m << o.followerCurrentTerm;
        m << o.followerLastLogIndex;
        m << o.followerAckType;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcEmptyAppendEntriesResponse& o) {
        m >> o.followerAppendOK;
        m >> o.followerCurrentTerm;
        m >> o.followerLastLogIndex;
        m >> o.followerAckType;
        return m;
    }

    struct RpcAppendEntriesDurableRequest {
        ballot_t term;
        siteid_t follower_id;
        uint64_t lastLogIndex;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAppendEntriesDurableRequest& o) {
        m << o.term;
        m << o.follower_id;
        m << o.lastLogIndex;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAppendEntriesDurableRequest& o) {
        m >> o.term;
        m >> o.follower_id;
        m >> o.lastLogIndex;
        return m;
    }

    struct RpcAppendEntriesDurableResponse {
        bool_t acknowledged;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAppendEntriesDurableResponse& o) {
        m << o.acknowledged;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAppendEntriesDurableResponse& o) {
        m >> o.acknowledged;
        return m;
    }

    struct RpcTimeoutNowRequest {
        uint64_t leaderTerm;
        siteid_t leaderSiteId;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTimeoutNowRequest& o) {
        m << o.leaderTerm;
        m << o.leaderSiteId;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTimeoutNowRequest& o) {
        m >> o.leaderTerm;
        m >> o.leaderSiteId;
        return m;
    }

    struct RpcTimeoutNowResponse {
        uint64_t followerTerm;
        bool_t success;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTimeoutNowResponse& o) {
        m << o.followerTerm;
        m << o.success;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTimeoutNowResponse& o) {
        m >> o.followerTerm;
        m >> o.success;
        return m;
    }

    struct RpcNotifyRestartRequest {
        siteid_t restartedSiteId;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcNotifyRestartRequest& o) {
        m << o.restartedSiteId;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcNotifyRestartRequest& o) {
        m >> o.restartedSiteId;
        return m;
    }

    struct RpcNotifyRestartResponse {
        bool_t acknowledged;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcNotifyRestartResponse& o) {
        m << o.acknowledged;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcNotifyRestartResponse& o) {
        m >> o.acknowledged;
        return m;
    }

    struct RpcInstallSnapshotRequest {
        uint64_t term;
        uint64_t leader_id;
        uint64_t last_included_index;
        uint64_t last_included_term;
        std::string data;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcInstallSnapshotRequest& o) {
        m << o.term;
        m << o.leader_id;
        m << o.last_included_index;
        m << o.last_included_term;
        m << o.data;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcInstallSnapshotRequest& o) {
        m >> o.term;
        m >> o.leader_id;
        m >> o.last_included_index;
        m >> o.last_included_term;
        m >> o.data;
        return m;
    }

    struct RpcInstallSnapshotResponse {
        uint64_t term_out;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcInstallSnapshotResponse& o) {
        m << o.term_out;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcInstallSnapshotResponse& o) {
        m >> o.term_out;
        return m;
    }

    struct RpcAddServerRequest {
        uint64_t term;
        uint64_t new_server_id;
        std::string new_server_addr;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAddServerRequest& o) {
        m << o.term;
        m << o.new_server_id;
        m << o.new_server_addr;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAddServerRequest& o) {
        m >> o.term;
        m >> o.new_server_id;
        m >> o.new_server_addr;
        return m;
    }

    struct RpcAddServerResponse {
        bool_t success;
        std::string error_msg;
        uint64_t leader_hint;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAddServerResponse& o) {
        m << o.success;
        m << o.error_msg;
        m << o.leader_hint;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAddServerResponse& o) {
        m >> o.success;
        m >> o.error_msg;
        m >> o.leader_hint;
        return m;
    }

    struct RpcRemoveServerRequest {
        uint64_t term;
        uint64_t server_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRemoveServerRequest& o) {
        m << o.term;
        m << o.server_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRemoveServerRequest& o) {
        m >> o.term;
        m >> o.server_id;
        return m;
    }

    struct RpcRemoveServerResponse {
        bool_t success;
        std::string error_msg;
        uint64_t leader_hint;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRemoveServerResponse& o) {
        m << o.success;
        m << o.error_msg;
        m << o.leader_hint;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRemoveServerResponse& o) {
        m >> o.success;
        m >> o.error_msg;
        m >> o.leader_hint;
        return m;
    }

    enum {
        VOTE = 0x52b2e43a,
        VOTEDURABLE = 0x21bbfb8d,
        APPENDENTRIES = 0x20d19e65,
        EMPTYAPPENDENTRIES = 0x17a93bf6,
        APPENDENTRIESDURABLE = 0x4d7146e9,
        TIMEOUTNOW = 0x66a30cfc,
        NOTIFYRESTART = 0x31419829,
        INSTALLSNAPSHOT = 0x2235bce7,
        ADDSERVER = 0x66d3f46a,
        REMOVESERVER = 0x5c0cab14,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
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
    virtual void Vote(const RpcVoteRequest& req, RpcVoteResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void VoteDurable(const RpcVoteDurableRequest& req, RpcVoteDurableResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void AppendEntries(const RpcAppendEntriesRequest& req, RpcAppendEntriesResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req, RpcEmptyAppendEntriesResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req, RpcAppendEntriesDurableResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void TimeoutNow(const RpcTimeoutNowRequest& req, RpcTimeoutNowResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void NotifyRestart(const RpcNotifyRestartRequest& req, RpcNotifyRestartResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void InstallSnapshot(const RpcInstallSnapshotRequest& req, RpcInstallSnapshotResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void AddServer(const RpcAddServerRequest& req, RpcAddServerResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void RemoveServer(const RpcRemoveServerRequest& req, RpcRemoveServerResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Vote__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVoteRequest __typed_req__;
            req->m >> __typed_req__.lst_log_idx;
            req->m >> __typed_req__.lst_log_term;
            req->m >> __typed_req__.site_id;
            req->m >> __typed_req__.cur_term;
            auto __typed_resp__ = std::make_shared<RpcVoteResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->max_ballot;
                    m << __typed_resp__->vote_granted;
                },
                []() {});
            this->Vote(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __VoteDurable__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVoteDurableRequest __typed_req__;
            req->m >> __typed_req__.term;
            req->m >> __typed_req__.voter_id;
            auto __typed_resp__ = std::make_shared<RpcVoteDurableResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->acknowledged;
                },
                []() {});
            this->VoteDurable(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __AppendEntries__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntriesRequest __typed_req__;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.leaderCurrentTerm;
            req->m >> __typed_req__.leaderSiteId;
            req->m >> __typed_req__.leaderPrevLogIndex;
            req->m >> __typed_req__.leaderPrevLogTerm;
            req->m >> __typed_req__.leaderCommitIndex;
            req->m >> __typed_req__.cmd;
            req->m >> __typed_req__.leaderNextLogTerm;
            auto __typed_resp__ = std::make_shared<RpcAppendEntriesResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->followerAppendOK;
                    m << __typed_resp__->followerCurrentTerm;
                    m << __typed_resp__->followerLastLogIndex;
                    m << __typed_resp__->followerAckType;
                },
                []() {});
            this->AppendEntries(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __EmptyAppendEntries__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcEmptyAppendEntriesRequest __typed_req__;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.leaderCurrentTerm;
            req->m >> __typed_req__.leaderSiteId;
            req->m >> __typed_req__.leaderPrevLogIndex;
            req->m >> __typed_req__.leaderPrevLogTerm;
            req->m >> __typed_req__.leaderCommitIndex;
            req->m >> __typed_req__.trigger_election_now;
            auto __typed_resp__ = std::make_shared<RpcEmptyAppendEntriesResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->followerAppendOK;
                    m << __typed_resp__->followerCurrentTerm;
                    m << __typed_resp__->followerLastLogIndex;
                    m << __typed_resp__->followerAckType;
                },
                []() {});
            this->EmptyAppendEntries(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __AppendEntriesDurable__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAppendEntriesDurableRequest __typed_req__;
            req->m >> __typed_req__.term;
            req->m >> __typed_req__.follower_id;
            req->m >> __typed_req__.lastLogIndex;
            auto __typed_resp__ = std::make_shared<RpcAppendEntriesDurableResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->acknowledged;
                },
                []() {});
            this->AppendEntriesDurable(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __TimeoutNow__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTimeoutNowRequest __typed_req__;
            req->m >> __typed_req__.leaderTerm;
            req->m >> __typed_req__.leaderSiteId;
            auto __typed_resp__ = std::make_shared<RpcTimeoutNowResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->followerTerm;
                    m << __typed_resp__->success;
                },
                []() {});
            this->TimeoutNow(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __NotifyRestart__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcNotifyRestartRequest __typed_req__;
            req->m >> __typed_req__.restartedSiteId;
            auto __typed_resp__ = std::make_shared<RpcNotifyRestartResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->acknowledged;
                },
                []() {});
            this->NotifyRestart(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __InstallSnapshot__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcInstallSnapshotRequest __typed_req__;
            req->m >> __typed_req__.term;
            req->m >> __typed_req__.leader_id;
            req->m >> __typed_req__.last_included_index;
            req->m >> __typed_req__.last_included_term;
            req->m >> __typed_req__.data;
            auto __typed_resp__ = std::make_shared<RpcInstallSnapshotResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->term_out;
                },
                []() {});
            this->InstallSnapshot(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __AddServer__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAddServerRequest __typed_req__;
            req->m >> __typed_req__.term;
            req->m >> __typed_req__.new_server_id;
            req->m >> __typed_req__.new_server_addr;
            auto __typed_resp__ = std::make_shared<RpcAddServerResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->success;
                    m << __typed_resp__->error_msg;
                    m << __typed_resp__->leader_hint;
                },
                []() {});
            this->AddServer(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RemoveServer__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRemoveServerRequest __typed_req__;
            req->m >> __typed_req__.term;
            req->m >> __typed_req__.server_id;
            auto __typed_resp__ = std::make_shared<RpcRemoveServerResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->success;
                    m << __typed_resp__->error_msg;
                    m << __typed_resp__->leader_hint;
                },
                []() {});
            this->RemoveServer(__typed_req__, *__typed_resp__, std::move(__defer__));
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
            __fu__->get_reply() >> __typed_resp__.max_ballot;
            __fu__->get_reply() >> __typed_resp__.vote_granted;
            return rusty::Result<RpcVoteResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<VoteTypedFuture, rrr::i32> async_Vote(const RpcVoteRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::VOTE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.lst_log_idx;
            __m__ << req.lst_log_term;
            __m__ << req.site_id;
            __m__ << req.cur_term;
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
    [[deprecated("use typed async_Vote(const RpcVoteRequest&) instead")]]
    rrr::FutureResult async_Vote(const uint64_t& lst_log_idx, const ballot_t& lst_log_term, const siteid_t& site_id, const ballot_t& cur_term, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcVoteRequest __req__;
        __req__.lst_log_idx = lst_log_idx;
        __req__.lst_log_term = lst_log_term;
        __req__.site_id = site_id;
        __req__.cur_term = cur_term;
        auto __typed_result__ = this->async_Vote(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Vote(const RpcVoteRequest&) instead")]]
    rrr::i32 Vote(const uint64_t& lst_log_idx, const ballot_t& lst_log_term, const siteid_t& site_id, const ballot_t& cur_term, ballot_t* max_ballot, bool_t* vote_granted) {
        RpcVoteRequest __req__;
        __req__.lst_log_idx = lst_log_idx;
        __req__.lst_log_term = lst_log_term;
        __req__.site_id = site_id;
        __req__.cur_term = cur_term;
        auto __typed_result__ = this->Vote(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (max_ballot) *max_ballot = __resp__.max_ballot;
        if (vote_granted) *vote_granted = __resp__.vote_granted;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.acknowledged;
            return rusty::Result<RpcVoteDurableResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<VoteDurableTypedFuture, rrr::i32> async_VoteDurable(const RpcVoteDurableRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::VOTEDURABLE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.term;
            __m__ << req.voter_id;
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
    [[deprecated("use typed async_VoteDurable(const RpcVoteDurableRequest&) instead")]]
    rrr::FutureResult async_VoteDurable(const ballot_t& term, const siteid_t& voter_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcVoteDurableRequest __req__;
        __req__.term = term;
        __req__.voter_id = voter_id;
        auto __typed_result__ = this->async_VoteDurable(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed VoteDurable(const RpcVoteDurableRequest&) instead")]]
    rrr::i32 VoteDurable(const ballot_t& term, const siteid_t& voter_id, bool_t* acknowledged) {
        RpcVoteDurableRequest __req__;
        __req__.term = term;
        __req__.voter_id = voter_id;
        auto __typed_result__ = this->VoteDurable(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (acknowledged) *acknowledged = __resp__.acknowledged;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.followerAppendOK;
            __fu__->get_reply() >> __typed_resp__.followerCurrentTerm;
            __fu__->get_reply() >> __typed_resp__.followerLastLogIndex;
            __fu__->get_reply() >> __typed_resp__.followerAckType;
            return rusty::Result<RpcAppendEntriesResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntriesTypedFuture, rrr::i32> async_AppendEntries(const RpcAppendEntriesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::APPENDENTRIES, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.slot;
            __m__ << req.ballot;
            __m__ << req.leaderCurrentTerm;
            __m__ << req.leaderSiteId;
            __m__ << req.leaderPrevLogIndex;
            __m__ << req.leaderPrevLogTerm;
            __m__ << req.leaderCommitIndex;
            __m__ << req.cmd;
            __m__ << req.leaderNextLogTerm;
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
    [[deprecated("use typed async_AppendEntries(const RpcAppendEntriesRequest&) instead")]]
    rrr::FutureResult async_AppendEntries(const uint64_t& slot, const ballot_t& ballot, const uint64_t& leaderCurrentTerm, const siteid_t& leaderSiteId, const uint64_t& leaderPrevLogIndex, const uint64_t& leaderPrevLogTerm, const uint64_t& leaderCommitIndex, const MarshallDeputy& cmd, const uint64_t& leaderNextLogTerm, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcAppendEntriesRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.leaderCurrentTerm = leaderCurrentTerm;
        __req__.leaderSiteId = leaderSiteId;
        __req__.leaderPrevLogIndex = leaderPrevLogIndex;
        __req__.leaderPrevLogTerm = leaderPrevLogTerm;
        __req__.leaderCommitIndex = leaderCommitIndex;
        __req__.cmd = cmd;
        __req__.leaderNextLogTerm = leaderNextLogTerm;
        auto __typed_result__ = this->async_AppendEntries(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed AppendEntries(const RpcAppendEntriesRequest&) instead")]]
    rrr::i32 AppendEntries(const uint64_t& slot, const ballot_t& ballot, const uint64_t& leaderCurrentTerm, const siteid_t& leaderSiteId, const uint64_t& leaderPrevLogIndex, const uint64_t& leaderPrevLogTerm, const uint64_t& leaderCommitIndex, const MarshallDeputy& cmd, const uint64_t& leaderNextLogTerm, uint64_t* followerAppendOK, uint64_t* followerCurrentTerm, uint64_t* followerLastLogIndex, uint64_t* followerAckType) {
        RpcAppendEntriesRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.leaderCurrentTerm = leaderCurrentTerm;
        __req__.leaderSiteId = leaderSiteId;
        __req__.leaderPrevLogIndex = leaderPrevLogIndex;
        __req__.leaderPrevLogTerm = leaderPrevLogTerm;
        __req__.leaderCommitIndex = leaderCommitIndex;
        __req__.cmd = cmd;
        __req__.leaderNextLogTerm = leaderNextLogTerm;
        auto __typed_result__ = this->AppendEntries(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (followerAppendOK) *followerAppendOK = __resp__.followerAppendOK;
        if (followerCurrentTerm) *followerCurrentTerm = __resp__.followerCurrentTerm;
        if (followerLastLogIndex) *followerLastLogIndex = __resp__.followerLastLogIndex;
        if (followerAckType) *followerAckType = __resp__.followerAckType;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.followerAppendOK;
            __fu__->get_reply() >> __typed_resp__.followerCurrentTerm;
            __fu__->get_reply() >> __typed_resp__.followerLastLogIndex;
            __fu__->get_reply() >> __typed_resp__.followerAckType;
            return rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<EmptyAppendEntriesTypedFuture, rrr::i32> async_EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::EMPTYAPPENDENTRIES, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.slot;
            __m__ << req.ballot;
            __m__ << req.leaderCurrentTerm;
            __m__ << req.leaderSiteId;
            __m__ << req.leaderPrevLogIndex;
            __m__ << req.leaderPrevLogTerm;
            __m__ << req.leaderCommitIndex;
            __m__ << req.trigger_election_now;
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
    [[deprecated("use typed async_EmptyAppendEntries(const RpcEmptyAppendEntriesRequest&) instead")]]
    rrr::FutureResult async_EmptyAppendEntries(const uint64_t& slot, const ballot_t& ballot, const uint64_t& leaderCurrentTerm, const siteid_t& leaderSiteId, const uint64_t& leaderPrevLogIndex, const uint64_t& leaderPrevLogTerm, const uint64_t& leaderCommitIndex, const bool_t& trigger_election_now, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcEmptyAppendEntriesRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.leaderCurrentTerm = leaderCurrentTerm;
        __req__.leaderSiteId = leaderSiteId;
        __req__.leaderPrevLogIndex = leaderPrevLogIndex;
        __req__.leaderPrevLogTerm = leaderPrevLogTerm;
        __req__.leaderCommitIndex = leaderCommitIndex;
        __req__.trigger_election_now = trigger_election_now;
        auto __typed_result__ = this->async_EmptyAppendEntries(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed EmptyAppendEntries(const RpcEmptyAppendEntriesRequest&) instead")]]
    rrr::i32 EmptyAppendEntries(const uint64_t& slot, const ballot_t& ballot, const uint64_t& leaderCurrentTerm, const siteid_t& leaderSiteId, const uint64_t& leaderPrevLogIndex, const uint64_t& leaderPrevLogTerm, const uint64_t& leaderCommitIndex, const bool_t& trigger_election_now, uint64_t* followerAppendOK, uint64_t* followerCurrentTerm, uint64_t* followerLastLogIndex, uint64_t* followerAckType) {
        RpcEmptyAppendEntriesRequest __req__;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.leaderCurrentTerm = leaderCurrentTerm;
        __req__.leaderSiteId = leaderSiteId;
        __req__.leaderPrevLogIndex = leaderPrevLogIndex;
        __req__.leaderPrevLogTerm = leaderPrevLogTerm;
        __req__.leaderCommitIndex = leaderCommitIndex;
        __req__.trigger_election_now = trigger_election_now;
        auto __typed_result__ = this->EmptyAppendEntries(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (followerAppendOK) *followerAppendOK = __resp__.followerAppendOK;
        if (followerCurrentTerm) *followerCurrentTerm = __resp__.followerCurrentTerm;
        if (followerLastLogIndex) *followerLastLogIndex = __resp__.followerLastLogIndex;
        if (followerAckType) *followerAckType = __resp__.followerAckType;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.acknowledged;
            return rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AppendEntriesDurableTypedFuture, rrr::i32> async_AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::APPENDENTRIESDURABLE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.term;
            __m__ << req.follower_id;
            __m__ << req.lastLogIndex;
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
    [[deprecated("use typed async_AppendEntriesDurable(const RpcAppendEntriesDurableRequest&) instead")]]
    rrr::FutureResult async_AppendEntriesDurable(const ballot_t& term, const siteid_t& follower_id, const uint64_t& lastLogIndex, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcAppendEntriesDurableRequest __req__;
        __req__.term = term;
        __req__.follower_id = follower_id;
        __req__.lastLogIndex = lastLogIndex;
        auto __typed_result__ = this->async_AppendEntriesDurable(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed AppendEntriesDurable(const RpcAppendEntriesDurableRequest&) instead")]]
    rrr::i32 AppendEntriesDurable(const ballot_t& term, const siteid_t& follower_id, const uint64_t& lastLogIndex, bool_t* acknowledged) {
        RpcAppendEntriesDurableRequest __req__;
        __req__.term = term;
        __req__.follower_id = follower_id;
        __req__.lastLogIndex = lastLogIndex;
        auto __typed_result__ = this->AppendEntriesDurable(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (acknowledged) *acknowledged = __resp__.acknowledged;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.followerTerm;
            __fu__->get_reply() >> __typed_resp__.success;
            return rusty::Result<RpcTimeoutNowResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TimeoutNowTypedFuture, rrr::i32> async_TimeoutNow(const RpcTimeoutNowRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::TIMEOUTNOW, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.leaderTerm;
            __m__ << req.leaderSiteId;
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
    [[deprecated("use typed async_TimeoutNow(const RpcTimeoutNowRequest&) instead")]]
    rrr::FutureResult async_TimeoutNow(const uint64_t& leaderTerm, const siteid_t& leaderSiteId, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcTimeoutNowRequest __req__;
        __req__.leaderTerm = leaderTerm;
        __req__.leaderSiteId = leaderSiteId;
        auto __typed_result__ = this->async_TimeoutNow(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed TimeoutNow(const RpcTimeoutNowRequest&) instead")]]
    rrr::i32 TimeoutNow(const uint64_t& leaderTerm, const siteid_t& leaderSiteId, uint64_t* followerTerm, bool_t* success) {
        RpcTimeoutNowRequest __req__;
        __req__.leaderTerm = leaderTerm;
        __req__.leaderSiteId = leaderSiteId;
        auto __typed_result__ = this->TimeoutNow(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (followerTerm) *followerTerm = __resp__.followerTerm;
        if (success) *success = __resp__.success;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.acknowledged;
            return rusty::Result<RpcNotifyRestartResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<NotifyRestartTypedFuture, rrr::i32> async_NotifyRestart(const RpcNotifyRestartRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::NOTIFYRESTART, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.restartedSiteId;
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
    [[deprecated("use typed async_NotifyRestart(const RpcNotifyRestartRequest&) instead")]]
    rrr::FutureResult async_NotifyRestart(const siteid_t& restartedSiteId, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcNotifyRestartRequest __req__;
        __req__.restartedSiteId = restartedSiteId;
        auto __typed_result__ = this->async_NotifyRestart(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed NotifyRestart(const RpcNotifyRestartRequest&) instead")]]
    rrr::i32 NotifyRestart(const siteid_t& restartedSiteId, bool_t* acknowledged) {
        RpcNotifyRestartRequest __req__;
        __req__.restartedSiteId = restartedSiteId;
        auto __typed_result__ = this->NotifyRestart(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (acknowledged) *acknowledged = __resp__.acknowledged;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.term_out;
            return rusty::Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<InstallSnapshotTypedFuture, rrr::i32> async_InstallSnapshot(const RpcInstallSnapshotRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::INSTALLSNAPSHOT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.term;
            __m__ << req.leader_id;
            __m__ << req.last_included_index;
            __m__ << req.last_included_term;
            __m__ << req.data;
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
    [[deprecated("use typed async_InstallSnapshot(const RpcInstallSnapshotRequest&) instead")]]
    rrr::FutureResult async_InstallSnapshot(const uint64_t& term, const uint64_t& leader_id, const uint64_t& last_included_index, const uint64_t& last_included_term, const std::string& data, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcInstallSnapshotRequest __req__;
        __req__.term = term;
        __req__.leader_id = leader_id;
        __req__.last_included_index = last_included_index;
        __req__.last_included_term = last_included_term;
        __req__.data = data;
        auto __typed_result__ = this->async_InstallSnapshot(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed InstallSnapshot(const RpcInstallSnapshotRequest&) instead")]]
    rrr::i32 InstallSnapshot(const uint64_t& term, const uint64_t& leader_id, const uint64_t& last_included_index, const uint64_t& last_included_term, const std::string& data, uint64_t* term_out) {
        RpcInstallSnapshotRequest __req__;
        __req__.term = term;
        __req__.leader_id = leader_id;
        __req__.last_included_index = last_included_index;
        __req__.last_included_term = last_included_term;
        __req__.data = data;
        auto __typed_result__ = this->InstallSnapshot(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (term_out) *term_out = __resp__.term_out;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.success;
            __fu__->get_reply() >> __typed_resp__.error_msg;
            __fu__->get_reply() >> __typed_resp__.leader_hint;
            return rusty::Result<RpcAddServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AddServerTypedFuture, rrr::i32> async_AddServer(const RpcAddServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::ADDSERVER, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.term;
            __m__ << req.new_server_id;
            __m__ << req.new_server_addr;
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
    [[deprecated("use typed async_AddServer(const RpcAddServerRequest&) instead")]]
    rrr::FutureResult async_AddServer(const uint64_t& term, const uint64_t& new_server_id, const std::string& new_server_addr, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcAddServerRequest __req__;
        __req__.term = term;
        __req__.new_server_id = new_server_id;
        __req__.new_server_addr = new_server_addr;
        auto __typed_result__ = this->async_AddServer(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed AddServer(const RpcAddServerRequest&) instead")]]
    rrr::i32 AddServer(const uint64_t& term, const uint64_t& new_server_id, const std::string& new_server_addr, bool_t* success, std::string* error_msg, uint64_t* leader_hint) {
        RpcAddServerRequest __req__;
        __req__.term = term;
        __req__.new_server_id = new_server_id;
        __req__.new_server_addr = new_server_addr;
        auto __typed_result__ = this->AddServer(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (success) *success = __resp__.success;
        if (error_msg) *error_msg = __resp__.error_msg;
        if (leader_hint) *leader_hint = __resp__.leader_hint;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.success;
            __fu__->get_reply() >> __typed_resp__.error_msg;
            __fu__->get_reply() >> __typed_resp__.leader_hint;
            return rusty::Result<RpcRemoveServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RemoveServerTypedFuture, rrr::i32> async_RemoveServer(const RpcRemoveServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(RaftService::REMOVESERVER, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.term;
            __m__ << req.server_id;
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
    [[deprecated("use typed async_RemoveServer(const RpcRemoveServerRequest&) instead")]]
    rrr::FutureResult async_RemoveServer(const uint64_t& term, const uint64_t& server_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRemoveServerRequest __req__;
        __req__.term = term;
        __req__.server_id = server_id;
        auto __typed_result__ = this->async_RemoveServer(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed RemoveServer(const RpcRemoveServerRequest&) instead")]]
    rrr::i32 RemoveServer(const uint64_t& term, const uint64_t& server_id, bool_t* success, std::string* error_msg, uint64_t* leader_hint) {
        RpcRemoveServerRequest __req__;
        __req__.term = term;
        __req__.server_id = server_id;
        auto __typed_result__ = this->RemoveServer(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (success) *success = __resp__.success;
        if (error_msg) *error_msg = __resp__.error_msg;
        if (leader_hint) *leader_hint = __resp__.leader_hint;
        return 0;
    }
};

class CopilotService : public rrr::Service {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcForwardRequest {
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcForwardRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcForwardRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcForwardResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcForwardResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcForwardResponse& o) {
        return m;
    }

    struct RpcPrepareRequest {
        uint8_t is_pilot;
        uint64_t slot;
        ballot_t ballot;
        DepId dep_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPrepareRequest& o) {
        m << o.is_pilot;
        m << o.slot;
        m << o.ballot;
        m << o.dep_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPrepareRequest& o) {
        m >> o.is_pilot;
        m >> o.slot;
        m >> o.ballot;
        m >> o.dep_id;
        return m;
    }

    struct RpcPrepareResponse {
        MarshallDeputy ret_cmd;
        ballot_t max_ballot;
        uint64_t dep;
        status_t status;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPrepareResponse& o) {
        m << o.ret_cmd;
        m << o.max_ballot;
        m << o.dep;
        m << o.status;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPrepareResponse& o) {
        m >> o.ret_cmd;
        m >> o.max_ballot;
        m >> o.dep;
        m >> o.status;
        return m;
    }

    struct RpcFastAcceptRequest {
        uint8_t is_pilot;
        uint64_t slot;
        ballot_t ballot;
        uint64_t dep;
        MarshallDeputy cmd;
        DepId dep_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastAcceptRequest& o) {
        m << o.is_pilot;
        m << o.slot;
        m << o.ballot;
        m << o.dep;
        m << o.cmd;
        m << o.dep_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastAcceptRequest& o) {
        m >> o.is_pilot;
        m >> o.slot;
        m >> o.ballot;
        m >> o.dep;
        m >> o.cmd;
        m >> o.dep_id;
        return m;
    }

    struct RpcFastAcceptResponse {
        ballot_t max_ballot;
        uint64_t ret_dep;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFastAcceptResponse& o) {
        m << o.max_ballot;
        m << o.ret_dep;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFastAcceptResponse& o) {
        m >> o.max_ballot;
        m >> o.ret_dep;
        return m;
    }

    struct RpcAcceptRequest {
        uint8_t is_pilot;
        uint64_t slot;
        ballot_t ballot;
        uint64_t dep;
        MarshallDeputy cmd;
        DepId dep_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAcceptRequest& o) {
        m << o.is_pilot;
        m << o.slot;
        m << o.ballot;
        m << o.dep;
        m << o.cmd;
        m << o.dep_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAcceptRequest& o) {
        m >> o.is_pilot;
        m >> o.slot;
        m >> o.ballot;
        m >> o.dep;
        m >> o.cmd;
        m >> o.dep_id;
        return m;
    }

    struct RpcAcceptResponse {
        ballot_t max_ballot;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAcceptResponse& o) {
        m << o.max_ballot;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAcceptResponse& o) {
        m >> o.max_ballot;
        return m;
    }

    struct RpcCommitRequest {
        uint8_t is_pilot;
        uint64_t slot;
        uint64_t dep;
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCommitRequest& o) {
        m << o.is_pilot;
        m << o.slot;
        m << o.dep;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCommitRequest& o) {
        m >> o.is_pilot;
        m >> o.slot;
        m >> o.dep;
        m >> o.cmd;
        return m;
    }

    struct RpcCommitResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCommitResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCommitResponse& o) {
        return m;
    }

    enum {
        FORWARD = 0x1435bb3b,
        PREPARE = 0x30a4e7e5,
        FASTACCEPT = 0x64a97f91,
        ACCEPT = 0x37106b70,
        COMMIT = 0x19ac1ab1,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        int ret = 0;
        if ((ret = svr.reg_rpc(FORWARD, svc_index)) != 0) {
            goto err;
        }
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
        svr.unreg(FORWARD);
        svr.unreg(PREPARE);
        svr.unreg(FASTACCEPT);
        svr.unreg(ACCEPT);
        svr.unreg(COMMIT);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
        switch (rpc_id) {
        case FORWARD: __Forward__wrapper__(std::move(req), weak_sconn); break;
        case PREPARE: __Prepare__wrapper__(std::move(req), weak_sconn); break;
        case FASTACCEPT: __FastAccept__wrapper__(std::move(req), weak_sconn); break;
        case ACCEPT: __Accept__wrapper__(std::move(req), weak_sconn); break;
        case COMMIT: __Commit__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void Forward(const RpcForwardRequest& req, RpcForwardResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Prepare(const RpcPrepareRequest& req, RpcPrepareResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void FastAccept(const RpcFastAcceptRequest& req, RpcFastAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Accept(const RpcAcceptRequest& req, RpcAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void Commit(const RpcCommitRequest& req, RpcCommitResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __Forward__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcForwardRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcForwardResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->Forward(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Prepare__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPrepareRequest __typed_req__;
            req->m >> __typed_req__.is_pilot;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.dep_id;
            auto __typed_resp__ = std::make_shared<RpcPrepareResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ret_cmd;
                    m << __typed_resp__->max_ballot;
                    m << __typed_resp__->dep;
                    m << __typed_resp__->status;
                },
                []() {});
            this->Prepare(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __FastAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcFastAcceptRequest __typed_req__;
            req->m >> __typed_req__.is_pilot;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.dep;
            req->m >> __typed_req__.cmd;
            req->m >> __typed_req__.dep_id;
            auto __typed_resp__ = std::make_shared<RpcFastAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->max_ballot;
                    m << __typed_resp__->ret_dep;
                },
                []() {});
            this->FastAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Accept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAcceptRequest __typed_req__;
            req->m >> __typed_req__.is_pilot;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.dep;
            req->m >> __typed_req__.cmd;
            req->m >> __typed_req__.dep_id;
            auto __typed_resp__ = std::make_shared<RpcAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->max_ballot;
                },
                []() {});
            this->Accept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Commit__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcCommitRequest __typed_req__;
            req->m >> __typed_req__.is_pilot;
            req->m >> __typed_req__.slot;
            req->m >> __typed_req__.dep;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcCommitResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->Commit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class CopilotProxy {
protected:
    rrr::Client* __cl__;
public:
    CopilotProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcForwardRequest = CopilotService::RpcForwardRequest;
    using RpcForwardResponse = CopilotService::RpcForwardResponse;
    using RpcPrepareRequest = CopilotService::RpcPrepareRequest;
    using RpcPrepareResponse = CopilotService::RpcPrepareResponse;
    using RpcFastAcceptRequest = CopilotService::RpcFastAcceptRequest;
    using RpcFastAcceptResponse = CopilotService::RpcFastAcceptResponse;
    using RpcAcceptRequest = CopilotService::RpcAcceptRequest;
    using RpcAcceptResponse = CopilotService::RpcAcceptResponse;
    using RpcCommitRequest = CopilotService::RpcCommitRequest;
    using RpcCommitResponse = CopilotService::RpcCommitResponse;
    class ForwardTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit ForwardTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcForwardResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcForwardResponse, rrr::i32>::Err(__ret__);
            }
            RpcForwardResponse __typed_resp__;
            return rusty::Result<RpcForwardResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ForwardTypedFuture, rrr::i32> async_Forward(const RpcForwardRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::FORWARD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<ForwardTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<ForwardTypedFuture, rrr::i32>::Ok(ForwardTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcForwardResponse, rrr::i32> Forward(const RpcForwardRequest& req) {
        auto __typed_fu_result__ = this->async_Forward(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcForwardResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_Forward(const RpcForwardRequest&) instead")]]
    rrr::FutureResult async_Forward(const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcForwardRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_Forward(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Forward(const RpcForwardRequest&) instead")]]
    rrr::i32 Forward(const MarshallDeputy& cmd) {
        RpcForwardRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->Forward(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ret_cmd;
            __fu__->get_reply() >> __typed_resp__.max_ballot;
            __fu__->get_reply() >> __typed_resp__.dep;
            __fu__->get_reply() >> __typed_resp__.status;
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<PrepareTypedFuture, rrr::i32> async_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::PREPARE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.is_pilot;
            __m__ << req.slot;
            __m__ << req.ballot;
            __m__ << req.dep_id;
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
    [[deprecated("use typed async_Prepare(const RpcPrepareRequest&) instead")]]
    rrr::FutureResult async_Prepare(const uint8_t& is_pilot, const uint64_t& slot, const ballot_t& ballot, const DepId& dep_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcPrepareRequest __req__;
        __req__.is_pilot = is_pilot;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->async_Prepare(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Prepare(const RpcPrepareRequest&) instead")]]
    rrr::i32 Prepare(const uint8_t& is_pilot, const uint64_t& slot, const ballot_t& ballot, const DepId& dep_id, MarshallDeputy* ret_cmd, ballot_t* max_ballot, uint64_t* dep, status_t* status) {
        RpcPrepareRequest __req__;
        __req__.is_pilot = is_pilot;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->Prepare(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ret_cmd) *ret_cmd = __resp__.ret_cmd;
        if (max_ballot) *max_ballot = __resp__.max_ballot;
        if (dep) *dep = __resp__.dep;
        if (status) *status = __resp__.status;
        return 0;
    }
    class FastAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit FastAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcFastAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcFastAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcFastAcceptResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.max_ballot;
            __fu__->get_reply() >> __typed_resp__.ret_dep;
            return rusty::Result<RpcFastAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<FastAcceptTypedFuture, rrr::i32> async_FastAccept(const RpcFastAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::FASTACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.is_pilot;
            __m__ << req.slot;
            __m__ << req.ballot;
            __m__ << req.dep;
            __m__ << req.cmd;
            __m__ << req.dep_id;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<FastAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<FastAcceptTypedFuture, rrr::i32>::Ok(FastAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcFastAcceptResponse, rrr::i32> FastAccept(const RpcFastAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_FastAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_FastAccept(const RpcFastAcceptRequest&) instead")]]
    rrr::FutureResult async_FastAccept(const uint8_t& is_pilot, const uint64_t& slot, const ballot_t& ballot, const uint64_t& dep, const MarshallDeputy& cmd, const DepId& dep_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcFastAcceptRequest __req__;
        __req__.is_pilot = is_pilot;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.dep = dep;
        __req__.cmd = cmd;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->async_FastAccept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed FastAccept(const RpcFastAcceptRequest&) instead")]]
    rrr::i32 FastAccept(const uint8_t& is_pilot, const uint64_t& slot, const ballot_t& ballot, const uint64_t& dep, const MarshallDeputy& cmd, const DepId& dep_id, ballot_t* max_ballot, uint64_t* ret_dep) {
        RpcFastAcceptRequest __req__;
        __req__.is_pilot = is_pilot;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.dep = dep;
        __req__.cmd = cmd;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->FastAccept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (max_ballot) *max_ballot = __resp__.max_ballot;
        if (ret_dep) *ret_dep = __resp__.ret_dep;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.max_ballot;
            return rusty::Result<RpcAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AcceptTypedFuture, rrr::i32> async_Accept(const RpcAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::ACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.is_pilot;
            __m__ << req.slot;
            __m__ << req.ballot;
            __m__ << req.dep;
            __m__ << req.cmd;
            __m__ << req.dep_id;
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
    [[deprecated("use typed async_Accept(const RpcAcceptRequest&) instead")]]
    rrr::FutureResult async_Accept(const uint8_t& is_pilot, const uint64_t& slot, const ballot_t& ballot, const uint64_t& dep, const MarshallDeputy& cmd, const DepId& dep_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcAcceptRequest __req__;
        __req__.is_pilot = is_pilot;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.dep = dep;
        __req__.cmd = cmd;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->async_Accept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Accept(const RpcAcceptRequest&) instead")]]
    rrr::i32 Accept(const uint8_t& is_pilot, const uint64_t& slot, const ballot_t& ballot, const uint64_t& dep, const MarshallDeputy& cmd, const DepId& dep_id, ballot_t* max_ballot) {
        RpcAcceptRequest __req__;
        __req__.is_pilot = is_pilot;
        __req__.slot = slot;
        __req__.ballot = ballot;
        __req__.dep = dep;
        __req__.cmd = cmd;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->Accept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (max_ballot) *max_ballot = __resp__.max_ballot;
        return 0;
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
            return rusty::Result<RpcCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<CommitTypedFuture, rrr::i32> async_Commit(const RpcCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::COMMIT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.is_pilot;
            __m__ << req.slot;
            __m__ << req.dep;
            __m__ << req.cmd;
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
    [[deprecated("use typed async_Commit(const RpcCommitRequest&) instead")]]
    rrr::FutureResult async_Commit(const uint8_t& is_pilot, const uint64_t& slot, const uint64_t& dep, const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcCommitRequest __req__;
        __req__.is_pilot = is_pilot;
        __req__.slot = slot;
        __req__.dep = dep;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_Commit(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Commit(const RpcCommitRequest&) instead")]]
    rrr::i32 Commit(const uint8_t& is_pilot, const uint64_t& slot, const uint64_t& dep, const MarshallDeputy& cmd) {
        RpcCommitRequest __req__;
        __req__.is_pilot = is_pilot;
        __req__.slot = slot;
        __req__.dep = dep;
        __req__.cmd = cmd;
        auto __typed_result__ = this->Commit(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
};

class ClassicService : public rrr::Service {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcMsgStringRequest {
        std::string arg;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcMsgStringRequest& o) {
        m << o.arg;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcMsgStringRequest& o) {
        m >> o.arg;
        return m;
    }

    struct RpcMsgStringResponse {
        std::string ret;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcMsgStringResponse& o) {
        m << o.ret;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcMsgStringResponse& o) {
        m >> o.ret;
        return m;
    }

    struct RpcMsgMarshallRequest {
        MarshallDeputy arg;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcMsgMarshallRequest& o) {
        m << o.arg;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcMsgMarshallRequest& o) {
        m >> o.arg;
        return m;
    }

    struct RpcMsgMarshallResponse {
        MarshallDeputy ret;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcMsgMarshallResponse& o) {
        m << o.ret;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcMsgMarshallResponse& o) {
        m >> o.ret;
        return m;
    }

    struct RpcReElectRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcReElectRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcReElectRequest& o) {
        return m;
    }

    struct RpcReElectResponse {
        bool_t success;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcReElectResponse& o) {
        m << o.success;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcReElectResponse& o) {
        m >> o.success;
        return m;
    }

    struct RpcRuleSpeculativeExecuteRequest {
        MarshallDeputy md;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRuleSpeculativeExecuteRequest& o) {
        m << o.md;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRuleSpeculativeExecuteRequest& o) {
        m >> o.md;
        return m;
    }

    struct RpcRuleSpeculativeExecuteResponse {
        bool_t accepted;
        int32_t result;
        bool_t is_leader;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRuleSpeculativeExecuteResponse& o) {
        m << o.accepted;
        m << o.result;
        m << o.is_leader;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRuleSpeculativeExecuteResponse& o) {
        m >> o.accepted;
        m >> o.result;
        m >> o.is_leader;
        return m;
    }

    struct RpcDispatchRequest {
        rrr::i64 tid;
        DepId dep_id;
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDispatchRequest& o) {
        m << o.tid;
        m << o.dep_id;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDispatchRequest& o) {
        m >> o.tid;
        m >> o.dep_id;
        m >> o.cmd;
        return m;
    }

    struct RpcDispatchResponse {
        rrr::i32 res;
        TxnOutput output;
        uint64_t coro_id;
        MarshallDeputy view_data;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDispatchResponse& o) {
        m << o.res;
        m << o.output;
        m << o.coro_id;
        m << o.view_data;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDispatchResponse& o) {
        m >> o.res;
        m >> o.output;
        m >> o.coro_id;
        m >> o.view_data;
        return m;
    }

    struct RpcPrepareRequest {
        rrr::i64 tid;
        std::vector<rrr::i32> sids;
        DepId dep_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPrepareRequest& o) {
        m << o.tid;
        m << o.sids;
        m << o.dep_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPrepareRequest& o) {
        m >> o.tid;
        m >> o.sids;
        m >> o.dep_id;
        return m;
    }

    struct RpcPrepareResponse {
        rrr::i32 res;
        bool_t slow;
        uint64_t coro_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPrepareResponse& o) {
        m << o.res;
        m << o.slow;
        m << o.coro_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPrepareResponse& o) {
        m >> o.res;
        m >> o.slow;
        m >> o.coro_id;
        return m;
    }

    struct RpcCommitRequest {
        rrr::i64 tid;
        DepId dep_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCommitRequest& o) {
        m << o.tid;
        m << o.dep_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCommitRequest& o) {
        m >> o.tid;
        m >> o.dep_id;
        return m;
    }

    struct RpcCommitResponse {
        rrr::i32 res;
        bool_t slow;
        uint64_t coro_id;
        Profiling profile;
        MarshallDeputy view_data;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCommitResponse& o) {
        m << o.res;
        m << o.slow;
        m << o.coro_id;
        m << o.profile;
        m << o.view_data;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCommitResponse& o) {
        m >> o.res;
        m >> o.slow;
        m >> o.coro_id;
        m >> o.profile;
        m >> o.view_data;
        return m;
    }

    struct RpcAbortRequest {
        rrr::i64 tid;
        DepId dep_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAbortRequest& o) {
        m << o.tid;
        m << o.dep_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAbortRequest& o) {
        m >> o.tid;
        m >> o.dep_id;
        return m;
    }

    struct RpcAbortResponse {
        rrr::i32 res;
        bool_t slow;
        uint64_t coro_id;
        Profiling profile;
        MarshallDeputy view_data;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAbortResponse& o) {
        m << o.res;
        m << o.slow;
        m << o.coro_id;
        m << o.profile;
        m << o.view_data;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAbortResponse& o) {
        m >> o.res;
        m >> o.slow;
        m >> o.coro_id;
        m >> o.profile;
        m >> o.view_data;
        return m;
    }

    struct RpcEarlyAbortRequest {
        rrr::i64 tid;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcEarlyAbortRequest& o) {
        m << o.tid;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcEarlyAbortRequest& o) {
        m >> o.tid;
        return m;
    }

    struct RpcEarlyAbortResponse {
        rrr::i32 res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcEarlyAbortResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcEarlyAbortResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcUpgradeEpochRequest {
        uint32_t curr_epoch;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcUpgradeEpochRequest& o) {
        m << o.curr_epoch;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcUpgradeEpochRequest& o) {
        m >> o.curr_epoch;
        return m;
    }

    struct RpcUpgradeEpochResponse {
        int32_t res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcUpgradeEpochResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcUpgradeEpochResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcTruncateEpochRequest {
        uint32_t old_epoch;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTruncateEpochRequest& o) {
        m << o.old_epoch;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTruncateEpochRequest& o) {
        m >> o.old_epoch;
        return m;
    }

    struct RpcTruncateEpochResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTruncateEpochResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTruncateEpochResponse& o) {
        return m;
    }

    struct RpcIsLeaderRequest {
        locid_t cur_pause;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcIsLeaderRequest& o) {
        m << o.cur_pause;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcIsLeaderRequest& o) {
        m >> o.cur_pause;
        return m;
    }

    struct RpcIsLeaderResponse {
        bool_t is_leader;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcIsLeaderResponse& o) {
        m << o.is_leader;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcIsLeaderResponse& o) {
        m >> o.is_leader;
        return m;
    }

    struct RpcIsFPGALeaderRequest {
        locid_t cur_pause;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcIsFPGALeaderRequest& o) {
        m << o.cur_pause;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcIsFPGALeaderRequest& o) {
        m >> o.cur_pause;
        return m;
    }

    struct RpcIsFPGALeaderResponse {
        bool_t is_leader;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcIsFPGALeaderResponse& o) {
        m << o.is_leader;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcIsFPGALeaderResponse& o) {
        m >> o.is_leader;
        return m;
    }

    struct RpcSimpleCmdRequest {
        SimpleCommand cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSimpleCmdRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSimpleCmdRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcSimpleCmdResponse {
        rrr::i32 res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSimpleCmdResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSimpleCmdResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcFailoverPauseSocketOutRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFailoverPauseSocketOutRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFailoverPauseSocketOutRequest& o) {
        return m;
    }

    struct RpcFailoverPauseSocketOutResponse {
        rrr::i32 res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFailoverPauseSocketOutResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFailoverPauseSocketOutResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcFailoverResumeSocketOutRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFailoverResumeSocketOutRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFailoverResumeSocketOutRequest& o) {
        return m;
    }

    struct RpcFailoverResumeSocketOutResponse {
        rrr::i32 res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcFailoverResumeSocketOutResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcFailoverResumeSocketOutResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcRpcNullRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRpcNullRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRpcNullRequest& o) {
        return m;
    }

    struct RpcRpcNullResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRpcNullResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRpcNullResponse& o) {
        return m;
    }

    struct RpcTapirAcceptRequest {
        uint64_t cmd_id;
        int64_t ballot;
        int32_t decision;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTapirAcceptRequest& o) {
        m << o.cmd_id;
        m << o.ballot;
        m << o.decision;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTapirAcceptRequest& o) {
        m >> o.cmd_id;
        m >> o.ballot;
        m >> o.decision;
        return m;
    }

    struct RpcTapirAcceptResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTapirAcceptResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTapirAcceptResponse& o) {
        return m;
    }

    struct RpcTapirFastAcceptRequest {
        uint64_t cmd_id;
        std::vector<SimpleCommand> txn_cmds;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTapirFastAcceptRequest& o) {
        m << o.cmd_id;
        m << o.txn_cmds;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTapirFastAcceptRequest& o) {
        m >> o.cmd_id;
        m >> o.txn_cmds;
        return m;
    }

    struct RpcTapirFastAcceptResponse {
        rrr::i32 res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTapirFastAcceptResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTapirFastAcceptResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcTapirDecideRequest {
        uint64_t cmd_id;
        rrr::i32 commit;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTapirDecideRequest& o) {
        m << o.cmd_id;
        m << o.commit;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTapirDecideRequest& o) {
        m >> o.cmd_id;
        m >> o.commit;
        return m;
    }

    struct RpcTapirDecideResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcTapirDecideResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcTapirDecideResponse& o) {
        return m;
    }

    struct RpcCarouselReadAndPrepareRequest {
        rrr::i64 tid;
        MarshallDeputy cmd;
        bool_t leader;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCarouselReadAndPrepareRequest& o) {
        m << o.tid;
        m << o.cmd;
        m << o.leader;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCarouselReadAndPrepareRequest& o) {
        m >> o.tid;
        m >> o.cmd;
        m >> o.leader;
        return m;
    }

    struct RpcCarouselReadAndPrepareResponse {
        rrr::i32 res;
        TxnOutput output;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCarouselReadAndPrepareResponse& o) {
        m << o.res;
        m << o.output;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCarouselReadAndPrepareResponse& o) {
        m >> o.res;
        m >> o.output;
        return m;
    }

    struct RpcCarouselAcceptRequest {
        uint64_t cmd_id;
        int64_t ballot;
        int32_t decision;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCarouselAcceptRequest& o) {
        m << o.cmd_id;
        m << o.ballot;
        m << o.decision;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCarouselAcceptRequest& o) {
        m >> o.cmd_id;
        m >> o.ballot;
        m >> o.decision;
        return m;
    }

    struct RpcCarouselAcceptResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCarouselAcceptResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCarouselAcceptResponse& o) {
        return m;
    }

    struct RpcCarouselFastAcceptRequest {
        uint64_t cmd_id;
        std::vector<SimpleCommand> txn_cmds;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCarouselFastAcceptRequest& o) {
        m << o.cmd_id;
        m << o.txn_cmds;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCarouselFastAcceptRequest& o) {
        m >> o.cmd_id;
        m >> o.txn_cmds;
        return m;
    }

    struct RpcCarouselFastAcceptResponse {
        rrr::i32 res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCarouselFastAcceptResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCarouselFastAcceptResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcCarouselDecideRequest {
        uint64_t cmd_id;
        rrr::i32 commit;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCarouselDecideRequest& o) {
        m << o.cmd_id;
        m << o.commit;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCarouselDecideRequest& o) {
        m >> o.cmd_id;
        m >> o.commit;
        return m;
    }

    struct RpcCarouselDecideResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCarouselDecideResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCarouselDecideResponse& o) {
        return m;
    }

    struct RpcRccDispatchRequest {
        std::vector<SimpleCommand> cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccDispatchRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccDispatchRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcRccDispatchResponse {
        rrr::i32 res;
        TxnOutput output;
        MarshallDeputy md_graph;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccDispatchResponse& o) {
        m << o.res;
        m << o.output;
        m << o.md_graph;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccDispatchResponse& o) {
        m >> o.res;
        m >> o.output;
        m >> o.md_graph;
        return m;
    }

    struct RpcRccFinishRequest {
        cmdid_t id;
        MarshallDeputy md_graph;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccFinishRequest& o) {
        m << o.id;
        m << o.md_graph;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccFinishRequest& o) {
        m >> o.id;
        m >> o.md_graph;
        return m;
    }

    struct RpcRccFinishResponse {
        std::map<uint32_t, std::map<int32_t, Value>> outputs;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccFinishResponse& o) {
        m << o.outputs;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccFinishResponse& o) {
        m >> o.outputs;
        return m;
    }

    struct RpcRccInquireRequest {
        txnid_t txn_id;
        int32_t rank;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccInquireRequest& o) {
        m << o.txn_id;
        m << o.rank;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccInquireRequest& o) {
        m >> o.txn_id;
        m >> o.rank;
        return m;
    }

    struct RpcRccInquireResponse {
        std::map<uint64_t, parent_set_t> out_0;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccInquireResponse& o) {
        m << o.out_0;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccInquireResponse& o) {
        m >> o.out_0;
        return m;
    }

    struct RpcRccDispatchRoRequest {
        SimpleCommand cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccDispatchRoRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccDispatchRoRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcRccDispatchRoResponse {
        std::map<rrr::i32, Value> output;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccDispatchRoResponse& o) {
        m << o.output;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccDispatchRoResponse& o) {
        m >> o.output;
        return m;
    }

    struct RpcRccInquireValidationRequest {
        txid_t tx_id;
        int32_t rank;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccInquireValidationRequest& o) {
        m << o.tx_id;
        m << o.rank;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccInquireValidationRequest& o) {
        m >> o.tx_id;
        m >> o.rank;
        return m;
    }

    struct RpcRccInquireValidationResponse {
        int32_t res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccInquireValidationResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccInquireValidationResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcRccNotifyGlobalValidationRequest {
        txid_t tx_id;
        int32_t rank;
        int32_t res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccNotifyGlobalValidationRequest& o) {
        m << o.tx_id;
        m << o.rank;
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccNotifyGlobalValidationRequest& o) {
        m >> o.tx_id;
        m >> o.rank;
        m >> o.res;
        return m;
    }

    struct RpcRccNotifyGlobalValidationResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccNotifyGlobalValidationResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccNotifyGlobalValidationResponse& o) {
        return m;
    }

    struct RpcJanusDispatchRequest {
        std::vector<SimpleCommand> cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusDispatchRequest& o) {
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusDispatchRequest& o) {
        m >> o.cmd;
        return m;
    }

    struct RpcJanusDispatchResponse {
        rrr::i32 res;
        TxnOutput output;
        MarshallDeputy ret_graph;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusDispatchResponse& o) {
        m << o.res;
        m << o.output;
        m << o.ret_graph;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusDispatchResponse& o) {
        m >> o.res;
        m >> o.output;
        m >> o.ret_graph;
        return m;
    }

    struct RpcRccCommitRequest {
        cmdid_t id;
        rank_t rank;
        int32_t need_validation;
        parent_set_t parents;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccCommitRequest& o) {
        m << o.id;
        m << o.rank;
        m << o.need_validation;
        m << o.parents;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccCommitRequest& o) {
        m >> o.id;
        m >> o.rank;
        m >> o.need_validation;
        m >> o.parents;
        return m;
    }

    struct RpcRccCommitResponse {
        int32_t res;
        TxnOutput output;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccCommitResponse& o) {
        m << o.res;
        m << o.output;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccCommitResponse& o) {
        m >> o.res;
        m >> o.output;
        return m;
    }

    struct RpcJanusCommitRequest {
        cmdid_t id;
        rank_t rank;
        int32_t need_validation;
        MarshallDeputy graph;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusCommitRequest& o) {
        m << o.id;
        m << o.rank;
        m << o.need_validation;
        m << o.graph;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusCommitRequest& o) {
        m >> o.id;
        m >> o.rank;
        m >> o.need_validation;
        m >> o.graph;
        return m;
    }

    struct RpcJanusCommitResponse {
        int32_t res;
        TxnOutput output;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusCommitResponse& o) {
        m << o.res;
        m << o.output;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusCommitResponse& o) {
        m >> o.res;
        m >> o.output;
        return m;
    }

    struct RpcJanusCommitWoGraphRequest {
        cmdid_t id;
        rank_t rank;
        int32_t need_validation;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusCommitWoGraphRequest& o) {
        m << o.id;
        m << o.rank;
        m << o.need_validation;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusCommitWoGraphRequest& o) {
        m >> o.id;
        m >> o.rank;
        m >> o.need_validation;
        return m;
    }

    struct RpcJanusCommitWoGraphResponse {
        int32_t res;
        TxnOutput output;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusCommitWoGraphResponse& o) {
        m << o.res;
        m << o.output;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusCommitWoGraphResponse& o) {
        m >> o.res;
        m >> o.output;
        return m;
    }

    struct RpcJanusInquireRequest {
        epoch_t epoch;
        txnid_t txn_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusInquireRequest& o) {
        m << o.epoch;
        m << o.txn_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusInquireRequest& o) {
        m >> o.epoch;
        m >> o.txn_id;
        return m;
    }

    struct RpcJanusInquireResponse {
        MarshallDeputy ret_graph;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusInquireResponse& o) {
        m << o.ret_graph;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusInquireResponse& o) {
        m >> o.ret_graph;
        return m;
    }

    struct RpcRccPreAcceptRequest {
        cmdid_t txn_id;
        rank_t rank;
        std::vector<SimpleCommand> cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccPreAcceptRequest& o) {
        m << o.txn_id;
        m << o.rank;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccPreAcceptRequest& o) {
        m >> o.txn_id;
        m >> o.rank;
        m >> o.cmd;
        return m;
    }

    struct RpcRccPreAcceptResponse {
        rrr::i32 res;
        parent_set_t x;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccPreAcceptResponse& o) {
        m << o.res;
        m << o.x;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccPreAcceptResponse& o) {
        m >> o.res;
        m >> o.x;
        return m;
    }

    struct RpcJanusPreAcceptRequest {
        cmdid_t txn_id;
        rank_t rank;
        std::vector<SimpleCommand> cmd;
        MarshallDeputy graph;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusPreAcceptRequest& o) {
        m << o.txn_id;
        m << o.rank;
        m << o.cmd;
        m << o.graph;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusPreAcceptRequest& o) {
        m >> o.txn_id;
        m >> o.rank;
        m >> o.cmd;
        m >> o.graph;
        return m;
    }

    struct RpcJanusPreAcceptResponse {
        rrr::i32 res;
        MarshallDeputy ret_graph;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusPreAcceptResponse& o) {
        m << o.res;
        m << o.ret_graph;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusPreAcceptResponse& o) {
        m >> o.res;
        m >> o.ret_graph;
        return m;
    }

    struct RpcJanusPreAcceptWoGraphRequest {
        cmdid_t txn_id;
        rank_t rank;
        std::vector<SimpleCommand> cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusPreAcceptWoGraphRequest& o) {
        m << o.txn_id;
        m << o.rank;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusPreAcceptWoGraphRequest& o) {
        m >> o.txn_id;
        m >> o.rank;
        m >> o.cmd;
        return m;
    }

    struct RpcJanusPreAcceptWoGraphResponse {
        rrr::i32 res;
        MarshallDeputy ret_graph;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusPreAcceptWoGraphResponse& o) {
        m << o.res;
        m << o.ret_graph;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusPreAcceptWoGraphResponse& o) {
        m >> o.res;
        m >> o.ret_graph;
        return m;
    }

    struct RpcRccAcceptRequest {
        cmdid_t txn_id;
        rrr::i32 rank;
        ballot_t ballot;
        parent_set_t p;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccAcceptRequest& o) {
        m << o.txn_id;
        m << o.rank;
        m << o.ballot;
        m << o.p;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccAcceptRequest& o) {
        m >> o.txn_id;
        m >> o.rank;
        m >> o.ballot;
        m >> o.p;
        return m;
    }

    struct RpcRccAcceptResponse {
        rrr::i32 res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcRccAcceptResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcRccAcceptResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcJanusAcceptRequest {
        cmdid_t txn_id;
        rrr::i32 rank;
        ballot_t ballot;
        MarshallDeputy graph;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusAcceptRequest& o) {
        m << o.txn_id;
        m << o.rank;
        m << o.ballot;
        m << o.graph;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusAcceptRequest& o) {
        m >> o.txn_id;
        m >> o.rank;
        m >> o.ballot;
        m >> o.graph;
        return m;
    }

    struct RpcJanusAcceptResponse {
        rrr::i32 res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJanusAcceptResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJanusAcceptResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcPreAcceptFebruusRequest {
        txid_t tx_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPreAcceptFebruusRequest& o) {
        m << o.tx_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPreAcceptFebruusRequest& o) {
        m >> o.tx_id;
        return m;
    }

    struct RpcPreAcceptFebruusResponse {
        rrr::i32 ret;
        uint64_t timestamp;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcPreAcceptFebruusResponse& o) {
        m << o.ret;
        m << o.timestamp;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcPreAcceptFebruusResponse& o) {
        m >> o.ret;
        m >> o.timestamp;
        return m;
    }

    struct RpcAcceptFebruusRequest {
        txid_t tx_id;
        ballot_t ballot;
        uint64_t timestamp;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAcceptFebruusRequest& o) {
        m << o.tx_id;
        m << o.ballot;
        m << o.timestamp;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAcceptFebruusRequest& o) {
        m >> o.tx_id;
        m >> o.ballot;
        m >> o.timestamp;
        return m;
    }

    struct RpcAcceptFebruusResponse {
        rrr::i32 ret;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcAcceptFebruusResponse& o) {
        m << o.ret;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcAcceptFebruusResponse& o) {
        m >> o.ret;
        return m;
    }

    struct RpcCommitFebruusRequest {
        txid_t tx_id;
        uint64_t timestamp;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCommitFebruusRequest& o) {
        m << o.tx_id;
        m << o.timestamp;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCommitFebruusRequest& o) {
        m >> o.tx_id;
        m >> o.timestamp;
        return m;
    }

    struct RpcCommitFebruusResponse {
        rrr::i32 ret;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcCommitFebruusResponse& o) {
        m << o.ret;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcCommitFebruusResponse& o) {
        m >> o.ret;
        return m;
    }

    struct RpcJetpackBeginRecoveryRequest {
        MarshallDeputy old_view;
        MarshallDeputy new_view;
        epoch_t new_view_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackBeginRecoveryRequest& o) {
        m << o.old_view;
        m << o.new_view;
        m << o.new_view_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackBeginRecoveryRequest& o) {
        m >> o.old_view;
        m >> o.new_view;
        m >> o.new_view_id;
        return m;
    }

    struct RpcJetpackBeginRecoveryResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackBeginRecoveryResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackBeginRecoveryResponse& o) {
        return m;
    }

    struct RpcJetpackPullIdSetRequest {
        epoch_t jepoch;
        epoch_t oepoch;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackPullIdSetRequest& o) {
        m << o.jepoch;
        m << o.oepoch;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackPullIdSetRequest& o) {
        m >> o.jepoch;
        m >> o.oepoch;
        return m;
    }

    struct RpcJetpackPullIdSetResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        MarshallDeputy reply_old_view;
        MarshallDeputy reply_new_view;
        MarshallDeputy id_set;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackPullIdSetResponse& o) {
        m << o.ok;
        m << o.reply_jepoch;
        m << o.reply_oepoch;
        m << o.reply_old_view;
        m << o.reply_new_view;
        m << o.id_set;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackPullIdSetResponse& o) {
        m >> o.ok;
        m >> o.reply_jepoch;
        m >> o.reply_oepoch;
        m >> o.reply_old_view;
        m >> o.reply_new_view;
        m >> o.id_set;
        return m;
    }

    struct RpcJetpackPullCmdRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        MarshallDeputy key_batch;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackPullCmdRequest& o) {
        m << o.jepoch;
        m << o.oepoch;
        m << o.key_batch;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackPullCmdRequest& o) {
        m >> o.jepoch;
        m >> o.oepoch;
        m >> o.key_batch;
        return m;
    }

    struct RpcJetpackPullCmdResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        MarshallDeputy reply_old_view;
        MarshallDeputy reply_new_view;
        MarshallDeputy cmd_batch;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackPullCmdResponse& o) {
        m << o.ok;
        m << o.reply_jepoch;
        m << o.reply_oepoch;
        m << o.reply_old_view;
        m << o.reply_new_view;
        m << o.cmd_batch;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackPullCmdResponse& o) {
        m >> o.ok;
        m >> o.reply_jepoch;
        m >> o.reply_oepoch;
        m >> o.reply_old_view;
        m >> o.reply_new_view;
        m >> o.cmd_batch;
        return m;
    }

    struct RpcJetpackRecordCmdRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        int32_t sid;
        int32_t rid;
        MarshallDeputy cmd_batch;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackRecordCmdRequest& o) {
        m << o.jepoch;
        m << o.oepoch;
        m << o.sid;
        m << o.rid;
        m << o.cmd_batch;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackRecordCmdRequest& o) {
        m >> o.jepoch;
        m >> o.oepoch;
        m >> o.sid;
        m >> o.rid;
        m >> o.cmd_batch;
        return m;
    }

    struct RpcJetpackRecordCmdResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackRecordCmdResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackRecordCmdResponse& o) {
        return m;
    }

    struct RpcJetpackPrepareRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        ballot_t max_seen_ballot;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackPrepareRequest& o) {
        m << o.jepoch;
        m << o.oepoch;
        m << o.max_seen_ballot;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackPrepareRequest& o) {
        m >> o.jepoch;
        m >> o.oepoch;
        m >> o.max_seen_ballot;
        return m;
    }

    struct RpcJetpackPrepareResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        MarshallDeputy reply_old_view;
        MarshallDeputy reply_new_view;
        ballot_t reply_max_seen_ballot;
        ballot_t accepted_ballot;
        int32_t replied_sid;
        int32_t replied_set_size;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackPrepareResponse& o) {
        m << o.ok;
        m << o.reply_jepoch;
        m << o.reply_oepoch;
        m << o.reply_old_view;
        m << o.reply_new_view;
        m << o.reply_max_seen_ballot;
        m << o.accepted_ballot;
        m << o.replied_sid;
        m << o.replied_set_size;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackPrepareResponse& o) {
        m >> o.ok;
        m >> o.reply_jepoch;
        m >> o.reply_oepoch;
        m >> o.reply_old_view;
        m >> o.reply_new_view;
        m >> o.reply_max_seen_ballot;
        m >> o.accepted_ballot;
        m >> o.replied_sid;
        m >> o.replied_set_size;
        return m;
    }

    struct RpcJetpackAcceptRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        ballot_t max_seen_ballot;
        int32_t sid;
        int32_t set_size;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackAcceptRequest& o) {
        m << o.jepoch;
        m << o.oepoch;
        m << o.max_seen_ballot;
        m << o.sid;
        m << o.set_size;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackAcceptRequest& o) {
        m >> o.jepoch;
        m >> o.oepoch;
        m >> o.max_seen_ballot;
        m >> o.sid;
        m >> o.set_size;
        return m;
    }

    struct RpcJetpackAcceptResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        MarshallDeputy reply_old_view;
        MarshallDeputy reply_new_view;
        ballot_t reply_max_seen_ballot;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackAcceptResponse& o) {
        m << o.ok;
        m << o.reply_jepoch;
        m << o.reply_oepoch;
        m << o.reply_old_view;
        m << o.reply_new_view;
        m << o.reply_max_seen_ballot;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackAcceptResponse& o) {
        m >> o.ok;
        m >> o.reply_jepoch;
        m >> o.reply_oepoch;
        m >> o.reply_old_view;
        m >> o.reply_new_view;
        m >> o.reply_max_seen_ballot;
        return m;
    }

    struct RpcJetpackCommitRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        int32_t sid;
        int32_t set_size;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackCommitRequest& o) {
        m << o.jepoch;
        m << o.oepoch;
        m << o.sid;
        m << o.set_size;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackCommitRequest& o) {
        m >> o.jepoch;
        m >> o.oepoch;
        m >> o.sid;
        m >> o.set_size;
        return m;
    }

    struct RpcJetpackCommitResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackCommitResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackCommitResponse& o) {
        return m;
    }

    struct RpcJetpackPullRecSetInsRequest {
        epoch_t jepoch;
        epoch_t oepoch;
        int32_t sid;
        int32_t rid;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackPullRecSetInsRequest& o) {
        m << o.jepoch;
        m << o.oepoch;
        m << o.sid;
        m << o.rid;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackPullRecSetInsRequest& o) {
        m >> o.jepoch;
        m >> o.oepoch;
        m >> o.sid;
        m >> o.rid;
        return m;
    }

    struct RpcJetpackPullRecSetInsResponse {
        bool_t ok;
        epoch_t reply_jepoch;
        epoch_t reply_oepoch;
        MarshallDeputy reply_old_view;
        MarshallDeputy reply_new_view;
        MarshallDeputy cmd;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackPullRecSetInsResponse& o) {
        m << o.ok;
        m << o.reply_jepoch;
        m << o.reply_oepoch;
        m << o.reply_old_view;
        m << o.reply_new_view;
        m << o.cmd;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackPullRecSetInsResponse& o) {
        m >> o.ok;
        m >> o.reply_jepoch;
        m >> o.reply_oepoch;
        m >> o.reply_old_view;
        m >> o.reply_new_view;
        m >> o.cmd;
        return m;
    }

    struct RpcJetpackFinishRecoveryRequest {
        epoch_t oepoch;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackFinishRecoveryRequest& o) {
        m << o.oepoch;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackFinishRecoveryRequest& o) {
        m >> o.oepoch;
        return m;
    }

    struct RpcJetpackFinishRecoveryResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcJetpackFinishRecoveryResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcJetpackFinishRecoveryResponse& o) {
        return m;
    }

    enum {
        MSGSTRING = 0x21ce954a,
        MSGMARSHALL = 0x15d47a97,
        REELECT = 0x184ef22b,
        RULESPECULATIVEEXECUTE = 0x34a35277,
        DISPATCH = 0x42b89161,
        PREPARE = 0x12313a17,
        COMMIT = 0x59284b0f,
        ABORT = 0x28e2f218,
        EARLYABORT = 0x28b88e7c,
        UPGRADEEPOCH = 0x34c69d2b,
        TRUNCATEEPOCH = 0x52735ed8,
        ISLEADER = 0x61df212b,
        ISFPGALEADER = 0x587a4c85,
        SIMPLECMD = 0x279b9df0,
        FAILOVERPAUSESOCKETOUT = 0x113bb1a3,
        FAILOVERRESUMESOCKETOUT = 0x12747a03,
        RPC_NULL = 0x30c16c46,
        TAPIRACCEPT = 0x1a79041e,
        TAPIRFASTACCEPT = 0x23a264a5,
        TAPIRDECIDE = 0x37ff0a66,
        CAROUSELREADANDPREPARE = 0x61f999be,
        CAROUSELACCEPT = 0x1c056c36,
        CAROUSELFASTACCEPT = 0x3622986c,
        CAROUSELDECIDE = 0x5dae847e,
        RCCDISPATCH = 0x423efaf6,
        RCCFINISH = 0x59386749,
        RCCINQUIRE = 0x46d9abf4,
        RCCDISPATCHRO = 0x1b07fa9f,
        RCCINQUIREVALIDATION = 0x2efcbb69,
        RCCNOTIFYGLOBALVALIDATION = 0x10e1189d,
        JANUSDISPATCH = 0x5104eb95,
        RCCCOMMIT = 0x6a375f78,
        JANUSCOMMIT = 0x6203cfef,
        JANUSCOMMITWOGRAPH = 0x15b98069,
        JANUSINQUIRE = 0x6acf714e,
        RCCPREACCEPT = 0x5d094f79,
        JANUSPREACCEPT = 0x504568b7,
        JANUSPREACCEPTWOGRAPH = 0x2034b659,
        RCCACCEPT = 0x62eb1238,
        JANUSACCEPT = 0x179a542f,
        PREACCEPTFEBRUUS = 0x36385ac6,
        ACCEPTFEBRUUS = 0x3548a7a3,
        COMMITFEBRUUS = 0x413d1b83,
        JETPACKBEGINRECOVERY = 0x58f36b71,
        JETPACKPULLIDSET = 0x667c4cca,
        JETPACKPULLCMD = 0x24b707b7,
        JETPACKRECORDCMD = 0x1d5f072d,
        JETPACKPREPARE = 0x41b07d16,
        JETPACKACCEPT = 0x16c306c4,
        JETPACKCOMMIT = 0x35c9216d,
        JETPACKPULLRECSETINS = 0x3d5b8815,
        JETPACKFINISHRECOVERY = 0x36eda683,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
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
        if ((ret = svr.reg_rpc(CAROUSELREADANDPREPARE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(CAROUSELACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(CAROUSELFASTACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(CAROUSELDECIDE, svc_index)) != 0) {
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
        if ((ret = svr.reg_rpc(JANUSDISPATCH, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCCOMMIT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JANUSCOMMIT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JANUSCOMMITWOGRAPH, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JANUSINQUIRE, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCPREACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JANUSPREACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JANUSPREACCEPTWOGRAPH, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(RCCACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(JANUSACCEPT, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(PREACCEPTFEBRUUS, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(ACCEPTFEBRUUS, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(COMMITFEBRUUS, svc_index)) != 0) {
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
        svr.unreg(CAROUSELREADANDPREPARE);
        svr.unreg(CAROUSELACCEPT);
        svr.unreg(CAROUSELFASTACCEPT);
        svr.unreg(CAROUSELDECIDE);
        svr.unreg(RCCDISPATCH);
        svr.unreg(RCCFINISH);
        svr.unreg(RCCINQUIRE);
        svr.unreg(RCCDISPATCHRO);
        svr.unreg(RCCINQUIREVALIDATION);
        svr.unreg(RCCNOTIFYGLOBALVALIDATION);
        svr.unreg(JANUSDISPATCH);
        svr.unreg(RCCCOMMIT);
        svr.unreg(JANUSCOMMIT);
        svr.unreg(JANUSCOMMITWOGRAPH);
        svr.unreg(JANUSINQUIRE);
        svr.unreg(RCCPREACCEPT);
        svr.unreg(JANUSPREACCEPT);
        svr.unreg(JANUSPREACCEPTWOGRAPH);
        svr.unreg(RCCACCEPT);
        svr.unreg(JANUSACCEPT);
        svr.unreg(PREACCEPTFEBRUUS);
        svr.unreg(ACCEPTFEBRUUS);
        svr.unreg(COMMITFEBRUUS);
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
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
        case CAROUSELREADANDPREPARE: __CarouselReadAndPrepare__wrapper__(std::move(req), weak_sconn); break;
        case CAROUSELACCEPT: __CarouselAccept__wrapper__(std::move(req), weak_sconn); break;
        case CAROUSELFASTACCEPT: __CarouselFastAccept__wrapper__(std::move(req), weak_sconn); break;
        case CAROUSELDECIDE: __CarouselDecide__wrapper__(std::move(req), weak_sconn); break;
        case RCCDISPATCH: __RccDispatch__wrapper__(std::move(req), weak_sconn); break;
        case RCCFINISH: __RccFinish__wrapper__(std::move(req), weak_sconn); break;
        case RCCINQUIRE: __RccInquire__wrapper__(std::move(req), weak_sconn); break;
        case RCCDISPATCHRO: __RccDispatchRo__wrapper__(std::move(req), weak_sconn); break;
        case RCCINQUIREVALIDATION: __RccInquireValidation__wrapper__(std::move(req), weak_sconn); break;
        case RCCNOTIFYGLOBALVALIDATION: __RccNotifyGlobalValidation__wrapper__(std::move(req), weak_sconn); break;
        case JANUSDISPATCH: __JanusDispatch__wrapper__(std::move(req), weak_sconn); break;
        case RCCCOMMIT: __RccCommit__wrapper__(std::move(req), weak_sconn); break;
        case JANUSCOMMIT: __JanusCommit__wrapper__(std::move(req), weak_sconn); break;
        case JANUSCOMMITWOGRAPH: __JanusCommitWoGraph__wrapper__(std::move(req), weak_sconn); break;
        case JANUSINQUIRE: __JanusInquire__wrapper__(std::move(req), weak_sconn); break;
        case RCCPREACCEPT: __RccPreAccept__wrapper__(std::move(req), weak_sconn); break;
        case JANUSPREACCEPT: __JanusPreAccept__wrapper__(std::move(req), weak_sconn); break;
        case JANUSPREACCEPTWOGRAPH: __JanusPreAcceptWoGraph__wrapper__(std::move(req), weak_sconn); break;
        case RCCACCEPT: __RccAccept__wrapper__(std::move(req), weak_sconn); break;
        case JANUSACCEPT: __JanusAccept__wrapper__(std::move(req), weak_sconn); break;
        case PREACCEPTFEBRUUS: __PreAcceptFebruus__wrapper__(std::move(req), weak_sconn); break;
        case ACCEPTFEBRUUS: __AcceptFebruus__wrapper__(std::move(req), weak_sconn); break;
        case COMMITFEBRUUS: __CommitFebruus__wrapper__(std::move(req), weak_sconn); break;
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
    virtual void RuleSpeculativeExecute(const RpcRuleSpeculativeExecuteRequest& req, RpcRuleSpeculativeExecuteResponse& resp, rrr::DeferredReply defer) = 0;
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
    virtual void IsFPGALeader(const RpcIsFPGALeaderRequest& req, RpcIsFPGALeaderResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void SimpleCmd(const RpcSimpleCmdRequest& req, RpcSimpleCmdResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void FailoverPauseSocketOut(const RpcFailoverPauseSocketOutRequest& req, RpcFailoverPauseSocketOutResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest& req, RpcFailoverResumeSocketOutResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void rpc_null(const RpcRpcNullRequest& req, RpcRpcNullResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void TapirAccept(const RpcTapirAcceptRequest& req, RpcTapirAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void TapirFastAccept(const RpcTapirFastAcceptRequest& req, RpcTapirFastAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void TapirDecide(const RpcTapirDecideRequest& req, RpcTapirDecideResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void CarouselReadAndPrepare(const RpcCarouselReadAndPrepareRequest& req, RpcCarouselReadAndPrepareResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void CarouselAccept(const RpcCarouselAcceptRequest& req, RpcCarouselAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void CarouselFastAccept(const RpcCarouselFastAcceptRequest& req, RpcCarouselFastAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void CarouselDecide(const RpcCarouselDecideRequest& req, RpcCarouselDecideResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void RccDispatch(const RpcRccDispatchRequest& req, RpcRccDispatchResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void RccFinish(const RpcRccFinishRequest& req, RpcRccFinishResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void RccInquire(const RpcRccInquireRequest& req, RpcRccInquireResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void RccDispatchRo(const RpcRccDispatchRoRequest& req, RpcRccDispatchRoResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void RccInquireValidation(const RpcRccInquireValidationRequest& req, RpcRccInquireValidationResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void RccNotifyGlobalValidation(const RpcRccNotifyGlobalValidationRequest& req, RpcRccNotifyGlobalValidationResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JanusDispatch(const RpcJanusDispatchRequest& req, RpcJanusDispatchResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void RccCommit(const RpcRccCommitRequest& req, RpcRccCommitResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JanusCommit(const RpcJanusCommitRequest& req, RpcJanusCommitResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JanusCommitWoGraph(const RpcJanusCommitWoGraphRequest& req, RpcJanusCommitWoGraphResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JanusInquire(const RpcJanusInquireRequest& req, RpcJanusInquireResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void RccPreAccept(const RpcRccPreAcceptRequest& req, RpcRccPreAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JanusPreAccept(const RpcJanusPreAcceptRequest& req, RpcJanusPreAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JanusPreAcceptWoGraph(const RpcJanusPreAcceptWoGraphRequest& req, RpcJanusPreAcceptWoGraphResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void RccAccept(const RpcRccAcceptRequest& req, RpcRccAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void JanusAccept(const RpcJanusAcceptRequest& req, RpcJanusAcceptResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void PreAcceptFebruus(const RpcPreAcceptFebruusRequest& req, RpcPreAcceptFebruusResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void AcceptFebruus(const RpcAcceptFebruusRequest& req, RpcAcceptFebruusResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void CommitFebruus(const RpcCommitFebruusRequest& req, RpcCommitFebruusResponse& resp, rrr::DeferredReply defer) = 0;
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
            req->m >> __typed_req__.arg;
            auto __typed_resp__ = std::make_shared<RpcMsgStringResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ret;
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
            req->m >> __typed_req__.arg;
            auto __typed_resp__ = std::make_shared<RpcMsgMarshallResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ret;
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->success;
                },
                []() {});
            this->ReElect(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RuleSpeculativeExecute__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRuleSpeculativeExecuteRequest __typed_req__;
            req->m >> __typed_req__.md;
            auto __typed_resp__ = std::make_shared<RpcRuleSpeculativeExecuteResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->accepted;
                    m << __typed_resp__->result;
                    m << __typed_resp__->is_leader;
                },
                []() {});
            this->RuleSpeculativeExecute(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Dispatch__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcDispatchRequest __typed_req__;
            req->m >> __typed_req__.tid;
            req->m >> __typed_req__.dep_id;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcDispatchResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->output;
                    m << __typed_resp__->coro_id;
                    m << __typed_resp__->view_data;
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
            req->m >> __typed_req__.tid;
            req->m >> __typed_req__.sids;
            req->m >> __typed_req__.dep_id;
            auto __typed_resp__ = std::make_shared<RpcPrepareResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->slow;
                    m << __typed_resp__->coro_id;
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
            req->m >> __typed_req__.tid;
            req->m >> __typed_req__.dep_id;
            auto __typed_resp__ = std::make_shared<RpcCommitResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->slow;
                    m << __typed_resp__->coro_id;
                    m << __typed_resp__->profile;
                    m << __typed_resp__->view_data;
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
            req->m >> __typed_req__.tid;
            req->m >> __typed_req__.dep_id;
            auto __typed_resp__ = std::make_shared<RpcAbortResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->slow;
                    m << __typed_resp__->coro_id;
                    m << __typed_resp__->profile;
                    m << __typed_resp__->view_data;
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
            req->m >> __typed_req__.tid;
            auto __typed_resp__ = std::make_shared<RpcEarlyAbortResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
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
            req->m >> __typed_req__.curr_epoch;
            auto __typed_resp__ = std::make_shared<RpcUpgradeEpochResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
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
            req->m >> __typed_req__.old_epoch;
            auto __typed_resp__ = std::make_shared<RpcTruncateEpochResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
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
            req->m >> __typed_req__.cur_pause;
            auto __typed_resp__ = std::make_shared<RpcIsLeaderResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->is_leader;
                },
                []() {});
            this->IsLeader(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __IsFPGALeader__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcIsFPGALeaderRequest __typed_req__;
            req->m >> __typed_req__.cur_pause;
            auto __typed_resp__ = std::make_shared<RpcIsFPGALeaderResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->is_leader;
                },
                []() {});
            this->IsFPGALeader(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SimpleCmd__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSimpleCmdRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcSimpleCmdResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                },
                []() {});
            this->FailoverResumeSocketOut(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __rpc_null__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRpcNullRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcRpcNullResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->rpc_null(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __TapirAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTapirAcceptRequest __typed_req__;
            req->m >> __typed_req__.cmd_id;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.decision;
            auto __typed_resp__ = std::make_shared<RpcTapirAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->TapirAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __TapirFastAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTapirFastAcceptRequest __typed_req__;
            req->m >> __typed_req__.cmd_id;
            req->m >> __typed_req__.txn_cmds;
            auto __typed_resp__ = std::make_shared<RpcTapirFastAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                },
                []() {});
            this->TapirFastAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __TapirDecide__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcTapirDecideRequest __typed_req__;
            req->m >> __typed_req__.cmd_id;
            req->m >> __typed_req__.commit;
            auto __typed_resp__ = std::make_shared<RpcTapirDecideResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->TapirDecide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __CarouselReadAndPrepare__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcCarouselReadAndPrepareRequest __typed_req__;
            req->m >> __typed_req__.tid;
            req->m >> __typed_req__.cmd;
            req->m >> __typed_req__.leader;
            auto __typed_resp__ = std::make_shared<RpcCarouselReadAndPrepareResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->output;
                },
                []() {});
            this->CarouselReadAndPrepare(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __CarouselAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcCarouselAcceptRequest __typed_req__;
            req->m >> __typed_req__.cmd_id;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.decision;
            auto __typed_resp__ = std::make_shared<RpcCarouselAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->CarouselAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __CarouselFastAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcCarouselFastAcceptRequest __typed_req__;
            req->m >> __typed_req__.cmd_id;
            req->m >> __typed_req__.txn_cmds;
            auto __typed_resp__ = std::make_shared<RpcCarouselFastAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                },
                []() {});
            this->CarouselFastAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __CarouselDecide__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcCarouselDecideRequest __typed_req__;
            req->m >> __typed_req__.cmd_id;
            req->m >> __typed_req__.commit;
            auto __typed_resp__ = std::make_shared<RpcCarouselDecideResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->CarouselDecide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccDispatch__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccDispatchRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcRccDispatchResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->output;
                    m << __typed_resp__->md_graph;
                },
                []() {});
            this->RccDispatch(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccFinish__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccFinishRequest __typed_req__;
            req->m >> __typed_req__.id;
            req->m >> __typed_req__.md_graph;
            auto __typed_resp__ = std::make_shared<RpcRccFinishResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->outputs;
                },
                []() {});
            this->RccFinish(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccInquire__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccInquireRequest __typed_req__;
            req->m >> __typed_req__.txn_id;
            req->m >> __typed_req__.rank;
            auto __typed_resp__ = std::make_shared<RpcRccInquireResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->out_0;
                },
                []() {});
            this->RccInquire(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccDispatchRo__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccDispatchRoRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcRccDispatchRoResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->output;
                },
                []() {});
            this->RccDispatchRo(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccInquireValidation__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccInquireValidationRequest __typed_req__;
            req->m >> __typed_req__.tx_id;
            req->m >> __typed_req__.rank;
            auto __typed_resp__ = std::make_shared<RpcRccInquireValidationResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                },
                []() {});
            this->RccInquireValidation(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccNotifyGlobalValidation__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccNotifyGlobalValidationRequest __typed_req__;
            req->m >> __typed_req__.tx_id;
            req->m >> __typed_req__.rank;
            req->m >> __typed_req__.res;
            auto __typed_resp__ = std::make_shared<RpcRccNotifyGlobalValidationResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                },
                []() {});
            this->RccNotifyGlobalValidation(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JanusDispatch__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJanusDispatchRequest __typed_req__;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcJanusDispatchResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->output;
                    m << __typed_resp__->ret_graph;
                },
                []() {});
            this->JanusDispatch(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccCommit__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccCommitRequest __typed_req__;
            req->m >> __typed_req__.id;
            req->m >> __typed_req__.rank;
            req->m >> __typed_req__.need_validation;
            req->m >> __typed_req__.parents;
            auto __typed_resp__ = std::make_shared<RpcRccCommitResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->output;
                },
                []() {});
            this->RccCommit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JanusCommit__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJanusCommitRequest __typed_req__;
            req->m >> __typed_req__.id;
            req->m >> __typed_req__.rank;
            req->m >> __typed_req__.need_validation;
            req->m >> __typed_req__.graph;
            auto __typed_resp__ = std::make_shared<RpcJanusCommitResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->output;
                },
                []() {});
            this->JanusCommit(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JanusCommitWoGraph__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJanusCommitWoGraphRequest __typed_req__;
            req->m >> __typed_req__.id;
            req->m >> __typed_req__.rank;
            req->m >> __typed_req__.need_validation;
            auto __typed_resp__ = std::make_shared<RpcJanusCommitWoGraphResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->output;
                },
                []() {});
            this->JanusCommitWoGraph(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JanusInquire__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJanusInquireRequest __typed_req__;
            req->m >> __typed_req__.epoch;
            req->m >> __typed_req__.txn_id;
            auto __typed_resp__ = std::make_shared<RpcJanusInquireResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ret_graph;
                },
                []() {});
            this->JanusInquire(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccPreAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccPreAcceptRequest __typed_req__;
            req->m >> __typed_req__.txn_id;
            req->m >> __typed_req__.rank;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcRccPreAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->x;
                },
                []() {});
            this->RccPreAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JanusPreAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJanusPreAcceptRequest __typed_req__;
            req->m >> __typed_req__.txn_id;
            req->m >> __typed_req__.rank;
            req->m >> __typed_req__.cmd;
            req->m >> __typed_req__.graph;
            auto __typed_resp__ = std::make_shared<RpcJanusPreAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->ret_graph;
                },
                []() {});
            this->JanusPreAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JanusPreAcceptWoGraph__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJanusPreAcceptWoGraphRequest __typed_req__;
            req->m >> __typed_req__.txn_id;
            req->m >> __typed_req__.rank;
            req->m >> __typed_req__.cmd;
            auto __typed_resp__ = std::make_shared<RpcJanusPreAcceptWoGraphResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                    m << __typed_resp__->ret_graph;
                },
                []() {});
            this->JanusPreAcceptWoGraph(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccAcceptRequest __typed_req__;
            req->m >> __typed_req__.txn_id;
            req->m >> __typed_req__.rank;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.p;
            auto __typed_resp__ = std::make_shared<RpcRccAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                },
                []() {});
            this->RccAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JanusAccept__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJanusAcceptRequest __typed_req__;
            req->m >> __typed_req__.txn_id;
            req->m >> __typed_req__.rank;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.graph;
            auto __typed_resp__ = std::make_shared<RpcJanusAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
                },
                []() {});
            this->JanusAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __PreAcceptFebruus__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPreAcceptFebruusRequest __typed_req__;
            req->m >> __typed_req__.tx_id;
            auto __typed_resp__ = std::make_shared<RpcPreAcceptFebruusResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ret;
                    m << __typed_resp__->timestamp;
                },
                []() {});
            this->PreAcceptFebruus(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __AcceptFebruus__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcAcceptFebruusRequest __typed_req__;
            req->m >> __typed_req__.tx_id;
            req->m >> __typed_req__.ballot;
            req->m >> __typed_req__.timestamp;
            auto __typed_resp__ = std::make_shared<RpcAcceptFebruusResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ret;
                },
                []() {});
            this->AcceptFebruus(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __CommitFebruus__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcCommitFebruusRequest __typed_req__;
            req->m >> __typed_req__.tx_id;
            req->m >> __typed_req__.timestamp;
            auto __typed_resp__ = std::make_shared<RpcCommitFebruusResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ret;
                },
                []() {});
            this->CommitFebruus(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackBeginRecovery__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackBeginRecoveryRequest __typed_req__;
            req->m >> __typed_req__.old_view;
            req->m >> __typed_req__.new_view;
            req->m >> __typed_req__.new_view_id;
            auto __typed_resp__ = std::make_shared<RpcJetpackBeginRecoveryResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
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
            req->m >> __typed_req__.jepoch;
            req->m >> __typed_req__.oepoch;
            auto __typed_resp__ = std::make_shared<RpcJetpackPullIdSetResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ok;
                    m << __typed_resp__->reply_jepoch;
                    m << __typed_resp__->reply_oepoch;
                    m << __typed_resp__->reply_old_view;
                    m << __typed_resp__->reply_new_view;
                    m << __typed_resp__->id_set;
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
            req->m >> __typed_req__.jepoch;
            req->m >> __typed_req__.oepoch;
            req->m >> __typed_req__.key_batch;
            auto __typed_resp__ = std::make_shared<RpcJetpackPullCmdResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ok;
                    m << __typed_resp__->reply_jepoch;
                    m << __typed_resp__->reply_oepoch;
                    m << __typed_resp__->reply_old_view;
                    m << __typed_resp__->reply_new_view;
                    m << __typed_resp__->cmd_batch;
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
            req->m >> __typed_req__.jepoch;
            req->m >> __typed_req__.oepoch;
            req->m >> __typed_req__.sid;
            req->m >> __typed_req__.rid;
            req->m >> __typed_req__.cmd_batch;
            auto __typed_resp__ = std::make_shared<RpcJetpackRecordCmdResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
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
            req->m >> __typed_req__.jepoch;
            req->m >> __typed_req__.oepoch;
            req->m >> __typed_req__.max_seen_ballot;
            auto __typed_resp__ = std::make_shared<RpcJetpackPrepareResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ok;
                    m << __typed_resp__->reply_jepoch;
                    m << __typed_resp__->reply_oepoch;
                    m << __typed_resp__->reply_old_view;
                    m << __typed_resp__->reply_new_view;
                    m << __typed_resp__->reply_max_seen_ballot;
                    m << __typed_resp__->accepted_ballot;
                    m << __typed_resp__->replied_sid;
                    m << __typed_resp__->replied_set_size;
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
            req->m >> __typed_req__.jepoch;
            req->m >> __typed_req__.oepoch;
            req->m >> __typed_req__.max_seen_ballot;
            req->m >> __typed_req__.sid;
            req->m >> __typed_req__.set_size;
            auto __typed_resp__ = std::make_shared<RpcJetpackAcceptResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ok;
                    m << __typed_resp__->reply_jepoch;
                    m << __typed_resp__->reply_oepoch;
                    m << __typed_resp__->reply_old_view;
                    m << __typed_resp__->reply_new_view;
                    m << __typed_resp__->reply_max_seen_ballot;
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
            req->m >> __typed_req__.jepoch;
            req->m >> __typed_req__.oepoch;
            req->m >> __typed_req__.sid;
            req->m >> __typed_req__.set_size;
            auto __typed_resp__ = std::make_shared<RpcJetpackCommitResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
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
            req->m >> __typed_req__.jepoch;
            req->m >> __typed_req__.oepoch;
            req->m >> __typed_req__.sid;
            req->m >> __typed_req__.rid;
            auto __typed_resp__ = std::make_shared<RpcJetpackPullRecSetInsResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->ok;
                    m << __typed_resp__->reply_jepoch;
                    m << __typed_resp__->reply_oepoch;
                    m << __typed_resp__->reply_old_view;
                    m << __typed_resp__->reply_new_view;
                    m << __typed_resp__->cmd;
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
            req->m >> __typed_req__.oepoch;
            auto __typed_resp__ = std::make_shared<RpcJetpackFinishRecoveryResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
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
    using RpcCarouselReadAndPrepareRequest = ClassicService::RpcCarouselReadAndPrepareRequest;
    using RpcCarouselReadAndPrepareResponse = ClassicService::RpcCarouselReadAndPrepareResponse;
    using RpcCarouselAcceptRequest = ClassicService::RpcCarouselAcceptRequest;
    using RpcCarouselAcceptResponse = ClassicService::RpcCarouselAcceptResponse;
    using RpcCarouselFastAcceptRequest = ClassicService::RpcCarouselFastAcceptRequest;
    using RpcCarouselFastAcceptResponse = ClassicService::RpcCarouselFastAcceptResponse;
    using RpcCarouselDecideRequest = ClassicService::RpcCarouselDecideRequest;
    using RpcCarouselDecideResponse = ClassicService::RpcCarouselDecideResponse;
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
    using RpcJanusDispatchRequest = ClassicService::RpcJanusDispatchRequest;
    using RpcJanusDispatchResponse = ClassicService::RpcJanusDispatchResponse;
    using RpcRccCommitRequest = ClassicService::RpcRccCommitRequest;
    using RpcRccCommitResponse = ClassicService::RpcRccCommitResponse;
    using RpcJanusCommitRequest = ClassicService::RpcJanusCommitRequest;
    using RpcJanusCommitResponse = ClassicService::RpcJanusCommitResponse;
    using RpcJanusCommitWoGraphRequest = ClassicService::RpcJanusCommitWoGraphRequest;
    using RpcJanusCommitWoGraphResponse = ClassicService::RpcJanusCommitWoGraphResponse;
    using RpcJanusInquireRequest = ClassicService::RpcJanusInquireRequest;
    using RpcJanusInquireResponse = ClassicService::RpcJanusInquireResponse;
    using RpcRccPreAcceptRequest = ClassicService::RpcRccPreAcceptRequest;
    using RpcRccPreAcceptResponse = ClassicService::RpcRccPreAcceptResponse;
    using RpcJanusPreAcceptRequest = ClassicService::RpcJanusPreAcceptRequest;
    using RpcJanusPreAcceptResponse = ClassicService::RpcJanusPreAcceptResponse;
    using RpcJanusPreAcceptWoGraphRequest = ClassicService::RpcJanusPreAcceptWoGraphRequest;
    using RpcJanusPreAcceptWoGraphResponse = ClassicService::RpcJanusPreAcceptWoGraphResponse;
    using RpcRccAcceptRequest = ClassicService::RpcRccAcceptRequest;
    using RpcRccAcceptResponse = ClassicService::RpcRccAcceptResponse;
    using RpcJanusAcceptRequest = ClassicService::RpcJanusAcceptRequest;
    using RpcJanusAcceptResponse = ClassicService::RpcJanusAcceptResponse;
    using RpcPreAcceptFebruusRequest = ClassicService::RpcPreAcceptFebruusRequest;
    using RpcPreAcceptFebruusResponse = ClassicService::RpcPreAcceptFebruusResponse;
    using RpcAcceptFebruusRequest = ClassicService::RpcAcceptFebruusRequest;
    using RpcAcceptFebruusResponse = ClassicService::RpcAcceptFebruusResponse;
    using RpcCommitFebruusRequest = ClassicService::RpcCommitFebruusRequest;
    using RpcCommitFebruusResponse = ClassicService::RpcCommitFebruusResponse;
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
            __fu__->get_reply() >> __typed_resp__.ret;
            return rusty::Result<RpcMsgStringResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<MsgStringTypedFuture, rrr::i32> async_MsgString(const RpcMsgStringRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::MSGSTRING, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.arg;
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
    [[deprecated("use typed async_MsgString(const RpcMsgStringRequest&) instead")]]
    rrr::FutureResult async_MsgString(const std::string& arg, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcMsgStringRequest __req__;
        __req__.arg = arg;
        auto __typed_result__ = this->async_MsgString(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed MsgString(const RpcMsgStringRequest&) instead")]]
    rrr::i32 MsgString(const std::string& arg, std::string* ret) {
        RpcMsgStringRequest __req__;
        __req__.arg = arg;
        auto __typed_result__ = this->MsgString(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ret) *ret = __resp__.ret;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ret;
            return rusty::Result<RpcMsgMarshallResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<MsgMarshallTypedFuture, rrr::i32> async_MsgMarshall(const RpcMsgMarshallRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::MSGMARSHALL, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.arg;
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
    [[deprecated("use typed async_MsgMarshall(const RpcMsgMarshallRequest&) instead")]]
    rrr::FutureResult async_MsgMarshall(const MarshallDeputy& arg, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcMsgMarshallRequest __req__;
        __req__.arg = arg;
        auto __typed_result__ = this->async_MsgMarshall(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed MsgMarshall(const RpcMsgMarshallRequest&) instead")]]
    rrr::i32 MsgMarshall(const MarshallDeputy& arg, MarshallDeputy* ret) {
        RpcMsgMarshallRequest __req__;
        __req__.arg = arg;
        auto __typed_result__ = this->MsgMarshall(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ret) *ret = __resp__.ret;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.success;
            return rusty::Result<RpcReElectResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<ReElectTypedFuture, rrr::i32> async_ReElect(const RpcReElectRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::REELECT, __fu_attr__);
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
    [[deprecated("use typed async_ReElect(const RpcReElectRequest&) instead")]]
    rrr::FutureResult async_ReElect(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcReElectRequest __req__;
        auto __typed_result__ = this->async_ReElect(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed ReElect(const RpcReElectRequest&) instead")]]
    rrr::i32 ReElect(bool_t* success) {
        RpcReElectRequest __req__;
        auto __typed_result__ = this->ReElect(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (success) *success = __resp__.success;
        return 0;
    }
    class RuleSpeculativeExecuteTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RuleSpeculativeExecuteTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRuleSpeculativeExecuteResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRuleSpeculativeExecuteResponse, rrr::i32>::Err(__ret__);
            }
            RpcRuleSpeculativeExecuteResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.accepted;
            __fu__->get_reply() >> __typed_resp__.result;
            __fu__->get_reply() >> __typed_resp__.is_leader;
            return rusty::Result<RpcRuleSpeculativeExecuteResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RuleSpeculativeExecuteTypedFuture, rrr::i32> async_RuleSpeculativeExecute(const RpcRuleSpeculativeExecuteRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RULESPECULATIVEEXECUTE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.md;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RuleSpeculativeExecuteTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RuleSpeculativeExecuteTypedFuture, rrr::i32>::Ok(RuleSpeculativeExecuteTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRuleSpeculativeExecuteResponse, rrr::i32> RuleSpeculativeExecute(const RpcRuleSpeculativeExecuteRequest& req) {
        auto __typed_fu_result__ = this->async_RuleSpeculativeExecute(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRuleSpeculativeExecuteResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_RuleSpeculativeExecute(const RpcRuleSpeculativeExecuteRequest&) instead")]]
    rrr::FutureResult async_RuleSpeculativeExecute(const MarshallDeputy& md, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRuleSpeculativeExecuteRequest __req__;
        __req__.md = md;
        auto __typed_result__ = this->async_RuleSpeculativeExecute(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed RuleSpeculativeExecute(const RpcRuleSpeculativeExecuteRequest&) instead")]]
    rrr::i32 RuleSpeculativeExecute(const MarshallDeputy& md, bool_t* accepted, int32_t* result, bool_t* is_leader) {
        RpcRuleSpeculativeExecuteRequest __req__;
        __req__.md = md;
        auto __typed_result__ = this->RuleSpeculativeExecute(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (accepted) *accepted = __resp__.accepted;
        if (result) *result = __resp__.result;
        if (is_leader) *is_leader = __resp__.is_leader;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.output;
            __fu__->get_reply() >> __typed_resp__.coro_id;
            __fu__->get_reply() >> __typed_resp__.view_data;
            return rusty::Result<RpcDispatchResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<DispatchTypedFuture, rrr::i32> async_Dispatch(const RpcDispatchRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::DISPATCH, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.tid;
            __m__ << req.dep_id;
            __m__ << req.cmd;
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
    [[deprecated("use typed async_Dispatch(const RpcDispatchRequest&) instead")]]
    rrr::FutureResult async_Dispatch(const rrr::i64& tid, const DepId& dep_id, const MarshallDeputy& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcDispatchRequest __req__;
        __req__.tid = tid;
        __req__.dep_id = dep_id;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_Dispatch(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Dispatch(const RpcDispatchRequest&) instead")]]
    rrr::i32 Dispatch(const rrr::i64& tid, const DepId& dep_id, const MarshallDeputy& cmd, rrr::i32* res, TxnOutput* output, uint64_t* coro_id, MarshallDeputy* view_data) {
        RpcDispatchRequest __req__;
        __req__.tid = tid;
        __req__.dep_id = dep_id;
        __req__.cmd = cmd;
        auto __typed_result__ = this->Dispatch(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (output) *output = __resp__.output;
        if (coro_id) *coro_id = __resp__.coro_id;
        if (view_data) *view_data = __resp__.view_data;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.slow;
            __fu__->get_reply() >> __typed_resp__.coro_id;
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<PrepareTypedFuture, rrr::i32> async_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::PREPARE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.tid;
            __m__ << req.sids;
            __m__ << req.dep_id;
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
    [[deprecated("use typed async_Prepare(const RpcPrepareRequest&) instead")]]
    rrr::FutureResult async_Prepare(const rrr::i64& tid, const std::vector<rrr::i32>& sids, const DepId& dep_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcPrepareRequest __req__;
        __req__.tid = tid;
        __req__.sids = sids;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->async_Prepare(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Prepare(const RpcPrepareRequest&) instead")]]
    rrr::i32 Prepare(const rrr::i64& tid, const std::vector<rrr::i32>& sids, const DepId& dep_id, rrr::i32* res, bool_t* slow, uint64_t* coro_id) {
        RpcPrepareRequest __req__;
        __req__.tid = tid;
        __req__.sids = sids;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->Prepare(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (slow) *slow = __resp__.slow;
        if (coro_id) *coro_id = __resp__.coro_id;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.slow;
            __fu__->get_reply() >> __typed_resp__.coro_id;
            __fu__->get_reply() >> __typed_resp__.profile;
            __fu__->get_reply() >> __typed_resp__.view_data;
            return rusty::Result<RpcCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<CommitTypedFuture, rrr::i32> async_Commit(const RpcCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::COMMIT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.tid;
            __m__ << req.dep_id;
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
    [[deprecated("use typed async_Commit(const RpcCommitRequest&) instead")]]
    rrr::FutureResult async_Commit(const rrr::i64& tid, const DepId& dep_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcCommitRequest __req__;
        __req__.tid = tid;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->async_Commit(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Commit(const RpcCommitRequest&) instead")]]
    rrr::i32 Commit(const rrr::i64& tid, const DepId& dep_id, rrr::i32* res, bool_t* slow, uint64_t* coro_id, Profiling* profile, MarshallDeputy* view_data) {
        RpcCommitRequest __req__;
        __req__.tid = tid;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->Commit(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (slow) *slow = __resp__.slow;
        if (coro_id) *coro_id = __resp__.coro_id;
        if (profile) *profile = __resp__.profile;
        if (view_data) *view_data = __resp__.view_data;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.slow;
            __fu__->get_reply() >> __typed_resp__.coro_id;
            __fu__->get_reply() >> __typed_resp__.profile;
            __fu__->get_reply() >> __typed_resp__.view_data;
            return rusty::Result<RpcAbortResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AbortTypedFuture, rrr::i32> async_Abort(const RpcAbortRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::ABORT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.tid;
            __m__ << req.dep_id;
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
    [[deprecated("use typed async_Abort(const RpcAbortRequest&) instead")]]
    rrr::FutureResult async_Abort(const rrr::i64& tid, const DepId& dep_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcAbortRequest __req__;
        __req__.tid = tid;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->async_Abort(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed Abort(const RpcAbortRequest&) instead")]]
    rrr::i32 Abort(const rrr::i64& tid, const DepId& dep_id, rrr::i32* res, bool_t* slow, uint64_t* coro_id, Profiling* profile, MarshallDeputy* view_data) {
        RpcAbortRequest __req__;
        __req__.tid = tid;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->Abort(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (slow) *slow = __resp__.slow;
        if (coro_id) *coro_id = __resp__.coro_id;
        if (profile) *profile = __resp__.profile;
        if (view_data) *view_data = __resp__.view_data;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcEarlyAbortResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<EarlyAbortTypedFuture, rrr::i32> async_EarlyAbort(const RpcEarlyAbortRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::EARLYABORT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.tid;
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
    [[deprecated("use typed async_EarlyAbort(const RpcEarlyAbortRequest&) instead")]]
    rrr::FutureResult async_EarlyAbort(const rrr::i64& tid, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcEarlyAbortRequest __req__;
        __req__.tid = tid;
        auto __typed_result__ = this->async_EarlyAbort(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed EarlyAbort(const RpcEarlyAbortRequest&) instead")]]
    rrr::i32 EarlyAbort(const rrr::i64& tid, rrr::i32* res) {
        RpcEarlyAbortRequest __req__;
        __req__.tid = tid;
        auto __typed_result__ = this->EarlyAbort(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcUpgradeEpochResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<UpgradeEpochTypedFuture, rrr::i32> async_UpgradeEpoch(const RpcUpgradeEpochRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::UPGRADEEPOCH, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.curr_epoch;
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
    [[deprecated("use typed async_UpgradeEpoch(const RpcUpgradeEpochRequest&) instead")]]
    rrr::FutureResult async_UpgradeEpoch(const uint32_t& curr_epoch, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcUpgradeEpochRequest __req__;
        __req__.curr_epoch = curr_epoch;
        auto __typed_result__ = this->async_UpgradeEpoch(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed UpgradeEpoch(const RpcUpgradeEpochRequest&) instead")]]
    rrr::i32 UpgradeEpoch(const uint32_t& curr_epoch, int32_t* res) {
        RpcUpgradeEpochRequest __req__;
        __req__.curr_epoch = curr_epoch;
        auto __typed_result__ = this->UpgradeEpoch(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
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
        auto __fu_result__ = __cl__->request(ClassicService::TRUNCATEEPOCH, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.old_epoch;
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
    [[deprecated("use typed async_TruncateEpoch(const RpcTruncateEpochRequest&) instead")]]
    rrr::FutureResult async_TruncateEpoch(const uint32_t& old_epoch, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcTruncateEpochRequest __req__;
        __req__.old_epoch = old_epoch;
        auto __typed_result__ = this->async_TruncateEpoch(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed TruncateEpoch(const RpcTruncateEpochRequest&) instead")]]
    rrr::i32 TruncateEpoch(const uint32_t& old_epoch) {
        RpcTruncateEpochRequest __req__;
        __req__.old_epoch = old_epoch;
        auto __typed_result__ = this->TruncateEpoch(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.is_leader;
            return rusty::Result<RpcIsLeaderResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<IsLeaderTypedFuture, rrr::i32> async_IsLeader(const RpcIsLeaderRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::ISLEADER, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cur_pause;
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
    [[deprecated("use typed async_IsLeader(const RpcIsLeaderRequest&) instead")]]
    rrr::FutureResult async_IsLeader(const locid_t& cur_pause, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcIsLeaderRequest __req__;
        __req__.cur_pause = cur_pause;
        auto __typed_result__ = this->async_IsLeader(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed IsLeader(const RpcIsLeaderRequest&) instead")]]
    rrr::i32 IsLeader(const locid_t& cur_pause, bool_t* is_leader) {
        RpcIsLeaderRequest __req__;
        __req__.cur_pause = cur_pause;
        auto __typed_result__ = this->IsLeader(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (is_leader) *is_leader = __resp__.is_leader;
        return 0;
    }
    class IsFPGALeaderTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit IsFPGALeaderTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcIsFPGALeaderResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcIsFPGALeaderResponse, rrr::i32>::Err(__ret__);
            }
            RpcIsFPGALeaderResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.is_leader;
            return rusty::Result<RpcIsFPGALeaderResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<IsFPGALeaderTypedFuture, rrr::i32> async_IsFPGALeader(const RpcIsFPGALeaderRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::ISFPGALEADER, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cur_pause;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<IsFPGALeaderTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<IsFPGALeaderTypedFuture, rrr::i32>::Ok(IsFPGALeaderTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcIsFPGALeaderResponse, rrr::i32> IsFPGALeader(const RpcIsFPGALeaderRequest& req) {
        auto __typed_fu_result__ = this->async_IsFPGALeader(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcIsFPGALeaderResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_IsFPGALeader(const RpcIsFPGALeaderRequest&) instead")]]
    rrr::FutureResult async_IsFPGALeader(const locid_t& cur_pause, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcIsFPGALeaderRequest __req__;
        __req__.cur_pause = cur_pause;
        auto __typed_result__ = this->async_IsFPGALeader(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed IsFPGALeader(const RpcIsFPGALeaderRequest&) instead")]]
    rrr::i32 IsFPGALeader(const locid_t& cur_pause, bool_t* is_leader) {
        RpcIsFPGALeaderRequest __req__;
        __req__.cur_pause = cur_pause;
        auto __typed_result__ = this->IsFPGALeader(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (is_leader) *is_leader = __resp__.is_leader;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcSimpleCmdResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SimpleCmdTypedFuture, rrr::i32> async_SimpleCmd(const RpcSimpleCmdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::SIMPLECMD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
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
    [[deprecated("use typed async_SimpleCmd(const RpcSimpleCmdRequest&) instead")]]
    rrr::FutureResult async_SimpleCmd(const SimpleCommand& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcSimpleCmdRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_SimpleCmd(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed SimpleCmd(const RpcSimpleCmdRequest&) instead")]]
    rrr::i32 SimpleCmd(const SimpleCommand& cmd, rrr::i32* res) {
        RpcSimpleCmdRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->SimpleCmd(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcFailoverPauseSocketOutResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<FailoverPauseSocketOutTypedFuture, rrr::i32> async_FailoverPauseSocketOut(const RpcFailoverPauseSocketOutRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::FAILOVERPAUSESOCKETOUT, __fu_attr__);
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
    [[deprecated("use typed async_FailoverPauseSocketOut(const RpcFailoverPauseSocketOutRequest&) instead")]]
    rrr::FutureResult async_FailoverPauseSocketOut(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcFailoverPauseSocketOutRequest __req__;
        auto __typed_result__ = this->async_FailoverPauseSocketOut(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed FailoverPauseSocketOut(const RpcFailoverPauseSocketOutRequest&) instead")]]
    rrr::i32 FailoverPauseSocketOut(rrr::i32* res) {
        RpcFailoverPauseSocketOutRequest __req__;
        auto __typed_result__ = this->FailoverPauseSocketOut(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcFailoverResumeSocketOutResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<FailoverResumeSocketOutTypedFuture, rrr::i32> async_FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::FAILOVERRESUMESOCKETOUT, __fu_attr__);
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
    [[deprecated("use typed async_FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest&) instead")]]
    rrr::FutureResult async_FailoverResumeSocketOut(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcFailoverResumeSocketOutRequest __req__;
        auto __typed_result__ = this->async_FailoverResumeSocketOut(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest&) instead")]]
    rrr::i32 FailoverResumeSocketOut(rrr::i32* res) {
        RpcFailoverResumeSocketOutRequest __req__;
        auto __typed_result__ = this->FailoverResumeSocketOut(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
    }
    class rpc_nullTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit rpc_nullTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRpcNullResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRpcNullResponse, rrr::i32>::Err(__ret__);
            }
            RpcRpcNullResponse __typed_resp__;
            return rusty::Result<RpcRpcNullResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<rpc_nullTypedFuture, rrr::i32> async_rpc_null(const RpcRpcNullRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RPC_NULL, __fu_attr__);
        if (__fu_result__.is_err()) {
            return rusty::Result<rpc_nullTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<rpc_nullTypedFuture, rrr::i32>::Ok(rpc_nullTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRpcNullResponse, rrr::i32> rpc_null(const RpcRpcNullRequest& req) {
        auto __typed_fu_result__ = this->async_rpc_null(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRpcNullResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_rpc_null(const RpcRpcNullRequest&) instead")]]
    rrr::FutureResult async_rpc_null(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRpcNullRequest __req__;
        auto __typed_result__ = this->async_rpc_null(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed rpc_null(const RpcRpcNullRequest&) instead")]]
    rrr::i32 rpc_null() {
        RpcRpcNullRequest __req__;
        auto __typed_result__ = this->rpc_null(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
    class TapirAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit TapirAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcTapirAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTapirAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcTapirAcceptResponse __typed_resp__;
            return rusty::Result<RpcTapirAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TapirAcceptTypedFuture, rrr::i32> async_TapirAccept(const RpcTapirAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::TAPIRACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd_id;
            __m__ << req.ballot;
            __m__ << req.decision;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TapirAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TapirAcceptTypedFuture, rrr::i32>::Ok(TapirAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTapirAcceptResponse, rrr::i32> TapirAccept(const RpcTapirAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_TapirAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTapirAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_TapirAccept(const RpcTapirAcceptRequest&) instead")]]
    rrr::FutureResult async_TapirAccept(const uint64_t& cmd_id, const int64_t& ballot, const int32_t& decision, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcTapirAcceptRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.ballot = ballot;
        __req__.decision = decision;
        auto __typed_result__ = this->async_TapirAccept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed TapirAccept(const RpcTapirAcceptRequest&) instead")]]
    rrr::i32 TapirAccept(const uint64_t& cmd_id, const int64_t& ballot, const int32_t& decision) {
        RpcTapirAcceptRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.ballot = ballot;
        __req__.decision = decision;
        auto __typed_result__ = this->TapirAccept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
    class TapirFastAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit TapirFastAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcTapirFastAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTapirFastAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcTapirFastAcceptResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcTapirFastAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TapirFastAcceptTypedFuture, rrr::i32> async_TapirFastAccept(const RpcTapirFastAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::TAPIRFASTACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd_id;
            __m__ << req.txn_cmds;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TapirFastAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TapirFastAcceptTypedFuture, rrr::i32>::Ok(TapirFastAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTapirFastAcceptResponse, rrr::i32> TapirFastAccept(const RpcTapirFastAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_TapirFastAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTapirFastAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_TapirFastAccept(const RpcTapirFastAcceptRequest&) instead")]]
    rrr::FutureResult async_TapirFastAccept(const uint64_t& cmd_id, const std::vector<SimpleCommand>& txn_cmds, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcTapirFastAcceptRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.txn_cmds = txn_cmds;
        auto __typed_result__ = this->async_TapirFastAccept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed TapirFastAccept(const RpcTapirFastAcceptRequest&) instead")]]
    rrr::i32 TapirFastAccept(const uint64_t& cmd_id, const std::vector<SimpleCommand>& txn_cmds, rrr::i32* res) {
        RpcTapirFastAcceptRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.txn_cmds = txn_cmds;
        auto __typed_result__ = this->TapirFastAccept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
    }
    class TapirDecideTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit TapirDecideTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcTapirDecideResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcTapirDecideResponse, rrr::i32>::Err(__ret__);
            }
            RpcTapirDecideResponse __typed_resp__;
            return rusty::Result<RpcTapirDecideResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<TapirDecideTypedFuture, rrr::i32> async_TapirDecide(const RpcTapirDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::TAPIRDECIDE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd_id;
            __m__ << req.commit;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TapirDecideTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TapirDecideTypedFuture, rrr::i32>::Ok(TapirDecideTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcTapirDecideResponse, rrr::i32> TapirDecide(const RpcTapirDecideRequest& req) {
        auto __typed_fu_result__ = this->async_TapirDecide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTapirDecideResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_TapirDecide(const RpcTapirDecideRequest&) instead")]]
    rrr::FutureResult async_TapirDecide(const uint64_t& cmd_id, const rrr::i32& commit, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcTapirDecideRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.commit = commit;
        auto __typed_result__ = this->async_TapirDecide(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed TapirDecide(const RpcTapirDecideRequest&) instead")]]
    rrr::i32 TapirDecide(const uint64_t& cmd_id, const rrr::i32& commit) {
        RpcTapirDecideRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.commit = commit;
        auto __typed_result__ = this->TapirDecide(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
    class CarouselReadAndPrepareTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit CarouselReadAndPrepareTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcCarouselReadAndPrepareResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcCarouselReadAndPrepareResponse, rrr::i32>::Err(__ret__);
            }
            RpcCarouselReadAndPrepareResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.output;
            return rusty::Result<RpcCarouselReadAndPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<CarouselReadAndPrepareTypedFuture, rrr::i32> async_CarouselReadAndPrepare(const RpcCarouselReadAndPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::CAROUSELREADANDPREPARE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.tid;
            __m__ << req.cmd;
            __m__ << req.leader;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<CarouselReadAndPrepareTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<CarouselReadAndPrepareTypedFuture, rrr::i32>::Ok(CarouselReadAndPrepareTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcCarouselReadAndPrepareResponse, rrr::i32> CarouselReadAndPrepare(const RpcCarouselReadAndPrepareRequest& req) {
        auto __typed_fu_result__ = this->async_CarouselReadAndPrepare(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcCarouselReadAndPrepareResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_CarouselReadAndPrepare(const RpcCarouselReadAndPrepareRequest&) instead")]]
    rrr::FutureResult async_CarouselReadAndPrepare(const rrr::i64& tid, const MarshallDeputy& cmd, const bool_t& leader, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcCarouselReadAndPrepareRequest __req__;
        __req__.tid = tid;
        __req__.cmd = cmd;
        __req__.leader = leader;
        auto __typed_result__ = this->async_CarouselReadAndPrepare(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed CarouselReadAndPrepare(const RpcCarouselReadAndPrepareRequest&) instead")]]
    rrr::i32 CarouselReadAndPrepare(const rrr::i64& tid, const MarshallDeputy& cmd, const bool_t& leader, rrr::i32* res, TxnOutput* output) {
        RpcCarouselReadAndPrepareRequest __req__;
        __req__.tid = tid;
        __req__.cmd = cmd;
        __req__.leader = leader;
        auto __typed_result__ = this->CarouselReadAndPrepare(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (output) *output = __resp__.output;
        return 0;
    }
    class CarouselAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit CarouselAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcCarouselAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcCarouselAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcCarouselAcceptResponse __typed_resp__;
            return rusty::Result<RpcCarouselAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<CarouselAcceptTypedFuture, rrr::i32> async_CarouselAccept(const RpcCarouselAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::CAROUSELACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd_id;
            __m__ << req.ballot;
            __m__ << req.decision;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<CarouselAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<CarouselAcceptTypedFuture, rrr::i32>::Ok(CarouselAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcCarouselAcceptResponse, rrr::i32> CarouselAccept(const RpcCarouselAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_CarouselAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcCarouselAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_CarouselAccept(const RpcCarouselAcceptRequest&) instead")]]
    rrr::FutureResult async_CarouselAccept(const uint64_t& cmd_id, const int64_t& ballot, const int32_t& decision, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcCarouselAcceptRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.ballot = ballot;
        __req__.decision = decision;
        auto __typed_result__ = this->async_CarouselAccept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed CarouselAccept(const RpcCarouselAcceptRequest&) instead")]]
    rrr::i32 CarouselAccept(const uint64_t& cmd_id, const int64_t& ballot, const int32_t& decision) {
        RpcCarouselAcceptRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.ballot = ballot;
        __req__.decision = decision;
        auto __typed_result__ = this->CarouselAccept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
    class CarouselFastAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit CarouselFastAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcCarouselFastAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcCarouselFastAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcCarouselFastAcceptResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcCarouselFastAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<CarouselFastAcceptTypedFuture, rrr::i32> async_CarouselFastAccept(const RpcCarouselFastAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::CAROUSELFASTACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd_id;
            __m__ << req.txn_cmds;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<CarouselFastAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<CarouselFastAcceptTypedFuture, rrr::i32>::Ok(CarouselFastAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcCarouselFastAcceptResponse, rrr::i32> CarouselFastAccept(const RpcCarouselFastAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_CarouselFastAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcCarouselFastAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_CarouselFastAccept(const RpcCarouselFastAcceptRequest&) instead")]]
    rrr::FutureResult async_CarouselFastAccept(const uint64_t& cmd_id, const std::vector<SimpleCommand>& txn_cmds, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcCarouselFastAcceptRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.txn_cmds = txn_cmds;
        auto __typed_result__ = this->async_CarouselFastAccept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed CarouselFastAccept(const RpcCarouselFastAcceptRequest&) instead")]]
    rrr::i32 CarouselFastAccept(const uint64_t& cmd_id, const std::vector<SimpleCommand>& txn_cmds, rrr::i32* res) {
        RpcCarouselFastAcceptRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.txn_cmds = txn_cmds;
        auto __typed_result__ = this->CarouselFastAccept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
    }
    class CarouselDecideTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit CarouselDecideTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcCarouselDecideResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcCarouselDecideResponse, rrr::i32>::Err(__ret__);
            }
            RpcCarouselDecideResponse __typed_resp__;
            return rusty::Result<RpcCarouselDecideResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<CarouselDecideTypedFuture, rrr::i32> async_CarouselDecide(const RpcCarouselDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::CAROUSELDECIDE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd_id;
            __m__ << req.commit;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<CarouselDecideTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<CarouselDecideTypedFuture, rrr::i32>::Ok(CarouselDecideTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcCarouselDecideResponse, rrr::i32> CarouselDecide(const RpcCarouselDecideRequest& req) {
        auto __typed_fu_result__ = this->async_CarouselDecide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcCarouselDecideResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_CarouselDecide(const RpcCarouselDecideRequest&) instead")]]
    rrr::FutureResult async_CarouselDecide(const uint64_t& cmd_id, const rrr::i32& commit, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcCarouselDecideRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.commit = commit;
        auto __typed_result__ = this->async_CarouselDecide(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed CarouselDecide(const RpcCarouselDecideRequest&) instead")]]
    rrr::i32 CarouselDecide(const uint64_t& cmd_id, const rrr::i32& commit) {
        RpcCarouselDecideRequest __req__;
        __req__.cmd_id = cmd_id;
        __req__.commit = commit;
        auto __typed_result__ = this->CarouselDecide(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
    class RccDispatchTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RccDispatchTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRccDispatchResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccDispatchResponse, rrr::i32>::Err(__ret__);
            }
            RpcRccDispatchResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.output;
            __fu__->get_reply() >> __typed_resp__.md_graph;
            return rusty::Result<RpcRccDispatchResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccDispatchTypedFuture, rrr::i32> async_RccDispatch(const RpcRccDispatchRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCDISPATCH, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccDispatchTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccDispatchTypedFuture, rrr::i32>::Ok(RccDispatchTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccDispatchResponse, rrr::i32> RccDispatch(const RpcRccDispatchRequest& req) {
        auto __typed_fu_result__ = this->async_RccDispatch(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccDispatchResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_RccDispatch(const RpcRccDispatchRequest&) instead")]]
    rrr::FutureResult async_RccDispatch(const std::vector<SimpleCommand>& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRccDispatchRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_RccDispatch(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed RccDispatch(const RpcRccDispatchRequest&) instead")]]
    rrr::i32 RccDispatch(const std::vector<SimpleCommand>& cmd, rrr::i32* res, TxnOutput* output, MarshallDeputy* md_graph) {
        RpcRccDispatchRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->RccDispatch(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (output) *output = __resp__.output;
        if (md_graph) *md_graph = __resp__.md_graph;
        return 0;
    }
    class RccFinishTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RccFinishTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRccFinishResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccFinishResponse, rrr::i32>::Err(__ret__);
            }
            RpcRccFinishResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.outputs;
            return rusty::Result<RpcRccFinishResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccFinishTypedFuture, rrr::i32> async_RccFinish(const RpcRccFinishRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCFINISH, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.id;
            __m__ << req.md_graph;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccFinishTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccFinishTypedFuture, rrr::i32>::Ok(RccFinishTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccFinishResponse, rrr::i32> RccFinish(const RpcRccFinishRequest& req) {
        auto __typed_fu_result__ = this->async_RccFinish(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccFinishResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_RccFinish(const RpcRccFinishRequest&) instead")]]
    rrr::FutureResult async_RccFinish(const cmdid_t& id, const MarshallDeputy& md_graph, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRccFinishRequest __req__;
        __req__.id = id;
        __req__.md_graph = md_graph;
        auto __typed_result__ = this->async_RccFinish(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed RccFinish(const RpcRccFinishRequest&) instead")]]
    rrr::i32 RccFinish(const cmdid_t& id, const MarshallDeputy& md_graph, std::map<uint32_t, std::map<int32_t, Value>>* outputs) {
        RpcRccFinishRequest __req__;
        __req__.id = id;
        __req__.md_graph = md_graph;
        auto __typed_result__ = this->RccFinish(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (outputs) *outputs = __resp__.outputs;
        return 0;
    }
    class RccInquireTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RccInquireTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRccInquireResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccInquireResponse, rrr::i32>::Err(__ret__);
            }
            RpcRccInquireResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.out_0;
            return rusty::Result<RpcRccInquireResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccInquireTypedFuture, rrr::i32> async_RccInquire(const RpcRccInquireRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCINQUIRE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.txn_id;
            __m__ << req.rank;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccInquireTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccInquireTypedFuture, rrr::i32>::Ok(RccInquireTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccInquireResponse, rrr::i32> RccInquire(const RpcRccInquireRequest& req) {
        auto __typed_fu_result__ = this->async_RccInquire(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccInquireResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_RccInquire(const RpcRccInquireRequest&) instead")]]
    rrr::FutureResult async_RccInquire(const txnid_t& txn_id, const int32_t& rank, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRccInquireRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        auto __typed_result__ = this->async_RccInquire(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed RccInquire(const RpcRccInquireRequest&) instead")]]
    rrr::i32 RccInquire(const txnid_t& txn_id, const int32_t& rank, std::map<uint64_t, parent_set_t>* out_0) {
        RpcRccInquireRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        auto __typed_result__ = this->RccInquire(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (out_0) *out_0 = __resp__.out_0;
        return 0;
    }
    class RccDispatchRoTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RccDispatchRoTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRccDispatchRoResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccDispatchRoResponse, rrr::i32>::Err(__ret__);
            }
            RpcRccDispatchRoResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.output;
            return rusty::Result<RpcRccDispatchRoResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccDispatchRoTypedFuture, rrr::i32> async_RccDispatchRo(const RpcRccDispatchRoRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCDISPATCHRO, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccDispatchRoTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccDispatchRoTypedFuture, rrr::i32>::Ok(RccDispatchRoTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccDispatchRoResponse, rrr::i32> RccDispatchRo(const RpcRccDispatchRoRequest& req) {
        auto __typed_fu_result__ = this->async_RccDispatchRo(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccDispatchRoResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_RccDispatchRo(const RpcRccDispatchRoRequest&) instead")]]
    rrr::FutureResult async_RccDispatchRo(const SimpleCommand& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRccDispatchRoRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_RccDispatchRo(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed RccDispatchRo(const RpcRccDispatchRoRequest&) instead")]]
    rrr::i32 RccDispatchRo(const SimpleCommand& cmd, std::map<rrr::i32, Value>* output) {
        RpcRccDispatchRoRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->RccDispatchRo(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (output) *output = __resp__.output;
        return 0;
    }
    class RccInquireValidationTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RccInquireValidationTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRccInquireValidationResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccInquireValidationResponse, rrr::i32>::Err(__ret__);
            }
            RpcRccInquireValidationResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcRccInquireValidationResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccInquireValidationTypedFuture, rrr::i32> async_RccInquireValidation(const RpcRccInquireValidationRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCINQUIREVALIDATION, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.tx_id;
            __m__ << req.rank;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccInquireValidationTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccInquireValidationTypedFuture, rrr::i32>::Ok(RccInquireValidationTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccInquireValidationResponse, rrr::i32> RccInquireValidation(const RpcRccInquireValidationRequest& req) {
        auto __typed_fu_result__ = this->async_RccInquireValidation(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccInquireValidationResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_RccInquireValidation(const RpcRccInquireValidationRequest&) instead")]]
    rrr::FutureResult async_RccInquireValidation(const txid_t& tx_id, const int32_t& rank, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRccInquireValidationRequest __req__;
        __req__.tx_id = tx_id;
        __req__.rank = rank;
        auto __typed_result__ = this->async_RccInquireValidation(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed RccInquireValidation(const RpcRccInquireValidationRequest&) instead")]]
    rrr::i32 RccInquireValidation(const txid_t& tx_id, const int32_t& rank, int32_t* res) {
        RpcRccInquireValidationRequest __req__;
        __req__.tx_id = tx_id;
        __req__.rank = rank;
        auto __typed_result__ = this->RccInquireValidation(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
    }
    class RccNotifyGlobalValidationTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RccNotifyGlobalValidationTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRccNotifyGlobalValidationResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccNotifyGlobalValidationResponse, rrr::i32>::Err(__ret__);
            }
            RpcRccNotifyGlobalValidationResponse __typed_resp__;
            return rusty::Result<RpcRccNotifyGlobalValidationResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccNotifyGlobalValidationTypedFuture, rrr::i32> async_RccNotifyGlobalValidation(const RpcRccNotifyGlobalValidationRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCNOTIFYGLOBALVALIDATION, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.tx_id;
            __m__ << req.rank;
            __m__ << req.res;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccNotifyGlobalValidationTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccNotifyGlobalValidationTypedFuture, rrr::i32>::Ok(RccNotifyGlobalValidationTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccNotifyGlobalValidationResponse, rrr::i32> RccNotifyGlobalValidation(const RpcRccNotifyGlobalValidationRequest& req) {
        auto __typed_fu_result__ = this->async_RccNotifyGlobalValidation(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccNotifyGlobalValidationResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_RccNotifyGlobalValidation(const RpcRccNotifyGlobalValidationRequest&) instead")]]
    rrr::FutureResult async_RccNotifyGlobalValidation(const txid_t& tx_id, const int32_t& rank, const int32_t& res, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRccNotifyGlobalValidationRequest __req__;
        __req__.tx_id = tx_id;
        __req__.rank = rank;
        __req__.res = res;
        auto __typed_result__ = this->async_RccNotifyGlobalValidation(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed RccNotifyGlobalValidation(const RpcRccNotifyGlobalValidationRequest&) instead")]]
    rrr::i32 RccNotifyGlobalValidation(const txid_t& tx_id, const int32_t& rank, const int32_t& res) {
        RpcRccNotifyGlobalValidationRequest __req__;
        __req__.tx_id = tx_id;
        __req__.rank = rank;
        __req__.res = res;
        auto __typed_result__ = this->RccNotifyGlobalValidation(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
    class JanusDispatchTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JanusDispatchTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcJanusDispatchResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJanusDispatchResponse, rrr::i32>::Err(__ret__);
            }
            RpcJanusDispatchResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.output;
            __fu__->get_reply() >> __typed_resp__.ret_graph;
            return rusty::Result<RpcJanusDispatchResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JanusDispatchTypedFuture, rrr::i32> async_JanusDispatch(const RpcJanusDispatchRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSDISPATCH, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusDispatchTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusDispatchTypedFuture, rrr::i32>::Ok(JanusDispatchTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJanusDispatchResponse, rrr::i32> JanusDispatch(const RpcJanusDispatchRequest& req) {
        auto __typed_fu_result__ = this->async_JanusDispatch(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusDispatchResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_JanusDispatch(const RpcJanusDispatchRequest&) instead")]]
    rrr::FutureResult async_JanusDispatch(const std::vector<SimpleCommand>& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJanusDispatchRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_JanusDispatch(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JanusDispatch(const RpcJanusDispatchRequest&) instead")]]
    rrr::i32 JanusDispatch(const std::vector<SimpleCommand>& cmd, rrr::i32* res, TxnOutput* output, MarshallDeputy* ret_graph) {
        RpcJanusDispatchRequest __req__;
        __req__.cmd = cmd;
        auto __typed_result__ = this->JanusDispatch(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (output) *output = __resp__.output;
        if (ret_graph) *ret_graph = __resp__.ret_graph;
        return 0;
    }
    class RccCommitTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RccCommitTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRccCommitResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccCommitResponse, rrr::i32>::Err(__ret__);
            }
            RpcRccCommitResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.output;
            return rusty::Result<RpcRccCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccCommitTypedFuture, rrr::i32> async_RccCommit(const RpcRccCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCCOMMIT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.id;
            __m__ << req.rank;
            __m__ << req.need_validation;
            __m__ << req.parents;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccCommitTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccCommitTypedFuture, rrr::i32>::Ok(RccCommitTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccCommitResponse, rrr::i32> RccCommit(const RpcRccCommitRequest& req) {
        auto __typed_fu_result__ = this->async_RccCommit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccCommitResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_RccCommit(const RpcRccCommitRequest&) instead")]]
    rrr::FutureResult async_RccCommit(const cmdid_t& id, const rank_t& rank, const int32_t& need_validation, const parent_set_t& parents, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRccCommitRequest __req__;
        __req__.id = id;
        __req__.rank = rank;
        __req__.need_validation = need_validation;
        __req__.parents = parents;
        auto __typed_result__ = this->async_RccCommit(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed RccCommit(const RpcRccCommitRequest&) instead")]]
    rrr::i32 RccCommit(const cmdid_t& id, const rank_t& rank, const int32_t& need_validation, const parent_set_t& parents, int32_t* res, TxnOutput* output) {
        RpcRccCommitRequest __req__;
        __req__.id = id;
        __req__.rank = rank;
        __req__.need_validation = need_validation;
        __req__.parents = parents;
        auto __typed_result__ = this->RccCommit(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (output) *output = __resp__.output;
        return 0;
    }
    class JanusCommitTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JanusCommitTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcJanusCommitResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJanusCommitResponse, rrr::i32>::Err(__ret__);
            }
            RpcJanusCommitResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.output;
            return rusty::Result<RpcJanusCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JanusCommitTypedFuture, rrr::i32> async_JanusCommit(const RpcJanusCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSCOMMIT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.id;
            __m__ << req.rank;
            __m__ << req.need_validation;
            __m__ << req.graph;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusCommitTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusCommitTypedFuture, rrr::i32>::Ok(JanusCommitTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJanusCommitResponse, rrr::i32> JanusCommit(const RpcJanusCommitRequest& req) {
        auto __typed_fu_result__ = this->async_JanusCommit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusCommitResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_JanusCommit(const RpcJanusCommitRequest&) instead")]]
    rrr::FutureResult async_JanusCommit(const cmdid_t& id, const rank_t& rank, const int32_t& need_validation, const MarshallDeputy& graph, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJanusCommitRequest __req__;
        __req__.id = id;
        __req__.rank = rank;
        __req__.need_validation = need_validation;
        __req__.graph = graph;
        auto __typed_result__ = this->async_JanusCommit(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JanusCommit(const RpcJanusCommitRequest&) instead")]]
    rrr::i32 JanusCommit(const cmdid_t& id, const rank_t& rank, const int32_t& need_validation, const MarshallDeputy& graph, int32_t* res, TxnOutput* output) {
        RpcJanusCommitRequest __req__;
        __req__.id = id;
        __req__.rank = rank;
        __req__.need_validation = need_validation;
        __req__.graph = graph;
        auto __typed_result__ = this->JanusCommit(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (output) *output = __resp__.output;
        return 0;
    }
    class JanusCommitWoGraphTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JanusCommitWoGraphTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcJanusCommitWoGraphResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJanusCommitWoGraphResponse, rrr::i32>::Err(__ret__);
            }
            RpcJanusCommitWoGraphResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.output;
            return rusty::Result<RpcJanusCommitWoGraphResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JanusCommitWoGraphTypedFuture, rrr::i32> async_JanusCommitWoGraph(const RpcJanusCommitWoGraphRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSCOMMITWOGRAPH, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.id;
            __m__ << req.rank;
            __m__ << req.need_validation;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusCommitWoGraphTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusCommitWoGraphTypedFuture, rrr::i32>::Ok(JanusCommitWoGraphTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJanusCommitWoGraphResponse, rrr::i32> JanusCommitWoGraph(const RpcJanusCommitWoGraphRequest& req) {
        auto __typed_fu_result__ = this->async_JanusCommitWoGraph(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusCommitWoGraphResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_JanusCommitWoGraph(const RpcJanusCommitWoGraphRequest&) instead")]]
    rrr::FutureResult async_JanusCommitWoGraph(const cmdid_t& id, const rank_t& rank, const int32_t& need_validation, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJanusCommitWoGraphRequest __req__;
        __req__.id = id;
        __req__.rank = rank;
        __req__.need_validation = need_validation;
        auto __typed_result__ = this->async_JanusCommitWoGraph(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JanusCommitWoGraph(const RpcJanusCommitWoGraphRequest&) instead")]]
    rrr::i32 JanusCommitWoGraph(const cmdid_t& id, const rank_t& rank, const int32_t& need_validation, int32_t* res, TxnOutput* output) {
        RpcJanusCommitWoGraphRequest __req__;
        __req__.id = id;
        __req__.rank = rank;
        __req__.need_validation = need_validation;
        auto __typed_result__ = this->JanusCommitWoGraph(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (output) *output = __resp__.output;
        return 0;
    }
    class JanusInquireTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JanusInquireTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcJanusInquireResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJanusInquireResponse, rrr::i32>::Err(__ret__);
            }
            RpcJanusInquireResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.ret_graph;
            return rusty::Result<RpcJanusInquireResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JanusInquireTypedFuture, rrr::i32> async_JanusInquire(const RpcJanusInquireRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSINQUIRE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.epoch;
            __m__ << req.txn_id;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusInquireTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusInquireTypedFuture, rrr::i32>::Ok(JanusInquireTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJanusInquireResponse, rrr::i32> JanusInquire(const RpcJanusInquireRequest& req) {
        auto __typed_fu_result__ = this->async_JanusInquire(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusInquireResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_JanusInquire(const RpcJanusInquireRequest&) instead")]]
    rrr::FutureResult async_JanusInquire(const epoch_t& epoch, const txnid_t& txn_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJanusInquireRequest __req__;
        __req__.epoch = epoch;
        __req__.txn_id = txn_id;
        auto __typed_result__ = this->async_JanusInquire(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JanusInquire(const RpcJanusInquireRequest&) instead")]]
    rrr::i32 JanusInquire(const epoch_t& epoch, const txnid_t& txn_id, MarshallDeputy* ret_graph) {
        RpcJanusInquireRequest __req__;
        __req__.epoch = epoch;
        __req__.txn_id = txn_id;
        auto __typed_result__ = this->JanusInquire(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ret_graph) *ret_graph = __resp__.ret_graph;
        return 0;
    }
    class RccPreAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RccPreAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRccPreAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccPreAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcRccPreAcceptResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.x;
            return rusty::Result<RpcRccPreAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccPreAcceptTypedFuture, rrr::i32> async_RccPreAccept(const RpcRccPreAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCPREACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.txn_id;
            __m__ << req.rank;
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccPreAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccPreAcceptTypedFuture, rrr::i32>::Ok(RccPreAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccPreAcceptResponse, rrr::i32> RccPreAccept(const RpcRccPreAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_RccPreAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccPreAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_RccPreAccept(const RpcRccPreAcceptRequest&) instead")]]
    rrr::FutureResult async_RccPreAccept(const cmdid_t& txn_id, const rank_t& rank, const std::vector<SimpleCommand>& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRccPreAcceptRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_RccPreAccept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed RccPreAccept(const RpcRccPreAcceptRequest&) instead")]]
    rrr::i32 RccPreAccept(const cmdid_t& txn_id, const rank_t& rank, const std::vector<SimpleCommand>& cmd, rrr::i32* res, parent_set_t* x) {
        RpcRccPreAcceptRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        __req__.cmd = cmd;
        auto __typed_result__ = this->RccPreAccept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (x) *x = __resp__.x;
        return 0;
    }
    class JanusPreAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JanusPreAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcJanusPreAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJanusPreAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcJanusPreAcceptResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.ret_graph;
            return rusty::Result<RpcJanusPreAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JanusPreAcceptTypedFuture, rrr::i32> async_JanusPreAccept(const RpcJanusPreAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSPREACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.txn_id;
            __m__ << req.rank;
            __m__ << req.cmd;
            __m__ << req.graph;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusPreAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusPreAcceptTypedFuture, rrr::i32>::Ok(JanusPreAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJanusPreAcceptResponse, rrr::i32> JanusPreAccept(const RpcJanusPreAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_JanusPreAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusPreAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_JanusPreAccept(const RpcJanusPreAcceptRequest&) instead")]]
    rrr::FutureResult async_JanusPreAccept(const cmdid_t& txn_id, const rank_t& rank, const std::vector<SimpleCommand>& cmd, const MarshallDeputy& graph, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJanusPreAcceptRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        __req__.cmd = cmd;
        __req__.graph = graph;
        auto __typed_result__ = this->async_JanusPreAccept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JanusPreAccept(const RpcJanusPreAcceptRequest&) instead")]]
    rrr::i32 JanusPreAccept(const cmdid_t& txn_id, const rank_t& rank, const std::vector<SimpleCommand>& cmd, const MarshallDeputy& graph, rrr::i32* res, MarshallDeputy* ret_graph) {
        RpcJanusPreAcceptRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        __req__.cmd = cmd;
        __req__.graph = graph;
        auto __typed_result__ = this->JanusPreAccept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (ret_graph) *ret_graph = __resp__.ret_graph;
        return 0;
    }
    class JanusPreAcceptWoGraphTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JanusPreAcceptWoGraphTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcJanusPreAcceptWoGraphResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJanusPreAcceptWoGraphResponse, rrr::i32>::Err(__ret__);
            }
            RpcJanusPreAcceptWoGraphResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            __fu__->get_reply() >> __typed_resp__.ret_graph;
            return rusty::Result<RpcJanusPreAcceptWoGraphResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JanusPreAcceptWoGraphTypedFuture, rrr::i32> async_JanusPreAcceptWoGraph(const RpcJanusPreAcceptWoGraphRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSPREACCEPTWOGRAPH, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.txn_id;
            __m__ << req.rank;
            __m__ << req.cmd;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusPreAcceptWoGraphTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusPreAcceptWoGraphTypedFuture, rrr::i32>::Ok(JanusPreAcceptWoGraphTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJanusPreAcceptWoGraphResponse, rrr::i32> JanusPreAcceptWoGraph(const RpcJanusPreAcceptWoGraphRequest& req) {
        auto __typed_fu_result__ = this->async_JanusPreAcceptWoGraph(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusPreAcceptWoGraphResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_JanusPreAcceptWoGraph(const RpcJanusPreAcceptWoGraphRequest&) instead")]]
    rrr::FutureResult async_JanusPreAcceptWoGraph(const cmdid_t& txn_id, const rank_t& rank, const std::vector<SimpleCommand>& cmd, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJanusPreAcceptWoGraphRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        __req__.cmd = cmd;
        auto __typed_result__ = this->async_JanusPreAcceptWoGraph(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JanusPreAcceptWoGraph(const RpcJanusPreAcceptWoGraphRequest&) instead")]]
    rrr::i32 JanusPreAcceptWoGraph(const cmdid_t& txn_id, const rank_t& rank, const std::vector<SimpleCommand>& cmd, rrr::i32* res, MarshallDeputy* ret_graph) {
        RpcJanusPreAcceptWoGraphRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        __req__.cmd = cmd;
        auto __typed_result__ = this->JanusPreAcceptWoGraph(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        if (ret_graph) *ret_graph = __resp__.ret_graph;
        return 0;
    }
    class RccAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit RccAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcRccAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcRccAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcRccAcceptResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcRccAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<RccAcceptTypedFuture, rrr::i32> async_RccAccept(const RpcRccAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.txn_id;
            __m__ << req.rank;
            __m__ << req.ballot;
            __m__ << req.p;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccAcceptTypedFuture, rrr::i32>::Ok(RccAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcRccAcceptResponse, rrr::i32> RccAccept(const RpcRccAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_RccAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_RccAccept(const RpcRccAcceptRequest&) instead")]]
    rrr::FutureResult async_RccAccept(const cmdid_t& txn_id, const rrr::i32& rank, const ballot_t& ballot, const parent_set_t& p, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcRccAcceptRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        __req__.ballot = ballot;
        __req__.p = p;
        auto __typed_result__ = this->async_RccAccept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed RccAccept(const RpcRccAcceptRequest&) instead")]]
    rrr::i32 RccAccept(const cmdid_t& txn_id, const rrr::i32& rank, const ballot_t& ballot, const parent_set_t& p, rrr::i32* res) {
        RpcRccAcceptRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        __req__.ballot = ballot;
        __req__.p = p;
        auto __typed_result__ = this->RccAccept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
    }
    class JanusAcceptTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit JanusAcceptTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcJanusAcceptResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcJanusAcceptResponse, rrr::i32>::Err(__ret__);
            }
            RpcJanusAcceptResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcJanusAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JanusAcceptTypedFuture, rrr::i32> async_JanusAccept(const RpcJanusAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.txn_id;
            __m__ << req.rank;
            __m__ << req.ballot;
            __m__ << req.graph;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusAcceptTypedFuture, rrr::i32>::Ok(JanusAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcJanusAcceptResponse, rrr::i32> JanusAccept(const RpcJanusAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_JanusAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_JanusAccept(const RpcJanusAcceptRequest&) instead")]]
    rrr::FutureResult async_JanusAccept(const cmdid_t& txn_id, const rrr::i32& rank, const ballot_t& ballot, const MarshallDeputy& graph, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJanusAcceptRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        __req__.ballot = ballot;
        __req__.graph = graph;
        auto __typed_result__ = this->async_JanusAccept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JanusAccept(const RpcJanusAcceptRequest&) instead")]]
    rrr::i32 JanusAccept(const cmdid_t& txn_id, const rrr::i32& rank, const ballot_t& ballot, const MarshallDeputy& graph, rrr::i32* res) {
        RpcJanusAcceptRequest __req__;
        __req__.txn_id = txn_id;
        __req__.rank = rank;
        __req__.ballot = ballot;
        __req__.graph = graph;
        auto __typed_result__ = this->JanusAccept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
    }
    class PreAcceptFebruusTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit PreAcceptFebruusTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcPreAcceptFebruusResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcPreAcceptFebruusResponse, rrr::i32>::Err(__ret__);
            }
            RpcPreAcceptFebruusResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.ret;
            __fu__->get_reply() >> __typed_resp__.timestamp;
            return rusty::Result<RpcPreAcceptFebruusResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<PreAcceptFebruusTypedFuture, rrr::i32> async_PreAcceptFebruus(const RpcPreAcceptFebruusRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::PREACCEPTFEBRUUS, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.tx_id;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<PreAcceptFebruusTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<PreAcceptFebruusTypedFuture, rrr::i32>::Ok(PreAcceptFebruusTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcPreAcceptFebruusResponse, rrr::i32> PreAcceptFebruus(const RpcPreAcceptFebruusRequest& req) {
        auto __typed_fu_result__ = this->async_PreAcceptFebruus(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcPreAcceptFebruusResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_PreAcceptFebruus(const RpcPreAcceptFebruusRequest&) instead")]]
    rrr::FutureResult async_PreAcceptFebruus(const txid_t& tx_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcPreAcceptFebruusRequest __req__;
        __req__.tx_id = tx_id;
        auto __typed_result__ = this->async_PreAcceptFebruus(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed PreAcceptFebruus(const RpcPreAcceptFebruusRequest&) instead")]]
    rrr::i32 PreAcceptFebruus(const txid_t& tx_id, rrr::i32* ret, uint64_t* timestamp) {
        RpcPreAcceptFebruusRequest __req__;
        __req__.tx_id = tx_id;
        auto __typed_result__ = this->PreAcceptFebruus(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ret) *ret = __resp__.ret;
        if (timestamp) *timestamp = __resp__.timestamp;
        return 0;
    }
    class AcceptFebruusTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit AcceptFebruusTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcAcceptFebruusResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcAcceptFebruusResponse, rrr::i32>::Err(__ret__);
            }
            RpcAcceptFebruusResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.ret;
            return rusty::Result<RpcAcceptFebruusResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<AcceptFebruusTypedFuture, rrr::i32> async_AcceptFebruus(const RpcAcceptFebruusRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::ACCEPTFEBRUUS, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.tx_id;
            __m__ << req.ballot;
            __m__ << req.timestamp;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AcceptFebruusTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AcceptFebruusTypedFuture, rrr::i32>::Ok(AcceptFebruusTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcAcceptFebruusResponse, rrr::i32> AcceptFebruus(const RpcAcceptFebruusRequest& req) {
        auto __typed_fu_result__ = this->async_AcceptFebruus(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAcceptFebruusResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_AcceptFebruus(const RpcAcceptFebruusRequest&) instead")]]
    rrr::FutureResult async_AcceptFebruus(const txid_t& tx_id, const ballot_t& ballot, const uint64_t& timestamp, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcAcceptFebruusRequest __req__;
        __req__.tx_id = tx_id;
        __req__.ballot = ballot;
        __req__.timestamp = timestamp;
        auto __typed_result__ = this->async_AcceptFebruus(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed AcceptFebruus(const RpcAcceptFebruusRequest&) instead")]]
    rrr::i32 AcceptFebruus(const txid_t& tx_id, const ballot_t& ballot, const uint64_t& timestamp, rrr::i32* ret) {
        RpcAcceptFebruusRequest __req__;
        __req__.tx_id = tx_id;
        __req__.ballot = ballot;
        __req__.timestamp = timestamp;
        auto __typed_result__ = this->AcceptFebruus(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ret) *ret = __resp__.ret;
        return 0;
    }
    class CommitFebruusTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit CommitFebruusTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcCommitFebruusResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcCommitFebruusResponse, rrr::i32>::Err(__ret__);
            }
            RpcCommitFebruusResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.ret;
            return rusty::Result<RpcCommitFebruusResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<CommitFebruusTypedFuture, rrr::i32> async_CommitFebruus(const RpcCommitFebruusRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::COMMITFEBRUUS, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.tx_id;
            __m__ << req.timestamp;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<CommitFebruusTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<CommitFebruusTypedFuture, rrr::i32>::Ok(CommitFebruusTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcCommitFebruusResponse, rrr::i32> CommitFebruus(const RpcCommitFebruusRequest& req) {
        auto __typed_fu_result__ = this->async_CommitFebruus(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcCommitFebruusResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_CommitFebruus(const RpcCommitFebruusRequest&) instead")]]
    rrr::FutureResult async_CommitFebruus(const txid_t& tx_id, const uint64_t& timestamp, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcCommitFebruusRequest __req__;
        __req__.tx_id = tx_id;
        __req__.timestamp = timestamp;
        auto __typed_result__ = this->async_CommitFebruus(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed CommitFebruus(const RpcCommitFebruusRequest&) instead")]]
    rrr::i32 CommitFebruus(const txid_t& tx_id, const uint64_t& timestamp, rrr::i32* ret) {
        RpcCommitFebruusRequest __req__;
        __req__.tx_id = tx_id;
        __req__.timestamp = timestamp;
        auto __typed_result__ = this->CommitFebruus(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ret) *ret = __resp__.ret;
        return 0;
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
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKBEGINRECOVERY, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.old_view;
            __m__ << req.new_view;
            __m__ << req.new_view_id;
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
    [[deprecated("use typed async_JetpackBeginRecovery(const RpcJetpackBeginRecoveryRequest&) instead")]]
    rrr::FutureResult async_JetpackBeginRecovery(const MarshallDeputy& old_view, const MarshallDeputy& new_view, const epoch_t& new_view_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJetpackBeginRecoveryRequest __req__;
        __req__.old_view = old_view;
        __req__.new_view = new_view;
        __req__.new_view_id = new_view_id;
        auto __typed_result__ = this->async_JetpackBeginRecovery(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JetpackBeginRecovery(const RpcJetpackBeginRecoveryRequest&) instead")]]
    rrr::i32 JetpackBeginRecovery(const MarshallDeputy& old_view, const MarshallDeputy& new_view, const epoch_t& new_view_id) {
        RpcJetpackBeginRecoveryRequest __req__;
        __req__.old_view = old_view;
        __req__.new_view = new_view;
        __req__.new_view_id = new_view_id;
        auto __typed_result__ = this->JetpackBeginRecovery(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ok;
            __fu__->get_reply() >> __typed_resp__.reply_jepoch;
            __fu__->get_reply() >> __typed_resp__.reply_oepoch;
            __fu__->get_reply() >> __typed_resp__.reply_old_view;
            __fu__->get_reply() >> __typed_resp__.reply_new_view;
            __fu__->get_reply() >> __typed_resp__.id_set;
            return rusty::Result<RpcJetpackPullIdSetResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPullIdSetTypedFuture, rrr::i32> async_JetpackPullIdSet(const RpcJetpackPullIdSetRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPULLIDSET, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.jepoch;
            __m__ << req.oepoch;
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
    [[deprecated("use typed async_JetpackPullIdSet(const RpcJetpackPullIdSetRequest&) instead")]]
    rrr::FutureResult async_JetpackPullIdSet(const epoch_t& jepoch, const epoch_t& oepoch, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJetpackPullIdSetRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        auto __typed_result__ = this->async_JetpackPullIdSet(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JetpackPullIdSet(const RpcJetpackPullIdSetRequest&) instead")]]
    rrr::i32 JetpackPullIdSet(const epoch_t& jepoch, const epoch_t& oepoch, bool_t* ok, epoch_t* reply_jepoch, epoch_t* reply_oepoch, MarshallDeputy* reply_old_view, MarshallDeputy* reply_new_view, MarshallDeputy* id_set) {
        RpcJetpackPullIdSetRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        auto __typed_result__ = this->JetpackPullIdSet(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ok) *ok = __resp__.ok;
        if (reply_jepoch) *reply_jepoch = __resp__.reply_jepoch;
        if (reply_oepoch) *reply_oepoch = __resp__.reply_oepoch;
        if (reply_old_view) *reply_old_view = __resp__.reply_old_view;
        if (reply_new_view) *reply_new_view = __resp__.reply_new_view;
        if (id_set) *id_set = __resp__.id_set;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ok;
            __fu__->get_reply() >> __typed_resp__.reply_jepoch;
            __fu__->get_reply() >> __typed_resp__.reply_oepoch;
            __fu__->get_reply() >> __typed_resp__.reply_old_view;
            __fu__->get_reply() >> __typed_resp__.reply_new_view;
            __fu__->get_reply() >> __typed_resp__.cmd_batch;
            return rusty::Result<RpcJetpackPullCmdResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPullCmdTypedFuture, rrr::i32> async_JetpackPullCmd(const RpcJetpackPullCmdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPULLCMD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.jepoch;
            __m__ << req.oepoch;
            __m__ << req.key_batch;
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
    [[deprecated("use typed async_JetpackPullCmd(const RpcJetpackPullCmdRequest&) instead")]]
    rrr::FutureResult async_JetpackPullCmd(const epoch_t& jepoch, const epoch_t& oepoch, const MarshallDeputy& key_batch, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJetpackPullCmdRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.key_batch = key_batch;
        auto __typed_result__ = this->async_JetpackPullCmd(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JetpackPullCmd(const RpcJetpackPullCmdRequest&) instead")]]
    rrr::i32 JetpackPullCmd(const epoch_t& jepoch, const epoch_t& oepoch, const MarshallDeputy& key_batch, bool_t* ok, epoch_t* reply_jepoch, epoch_t* reply_oepoch, MarshallDeputy* reply_old_view, MarshallDeputy* reply_new_view, MarshallDeputy* cmd_batch) {
        RpcJetpackPullCmdRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.key_batch = key_batch;
        auto __typed_result__ = this->JetpackPullCmd(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ok) *ok = __resp__.ok;
        if (reply_jepoch) *reply_jepoch = __resp__.reply_jepoch;
        if (reply_oepoch) *reply_oepoch = __resp__.reply_oepoch;
        if (reply_old_view) *reply_old_view = __resp__.reply_old_view;
        if (reply_new_view) *reply_new_view = __resp__.reply_new_view;
        if (cmd_batch) *cmd_batch = __resp__.cmd_batch;
        return 0;
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
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKRECORDCMD, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.jepoch;
            __m__ << req.oepoch;
            __m__ << req.sid;
            __m__ << req.rid;
            __m__ << req.cmd_batch;
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
    [[deprecated("use typed async_JetpackRecordCmd(const RpcJetpackRecordCmdRequest&) instead")]]
    rrr::FutureResult async_JetpackRecordCmd(const epoch_t& jepoch, const epoch_t& oepoch, const int32_t& sid, const int32_t& rid, const MarshallDeputy& cmd_batch, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJetpackRecordCmdRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.sid = sid;
        __req__.rid = rid;
        __req__.cmd_batch = cmd_batch;
        auto __typed_result__ = this->async_JetpackRecordCmd(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JetpackRecordCmd(const RpcJetpackRecordCmdRequest&) instead")]]
    rrr::i32 JetpackRecordCmd(const epoch_t& jepoch, const epoch_t& oepoch, const int32_t& sid, const int32_t& rid, const MarshallDeputy& cmd_batch) {
        RpcJetpackRecordCmdRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.sid = sid;
        __req__.rid = rid;
        __req__.cmd_batch = cmd_batch;
        auto __typed_result__ = this->JetpackRecordCmd(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ok;
            __fu__->get_reply() >> __typed_resp__.reply_jepoch;
            __fu__->get_reply() >> __typed_resp__.reply_oepoch;
            __fu__->get_reply() >> __typed_resp__.reply_old_view;
            __fu__->get_reply() >> __typed_resp__.reply_new_view;
            __fu__->get_reply() >> __typed_resp__.reply_max_seen_ballot;
            __fu__->get_reply() >> __typed_resp__.accepted_ballot;
            __fu__->get_reply() >> __typed_resp__.replied_sid;
            __fu__->get_reply() >> __typed_resp__.replied_set_size;
            return rusty::Result<RpcJetpackPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPrepareTypedFuture, rrr::i32> async_JetpackPrepare(const RpcJetpackPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPREPARE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.jepoch;
            __m__ << req.oepoch;
            __m__ << req.max_seen_ballot;
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
    [[deprecated("use typed async_JetpackPrepare(const RpcJetpackPrepareRequest&) instead")]]
    rrr::FutureResult async_JetpackPrepare(const epoch_t& jepoch, const epoch_t& oepoch, const ballot_t& max_seen_ballot, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJetpackPrepareRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.max_seen_ballot = max_seen_ballot;
        auto __typed_result__ = this->async_JetpackPrepare(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JetpackPrepare(const RpcJetpackPrepareRequest&) instead")]]
    rrr::i32 JetpackPrepare(const epoch_t& jepoch, const epoch_t& oepoch, const ballot_t& max_seen_ballot, bool_t* ok, epoch_t* reply_jepoch, epoch_t* reply_oepoch, MarshallDeputy* reply_old_view, MarshallDeputy* reply_new_view, ballot_t* reply_max_seen_ballot, ballot_t* accepted_ballot, int32_t* replied_sid, int32_t* replied_set_size) {
        RpcJetpackPrepareRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.max_seen_ballot = max_seen_ballot;
        auto __typed_result__ = this->JetpackPrepare(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ok) *ok = __resp__.ok;
        if (reply_jepoch) *reply_jepoch = __resp__.reply_jepoch;
        if (reply_oepoch) *reply_oepoch = __resp__.reply_oepoch;
        if (reply_old_view) *reply_old_view = __resp__.reply_old_view;
        if (reply_new_view) *reply_new_view = __resp__.reply_new_view;
        if (reply_max_seen_ballot) *reply_max_seen_ballot = __resp__.reply_max_seen_ballot;
        if (accepted_ballot) *accepted_ballot = __resp__.accepted_ballot;
        if (replied_sid) *replied_sid = __resp__.replied_sid;
        if (replied_set_size) *replied_set_size = __resp__.replied_set_size;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ok;
            __fu__->get_reply() >> __typed_resp__.reply_jepoch;
            __fu__->get_reply() >> __typed_resp__.reply_oepoch;
            __fu__->get_reply() >> __typed_resp__.reply_old_view;
            __fu__->get_reply() >> __typed_resp__.reply_new_view;
            __fu__->get_reply() >> __typed_resp__.reply_max_seen_ballot;
            return rusty::Result<RpcJetpackAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackAcceptTypedFuture, rrr::i32> async_JetpackAccept(const RpcJetpackAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKACCEPT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.jepoch;
            __m__ << req.oepoch;
            __m__ << req.max_seen_ballot;
            __m__ << req.sid;
            __m__ << req.set_size;
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
    [[deprecated("use typed async_JetpackAccept(const RpcJetpackAcceptRequest&) instead")]]
    rrr::FutureResult async_JetpackAccept(const epoch_t& jepoch, const epoch_t& oepoch, const ballot_t& max_seen_ballot, const int32_t& sid, const int32_t& set_size, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJetpackAcceptRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.max_seen_ballot = max_seen_ballot;
        __req__.sid = sid;
        __req__.set_size = set_size;
        auto __typed_result__ = this->async_JetpackAccept(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JetpackAccept(const RpcJetpackAcceptRequest&) instead")]]
    rrr::i32 JetpackAccept(const epoch_t& jepoch, const epoch_t& oepoch, const ballot_t& max_seen_ballot, const int32_t& sid, const int32_t& set_size, bool_t* ok, epoch_t* reply_jepoch, epoch_t* reply_oepoch, MarshallDeputy* reply_old_view, MarshallDeputy* reply_new_view, ballot_t* reply_max_seen_ballot) {
        RpcJetpackAcceptRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.max_seen_ballot = max_seen_ballot;
        __req__.sid = sid;
        __req__.set_size = set_size;
        auto __typed_result__ = this->JetpackAccept(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ok) *ok = __resp__.ok;
        if (reply_jepoch) *reply_jepoch = __resp__.reply_jepoch;
        if (reply_oepoch) *reply_oepoch = __resp__.reply_oepoch;
        if (reply_old_view) *reply_old_view = __resp__.reply_old_view;
        if (reply_new_view) *reply_new_view = __resp__.reply_new_view;
        if (reply_max_seen_ballot) *reply_max_seen_ballot = __resp__.reply_max_seen_ballot;
        return 0;
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
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKCOMMIT, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.jepoch;
            __m__ << req.oepoch;
            __m__ << req.sid;
            __m__ << req.set_size;
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
    [[deprecated("use typed async_JetpackCommit(const RpcJetpackCommitRequest&) instead")]]
    rrr::FutureResult async_JetpackCommit(const epoch_t& jepoch, const epoch_t& oepoch, const int32_t& sid, const int32_t& set_size, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJetpackCommitRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.sid = sid;
        __req__.set_size = set_size;
        auto __typed_result__ = this->async_JetpackCommit(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JetpackCommit(const RpcJetpackCommitRequest&) instead")]]
    rrr::i32 JetpackCommit(const epoch_t& jepoch, const epoch_t& oepoch, const int32_t& sid, const int32_t& set_size) {
        RpcJetpackCommitRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.sid = sid;
        __req__.set_size = set_size;
        auto __typed_result__ = this->JetpackCommit(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.ok;
            __fu__->get_reply() >> __typed_resp__.reply_jepoch;
            __fu__->get_reply() >> __typed_resp__.reply_oepoch;
            __fu__->get_reply() >> __typed_resp__.reply_old_view;
            __fu__->get_reply() >> __typed_resp__.reply_new_view;
            __fu__->get_reply() >> __typed_resp__.cmd;
            return rusty::Result<RpcJetpackPullRecSetInsResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<JetpackPullRecSetInsTypedFuture, rrr::i32> async_JetpackPullRecSetIns(const RpcJetpackPullRecSetInsRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKPULLRECSETINS, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.jepoch;
            __m__ << req.oepoch;
            __m__ << req.sid;
            __m__ << req.rid;
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
    [[deprecated("use typed async_JetpackPullRecSetIns(const RpcJetpackPullRecSetInsRequest&) instead")]]
    rrr::FutureResult async_JetpackPullRecSetIns(const epoch_t& jepoch, const epoch_t& oepoch, const int32_t& sid, const int32_t& rid, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJetpackPullRecSetInsRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.sid = sid;
        __req__.rid = rid;
        auto __typed_result__ = this->async_JetpackPullRecSetIns(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JetpackPullRecSetIns(const RpcJetpackPullRecSetInsRequest&) instead")]]
    rrr::i32 JetpackPullRecSetIns(const epoch_t& jepoch, const epoch_t& oepoch, const int32_t& sid, const int32_t& rid, bool_t* ok, epoch_t* reply_jepoch, epoch_t* reply_oepoch, MarshallDeputy* reply_old_view, MarshallDeputy* reply_new_view, MarshallDeputy* cmd) {
        RpcJetpackPullRecSetInsRequest __req__;
        __req__.jepoch = jepoch;
        __req__.oepoch = oepoch;
        __req__.sid = sid;
        __req__.rid = rid;
        auto __typed_result__ = this->JetpackPullRecSetIns(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (ok) *ok = __resp__.ok;
        if (reply_jepoch) *reply_jepoch = __resp__.reply_jepoch;
        if (reply_oepoch) *reply_oepoch = __resp__.reply_oepoch;
        if (reply_old_view) *reply_old_view = __resp__.reply_old_view;
        if (reply_new_view) *reply_new_view = __resp__.reply_new_view;
        if (cmd) *cmd = __resp__.cmd;
        return 0;
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
        auto __fu_result__ = __cl__->request(ClassicService::JETPACKFINISHRECOVERY, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.oepoch;
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
    [[deprecated("use typed async_JetpackFinishRecovery(const RpcJetpackFinishRecoveryRequest&) instead")]]
    rrr::FutureResult async_JetpackFinishRecovery(const epoch_t& oepoch, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcJetpackFinishRecoveryRequest __req__;
        __req__.oepoch = oepoch;
        auto __typed_result__ = this->async_JetpackFinishRecovery(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed JetpackFinishRecovery(const RpcJetpackFinishRecoveryRequest&) instead")]]
    rrr::i32 JetpackFinishRecovery(const epoch_t& oepoch) {
        RpcJetpackFinishRecoveryRequest __req__;
        __req__.oepoch = oepoch;
        auto __typed_result__ = this->JetpackFinishRecovery(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
};

class ServerControlService : public rrr::Service {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcServerShutdownRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcServerShutdownRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcServerShutdownRequest& o) {
        return m;
    }

    struct RpcServerShutdownResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcServerShutdownResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcServerShutdownResponse& o) {
        return m;
    }

    struct RpcServerReadyRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcServerReadyRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcServerReadyRequest& o) {
        return m;
    }

    struct RpcServerReadyResponse {
        rrr::i32 res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcServerReadyResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcServerReadyResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcServerHeartBeatWithDataRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcServerHeartBeatWithDataRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcServerHeartBeatWithDataRequest& o) {
        return m;
    }

    struct RpcServerHeartBeatWithDataResponse {
        ServerResponse res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcServerHeartBeatWithDataResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcServerHeartBeatWithDataResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcServerHeartBeatRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcServerHeartBeatRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcServerHeartBeatRequest& o) {
        return m;
    }

    struct RpcServerHeartBeatResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcServerHeartBeatResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcServerHeartBeatResponse& o) {
        return m;
    }

    enum {
        SERVER_SHUTDOWN = 0x6630e87c,
        SERVER_READY = 0x1691dad0,
        SERVER_HEART_BEAT_WITH_DATA = 0x1c4541ac,
        SERVER_HEART_BEAT = 0x586cf4a6,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
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
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_SHUTDOWN, __fu_attr__);
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
    [[deprecated("use typed async_server_shutdown(const RpcServerShutdownRequest&) instead")]]
    rrr::FutureResult async_server_shutdown(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcServerShutdownRequest __req__;
        auto __typed_result__ = this->async_server_shutdown(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed server_shutdown(const RpcServerShutdownRequest&) instead")]]
    rrr::i32 server_shutdown() {
        RpcServerShutdownRequest __req__;
        auto __typed_result__ = this->server_shutdown(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcServerReadyResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_readyTypedFuture, rrr::i32> async_server_ready(const RpcServerReadyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_READY, __fu_attr__);
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
    [[deprecated("use typed async_server_ready(const RpcServerReadyRequest&) instead")]]
    rrr::FutureResult async_server_ready(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcServerReadyRequest __req__;
        auto __typed_result__ = this->async_server_ready(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed server_ready(const RpcServerReadyRequest&) instead")]]
    rrr::i32 server_ready(rrr::i32* res) {
        RpcServerReadyRequest __req__;
        auto __typed_result__ = this->server_ready(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcServerHeartBeatWithDataResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<server_heart_beat_with_dataTypedFuture, rrr::i32> async_server_heart_beat_with_data(const RpcServerHeartBeatWithDataRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_HEART_BEAT_WITH_DATA, __fu_attr__);
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
    [[deprecated("use typed async_server_heart_beat_with_data(const RpcServerHeartBeatWithDataRequest&) instead")]]
    rrr::FutureResult async_server_heart_beat_with_data(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcServerHeartBeatWithDataRequest __req__;
        auto __typed_result__ = this->async_server_heart_beat_with_data(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed server_heart_beat_with_data(const RpcServerHeartBeatWithDataRequest&) instead")]]
    rrr::i32 server_heart_beat_with_data(ServerResponse* res) {
        RpcServerHeartBeatWithDataRequest __req__;
        auto __typed_result__ = this->server_heart_beat_with_data(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
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
        auto __fu_result__ = __cl__->request(ServerControlService::SERVER_HEART_BEAT, __fu_attr__);
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
    [[deprecated("use typed async_server_heart_beat(const RpcServerHeartBeatRequest&) instead")]]
    rrr::FutureResult async_server_heart_beat(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcServerHeartBeatRequest __req__;
        auto __typed_result__ = this->async_server_heart_beat(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed server_heart_beat(const RpcServerHeartBeatRequest&) instead")]]
    rrr::i32 server_heart_beat() {
        RpcServerHeartBeatRequest __req__;
        auto __typed_result__ = this->server_heart_beat(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
    }
};

class ClientControlService : public rrr::Service {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcClientGetTxnNamesRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientGetTxnNamesRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientGetTxnNamesRequest& o) {
        return m;
    }

    struct RpcClientGetTxnNamesResponse {
        std::map<rrr::i32, std::string> txn_names;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientGetTxnNamesResponse& o) {
        m << o.txn_names;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientGetTxnNamesResponse& o) {
        m >> o.txn_names;
        return m;
    }

    struct RpcClientShutdownRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientShutdownRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientShutdownRequest& o) {
        return m;
    }

    struct RpcClientShutdownResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientShutdownResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientShutdownResponse& o) {
        return m;
    }

    struct RpcClientForceStopRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientForceStopRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientForceStopRequest& o) {
        return m;
    }

    struct RpcClientForceStopResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientForceStopResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientForceStopResponse& o) {
        return m;
    }

    struct RpcClientResponseRequest {
        DepId dep_id;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientResponseRequest& o) {
        m << o.dep_id;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientResponseRequest& o) {
        m >> o.dep_id;
        return m;
    }

    struct RpcClientResponseResponse {
        ClientResponse res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientResponseResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientResponseResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcClientReadyRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientReadyRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientReadyRequest& o) {
        return m;
    }

    struct RpcClientReadyResponse {
        rrr::i32 res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientReadyResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientReadyResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcClientReadyBlockRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientReadyBlockRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientReadyBlockRequest& o) {
        return m;
    }

    struct RpcClientReadyBlockResponse {
        rrr::i32 res;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientReadyBlockResponse& o) {
        m << o.res;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientReadyBlockResponse& o) {
        m >> o.res;
        return m;
    }

    struct RpcClientStartRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientStartRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientStartRequest& o) {
        return m;
    }

    struct RpcClientStartResponse {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcClientStartResponse& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcClientStartResponse& o) {
        return m;
    }

    struct RpcDispatchTxnRequest {
        TxDispatchRequest req;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDispatchTxnRequest& o) {
        m << o.req;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDispatchTxnRequest& o) {
        m >> o.req;
        return m;
    }

    struct RpcDispatchTxnResponse {
        TxReply result;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcDispatchTxnResponse& o) {
        m << o.result;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcDispatchTxnResponse& o) {
        m >> o.result;
        return m;
    }

    enum {
        CLIENT_GET_TXN_NAMES = 0x643416fc,
        CLIENT_SHUTDOWN = 0x55feb5c4,
        CLIENT_FORCE_STOP = 0x2c7f6255,
        CLIENT_RESPONSE = 0x6e8788fb,
        CLIENT_READY = 0x17440886,
        CLIENT_READY_BLOCK = 0x6801155f,
        CLIENT_START = 0x1445ca86,
        DISPATCHTXN = 0x2572734a,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->txn_names;
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
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
            req->m >> __typed_req__.dep_id;
            auto __typed_resp__ = std::make_shared<RpcClientResponseResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->res;
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
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
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
            req->m >> __typed_req__.req;
            auto __typed_resp__ = std::make_shared<RpcDispatchTxnResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->result;
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
            __fu__->get_reply() >> __typed_resp__.txn_names;
            return rusty::Result<RpcClientGetTxnNamesResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_get_txn_namesTypedFuture, rrr::i32> async_client_get_txn_names(const RpcClientGetTxnNamesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_GET_TXN_NAMES, __fu_attr__);
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
    [[deprecated("use typed async_client_get_txn_names(const RpcClientGetTxnNamesRequest&) instead")]]
    rrr::FutureResult async_client_get_txn_names(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcClientGetTxnNamesRequest __req__;
        auto __typed_result__ = this->async_client_get_txn_names(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed client_get_txn_names(const RpcClientGetTxnNamesRequest&) instead")]]
    rrr::i32 client_get_txn_names(std::map<rrr::i32, std::string>* txn_names) {
        RpcClientGetTxnNamesRequest __req__;
        auto __typed_result__ = this->client_get_txn_names(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (txn_names) *txn_names = __resp__.txn_names;
        return 0;
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
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_SHUTDOWN, __fu_attr__);
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
    [[deprecated("use typed async_client_shutdown(const RpcClientShutdownRequest&) instead")]]
    rrr::FutureResult async_client_shutdown(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcClientShutdownRequest __req__;
        auto __typed_result__ = this->async_client_shutdown(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed client_shutdown(const RpcClientShutdownRequest&) instead")]]
    rrr::i32 client_shutdown() {
        RpcClientShutdownRequest __req__;
        auto __typed_result__ = this->client_shutdown(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
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
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_FORCE_STOP, __fu_attr__);
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
    [[deprecated("use typed async_client_force_stop(const RpcClientForceStopRequest&) instead")]]
    rrr::FutureResult async_client_force_stop(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcClientForceStopRequest __req__;
        auto __typed_result__ = this->async_client_force_stop(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed client_force_stop(const RpcClientForceStopRequest&) instead")]]
    rrr::i32 client_force_stop() {
        RpcClientForceStopRequest __req__;
        auto __typed_result__ = this->client_force_stop(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcClientResponseResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_responseTypedFuture, rrr::i32> async_client_response(const RpcClientResponseRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_RESPONSE, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.dep_id;
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
    [[deprecated("use typed async_client_response(const RpcClientResponseRequest&) instead")]]
    rrr::FutureResult async_client_response(const DepId& dep_id, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcClientResponseRequest __req__;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->async_client_response(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed client_response(const RpcClientResponseRequest&) instead")]]
    rrr::i32 client_response(const DepId& dep_id, ClientResponse* res) {
        RpcClientResponseRequest __req__;
        __req__.dep_id = dep_id;
        auto __typed_result__ = this->client_response(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcClientReadyResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_readyTypedFuture, rrr::i32> async_client_ready(const RpcClientReadyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_READY, __fu_attr__);
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
    [[deprecated("use typed async_client_ready(const RpcClientReadyRequest&) instead")]]
    rrr::FutureResult async_client_ready(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcClientReadyRequest __req__;
        auto __typed_result__ = this->async_client_ready(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed client_ready(const RpcClientReadyRequest&) instead")]]
    rrr::i32 client_ready(rrr::i32* res) {
        RpcClientReadyRequest __req__;
        auto __typed_result__ = this->client_ready(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.res;
            return rusty::Result<RpcClientReadyBlockResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<client_ready_blockTypedFuture, rrr::i32> async_client_ready_block(const RpcClientReadyBlockRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_READY_BLOCK, __fu_attr__);
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
    [[deprecated("use typed async_client_ready_block(const RpcClientReadyBlockRequest&) instead")]]
    rrr::FutureResult async_client_ready_block(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcClientReadyBlockRequest __req__;
        auto __typed_result__ = this->async_client_ready_block(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed client_ready_block(const RpcClientReadyBlockRequest&) instead")]]
    rrr::i32 client_ready_block(rrr::i32* res) {
        RpcClientReadyBlockRequest __req__;
        auto __typed_result__ = this->client_ready_block(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (res) *res = __resp__.res;
        return 0;
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
        auto __fu_result__ = __cl__->request(ClientControlService::CLIENT_START, __fu_attr__);
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
    [[deprecated("use typed async_client_start(const RpcClientStartRequest&) instead")]]
    rrr::FutureResult async_client_start(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcClientStartRequest __req__;
        auto __typed_result__ = this->async_client_start(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed client_start(const RpcClientStartRequest&) instead")]]
    rrr::i32 client_start() {
        RpcClientStartRequest __req__;
        auto __typed_result__ = this->client_start(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        return 0;
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
            __fu__->get_reply() >> __typed_resp__.result;
            return rusty::Result<RpcDispatchTxnResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<DispatchTxnTypedFuture, rrr::i32> async_DispatchTxn(const RpcDispatchTxnRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClientControlService::DISPATCHTXN, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.req;
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
    [[deprecated("use typed async_DispatchTxn(const RpcDispatchTxnRequest&) instead")]]
    rrr::FutureResult async_DispatchTxn(const TxDispatchRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcDispatchTxnRequest __req__;
        __req__.req = req;
        auto __typed_result__ = this->async_DispatchTxn(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed DispatchTxn(const RpcDispatchTxnRequest&) instead")]]
    rrr::i32 DispatchTxn(const TxDispatchRequest& req, TxReply* result) {
        RpcDispatchTxnRequest __req__;
        __req__.req = req;
        auto __typed_result__ = this->DispatchTxn(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (result) *result = __resp__.result;
        return 0;
    }
};

class ConfigServiceService : public rrr::Service {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcGetConfigRequest {
        uint64_t client_version;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcGetConfigRequest& o) {
        m << o.client_version;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcGetConfigRequest& o) {
        m >> o.client_version;
        return m;
    }

    struct RpcGetConfigResponse {
        uint64_t current_version;
        rrr::i32 has_update;
        std::string config_data;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcGetConfigResponse& o) {
        m << o.current_version;
        m << o.has_update;
        m << o.config_data;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcGetConfigResponse& o) {
        m >> o.current_version;
        m >> o.has_update;
        m >> o.config_data;
        return m;
    }

    struct RpcGetConfigVersionRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcGetConfigVersionRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcGetConfigVersionRequest& o) {
        return m;
    }

    struct RpcGetConfigVersionResponse {
        uint64_t version;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcGetConfigVersionResponse& o) {
        m << o.version;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcGetConfigVersionResponse& o) {
        m >> o.version;
        return m;
    }

    struct RpcHasConfigRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcHasConfigRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcHasConfigRequest& o) {
        return m;
    }

    struct RpcHasConfigResponse {
        rrr::i32 has_config;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcHasConfigResponse& o) {
        m << o.has_config;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcHasConfigResponse& o) {
        m >> o.has_config;
        return m;
    }

    struct RpcSetShardingPolicyRequest {
        std::string policy_data;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSetShardingPolicyRequest& o) {
        m << o.policy_data;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSetShardingPolicyRequest& o) {
        m >> o.policy_data;
        return m;
    }

    struct RpcSetShardingPolicyResponse {
        rrr::i32 success;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcSetShardingPolicyResponse& o) {
        m << o.success;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcSetShardingPolicyResponse& o) {
        m >> o.success;
        return m;
    }

    struct RpcGetShardingPolicyRequest {
        uint64_t client_version;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcGetShardingPolicyRequest& o) {
        m << o.client_version;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcGetShardingPolicyRequest& o) {
        m >> o.client_version;
        return m;
    }

    struct RpcGetShardingPolicyResponse {
        uint64_t current_version;
        rrr::i32 has_update;
        std::string policy_data;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcGetShardingPolicyResponse& o) {
        m << o.current_version;
        m << o.has_update;
        m << o.policy_data;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcGetShardingPolicyResponse& o) {
        m >> o.current_version;
        m >> o.has_update;
        m >> o.policy_data;
        return m;
    }

    struct RpcGetShardingPolicyVersionRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcGetShardingPolicyVersionRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcGetShardingPolicyVersionRequest& o) {
        return m;
    }

    struct RpcGetShardingPolicyVersionResponse {
        uint64_t version;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcGetShardingPolicyVersionResponse& o) {
        m << o.version;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcGetShardingPolicyVersionResponse& o) {
        m >> o.version;
        return m;
    }

    struct RpcHasShardingPolicyRequest {
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcHasShardingPolicyRequest& o) {
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcHasShardingPolicyRequest& o) {
        return m;
    }

    struct RpcHasShardingPolicyResponse {
        rrr::i32 has_policy;
    };
    friend inline rrr::Marshal& operator <<(rrr::Marshal& m, const RpcHasShardingPolicyResponse& o) {
        m << o.has_policy;
        return m;
    }
    friend inline rrr::Marshal& operator >>(rrr::Marshal& m, RpcHasShardingPolicyResponse& o) {
        m >> o.has_policy;
        return m;
    }

    enum {
        GETCONFIG = 0x568059fa,
        GETCONFIGVERSION = 0x1954a0df,
        HASCONFIG = 0x11d0d00e,
        SETSHARDINGPOLICY = 0x156af9b4,
        GETSHARDINGPOLICY = 0x2f1fd8ab,
        GETSHARDINGPOLICYVERSION = 0x1aada5fe,
        HASSHARDINGPOLICY = 0x1bb98a03,
    };
    // Registers RPC IDs with server using service index
    // @safe
    int __reg_to__(rrr::Server& svr, size_t svc_index) override {
        int ret = 0;
        if ((ret = svr.reg_rpc(GETCONFIG, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(GETCONFIGVERSION, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(HASCONFIG, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(SETSHARDINGPOLICY, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(GETSHARDINGPOLICY, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(GETSHARDINGPOLICYVERSION, svc_index)) != 0) {
            goto err;
        }
        if ((ret = svr.reg_rpc(HASSHARDINGPOLICY, svc_index)) != 0) {
            goto err;
        }
        return 0;
    err:
        svr.unreg(GETCONFIG);
        svr.unreg(GETCONFIGVERSION);
        svr.unreg(HASCONFIG);
        svr.unreg(SETSHARDINGPOLICY);
        svr.unreg(GETSHARDINGPOLICY);
        svr.unreg(GETSHARDINGPOLICYVERSION);
        svr.unreg(HASSHARDINGPOLICY);
        return ret;
    }
    // @safe - Dispatch for RPC requests
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) override {
        switch (rpc_id) {
        case GETCONFIG: __GetConfig__wrapper__(std::move(req), weak_sconn); break;
        case GETCONFIGVERSION: __GetConfigVersion__wrapper__(std::move(req), weak_sconn); break;
        case HASCONFIG: __HasConfig__wrapper__(std::move(req), weak_sconn); break;
        case SETSHARDINGPOLICY: __SetShardingPolicy__wrapper__(std::move(req), weak_sconn); break;
        case GETSHARDINGPOLICY: __GetShardingPolicy__wrapper__(std::move(req), weak_sconn); break;
        case GETSHARDINGPOLICYVERSION: __GetShardingPolicyVersion__wrapper__(std::move(req), weak_sconn); break;
        case HASSHARDINGPOLICY: __HasShardingPolicy__wrapper__(std::move(req), weak_sconn); break;
        default: break;  // Unknown RPC ID, ignore
        }
    }
    // typed service signatures
    // @safe
    virtual void GetConfig(const RpcGetConfigRequest& req, RpcGetConfigResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void GetConfigVersion(const RpcGetConfigVersionRequest& req, RpcGetConfigVersionResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void HasConfig(const RpcHasConfigRequest& req, RpcHasConfigResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void SetShardingPolicy(const RpcSetShardingPolicyRequest& req, RpcSetShardingPolicyResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void GetShardingPolicy(const RpcGetShardingPolicyRequest& req, RpcGetShardingPolicyResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void GetShardingPolicyVersion(const RpcGetShardingPolicyVersionRequest& req, RpcGetShardingPolicyVersionResponse& resp, rrr::DeferredReply defer) = 0;
    // @safe
    virtual void HasShardingPolicy(const RpcHasShardingPolicyRequest& req, RpcHasShardingPolicyResponse& resp, rrr::DeferredReply defer) = 0;
    // these RPC handler functions need to be implemented by user
    // for 'raw' handlers, req is rusty::Box (auto-cleaned); weak_sconn requires lock() before use
private:
    // @safe
    void __GetConfig__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcGetConfigRequest __typed_req__;
            req->m >> __typed_req__.client_version;
            auto __typed_resp__ = std::make_shared<RpcGetConfigResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->current_version;
                    m << __typed_resp__->has_update;
                    m << __typed_resp__->config_data;
                },
                []() {});
            this->GetConfig(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __GetConfigVersion__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcGetConfigVersionRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcGetConfigVersionResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->version;
                },
                []() {});
            this->GetConfigVersion(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __HasConfig__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcHasConfigRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcHasConfigResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->has_config;
                },
                []() {});
            this->HasConfig(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __SetShardingPolicy__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSetShardingPolicyRequest __typed_req__;
            req->m >> __typed_req__.policy_data;
            auto __typed_resp__ = std::make_shared<RpcSetShardingPolicyResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->success;
                },
                []() {});
            this->SetShardingPolicy(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __GetShardingPolicy__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcGetShardingPolicyRequest __typed_req__;
            req->m >> __typed_req__.client_version;
            auto __typed_resp__ = std::make_shared<RpcGetShardingPolicyResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->current_version;
                    m << __typed_resp__->has_update;
                    m << __typed_resp__->policy_data;
                },
                []() {});
            this->GetShardingPolicy(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __GetShardingPolicyVersion__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcGetShardingPolicyVersionRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcGetShardingPolicyVersionResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->version;
                },
                []() {});
            this->GetShardingPolicyVersion(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __HasShardingPolicy__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcHasShardingPolicyRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcHasShardingPolicyResponse>();
            rrr::DeferredReply __defer__(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::Marshal& m) {
                    m << __typed_resp__->has_policy;
                },
                []() {});
            this->HasShardingPolicy(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
};

class ConfigServiceProxy {
protected:
    rrr::Client* __cl__;
public:
    ConfigServiceProxy(rrr::Client* cl): __cl__(cl) { }
    // Alias typed request/response structs from the sibling Service class.
    using RpcGetConfigRequest = ConfigServiceService::RpcGetConfigRequest;
    using RpcGetConfigResponse = ConfigServiceService::RpcGetConfigResponse;
    using RpcGetConfigVersionRequest = ConfigServiceService::RpcGetConfigVersionRequest;
    using RpcGetConfigVersionResponse = ConfigServiceService::RpcGetConfigVersionResponse;
    using RpcHasConfigRequest = ConfigServiceService::RpcHasConfigRequest;
    using RpcHasConfigResponse = ConfigServiceService::RpcHasConfigResponse;
    using RpcSetShardingPolicyRequest = ConfigServiceService::RpcSetShardingPolicyRequest;
    using RpcSetShardingPolicyResponse = ConfigServiceService::RpcSetShardingPolicyResponse;
    using RpcGetShardingPolicyRequest = ConfigServiceService::RpcGetShardingPolicyRequest;
    using RpcGetShardingPolicyResponse = ConfigServiceService::RpcGetShardingPolicyResponse;
    using RpcGetShardingPolicyVersionRequest = ConfigServiceService::RpcGetShardingPolicyVersionRequest;
    using RpcGetShardingPolicyVersionResponse = ConfigServiceService::RpcGetShardingPolicyVersionResponse;
    using RpcHasShardingPolicyRequest = ConfigServiceService::RpcHasShardingPolicyRequest;
    using RpcHasShardingPolicyResponse = ConfigServiceService::RpcHasShardingPolicyResponse;
    class GetConfigTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit GetConfigTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcGetConfigResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcGetConfigResponse, rrr::i32>::Err(__ret__);
            }
            RpcGetConfigResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.current_version;
            __fu__->get_reply() >> __typed_resp__.has_update;
            __fu__->get_reply() >> __typed_resp__.config_data;
            return rusty::Result<RpcGetConfigResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<GetConfigTypedFuture, rrr::i32> async_GetConfig(const RpcGetConfigRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::GETCONFIG, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.client_version;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<GetConfigTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<GetConfigTypedFuture, rrr::i32>::Ok(GetConfigTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcGetConfigResponse, rrr::i32> GetConfig(const RpcGetConfigRequest& req) {
        auto __typed_fu_result__ = this->async_GetConfig(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcGetConfigResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_GetConfig(const RpcGetConfigRequest&) instead")]]
    rrr::FutureResult async_GetConfig(const uint64_t& client_version, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcGetConfigRequest __req__;
        __req__.client_version = client_version;
        auto __typed_result__ = this->async_GetConfig(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed GetConfig(const RpcGetConfigRequest&) instead")]]
    rrr::i32 GetConfig(const uint64_t& client_version, uint64_t* current_version, rrr::i32* has_update, std::string* config_data) {
        RpcGetConfigRequest __req__;
        __req__.client_version = client_version;
        auto __typed_result__ = this->GetConfig(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (current_version) *current_version = __resp__.current_version;
        if (has_update) *has_update = __resp__.has_update;
        if (config_data) *config_data = __resp__.config_data;
        return 0;
    }
    class GetConfigVersionTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit GetConfigVersionTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcGetConfigVersionResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcGetConfigVersionResponse, rrr::i32>::Err(__ret__);
            }
            RpcGetConfigVersionResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.version;
            return rusty::Result<RpcGetConfigVersionResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<GetConfigVersionTypedFuture, rrr::i32> async_GetConfigVersion(const RpcGetConfigVersionRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::GETCONFIGVERSION, __fu_attr__);
        if (__fu_result__.is_err()) {
            return rusty::Result<GetConfigVersionTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<GetConfigVersionTypedFuture, rrr::i32>::Ok(GetConfigVersionTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcGetConfigVersionResponse, rrr::i32> GetConfigVersion(const RpcGetConfigVersionRequest& req) {
        auto __typed_fu_result__ = this->async_GetConfigVersion(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcGetConfigVersionResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_GetConfigVersion(const RpcGetConfigVersionRequest&) instead")]]
    rrr::FutureResult async_GetConfigVersion(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcGetConfigVersionRequest __req__;
        auto __typed_result__ = this->async_GetConfigVersion(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed GetConfigVersion(const RpcGetConfigVersionRequest&) instead")]]
    rrr::i32 GetConfigVersion(uint64_t* version) {
        RpcGetConfigVersionRequest __req__;
        auto __typed_result__ = this->GetConfigVersion(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (version) *version = __resp__.version;
        return 0;
    }
    class HasConfigTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit HasConfigTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcHasConfigResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcHasConfigResponse, rrr::i32>::Err(__ret__);
            }
            RpcHasConfigResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.has_config;
            return rusty::Result<RpcHasConfigResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<HasConfigTypedFuture, rrr::i32> async_HasConfig(const RpcHasConfigRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::HASCONFIG, __fu_attr__);
        if (__fu_result__.is_err()) {
            return rusty::Result<HasConfigTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<HasConfigTypedFuture, rrr::i32>::Ok(HasConfigTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcHasConfigResponse, rrr::i32> HasConfig(const RpcHasConfigRequest& req) {
        auto __typed_fu_result__ = this->async_HasConfig(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcHasConfigResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_HasConfig(const RpcHasConfigRequest&) instead")]]
    rrr::FutureResult async_HasConfig(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcHasConfigRequest __req__;
        auto __typed_result__ = this->async_HasConfig(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed HasConfig(const RpcHasConfigRequest&) instead")]]
    rrr::i32 HasConfig(rrr::i32* has_config) {
        RpcHasConfigRequest __req__;
        auto __typed_result__ = this->HasConfig(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (has_config) *has_config = __resp__.has_config;
        return 0;
    }
    class SetShardingPolicyTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit SetShardingPolicyTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcSetShardingPolicyResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcSetShardingPolicyResponse, rrr::i32>::Err(__ret__);
            }
            RpcSetShardingPolicyResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.success;
            return rusty::Result<RpcSetShardingPolicyResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<SetShardingPolicyTypedFuture, rrr::i32> async_SetShardingPolicy(const RpcSetShardingPolicyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::SETSHARDINGPOLICY, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.policy_data;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SetShardingPolicyTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SetShardingPolicyTypedFuture, rrr::i32>::Ok(SetShardingPolicyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcSetShardingPolicyResponse, rrr::i32> SetShardingPolicy(const RpcSetShardingPolicyRequest& req) {
        auto __typed_fu_result__ = this->async_SetShardingPolicy(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSetShardingPolicyResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_SetShardingPolicy(const RpcSetShardingPolicyRequest&) instead")]]
    rrr::FutureResult async_SetShardingPolicy(const std::string& policy_data, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcSetShardingPolicyRequest __req__;
        __req__.policy_data = policy_data;
        auto __typed_result__ = this->async_SetShardingPolicy(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed SetShardingPolicy(const RpcSetShardingPolicyRequest&) instead")]]
    rrr::i32 SetShardingPolicy(const std::string& policy_data, rrr::i32* success) {
        RpcSetShardingPolicyRequest __req__;
        __req__.policy_data = policy_data;
        auto __typed_result__ = this->SetShardingPolicy(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (success) *success = __resp__.success;
        return 0;
    }
    class GetShardingPolicyTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit GetShardingPolicyTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcGetShardingPolicyResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcGetShardingPolicyResponse, rrr::i32>::Err(__ret__);
            }
            RpcGetShardingPolicyResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.current_version;
            __fu__->get_reply() >> __typed_resp__.has_update;
            __fu__->get_reply() >> __typed_resp__.policy_data;
            return rusty::Result<RpcGetShardingPolicyResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<GetShardingPolicyTypedFuture, rrr::i32> async_GetShardingPolicy(const RpcGetShardingPolicyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::GETSHARDINGPOLICY, __fu_attr__, [&](rrr::Marshal& __m__) {
            __m__ << req.client_version;
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<GetShardingPolicyTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<GetShardingPolicyTypedFuture, rrr::i32>::Ok(GetShardingPolicyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcGetShardingPolicyResponse, rrr::i32> GetShardingPolicy(const RpcGetShardingPolicyRequest& req) {
        auto __typed_fu_result__ = this->async_GetShardingPolicy(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcGetShardingPolicyResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_GetShardingPolicy(const RpcGetShardingPolicyRequest&) instead")]]
    rrr::FutureResult async_GetShardingPolicy(const uint64_t& client_version, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcGetShardingPolicyRequest __req__;
        __req__.client_version = client_version;
        auto __typed_result__ = this->async_GetShardingPolicy(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed GetShardingPolicy(const RpcGetShardingPolicyRequest&) instead")]]
    rrr::i32 GetShardingPolicy(const uint64_t& client_version, uint64_t* current_version, rrr::i32* has_update, std::string* policy_data) {
        RpcGetShardingPolicyRequest __req__;
        __req__.client_version = client_version;
        auto __typed_result__ = this->GetShardingPolicy(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (current_version) *current_version = __resp__.current_version;
        if (has_update) *has_update = __resp__.has_update;
        if (policy_data) *policy_data = __resp__.policy_data;
        return 0;
    }
    class GetShardingPolicyVersionTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit GetShardingPolicyVersionTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcGetShardingPolicyVersionResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcGetShardingPolicyVersionResponse, rrr::i32>::Err(__ret__);
            }
            RpcGetShardingPolicyVersionResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.version;
            return rusty::Result<RpcGetShardingPolicyVersionResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<GetShardingPolicyVersionTypedFuture, rrr::i32> async_GetShardingPolicyVersion(const RpcGetShardingPolicyVersionRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::GETSHARDINGPOLICYVERSION, __fu_attr__);
        if (__fu_result__.is_err()) {
            return rusty::Result<GetShardingPolicyVersionTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<GetShardingPolicyVersionTypedFuture, rrr::i32>::Ok(GetShardingPolicyVersionTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcGetShardingPolicyVersionResponse, rrr::i32> GetShardingPolicyVersion(const RpcGetShardingPolicyVersionRequest& req) {
        auto __typed_fu_result__ = this->async_GetShardingPolicyVersion(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcGetShardingPolicyVersionResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_GetShardingPolicyVersion(const RpcGetShardingPolicyVersionRequest&) instead")]]
    rrr::FutureResult async_GetShardingPolicyVersion(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcGetShardingPolicyVersionRequest __req__;
        auto __typed_result__ = this->async_GetShardingPolicyVersion(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed GetShardingPolicyVersion(const RpcGetShardingPolicyVersionRequest&) instead")]]
    rrr::i32 GetShardingPolicyVersion(uint64_t* version) {
        RpcGetShardingPolicyVersionRequest __req__;
        auto __typed_result__ = this->GetShardingPolicyVersion(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (version) *version = __resp__.version;
        return 0;
    }
    class HasShardingPolicyTypedFuture {
    private:
        rusty::Arc<rrr::Future> __fu__;
    public:
        explicit HasShardingPolicyTypedFuture(rusty::Arc<rrr::Future> fu): __fu__(std::move(fu)) { }
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
        rusty::Result<RpcHasShardingPolicyResponse, rrr::i32> resolve() const {
            rrr::i32 __ret__ = __fu__->get_error_code();
            if (__ret__ != 0) {
                return rusty::Result<RpcHasShardingPolicyResponse, rrr::i32>::Err(__ret__);
            }
            RpcHasShardingPolicyResponse __typed_resp__;
            __fu__->get_reply() >> __typed_resp__.has_policy;
            return rusty::Result<RpcHasShardingPolicyResponse, rrr::i32>::Ok(__typed_resp__);
        }
    };
    rusty::Result<HasShardingPolicyTypedFuture, rrr::i32> async_HasShardingPolicy(const RpcHasShardingPolicyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::HASSHARDINGPOLICY, __fu_attr__);
        if (__fu_result__.is_err()) {
            return rusty::Result<HasShardingPolicyTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<HasShardingPolicyTypedFuture, rrr::i32>::Ok(HasShardingPolicyTypedFuture(__fu_result__.unwrap()));
    }
    rusty::Result<RpcHasShardingPolicyResponse, rrr::i32> HasShardingPolicy(const RpcHasShardingPolicyRequest& req) {
        auto __typed_fu_result__ = this->async_HasShardingPolicy(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcHasShardingPolicyResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
    [[deprecated("use typed async_HasShardingPolicy(const RpcHasShardingPolicyRequest&) instead")]]
    rrr::FutureResult async_HasShardingPolicy(const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        RpcHasShardingPolicyRequest __req__;
        auto __typed_result__ = this->async_HasShardingPolicy(__req__, __fu_attr__);
        if (__typed_result__.is_err()) {
            return rrr::FutureResult::Err(__typed_result__.unwrap_err());
        }
        return rrr::FutureResult::Ok(__typed_result__.unwrap().raw_future());
    }
    [[deprecated("use typed HasShardingPolicy(const RpcHasShardingPolicyRequest&) instead")]]
    rrr::i32 HasShardingPolicy(rrr::i32* has_policy) {
        RpcHasShardingPolicyRequest __req__;
        auto __typed_result__ = this->HasShardingPolicy(__req__);
        if (__typed_result__.is_err()) {
            return __typed_result__.unwrap_err();
        }
        auto __resp__ = __typed_result__.unwrap();
        if (has_policy) *has_policy = __resp__.has_policy;
        return 0;
    }
};

} // namespace janus



