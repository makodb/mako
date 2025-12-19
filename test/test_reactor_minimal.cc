#include <iostream>
#include <thread>
#include <chrono>
#include "reactor/reactor.h"
#include "reactor/event.h"
#include "reactor/coroutine.h"

using namespace rrr;
using namespace std::chrono;

int main() {
    // Test basic PollThread creation
    PollThread* poll_mgr = new PollThread(1);  // Create with 1 thread
    
    if (poll_mgr != nullptr) {
        std::cout << "PollThread created successfully" << std::endl;
        std::cout << "Number of threads: " << poll_mgr->n_threads_ << std::endl;
    }
    
    // Test basic Reactor creation
    auto reactor = Reactor::GetReactor();
    if (reactor != nullptr) {
        std::cout << "Reactor created successfully" << std::endl;
    }
    
    // Clean up
    delete poll_mgr;
    
    return 0;
}