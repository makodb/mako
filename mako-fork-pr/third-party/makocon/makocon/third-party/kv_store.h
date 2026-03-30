
#ifndef KV_STORE_H
#define KV_STORE_H

#include <iostream>
#include <string>
#include <map>
#include <queue>
#include <unordered_map>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/atomic.hpp>
#include <boost/chrono.hpp>

using namespace std;

struct Request {
    int id;
    string operation;
    string key;
    string value;
};

struct Response {
    int id;
    string result;
    bool success;
};

class KVStore {
public:
    KVStore();
    ~KVStore();

    bool init();
    int sendtoqueue(string request);
    string recvfromqueue(int reqId);

private:
    void processRequests();
    void executeRequest(const Request& req);
    
    // Core storage
    std::map<std::string, std::string> store_;
    
    // Request/Response queues
    std::queue<Request> request_queue_;
    std::unordered_map<int, Response> response_map_;
    
    // Thread synchronization
    boost::mutex queue_mutex_;
    boost::mutex response_mutex_;
    
    // Worker thread
    boost::thread worker_thread_;
    boost::atomic<bool> running_;
    
    // Request ID generation
    boost::atomic<int> next_id_;
    bool initialized_;
};

#endif
