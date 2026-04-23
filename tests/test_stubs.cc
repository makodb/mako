// Stubs for missing symbols needed by some tests
// These provide minimal implementations when full txn.cc is not needed

#include <string>
#include <cstdint>
#include <sstream>

// Stub for g_proto_version_str used in tuple.cc formatting
// Real implementation is in txn.cc but brings many dependencies
static std::string stub_version_str(uint64_t v) {
    std::ostringstream b;
    b << "[v=" << v << "]";
    return b.str();
}

std::string (*g_proto_version_str)(uint64_t v) = stub_version_str;
