
#pragma once

// External safety annotations for types included transitively through this umbrella header
// These annotations ensure the borrow checker recognizes types as unsafe when analyzing
// files that include rrr.hpp
// @external: {
//   rrr::ServerConnection: [unsafe_type]
// }

#include "base/all.hpp"

#include "misc/stat.hpp"
#include "misc/dball.hpp"
#include "misc/alarm.hpp"
#include "misc/alock.hpp"
#include "misc/rand.hpp"
#include "misc/marshal.hpp"
#include "misc/recorder.hpp"
#include "misc/cpuinfo.hpp"
#include "misc/netinfo.hpp"
#include "misc/io.hpp"

#include "reactor/reactor.h"
#include "reactor/coroutine.h"
#include "reactor/event.h"
#include "reactor/epoll_wrapper.h"

#include "rpc/utils.hpp"
#include "rpc/client.hpp"
#include "rpc/server.hpp"

namespace base = rrr;

