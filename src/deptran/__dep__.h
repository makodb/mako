#pragma once

// Establish the full textual STL surface BEFORE any `import std` (which arrives
// transitively via the imported srpc modules) — the same ordering workaround the
// srpc targets use. Without it, late textual <stack>/<functional>/<cinttypes>
// (pulled by server.h / memdb/row.h / this header) clash with the std module:
// "cannot add 'abi_tag' attribute in a redeclaration". Must stay first; deptran
// TUs include __dep__.h before anything else. See src/srpc/std_compat.hpp.
#include <std_compat.hpp>

//C++ standard library
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <string>
#include <vector>
#include <list>
#include <chrono>
#include <thread>
#include <iostream>
#include <condition_variable>
#include <atomic>
#include <algorithm>
#include <set>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <filesystem>

using namespace std;
// using the following will trigger errors in CLion (<= 2017.3)
// using std::map;
// using std::unordered_map;
// using std::string;
// using std::vector;
// using std::list;
// using std::set;
// using std::unordered_set;
// using std::pair;
// using std::function;
// using std::shared_ptr;
// using std::unique_ptr;

// system library
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>
#include <errno.h>

// yaml-cpp
#include <yaml-cpp/yaml.h>

// c library
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <stdint.h>
#ifndef PRId32
#define PRId32 "d"
#endif
#ifndef PRId64
#if defined(__APPLE__) && defined(__MACH__)
#define PRId64 "lld"
#else
#define PRId64 "ld"
#endif
#endif
#ifndef PRIx64
#if defined(__APPLE__) && defined(__MACH__)
#define PRIx64 "llx"
#else
#define PRIx64 "lx"
#endif
#endif

// google library

// misc helper files
#include "srpc/srpc.hpp"
// The variadic Log_* wrappers now live outside src/srpc (see the header).
#include "srpc_log.h"

using namespace srpc;


using srpc::Log;
using srpc::i8;
using srpc::i16;
using srpc::i32;
using srpc::i64;
using srpc::Future;
using srpc::RandomGenerator;
// removed `using srpc::Recorder;` — class deleted.
using srpc::AvgStat;
using srpc::PollThread;
// retired
// `using srpc::Marshallable` and `using srpc::MarshallDeputy` —
// the underlying classes are gone in this same release.

// User include files
//

#include "memdb/value.h"
#include "memdb/schema.h"
#include "memdb/table.h"
#include "memdb/txn.h"
#include "memdb/txn_2pl.h"
#include "memdb/txn_occ.h"
#include "memdb/txn_unsafe.h"
#include "memdb/utils.h"
#include "memdb/row.h"
#include "deptran/marshal-value.h"
using mdb::Value;
using mdb::Row;
using mdb::VersionedRow;
using mdb::symbol_t;
using mdb::Table;
using mdb::colid_t;
using mdb::SnapshotTable;

// rpc library
class dummy_class {
 public:
  dummy_class() {
#ifdef LOG_LEVEL_AS_DEBUG
    Log::set_level(Log::DEBUG);
#else
    Log::set_level(Log::INFO);
#endif
  }
};
static dummy_class dummy___;

#include "constants.h"
typedef map<innid_t, map<int32_t, Value>> TxnOutput;
