# Quick Start Tutorial

Get Mako running in 10 minutes! This tutorial will guide you through installing Mako and running your first distributed transaction.

## Prerequisites

Before you begin, ensure you have:
- Debian 12 or Ubuntu 22.04 LTS
- At least 8 GB RAM and 4 CPU cores
- 20 GB free disk space
- sudo access for installing packages

## Step 1: Install Mako (5 minutes)

```bash
# Clone the repository
git clone --recursive https://github.com/makodb/mako.git
cd mako

# Install dependencies
bash apt_packages.sh
source install_rustc.sh

# Build Mako (this takes a few minutes)
make -j$(nproc)
```

## Step 2: Verify Installation (1 minute)

Run a simple transaction test to verify everything works:

```bash
./ci/ci.sh simpleTransaction
```

You should see output like:
```
Running simple transaction test...
Transaction committed successfully!
✓ Test passed
```

## Step 3: Start Your First Cluster (2 minutes)

### Single-Node Cluster

The simplest deployment is a single shard on localhost:

```bash
# Run the simple Paxos example
./ci/ci.sh simplePaxos
```

This starts:
- 1 server shard with Paxos replication
- Executes sample transactions
- Verifies results

### Multi-Shard Cluster

Run a 2-shard cluster with replication:

```bash
./ci/ci.sh shard2Replication
```

This demonstrates:
- Data sharding across 2 partitions
- Paxos consensus for each shard
- Distributed transactions across shards
- Automatic failover

### Shard Fault Tolerance Test

Test independent shard operation during partner failures:

```bash
./ci/ci.sh shardFaultTolerance
```

This test:
- Starts 2 shards without replication
- Runs continuous transaction workload
- Periodically reboots each shard
- Verifies the other shard continues processing transactions independently
- Validates graceful degradation under shard failures

**Custom parameters**:
```bash
# Run with custom settings
bash examples/test_shard_fault_tolerance.sh [threads] [num_reboots] [interval_seconds]

# Example: 8 threads, 3 reboot cycles, 15-second intervals
bash examples/test_shard_fault_tolerance.sh 8 3 15
```

## Step 4: Understanding the Output

When you run the tests, you'll see output like this:

```
[Server] Starting Mako server on shard 0...
[Server] Listening on 127.0.0.1:8100
[Paxos] Initialized with 3 replicas
[Storage] Using Masstree in-memory storage
[Storage] RocksDB persistence enabled
[Client] Connected to server
[Transaction] Starting transaction txn-001
[Transaction] Read key="user:1001"
[Transaction] Write key="user:1001" value="..."
[Transaction] Commit successful (latency: 2.3ms)
✓ All transactions completed successfully
```

**Key components**:
- **Server**: The main database server process (dbtest)
- **Paxos**: Consensus protocol for replication
- **Storage**: Masstree (in-memory) + RocksDB (disk)
- **Client**: Test client executing transactions
- **Transaction**: Individual transaction lifecycle

## Step 5: Run Benchmarks (2 minutes)

Try running TPC-C benchmark to see Mako's performance:

```bash
# Run dbtest with simple configuration (To be implemented - full TPC-C benchmarking interface)
./build/dbtest
```

Expected output (example numbers):
```
Throughput: ~50,000 transactions/second
Latency p50: 1.8ms
Latency p99: 7.1ms
```

## What's Next?

Now that Mako is running, explore more:

### Learn the Concepts
- **[Key Concepts](concepts.md)** - Understand shards, replicas, transactions
- **[Architecture Overview](architecture.md)** - How Mako works internally
- **[Speculative 2PC](speculative-2pc.md)** - The core innovation

### Deploy Mako
- **[Configuration Reference](config.md)** - Customize your deployment
- **[Multi-Datacenter Setup](multi-dc.md)** - Geo-replication across datacenters
- **[EC2 Deployment](ec2.md)** - Deploy on AWS

### Use Mako
- **[CRUD Operations](crud/insert.md)** - Basic data operations *(To be implemented)*
- **[Transactions](transactions/basics.md)** - Understanding distributed transactions *(To be implemented)*
- **[Client API](clients/cpp.md)** - C++ client library *(To be implemented)*

### Performance
- **[Benchmarking Guide](performance/benchmarks.md)** - Run comprehensive benchmarks
- **[Performance Tuning](performance/tuning.md)** - Optimize throughput and latency

## Common Quick Start Issues

### Issue: Build fails with "submodule not found"
**Solution**: Clone with `--recursive`:
```bash
git clone --recursive https://github.com/makodb/mako.git
# Or if already cloned:
git submodule update --init --recursive
```

### Issue: Tests fail with "Address already in use"
**Solution**: Kill lingering processes:
```bash
pkill -9 dbtest
pkill -9 simpleTransaction
sleep 2
# Then re-run the test
```

### Issue: Out of memory during build
**Solution**: Reduce build parallelism:
```bash
make -j2  # Use only 2 cores
```

### Issue: Permission denied
**Solution**: Don't use sudo for build:
```bash
make -j$(nproc)  # NOT: sudo make
```

## Example: Writing Your First Transaction

Here's a minimal C++ example using Mako's API *(To be implemented - API documentation and client library)*:

```cpp
#include "mako/client.h"

int main() {
    // Connect to Mako server
    MakoClient client("127.0.0.1:8100");

    // Start a transaction
    auto txn = client.BeginTransaction();

    // Write data
    txn->Put("user:1001", "Alice");
    txn->Put("balance:1001", "1000.00");

    // Read data
    std::string name = txn->Get("user:1001");

    // Commit transaction
    txn->Commit();

    std::cout << "Transaction committed! User: " << name << std::endl;
    return 0;
}
```

**Note**: The native client API is currently under development. For now, use the test examples in `src/mako/` as reference.

## Exploring the Codebase

If you want to understand how things work:

```
mako/
├── src/
│   ├── mako/           # Main Mako implementation
│   │   ├── mako.cc     # Server entry point
│   │   ├── Transaction.cc  # Transaction logic
│   │   └── masstree/   # Masstree storage engine
│   ├── deptran/        # Transaction protocols
│   │   ├── paxos/      # Paxos consensus
│   │   └── rcc/        # RCC protocol
│   └── rrr/            # RPC framework
├── config/             # Configuration files
│   ├── 1c1s1p.yml      # 1 client, 1 shard, 1 partition
│   └── ...
├── examples/           # Example scripts
│   ├── simplePaxos.sh
│   ├── test_2shard_replication.sh
│   └── test_shard_fault_tolerance.sh
└── ci/                 # CI/test scripts
    └── ci.sh           # Main test runner
```

**Key files to explore**:
- `src/mako/mako.cc` - Server initialization and main loop
- `src/mako/Transaction.cc` - Transaction execution logic
- `src/deptran/paxos/` - Paxos consensus implementation
- `config/*.yml` - Configuration examples

## Next Steps

### For Users:
1. Read [Key Concepts](concepts.md) to understand Mako's architecture
2. Review [Configuration Reference](config.md) for deployment options
3. Try [EC2 Deployment](ec2.md) for distributed setup

### For Developers:
1. Explore [Development Setup](dev-setup.md)
2. Understand [Coroutines & Reactor Pattern](coroutines_guide.md)
3. Learn about [RustyCpp Memory Safety](rrr-rustycpp-migration-plan.md)

## Getting Help

- **Issues**: Found a bug? [Report it on GitHub](https://github.com/makodb/mako/issues)
- **Questions**: [Ask in Discussions](https://github.com/makodb/mako/discussions)
- **Documentation**: Browse the [full documentation index](index.md)

---

**Congratulations!** 🎉 You've successfully installed and run Mako. Welcome to the world of high-performance distributed transactions!

---

**Next**: [Key Concepts](concepts.md) | [Configuration Reference](config.md) | [Architecture Overview](architecture.md)
