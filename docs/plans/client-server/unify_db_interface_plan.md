# Plan: Unify DB and RemoteDB Interface

## Task Overview

This plan addresses two problems:
1. Merge 5 documentation files into 1-2 up-to-date docs
2. Unify db/remote_db interfaces so all tests can run uniformly without mode branching

## Problem Analysis

### Current State

**Documentation files:**
- `client_decoupling_design.md` - Main design doc (completed work)
- `client_rpc_implementation_plan.md` - RPC implementation details (completed work)
- `multi_client_support_plan.md` - Multi-client worker slots (planned, not implemented)
- `srpc_rpc_refactoring_plan.md` - SRPC framework migration (planned, not implemented)
- `avoid_duplication_client_server_plan.md` - Code consolidation (completed work)

**Interface differences:**
- `mako::DB`:
  - `Open()` static factory
  - `GetDB()` returns abstract_db*
  - `BeginTransaction()`, `Commit()`, `Rollback()`
  - `InitThread()` for thread initialization
  - Uses `mbta_sharded_ordered_index` for table access

- `mako::RemoteDB`:
  - `Connect()` static factory (requires host/port)
  - `GetTable()` returns RemoteTable*
  - `BeginTransaction()`, `Commit()`, `Rollback()`
  - No `InitThread()` needed (server handles thread context)
  - Uses `RemoteTable` for table access

**Current test code in simpleTransactionRep.cc:**
- Separate `run_client_mode()` function for RemoteDB
- Main test code uses `mako::DB` with `mbta_sharded_ordered_index`
- TransactionWorker uses `mako::DB*` pointer

## Solution Design

### Part 1: Documentation Consolidation

Merge into 2 documents:
1. `client_server_architecture.md` - Design doc covering current implementation
2. `client_server_roadmap.md` - Future plans (multi-client, SRPC migration)

### Part 2: Interface Unification

**Approach**: Create a common abstract interface that both DB and RemoteDB implement.

```cpp
// Abstract interface for database operations
class IDatabase {
public:
    virtual ~IDatabase() = default;

    // Transaction operations (already identical)
    virtual void* BeginTransaction() = 0;
    virtual void Commit(void* txn) = 0;
    virtual void Rollback(void* txn) = 0;

    // Connection interface (no-op for local DB)
    virtual Status Connect() { return Status::OK(); }  // Already connected for local
    virtual void Disconnect() {}                        // No-op for local
    virtual bool IsConnected() const { return true; }   // Always true for local

    // Thread initialization (no-op for remote)
    virtual void InitThread() {}

    // Table interface - abstract
    virtual ITable* GetTable(const std::string& name) = 0;
};

// Abstract interface for table operations
class ITable {
public:
    virtual ~ITable() = default;
    virtual Status Put(void* txn, const std::string& key, const std::string& value) = 0;
    virtual Status Get(void* txn, const std::string& key, std::string& value) = 0;
    virtual Status Delete(void* txn, const std::string& key) = 0;
    virtual const std::string& GetName() const = 0;
};
```

**Key Changes:**
1. Add `IDatabase` and `ITable` abstract interfaces
2. Make `DB` and `RemoteDB` both implement `IDatabase`
3. Create `LocalTable` wrapper around `mbta_sharded_ordered_index`
4. Modify `TransactionWorker` to use `IDatabase*` instead of `mako::DB*`

### Part 3: Unified Test Code

**Before:**
```cpp
// TransactionWorker uses mako::DB directly
class TransactionWorker {
public:
    TransactionWorker(mako::DB *mako_db, int worker_id = 0);
    // ...
protected:
    mako::DB* mako_db_;
};

// Separate function for client mode
static int run_client_mode(const char* server_host, int server_port);
```

**After:**
```cpp
// TransactionWorker uses abstract interface
class TransactionWorker {
public:
    TransactionWorker(mako::IDatabase* db, int worker_id = 0);
    // ...
protected:
    mako::IDatabase* db_;
};

// No separate client mode function - same tests work with both
int main() {
    mako::IDatabase* db;
    if (client_mode) {
        mako::RemoteDB* remote_db;
        mako::RemoteDB::Connect(opts, &remote_db);
        db = remote_db;
    } else {
        mako::DB* local_db;
        mako::DB::Open(opts, path, &local_db);
        db = local_db;
    }

    // Same tests for both modes
    run_tests(db);
}
```

## Implementation Steps

### Step 1: Create Abstract Interfaces (~50 LOC)
- Add `IDatabase` and `ITable` to `src/mako/idb.hh` (new file)

### Step 2: Update DB to Implement IDatabase (~100 LOC)
- Add `GetTable()` method to `DB` class
- Create `LocalTable` wrapper in `src/mako/local_table.hh`
- Wrap `mbta_sharded_ordered_index` with `ITable` interface

### Step 3: Update RemoteDB to Implement IDatabase (~20 LOC)
- RemoteDB already has most methods
- Add missing methods as no-ops

### Step 4: Update simpleTransactionRep.cc (~100 LOC)
- Change TransactionWorker to use IDatabase*
- Remove separate run_client_mode() function
- Unify initialization and test code

### Step 5: Merge Documentation (~documentation only)
- Create consolidated docs
- Delete or archive old docs

## Estimated Changes
- New files: ~100 LOC (idb.hh, local_table.hh)
- Modified files: ~150 LOC (db.hh, remote_db.hh, simpleTransactionRep.cc)
- Total: ~250 LOC code changes + documentation

## Success Criteria
1. TransactionWorker can run the same tests with both DB and RemoteDB
2. No branching on mode in test code (except for db initialization)
3. All existing CI tests pass
4. Documentation is consolidated and up-to-date
