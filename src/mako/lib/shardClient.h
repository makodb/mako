
#ifndef _LIB_SHARDCLIENT_H_
#define _LIB_SHARDCLIENT_H_

#include <atomic>

#include <rusty/rusty.hpp>
#include "lib/fasttransport.h"
#include "lib/client.h"
#include "lib/promise.h"
#include "lib/common.h"
import rusty;

namespace mako
{
    using namespace std;

    class ShardClient
    {
    public:
        ShardClient(std::string file, string cluster, int shardIndex, int par_id,
                    bool ephemeral_client = false);
        // @unsafe - preserves unresolved participant obligations, then closes the
        // legacy transport before destroying callback state.
        ~ShardClient();
        ShardClient(const ShardClient&) = delete;
        ShardClient& operator=(const ShardClient&) = delete;
        // @unsafe - Begin once before the first remote operation of an attempt.
        void beginAttempt(uint32_t term);
        // @safe - Includes read-only participants until terminal cleanup.
        bool hasTouchedParticipants() const noexcept { return touched_shards != 0; }
        // @unsafe - immediately dispatches the thin point under storage's logical guard.
        int remoteGet(int remote_table_id, const std::string& key, std::string &value,
                      const ShardingPoint& point);
        // @unsafe - callers without a captured point (Rust STO participants): the
        // point is resolved here and dispatched at once.
        int remoteGet(int remote_table_id, const std::string& key, std::string &value);
        // @unsafe - captures and explicitly transports this scan's grant and snapshot.
        int remoteScan(int remote_table_id, std::string start_key, std::string end_key, std::string &value);
        // @unsafe - one bounded transaction piece, never a terminal/EOF shortcut.
        int fullScanPage(int table_id, const ShardingRequest& sharding,
                         const uint8_t* payload, size_t length,
                         std::string& response);

        // Self-contained non-transactional writes (docs/storage-interface.md).
        // The op runs as a one-op OCC transaction on the owning shard and
        // replicates via the normal commit path there. Returns
        // ErrorCode::SUCCESS/ERROR/TIMEOUT/SERVER_BUSY; the op's
        // Masstree-parity boolean ("newly inserted" for put/insert,
        // "existed" for remove) comes back via *op_result.
        // Native mode: the storage boundary owns a ShardingOperation across
        // timeout retries. Its TxnId and first owner+epoch grant are unchanged;
        // ERROR is terminal admission failure, never a key-not-found result.
        int nontxnPut(int remote_table_id, const std::string &key,
                      const std::string &value, bool *op_result, const ShardingPoint& point);
        int nontxnInsert(int remote_table_id, const std::string &key,
                         const std::string &value, bool *op_result, const ShardingPoint& point);
        int nontxnGet(int remote_table_id, const std::string &key,
                      std::string &value, const ShardingPoint& point);
        int nontxnRemove(int remote_table_id, const std::string &key,
                         bool *op_result, const ShardingPoint& point);
        // @unsafe - ingress forwarding retains its external ID and selected grant.
        int forwardNontxn(const ShardingRequest& request, uint8_t kind,
                          uint16_t legacy_table, const std::string& key,
                          const std::string& value, bool* result, std::string* output);

        // Single timestamp interfaces
        int remoteGetTimestamp(uint32_t &timestamp);
        int remoteExchangeWatermark(uint32_t &watermark, uint64_t set_bits);
        int remoteControl(int control, uint32_t value, uint32_t &ret_value, uint64_t set_bits);
        int remoteAbort();
        // @unsafe - engine API: operation 0 stages a put, any other value a delete.
        int remoteBatchLock(rusty::Vec<int>& remote_table_id_batch,
                            rusty::Vec<string>& key_batch, rusty::Vec<string>& value_batch,
                            rusty::Vec<uint8_t>& operation_batch);
        // @unsafe - participant wire kinds: 0 = put, 1 = insert, 2 = remove.
        int remoteBatchLock(vector<int> &remote_table_id_batch, vector<string> &key_batch,
                            vector<string> &value_batch,
                            const vector<uint8_t> *operations = nullptr);
        int remoteValidate(uint32_t &watermark);
        int remoteInstall(uint32_t timestamp);
        int remoteUnLock();
        int warmupRequest(uint32_t req_val, uint8_t centerId, uint32_t &ret_value, uint64_t set_bits);
        // Check if a remote shard is ready to receive requests
        // Returns SUCCESS if ready, ERROR/TIMEOUT if not ready yet
        int checkRemoteShardReady(int dstShardIndex);
        int remoteInvokeSerializeUtil(uint32_t timestamp);
        void statistics();
        void stop();
        void setBreakTimeout(bool);
        void setBlocking(bool);
        bool getBreakTimeout();
        bool isBreakTimeout;
        std::atomic<bool> isBlocking;
        bool stopped;
    protected:
        transport::Configuration config;
        Transport *transport;
        mako::Client *client;
        int shardIndex;
        std::string cluster;
        int clusterRole;
        int par_id;  // in mako, each worker thread has a partition
        Promise *waiting; // waiting thread
        int tid;
        uint64_t touched_shards = 0;

        int num_response_waiting = 0;
        int num_response_received = 0;
        bool responses_successful = true;
        vector<uint64_t> int_received; // indexed by shard

        /* Callbacks for hearing back from a shard for an operation. */
        void GetCallback(char *respBuf);
        void ScanCallback(char *respBuf);
        void FullScanCallback(char* respBuf);
        void BasicCallBack(char *respBuf);
        void NontxnWriteCallback(char *respBuf);

        // Shared body of nontxnPut/nontxnInsert/nontxnRemove.
        int nontxnWrite(uint8_t reqType, int remote_table_id,
                        const std::string &key, const std::string &value,
                        bool *op_result, const ShardingPoint& point);

        /* Timeout which only go to one shard. */
        void GiveUpTimeout();
        // void VectorIntCallback(char *respBuf);

        void SendToAllStatusCallBack(char *respBuf);
        void SendToAllIntCallBack(char *respBuf);
        void SendToAllGiveUpTimeout();
        int is_all_response_ok();
        void calculate_num_response_waiting(uint64_t shards_to_send_bits);
        void calculate_num_response_waiting_no_skip(uint64_t shards_to_send_bits);
        // @unsafe - shared body of both remoteBatchLock overloads; operation_at
        // yields the wire mutation kind of row i.
        template <typename Tables, typename Strings, typename OperationAt>
        int stageRemoteBatch(size_t rows, Tables &remote_table_id_batch,
                             Strings &key_batch, Strings &value_batch,
                             OperationAt operation_at);

    };

}

#endif
