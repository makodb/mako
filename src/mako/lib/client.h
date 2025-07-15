// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * Client.h:
 *   Client information - issue eRPC request
 *
 **********************************************************************/

#ifndef _LIB_CLIENT_H_
#define _LIB_CLIENT_H_

#include "lib/fasttransport.h"
#include "lib/common.h"

void register_sync_util_sc(std::function<int()>);

namespace srolis
{
    using namespace std;

    class Client : public TransportReceiver
    {
    public:
        Client(std::string file,
               Transport *transport,
               uint64_t clientid);

        void ReceiveResponse(uint8_t reqType, char *respBuf);

        void InvokeGet(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            const string &key,
                            uint16_t table_id,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);
        
        void InvokeScan(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            const string &start_key,
                            const string &end_key,
                            uint16_t table_id,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);

        void InvokeBatchLock(uint64_t txn_nr,
                            uint16_t id,
                            map<int, BatchLockRequestWrapper> &request_batch_per_shard,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);

        void InvokeLock(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            const string &key,
                            const string &value,
                            uint16_t table_id,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);

        void InvokeValidate(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);

        void InvokeInstall(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            char*cc,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);

        void InvokeUnLock(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);

        void InvokeAbort(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            basic_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);

        void InvokeGetTimestamp(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);
        
        void InvokeExchangeWatermark(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);

        void InvokeControl(uint64_t txn_nr,
                           int control,
                           uint64_t value,
                            int dstShardIdx,
                            uint16_t server_id,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);

        void InvokeWarmup(uint64_t txn_nr,
                           uint32_t req_val,
                           uint8_t centerId,
                            int dstShardIdx,
                            uint16_t server_id,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout);


        void InvokeSerializeUtil(uint64_t txn_nr,
                            int dstShardIdx,
                            uint16_t server_id,
                            char*cc,
                            resp_continuation_t continuation,
                            error_continuation_t error_continuation,
                            uint32_t timeout); 

        void HandleGetReply(char *respBuf);
        void HandleScanReply(char *respBuf);
        void HandleLockReply(char *respBuf);
        void HandleBatchLockReply(char *respBuf);
        void HandleValidateReply(char *respBuf);
        void HandleControlReply(char *respBuf);
        void HandleSerializeUtil(char *respBuf);
        void HandleInstallReply(char *respBuf);
        void HandleUnLockReply(char *respBuf);
        void HandleAbortReply(char *respBuf);
        void HandleGetTimestamp(char *respBuf);
        void HandleWatermarkReply(char *respBuf);
        void HandleWarmupReply(char *respBuf);
        size_t ReceiveRequest(uint8_t reqType, char *reqBuf, char *respBuf) override { PPanic("Not implemented."); };
        bool Blocked() override { return blocked; };
        void SetNumResponseWaiting(int num_response_waiting) { this->num_response_waiting = num_response_waiting; };

    protected:
        struct PendingRequest
        {
            string request;
            uint64_t req_nr;
            uint64_t txn_nr;
            uint16_t id;
            continuation_t continuation;
            bool continuationInvoked = false;

            inline PendingRequest(){};
            inline PendingRequest(string request, uint64_t req_nr,
                                  uint64_t txn_nr, uint16_t server_id,
                                  continuation_t continuation
                                  )
                : request(request),
                  req_nr(req_nr),
                  txn_nr(txn_nr),
                  id(id),
                  continuation(continuation)
                  {};
            virtual ~PendingRequest(){};
        };

        struct PendingRequestK : public PendingRequest
        {
            error_continuation_t error_continuation;
            resp_continuation_t resp_continuation;

            inline PendingRequestK(){};
            inline PendingRequestK(
                string request, uint64_t clientReqId, uint64_t clienttxn_nr,
                uint16_t server_id,
                resp_continuation_t resp_continuation,
                error_continuation_t error_continuation)
                : PendingRequest(request, clientReqId, clienttxn_nr, id, nullptr),
                  error_continuation(error_continuation),
                  resp_continuation(resp_continuation){};
        };

        transport::Configuration config;
        uint32_t lastReqId;
        PendingRequestK crtReqK;
        Transport *transport;
        uint64_t clientid;
        bool blocked;
        int num_response_waiting;
        
        int current_term;
    };

}
#endif  /* _LIB_CONFIGURATION_H_ */
