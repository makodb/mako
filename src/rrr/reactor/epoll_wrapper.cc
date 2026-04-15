#include "epoll_wrapper.h"

namespace rrr {

void Epoll::Wait() {
	Wait([](int /*fd*/, int /*ready_events*/) {});
}

} // namespace rrr
