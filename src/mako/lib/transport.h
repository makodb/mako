// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * transport.h:
 *   message-passing network interface definition
 *
 **********************************************************************/

#ifndef _LIB_TRANSPORT_H_
#define _LIB_TRANSPORT_H_

#include "lib/message.h"
#include <map>
#include <functional>

#define CLIENT_NETWORK_DELAY 0
#define REPLICA_NETWORK_DELAY 0
#define READ_AT_LEADER 1

class TransportReceiver
{
public:
    virtual ~TransportReceiver();
    virtual size_t ReceiveRequest(uint8_t reqType, char *reqBuf, char *respBuf)
    {
        PPanic("Not implemented; did you forget to set the MULTIPLE_ACTIVE_REQUESTS flag accordingly?");
    };
    virtual size_t ReceiveRequest(uint64_t reqHandleIdx, uint8_t reqType, char *reqBuf, char *respBuf)
    {
        PPanic("Not implemented; did you forget to set the MULTIPLE_ACTIVE_REQUESTS flag accordingly?");
    };
    virtual void ReceiveResponse(uint8_t reqType, char *respBuf) = 0;
    virtual bool Blocked() = 0;
};

typedef std::function<void(void)> timer_callback_t;

class Transport
{
public:
    // virtual bool SendResponse(uint64_t bufIdx, size_t msgLen) = 0;
    virtual bool SendRequestToAll(TransportReceiver *src, uint8_t reqType, int shards_to_send_bit_set, uint16_t id, size_t respMsgLen, size_t reqMsgLen, int forceCenter = -1) = 0;

    /*
     * In SendBatchRequestToAll, the content of request sent to each shard could be different.
     */
    virtual bool SendBatchRequestToAll(
        TransportReceiver *src,
        uint8_t req_type,
        uint16_t id,
        size_t resp_msg_len,
        const std::map<int, std::pair<char*, size_t>> &data_to_send
    ) = 0;
    virtual bool SendRequestToShard(TransportReceiver *src, uint8_t reqType, uint8_t shardIdx, uint16_t coreIdx, size_t msgLen) = 0;
    virtual int Timer(uint64_t ms, timer_callback_t cb) = 0;
    virtual bool CancelTimer(int id) = 0;
    virtual void CancelAllTimers() = 0;

    virtual char *GetRequestBuf(size_t reqLen, size_t respLen) = 0;
    virtual int GetSession(TransportReceiver *src, uint8_t replicaIdx, uint16_t dstRpcIdx, int forceCenter) = 0;

    virtual uint16_t GetID() = 0;

    virtual void Statistics() = 0;
};

class Timeout
{
public:
    Timeout(Transport *transport, uint64_t ms, timer_callback_t cb);
    virtual ~Timeout();
    virtual void SetTimeout(uint64_t ms);
    virtual uint64_t Start();
    virtual uint64_t Reset();
    virtual void Stop();
    virtual bool Active() const;

private:
    Transport *transport;
    uint64_t ms;
    timer_callback_t cb;
    int timerId;
};

#endif // _LIB_TRANSPORT_H_
