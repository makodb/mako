#include "../srpc.hpp"

import std;

using namespace srpc;
using namespace std::chrono;

int main() {
    // Test basic PollThread creation (now uses factory method returning Arc)
    auto poll_mgr = PollThread::create();

    if (poll_mgr) {
        std::cout << "PollThread created successfully" << std::endl;
        // Note: n_threads_ member no longer exists in new API design
        // The new design uses a single worker thread per PollThread
    }

    // Test basic Reactor creation (returns Rc).
    //
    // This used to be `if (reactor)`. `rusty::Rc` deliberately has no
    // `operator bool` — unlike the historical `shared_ptr`, an `Rc` is never
    // null by construction, so a null test would be dead code that always
    // took the true branch. The property actually worth smoke-testing is the
    // one the reactor's thread-local factory promises: two calls on the same
    // thread hand back the same object.
    auto reactor = Reactor::get_reactor();
    auto reactor_again = Reactor::get_reactor();
    if (&*reactor != &*reactor_again) {
        std::cerr << "Reactor TLS handed back two different reactors"
                  << std::endl;
        return 1;
    }
    std::cout << "Reactor created successfully" << std::endl;

    // Shutdown the poll thread (Arc handles cleanup)
    poll_mgr->shutdown();

    return 0;
}