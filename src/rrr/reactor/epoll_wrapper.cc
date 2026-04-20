module;

#include <rusty/arc.hpp>
#include <rusty/rc.hpp>
#include <rusty/rc/weak.hpp>
#include <rusty/refcell.hpp>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include <cerrno>
#include <sys/epoll.h>




module rrr:impl.reactor.epoll_wrapper;
import rrr;

namespace rrr {

void Epoll::Wait() {
	Wait([](int /*fd*/, int /*ready_events*/) {});
}

} // namespace rrr
