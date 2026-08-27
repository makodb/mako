# Troubleshooting Guide

> **Warning**: Parts of this document contain inaccurate or aspirational content that does not reflect the current codebase. Specifically: the SSL/TLS section, `MAKO_LOG_LEVEL` environment variable, `COMPOSITE` sharding method, `transaction.timeout_ms`, and `replication.batch_size` config keys do not exist in the current implementation. Use with caution and verify against actual code.

This guide helps you diagnose and resolve common issues when running Mako.

## Table of Contents

1. [Quick Diagnostic Checklist](#quick-diagnostic-checklist)
2. [Build Issues](#build-issues)
3. [Startup Issues](#startup-issues)
4. [Connection Issues](#connection-issues)
5. [Performance Issues](#performance-issues)
6. [Replication Issues](#replication-issues)
7. [Transaction Issues](#transaction-issues)
8. [Memory Issues](#memory-issues)
9. [Debugging Tools](#debugging-tools)
10. [Getting Help](#getting-help)

---

## Quick Diagnostic Checklist

Before diving into specific issues, check these common problems:

- [ ] **Ports available?** `lsof -i :8100` - No other process using Mako's ports
- [ ] **Build successful?** `make -j$(nproc)` completed without errors
- [ ] **Correct config?** YAML syntax valid, hosts reachable
- [ ] **Sufficient memory?** At least 8GB RAM available
- [ ] **Network accessible?** Firewalls allow connections between nodes
- [ ] **Processes running?** `pgrep dbtest` shows running servers

---

## Build Issues

### Issue: Submodule Not Found

**Symptoms:**
```
fatal: No url found for submodule path 'third-party/masstree-beta'
CMake Error: Could not find required dependency...
```

**Solution:**
```bash
# Clone with recursive flag
git clone --recursive https://github.com/makodb/mako.git

# Or if already cloned
git submodule update --init --recursive
```

### Issue: Missing Dependencies

**Symptoms:**
```
CMake Error: Could not find package XXX
/usr/bin/ld: cannot find -lrocksdb
```

**Solution:**
```bash
# Install all dependencies
bash apt_packages.sh

# For Rust components
source install_rustc.sh
```

### Issue: Out of Memory During Build

**Symptoms:**
```
c++: fatal error: Killed signal terminated program cc1plus
internal compiler error: Killed (program cc1plus)
```

**Solution:**
```bash
# Reduce parallelism
make -j2   # Use only 2 cores

# Or add swap space
sudo fallocate -l 8G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
```

### Issue: Compiler Version Mismatch

**Symptoms:**
```
error: 'std::optional' was not declared in this scope
error: unknown type name 'constexpr'
```

**Solution:**
```bash
# Mako requires C++17
# Check compiler version
g++ --version  # Should be >= 7.0

# Install newer compiler if needed
sudo apt install g++-10
export CXX=g++-10
make clean && make -j$(nproc)
```

### Issue: RustyCpp Borrow Check Failures

**Symptoms:**
```
Borrow check failed: use after move in file.cc:123
```

**Solution:**
```bash
# Check which files have borrow check enabled
grep "borrow_check" CMakeLists.txt

# Temporarily disable for debugging
# In CMakeLists.txt, comment out the borrow_check target

# See doc/srpc-rustycpp-migration-plan.md for migration guide
```

---

## Startup Issues

### Issue: Address Already in Use

**Symptoms:**
```
Error: bind() failed: Address already in use (port 8100)
Server failed to start
```

**Solution:**
```bash
# Find and kill lingering processes
pkill -9 dbtest
pkill -9 simpleTransaction

# Wait for ports to be released
sleep 5

# Or use different ports in config
# In config.yml:
site:
  server:
    - ["s101:8200"]  # Changed from 8100
```

### Issue: Configuration Parse Error

**Symptoms:**
```
YAML parse error at line 15: expected value
Error loading configuration file
```

**Solution:**
```bash
# Validate YAML syntax
python3 -c "import yaml; yaml.safe_load(open('config/myconfig.yml'))"

# Common YAML issues:
# - Tabs instead of spaces (use spaces only)
# - Missing quotes around strings with special characters
# - Incorrect indentation
```

### Issue: Cannot Find Hosts

**Symptoms:**
```
Error: Cannot resolve hostname 'server1'
Connection refused to 10.0.1.100:8100
```

**Solution:**
```bash
# Check /etc/hosts or DNS resolution
ping server1

# Add to /etc/hosts if needed
echo "10.0.1.100 server1" | sudo tee -a /etc/hosts

# Or use IP addresses directly in config
host:
  server1: 10.0.1.100
```

### Issue: Permission Denied

**Symptoms:**
```
Error: cannot open file '/var/log/mako/server.log': Permission denied
Failed to create RocksDB directory
```

**Solution:**
```bash
# Don't run as root
# Run as normal user with appropriate permissions

# Create directories with proper ownership
mkdir -p /tmp/mako_rocksdb
chmod 755 /tmp/mako_rocksdb

# Or change data directory in config to user-writable location
```

---

## Connection Issues

### Issue: Client Cannot Connect to Server

**Symptoms:**
```
Connection failed: Connection refused
RPC timeout after 5000ms
```

**Diagnosis:**
```bash
# 1. Check server is running
pgrep -a dbtest

# 2. Check server is listening
netstat -tlnp | grep 8100

# 3. Check network connectivity
telnet server1 8100

# 4. Check firewall
sudo iptables -L -n | grep 8100
```

**Solutions:**
```bash
# Open firewall port
sudo ufw allow 8100

# Or disable firewall (development only!)
sudo ufw disable

# Check server logs for errors
tail -f /tmp/mako_server.log
```

### Issue: Intermittent Connection Drops

**Symptoms:**
```
Connection reset by peer
RPC call failed: broken pipe
Sporadic transaction failures
```

**Solutions:**
```bash
# 1. Check for network issues
ping -c 100 server1 | grep -v "time="

# 2. Increase timeouts
# In client code or config:
client:
  timeout_ms: 30000  # 30 seconds

# 3. Check for resource exhaustion
ulimit -n  # Check file descriptor limit
ulimit -n 65535  # Increase if needed

# 4. Check system logs
dmesg | tail -50
```

### Issue: SSL/TLS Handshake Failure

**Symptoms:**
```
SSL_connect: certificate verify failed
TLS handshake error
```

**Solution:**
```bash
# Check certificate validity
openssl s_client -connect server1:8100

# Ensure certificates match
# Compare fingerprints on client and server

# For development, disable TLS verification (NOT for production!)
```

---

## Performance Issues

### Issue: Low Throughput

**Symptoms:**
- TPS much lower than expected
- CPU utilization low
- Network utilization low

**Diagnosis:**
```bash
# Check CPU usage
top -p $(pgrep dbtest)

# Check thread count
ps -T -p $(pgrep dbtest) | wc -l

# Check for lock contention
perf top -p $(pgrep dbtest)
```

**Solutions:**

**1. Increase thread count:**
```bash
./build/dbtest --num-threads 24  # Match CPU cores
```

**2. Increase concurrency:**
```yaml
n_concurrent: 10000  # More concurrent transactions
```

**3. Enable batching:**
```cpp
// Batch multiple operations per transaction
for (int i = 0; i < batch_size; i++) {
    txn->put(keys[i], values[i]);
}
txn->commit();
```

### Issue: High Latency

**Symptoms:**
- p99 latency >> p50 latency
- Occasional latency spikes
- Slow response times

**Diagnosis:**
```bash
# Profile the application
perf record -g -p $(pgrep dbtest) -- sleep 30
perf report

# Check for GC pressure
# Mako uses jemalloc, check for fragmentation
```

**Solutions:**

**1. Check watermark lag:**
```cpp
// If watermark is far behind, transactions may be queuing
uint32_t lag = current_timestamp - watermark;
if (lag > threshold) {
    // Watermark is falling behind
}
```

**2. Reduce cross-shard transactions:**
```yaml
# Design schema to keep related data together
sharding:
  user_data: MOD  # Shard by user_id
```

**3. Enable CPU affinity:**
```bash
# Pin threads to specific cores
taskset -c 0-23 ./build/dbtest --num-threads 24
```

### Issue: Uneven Load Distribution

**Symptoms:**
- Some shards heavily loaded, others idle
- Hot shard bottleneck
- Non-linear scaling

**Solutions:**

**1. Check data distribution:**
```bash
# Analyze key distribution across shards
# Look for hot keys
```

**2. Improve sharding:**
```yaml
# Use composite keys for better distribution
sharding:
  order: COMPOSITE  # (user_id, order_id)
```

**3. Add more shards:**
```yaml
site:
  server:
    - ["s1:8100"]
    - ["s2:8100"]
    - ["s3:8100"]
    - ["s4:8100"]  # Add more shards
```

---

## Replication Issues

### Issue: Paxos Not Making Progress

**Symptoms:**
```
Paxos timeout waiting for quorum
No progress on slot 123
Leader election stuck
```

**Diagnosis:**
```bash
# Check all replicas are running
for host in s101 s201 s301; do
    ssh $host "pgrep dbtest"
done

# Check network between replicas
for host in s201 s301; do
    ping -c 5 $host
done
```

**Solutions:**

**1. Ensure quorum is reachable:**
```bash
# With 3 replicas, 2 must be reachable
# With 5 replicas, 3 must be reachable
```

**2. Check Paxos logs:**
```bash
# Look for ballot conflicts or stuck elections
grep -i "paxos\|ballot\|election" /tmp/mako_server.log
```

**3. Restart stuck replica:**
```bash
ssh s201 "pkill dbtest && ./start_server.sh"
```

### Issue: Follower Falling Behind

**Symptoms:**
```
Replica s201 replication lag: 5000 entries
Watermark not advancing
```

**Solutions:**

**1. Check follower health:**
```bash
ssh s201 "top -b -n 1 | head -20"
```

**2. Increase replication bandwidth:**
```yaml
# In network config
replication:
  batch_size: 100  # Larger batches
```

**3. Check for slow disk:**
```bash
iostat -x 1 5  # Check disk utilization
```

### Issue: Split Brain

**Symptoms:**
```
Multiple leaders detected for partition 0
Inconsistent data between replicas
```

**Solutions:**

**1. This shouldn't happen with correct Paxos**
- Check for network partitions that healed
- Verify Paxos implementation

**2. Force re-election:**
```bash
# Stop all replicas
pkill dbtest

# Restart in order (leader first)
ssh s101 "./start_server.sh"
sleep 5
ssh s201 "./start_server.sh"
ssh s301 "./start_server.sh"
```

---

## Transaction Issues

### Issue: High Abort Rate

**Symptoms:**
```
Transaction abort rate: 15%
Conflict detected on key 'X'
Retry limit exceeded
```

**Solutions:**

**1. Reduce conflict rate:**
```cpp
// Access keys in consistent order
sort(keys_to_access);
for (auto& key : keys_to_access) {
    txn->get(key);
}
```

**2. Shorter transactions:**
```cpp
// BAD: Long transaction
txn->begin();
expensive_computation();  // Holds locks
txn->commit();

// GOOD: Short transaction
auto data = txn->get_all(keys);
auto result = expensive_computation(data);  // No locks
txn->put_all(result);
```

**3. Retry with backoff:**
```cpp
for (int retry = 0; retry < max_retries; retry++) {
    try {
        execute_transaction();
        break;
    } catch (ConflictException& e) {
        sleep(backoff * (1 << retry));  // Exponential backoff
    }
}
```

### Issue: Transaction Timeout

**Symptoms:**
```
Transaction timeout after 5000ms
Lock acquisition timeout
```

**Solutions:**

**1. Increase timeout:**
```yaml
transaction:
  timeout_ms: 30000  # 30 seconds
```

**2. Check for deadlocks:**
```cpp
// Access keys in consistent order to prevent deadlock
// If txn1: lock(A), lock(B)
// And txn2: lock(B), lock(A)
// Deadlock!

// Solution: Always lock in sorted key order
```

**3. Break up large transactions:**
```cpp
// Instead of one big transaction
// Use multiple smaller ones
```

### Issue: Read-After-Write Not Seeing Write

**Symptoms:**
```cpp
txn1->put("key", "value");
txn1->commit();
// Immediately after:
txn2->get("key");  // Returns old value!
```

**Explanation:**
This is expected behavior with speculative execution! The write hasn't been replicated yet.

**Solutions:**

**1. Read in same transaction:**
```cpp
txn->put("key", "value");
auto val = txn->get("key");  // Sees buffered write
txn->commit();
```

**2. Wait for watermark:**
```cpp
txn1->put("key", "value");
auto ts = txn1->commit();
wait_for_watermark(ts);  // Wait for replication
txn2->get("key");  // Now sees write
```

**3. Use synchronous commit:**
```cpp
txn->commit({.synchronous = true});  // Wait for Paxos
```

---

## Memory Issues

### Issue: Out of Memory

**Symptoms:**
```
std::bad_alloc
Killed by OOM killer
Memory usage growing unboundedly
```

**Solutions:**

**1. Check memory usage:**
```bash
# Check Mako memory
ps -o pid,rss,vsz,comm -p $(pgrep dbtest)

# Check system memory
free -h
```

**2. Limit data size:**
```yaml
# Reduce test data population
bench:
  population:
    warehouse: 1  # Smaller scale
```

**3. Enable memory limits:**
```bash
# Use cgroups to limit memory
cgcreate -g memory:/mako
echo "8G" > /sys/fs/cgroup/memory/mako/memory.limit_in_bytes
cgexec -g memory:mako ./build/dbtest
```

### Issue: Memory Leak

**Symptoms:**
- Memory usage grows over time
- Eventually runs out of memory
- Performance degrades

**Diagnosis:**
```bash
# Use valgrind (slow but thorough)
valgrind --leak-check=full ./build/dbtest

# Use AddressSanitizer (faster)
# Rebuild with: cmake -DCMAKE_CXX_FLAGS="-fsanitize=address"
```

**Solutions:**
- Report the issue with reproduction steps
- Check for missing `delete` or `release()` calls
- Ensure RustyCpp smart pointers are used correctly

---

## Debugging Tools

### Logging

```bash
# Enable verbose logging
export MAKO_LOG_LEVEL=DEBUG
./build/dbtest

# Log to file
./build/dbtest 2>&1 | tee mako.log

# Filter specific components
./build/dbtest 2>&1 | grep -E "PAXOS|TRANSACTION"
```

### Profiling

```bash
# CPU profiling with perf
perf record -g -p $(pgrep dbtest) -- sleep 30
perf report

# Generate flame graph
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg

# Memory profiling with heaptrack
heaptrack ./build/dbtest
heaptrack_print heaptrack.dbtest.*.gz
```

### Tracing

```bash
# System call tracing
strace -f -p $(pgrep dbtest) -o strace.log

# Network tracing
tcpdump -i eth0 -w mako.pcap port 8100
```

### Core Dumps

```bash
# Enable core dumps
ulimit -c unlimited

# Set core dump pattern
echo "/tmp/core.%e.%p" | sudo tee /proc/sys/kernel/core_pattern

# Analyze core dump
gdb ./build/dbtest /tmp/core.dbtest.12345
(gdb) bt  # Backtrace
```

---

## Getting Help

### Before Asking for Help

1. **Check this guide** - Your issue may be covered above
2. **Search existing issues** - Someone may have encountered this before
3. **Prepare information**:
   - Mako version (`git log -1`)
   - OS version (`uname -a`)
   - Configuration file
   - Error messages and logs
   - Steps to reproduce

### Where to Get Help

- **GitHub Issues**: [github.com/makodb/mako/issues](https://github.com/makodb/mako/issues)
- **GitHub Discussions**: [github.com/makodb/mako/discussions](https://github.com/makodb/mako/discussions)
- **Documentation**: [doc/index.md](index.md)

### Reporting Bugs

Include:
1. **Summary**: One-line description
2. **Environment**: OS, hardware, Mako version
3. **Steps to reproduce**: Minimal steps to trigger the bug
4. **Expected behavior**: What should happen
5. **Actual behavior**: What actually happens
6. **Logs**: Relevant log excerpts
7. **Configuration**: Sanitized config file

---

**Next**: [Connection Issues](connection.md) | [Performance Issues](performance.md) | [FAQ](../faq/general.md)
