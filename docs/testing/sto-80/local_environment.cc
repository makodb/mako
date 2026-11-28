// Benchmark-only environment: no routing, RPC or durable log. The unchanged
// MassTrans/Transaction sources run their normal local transaction paths.
// These admission results match sharding_admission_enabled()==false. Any
// accidentally reached remote/log path terminates rather than succeeding.
#include "lib/sharding_admission.h"
#include "lib/shardClient.h"
#include <cstdio>
#include <cstdlib>
namespace { [[noreturn]] void forbidden() { std::fputs("reference reached excluded RPC/log path\n", stderr); std::abort(); } }
namespace mako {
bool sharding_has_request() { return false; }
void sharding_engine_start() {}
void sharding_engine_complete() {}
ShardingOperation::ShardingOperation() : outer_(false) {}
ShardingOperation::~ShardingOperation() { if (outer_) forbidden(); }
void ShardClient::beginAttempt(uint32_t) { forbidden(); }
}
extern "C" void excluded_0() asm("_Z13add_log_to_ncPKcijiPt");
extern "C" void excluded_0() { forbidden(); }
extern "C" void excluded_1() asm("_ZN4mako11ShardClient11remoteAbortEv");
extern "C" void excluded_1() { forbidden(); }
extern "C" void excluded_2() asm("_ZN4mako11ShardClient13remoteInstallEj");
extern "C" void excluded_2() { forbidden(); }
extern "C" void excluded_3() asm("_ZN4mako11ShardClient14remoteValidateERj");
extern "C" void excluded_3() { forbidden(); }
extern "C" void excluded_4() asm("_ZN4mako11ShardClient15remoteBatchLockERN5rusty4port3vecW8vec_portW3vec3VecIiNS1_5alloc6GlobalEEERNS6_INSt3__112basic_stringIcNSB_11char_traitsIcEENSB_9allocatorIcEEEES8_EESJ_RNS6_IhS8_EE");
extern "C" void excluded_4() { forbidden(); }
extern "C" void excluded_5() asm("_ZN4mako11ShardClient18remoteGetTimestampERj");
extern "C" void excluded_5() { forbidden(); }
