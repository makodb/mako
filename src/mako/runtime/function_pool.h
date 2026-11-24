#pragma once
#include <cstdint>

// support flexible K-V, each WrappedLogV2 contains one transaction with operations
struct WrappedLogV2{
    char* pairs;
    uint64_t cid;
    unsigned short int count;   // the count of K-V operations
    unsigned int len;   // the len of K-V operations
};
