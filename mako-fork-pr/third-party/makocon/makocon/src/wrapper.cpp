// wrapper.cpp
#include "wrapper.h"
#include "../third-party/kv_store.h"
#include <iostream>

// This KVStore is not a really instance of KVStore; it is a wrapper in our database code to intercept requests and execute them!
static KVStore* kv_store_instance = nullptr;

bool Wrapper::init() {
    if (kv_store_instance == nullptr) {
        kv_store_instance = new KVStore();
        return kv_store_instance->init();
    }
    return true;
}

int Wrapper::sendtoqueue(string request)  {
    if (kv_store_instance == nullptr) {
        return -1; // Not initialized
    }
    return kv_store_instance->sendtoqueue(request);
}

std::string Wrapper::recvfromqueue(int reqId) {
    if (kv_store_instance == nullptr) {
        return ""; // Not initialized
    }
    return kv_store_instance->recvfromqueue(reqId);
}