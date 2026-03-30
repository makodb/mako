#pragma once
#include "MassTrans.hh"
#include <vector>

typedef MassTrans<std::string, versioned_str_struct, false/*opacity*/> actual_directs;

// support flexible K-V, each WrappedLogV2 contains one transaction with operations
struct WrappedLogV2{
    char* pairs;
    uint64_t cid;
    unsigned short int count;   // the count of K-V operations
    unsigned int len;   // the len of K-V operations
};