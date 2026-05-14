#include <rusty/arc.hpp>
#include <rusty/rc.hpp>
#include <rusty/rc/weak.hpp>
#include <rusty/refcell.hpp>
#include <unistd.h>
#include <sys/epoll.h>

#include "epoll_wrapper.h"
#include "../rrr.hpp"

// `import std;` lands after every textual `#include`. libc++ rejects
// textual STL that appears AFTER `import std;` in the TU, so we keep
// rusty-cpp / system headers (which pull in textual STL transitively)
// at the top and `import std;` at the bottom of the preamble.
import std;

namespace rrr {

void Epoll::Wait() {
	Wait([](int /*fd*/, int /*ready_events*/) {});
}

} // namespace rrr
