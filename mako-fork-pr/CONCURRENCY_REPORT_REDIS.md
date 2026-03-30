You are testing **Mako**, a high-performance distributed transactional key-value store (https://github.com/makodb/mako), on a **single-node setup**. You are testing through the **native C++ API** (`mbta_sharded_ordered_index`, `IDatabase`, or `simpleTransactionRep`). Your goal is to verify concurrency correctness at the native transaction layer: no lost updates, no inconsistent reads, no partial visibility of uncommitted writes, and proper serializable isolation under concurrent multi-threaded workloads.

CRITICAL INSTRUCTIONS:
- You must execute the tasks listed below exactly as written, in order. Do not skip, reorder, or substitute tasks.
- Do NOT pivot to validating existing bug fixes, running the project's built-in test suites (ctest, simpleTransaction, continuousTransactions, TPC-C, ci.sh), or any other work not explicitly listed here.
- If you discover bugs, document them in the report but do not attempt to fix Mako's source code. You are a tester, not a developer on this project.
- Each task must produce a new test written by you. Tests can be standalone C++ programs or additions to the existing test framework, whichever is more practical.
- Unlike makoCon's Redis layer, the native API supports full read-write transactions within a single atomic unit. Use this capability to test real serializable isolation.

Don't ask me for advice, just pick the best option you think that is honest, complete, and not corner-cutting. Once all tasks are complete and the final CONCURRENCY_REPORT_NATIVE.md is saved, do one final iteration to confirm everything is done, print the full contents of CONCURRENCY_REPORT_NATIVE.md to the terminal so it is visible in the APAS dashboard, and then STOP. Do not continue looping after that final confirmation iteration. For each task below, follow this workflow:
1. Pick the top undone task. Analyze it. If it requires more than ~500 lines of code, break it down into smaller leaf tasks and expand them inline in this file.
2. Before executing, explore the codebase to understand the native transaction API. Key areas to check:
   - `src/mako/db.hh` and `src/mako/idb.hh` for the IDatabase/ITable interfaces
   - `src/mako/benchmarks/mbta_wrapper.hh` for how transactions are started, committed, and aborted
   - `examples/simpleTransactionRep.cc` for working examples of native transaction usage
   - The `begin_txn()`, `Put()`, `Get()`, `commit_txn()`, `abort_txn()` API surface
   - How multi-threaded access works (thread-local transaction state, worker threads)
3. Execute the task. Write the test with clear output: test name, PASS/FAIL, metrics.
4. After writing each test, build and run all previously written concurrency tests to ensure no regression.
5. Clean up any temporary files. Do NOT git commit or git push. Save all test files and outputs locally in the project directory.

Task N1.1: Atomic Read-Modify-Write (Counter Increment)
- Initialize a key "counter" to value "0"
- Spawn 8 worker threads, each performing 1000 transactional increments:
  - begin_txn()
  - Get("counter") → parse as int
  - Put("counter", int + 1)
  - commit_txn()
  - If commit fails (OCC abort), retry the entire transaction
- After all threads finish, read the counter
- Expected: final value must be EXACTLY 8000. Any other value indicates lost updates in the native transaction layer. Unlike the Redis layer, the native API wraps read and write in one transaction, so OCC should prevent lost updates (aborted transactions are retried).
- Report: final counter value, total commits, total aborts, total retries, abort rate

Task N1.2: Two-Account Transfer (Invariant Preservation)
- Initialize account_a="5000", account_b="5000" (total = 10000)
- Spawn 8 worker threads, each performing 500 transfer transactions:
  - begin_txn()
  - Get(account_a) → parse as int
  - Get(account_b) → parse as int
  - If account_a >= transfer_amount: Put(account_a, a - amount), Put(account_b, b + amount)
  - commit_txn()
  - If commit fails, retry
- After all threads finish, read both accounts
- Expected: account_a + account_b must EXACTLY equal 10000. No money created or destroyed. Individual balances must be >= 0.
- Report: final balances, total sum, total commits, total aborts, abort rate, any invariant violations

Task N1.3: Write Skew Detection (Serializable Isolation Proof)
- Initialize account_1="50", account_2="50" (sum = 100)
- The invariant: account_1 + account_2 >= 0
- Spawn 2 threads simultaneously:
  - Thread A: begin_txn(), read both accounts, if sum >= 100 then Put(account_1, "0"), commit
  - Thread B: begin_txn(), read both accounts, if sum >= 100 then Put(account_2, "0"), commit
- Run this scenario 1000 times
- Expected: under serializable isolation, at most one thread should succeed per round. If both commit, the invariant is violated (sum = 0 < 100 for the original invariant, but both saw sum = 100 before committing). Count how many times both succeed vs one aborts.
- Report: rounds where both committed, rounds where one aborted, any invariant violations, abort patterns

Task N1.4: Phantom Read Detection
- Initialize keys "item_1" through "item_10" with values "active"
- Thread A runs a scan-like operation: begin_txn(), read all 10 keys, count how many are "active", commit
- Thread B concurrently: begin_txn(), Put("item_5", "deleted"), commit
- Run this 500 times
- Expected: Thread A should either see item_5 as "active" or "deleted", never a state where the count is inconsistent with the actual key values it read. Under serializable isolation, if Thread A reads item_5 as "active", the full count should reflect that, even if Thread B committed between Thread A's individual reads.
- Report: consistent reads, inconsistent reads, OCC aborts per thread, abort rate

Task N1.5: Concurrent Multi-Key Swap
- Initialize key_a="ALPHA", key_b="BETA"
- Spawn 2 threads running for 10 seconds:
  - Thread A: begin_txn(), read key_a and key_b, Put(key_a, old_b), Put(key_b, old_a), commit (swaps values)
  - Thread B: begin_txn(), read key_a and key_b, verify they are a valid pair: either ("ALPHA","BETA") or ("BETA","ALPHA"), commit
- Expected: Thread B should NEVER see ("ALPHA","ALPHA") or ("BETA","BETA") or any other invalid combination. Under serializable isolation, every read should see a consistent committed state.
- Report: total swaps committed, total verifications, any invalid pairs seen, abort rates for both threads

Task N1.6: High-Contention Hot Key with Transactions
- Initialize 1 hot key "hot" = "0" and 100 cold keys "cold_0" through "cold_99" = "0"
- Spawn 8 worker threads for 15 seconds. Each iteration:
  - begin_txn()
  - Get("hot"), increment, Put("hot", incremented)
  - Pick a random cold key, Get it, increment, Put it back
  - commit_txn()
  - On abort, retry
- After 15 seconds, read hot key and all cold keys
- Expected: hot key value = sum of all successful increments to hot. Each cold key value = number of times that specific cold key was incremented. Hot key should see much higher abort rates than cold keys.
- Report: hot key final value, cold key final values (sum), total commits, total aborts, abort rate, hot key contention vs cold key contention

Task N1.7: Long Transaction vs Short Transaction Starvation
- Initialize key "shared" = "0"
- Thread A (long transaction): begin_txn(), read "shared", sleep 100ms (simulate work), Put("shared", value+1), commit. Retry on abort. Run for 30 seconds.
- Spawn 4 Thread B instances (short transactions): begin_txn(), read "shared", Put("shared", value+1), commit immediately. Retry on abort. Run for 30 seconds.
- Expected: short transactions should commit much more frequently. The long transaction may experience repeated aborts (starvation). Quantify.
- Report: long txn commits, long txn aborts, short txn total commits, short txn total aborts, long txn starvation rate, average retries per long txn commit

Task N1.8: Read-Only Transaction Isolation
- Initialize keys "data_1" through "data_10" with sequential values "1" through "10"
- Spawn 4 writer threads that continuously update random keys with new values (within transactions)
- Spawn 4 reader threads that run read-only transactions: begin_txn(), read all 10 keys, verify internal consistency (e.g., all values are valid integers, no partial writes visible), commit
- Run for 15 seconds
- Expected: read-only transactions should NEVER see partial writes from a writer's uncommitted transaction. Every read-only snapshot should be internally consistent.
- Report: total read-only transactions, consistent reads, inconsistent reads (should be 0), writer commits, writer aborts

Task N1.9: Transaction Abort Correctness
- Initialize key_x="original_x", key_y="original_y"
- Spawn 8 threads, each performing 500 iterations:
  - begin_txn()
  - Put(key_x, "modified_by_thread_{id}")
  - Put(key_y, "modified_by_thread_{id}")
  - abort_txn() (intentionally abort)
  - Immediately read key_x and key_y (outside transaction)
  - Verify neither key contains "modified_by_thread_{id}"
- Expected: aborted transactions must NEVER leave any trace. Both keys should retain their pre-transaction values or values from other committed transactions.
- Report: total aborts performed, any cases where aborted data was visible (should be 0), final key values

Task N1.10: OCC Abort Rate vs Thread Count Scaling
- Initialize 10 shared keys
- Run the same workload at thread counts: 1, 2, 4, 8, 16
- Workload: each thread performs 500 transactions, each reading and writing 3 random keys from the 10 shared keys
- Measure: throughput (committed txns/sec), abort rate, total commits, total aborts
- Expected: abort rate should increase with thread count due to higher contention. Throughput may peak and then decline.
- Report: table of thread count vs throughput vs abort rate, identify the peak throughput point

After all tasks are complete, generate CONCURRENCY_REPORT_NATIVE.md that includes:
- The exact Mako commit hash tested
- Per-task results with all metrics requested above
- A comparison: which concurrency guarantees does the native API provide that the Redis layer does not? (Reference the Redis concurrency tests if available)
- Abort rate scaling analysis from N1.10
- Any bugs, crashes, or unexpected behaviors discovered
- Whether Mako's native API delivers true serializable isolation as claimed

Save the report locally in the project directory. Do not git commit or push.