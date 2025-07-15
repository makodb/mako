#include <iostream>
#include "rpc.h"
#include "rpc_constants.h"

static int clientPort = 31000;
static int serverPort = 32000;
static int kMsgSize = 96; // reserved message size

const int value_size = 10;

struct basic_req_t {
    uint64_t req_nr; // req_id
    uint16_t tid; // thread_id
    char value[value_size];
} ;

struct req_tag_t
{
    erpc::MsgBuffer req_msgbuf;
    erpc::MsgBuffer resp_msgbuf;
    bool blocked;
    size_t latency;
};

class context_t {
public:
    erpc::Rpc<erpc::CTransport> *rpc = nullptr;
    size_t start_tsc_;
}; 