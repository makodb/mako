# MakoCon - Redis-Compatible Key-Value Store

MakoCon is a Redis-compatible key-value store implementation written in C++ with Rust networking components. It provides a subset of Redis functionality while maintaining protocol compatibility for client libraries.

## Architecture

The system uses a hybrid Rust-C++ architecture:

- **Rust Layer** (`rust-lib/`): Handles TCP networking, RESP3 protocol parsing, and client connections
  * When initialized, spawns a background thread hosting a tokio runtime
  * Per client connection, creates a tokio task inside the tokio runtime
  * tokio task stays lightweight and only comsumes CPU when active
- **C++ Layer** (`src/`): Implements the core key-value storage engine and data structures
- **Communication**: Rust and C++ communicate via a request-response queue system

## Supported Redis Commands

### ✅ String Operations
- `SET key value` - Store string value  
  **Implementation:** uses `std::map<std::string, std::string> store_` with `store_[key] = value`
- `GET key` - Retrieve string value  
  **Implementation:** returns `store_[key]` if exists, otherwise NULL
- `PING` - Connection test (returns PONG)  
  **Implementation:** hardcoded response, no data structure needed

### ✅ Numeric Operations  
- `INCR key` - Increment integer value by 1  
  **Implementation:** parses `store_[key]` as int, increments, stores back as string
- `DECR key` - Decrement integer value by 1  
  **Implementation:** parses `store_[key]` as int, decrements, stores back as string
- `INCRBY key increment` - Increment by specified amount  
  **Implementation:** parses `store_[key]` as int, adds increment, stores back
- `DECRBY key decrement` - Decrement by specified amount  
  **Implementation:** parses `store_[key]` as int, subtracts decrement, stores back

### ✅ List Operations
- `LPUSH key value [value ...]` - Push to left side of list  
  **Implementation:** uses `std::map<std::string, std::list<std::string>> lists_` with `lists_[key].push_front(value)`
- `RPUSH key value [value ...]` - Push to right side of list  
  **Implementation:** uses `lists_[key].push_back(value)`
- `LPOP key` - Pop from left side  
  **Implementation:** returns and removes `lists_[key].front()`
- `RPOP key` - Pop from right side  
  **Implementation:** returns and removes `lists_[key].back()`
- `LLEN key` - Get list length  
  **Implementation:** returns `lists_[key].size()`
- `LRANGE key start stop` - Get range of elements  
  **Implementation:** iterates through `lists_[key]` from start to stop indices

### ✅ Hash Operations
- `HSET key field value` - Set hash field  
  **Implementation:** uses `std::map<std::string, std::unordered_map<std::string, std::string>> hashes_` with `hashes_[key][field] = value`
- `HGET key field` - Get hash field  
  **Implementation:** returns `hashes_[key][field]` if exists
- `HGETALL key` - Get all hash fields and values  
  **Implementation:** iterates through `hashes_[key]` returning all key-value pairs
- `HMGET key field [field ...]` - Get multiple hash fields  
  **Implementation:** looks up each field in `hashes_[key][field]`
- `HDEL key field` - Delete hash field  
  **Implementation:** removes field with `hashes_[key].erase(field)`
- `HEXISTS key field` - Check if hash field exists  
  **Implementation:** checks `hashes_[key].find(field) != end()`

### ✅ Set Operations
- `SADD key member [member ...]` - Add members to set  
  **Implementation:** uses `std::map<std::string, std::unordered_set<std::string>> sets_` with `sets_[key].insert(member)`
- `SMEMBERS key` - Get all set members  
  **Implementation:** iterates through `sets_[key]` returning all members
- `SISMEMBER key member` - Check set membership  
  **Implementation:** checks `sets_[key].find(member) != end()`
- `SINTER key1 key2` - Set intersection  
  **Implementation:** iterates through `sets_[key1]` checking if each member exists in `sets_[key2]`
- `SDIFF key1 key2` - Set difference  
  **Implementation:** iterates through `sets_[key1]` excluding members that exist in `sets_[key2]`
- `SCARD key` - Get set cardinality (size)  
  **Implementation:** returns `sets_[key].size()`

### ✅ Key Management
- `DEL key` - Delete key  
  **Implementation:** removes key from all data structures (`store_.erase(key)`, `lists_.erase(key)`, etc.)
- `EXISTS key` - Check if key exists  
  **Implementation:** searches across all data structures (`store_.find(key) != end()`, etc.)
- `EXPIRE key seconds` - Set key expiration  
  **Implementation:** uses `std::map<std::string, std::chrono::steady_clock::time_point> expiry_times_` to store `expiry_times_[key] = now + seconds`
- `TTL key` - Get time to live  
  **Implementation:** calculates remaining time from `expiry_times_[key] - now`
- `KEYS pattern` - Find keys matching pattern  
  **Implementation:** iterates through all data structures using `std::regex` to match pattern against key names

### ✅ Transaction Commands (Sequential Execution)
- `MULTI` - Start transaction block  
  **Implementation:** begins command queuing, returns "OK"
- `EXEC` - Execute transaction  
  **Implementation:** executes all queued commands in order, returns array of results
- `DISCARD` - Discard transaction  
  **Implementation:** clears command queue, returns "OK"
- `WATCH key` - Watch key for changes  
  **Implementation:** returns "OK" (no actual monitoring, for client compatibility)
- `UNWATCH` - Stop watching keys  
  **Implementation:** returns "OK" (no actual unwatching needed)

**Transaction Behavior:**
- Commands between MULTI and EXEC are queued and return "+QUEUED"
- EXEC processes all queued commands sequentially and returns an array of results
- Pipeline transactions work with Redis clients (redis-py pipelines)
- No true atomicity: commands execute in order but are not atomic as a unit

### ✅ Pipeline Support
- Redis client pipelines are supported for batching commands
- Compatible with redis-py `pipeline()` usage patterns
- Supports both explicit MULTI/EXEC transactions and implicit pipelining


## TODOs:
- ❌ pipe/exec() returns results for each operation. For Mako's transaction model, how to be compatible with it, https://redis.io/docs/latest/develop/using-commands/transactions/.
- ❌ can't support regex expression, see `cleanup_redis`

## ❌ Unsupported Features

### True Atomic Transactions
**Note:** Transaction commands provide sequential execution only, not true atomicity. Commands in a transaction execute in order but individual command failures don't roll back the entire transaction.

**Limitations:**
- No rollback on partial transaction failure
- No isolation between concurrent transactions
- WATCH doesn't actually monitor key changes (returns OK for compatibility)

### Other Unsupported Features
- **Pub/Sub** - Requires persistent connection state and message broadcasting
- **Lua Scripting** - Would need embedded Lua interpreter
- **Clustering** - Single-node implementation only
- **Persistence** - In-memory only (no RDB/AOF)
- **Replication** - No master-slave support
- **Streams** - Complex data structure not implemented
- **Modules** - No plugin system
- **Advanced Set Operations** - SUNION, SINTERSTORE, etc.
- **Sorted Sets** - ZADD, ZRANGE, etc. not implemented
