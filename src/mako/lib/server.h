#ifndef _LIB_SERVER_H_
#define _LIB_SERVER_H_

#include <iostream>
#include <random>
#include <chrono>
#include <thread>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include "lib/fasttransport.h"
#include "lib/timestamp.h"
#include "lib/common.h"
#include "storage/abstract_db.h"
#include "storage/abstract_ordered_index.h"
#include "lib/helper_queue.h"
#include <rusty/mutex.hpp>
#include "rocks_interface/gateway_protocol.h"

void register_sync_util_ss(std::function<int()>);

namespace mako
{
    using namespace std;

    class ShardReceiver : TransportReceiver
    {
    public:
        ShardReceiver(std::string file);
        // Register the startup table map before request handling.
        void Register(abstract_db *db,
                 const map<int, abstract_ordered_index *> &open_tables_table_id /*,
                 const map<string, vector<abstract_ordered_index *>> &partitions,
                 const map<string, vector<abstract_ordered_index *>> &remote_partitions*/);
        // Synchronized publication of one (possibly lazily adopted) index.
        void UpdateTableEntry(int table_id, abstract_ordered_index *table);

        // Message handlers.
        size_t ReceiveRequest(uint8_t reqType, char *reqBuf, char *respBuf);

        void ReceiveResponse(uint8_t reqType, char *respBuf) override{}; // TODO: for now, replicas
                                                                         // do not need to communicate
                                                                         // with eachother; they will need
                                                                         // to for synchronization
        bool Blocked() override { return false; };
        // new handlers
        void HandleGetRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleScanRequest(char *reqBuf, char *respBuf, size_t &respLen);
        // @unsafe - bounded raw scan page in the originating participant txn.
        void HandleFullScanRequest(char* request, char* response, size_t& length);
        // Self-contained non-txn writes (docs/storage-interface.md):
        // runs the op as a local one-op OCC txn via the L3 non-txn API
        // (put / insert / remove selected by reqType).
        void HandleNontxnWriteRequest(uint8_t reqType, char *reqBuf,
                                      char *respBuf, size_t &respLen);

        // Shared core of the self-contained non-txn ops (types 14-17):
        // runs one op on the CALLING thread via the L3 non-txn API (an
        // internal one-op OCC transaction using the backend's normal commit
        // machinery; this helper alone does not certify replication). The
        // calling thread must be Silo-registered (helper threads and
        // ClientTcpServer workers are). Returns ErrorCode::SUCCESS /
        // SERVER_BUSY (this
        // thread's participant txn holds staged 2PC state; retry) /
        // ABORT (get: key not found) / ERROR (non-leader write or
        // unknown table). op_result: put="newly inserted",
        // insert="inserted", remove="was present", get="found".
        // get_out (get only) receives the value with the EXTRA_BITS
        // suffix already stripped by the L3 get.
        int RunNontxnOp(uint8_t opType, uint16_t table_id,
                        const std::string &key, const std::string &value,
                        bool *op_result, std::string *get_out);
        void HandleLockRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleBatchLockRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleValidateRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleGetTimestampRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleSerializeUtilRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleAbortRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleInstallRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleUnLockRequest(char *reqBuf, char *respBuf, size_t &respLen);

        void HandleGetMegaRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleBatchLockMegaRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleGetMicroMegaRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleBatchLockMicroMegaRequest(char *reqBuf, char *respBuf, size_t &respLen);

        // Client API handlers (for decoupled client-server mode)
        // @unsafe - handles raw buffer pointers from transport layer
        void HandleClientBeginTxnRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientCommitRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientRollbackRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientPutRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientGetRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientDeleteRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientRouteRequest(char *reqBuf, char *respBuf, size_t &respLen);
        void HandleClientInsertRequest(char *reqBuf, char *respBuf, size_t &respLen);

        // @safe - immutable owner captured when this receiver is registered.
        int GetOwner() const { return owner_; }

        // @unsafe - snapshot of process-lifetime legacy table pointers under lock.
        map<int, abstract_ordered_index *> GetOpenTables() const {
            auto tables = tables_.lock().unwrap();
            return *tables;
        }

        // @safe - Get database reference
        abstract_db* GetDb() const { return db; }
        // @safe - A sealed empty frame still owes its coordinator a terminal.
        bool CanParkParticipant() const noexcept {
            return (!participant_sealed || participant_terminal) && !participant_unknown;
        }

        // @unsafe - legacy engine callbacks; all identity/session/result state
        // belongs to the native Rust gateway. No multi-operation atomicity.
        void ClientOperation(const MakoGatewayRequest& request, MakoGatewayResponse& response);
        void ClientRoute(const MakoGatewayRequest& request, MakoGatewayResponse& response);
        void BeginClientTransaction(const MakoGatewayRequest& request, MakoGatewayResponse& response);
        void CommitClientTransaction(const MakoGatewayRequest& request, MakoGatewayResponse& response);
        void RollbackClientTransaction(const MakoGatewayRequest& request, MakoGatewayResponse& response);
        static void ExecuteClientOperation(void* receiver, const MakoGatewayRequest* request,
                                           MakoGatewayResponse* response);

    protected:
        inline void *txn_buf() { return (void *) txn_obj_buf.data(); }

    private:
        transport::Configuration config;
        // @unsafe - legacy handler dispatch, called only through ReceiveRequest.
        size_t DispatchRequest(uint8_t reqType, char* reqBuf, char* respBuf);
        // @unsafe - synchronized lookup; published indexes live for the process.
        abstract_ordered_index* table_for(int id) const;
        abstract_ordered_index* require_table(int id) const;

        // Opaque wire replies, retained until this sequential client advances.
        // The receiver lock also serializes legacy ambient-engine dispatch.
        struct ShardingPeer {
            uint64_t sequence = 0;
            uint32_t request = 0;
            uint8_t kind = 0;
            bool terminal = false;
            bool aborted = false;
            std::string reply;
        };
        rusty::Mutex<std::unordered_map<uint64_t, ShardingPeer>> sharding_peers_{
            std::unordered_map<uint64_t, ShardingPeer>{}};

        // std::vector<uint64_t> latency_get;
        // std::vector<uint64_t> latency_prepare;
        // std::vector<uint64_t> latency_commit;

        // store layer
        abstract_db *db;
        int owner_ = -1;
        mutable rusty::Mutex<map<int, abstract_ordered_index *>> tables_{
            map<int, abstract_ordered_index *>{}};
        // map<string, vector<abstract_ordered_index *>> partitions;
        // map<string, vector<abstract_ordered_index *>> remote_partitions;

        uint64_t txn_flags = 0;
        std::string txn_obj_buf;
        str_arena arena;

        string obj_key0;
        string obj_key1;
        string obj_v;

        int current_term ;
        ParticipantIdentity admitted_identity{};
        bool participant_terminal = false;
        bool participant_committed = false;
        bool participant_sealed = false;
        bool participant_unknown = false;
        uint8_t cached_request_type = 0;
        size_t cached_response_size = 0;
        char cached_response[sizeof(get_response_t)]{};

        // @safe - Requests fenced by ParticipantIdentity: the legacy 2PC kinds
        // and the full-scan page, which is a piece of the same participant.
        static constexpr bool IsParticipantRequest(uint8_t type) noexcept {
            return (type >= getReqType && type <= batchLockReqType) ||
                   type == fullScanReqType;
        }
        bool AdmitParticipant(uint8_t type, char *request, char *response,
                              size_t &response_size);
        void CompleteParticipant(uint8_t type, char *response, size_t response_size);
        size_t RejectParticipant(uint8_t type, const basic_request_t &request,
                                 char *response, int status);
        // @unsafe - The sharding boundary ended the admitted attempt's frame.
        void TerminateParticipant(const ParticipantIdentity &identity);
        // @unsafe - A standalone one-op transaction may neither run on nor end
        // this lane's participant frame while this holds (see RunNontxnOp).
        bool ParticipantFrameBusy() const;

    };

    class ShardServer
    {
    public:
        ShardServer(std::string file, int clientShardIndex, int shardIndex, int par_id);
        ~ShardServer();
        ShardServer(const ShardServer &) = delete;
        ShardServer &operator=(const ShardServer &) = delete;
        ShardServer(ShardServer &&) = delete;
        ShardServer &operator=(ShardServer &&) = delete;
        // Register the startup table map before Run() starts. The receiver keeps
        // its own synchronized copy, which UpdateTable extends at run time.
        void Register(abstract_db *db,
                 mako::HelperQueue *queue,
                 mako::HelperQueue *queue_res,
                 const map<int, abstract_ordered_index *> &open_tables /*,
                 const map<string, vector<abstract_ordered_index *>> &partitions,
                 const map<string, vector<abstract_ordered_index *>> &remote_partitions*/);
        // Publishes one index opened after startup (lazily adopted native
        // warehouse tables included) to the receiver's synchronized map while
        // Run() may be serving. Callers serialize through setup_update_table's
        // helper lifecycle lock: this object's own startup copy is unsynchronized
        // and is never read by Run().
        void UpdateTable(int table_id, abstract_ordered_index *table);
        void Run();

        // Get the underlying ShardReceiver (for ClientTcpServer integration)
        // @safe - Returns borrowed pointer
        ShardReceiver* GetReceiver() { return shardReceiver; }

    protected:
        transport::Configuration config;
        mako::ShardReceiver *shardReceiver;
        // create a shard-server on {clientShardIndex} to receive a client request from 
        //  a TPC-C worker thread <shardIndex, par-id>
        int clientShardIndex;
        int serverShardIndex;
        int par_id;

        // store layer
        abstract_db *db;
        mako::HelperQueue *queue;
        mako::HelperQueue *queue_response;
        map<int, abstract_ordered_index *> open_tables_table_id;
        // map<string, vector<abstract_ordered_index *>> partitions;
        // map<string, vector<abstract_ordered_index *>> remote_partitions;
    };
}
#endif
