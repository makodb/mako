
// import std; replacement — see <std_compat.hpp> for rationale.
#include <std_compat.hpp>

// @c-compat-added
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <rusty/arc.hpp>
#include <rusty/rc.hpp>
#include <rusty/rc/weak.hpp>
#include <rusty/refcell.hpp>
#include <unistd.h>
#include <sys/epoll.h>




#include "epoll_wrapper.h"


#include "../rrr.hpp"

namespace rrr {

void Epoll::Wait() {
	Wait([](int /*fd*/, int /*ready_events*/) {});
}

} // namespace rrr
