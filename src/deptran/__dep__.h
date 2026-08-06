#pragma once

// Establish the full textual STL surface BEFORE any `import std` (which arrives
// transitively via the imported rrr modules) — the same ordering workaround the
// rrr targets use. Without it, late textual <stack>/<functional>/<cinttypes>
// (pulled by server.h / memdb/row.h / this header) clash with the std module:
// "cannot add 'abi_tag' attribute in a redeclaration". Must stay first; deptran
// TUs include __dep__.h before anything else. See src/rrr/std_compat.hpp.
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
#include <cinttypes>

// google library

// misc helper files
#include "rrr/rrr.hpp"
// The variadic Log_* wrappers now live outside src/rrr (see the header).
#include "rrr_log.h"

using namespace rrr;


using rrr::Log;
using rrr::i8;
using rrr::i16;
using rrr::i32;
using rrr::i64;
using rrr::Future;
using rrr::RandomGenerator;
// removed `using rrr::Recorder;` — class deleted.
using rrr::AvgStat;
using rrr::PollThread;
// retired
// `using rrr::Marshallable` and `using rrr::MarshallDeputy` —
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
