#include <iostream>
#include <thread>
#include <chrono>
#include "../rrr.hpp"

using namespace rrr;
using namespace std::chrono;

int main() {
    // Test basic PollThread creation (now uses factory method returning Arc)
    auto poll_mgr = PollThread::create();

    if (poll_mgr) {
        std::cout << "PollThread created successfully" << std::endl;
        // Note: n_threads_ member no longer exists in new API design
        // The new design uses a single worker thread per PollThread
    }

    // Test basic Reactor creation (returns Rc)
    auto reactor = Reactor::get_reactor();
    if (reactor) {
        std::cout << "Reactor created successfully" << std::endl;
    }

    // Shutdown the poll thread (Arc handles cleanup)
    poll_mgr->shutdown();

    return 0;
}