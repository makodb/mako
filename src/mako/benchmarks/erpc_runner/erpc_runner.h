#pragma once

#include "rpc.h"
#include "configuration.h"
#include "common.h"
#include "erpc_runner_queue.h"

#include <map>


struct AppContext {
    std::map<DATA_TYPE, ERPCMessageClientQueue*> *client_holder;
    std::map<DATA_TYPE, ERPCMessageServerQueue*> *server_holder;
};


class ERPCRunner {
public:
    ERPCRunner(const std::string &file, DATA_TYPE erpc_runner_id, DATA_TYPE replica_id, DATA_TYPE shard_id);

    ERPCMessageClientQueue* register_client(DATA_TYPE client_id, DATA_TYPE replica_id, DATA_TYPE shard_id);
    ERPCMessageServerQueue* register_server(DATA_TYPE server_id, DATA_TYPE replica_id, DATA_TYPE shard_id);

    void loading_finished();
    void run();
    void stop();
    
private:
    DATA_TYPE get_session(DATA_TYPE target_shard_id, DATA_TYPE target_server_id);
    void send_cached_request();
    void send_cached_response();

    std::map<DATA_TYPE, ERPCMessageClientQueue*> client_holder;
    std::map<DATA_TYPE, ERPCMessageServerQueue*> server_holder;

    erpc::Rpc<erpc::CTransport> *rpc;
    erpc::Nexus *nexus;
    Configuration config;
    std::map<std::pair<int, int>, int> sessions;
    DATA_TYPE erpc_runner_id, replica_id, shard_id;

    AppContext app_context;

    std::mutex client_holder_mutex;
    std::mutex server_holder_mutex;
    std::mutex erpc_sessions_mutex;

    bool stopped, is_loading_finished;
};
