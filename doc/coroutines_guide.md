# Understanding Coroutines and the Reactor Pattern in RRR

## Table of Contents
1. [Introduction: Why Coroutines?](#introduction-why-coroutines)
2. [Threads vs Coroutines](#threads-vs-coroutines)
3. [The Reactor Pattern](#the-reactor-pattern)
4. [Using RRR Coroutines](#using-rrr-coroutines)
5. [The Event System](#the-event-system)
6. [The Scheduler: Why Coroutines Need Resume](#the-scheduler-why-coroutines-need-resume)
7. [Common Patterns and Best Practices](#common-patterns-and-best-practices)
8. [Pitfalls to Avoid](#pitfalls-to-avoid)

## Introduction: Why Coroutines?

If you're familiar with threads, you know they're great for concurrent programming but come with costs:
- **Context switching overhead**: The OS kernel must save/restore CPU state
- **Memory overhead**: Each thread needs its own stack (typically 1-8 MB)
- **Synchronization complexity**: Mutexes, deadlocks, race conditions

Coroutines offer a lighter alternative. Think of them as **functions that can pause and resume**, allowing thousands of concurrent operations with minimal overhead.

### Real-World Analogy
Imagine you're a chef in a restaurant:
- **Thread approach**: Hire multiple chefs, each handling one order start-to-finish
- **Coroutine approach**: One chef efficiently switches between orders while waiting (for oven, boiling water, etc.)

The coroutine approach is what RRR implements - efficient task switching without the overhead of multiple threads.

## Threads vs Coroutines

### Threads (OS-level)
```cpp
// Traditional thread - expensive context switch
std::thread worker([]() {
    auto data = fetch_from_database();  // Blocks thread
    process(data);
});
```

**Characteristics:**
- Managed by the OS kernel
- Preemptive: OS decides when to switch
- ~1-8 MB stack per thread
- Kernel-level context switch (~1-10 microseconds)
- True parallelism on multiple cores
- **Requires synchronization**: Mutexes, locks, atomic operations

### Coroutines (User-level)
```cpp
// RRR coroutine - cheap context switch
reactor->CreateRunCoroutine([]() {
    auto event = wait_for_database();
    event->Wait();  // Yields to other coroutines
    process(event->data);
});
```

**Characteristics:**
- Managed by your application (the reactor)
- Cooperative: Coroutine decides when to yield
- ~4-64 KB stack per coroutine
- User-level context switch (~10-100 nanoseconds)
- Concurrency within a single thread
- **No synchronization needed**: All coroutines run in the same thread!

### The Thread Safety Advantage

One of the **biggest advantages** of coroutines is that you don't need to worry about thread safety:

#### With Threads - Synchronization Required
```cpp
// Multiple threads accessing shared data - DANGEROUS without locks!
class BankAccount {
    int balance = 1000;
    std::mutex mtx;  // Need mutex for thread safety
    
public:
    void withdraw(int amount) {
        std::lock_guard<std::mutex> lock(mtx);  // Must lock!
        if (balance >= amount) {
            balance -= amount;  // Without lock, race condition!
        }
    }
};

// Two threads accessing same account
BankAccount account;
std::thread t1([&]() { account.withdraw(100); });
std::thread t2([&]() { account.withdraw(200); });  // Race condition without mutex!
```

#### With Coroutines - No Synchronization Needed
```cpp
// Multiple coroutines accessing shared data - SAFE without locks!
class BankAccount {
    int balance = 1000;
    // No mutex needed!
    
public:
    void withdraw(int amount) {
        // No lock needed - only one coroutine runs at a time
        if (balance >= amount) {
            balance -= amount;  // Safe! No race condition possible
        }
    }
};

// Two coroutines accessing same account
BankAccount account;
auto reactor = Reactor::GetReactor();
reactor->CreateRunCoroutine([&]() { 
    account.withdraw(100);  // Safe - runs to completion or yields
    Coroutine::CurrentCoroutine()->Yield();
});
reactor->CreateRunCoroutine([&]() { 
    account.withdraw(200);  // Safe - only runs when first yields
});
```

**Why is this safe?** Because coroutines in the same reactor:
- Never run simultaneously (no true parallelism)
- Only switch at explicit yield points (cooperative)
- Share the same thread context (no concurrent access)

This means you can write concurrent code without:
- Mutexes or locks
- Atomic operations
- Memory barriers
- Race conditions
- Deadlocks

The only time you need synchronization is when:
- Multiple threads each have their own reactors
- You're sharing data between different reactor threads (which you should avoid!)

### Key Difference: Stackful vs Stackless

RRR uses **stackful coroutines** (via Boost.Coroutine2):
- Each coroutine has its own stack
- Can pause/resume from any function depth
- Natural code flow (looks like synchronous code)

```cpp
// Stackful - can pause anywhere in the call stack
void deep_function() {
    Coroutine::CurrentCoroutine()->Yield();  // Can pause here!
}

reactor->CreateRunCoroutine([]() {
    deep_function();  // Pause/resume works through function calls
});
```

## The Reactor Pattern

The Reactor is the **event loop** that manages all coroutines in a thread. Think of it as a scheduler that:
1. Runs coroutines until they yield
2. Checks for ready events
3. Resumes coroutines waiting on ready events
4. Repeats

### The Reactor Lifecycle

```cpp
// 1. Create reactor (one per thread)
auto reactor = Reactor::GetReactor();

// 2. Create coroutines
reactor->CreateRunCoroutine([]() {
    // Your concurrent task
});

// 3. Run event loop
reactor->Loop(false);  // Process once
reactor->Loop(true);   // Run forever
```

### Thread-Local Design

**Critical**: Each reactor is bound to its thread. Never access a reactor or its events from another thread!

```cpp
// WRONG - Undefined behavior!
std::thread t([reactor]() {
    reactor->CreateRunCoroutine([]() { /* ... */ });  // BAD!
});

// RIGHT - Each thread has its own reactor
std::thread t([]() {
    auto reactor = Reactor::GetReactor();  // Thread-local reactor
    reactor->CreateRunCoroutine([]() { /* ... */ });
});
```

## Using RRR Coroutines

### Basic Coroutine Creation

```cpp
auto reactor = Reactor::GetReactor();

// Simple coroutine - runs to completion
reactor->CreateRunCoroutine([]() {
    std::cout << "Hello from coroutine!" << std::endl;
});

// Coroutine with yielding
auto coro = reactor->CreateRunCoroutine([]() {
    std::cout << "Step 1" << std::endl;
    Coroutine::CurrentCoroutine()->Yield();  // Pause here
    std::cout << "Step 2" << std::endl;
    Coroutine::CurrentCoroutine()->Yield();  // Pause again
    std::cout << "Step 3" << std::endl;
});

// Continue a paused coroutine
reactor->ContinueCoro(coro);  // Resumes at Step 2
reactor->ContinueCoro(coro);  // Resumes at Step 3
```

### Practical Example: Concurrent I/O

```cpp
// Handle multiple client requests concurrently
void handle_clients(Reactor* reactor) {
    for (int client_id = 0; client_id < 1000; client_id++) {
        reactor->CreateRunCoroutine([client_id, reactor]() {
            // Each client handled by its own coroutine
            auto request_event = Reactor::CreateSpEvent<IntEvent>();
            
            // Simulate waiting for client request
            request_event->Wait(5000000);  // 5 second timeout
            
            if (request_event->status_ == Event::TIMEOUT) {
                std::cout << "Client " << client_id << " timed out" << std::endl;
                return;
            }
            
            // Process request
            std::cout << "Processing client " << client_id << std::endl;
            
            // Yield while processing
            Coroutine::CurrentCoroutine()->Yield();
            
            std::cout << "Client " << client_id << " done" << std::endl;
        });
    }
    
    // Run event loop to process all coroutines
    reactor->Loop(true);
}
```

## The Event System

Events are the synchronization primitives for coroutines. They allow coroutines to:
- Wait for conditions without blocking
- Coordinate with other coroutines
- Implement timeouts

### Event Types

#### IntEvent - Integer-based condition
```cpp
auto event = Reactor::CreateSpEvent<IntEvent>();
event->target_ = 42;  // Will be ready when value_ == 42

reactor->CreateRunCoroutine([event]() {
    // Another coroutine sets the value
    event->Set(42);
});

reactor->CreateRunCoroutine([event]() {
    event->Wait();  // Waits until value_ == target_
    std::cout << "Got value: " << event->value_ << std::endl;
});
```

#### TimeoutEvent - Time-based trigger
```cpp
auto timeout = Reactor::CreateSpEvent<TimeoutEvent>(1000000);  // 1 second

reactor->CreateRunCoroutine([timeout]() {
    timeout->Wait();
    std::cout << "1 second elapsed!" << std::endl;
});
```

#### OrEvent - Any of multiple events
```cpp
auto event1 = Reactor::CreateSpEvent<IntEvent>();
auto event2 = Reactor::CreateSpEvent<IntEvent>();
auto or_event = Reactor::CreateSpEvent<OrEvent>(event1, event2);

reactor->CreateRunCoroutine([or_event]() {
    or_event->Wait();  // Continues when ANY event is ready
    std::cout << "One of the events triggered!" << std::endl;
});
```

#### AndEvent - All events must be ready
```cpp
auto event1 = Reactor::CreateSpEvent<IntEvent>();
auto event2 = Reactor::CreateSpEvent<IntEvent>();
auto and_event = Reactor::CreateSpEvent<AndEvent>(event1, event2);

reactor->CreateRunCoroutine([and_event]() {
    and_event->Wait();  // Continues when ALL events are ready
    std::cout << "Both events triggered!" << std::endl;
});
```

### Event with Timeout

```cpp
reactor->CreateRunCoroutine([reactor]() {
    auto event = Reactor::CreateSpEvent<IntEvent>();

    // Wait with timeout (microseconds)
    event->Wait(1000000);  // 1 second timeout

    if (event->status_ == Event::TIMEOUT) {
        std::cout << "Operation timed out" << std::endl;
    } else if (event->status_ == Event::DONE) {
        std::cout << "Operation completed" << std::endl;
    }
});
```

### The Scheduler: Why Coroutines Need Resume

A key concept to understand is that **coroutines require explicit resumption** to continue execution. When a coroutine yields (via `Yield()` or `Wait()`), it doesn't magically wake up on its own - something must call `Resume()` on it.

This is where the **scheduler** comes in. The reactor's event loop acts as a scheduler that:
1. Checks which events are ready (satisfied or timed out)
2. Calls `Resume()` on coroutines waiting for those events
3. Runs resumed coroutines until they yield again

Without a running event loop, coroutines will yield and never wake up!

#### Starting the Scheduler

One common pattern is to start a dedicated thread that runs the reactor loop forever:

```cpp
// Create a thread dedicated to running coroutines
std::thread scheduler_thread([]() {
    auto reactor = Reactor::GetReactor();

    // Create some initial coroutines
    reactor->CreateRunCoroutine([]() {
        std::cout << "Initial coroutine running" << std::endl;
        // ... do work, launching new coroutines, wait on events, etc.
    });

    // continuously checks events and resumes coroutines
    while (true) {
        sleep(1)
        reactor->check_resume();
        // the actual api in the rrr reactor is Loop(false), it does the check once
    }
    // the rrr api has Loop(true), equivalent to the above loop. 
    // reactor->Loop(true);  // true = run forever
});
```

The loop continuously:
- Checks timeout conditions
- Resumes coroutines whose events are satisfied

#### How It Works in RRR: PollThread

In the actual RRR codebase, this scheduler is triggered in `PollThread`. Each `PollThread` owns a reactor and runs the event loop in its thread:

```cpp
// Simplified view of how PollThread works
class PollThread {
    Reactor* reactor_;

    void Run() {
        // This is where the scheduler runs!
        // It processes events and resumes coroutines
        while (running_) {
            reactor_->Loop(false);  // Process pending events
            // ... poll for I/O, handle timeouts ...
        }
    }
};
```

When you use the RPC framework, `PollMgr` creates `PollThread` instances that handle coroutine scheduling automatically. You don't need to manually call `Loop()` - the poll threads do it for you.

#### Key Takeaway

Always remember: **a coroutine that yields is dead until something resumes it**. That "something" is the reactor's event loop running in a thread. Whether you call `reactor->Loop()` directly or let `PollThread` handle it, there must be an active scheduler checking events and resuming coroutines.

## Common Patterns and Best Practices

### 1. Producer-Consumer Pattern

```cpp
void producer_consumer_example() {
    auto reactor = Reactor::GetReactor();
    auto queue_event = Reactor::CreateSpEvent<IntEvent>();
    
    // Producer coroutine
    reactor->CreateRunCoroutine([queue_event]() {
        for (int i = 0; i < 10; i++) {
            std::cout << "Producing: " << i << std::endl;
            queue_event->Set(1);  // Signal item available
            Coroutine::CurrentCoroutine()->Yield();
        }
    });
    
    // Consumer coroutine
    reactor->CreateRunCoroutine([queue_event]() {
        for (int i = 0; i < 10; i++) {
            queue_event->Wait();  // Wait for item
            std::cout << "Consuming item" << std::endl;
            queue_event->value_ = 0;  // Reset for next wait
            queue_event->status_ = Event::INIT;
        }
    });
    
    reactor->Loop(false);
}
```

### 2. Chain of Operations

```cpp
void operation_chain() {
    auto reactor = Reactor::GetReactor();
    
    auto step1_done = Reactor::CreateSpEvent<IntEvent>();
    auto step2_done = Reactor::CreateSpEvent<IntEvent>();
    auto step3_done = Reactor::CreateSpEvent<IntEvent>();
    
    // Step 1: Fetch data
    reactor->CreateRunCoroutine([step1_done]() {
        std::cout << "Fetching data..." << std::endl;
        // Simulate async operation
        step1_done->Set(1);
    });
    
    // Step 2: Process data
    reactor->CreateRunCoroutine([step1_done, step2_done]() {
        step1_done->Wait();
        std::cout << "Processing data..." << std::endl;
        step2_done->Set(1);
    });
    
    // Step 3: Save results
    reactor->CreateRunCoroutine([step2_done, step3_done]() {
        step2_done->Wait();
        std::cout << "Saving results..." << std::endl;
        step3_done->Set(1);
    });
    
    // With our Loop() fix, this processes the entire chain!
    reactor->Loop(false);
}
```

### 3. Timeout Handling

```cpp
void robust_operation() {
    auto reactor = Reactor::GetReactor();
    
    reactor->CreateRunCoroutine([reactor]() {
        auto response = Reactor::CreateSpEvent<IntEvent>();
        
        // Start async operation
        reactor->CreateRunCoroutine([response]() {
            // Simulate slow operation
            Coroutine::CurrentCoroutine()->Yield();
            Coroutine::CurrentCoroutine()->Yield();
            response->Set(1);
        });
        
        // Wait with timeout
        response->Wait(100000);  // 100ms timeout
        
        switch (response->status_) {
            case Event::DONE:
                std::cout << "Operation succeeded" << std::endl;
                break;
            case Event::TIMEOUT:
                std::cout << "Operation timed out - taking fallback action" << std::endl;
                break;
            default:
                std::cout << "Unexpected status" << std::endl;
        }
    });
    
    reactor->Loop(false);
}
```

## Pitfalls to Avoid

### 1. Cross-Thread Event Access (NEVER DO THIS!)
```cpp
// WRONG - Causes undefined behavior
auto event = Reactor::CreateSpEvent<IntEvent>();
std::thread t([event]() {
    event->Set(1);  // BAD! Event belongs to different thread's reactor
});
```

### 2. Reusing Events After Completion
```cpp
// WRONG - Events can't be reused
auto event = Reactor::CreateSpEvent<IntEvent>();
event->Wait();  // Event becomes DONE or TIMEOUT
event->Wait();  // BAD! Undefined behavior
```

### 3. Multiple Waiters on Same Event
```cpp
// WRONG - Not supported
auto event = Reactor::CreateSpEvent<IntEvent>();
reactor->CreateRunCoroutine([event]() { event->Wait(); });
reactor->CreateRunCoroutine([event]() { event->Wait(); });  // BAD!
```

### 4. Forgetting to Process Events
```cpp
// WRONG - Coroutines created but never run
reactor->CreateRunCoroutine([]() { /* ... */ });
// Forgot reactor->Loop() - coroutine never executes!
```

### 5. Infinite Loops Without Yielding
```cpp
// WRONG - Blocks all other coroutines
reactor->CreateRunCoroutine([]() {
    while (true) {
        // Busy work without yielding
    }  // BAD! Never yields control
});

// RIGHT - Cooperative multitasking
reactor->CreateRunCoroutine([]() {
    while (true) {
        // Do work
        Coroutine::CurrentCoroutine()->Yield();  // Let others run
    }
});
```

## Performance Considerations

1. **Coroutine Creation**: Very cheap (~100 bytes + stack size)
2. **Context Switch**: ~10-100 nanoseconds (vs microseconds for threads)
3. **Memory Usage**: Can run 10,000+ coroutines in the memory of 10 threads
4. **Scalability**: Single reactor can handle thousands of concurrent operations

## Summary

RRR's coroutine system provides:
- **Lightweight concurrency**: Thousands of concurrent tasks in one thread
- **Simple async code**: Write async logic that looks synchronous
- **Efficient I/O handling**: Perfect for network servers with many connections
- **Predictable behavior**: No race conditions within the reactor thread

Remember the golden rules:
1. One reactor per thread
2. Never access events/coroutines across threads
3. Always yield in long-running operations
4. Events are single-use only
5. Only one coroutine can wait on an event

By following these principles, you can build highly concurrent, efficient applications that handle thousands of operations with minimal resource usage.
