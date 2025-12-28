# RocksDB Usage Guide

This guide provides a brief overview of the common interfaces of RocksDB and how to use them, with examples relevant to this project.

## 1. Introduction to RocksDB

RocksDB is a high-performance embedded key-value store. It's used in this project for data persistence. Keys and values are arbitrary-sized byte streams.

## 2. Basic Operations

### Opening a Database

To open a RocksDB database, you need to specify the path to the database directory and provide some options. If the database doesn't exist, it will be created.

```cpp
#include <rocksdb/db.h>
#include <rocksdb/options.h>

rocksdb::DB* db;
rocksdb::Options options;
options.create_if_missing = true;

rocksdb::Status status = rocksdb::DB::Open(options, "/tmp/testdb", &db);

if (!status.ok()) {
  // Handle error
}
```

### Writing Data (Put)

To add or update a key-value pair, use the `Put` method.

```cpp
rocksdb::WriteOptions write_options;
rocksdb::Status status = db->Put(write_options, "my_key", "my_value");

if (!status.ok()) {
  // Handle error
}
```

### Reading Data (Get)

To retrieve a value by its key, use the `Get` method.

```cpp
rocksdb::ReadOptions read_options;
std::string value;
rocksdb::Status status = db->Get(read_options, "my_key", &value);

if (status.ok()) {
  // Value found
  std::cout << "Found value: " << value << std::endl;
} else if (status.IsNotFound()) {
  // Key not found
} else {
  // Handle other errors
}
```

### Deleting Data

To remove a key-value pair, use the `Delete` method.

```cpp
rocksdb::WriteOptions write_options;
rocksdb::Status status = db->Delete(write_options, "my_key");

if (!status.ok()) {
  // Handle error
}
```

## 3. Advanced Configuration

You can tune RocksDB's performance and behavior through the `rocksdb::Options` object.

### Setting a Block Cache

A block cache is used to cache uncompressed data blocks. A shared cache can be used across multiple RocksDB instances to control total memory usage.

```cpp
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/cache.h>

rocksdb::DB* db;
rocksdb::Options options;
options.create_if_missing = true;

// Create a 128MB LRU cache
std::shared_ptr<rocksdb::Cache> cache = rocksdb::NewLRUCache(128 * 1024 * 1024);

rocksdb::BlockBasedTableOptions table_options;
table_options.block_cache = cache;
options.table_factory.reset(NewBlockBasedTableFactory(table_options));

rocksdb::Status status = rocksdb::DB::Open(options, "/tmp/testdb", &db);

if (!status.ok()) {
  // Handle error
}
```

## 4. Advanced Features

### Iterators

Iterators allow you to perform a range scan over the database. This is an efficient way to access all keys or a subset of keys.

```cpp
rocksdb::Iterator* it = db->NewIterator(rocksdb::ReadOptions());

for (it->SeekToFirst(); it->Valid(); it->Next()) {
  std_cout << it->key().ToString() << ": " << it->value().ToString() << std::endl;
}

if (!it->status().ok()) {
  // Handle error
}

delete it;
```

### Write Batches

A `WriteBatch` allows you to perform multiple `Put` and `Delete` operations atomically. This is more efficient than performing multiple individual writes.

```cpp
#include <rocksdb/write_batch.h>

rocksdb::WriteBatch batch;
batch.Put("key1", "value1");
batch.Put("key2", "value2");
batch.Delete("key3");

rocksdb::WriteOptions write_options;
rocksdb::Status status = db->Write(write_options, &batch);

if (!status.ok()) {
  // Handle error
}
```

### Transactions

For operations that require read-modify-write sequences, you should use transactions to avoid race conditions. RocksDB provides optimistic and pessimistic transactions. Here is an example of a pessimistic transaction.

First, you need to open a `TransactionDB`.

```cpp
#include <rocksdb/db.hh>
#include <rocksdb/options.h>
#include "rocksdb/utilities/transaction_db.h"

rocksdb::TransactionDB* txn_db;
rocksdb::Options options;
rocksdb::TransactionDBOptions txn_db_options;
options.create_if_missing = true;

rocksdb::Status status = rocksdb::TransactionDB::Open(options, txn_db_options, "/tmp/txndb", &txn_db);

if (!status.ok()) {
    // Handle error
}
```

Then, you can perform transactional operations.

```cpp
#include "rocksdb/utilities/transaction.h"

rocksdb::WriteOptions write_options;
rocksdb::ReadOptions read_options;

// Start a transaction
rocksdb::Transaction* txn = txn_db->BeginTransaction(write_options);

// Perform operations within the transaction
std::string value;
status = txn->Get(read_options, "my_key", &value);
if (status.ok() || status.IsNotFound()) {
    // process value...
    status = txn->Put("my_key", "new_value");
}

if (status.ok()) {
    // Commit the transaction
    status = txn->Commit();
} else {
    // Or rollback on error
    status = txn->Rollback();
}

delete txn;
```

## 5. Multi-Threading Considerations

You do not need to perform any special thread initialization to use RocksDB. The main `rocksdb::DB` (or `rocksdb::TransactionDB`) object is thread-safe and designed to be shared among threads.

However, some objects, like `Iterator` and `Transaction`, are **not** thread-safe and must only be used by a single thread.

The following example shows two threads safely incrementing a shared counter using transactions and reading data using their own separate iterators.

```cpp
#include <thread>
#include <vector>
#include "rocksdb/utilities/transaction_db.h"
#include "rocksdb/utilities/transaction.h"

// A function to be executed by each thread
void worker_thread(rocksdb::TransactionDB* txn_db, int thread_id) {
    // --- Thread-Safe Read-Modify-Write using Transactions ---
    // Each thread must start its own transaction.
    rocksdb::WriteOptions write_options;
    rocksdb::ReadOptions read_options;
    rocksdb::Transaction* txn = txn_db->BeginTransaction(write_options);

    // Atomically increment a shared counter
    std::string counter_key = "shared_counter";
    std::string current_value_str;
    rocksdb::Status status = txn->Get(read_options, counter_key, &current_value_str);
    
    int current_value = 0;
    if (status.IsNotFound()) {
        // The counter doesn't exist yet, so we'll start at 0.
    } else if (status.ok()) {
        try {
            current_value = std::stoi(current_value_str);
        } catch (const std::invalid_argument& e) {
            // Handle cases where the value is not a valid integer
            txn->Rollback();
            delete txn;
            return;
        }
    } else {
        // Handle other read errors
        txn->Rollback();
        delete txn;
        return;
    }
    
    std::string new_value_str = std::to_string(current_value + 1);
    status = txn->Put(counter_key, new_value_str);

    if (status.ok()) {
        status = txn->Commit();
    } else {
        txn->Rollback();
    }
    delete txn;


    // --- Using Iterators (NOT Thread-Safe) ---
    // Each thread must create its own iterator.
    rocksdb::Iterator* it = txn_db->NewIterator(read_options);
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        // Process data...
        // std::cout << "Thread " << thread_id << " sees key: " << it->key().ToString() << std::endl;
    }
    delete it; // Clean up the iterator for this thread
}
```

## 6. Closing the Database

When you are finished with the database, you should close it to release the resources.

```cpp
delete db; // or delete txn_db;
```
