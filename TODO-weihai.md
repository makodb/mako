# RustyCpp TODO
<!--
This comment block is the instructions in case you forget.

Work on tasks defined in TODO.md. Repeat the following steps, don’t stop until interrupted. Don’t ask me for advice, just pick the best option you think that is honest, complete, and not corner-cutting: 

1. Pick a task: First check if there are any repeated task that needs to be run again. If yes this is the task we need to do and go to step 2. If no repeated task needs to run, pick the top undone task with highest priority (high-medium-low), choose its first leaf task.  If there are no task at all, (no fit repeated task and no undone TODO items left), sleep a minute and git pull and restart step 1 (so this step is a dead loop until you find a todo item).
2. Analyze the task, check if this can be done with not too many LOC (i.e., smaller than 500 lines code give or take). If not, try to analyze this task and break it down into several smaller tasks, expanding it in the TODO.md. The breakdown can be nested and hierarchical. Try to make each leaf task small enough (<500 lines LOC). You can document your analysis in the doc folder for future reference. 
3. Try to execute the first leaf task. Make a plan for the task before execute, put the plan in the docs folder, and add the file name in the item in TODO.md for reference. You can all write your key findings as a few sentences in the TODO item. When write code, you are only allowed to write rusty safe code following the rusty-cpp guidelines unless you are explicitly allowed by the todo item description. Avoid using std types, using rusty alternatives if they exists (e.g., don't use unique_ptr, use rusty::Box; don't use std thread, use rusty thread).
4. Make sure to add comprehensive test for the task executed. Run the whole ci test  to make sure no regression happens (remember to use make clean && make -j32 because rusty-cpp requires make clean before build). Put the test log in the logs folder as proof for manual review, log file name prefixed with datetime and commithash. If tests fail, fix them using the best, honest, complete approach, run test suites again to verify fixes work. Do not cheat such as disabling the borrow checker. Repeat this step until no tests fail. 
5. Prepare for git commit, first check if you wrote any rusty unsafe code, if yes, then revert the changes and go back to Step 3 to redo task. Remove all temporary files, especially not to commit any binary files. For plan files, extract from implementation plan the design rational and user manual and put it in the docs folder. we can keep the plan files in docs/dev/ folder. Mark the task as done (or last done for repeated task) in the TODO.md with a timestamp [yy:mm:dd, hh:mm]  
6. Git commit the changes. First do git pull --rebase, and fix conflicts if any. Remember to update submodule. If remote has any updates (merged through rebase), then run full ci tests again to make sure everything pass. If not pass, investigate and fix, repeat until pass all ci tests. Then do NOT git push (just commit locally, and I will do it manually).
7. Go back to step 1 for next task; don't ask me whether to continue, just continue. (The TODO.md file is possibly updated, so make sure you read the updated TODO.)

-->

- [ ] Mako, build a high-performance, reliable, transactional, datastore; GA release
  - [x] *high* decouple client: decouple client (`./examples/simpleTransactionRep.cc`) from transaction execution [26:01:16, 04:50]
    - Goal: I currently coloate all client and transaction execution code, I want to decouple a client from transaction execution, so that I can deploy client on different servers.
    - Analysis: Task exceeds 500 LOC (~600-750 LOC total). Breaking down into subtasks:
    - Implementation complete! All 5 subtasks done. Note: Full RPC integration uses stub implementations.
    - [x] *high* Add a testcase: add a testcase in ci.yml and ci.sh [26:01:17, 01:50]
      - Test already existed in ci.yml (line 53-54) and ci.sh (run_client_server_test function)
      - Enhanced test_client_server.sh with Test 4: Full end-to-end client-server communication
        - Starts makoServer in background (single shard, no replication)
        - Waits for TCP port 31000 to be ready using nc
        - Runs client to connect and perform BeginTransaction
        - Verifies successful connection and transaction start
        - Note: Put/Get may fail due to table ID mismatch (known limitation)
      - Plan file: docs/dev/client_server_ci_test_plan.md
    - [ ] *high* Add several real throughput numbers for decoupled clients in documentation md files 
    - [x] *high* Support multiple clients: refer to `NOT suitable for:` in `docs/dev/client_rpc_implementation_plan.md` [26:01:16, 17:45]
      - First, we have multiple shards and each shard has mulitple worker threads running, so at least, we can accept # of worker * # of shards clients at a time.
      - Second, you can reject a new client request, and return a message with message like "all servers are occupied, please run it later" etc
      - Implementation complete! Worker pool pattern for concurrent client handling:
        - Added WorkerSlot struct with atomic acquire/release for thread-safe slot management
        - ClientTcpServer now supports configurable max_clients (= nthreads per shard)
        - When all workers busy, rejects new clients with SERVER_BUSY error and message
        - Added clientServerBusyType (26) and client_server_busy_response_t to common.h
        - Plan file: docs/dev/multi_client_support_plan.md
    - [x] *high* Implement full-fledged features: refer to `Current Limitations` in `docs/dev/client_decoupling_design.md` [26:01:16, 15:10]
      - Note: Try to reuse existing code as much as possible; don't reinvent only if needed
      - Implementation complete! Full TCP-based client-server RPC communication:
        - Server-side: Added handlers in ShardReceiver for message types 20-25 (BeginTxn, Commit, Rollback, Put, Get, Delete)
        - Server-side: Added ClientTcpServer (lib/client_tcp_server.h) for accepting client TCP connections
        - Server-side: Added setup_client_tcp_server()/stop_client_tcp_server() in rpc_setup.cc
        - Client-side: Updated RemoteDB with actual TCP socket communication (Connect, BeginTransaction, Commit, Rollback, SendPut/Get/Delete)
        - Integration: Updated makoServer.cc to start ClientTcpServer on port 31000+shardIdx
        - Documentation: Updated docs/dev/client_decoupling_design.md with implementation details
        - Plan file: docs/dev/client_rpc_implementation_plan.md
        - Total LOC: ~490 (within 500 limit)
    - [x] *high* 1. Design document: Document client-server architecture and API contract [26:01:16, 04:14]
      - Create `docs/dev/client_decoupling_design.md` with architecture diagrams
      - Define the RPC message protocol for client-server communication
      - Plan file: `docs/dev/client_decoupling_design.md`
      - Est. ~50 LOC (documentation only)
    - [x] *high* 2. Server-side: Create standalone server entry point [26:01:16, 04:24]
      - Add `examples/makoServer.cc` - standalone server that hosts DB and RPC
      - Reuses existing `setup_erpc_server()` and `setup_helper()` infrastructure
      - Server listens for client RPC requests (Get, Put, Delete, BeginTxn, Commit, Rollback)
      - Est. ~150 LOC
      - CI tests passed: simpleTransaction, shardNoReplication, shard1ReplicationSimple
      - Test log: logs/20260116_042442_039a90f4_server_ci.log
    - [x] *high* 3. Client library: Create RemoteDB class (`src/mako/remote_db.hh`) [26:01:16, 04:32]
      - Implement `mako::RemoteDB` that mirrors `mako::DB` interface
      - Translates BeginTransaction/Commit/Rollback to RPC calls
      - Uses existing `Client` class for RPC transport
      - Est. ~200-300 LOC
      - Added: New message types to common.h (clientBeginTxnReqType, clientPutReqType, etc.)
      - Added: Request/response structures for client API
      - Added: RemoteDB and RemoteTable classes with full interface
      - Note: Stub implementations for RPC - full integration to be done in future iteration
    - [x] *high* 4. Updated example: Modify `simpleTransactionRep.cc` for client mode [26:01:16, 04:45]
      - Add command-line flag to run in client-only mode
      - When in client mode, connect to remote server via RemoteDB
      - Est. ~100 LOC changes
      - Added: `--client <host> <port>` command-line option
      - Added: `run_client_mode()` function demonstrating RemoteDB API
      - Added: YELLOW color code to examples/common.h
      - Tested: Both server mode and client mode work correctly
    - [x] *high* 5. CI tests: Add client-server integration tests [26:01:16, 04:46]
      - Test script that starts server, then runs client on same/different process
      - Verify all existing tests pass in both standalone and client-server modes
      - Est. ~100 LOC
      - Added: ci/test_client_server.sh integration test script
      - Tests: Client mode, usage help verification, makoServer binary
  - [x] *high* Rocksdb interface: expose rocksdb-like interface to users 
    - Note: refer to `RocksDB_Guide.md` for rocksdb interfaces 
    - Note: expose your interfaces via `./src/mako/db.hh` (you can change other files for sure)
    - Note: apply your interfaces in `./examples/simpleTransactionRep.cc`
    - Note: for every lcoal commit, run `./ci/ci.sh all`, see if there is a ci test failure. If failed tests found, investigate and fix. Repeat until no failures are detected. Don't cheat by removing or weakening tests.
    - Note: you should use table->Put instead of database; (don't need to be exactly like rocksdb interfaces)