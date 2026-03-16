You are testing **Mako**, a high-performance distributed transactional key-value store (https://github.com/makodb/mako), on a **single-node setup**. Mako supports serializable transactions, a key-value API, RocksDB persistence, and a Redis-compatible layer. Your goal is to systematically verify correctness of Mako's core operations and transactional guarantees through both scripted unit tests and realistic application-level workloads.

CRITICAL INSTRUCTIONS:
- You must execute the tasks listed below exactly as written, in order. Do not skip, reorder, or substitute tasks.
- Do NOT pivot to validating existing bug fixes, running the project's built-in test suites (ctest, simpleTransaction, continuousTransactions, TPC-C, ci.sh), or any other work not explicitly listed here. Those are not your job. Your job is to write and run NEW black-box tests as an external user of Mako's API.
- Do NOT treat passing existing CI tests as a substitute for the tasks below. The existing test suites test internal correctness. You are testing external user-facing behavior.
- If you discover bugs, document them in the report but do not attempt to fix Mako's source code. You are a tester, not a developer on this project.
- Each task must produce a new, self-contained test script written by you. Do not reuse or wrap existing test binaries.

Don't ask me for advice, just pick the best option you think that is honest, complete, and not corner-cutting. Once all tasks are complete and the final CORRECTNESS_REPORT.md is saved, do one final iteration to confirm everything is done, print the full contents of CORRECTNESS_REPORT.md to the terminal so it is visible in the APAS dashboard, and then STOP. Do not continue looping after that final confirmation iteration. For each task below, follow this workflow:
1. Do a `git fetch origin` and compare local HEAD against `origin/mako-dev`. If behind, do `git pull` to update. If the pull fails due to local changes, report the status and stop. Log the current commit hash so the report can reference exactly which version of Mako was tested.
2. Pick the top undone task. Analyze it. If it requires more than ~500 lines of code, break it down into smaller leaf tasks and expand them inline in this file. Document your analysis in the `docs/` folder for future reference.
3. Before executing, make a plan for the task. Document key findings either as a few sentences in the TODO item itself or in the `docs/` folder for longer details.
4. Execute the task. Write each test as a self-contained script that prints a clear test name and description before running, prints PASS or FAIL with a brief reason at the end, exits with code 0 on pass and non-zero on fail, and is idempotent (cleans up after itself or uses unique key prefixes per run).
5. After writing each test, run all previously written correctness test scripts (the ones you created, not the project's built-in tests) to make sure no regression happens. If tests fail, fix your test scripts, run again, and repeat until all pass.
6. Clean up any temporary files. Do NOT git commit or git push. Save all test scripts and outputs locally in the project directory.

Before writing any test, explore the codebase to understand:
- How the Mako server is started and configured (check `config/`, `scripts/`, `bin/`, and the `Makefile`)
- What client interfaces are available (check `extern_interface/`, `pylib/`, `examples/`, and `src/`)
- Whether there is a Python client, C++ client, Redis-compatible CLI, or RPC-based client you can use
- The exact API for beginning a transaction, reading a key, writing a key, committing, and aborting
- How to start and stop a single-node Mako server instance for testing

Use this exploration ONLY to understand how to interact with Mako as a user. Do not get sidetracked into analyzing or fixing Mako internals.

Adapt all tests to use whatever client interface is actually available. If a Python client (`pylib/`) exists, prefer it for rapid test development. If only a C++ client or RPC interface exists, use that. If a Redis-compatible layer is available, write a parallel set of tests using `redis-cli` or a Redis client library.

Task 1.1: Single Key Put and Get
- Insert a key-value pair (e.g., key="test_key_1", value="hello_mako")
- Immediately read the same key back
- Assert the returned value matches exactly what was written
- Expected: exact match, no data corruption

Task 1.2: Key Overwrite
- Insert key="overwrite_key" with value="version_1"
- Read it back and verify value="version_1"
- Overwrite the same key with value="version_2"
- Read it back and verify value="version_2"
- Expected: latest write wins, old value is fully replaced

Task 1.3: Delete Key
- Insert key="delete_key" with value="to_be_deleted"
- Read it back and verify it exists
- Delete the key
- Attempt to read the deleted key
- Expected: read after delete should return an error, empty result, or a "not found" indicator (document which behavior Mako exhibits)

Task 1.4: Read Non-Existent Key
- Attempt to read a key that was never written (e.g., key="nonexistent_key_xyz")
- Expected: should return a clear "not found" error or empty result, not a crash or hang

Task 1.5: Large Value Handling
- Write a key with a 1KB value, read it back, verify correctness
- Write a key with a 100KB value, read it back, verify correctness
- Write a key with a 1MB value, read it back, verify correctness
- Write a key with a 10MB value (if supported), read it back, verify correctness
- Expected: all values should round-trip correctly. Document any size limits or failures.

Task 1.6: Key Boundary Conditions
- Write and read back a key that is a single character (e.g., key="a")
- Write and read back a key that is very long (e.g., 1024 characters, 4096 characters)
- Write and read back a key with special characters: spaces, unicode, null bytes, slashes, dots
- Write and read back an empty string as a value (key="empty_val", value="")
- Expected: document which key formats are accepted and which are rejected. No silent data corruption.

Task 1.7: Batch/Bulk Write and Read
- Write 1000 unique key-value pairs in a loop
- Read all 1000 keys back and verify each value matches
- Expected: 100% match rate, no missing or corrupted entries

Task 2.1: Transaction Commit (Multi-Key)
- Begin a transaction
- Within the transaction, write key="txn_a"="val_a", key="txn_b"="val_b", key="txn_c"="val_c"
- Commit the transaction
- Outside the transaction, read all three keys
- Expected: all three keys should exist with correct values. All-or-nothing semantics on commit.

Task 2.2: Transaction Abort/Rollback (Multi-Key)
- Begin a transaction
- Within the transaction, write key="abort_a"="val_a", key="abort_b"="val_b", key="abort_c"="val_c"
- Abort/rollback the transaction (do NOT commit)
- Outside the transaction, attempt to read all three keys
- Expected: none of the three keys should exist. Abort must discard all writes.

Task 2.3: Partial Failure Within Transaction
- Begin a transaction
- Write key="partial_1"="good_value"
- Attempt an operation likely to fail (e.g., write to an invalid key format if one exists, or trigger an error condition)
- Attempt to commit
- Read key="partial_1" outside the transaction
- Expected: if the transaction was aborted due to the failure, "partial_1" should not exist. Document whether Mako auto-aborts on error or allows partial commits.

Task 2.4: Transaction Overwriting Pre-Existing Keys
- Outside any transaction, write key="pre_existing"="original_value"
- Begin a transaction
- Within the transaction, overwrite key="pre_existing"="txn_updated_value"
- Commit the transaction
- Read key="pre_existing"
- Expected: value should be "txn_updated_value"

Task 2.5: Transaction Abort Does Not Affect Pre-Existing Data
- Outside any transaction, write key="safe_key"="safe_value"
- Begin a transaction
- Within the transaction, overwrite key="safe_key"="dangerous_value"
- Abort the transaction
- Read key="safe_key"
- Expected: value should still be "safe_value". Abort must restore the original state.

Task 2.6: Empty Transaction
- Begin a transaction
- Immediately commit without performing any operations
- Expected: no error, no side effects. The system should handle this gracefully.

Task 2.7: Large Transaction
- Begin a transaction
- Write 500 unique key-value pairs within the transaction
- Commit the transaction
- Read all 500 keys outside the transaction
- Expected: all 500 keys exist with correct values. Tests atomicity at scale.

Task 3.1: Read Uncommitted Data (Dirty Read Test)
- Begin transaction T1
- In T1, write key="isolation_key"="t1_value"
- Do NOT commit T1 yet
- From a separate client/session (or after T1 but before commit), read key="isolation_key"
- Expected: if Mako provides serializable isolation, the read should NOT see "t1_value" until T1 commits. Document the actual behavior.

Task 3.2: Read-After-Commit Visibility
- Begin transaction T1
- In T1, write key="visibility_key"="committed_value"
- Commit T1
- From a separate client/session, read key="visibility_key"
- Expected: value should be "committed_value". Committed data must be visible to subsequent reads.

Task 3.3: Concurrent Write Conflict
- From two separate clients/sessions simultaneously (or as close to simultaneous as possible):
  - Client A: begin txn, write key="conflict_key"="value_A", commit
  - Client B: begin txn, write key="conflict_key"="value_B", commit
- Read key="conflict_key"
- Expected: the final value should be either "value_A" or "value_B" (not a mix, not corrupted). One transaction should win. Document whether the other transaction is aborted or retried.

Task 3.4: Read-Write Conflict
- Write key="rw_conflict"="initial_value"
- Client A: begin txn, read key="rw_conflict", then write key="rw_conflict"="updated_by_A", commit
- Client B (concurrently): begin txn, read key="rw_conflict", then write key="rw_conflict"="updated_by_B", commit
- Expected: under serializable isolation, one transaction should be aborted or serialized. The final value should reflect a consistent serial ordering. Document the exact behavior.

Task 3.5: Write Skew Detection
- Write key="account_1"="50", key="account_2"="50" (representing two balances summing to 100)
- Client A: begin txn, read account_1 (50), read account_2 (50), verify sum >= 100, write account_1="0", commit
- Client B (concurrently): begin txn, read account_1 (50), read account_2 (50), verify sum >= 100, write account_2="0", commit
- Expected: under serializable isolation, at most one transaction should succeed. If both commit, the invariant (sum >= 100) is violated, indicating a write skew anomaly. Document whether Mako prevents this.

Task 4.1: Survive Clean Restart
- Write 100 key-value pairs
- Verify all 100 are readable
- Gracefully stop the Mako server process (SIGTERM or clean shutdown command)
- Restart the Mako server
- Read all 100 keys
- Expected: all 100 keys should still exist with correct values

Task 4.2: Survive Crash (SIGKILL)
- Write 100 key-value pairs and ensure they are committed
- Kill the Mako server process with SIGKILL (simulating a crash)
- Restart the Mako server
- Read all 100 keys
- Expected: all committed data should survive. Document any data loss.

Task 4.3: Uncommitted Data Does Not Survive Restart
- Begin a transaction, write key="uncommitted_durability"="should_not_persist"
- Do NOT commit
- Kill the Mako server with SIGKILL
- Restart the Mako server
- Read key="uncommitted_durability"
- Expected: key should not exist. Uncommitted data must not be persisted.

Task 5.1: Rapid Repeated Operations on Same Key
- In a tight loop (1000 iterations), write and immediately read the same key with incrementing values
- Verify each read returns the value just written
- Expected: no stale reads, no lost writes, no corruption

Task 5.2: Concurrent Readers During Writes
- Spawn a writer that continuously updates key="hot_key" with incrementing values
- Spawn 5 readers that continuously read key="hot_key"
- Each reader should verify the value is a valid integer (not corrupted or partial)
- Run for 10 seconds
- Expected: no torn reads, no corrupted values. Every read returns a valid value that was written at some point.

Task 5.3: Transaction Timeout/Stale Transaction
- Begin a transaction
- Wait for a long period (e.g., 60 seconds or whatever exceeds Mako's transaction timeout if one exists)
- Attempt to commit the transaction
- Expected: document whether Mako has a transaction timeout and what happens (auto-abort, error on commit, or indefinite hold)

Task 5.4: Double Commit / Double Abort
- Begin a transaction, write a key, commit
- Attempt to commit again on the same transaction handle
- Expected: second commit should error gracefully, not corrupt data or crash
- Begin a transaction, abort
- Attempt to abort again
- Expected: second abort should error gracefully or be a no-op

Task 5.5: Operations After Transaction End
- Begin a transaction, commit it
- Attempt to write a key using the committed transaction handle
- Expected: should error, not silently succeed

Task 6.1: Bank Simulation (Invariant Preservation Under Concurrency)
- Create 100 accounts with an initial balance of 1000 each (total system balance = 100,000)
- Spawn 10 concurrent clients, each running in a loop for 60 seconds
- Each client picks two random accounts per iteration, begins a transaction, reads both balances, transfers a random amount (1-100) from one to the other (only if sufficient funds), and commits
- A separate auditor client runs concurrently: every 2 seconds it begins a read-only transaction, reads all 100 accounts, sums the balances, and verifies the total is exactly 100,000
- After 60 seconds, stop all clients, do a final audit
- Expected: total balance must be exactly 100,000 at every audit point. No money created or destroyed. All individual balances must be >= 0. Log every audit result and any failed transfers (aborts are acceptable, lost money is not).
- Report: total transactions attempted, committed, aborted, abort rate, all audit results, any invariant violations

Task 6.2: Session Store Simulation (Web Application Workload)
- Simulate a web application using Mako as a session store
- Create 500 user sessions, each as a key with a JSON-like value containing: session_id, user_id, login_time, last_active, cart_items (list of item IDs), page_views (integer counter)
- Spawn 20 concurrent clients simulating user activity for 60 seconds. Each client randomly performs one of these operations per iteration:
  - Login: create a new session key with initial values
  - Browse: read a session, increment page_views, update last_active, write back (within a transaction)
  - Add to cart: read a session, append an item to cart_items, write back (within a transaction)
  - Checkout: read a session, verify cart is non-empty, clear cart_items, write back (within a transaction)
  - Logout: delete a session key
- After 60 seconds, read all surviving sessions and verify:
  - Every session has valid, parseable data (no corruption or partial writes)
  - page_views is a non-negative integer
  - cart_items is a valid list
  - No session contains data from a different session (cross-contamination check)
- Report: total operations per type, success/fail counts, any corrupted sessions, any cross-contamination

Task 6.3: Counter Service (High-Contention Single Key)
- Create a single counter key initialized to 0
- Spawn 10 concurrent clients, each incrementing the counter 1000 times using read-modify-write transactions (read current value, add 1, write back, commit)
- After all clients finish, read the counter
- Expected: final value must be exactly 10,000. Any other value indicates lost updates.
- Track: total commits, total aborts, retry counts per client, final counter value
- This is the hardest concurrency test: maximum contention on a single key. High abort rates are expected and acceptable. Lost updates are not.

Task 6.4: Message Queue Simulation (Producer-Consumer)
- Simulate a simple message queue backed by Mako
- Messages are stored as keys like "queue:{sequence_number}" with a value containing sender, timestamp, and payload
- A metadata key "queue:head" tracks the next sequence number to consume, "queue:tail" tracks the next sequence number to produce
- Spawn 5 producer clients, each producing 200 messages (1000 total). Each producer atomically reads queue:tail, writes the message at that sequence, increments queue:tail, and commits.
- Spawn 3 consumer clients running concurrently. Each consumer atomically reads queue:head, reads the message at that sequence, deletes it, increments queue:head, and commits.
- Run until all 1000 messages are consumed or 120 seconds elapse (whichever comes first)
- Expected: every message produced must be consumed exactly once. No duplicates, no missing messages. The final queue should be empty.
- Report: messages produced, messages consumed, duplicates detected, messages lost, time to drain, abort rates for producers and consumers

Task 6.5: Inventory Management (Multi-Key Transactions Under Load)
- Create 50 products, each with a key "product:{id}:stock" initialized to 100 and a key "product:{id}:reserved" initialized to 0
- Create an "orders" counter initialized to 0
- Spawn 15 concurrent clients simulating order placement for 60 seconds. Each client per iteration:
  - Picks 1-5 random products and random quantities (1-10 each)
  - Begins a transaction
  - For each product: reads stock and reserved, verifies (stock - reserved) >= requested quantity, increments reserved
  - If all products have sufficient unreserved stock: increments order counter, commits
  - If any product is insufficient: aborts the entire transaction (no partial reservation)
- Spawn 3 fulfillment clients running concurrently. Each picks a random product, begins a transaction, reads stock and reserved, if reserved > 0: decrements both stock and reserved by min(reserved, 10), commits.
- After 60 seconds, stop all clients and verify:
  - For every product: stock >= 0, reserved >= 0, reserved <= stock
  - Total orders placed matches the orders counter value
  - Sum of all reservations across products is consistent with order history
- Report: orders placed, orders failed (aborts), fulfillments processed, any invariant violations, final inventory state

Task 6.6: Sustained Load With Crash Recovery
- This test combines durability testing with real-world workload
- Start the bank simulation from Task 6.1 with 50 accounts and 5 clients
- Use 50 accounts with initial balance 1000 each, total = 50,000
- Let it run for 30 seconds, record the audit (total balance should be 50,000)
- After 30 seconds of sustained load, SIGKILL the Mako server (hard crash)
- Restart the Mako server
- Read all 50 accounts, sum the balances
- Expected: total balance must be exactly 50,000. Individual account balances may reflect any committed state, but the global invariant must hold. If uncommitted transactions were in-flight during the crash, they must have been fully rolled back.
- Run the bank simulation again for another 30 seconds after recovery to verify the system is still functional
- Do a final audit
- Report: pre-crash audit results, post-crash balance sum, post-recovery audit results, any invariant violations, any data corruption detected

After all tasks (1.1 through 6.6) are complete, update the `CORRECTNESS_REPORT.md` to include:
- The exact Mako commit hash that was tested
- Total tests run, passed, failed, skipped (covering both phases)
- For each failed test: description of expected vs actual behavior
- Real-world simulation results: throughput numbers, abort rates, invariant violations, corruption incidents
- Any anomalies, undocumented behaviors, or crashes observed
- API observations: which operations are supported, which error conditions were encountered, any missing or incomplete API surface
- A comparison: did the real-world tests surface any issues that the scripted unit tests (Tasks 1-5) missed?
- Recommendations for further testing based on findings

Save the report locally in the project directory. Do not git commit or push.