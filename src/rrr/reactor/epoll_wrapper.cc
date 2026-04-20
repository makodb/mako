module;

#include <rusty/arc.hpp>
#include <rusty/rc.hpp>
#include <rusty/rc/weak.hpp>
#include <rusty/refcell.hpp>
#include <unistd.h>
#include <sys/epoll.h>




module rrr:impl.reactor.epoll_wrapper;

import <array>;
import <algorithm>;
import <cstring>;
import <memory>;
import <vector>;
import <cerrno>;
import rrr;

namespace rrr {

void Epoll::Wait() {
	Wait([](int /*fd*/, int /*ready_events*/) {});
}

} // namespace rrr
