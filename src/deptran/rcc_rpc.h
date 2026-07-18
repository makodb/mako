#pragma once

#include "rrr/rrr.hpp"
#include <rusty/async.hpp>
#include <rusty/arc.hpp>
#include <rusty/box.hpp>
#include <rusty/result.hpp>

#include <errno.h>
#include <memory>

#include "procedure.h"
#include "rcc/tx.h"
#include "rrr/misc/any_message.hpp"  // graph fields are AnyMessage
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
    double cpu_util;
    rrr::i64 r_cnt_sum;
    rrr::i64 r_cnt_num;
    rrr::i64 r_sz_sum;
    rrr::i64 r_sz_num;
};

inline void serialize(const ServerResponse& o, rrr::BinaryWriteArchive& ar) {
    rrr::Serialize_::serialize(o.statistics, ar);
    rrr::Serialize_::serialize(o.cpu_util, ar);
    rrr::Serialize_::serialize(o.r_cnt_sum, ar);
    rrr::Serialize_::serialize(o.r_cnt_num, ar);
    rrr::Serialize_::serialize(o.r_sz_sum, ar);
    rrr::Serialize_::serialize(o.r_sz_num, ar);
}

inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const ServerResponse& o) { serialize(o, ar); return ar; }

inline void deserialize(ServerResponse& o, rrr::BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(o.statistics, ar);
    rrr::Deserialize_::deserialize(o.cpu_util, ar);
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
    rrr::i64 n_asking;
};

inline void serialize(const ClientResponse& o, rrr::BinaryWriteArchive& ar) {
    rrr::Serialize_::serialize(o.txn_info, ar);
    rrr::Serialize_::serialize(o.run_sec, ar);
    rrr::Serialize_::serialize(o.run_nsec, ar);
    rrr::Serialize_::serialize(o.period_sec, ar);
    rrr::Serialize_::serialize(o.period_nsec, ar);
    rrr::Serialize_::serialize(o.is_finish, ar);
    rrr::Serialize_::serialize(o.n_asking, ar);
}

inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const ClientResponse& o) { serialize(o, ar); return ar; }

inline void deserialize(ClientResponse& o, rrr::BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(o.txn_info, ar);
    rrr::Deserialize_::deserialize(o.run_sec, ar);
    rrr::Deserialize_::deserialize(o.run_nsec, ar);
    rrr::Deserialize_::deserialize(o.period_sec, ar);
    rrr::Deserialize_::deserialize(o.period_nsec, ar);
    rrr::Deserialize_::deserialize(o.is_finish, ar);
    rrr::Deserialize_::deserialize(o.n_asking, ar);
}

inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, ClientResponse& o) { deserialize(o, ar); return ar; }

struct Profiling {
    double cpu_util;
    double tx_util;
    double rx_util;
    double mem_util;
};

inline void serialize(const Profiling& o, rrr::BinaryWriteArchive& ar) {
    rrr::Serialize_::serialize(o.cpu_util, ar);
    rrr::Serialize_::serialize(o.tx_util, ar);
    rrr::Serialize_::serialize(o.rx_util, ar);
    rrr::Serialize_::serialize(o.mem_util, ar);
}

inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const Profiling& o) { serialize(o, ar); return ar; }

inline void deserialize(Profiling& o, rrr::BinaryReadArchive& ar) {
    rrr::Deserialize_::deserialize(o.cpu_util, ar);
    rrr::Deserialize_::deserialize(o.tx_util, ar);
    rrr::Deserialize_::deserialize(o.rx_util, ar);
    rrr::Deserialize_::deserialize(o.mem_util, ar);
}

inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, Profiling& o) { deserialize(o, ar); return ar; }

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
        PREPARE = 0x28cac838,
        ACCEPT = 0x395402ea,
        DECIDE = 0x61c1b31b,
        FORWARDTOLEARNERSERVER = 0x23d22c8f,
        BULKACCEPT = 0x49e3f066,
        SYNCLOG = 0x3d383ee2,
        SYNCCOMMIT = 0x5a88ff14,
        BULKDECIDE = 0x2962ac46,
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<PrepareTypedFuture> await_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Prepare(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            return rusty::Result<RpcAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<AcceptTypedFuture> await_Accept(const RpcAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Accept(req, __fu_attr__));
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<DecideTypedFuture> await_Decide(const RpcDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Decide(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ret_slot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.ret_ballot, __reply_ar__);
            return rusty::Result<RpcForwardToLearnerServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<ForwardToLearnerServerTypedFuture> await_ForwardToLearnerServer(const RpcForwardToLearnerServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_ForwardToLearnerServer(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcBulkAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<BulkAcceptTypedFuture> await_BulkAccept(const RpcBulkAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_BulkAccept(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.ret, __reply_ar__);
            return rusty::Result<RpcSyncLogResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<SyncLogTypedFuture> await_SyncLog(const RpcSyncLogRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_SyncLog(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcSyncCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<SyncCommitTypedFuture> await_SyncCommit(const RpcSyncCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_SyncCommit(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.val, __reply_ar__);
            return rusty::Result<RpcBulkDecideResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<BulkDecideTypedFuture> await_BulkDecide(const RpcBulkDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_BulkDecide(req, __fu_attr__));
    }
    rusty::Result<RpcBulkDecideResponse, rrr::i32> BulkDecide(const RpcBulkDecideRequest& req) {
        auto __typed_fu_result__ = this->async_BulkDecide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcBulkDecideResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class MenciusService {
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

    struct RpcSuggestRequest {
        uint64_t slot;
        uint64_t time;
        ballot_t ballot;
        uint64_t sender;
        std::vector<uint64_t> skip_commits;
        std::vector<uint64_t> skip_potentials;
        Command cmd;
    };
    friend inline void serialize(const RpcSuggestRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.time, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.sender, ar);
        rrr::Serialize_::serialize(o.skip_commits, ar);
        rrr::Serialize_::serialize(o.skip_potentials, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSuggestRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSuggestRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.time, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.sender, ar);
        rrr::Deserialize_::deserialize(o.skip_commits, ar);
        rrr::Deserialize_::deserialize(o.skip_potentials, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSuggestRequest& o) { deserialize(o, ar); return ar; }

    struct RpcSuggestResponse {
        ballot_t max_ballot;
        uint64_t coro_id;
    };
    friend inline void serialize(const RpcSuggestResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.max_ballot, ar);
        rrr::Serialize_::serialize(o.coro_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSuggestResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSuggestResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.max_ballot, ar);
        rrr::Deserialize_::deserialize(o.coro_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSuggestResponse& o) { deserialize(o, ar); return ar; }

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

    enum {
        PREPARE = 0x1cd5a51d,
        SUGGEST = 0x546cab84,
        DECIDE = 0x1330c8e9,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
    void __Suggest__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcSuggestRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.time, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.sender, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.skip_commits, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.skip_potentials, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcSuggestResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->coro_id, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<PrepareTypedFuture, rrr::i32> async_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MenciusService::PREPARE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<PrepareTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<PrepareTypedFuture, rrr::i32>::Ok(PrepareTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<PrepareTypedFuture> await_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Prepare(req, __fu_attr__));
    }
    rusty::Result<RpcPrepareResponse, rrr::i32> Prepare(const RpcPrepareRequest& req) {
        auto __typed_fu_result__ = this->async_Prepare(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            return rusty::Result<RpcSuggestResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<SuggestTypedFuture, rrr::i32> async_Suggest(const RpcSuggestRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MenciusService::SUGGEST, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.time, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.sender, __m__);
            rrr::Serialize_::serialize(req.skip_commits, __m__);
            rrr::Serialize_::serialize(req.skip_potentials, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SuggestTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SuggestTypedFuture, rrr::i32>::Ok(SuggestTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<SuggestTypedFuture> await_Suggest(const RpcSuggestRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Suggest(req, __fu_attr__));
    }
    rusty::Result<RpcSuggestResponse, rrr::i32> Suggest(const RpcSuggestRequest& req) {
        auto __typed_fu_result__ = this->async_Suggest(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSuggestResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<DecideTypedFuture, rrr::i32> async_Decide(const RpcDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(MenciusService::DECIDE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<DecideTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<DecideTypedFuture, rrr::i32>::Ok(DecideTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<DecideTypedFuture> await_Decide(const RpcDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Decide(req, __fu_attr__));
    }
    rusty::Result<RpcDecideResponse, rrr::i32> Decide(const RpcDecideRequest& req) {
        auto __typed_fu_result__ = this->async_Decide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDecideResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
    friend inline void serialize(const RpcHeartbeatRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        rrr::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcHeartbeatRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcHeartbeatRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        rrr::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcHeartbeatRequest& o) { deserialize(o, ar); return ar; }

    struct RpcHeartbeatResponse {
        uint64_t followerPrevLogIndex;
    };
    friend inline void serialize(const RpcHeartbeatResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.followerPrevLogIndex, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcHeartbeatResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcHeartbeatResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.followerPrevLogIndex, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcHeartbeatResponse& o) { deserialize(o, ar); return ar; }

    struct RpcVoteRequest {
        uint64_t lst_log_idx;
        ballot_t lst_log_term;
        parid_t par_id;
        ballot_t cur_term;
    };
    friend inline void serialize(const RpcVoteRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.lst_log_idx, ar);
        rrr::Serialize_::serialize(o.lst_log_term, ar);
        rrr::Serialize_::serialize(o.par_id, ar);
        rrr::Serialize_::serialize(o.cur_term, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcVoteRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVoteRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.lst_log_idx, ar);
        rrr::Deserialize_::deserialize(o.lst_log_term, ar);
        rrr::Deserialize_::deserialize(o.par_id, ar);
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

    struct RpcVote2FPGARequest {
        uint64_t lst_log_idx;
        ballot_t lst_log_term;
        parid_t par_id;
        ballot_t cur_term;
    };
    friend inline void serialize(const RpcVote2FPGARequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.lst_log_idx, ar);
        rrr::Serialize_::serialize(o.lst_log_term, ar);
        rrr::Serialize_::serialize(o.par_id, ar);
        rrr::Serialize_::serialize(o.cur_term, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcVote2FPGARequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVote2FPGARequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.lst_log_idx, ar);
        rrr::Deserialize_::deserialize(o.lst_log_term, ar);
        rrr::Deserialize_::deserialize(o.par_id, ar);
        rrr::Deserialize_::deserialize(o.cur_term, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcVote2FPGARequest& o) { deserialize(o, ar); return ar; }

    struct RpcVote2FPGAResponse {
        ballot_t max_ballot;
        bool_t vote_granted;
    };
    friend inline void serialize(const RpcVote2FPGAResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.max_ballot, ar);
        rrr::Serialize_::serialize(o.vote_granted, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcVote2FPGAResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcVote2FPGAResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.max_ballot, ar);
        rrr::Deserialize_::deserialize(o.vote_granted, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcVote2FPGAResponse& o) { deserialize(o, ar); return ar; }

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
    friend inline void serialize(const RpcAppendEntriesRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.leaderCurrentTerm, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        rrr::Serialize_::serialize(o.leaderCommitIndex, ar);
        rrr::Serialize_::serialize(o.dep_id, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntriesRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderCommitIndex, ar);
        rrr::Deserialize_::deserialize(o.dep_id, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntriesRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntriesResponse {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
    };
    friend inline void serialize(const RpcAppendEntriesResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.followerAppendOK, ar);
        rrr::Serialize_::serialize(o.followerCurrentTerm, ar);
        rrr::Serialize_::serialize(o.followerLastLogIndex, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntriesResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntriesResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.followerAppendOK, ar);
        rrr::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.followerLastLogIndex, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntriesResponse& o) { deserialize(o, ar); return ar; }

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
    friend inline void serialize(const RpcAppendEntries2Request& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.leaderCurrentTerm, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogIndex, ar);
        rrr::Serialize_::serialize(o.leaderPrevLogTerm, ar);
        rrr::Serialize_::serialize(o.leaderCommitIndex, ar);
        rrr::Serialize_::serialize(o.dep_id, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntries2Request& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntries2Request& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.leaderCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogIndex, ar);
        rrr::Deserialize_::deserialize(o.leaderPrevLogTerm, ar);
        rrr::Deserialize_::deserialize(o.leaderCommitIndex, ar);
        rrr::Deserialize_::deserialize(o.dep_id, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntries2Request& o) { deserialize(o, ar); return ar; }

    struct RpcAppendEntries2Response {
        uint64_t followerAppendOK;
        uint64_t followerCurrentTerm;
        uint64_t followerLastLogIndex;
    };
    friend inline void serialize(const RpcAppendEntries2Response& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.followerAppendOK, ar);
        rrr::Serialize_::serialize(o.followerCurrentTerm, ar);
        rrr::Serialize_::serialize(o.followerLastLogIndex, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAppendEntries2Response& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAppendEntries2Response& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.followerAppendOK, ar);
        rrr::Deserialize_::deserialize(o.followerCurrentTerm, ar);
        rrr::Deserialize_::deserialize(o.followerLastLogIndex, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAppendEntries2Response& o) { deserialize(o, ar); return ar; }

    struct RpcDecideRequest {
        uint64_t slot;
        ballot_t ballot;
        DepId dep_id;
        Command cmd;
    };
    friend inline void serialize(const RpcDecideRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.dep_id, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcDecideRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcDecideRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.dep_id, ar);
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

    enum {
        HEARTBEAT = 0x1ffd8104,
        VOTE = 0x35b086b2,
        VOTE2FPGA = 0x10993a23,
        APPENDENTRIES = 0x64f75070,
        APPENDENTRIES2 = 0x436875b5,
        DECIDE = 0x1914cb49,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
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
    virtual void Heartbeat(const RpcHeartbeatRequest& req, RpcHeartbeatResponse& resp, rrr::DeferredReply defer) = 0;
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcHeartbeatResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->followerPrevLogIndex, m);
                },
                []() {});
            this->Heartbeat(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __Vote__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcVoteRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.lst_log_idx, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.lst_log_term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.par_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cur_term, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcVoteResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->vote_granted, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.lst_log_idx, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.lst_log_term, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.par_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cur_term, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcVote2FPGAResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->vote_granted, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcAppendEntriesResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->followerAppendOK, m);
                    rrr::Serialize_::serialize(__typed_resp__->followerCurrentTerm, m);
                    rrr::Serialize_::serialize(__typed_resp__->followerLastLogIndex, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCurrentTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderPrevLogTerm, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.leaderCommitIndex, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcAppendEntries2Response>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->followerAppendOK, m);
                    rrr::Serialize_::serialize(__typed_resp__->followerCurrentTerm, m);
                    rrr::Serialize_::serialize(__typed_resp__->followerLastLogIndex, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
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
};

class FpgaRaftProxy {
protected:
    rrr::Client* __cl__;
public:
    FpgaRaftProxy(rrr::Client* cl): __cl__(cl) { }
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.followerPrevLogIndex, __reply_ar__);
            return rusty::Result<RpcHeartbeatResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<HeartbeatTypedFuture, rrr::i32> async_Heartbeat(const RpcHeartbeatRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::HEARTBEAT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            rrr::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<HeartbeatTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<HeartbeatTypedFuture, rrr::i32>::Ok(HeartbeatTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<HeartbeatTypedFuture> await_Heartbeat(const RpcHeartbeatRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Heartbeat(req, __fu_attr__));
    }
    rusty::Result<RpcHeartbeatResponse, rrr::i32> Heartbeat(const RpcHeartbeatRequest& req) {
        auto __typed_fu_result__ = this->async_Heartbeat(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcHeartbeatResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.vote_granted, __reply_ar__);
            return rusty::Result<RpcVoteResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<VoteTypedFuture, rrr::i32> async_Vote(const RpcVoteRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::VOTE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.lst_log_idx, __m__);
            rrr::Serialize_::serialize(req.lst_log_term, __m__);
            rrr::Serialize_::serialize(req.par_id, __m__);
            rrr::Serialize_::serialize(req.cur_term, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<VoteTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<VoteTypedFuture, rrr::i32>::Ok(VoteTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<VoteTypedFuture> await_Vote(const RpcVoteRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Vote(req, __fu_attr__));
    }
    rusty::Result<RpcVoteResponse, rrr::i32> Vote(const RpcVoteRequest& req) {
        auto __typed_fu_result__ = this->async_Vote(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVoteResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.vote_granted, __reply_ar__);
            return rusty::Result<RpcVote2FPGAResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<Vote2FPGATypedFuture, rrr::i32> async_Vote2FPGA(const RpcVote2FPGARequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::VOTE2FPGA, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.lst_log_idx, __m__);
            rrr::Serialize_::serialize(req.lst_log_term, __m__);
            rrr::Serialize_::serialize(req.par_id, __m__);
            rrr::Serialize_::serialize(req.cur_term, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<Vote2FPGATypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<Vote2FPGATypedFuture, rrr::i32>::Ok(Vote2FPGATypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<Vote2FPGATypedFuture> await_Vote2FPGA(const RpcVote2FPGARequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Vote2FPGA(req, __fu_attr__));
    }
    rusty::Result<RpcVote2FPGAResponse, rrr::i32> Vote2FPGA(const RpcVote2FPGARequest& req) {
        auto __typed_fu_result__ = this->async_Vote2FPGA(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcVote2FPGAResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            return rusty::Result<RpcAppendEntriesResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<AppendEntriesTypedFuture, rrr::i32> async_AppendEntries(const RpcAppendEntriesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::APPENDENTRIES, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            rrr::Serialize_::serialize(req.leaderCommitIndex, __m__);
            rrr::Serialize_::serialize(req.dep_id, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntriesTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntriesTypedFuture, rrr::i32>::Ok(AppendEntriesTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<AppendEntriesTypedFuture> await_AppendEntries(const RpcAppendEntriesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_AppendEntries(req, __fu_attr__));
    }
    rusty::Result<RpcAppendEntriesResponse, rrr::i32> AppendEntries(const RpcAppendEntriesRequest& req) {
        auto __typed_fu_result__ = this->async_AppendEntries(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntriesResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            return rusty::Result<RpcAppendEntries2Response, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<AppendEntries2TypedFuture, rrr::i32> async_AppendEntries2(const RpcAppendEntries2Request& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::APPENDENTRIES2, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.leaderCurrentTerm, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogIndex, __m__);
            rrr::Serialize_::serialize(req.leaderPrevLogTerm, __m__);
            rrr::Serialize_::serialize(req.leaderCommitIndex, __m__);
            rrr::Serialize_::serialize(req.dep_id, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AppendEntries2TypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AppendEntries2TypedFuture, rrr::i32>::Ok(AppendEntries2TypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<AppendEntries2TypedFuture> await_AppendEntries2(const RpcAppendEntries2Request& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_AppendEntries2(req, __fu_attr__));
    }
    rusty::Result<RpcAppendEntries2Response, rrr::i32> AppendEntries2(const RpcAppendEntries2Request& req) {
        auto __typed_fu_result__ = this->async_AppendEntries2(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAppendEntries2Response, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<DecideTypedFuture, rrr::i32> async_Decide(const RpcDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(FpgaRaftService::DECIDE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.dep_id, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<DecideTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<DecideTypedFuture, rrr::i32>::Ok(DecideTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<DecideTypedFuture> await_Decide(const RpcDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Decide(req, __fu_attr__));
    }
    rusty::Result<RpcDecideResponse, rrr::i32> Decide(const RpcDecideRequest& req) {
        auto __typed_fu_result__ = this->async_Decide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDecideResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
        VOTE = 0x6b030337,
        VOTEDURABLE = 0x21efdc41,
        APPENDENTRIES = 0x4877f237,
        EMPTYAPPENDENTRIES = 0x50eacfc5,
        APPENDENTRIESDURABLE = 0x4db1db80,
        TIMEOUTNOW = 0x3a21c3b9,
        NOTIFYRESTART = 0x35f36f0e,
        INSTALLSNAPSHOT = 0x4d6837a3,
        ADDSERVER = 0x55fc2b84,
        REMOVESERVER = 0x4b9e0e02,
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.vote_granted, __reply_ar__);
            return rusty::Result<RpcVoteResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<VoteTypedFuture> await_Vote(const RpcVoteRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Vote(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcVoteDurableResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<VoteDurableTypedFuture> await_VoteDurable(const RpcVoteDurableRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_VoteDurable(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerAckType, __reply_ar__);
            return rusty::Result<RpcAppendEntriesResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<AppendEntriesTypedFuture> await_AppendEntries(const RpcAppendEntriesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_AppendEntries(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.followerAppendOK, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerCurrentTerm, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerLastLogIndex, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.followerAckType, __reply_ar__);
            return rusty::Result<RpcEmptyAppendEntriesResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<EmptyAppendEntriesTypedFuture> await_EmptyAppendEntries(const RpcEmptyAppendEntriesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_EmptyAppendEntries(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcAppendEntriesDurableResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<AppendEntriesDurableTypedFuture> await_AppendEntriesDurable(const RpcAppendEntriesDurableRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_AppendEntriesDurable(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.followerTerm, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            return rusty::Result<RpcTimeoutNowResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<TimeoutNowTypedFuture> await_TimeoutNow(const RpcTimeoutNowRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_TimeoutNow(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.acknowledged, __reply_ar__);
            return rusty::Result<RpcNotifyRestartResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<NotifyRestartTypedFuture> await_NotifyRestart(const RpcNotifyRestartRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_NotifyRestart(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.term_out, __reply_ar__);
            return rusty::Result<RpcInstallSnapshotResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<InstallSnapshotTypedFuture> await_InstallSnapshot(const RpcInstallSnapshotRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_InstallSnapshot(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.error_msg, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.leader_hint, __reply_ar__);
            return rusty::Result<RpcAddServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<AddServerTypedFuture> await_AddServer(const RpcAddServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_AddServer(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.error_msg, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.leader_hint, __reply_ar__);
            return rusty::Result<RpcRemoveServerResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<RemoveServerTypedFuture> await_RemoveServer(const RpcRemoveServerRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_RemoveServer(req, __fu_attr__));
    }
    rusty::Result<RpcRemoveServerResponse, rrr::i32> RemoveServer(const RpcRemoveServerRequest& req) {
        auto __typed_fu_result__ = this->async_RemoveServer(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRemoveServerResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
    friend inline void serialize(const RpcPrepareRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.is_pilot, ar);
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcPrepareRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.is_pilot, ar);
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcPrepareRequest& o) { deserialize(o, ar); return ar; }

    struct RpcPrepareResponse {
        Command ret_cmd;
        ballot_t max_ballot;
        uint64_t dep;
        status_t status;
    };
    friend inline void serialize(const RpcPrepareResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ret_cmd, ar);
        rrr::Serialize_::serialize(o.max_ballot, ar);
        rrr::Serialize_::serialize(o.dep, ar);
        rrr::Serialize_::serialize(o.status, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcPrepareResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcPrepareResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ret_cmd, ar);
        rrr::Deserialize_::deserialize(o.max_ballot, ar);
        rrr::Deserialize_::deserialize(o.dep, ar);
        rrr::Deserialize_::deserialize(o.status, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcPrepareResponse& o) { deserialize(o, ar); return ar; }

    struct RpcFastAcceptRequest {
        uint8_t is_pilot;
        uint64_t slot;
        ballot_t ballot;
        uint64_t dep;
        Command cmd;
        DepId dep_id;
    };
    friend inline void serialize(const RpcFastAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.is_pilot, ar);
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.dep, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
        rrr::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.is_pilot, ar);
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.dep, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
        rrr::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcFastAcceptResponse {
        ballot_t max_ballot;
        uint64_t ret_dep;
    };
    friend inline void serialize(const RpcFastAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.max_ballot, ar);
        rrr::Serialize_::serialize(o.ret_dep, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcFastAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcFastAcceptResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.max_ballot, ar);
        rrr::Deserialize_::deserialize(o.ret_dep, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcFastAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcAcceptRequest {
        uint8_t is_pilot;
        uint64_t slot;
        ballot_t ballot;
        uint64_t dep;
        Command cmd;
        DepId dep_id;
    };
    friend inline void serialize(const RpcAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.is_pilot, ar);
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.dep, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
        rrr::Serialize_::serialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.is_pilot, ar);
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.dep, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
        rrr::Deserialize_::deserialize(o.dep_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcAcceptResponse {
        ballot_t max_ballot;
    };
    friend inline void serialize(const RpcAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.max_ballot, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAcceptResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.max_ballot, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcCommitRequest {
        uint8_t is_pilot;
        uint64_t slot;
        uint64_t dep;
        Command cmd;
    };
    friend inline void serialize(const RpcCommitRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.is_pilot, ar);
        rrr::Serialize_::serialize(o.slot, ar);
        rrr::Serialize_::serialize(o.dep, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcCommitRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcCommitRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.is_pilot, ar);
        rrr::Deserialize_::deserialize(o.slot, ar);
        rrr::Deserialize_::deserialize(o.dep, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcCommitRequest& o) { deserialize(o, ar); return ar; }

    struct RpcCommitResponse {
    };
    friend inline void serialize(const RpcCommitResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcCommitResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcCommitResponse& o) { deserialize(o, ar); return ar; }

    enum {
        PREPARE = 0x4711b584,
        FASTACCEPT = 0x635652ef,
        ACCEPT = 0x2c5c801e,
        COMMIT = 0x3ea97041,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
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
    void __Prepare__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcPrepareRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.is_pilot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcPrepareResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ret_cmd, m);
                    rrr::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->dep, m);
                    rrr::Serialize_::serialize(__typed_resp__->status, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.is_pilot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcFastAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->max_ballot, m);
                    rrr::Serialize_::serialize(__typed_resp__->ret_dep, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.is_pilot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->max_ballot, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.is_pilot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.slot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.dep, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcCommitResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ret_cmd, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.dep, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.status, __reply_ar__);
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<PrepareTypedFuture, rrr::i32> async_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::PREPARE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.is_pilot, __m__);
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<PrepareTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<PrepareTypedFuture, rrr::i32>::Ok(PrepareTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<PrepareTypedFuture> await_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Prepare(req, __fu_attr__));
    }
    rusty::Result<RpcPrepareResponse, rrr::i32> Prepare(const RpcPrepareRequest& req) {
        auto __typed_fu_result__ = this->async_Prepare(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.ret_dep, __reply_ar__);
            return rusty::Result<RpcFastAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<FastAcceptTypedFuture, rrr::i32> async_FastAccept(const RpcFastAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::FASTACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.is_pilot, __m__);
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.dep, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
            rrr::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<FastAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<FastAcceptTypedFuture, rrr::i32>::Ok(FastAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<FastAcceptTypedFuture> await_FastAccept(const RpcFastAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_FastAccept(req, __fu_attr__));
    }
    rusty::Result<RpcFastAcceptResponse, rrr::i32> FastAccept(const RpcFastAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_FastAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFastAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.max_ballot, __reply_ar__);
            return rusty::Result<RpcAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<AcceptTypedFuture, rrr::i32> async_Accept(const RpcAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::ACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.is_pilot, __m__);
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.dep, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
            rrr::Serialize_::serialize(req.dep_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<AcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<AcceptTypedFuture, rrr::i32>::Ok(AcceptTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<AcceptTypedFuture> await_Accept(const RpcAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Accept(req, __fu_attr__));
    }
    rusty::Result<RpcAcceptResponse, rrr::i32> Accept(const RpcAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_Accept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
            return rusty::Result<RpcCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<CommitTypedFuture, rrr::i32> async_Commit(const RpcCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(CopilotService::COMMIT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.is_pilot, __m__);
            rrr::Serialize_::serialize(req.slot, __m__);
            rrr::Serialize_::serialize(req.dep, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<CommitTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<CommitTypedFuture, rrr::i32>::Ok(CommitTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<CommitTypedFuture> await_Commit(const RpcCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Commit(req, __fu_attr__));
    }
    rusty::Result<RpcCommitResponse, rrr::i32> Commit(const RpcCommitRequest& req) {
        auto __typed_fu_result__ = this->async_Commit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcCommitResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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

    struct RpcRuleSpeculativeExecuteRequest {
        Command md;
    };
    friend inline void serialize(const RpcRuleSpeculativeExecuteRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.md, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRuleSpeculativeExecuteRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRuleSpeculativeExecuteRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.md, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRuleSpeculativeExecuteRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRuleSpeculativeExecuteResponse {
        bool_t accepted;
        int32_t result;
        bool_t is_leader;
    };
    friend inline void serialize(const RpcRuleSpeculativeExecuteResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.accepted, ar);
        rrr::Serialize_::serialize(o.result, ar);
        rrr::Serialize_::serialize(o.is_leader, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRuleSpeculativeExecuteResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRuleSpeculativeExecuteResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.accepted, ar);
        rrr::Deserialize_::deserialize(o.result, ar);
        rrr::Deserialize_::deserialize(o.is_leader, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRuleSpeculativeExecuteResponse& o) { deserialize(o, ar); return ar; }

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
        Profiling profile;
        Command view_data;
    };
    friend inline void serialize(const RpcCommitResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.slow, ar);
        rrr::Serialize_::serialize(o.coro_id, ar);
        rrr::Serialize_::serialize(o.profile, ar);
        rrr::Serialize_::serialize(o.view_data, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcCommitResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.slow, ar);
        rrr::Deserialize_::deserialize(o.coro_id, ar);
        rrr::Deserialize_::deserialize(o.profile, ar);
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
        Profiling profile;
        Command view_data;
    };
    friend inline void serialize(const RpcAbortResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.slow, ar);
        rrr::Serialize_::serialize(o.coro_id, ar);
        rrr::Serialize_::serialize(o.profile, ar);
        rrr::Serialize_::serialize(o.view_data, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcAbortResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcAbortResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.slow, ar);
        rrr::Deserialize_::deserialize(o.coro_id, ar);
        rrr::Deserialize_::deserialize(o.profile, ar);
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

    struct RpcIsFPGALeaderRequest {
        locid_t cur_pause;
    };
    friend inline void serialize(const RpcIsFPGALeaderRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cur_pause, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcIsFPGALeaderRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcIsFPGALeaderRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cur_pause, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcIsFPGALeaderRequest& o) { deserialize(o, ar); return ar; }

    struct RpcIsFPGALeaderResponse {
        bool_t is_leader;
    };
    friend inline void serialize(const RpcIsFPGALeaderResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.is_leader, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcIsFPGALeaderResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcIsFPGALeaderResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.is_leader, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcIsFPGALeaderResponse& o) { deserialize(o, ar); return ar; }

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

    struct RpcRpcNullRequest {
    };
    friend inline void serialize(const RpcRpcNullRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRpcNullRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRpcNullRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRpcNullRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRpcNullResponse {
    };
    friend inline void serialize(const RpcRpcNullResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRpcNullResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRpcNullResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRpcNullResponse& o) { deserialize(o, ar); return ar; }

    struct RpcTapirAcceptRequest {
        uint64_t cmd_id;
        int64_t ballot;
        int32_t decision;
    };
    friend inline void serialize(const RpcTapirAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd_id, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.decision, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTapirAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd_id, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.decision, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTapirAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTapirAcceptResponse {
    };
    friend inline void serialize(const RpcTapirAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTapirAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirAcceptResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTapirAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcTapirFastAcceptRequest {
        uint64_t cmd_id;
        std::vector<SimpleCommand> txn_cmds;
    };
    friend inline void serialize(const RpcTapirFastAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd_id, ar);
        rrr::Serialize_::serialize(o.txn_cmds, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTapirFastAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirFastAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd_id, ar);
        rrr::Deserialize_::deserialize(o.txn_cmds, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTapirFastAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTapirFastAcceptResponse {
        rrr::i32 res;
    };
    friend inline void serialize(const RpcTapirFastAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTapirFastAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirFastAcceptResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTapirFastAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcTapirDecideRequest {
        uint64_t cmd_id;
        rrr::i32 commit;
    };
    friend inline void serialize(const RpcTapirDecideRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd_id, ar);
        rrr::Serialize_::serialize(o.commit, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTapirDecideRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirDecideRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd_id, ar);
        rrr::Deserialize_::deserialize(o.commit, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTapirDecideRequest& o) { deserialize(o, ar); return ar; }

    struct RpcTapirDecideResponse {
    };
    friend inline void serialize(const RpcTapirDecideResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcTapirDecideResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcTapirDecideResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcTapirDecideResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccDispatchRequest {
        std::vector<SimpleCommand> cmd;
    };
    friend inline void serialize(const RpcRccDispatchRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccDispatchRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccDispatchRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccDispatchRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccDispatchResponse {
        rrr::i32 res;
        TxnOutput output;
        AnyMessage md_graph;
    };
    friend inline void serialize(const RpcRccDispatchResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.output, ar);
        rrr::Serialize_::serialize(o.md_graph, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccDispatchResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccDispatchResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.output, ar);
        rrr::Deserialize_::deserialize(o.md_graph, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccDispatchResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccFinishRequest {
        cmdid_t id;
        AnyMessage md_graph;
    };
    friend inline void serialize(const RpcRccFinishRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.id, ar);
        rrr::Serialize_::serialize(o.md_graph, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccFinishRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccFinishRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.id, ar);
        rrr::Deserialize_::deserialize(o.md_graph, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccFinishRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccFinishResponse {
        std::map<uint32_t, std::map<int32_t, Value>> outputs;
    };
    friend inline void serialize(const RpcRccFinishResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.outputs, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccFinishResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccFinishResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.outputs, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccFinishResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccInquireRequest {
        txnid_t txn_id;
        int32_t rank;
    };
    friend inline void serialize(const RpcRccInquireRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.txn_id, ar);
        rrr::Serialize_::serialize(o.rank, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccInquireRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccInquireRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.txn_id, ar);
        rrr::Deserialize_::deserialize(o.rank, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccInquireRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccInquireResponse {
        std::map<uint64_t, parent_set_t> out_0;
    };
    friend inline void serialize(const RpcRccInquireResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.out_0, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccInquireResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccInquireResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.out_0, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccInquireResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccDispatchRoRequest {
        SimpleCommand cmd;
    };
    friend inline void serialize(const RpcRccDispatchRoRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccDispatchRoRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccDispatchRoRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccDispatchRoRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccDispatchRoResponse {
        std::map<rrr::i32, Value> output;
    };
    friend inline void serialize(const RpcRccDispatchRoResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.output, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccDispatchRoResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccDispatchRoResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.output, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccDispatchRoResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccInquireValidationRequest {
        txid_t tx_id;
        int32_t rank;
    };
    friend inline void serialize(const RpcRccInquireValidationRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.tx_id, ar);
        rrr::Serialize_::serialize(o.rank, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccInquireValidationRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccInquireValidationRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.tx_id, ar);
        rrr::Deserialize_::deserialize(o.rank, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccInquireValidationRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccInquireValidationResponse {
        int32_t res;
    };
    friend inline void serialize(const RpcRccInquireValidationResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccInquireValidationResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccInquireValidationResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccInquireValidationResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccNotifyGlobalValidationRequest {
        txid_t tx_id;
        int32_t rank;
        int32_t res;
    };
    friend inline void serialize(const RpcRccNotifyGlobalValidationRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.tx_id, ar);
        rrr::Serialize_::serialize(o.rank, ar);
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccNotifyGlobalValidationRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccNotifyGlobalValidationRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.tx_id, ar);
        rrr::Deserialize_::deserialize(o.rank, ar);
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccNotifyGlobalValidationRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccNotifyGlobalValidationResponse {
    };
    friend inline void serialize(const RpcRccNotifyGlobalValidationResponse& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccNotifyGlobalValidationResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccNotifyGlobalValidationResponse& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccNotifyGlobalValidationResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJanusDispatchRequest {
        std::vector<SimpleCommand> cmd;
    };
    friend inline void serialize(const RpcJanusDispatchRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusDispatchRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusDispatchRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusDispatchRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJanusDispatchResponse {
        rrr::i32 res;
        TxnOutput output;
        AnyMessage ret_graph;
    };
    friend inline void serialize(const RpcJanusDispatchResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.output, ar);
        rrr::Serialize_::serialize(o.ret_graph, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusDispatchResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusDispatchResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.output, ar);
        rrr::Deserialize_::deserialize(o.ret_graph, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusDispatchResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccCommitRequest {
        cmdid_t id;
        rank_t rank;
        int32_t need_validation;
        parent_set_t parents;
    };
    friend inline void serialize(const RpcRccCommitRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.id, ar);
        rrr::Serialize_::serialize(o.rank, ar);
        rrr::Serialize_::serialize(o.need_validation, ar);
        rrr::Serialize_::serialize(o.parents, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccCommitRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccCommitRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.id, ar);
        rrr::Deserialize_::deserialize(o.rank, ar);
        rrr::Deserialize_::deserialize(o.need_validation, ar);
        rrr::Deserialize_::deserialize(o.parents, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccCommitRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccCommitResponse {
        int32_t res;
        TxnOutput output;
    };
    friend inline void serialize(const RpcRccCommitResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.output, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccCommitResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.output, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccCommitResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJanusCommitRequest {
        cmdid_t id;
        rank_t rank;
        int32_t need_validation;
        AnyMessage graph;
    };
    friend inline void serialize(const RpcJanusCommitRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.id, ar);
        rrr::Serialize_::serialize(o.rank, ar);
        rrr::Serialize_::serialize(o.need_validation, ar);
        rrr::Serialize_::serialize(o.graph, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusCommitRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusCommitRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.id, ar);
        rrr::Deserialize_::deserialize(o.rank, ar);
        rrr::Deserialize_::deserialize(o.need_validation, ar);
        rrr::Deserialize_::deserialize(o.graph, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusCommitRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJanusCommitResponse {
        int32_t res;
        TxnOutput output;
    };
    friend inline void serialize(const RpcJanusCommitResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.output, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusCommitResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusCommitResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.output, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusCommitResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJanusCommitWoGraphRequest {
        cmdid_t id;
        rank_t rank;
        int32_t need_validation;
    };
    friend inline void serialize(const RpcJanusCommitWoGraphRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.id, ar);
        rrr::Serialize_::serialize(o.rank, ar);
        rrr::Serialize_::serialize(o.need_validation, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusCommitWoGraphRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusCommitWoGraphRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.id, ar);
        rrr::Deserialize_::deserialize(o.rank, ar);
        rrr::Deserialize_::deserialize(o.need_validation, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusCommitWoGraphRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJanusCommitWoGraphResponse {
        int32_t res;
        TxnOutput output;
    };
    friend inline void serialize(const RpcJanusCommitWoGraphResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.output, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusCommitWoGraphResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusCommitWoGraphResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.output, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusCommitWoGraphResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJanusInquireRequest {
        epoch_t epoch;
        txnid_t txn_id;
    };
    friend inline void serialize(const RpcJanusInquireRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.epoch, ar);
        rrr::Serialize_::serialize(o.txn_id, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusInquireRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusInquireRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.epoch, ar);
        rrr::Deserialize_::deserialize(o.txn_id, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusInquireRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJanusInquireResponse {
        AnyMessage ret_graph;
    };
    friend inline void serialize(const RpcJanusInquireResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.ret_graph, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusInquireResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusInquireResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.ret_graph, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusInquireResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccPreAcceptRequest {
        cmdid_t txn_id;
        rank_t rank;
        std::vector<SimpleCommand> cmd;
    };
    friend inline void serialize(const RpcRccPreAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.txn_id, ar);
        rrr::Serialize_::serialize(o.rank, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccPreAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccPreAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.txn_id, ar);
        rrr::Deserialize_::deserialize(o.rank, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccPreAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccPreAcceptResponse {
        rrr::i32 res;
        parent_set_t x;
    };
    friend inline void serialize(const RpcRccPreAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.x, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccPreAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccPreAcceptResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.x, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccPreAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJanusPreAcceptRequest {
        cmdid_t txn_id;
        rank_t rank;
        std::vector<SimpleCommand> cmd;
        AnyMessage graph;
    };
    friend inline void serialize(const RpcJanusPreAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.txn_id, ar);
        rrr::Serialize_::serialize(o.rank, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
        rrr::Serialize_::serialize(o.graph, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusPreAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusPreAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.txn_id, ar);
        rrr::Deserialize_::deserialize(o.rank, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
        rrr::Deserialize_::deserialize(o.graph, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusPreAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJanusPreAcceptResponse {
        rrr::i32 res;
        AnyMessage ret_graph;
    };
    friend inline void serialize(const RpcJanusPreAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.ret_graph, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusPreAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusPreAcceptResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.ret_graph, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusPreAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJanusPreAcceptWoGraphRequest {
        cmdid_t txn_id;
        rank_t rank;
        std::vector<SimpleCommand> cmd;
    };
    friend inline void serialize(const RpcJanusPreAcceptWoGraphRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.txn_id, ar);
        rrr::Serialize_::serialize(o.rank, ar);
        rrr::Serialize_::serialize(o.cmd, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusPreAcceptWoGraphRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusPreAcceptWoGraphRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.txn_id, ar);
        rrr::Deserialize_::deserialize(o.rank, ar);
        rrr::Deserialize_::deserialize(o.cmd, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusPreAcceptWoGraphRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJanusPreAcceptWoGraphResponse {
        rrr::i32 res;
        AnyMessage ret_graph;
    };
    friend inline void serialize(const RpcJanusPreAcceptWoGraphResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
        rrr::Serialize_::serialize(o.ret_graph, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusPreAcceptWoGraphResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusPreAcceptWoGraphResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
        rrr::Deserialize_::deserialize(o.ret_graph, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusPreAcceptWoGraphResponse& o) { deserialize(o, ar); return ar; }

    struct RpcRccAcceptRequest {
        cmdid_t txn_id;
        rrr::i32 rank;
        ballot_t ballot;
        parent_set_t p;
    };
    friend inline void serialize(const RpcRccAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.txn_id, ar);
        rrr::Serialize_::serialize(o.rank, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.p, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.txn_id, ar);
        rrr::Deserialize_::deserialize(o.rank, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.p, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcRccAcceptResponse {
        rrr::i32 res;
    };
    friend inline void serialize(const RpcRccAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcRccAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcRccAcceptResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcRccAcceptResponse& o) { deserialize(o, ar); return ar; }

    struct RpcJanusAcceptRequest {
        cmdid_t txn_id;
        rrr::i32 rank;
        ballot_t ballot;
        AnyMessage graph;
    };
    friend inline void serialize(const RpcJanusAcceptRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.txn_id, ar);
        rrr::Serialize_::serialize(o.rank, ar);
        rrr::Serialize_::serialize(o.ballot, ar);
        rrr::Serialize_::serialize(o.graph, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusAcceptRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusAcceptRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.txn_id, ar);
        rrr::Deserialize_::deserialize(o.rank, ar);
        rrr::Deserialize_::deserialize(o.ballot, ar);
        rrr::Deserialize_::deserialize(o.graph, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusAcceptRequest& o) { deserialize(o, ar); return ar; }

    struct RpcJanusAcceptResponse {
        rrr::i32 res;
    };
    friend inline void serialize(const RpcJanusAcceptResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.res, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcJanusAcceptResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcJanusAcceptResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.res, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcJanusAcceptResponse& o) { deserialize(o, ar); return ar; }

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
        JANUSDISPATCH = 0x38e84031,
        RCCCOMMIT = 0x4e47267b,
        JANUSCOMMIT = 0x50247bc3,
        JANUSCOMMITWOGRAPH = 0x54f9701f,
        JANUSINQUIRE = 0x124208f5,
        RCCPREACCEPT = 0x21d3c639,
        JANUSPREACCEPT = 0x3285a28a,
        JANUSPREACCEPTWOGRAPH = 0x28bf34a9,
        RCCACCEPT = 0x6fc6c306,
        JANUSACCEPT = 0x3bef0a0d,
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
    void __RuleSpeculativeExecute__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRuleSpeculativeExecuteRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.md, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRuleSpeculativeExecuteResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->accepted, m);
                    rrr::Serialize_::serialize(__typed_resp__->result, m);
                    rrr::Serialize_::serialize(__typed_resp__->is_leader, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
                    rrr::Serialize_::serialize(__typed_resp__->profile, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
                    rrr::Serialize_::serialize(__typed_resp__->profile, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
    void __IsFPGALeader__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcIsFPGALeaderRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.cur_pause, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcIsFPGALeaderResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->is_leader, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
    void __rpc_null__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRpcNullRequest __typed_req__;
            auto __typed_resp__ = std::make_shared<RpcRpcNullResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.cmd_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.decision, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcTapirAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.cmd_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.txn_cmds, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcTapirFastAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.cmd_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.commit, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcTapirDecideResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                },
                []() {});
            this->TapirDecide(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __RccDispatch__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcRccDispatchRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccDispatchResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->output, m);
                    rrr::Serialize_::serialize(__typed_resp__->md_graph, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.md_graph, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccFinishResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->outputs, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.txn_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccInquireResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->out_0, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccDispatchRoResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->output, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.tx_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccInquireValidationResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.tx_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.res, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccNotifyGlobalValidationResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJanusDispatchResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->output, m);
                    rrr::Serialize_::serialize(__typed_resp__->ret_graph, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.need_validation, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.parents, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccCommitResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->output, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.need_validation, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.graph, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJanusCommitResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->output, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.need_validation, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJanusCommitWoGraphResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->output, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.epoch, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.txn_id, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJanusInquireResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->ret_graph, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.txn_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccPreAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->x, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.txn_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.graph, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJanusPreAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->ret_graph, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.txn_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.cmd, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJanusPreAcceptWoGraphResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                    rrr::Serialize_::serialize(__typed_resp__->ret_graph, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.txn_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.p, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcRccAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.txn_id, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.rank, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.ballot, __req_ar__);
            rrr::Deserialize_::deserialize(__typed_req__.graph, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcJanusAcceptResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->res, m);
                },
                []() {});
            this->JanusAccept(__typed_req__, *__typed_resp__, std::move(__defer__));
        }
    }
    // @safe
    void __JetpackBeginRecovery__wrapper__(rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
        // @unsafe
        {
            RpcJetpackBeginRecoveryRequest __typed_req__;
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ret, __reply_ar__);
            return rusty::Result<RpcMsgStringResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<MsgStringTypedFuture> await_MsgString(const RpcMsgStringRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_MsgString(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ret, __reply_ar__);
            return rusty::Result<RpcMsgMarshallResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<MsgMarshallTypedFuture> await_MsgMarshall(const RpcMsgMarshallRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_MsgMarshall(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            return rusty::Result<RpcReElectResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<ReElectTypedFuture> await_ReElect(const RpcReElectRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_ReElect(req, __fu_attr__));
    }
    rusty::Result<RpcReElectResponse, rrr::i32> ReElect(const RpcReElectRequest& req) {
        auto __typed_fu_result__ = this->async_ReElect(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcReElectResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.accepted, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.result, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.is_leader, __reply_ar__);
            return rusty::Result<RpcRuleSpeculativeExecuteResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<RuleSpeculativeExecuteTypedFuture, rrr::i32> async_RuleSpeculativeExecute(const RpcRuleSpeculativeExecuteRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RULESPECULATIVEEXECUTE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.md, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RuleSpeculativeExecuteTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RuleSpeculativeExecuteTypedFuture, rrr::i32>::Ok(RuleSpeculativeExecuteTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<RuleSpeculativeExecuteTypedFuture> await_RuleSpeculativeExecute(const RpcRuleSpeculativeExecuteRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_RuleSpeculativeExecute(req, __fu_attr__));
    }
    rusty::Result<RpcRuleSpeculativeExecuteResponse, rrr::i32> RuleSpeculativeExecute(const RpcRuleSpeculativeExecuteRequest& req) {
        auto __typed_fu_result__ = this->async_RuleSpeculativeExecute(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRuleSpeculativeExecuteResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.view_data, __reply_ar__);
            return rusty::Result<RpcDispatchResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<DispatchTypedFuture> await_Dispatch(const RpcDispatchRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Dispatch(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.slow, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            return rusty::Result<RpcPrepareResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<PrepareTypedFuture> await_Prepare(const RpcPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Prepare(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.slow, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.profile, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.view_data, __reply_ar__);
            return rusty::Result<RpcCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<CommitTypedFuture> await_Commit(const RpcCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Commit(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.slow, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.coro_id, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.profile, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.view_data, __reply_ar__);
            return rusty::Result<RpcAbortResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<AbortTypedFuture> await_Abort(const RpcAbortRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_Abort(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcEarlyAbortResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<EarlyAbortTypedFuture> await_EarlyAbort(const RpcEarlyAbortRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_EarlyAbort(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcUpgradeEpochResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<UpgradeEpochTypedFuture> await_UpgradeEpoch(const RpcUpgradeEpochRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_UpgradeEpoch(req, __fu_attr__));
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<TruncateEpochTypedFuture> await_TruncateEpoch(const RpcTruncateEpochRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_TruncateEpoch(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.is_leader, __reply_ar__);
            return rusty::Result<RpcIsLeaderResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<IsLeaderTypedFuture> await_IsLeader(const RpcIsLeaderRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_IsLeader(req, __fu_attr__));
    }
    rusty::Result<RpcIsLeaderResponse, rrr::i32> IsLeader(const RpcIsLeaderRequest& req) {
        auto __typed_fu_result__ = this->async_IsLeader(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcIsLeaderResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.is_leader, __reply_ar__);
            return rusty::Result<RpcIsFPGALeaderResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<IsFPGALeaderTypedFuture, rrr::i32> async_IsFPGALeader(const RpcIsFPGALeaderRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::ISFPGALEADER, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cur_pause, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<IsFPGALeaderTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<IsFPGALeaderTypedFuture, rrr::i32>::Ok(IsFPGALeaderTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<IsFPGALeaderTypedFuture> await_IsFPGALeader(const RpcIsFPGALeaderRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_IsFPGALeader(req, __fu_attr__));
    }
    rusty::Result<RpcIsFPGALeaderResponse, rrr::i32> IsFPGALeader(const RpcIsFPGALeaderRequest& req) {
        auto __typed_fu_result__ = this->async_IsFPGALeader(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcIsFPGALeaderResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcSimpleCmdResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<SimpleCmdTypedFuture> await_SimpleCmd(const RpcSimpleCmdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_SimpleCmd(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcFailoverPauseSocketOutResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<FailoverPauseSocketOutTypedFuture> await_FailoverPauseSocketOut(const RpcFailoverPauseSocketOutRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_FailoverPauseSocketOut(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcFailoverResumeSocketOutResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<FailoverResumeSocketOutTypedFuture> await_FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_FailoverResumeSocketOut(req, __fu_attr__));
    }
    rusty::Result<RpcFailoverResumeSocketOutResponse, rrr::i32> FailoverResumeSocketOut(const RpcFailoverResumeSocketOutRequest& req) {
        auto __typed_fu_result__ = this->async_FailoverResumeSocketOut(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcFailoverResumeSocketOutResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<rpc_nullTypedFuture, rrr::i32> async_rpc_null(const RpcRpcNullRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RPC_NULL, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<rpc_nullTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<rpc_nullTypedFuture, rrr::i32>::Ok(rpc_nullTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<rpc_nullTypedFuture> await_rpc_null(const RpcRpcNullRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_rpc_null(req, __fu_attr__));
    }
    rusty::Result<RpcRpcNullResponse, rrr::i32> rpc_null(const RpcRpcNullRequest& req) {
        auto __typed_fu_result__ = this->async_rpc_null(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRpcNullResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<TapirAcceptTypedFuture, rrr::i32> async_TapirAccept(const RpcTapirAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::TAPIRACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd_id, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.decision, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TapirAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TapirAcceptTypedFuture, rrr::i32>::Ok(TapirAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<TapirAcceptTypedFuture> await_TapirAccept(const RpcTapirAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_TapirAccept(req, __fu_attr__));
    }
    rusty::Result<RpcTapirAcceptResponse, rrr::i32> TapirAccept(const RpcTapirAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_TapirAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTapirAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcTapirFastAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<TapirFastAcceptTypedFuture, rrr::i32> async_TapirFastAccept(const RpcTapirFastAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::TAPIRFASTACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd_id, __m__);
            rrr::Serialize_::serialize(req.txn_cmds, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TapirFastAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TapirFastAcceptTypedFuture, rrr::i32>::Ok(TapirFastAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<TapirFastAcceptTypedFuture> await_TapirFastAccept(const RpcTapirFastAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_TapirFastAccept(req, __fu_attr__));
    }
    rusty::Result<RpcTapirFastAcceptResponse, rrr::i32> TapirFastAccept(const RpcTapirFastAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_TapirFastAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTapirFastAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<TapirDecideTypedFuture, rrr::i32> async_TapirDecide(const RpcTapirDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::TAPIRDECIDE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd_id, __m__);
            rrr::Serialize_::serialize(req.commit, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<TapirDecideTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<TapirDecideTypedFuture, rrr::i32>::Ok(TapirDecideTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<TapirDecideTypedFuture> await_TapirDecide(const RpcTapirDecideRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_TapirDecide(req, __fu_attr__));
    }
    rusty::Result<RpcTapirDecideResponse, rrr::i32> TapirDecide(const RpcTapirDecideRequest& req) {
        auto __typed_fu_result__ = this->async_TapirDecide(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcTapirDecideResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.md_graph, __reply_ar__);
            return rusty::Result<RpcRccDispatchResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<RccDispatchTypedFuture, rrr::i32> async_RccDispatch(const RpcRccDispatchRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCDISPATCH, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccDispatchTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccDispatchTypedFuture, rrr::i32>::Ok(RccDispatchTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<RccDispatchTypedFuture> await_RccDispatch(const RpcRccDispatchRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_RccDispatch(req, __fu_attr__));
    }
    rusty::Result<RpcRccDispatchResponse, rrr::i32> RccDispatch(const RpcRccDispatchRequest& req) {
        auto __typed_fu_result__ = this->async_RccDispatch(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccDispatchResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.outputs, __reply_ar__);
            return rusty::Result<RpcRccFinishResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<RccFinishTypedFuture, rrr::i32> async_RccFinish(const RpcRccFinishRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCFINISH, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.id, __m__);
            rrr::Serialize_::serialize(req.md_graph, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccFinishTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccFinishTypedFuture, rrr::i32>::Ok(RccFinishTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<RccFinishTypedFuture> await_RccFinish(const RpcRccFinishRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_RccFinish(req, __fu_attr__));
    }
    rusty::Result<RpcRccFinishResponse, rrr::i32> RccFinish(const RpcRccFinishRequest& req) {
        auto __typed_fu_result__ = this->async_RccFinish(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccFinishResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.out_0, __reply_ar__);
            return rusty::Result<RpcRccInquireResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<RccInquireTypedFuture, rrr::i32> async_RccInquire(const RpcRccInquireRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCINQUIRE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.txn_id, __m__);
            rrr::Serialize_::serialize(req.rank, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccInquireTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccInquireTypedFuture, rrr::i32>::Ok(RccInquireTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<RccInquireTypedFuture> await_RccInquire(const RpcRccInquireRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_RccInquire(req, __fu_attr__));
    }
    rusty::Result<RpcRccInquireResponse, rrr::i32> RccInquire(const RpcRccInquireRequest& req) {
        auto __typed_fu_result__ = this->async_RccInquire(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccInquireResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            return rusty::Result<RpcRccDispatchRoResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<RccDispatchRoTypedFuture, rrr::i32> async_RccDispatchRo(const RpcRccDispatchRoRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCDISPATCHRO, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccDispatchRoTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccDispatchRoTypedFuture, rrr::i32>::Ok(RccDispatchRoTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<RccDispatchRoTypedFuture> await_RccDispatchRo(const RpcRccDispatchRoRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_RccDispatchRo(req, __fu_attr__));
    }
    rusty::Result<RpcRccDispatchRoResponse, rrr::i32> RccDispatchRo(const RpcRccDispatchRoRequest& req) {
        auto __typed_fu_result__ = this->async_RccDispatchRo(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccDispatchRoResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcRccInquireValidationResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<RccInquireValidationTypedFuture, rrr::i32> async_RccInquireValidation(const RpcRccInquireValidationRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCINQUIREVALIDATION, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.tx_id, __m__);
            rrr::Serialize_::serialize(req.rank, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccInquireValidationTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccInquireValidationTypedFuture, rrr::i32>::Ok(RccInquireValidationTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<RccInquireValidationTypedFuture> await_RccInquireValidation(const RpcRccInquireValidationRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_RccInquireValidation(req, __fu_attr__));
    }
    rusty::Result<RpcRccInquireValidationResponse, rrr::i32> RccInquireValidation(const RpcRccInquireValidationRequest& req) {
        auto __typed_fu_result__ = this->async_RccInquireValidation(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccInquireValidationResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<RccNotifyGlobalValidationTypedFuture, rrr::i32> async_RccNotifyGlobalValidation(const RpcRccNotifyGlobalValidationRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCNOTIFYGLOBALVALIDATION, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.tx_id, __m__);
            rrr::Serialize_::serialize(req.rank, __m__);
            rrr::Serialize_::serialize(req.res, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccNotifyGlobalValidationTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccNotifyGlobalValidationTypedFuture, rrr::i32>::Ok(RccNotifyGlobalValidationTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<RccNotifyGlobalValidationTypedFuture> await_RccNotifyGlobalValidation(const RpcRccNotifyGlobalValidationRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_RccNotifyGlobalValidation(req, __fu_attr__));
    }
    rusty::Result<RpcRccNotifyGlobalValidationResponse, rrr::i32> RccNotifyGlobalValidation(const RpcRccNotifyGlobalValidationRequest& req) {
        auto __typed_fu_result__ = this->async_RccNotifyGlobalValidation(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccNotifyGlobalValidationResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.ret_graph, __reply_ar__);
            return rusty::Result<RpcJanusDispatchResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<JanusDispatchTypedFuture, rrr::i32> async_JanusDispatch(const RpcJanusDispatchRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSDISPATCH, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusDispatchTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusDispatchTypedFuture, rrr::i32>::Ok(JanusDispatchTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<JanusDispatchTypedFuture> await_JanusDispatch(const RpcJanusDispatchRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JanusDispatch(req, __fu_attr__));
    }
    rusty::Result<RpcJanusDispatchResponse, rrr::i32> JanusDispatch(const RpcJanusDispatchRequest& req) {
        auto __typed_fu_result__ = this->async_JanusDispatch(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusDispatchResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            return rusty::Result<RpcRccCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<RccCommitTypedFuture, rrr::i32> async_RccCommit(const RpcRccCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCCOMMIT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.id, __m__);
            rrr::Serialize_::serialize(req.rank, __m__);
            rrr::Serialize_::serialize(req.need_validation, __m__);
            rrr::Serialize_::serialize(req.parents, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccCommitTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccCommitTypedFuture, rrr::i32>::Ok(RccCommitTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<RccCommitTypedFuture> await_RccCommit(const RpcRccCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_RccCommit(req, __fu_attr__));
    }
    rusty::Result<RpcRccCommitResponse, rrr::i32> RccCommit(const RpcRccCommitRequest& req) {
        auto __typed_fu_result__ = this->async_RccCommit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccCommitResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            return rusty::Result<RpcJanusCommitResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<JanusCommitTypedFuture, rrr::i32> async_JanusCommit(const RpcJanusCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSCOMMIT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.id, __m__);
            rrr::Serialize_::serialize(req.rank, __m__);
            rrr::Serialize_::serialize(req.need_validation, __m__);
            rrr::Serialize_::serialize(req.graph, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusCommitTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusCommitTypedFuture, rrr::i32>::Ok(JanusCommitTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<JanusCommitTypedFuture> await_JanusCommit(const RpcJanusCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JanusCommit(req, __fu_attr__));
    }
    rusty::Result<RpcJanusCommitResponse, rrr::i32> JanusCommit(const RpcJanusCommitRequest& req) {
        auto __typed_fu_result__ = this->async_JanusCommit(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusCommitResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.output, __reply_ar__);
            return rusty::Result<RpcJanusCommitWoGraphResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<JanusCommitWoGraphTypedFuture, rrr::i32> async_JanusCommitWoGraph(const RpcJanusCommitWoGraphRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSCOMMITWOGRAPH, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.id, __m__);
            rrr::Serialize_::serialize(req.rank, __m__);
            rrr::Serialize_::serialize(req.need_validation, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusCommitWoGraphTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusCommitWoGraphTypedFuture, rrr::i32>::Ok(JanusCommitWoGraphTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<JanusCommitWoGraphTypedFuture> await_JanusCommitWoGraph(const RpcJanusCommitWoGraphRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JanusCommitWoGraph(req, __fu_attr__));
    }
    rusty::Result<RpcJanusCommitWoGraphResponse, rrr::i32> JanusCommitWoGraph(const RpcJanusCommitWoGraphRequest& req) {
        auto __typed_fu_result__ = this->async_JanusCommitWoGraph(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusCommitWoGraphResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ret_graph, __reply_ar__);
            return rusty::Result<RpcJanusInquireResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<JanusInquireTypedFuture, rrr::i32> async_JanusInquire(const RpcJanusInquireRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSINQUIRE, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.epoch, __m__);
            rrr::Serialize_::serialize(req.txn_id, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusInquireTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusInquireTypedFuture, rrr::i32>::Ok(JanusInquireTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<JanusInquireTypedFuture> await_JanusInquire(const RpcJanusInquireRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JanusInquire(req, __fu_attr__));
    }
    rusty::Result<RpcJanusInquireResponse, rrr::i32> JanusInquire(const RpcJanusInquireRequest& req) {
        auto __typed_fu_result__ = this->async_JanusInquire(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusInquireResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.x, __reply_ar__);
            return rusty::Result<RpcRccPreAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<RccPreAcceptTypedFuture, rrr::i32> async_RccPreAccept(const RpcRccPreAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCPREACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.txn_id, __m__);
            rrr::Serialize_::serialize(req.rank, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccPreAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccPreAcceptTypedFuture, rrr::i32>::Ok(RccPreAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<RccPreAcceptTypedFuture> await_RccPreAccept(const RpcRccPreAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_RccPreAccept(req, __fu_attr__));
    }
    rusty::Result<RpcRccPreAcceptResponse, rrr::i32> RccPreAccept(const RpcRccPreAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_RccPreAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccPreAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.ret_graph, __reply_ar__);
            return rusty::Result<RpcJanusPreAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<JanusPreAcceptTypedFuture, rrr::i32> async_JanusPreAccept(const RpcJanusPreAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSPREACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.txn_id, __m__);
            rrr::Serialize_::serialize(req.rank, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
            rrr::Serialize_::serialize(req.graph, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusPreAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusPreAcceptTypedFuture, rrr::i32>::Ok(JanusPreAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<JanusPreAcceptTypedFuture> await_JanusPreAccept(const RpcJanusPreAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JanusPreAccept(req, __fu_attr__));
    }
    rusty::Result<RpcJanusPreAcceptResponse, rrr::i32> JanusPreAccept(const RpcJanusPreAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_JanusPreAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusPreAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.ret_graph, __reply_ar__);
            return rusty::Result<RpcJanusPreAcceptWoGraphResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<JanusPreAcceptWoGraphTypedFuture, rrr::i32> async_JanusPreAcceptWoGraph(const RpcJanusPreAcceptWoGraphRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSPREACCEPTWOGRAPH, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.txn_id, __m__);
            rrr::Serialize_::serialize(req.rank, __m__);
            rrr::Serialize_::serialize(req.cmd, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusPreAcceptWoGraphTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusPreAcceptWoGraphTypedFuture, rrr::i32>::Ok(JanusPreAcceptWoGraphTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<JanusPreAcceptWoGraphTypedFuture> await_JanusPreAcceptWoGraph(const RpcJanusPreAcceptWoGraphRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JanusPreAcceptWoGraph(req, __fu_attr__));
    }
    rusty::Result<RpcJanusPreAcceptWoGraphResponse, rrr::i32> JanusPreAcceptWoGraph(const RpcJanusPreAcceptWoGraphRequest& req) {
        auto __typed_fu_result__ = this->async_JanusPreAcceptWoGraph(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusPreAcceptWoGraphResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcRccAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<RccAcceptTypedFuture, rrr::i32> async_RccAccept(const RpcRccAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::RCCACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.txn_id, __m__);
            rrr::Serialize_::serialize(req.rank, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.p, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<RccAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<RccAcceptTypedFuture, rrr::i32>::Ok(RccAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<RccAcceptTypedFuture> await_RccAccept(const RpcRccAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_RccAccept(req, __fu_attr__));
    }
    rusty::Result<RpcRccAcceptResponse, rrr::i32> RccAccept(const RpcRccAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_RccAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcRccAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcJanusAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<JanusAcceptTypedFuture, rrr::i32> async_JanusAccept(const RpcJanusAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ClassicService::JANUSACCEPT, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.txn_id, __m__);
            rrr::Serialize_::serialize(req.rank, __m__);
            rrr::Serialize_::serialize(req.ballot, __m__);
            rrr::Serialize_::serialize(req.graph, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<JanusAcceptTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<JanusAcceptTypedFuture, rrr::i32>::Ok(JanusAcceptTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<JanusAcceptTypedFuture> await_JanusAccept(const RpcJanusAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JanusAccept(req, __fu_attr__));
    }
    rusty::Result<RpcJanusAcceptResponse, rrr::i32> JanusAccept(const RpcJanusAcceptRequest& req) {
        auto __typed_fu_result__ = this->async_JanusAccept(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcJanusAcceptResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<JetpackBeginRecoveryTypedFuture> await_JetpackBeginRecovery(const RpcJetpackBeginRecoveryRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JetpackBeginRecovery(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.id_set, __reply_ar__);
            return rusty::Result<RpcJetpackPullIdSetResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<JetpackPullIdSetTypedFuture> await_JetpackPullIdSet(const RpcJetpackPullIdSetRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JetpackPullIdSet(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.cmd_batch, __reply_ar__);
            return rusty::Result<RpcJetpackPullCmdResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<JetpackPullCmdTypedFuture> await_JetpackPullCmd(const RpcJetpackPullCmdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JetpackPullCmd(req, __fu_attr__));
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<JetpackRecordCmdTypedFuture> await_JetpackRecordCmd(const RpcJetpackRecordCmdRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JetpackRecordCmd(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<JetpackPrepareTypedFuture> await_JetpackPrepare(const RpcJetpackPrepareRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JetpackPrepare(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_max_seen_ballot, __reply_ar__);
            return rusty::Result<RpcJetpackAcceptResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<JetpackAcceptTypedFuture> await_JetpackAccept(const RpcJetpackAcceptRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JetpackAccept(req, __fu_attr__));
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<JetpackCommitTypedFuture> await_JetpackCommit(const RpcJetpackCommitRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JetpackCommit(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.ok, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_jepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_oepoch, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_old_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.reply_new_view, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.cmd, __reply_ar__);
            return rusty::Result<RpcJetpackPullRecSetInsResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<JetpackPullRecSetInsTypedFuture> await_JetpackPullRecSetIns(const RpcJetpackPullRecSetInsRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JetpackPullRecSetIns(req, __fu_attr__));
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<JetpackFinishRecoveryTypedFuture> await_JetpackFinishRecovery(const RpcJetpackFinishRecoveryRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_JetpackFinishRecovery(req, __fu_attr__));
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
        SERVER_SHUTDOWN = 0x4eb35edf,
        SERVER_READY = 0x4965bedc,
        SERVER_HEART_BEAT_WITH_DATA = 0x3caefb9f,
        SERVER_HEART_BEAT = 0x12271c0c,
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<server_shutdownTypedFuture> await_server_shutdown(const RpcServerShutdownRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_server_shutdown(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcServerReadyResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<server_readyTypedFuture> await_server_ready(const RpcServerReadyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_server_ready(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcServerHeartBeatWithDataResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<server_heart_beat_with_dataTypedFuture> await_server_heart_beat_with_data(const RpcServerHeartBeatWithDataRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_server_heart_beat_with_data(req, __fu_attr__));
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<server_heart_beatTypedFuture> await_server_heart_beat(const RpcServerHeartBeatRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_server_heart_beat(req, __fu_attr__));
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
        CLIENT_GET_TXN_NAMES = 0x256199c1,
        CLIENT_SHUTDOWN = 0x3af88122,
        CLIENT_FORCE_STOP = 0x33192842,
        CLIENT_RESPONSE = 0x3b2760e0,
        CLIENT_READY = 0x4e8bdefc,
        CLIENT_READY_BLOCK = 0x2239cb27,
        CLIENT_START = 0x3e9876d4,
        DISPATCHTXN = 0x22e75ded,
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.txn_names, __reply_ar__);
            return rusty::Result<RpcClientGetTxnNamesResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<client_get_txn_namesTypedFuture> await_client_get_txn_names(const RpcClientGetTxnNamesRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_client_get_txn_names(req, __fu_attr__));
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<client_shutdownTypedFuture> await_client_shutdown(const RpcClientShutdownRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_client_shutdown(req, __fu_attr__));
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<client_force_stopTypedFuture> await_client_force_stop(const RpcClientForceStopRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_client_force_stop(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcClientResponseResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<client_responseTypedFuture> await_client_response(const RpcClientResponseRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_client_response(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcClientReadyResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<client_readyTypedFuture> await_client_ready(const RpcClientReadyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_client_ready(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.res, __reply_ar__);
            return rusty::Result<RpcClientReadyBlockResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<client_ready_blockTypedFuture> await_client_ready_block(const RpcClientReadyBlockRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_client_ready_block(req, __fu_attr__));
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
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<client_startTypedFuture> await_client_start(const RpcClientStartRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_client_start(req, __fu_attr__));
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
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.result, __reply_ar__);
            return rusty::Result<RpcDispatchTxnResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
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
    rrr::TypedFutureResultAwaiter<DispatchTxnTypedFuture> await_DispatchTxn(const RpcDispatchTxnRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_DispatchTxn(req, __fu_attr__));
    }
    rusty::Result<RpcDispatchTxnResponse, rrr::i32> DispatchTxn(const RpcDispatchTxnRequest& req) {
        auto __typed_fu_result__ = this->async_DispatchTxn(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcDispatchTxnResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

class ConfigServiceService {
public:
    // Typed request/response scaffolding generated from RPC signature lists.
    struct RpcGetConfigRequest {
        uint64_t client_version;
    };
    friend inline void serialize(const RpcGetConfigRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.client_version, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcGetConfigRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcGetConfigRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.client_version, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcGetConfigRequest& o) { deserialize(o, ar); return ar; }

    struct RpcGetConfigResponse {
        uint64_t current_version;
        rrr::i32 has_update;
        std::string config_data;
    };
    friend inline void serialize(const RpcGetConfigResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.current_version, ar);
        rrr::Serialize_::serialize(o.has_update, ar);
        rrr::Serialize_::serialize(o.config_data, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcGetConfigResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcGetConfigResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.current_version, ar);
        rrr::Deserialize_::deserialize(o.has_update, ar);
        rrr::Deserialize_::deserialize(o.config_data, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcGetConfigResponse& o) { deserialize(o, ar); return ar; }

    struct RpcGetConfigVersionRequest {
    };
    friend inline void serialize(const RpcGetConfigVersionRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcGetConfigVersionRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcGetConfigVersionRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcGetConfigVersionRequest& o) { deserialize(o, ar); return ar; }

    struct RpcGetConfigVersionResponse {
        uint64_t version;
    };
    friend inline void serialize(const RpcGetConfigVersionResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.version, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcGetConfigVersionResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcGetConfigVersionResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.version, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcGetConfigVersionResponse& o) { deserialize(o, ar); return ar; }

    struct RpcHasConfigRequest {
    };
    friend inline void serialize(const RpcHasConfigRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcHasConfigRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcHasConfigRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcHasConfigRequest& o) { deserialize(o, ar); return ar; }

    struct RpcHasConfigResponse {
        rrr::i32 has_config;
    };
    friend inline void serialize(const RpcHasConfigResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.has_config, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcHasConfigResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcHasConfigResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.has_config, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcHasConfigResponse& o) { deserialize(o, ar); return ar; }

    struct RpcSetShardingPolicyRequest {
        std::string policy_data;
    };
    friend inline void serialize(const RpcSetShardingPolicyRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.policy_data, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSetShardingPolicyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSetShardingPolicyRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.policy_data, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSetShardingPolicyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcSetShardingPolicyResponse {
        rrr::i32 success;
    };
    friend inline void serialize(const RpcSetShardingPolicyResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.success, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcSetShardingPolicyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcSetShardingPolicyResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.success, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcSetShardingPolicyResponse& o) { deserialize(o, ar); return ar; }

    struct RpcGetShardingPolicyRequest {
        uint64_t client_version;
    };
    friend inline void serialize(const RpcGetShardingPolicyRequest& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.client_version, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcGetShardingPolicyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcGetShardingPolicyRequest& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.client_version, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcGetShardingPolicyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcGetShardingPolicyResponse {
        uint64_t current_version;
        rrr::i32 has_update;
        std::string policy_data;
    };
    friend inline void serialize(const RpcGetShardingPolicyResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.current_version, ar);
        rrr::Serialize_::serialize(o.has_update, ar);
        rrr::Serialize_::serialize(o.policy_data, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcGetShardingPolicyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcGetShardingPolicyResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.current_version, ar);
        rrr::Deserialize_::deserialize(o.has_update, ar);
        rrr::Deserialize_::deserialize(o.policy_data, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcGetShardingPolicyResponse& o) { deserialize(o, ar); return ar; }

    struct RpcGetShardingPolicyVersionRequest {
    };
    friend inline void serialize(const RpcGetShardingPolicyVersionRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcGetShardingPolicyVersionRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcGetShardingPolicyVersionRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcGetShardingPolicyVersionRequest& o) { deserialize(o, ar); return ar; }

    struct RpcGetShardingPolicyVersionResponse {
        uint64_t version;
    };
    friend inline void serialize(const RpcGetShardingPolicyVersionResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.version, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcGetShardingPolicyVersionResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcGetShardingPolicyVersionResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.version, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcGetShardingPolicyVersionResponse& o) { deserialize(o, ar); return ar; }

    struct RpcHasShardingPolicyRequest {
    };
    friend inline void serialize(const RpcHasShardingPolicyRequest& o, rrr::BinaryWriteArchive& ar) {
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcHasShardingPolicyRequest& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcHasShardingPolicyRequest& o, rrr::BinaryReadArchive& ar) {
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcHasShardingPolicyRequest& o) { deserialize(o, ar); return ar; }

    struct RpcHasShardingPolicyResponse {
        rrr::i32 has_policy;
    };
    friend inline void serialize(const RpcHasShardingPolicyResponse& o, rrr::BinaryWriteArchive& ar) {
        rrr::Serialize_::serialize(o.has_policy, ar);
    }
    friend inline rrr::BinaryWriteArchive& operator <<(rrr::BinaryWriteArchive& ar, const RpcHasShardingPolicyResponse& o) { serialize(o, ar); return ar; }
    friend inline void deserialize(RpcHasShardingPolicyResponse& o, rrr::BinaryReadArchive& ar) {
        rrr::Deserialize_::deserialize(o.has_policy, ar);
    }
    friend inline rrr::BinaryReadArchive& operator >>(rrr::BinaryReadArchive& ar, RpcHasShardingPolicyResponse& o) { deserialize(o, ar); return ar; }

    enum {
        GETCONFIG = 0x6a73203c,
        GETCONFIGVERSION = 0x5fafb6e5,
        HASCONFIG = 0x1049f52e,
        SETSHARDINGPOLICY = 0x14f5087e,
        GETSHARDINGPOLICY = 0x1797df5f,
        GETSHARDINGPOLICYVERSION = 0x5696fa63,
        HASSHARDINGPOLICY = 0x265a5080,
    };
    // Registers RPC IDs with server using service index
    // @unsafe - calls rrr::Server::reg_rpc / unreg (not borrow-checked)
    int __reg_to__(rrr::Server& svr, size_t svc_index) {
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
    void __dispatch__(rrr::i32 rpc_id, rusty::Box<rrr::Request> req, rrr::WeakServerConnection weak_sconn) {
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.client_version, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcGetConfigResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->current_version, m);
                    rrr::Serialize_::serialize(__typed_resp__->has_update, m);
                    rrr::Serialize_::serialize(__typed_resp__->config_data, m);
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
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->version, m);
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
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->has_config, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.policy_data, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcSetShardingPolicyResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->success, m);
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
            rrr::BinaryReadArchive __req_ar__(rrr::make_source_proxy(&req->m));
            rrr::Deserialize_::deserialize(__typed_req__.client_version, __req_ar__);
            auto __typed_resp__ = std::make_shared<RpcGetShardingPolicyResponse>();
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->current_version, m);
                    rrr::Serialize_::serialize(__typed_resp__->has_update, m);
                    rrr::Serialize_::serialize(__typed_resp__->policy_data, m);
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
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->version, m);
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
            auto __defer__ = rrr::DeferredReply::new_(
                std::move(req),
                weak_sconn,
                [__typed_resp__](rrr::BinaryWriteArchive& m) {
                    rrr::Serialize_::serialize(__typed_resp__->has_policy, m);
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.current_version, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.has_update, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.config_data, __reply_ar__);
            return rusty::Result<RpcGetConfigResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<GetConfigTypedFuture, rrr::i32> async_GetConfig(const RpcGetConfigRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::GETCONFIG, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.client_version, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<GetConfigTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<GetConfigTypedFuture, rrr::i32>::Ok(GetConfigTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<GetConfigTypedFuture> await_GetConfig(const RpcGetConfigRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_GetConfig(req, __fu_attr__));
    }
    rusty::Result<RpcGetConfigResponse, rrr::i32> GetConfig(const RpcGetConfigRequest& req) {
        auto __typed_fu_result__ = this->async_GetConfig(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcGetConfigResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.version, __reply_ar__);
            return rusty::Result<RpcGetConfigVersionResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<GetConfigVersionTypedFuture, rrr::i32> async_GetConfigVersion(const RpcGetConfigVersionRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::GETCONFIGVERSION, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<GetConfigVersionTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<GetConfigVersionTypedFuture, rrr::i32>::Ok(GetConfigVersionTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<GetConfigVersionTypedFuture> await_GetConfigVersion(const RpcGetConfigVersionRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_GetConfigVersion(req, __fu_attr__));
    }
    rusty::Result<RpcGetConfigVersionResponse, rrr::i32> GetConfigVersion(const RpcGetConfigVersionRequest& req) {
        auto __typed_fu_result__ = this->async_GetConfigVersion(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcGetConfigVersionResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.has_config, __reply_ar__);
            return rusty::Result<RpcHasConfigResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<HasConfigTypedFuture, rrr::i32> async_HasConfig(const RpcHasConfigRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::HASCONFIG, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<HasConfigTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<HasConfigTypedFuture, rrr::i32>::Ok(HasConfigTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<HasConfigTypedFuture> await_HasConfig(const RpcHasConfigRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_HasConfig(req, __fu_attr__));
    }
    rusty::Result<RpcHasConfigResponse, rrr::i32> HasConfig(const RpcHasConfigRequest& req) {
        auto __typed_fu_result__ = this->async_HasConfig(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcHasConfigResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.success, __reply_ar__);
            return rusty::Result<RpcSetShardingPolicyResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<SetShardingPolicyTypedFuture, rrr::i32> async_SetShardingPolicy(const RpcSetShardingPolicyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::SETSHARDINGPOLICY, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.policy_data, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<SetShardingPolicyTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<SetShardingPolicyTypedFuture, rrr::i32>::Ok(SetShardingPolicyTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<SetShardingPolicyTypedFuture> await_SetShardingPolicy(const RpcSetShardingPolicyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_SetShardingPolicy(req, __fu_attr__));
    }
    rusty::Result<RpcSetShardingPolicyResponse, rrr::i32> SetShardingPolicy(const RpcSetShardingPolicyRequest& req) {
        auto __typed_fu_result__ = this->async_SetShardingPolicy(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcSetShardingPolicyResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.current_version, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.has_update, __reply_ar__);
            rrr::Deserialize_::deserialize(__typed_resp__.policy_data, __reply_ar__);
            return rusty::Result<RpcGetShardingPolicyResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<GetShardingPolicyTypedFuture, rrr::i32> async_GetShardingPolicy(const RpcGetShardingPolicyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::GETSHARDINGPOLICY, __fu_attr__, [&](rrr::BinaryWriteArchive& __m__) {
            rrr::Serialize_::serialize(req.client_version, __m__);
        });
        if (__fu_result__.is_err()) {
            return rusty::Result<GetShardingPolicyTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        return rusty::Result<GetShardingPolicyTypedFuture, rrr::i32>::Ok(GetShardingPolicyTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<GetShardingPolicyTypedFuture> await_GetShardingPolicy(const RpcGetShardingPolicyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_GetShardingPolicy(req, __fu_attr__));
    }
    rusty::Result<RpcGetShardingPolicyResponse, rrr::i32> GetShardingPolicy(const RpcGetShardingPolicyRequest& req) {
        auto __typed_fu_result__ = this->async_GetShardingPolicy(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcGetShardingPolicyResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.version, __reply_ar__);
            return rusty::Result<RpcGetShardingPolicyVersionResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<GetShardingPolicyVersionTypedFuture, rrr::i32> async_GetShardingPolicyVersion(const RpcGetShardingPolicyVersionRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::GETSHARDINGPOLICYVERSION, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<GetShardingPolicyVersionTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<GetShardingPolicyVersionTypedFuture, rrr::i32>::Ok(GetShardingPolicyVersionTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<GetShardingPolicyVersionTypedFuture> await_GetShardingPolicyVersion(const RpcGetShardingPolicyVersionRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_GetShardingPolicyVersion(req, __fu_attr__));
    }
    rusty::Result<RpcGetShardingPolicyVersionResponse, rrr::i32> GetShardingPolicyVersion(const RpcGetShardingPolicyVersionRequest& req) {
        auto __typed_fu_result__ = this->async_GetShardingPolicyVersion(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcGetShardingPolicyVersionResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
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
            auto __reply_guard__ = __fu__->get_reply();
            rrr::BinaryReadArchive __reply_ar__(rrr::make_source_proxy(&*__reply_guard__));
            rrr::Deserialize_::deserialize(__typed_resp__.has_policy, __reply_ar__);
            return rusty::Result<RpcHasShardingPolicyResponse, rrr::i32>::Ok(__typed_resp__);
        }
        auto operator co_await() const {
            return rrr::make_typed_future_awaitable(*this);
        }
    };
    rusty::Result<HasShardingPolicyTypedFuture, rrr::i32> async_HasShardingPolicy(const RpcHasShardingPolicyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        auto __fu_result__ = __cl__->request(ConfigServiceService::HASSHARDINGPOLICY, __fu_attr__, [](rrr::BinaryWriteArchive&) {});
        if (__fu_result__.is_err()) {
            return rusty::Result<HasShardingPolicyTypedFuture, rrr::i32>::Err(__fu_result__.unwrap_err());
        }
        (void)req;
        return rusty::Result<HasShardingPolicyTypedFuture, rrr::i32>::Ok(HasShardingPolicyTypedFuture(__fu_result__.unwrap()));
    }
    rrr::TypedFutureResultAwaiter<HasShardingPolicyTypedFuture> await_HasShardingPolicy(const RpcHasShardingPolicyRequest& req, const rrr::FutureAttr& __fu_attr__ = rrr::FutureAttr()) {
        return rrr::make_typed_future_result_awaitable(this->async_HasShardingPolicy(req, __fu_attr__));
    }
    rusty::Result<RpcHasShardingPolicyResponse, rrr::i32> HasShardingPolicy(const RpcHasShardingPolicyRequest& req) {
        auto __typed_fu_result__ = this->async_HasShardingPolicy(req);
        if (__typed_fu_result__.is_err()) {
            return rusty::Result<RpcHasShardingPolicyResponse, rrr::i32>::Err(__typed_fu_result__.unwrap_err());
        }
        return __typed_fu_result__.unwrap().resolve();
    }
};

} // namespace janus



