# RDMA Implementation Project Summary

## 🎯 Project Goal
Implement RDMA functionality within the **sRPC framework** by taking functionality from **eRPC** to improve:
1. **CPU Utilization** - Reduce CPU overhead via kernel bypass
2. **Latency** - Achieve ultra-low latency (~1-2 μs)

## ✅ You're Correct: Skip Geo-Distributed Testing
For RDMA implementation, you **DO NOT need**:
- ❌ Multi-datacenter geo-replication
- ❌ 10-shard distributed setups
- ❌ Cross-region latency testing
- ❌ Large-scale TPC-C benchmarks

**Why?** RDMA benefits are for **single-datacenter, low-latency RPC** communication.

---

## 📊 The 3 Core Metrics You Should Focus On

### 1. **RPC Latency** (μs) - PRIMARY
**Current (sRPC/TCP):** 15-30 μs  
**Target (RDMA):** < 2 μs  
**Improvement Goal:** 10-15x reduction

**Why it matters:** RDMA's kernel bypass provides ultra-low latency.

### 2. **CPU Utilization** (%) - PRIMARY
**Current (sRPC/TCP):** 70-80% CPU at 100K RPS  
**Target (RDMA):** 30-40% CPU at 100K RPS  
**Improvement Goal:** 50% reduction

**Why it matters:** Kernel bypass reduces CPU cycles spent on network I/O.

### 3. **Throughput** (RPS) - SECONDARY
**Current (sRPC/TCP):** 100-200K RPS  
**Target (RDMA):** 500K-1M RPS  
**Improvement Goal:** 3-5x increase

**Why it matters:** Lower overhead = more requests per second.

---

## 🛠️ How to Measure These Metrics

### Quick Start: Baseline Measurement
```bash
# Build rpcbench (RPC benchmark tool)
make rpcbench -j$(nproc)

# Run baseline metrics collection
./collect_rdma_metrics.sh
```

This will measure:
- ✅ Latency at low concurrency (1 thread, 1 outstanding request)
- ✅ CPU usage at high load (8 threads, 5000 outstanding requests)
- ✅ Throughput scaling (1, 2, 4, 8, 16 threads)
- ✅ Message size impact (10B to 8KB)

### Manual Testing

**Test 1: Latency**
```bash
# Terminal 1: Start server
./build/rpcbench -s 0.0.0.0:8848 -f

# Terminal 2: Measure latency
./build/rpcbench -c localhost:8848 -f -n 30 -o 1 -t 1
# Look for: Average RPS → Latency ≈ 1M / RPS μs
```

**Test 2: CPU Usage**
```bash
# Terminal 1: Start server
./build/rpcbench -s 0.0.0.0:8848 -w 8 &
SERVER_PID=$!

# Terminal 2: Monitor CPU
top -p $SERVER_PID

# Terminal 3: Generate load
./build/rpcbench -c localhost:8848 -t 8 -o 5000 -n 60
```

**Test 3: Throughput**
```bash
# Max throughput test
./build/rpcbench -s 0.0.0.0:8848 -w 16 &
./build/rpcbench -c localhost:8848 -t 16 -o 10000 -n 60
# Look for: Average: XXX reqs/s
```

---

## 📈 Success Criteria

### Minimum Viable RDMA Implementation
- ✅ Latency: < 5 μs (vs 15-30 μs)
- ✅ CPU: < 50% of sRPC CPU at same RPS
- ✅ Throughput: > 300K RPS (vs 100-200K)

### Excellent RDMA Implementation
- ✅ Latency: < 2 μs
- ✅ CPU: < 30% of sRPC CPU
- ✅ Throughput: > 500K RPS
- ✅ Linear scaling up to 16 threads

---

## 🔧 Current System Architecture

Mako already has **dual transport support**:

1. **rrr/rpc** (default) - TCP/IP based
   - Latency: ~10-50 μs
   - Portable, works everywhere
   - Located: `src/mako/lib/rrr_rpc_backend.{h,cc}`

2. **eRPC** - RDMA based
   - Latency: ~1-2 μs
   - Requires RDMA hardware
   - Located: `src/mako/lib/erpc_backend.{h,cc}`

**Your Goal:** Add RDMA to **sRPC** (a third option) by borrowing from eRPC.

### How to Switch Transports
```bash
# Use rrr/rpc (default)
./build/dbtest config/mako_tpcc.yml

# Use eRPC (RDMA)
MAKO_TRANSPORT=erpc ./build/dbtest config/mako_tpcc.yml

# Future: Use sRPC with RDMA
MAKO_TRANSPORT=srpc_rdma ./build/dbtest config/mako_tpcc.yml
```

---

## 📁 Key Files for Your Implementation

### Source Files to Study
```
src/mako/lib/erpc_backend.h         - eRPC backend interface
src/mako/lib/erpc_backend.cc        - eRPC implementation (RDMA)
src/mako/lib/transport_backend.h    - Abstract transport interface
third-party/erpc/                   - eRPC library source
```

### Benchmark Tools
```
test/rpcbench.cc                    - RPC benchmark tool
test/benchmark_service.rpc          - RPC service definition
doc/rpc-benchmark.md                - Benchmark documentation
```

### Configuration
```
src/mako/lib/configuration.h        - Transport configuration
doc/transport_backends.md           - Transport switching guide
```

---

## 🚀 Recommended Development Flow

### Phase 1: Baseline Measurement (Now)
```bash
# Measure current sRPC performance
./collect_rdma_metrics.sh

# Study results in rdma_metrics_YYYYMMDD_HHMMSS/
# Note: Latency, CPU usage, Throughput
```

### Phase 2: Study eRPC Implementation
```bash
# Read eRPC backend code
cat src/mako/lib/erpc_backend.h
cat src/mako/lib/erpc_backend.cc

# Understand RDMA setup
grep -r "ibv_" third-party/erpc/src/  # InfiniBand verbs
grep -r "hugepage" third-party/erpc/  # Memory setup
```

### Phase 3: Implement RDMA in sRPC
```bash
# Create new files (example)
# src/mako/lib/srpc_rdma_backend.h
# src/mako/lib/srpc_rdma_backend.cc

# Key functions to implement:
# - RDMA connection setup
# - Zero-copy send/receive
# - Kernel bypass I/O
# - Memory registration
```

### Phase 4: Validate with Metrics
```bash
# Test your RDMA implementation
MAKO_TRANSPORT=srpc_rdma ./collect_rdma_metrics.sh

# Compare with baseline
# Target: 10x latency reduction, 50% CPU reduction
```

---

## 💡 Key RDMA Concepts to Implement

### 1. **Kernel Bypass**
- Direct hardware access (no system calls)
- Reduces context switches
- **Metric Impact:** Lower CPU usage

### 2. **Zero-Copy**
- DMA directly to/from application buffers
- No kernel buffer copying
- **Metric Impact:** Lower latency, lower CPU

### 3. **RDMA Verbs API**
```c
// Key functions from eRPC you'll need:
ibv_create_qp()      // Queue pair for RDMA
ibv_post_send()      // Send without kernel
ibv_post_recv()      // Receive without kernel
ibv_poll_cq()        // Poll completion queue
```

### 4. **Memory Registration**
```c
// Pin memory for DMA
ibv_reg_mr()         // Register memory region
// Allows NIC to directly access memory
```

---

## 📊 Expected Results Comparison

| Metric | sRPC (TCP) | eRPC (RDMA) | Your Target |
|--------|------------|-------------|-------------|
| **Latency (μs)** | 15-30 | 1-2 | < 5 |
| **CPU % (100K RPS)** | 70-80% | 20-30% | < 40% |
| **Throughput (RPS)** | 100-200K | 500K-1M | > 300K |
| **Context Switches** | High | Very Low | Low |
| **System Calls** | Many | Near Zero | Near Zero |

---

## 🎓 Learning Resources

### Documentation in This Repo
- `doc/transport_backends.md` - Transport architecture
- `doc/rpc-benchmark.md` - How to use rpcbench
- `RDMA_METRICS_GUIDE.md` - Detailed metrics guide (created for you)

### External Resources
- eRPC Paper: https://www.usenix.org/conference/nsdi19/presentation/kalia
- RDMA Programming: https://github.com/jcxue/RDMA-Tutorial
- InfiniBand Verbs: https://www.rdmamojo.com/

---

## 📝 Quick Reference Commands

```bash
# Collect all RDMA-relevant metrics
./collect_rdma_metrics.sh

# Test latency only
./build/rpcbench -s 0.0.0.0:8848 -f &
./build/rpcbench -c localhost:8848 -f -n 30 -o 1 -t 1

# Test throughput only
./build/rpcbench -s 0.0.0.0:8848 -w 16 &
./build/rpcbench -c localhost:8848 -t 16 -o 10000 -n 60

# Monitor CPU during test
top -p $(pgrep rpcbench | head -1)

# Profile with perf
perf stat -e cycles,instructions ./build/rpcbench -c localhost:8848 -n 30

# Check RDMA hardware
ibstat
ibv_devices
```

---

## ✅ Summary

**Your Goal:** Implement RDMA in sRPC framework

**Focus On:**
1. ✅ RPC Latency (< 2 μs target)
2. ✅ CPU Utilization (50% reduction target)
3. ✅ Throughput (> 500K RPS target)

**Skip:**
- ❌ Geo-distributed testing
- ❌ Multi-shard TPC-C
- ❌ Cross-datacenter latency

**Tools:**
- `rpcbench` - RPC benchmark
- `collect_rdma_metrics.sh` - Automated metrics collection
- `RDMA_METRICS_GUIDE.md` - Detailed guide

**Next Steps:**
1. Run `./collect_rdma_metrics.sh` to get baseline
2. Study `src/mako/lib/erpc_backend.cc` for RDMA implementation
3. Implement RDMA in sRPC
4. Validate with same metrics script

Good luck! 🚀
