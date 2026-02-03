# Mako Documentation

Welcome to the "Mako-book".

---

## Documentation Structure

This documentation is organized into two main sections:

1. **[User Manual](#user-manual)** - For users who want to deploy, configure, and use Mako
2. **[Developer Manual](#developer-manual)** - For developers who want to understand Mako's internals or contribute

---

# User Manual

## Introduction

### Overview
- **[What is Mako?](introduction.md)** - Understanding Mako's purpose and capabilities
- **[Key Concepts](concepts.md)** - Fundamental concepts: shards, replicas, transactions, speculative execution
- **[Comparison with Other Systems](comparison.md)** - How Mako differs from MongoDB, PostgreSQL, Spanner, etc.
- **[Use Cases](use-cases.md)** - When to use Mako vs. other databases

## Getting Started

### Installation & Setup
- **[Installation Guide](install.md)** - Step-by-step installation instructions for Debian/Ubuntu
- **[Build Instructions](build.md)** - Building on different systems and configurations
- **[Docker Deployment](DOCKER_BUILD.md)** - Running Mako in Docker containers
  - [Docker Build Success Guide](DOCKER_BUILD_SUCCESS.md)
  - [Docker Verification](DOCKER_VERIFICATION.md)
- **[Quick Start Tutorial](quickstart.md)** - Get up and running in 10 minutes

### Configuration
- **[Configuration Reference](config.md)** - Complete YAML configuration options
- **[Cluster Topology](topology.md)** - Defining shards, replicas, and datacenters
- **[Network Configuration](network.md)** - Setting up networking for distributed deployments
- **[Storage Configuration](storage-config.md)** - Masstree vs RocksDB persistence options

### Deployment
- **[Local Deployment](run.md)** - Running distributed tests on localhost
- **[EC2 Deployment](ec2.md)** - Deploying Mako on AWS EC2
- **[Multi-Datacenter Setup](multi-dc.md)** - Configuring geo-replication across datacenters
- **[Production Deployment Checklist](production.md)** - Best practices for production
- **[Capacity Planning](capacity-planning.md)** - Sizing your cluster

## Working with Data

### CRUD Operations
- **[Insert Data](crud/insert.md)** - Creating records in Mako
- **[Query Data](crud/query.md)** - Reading and retrieving data
- **[Update Data](crud/update.md)** - Modifying existing records
- **[Delete Data](crud/delete.md)** - Removing records

### Transactions
- **[Transaction Basics](transactions/basics.md)** - Understanding distributed transactions
- **[ACID Guarantees](transactions/acid.md)** - Consistency and isolation levels
- **[Read-Write Transactions](transactions/read-write.md)** - Mixed read/write operations
- **[Transaction Best Practices](transactions/best-practices.md)** - Optimizing transaction performance
- **[Handling Conflicts](transactions/conflicts.md)** - Understanding and resolving conflicts

### Data Modeling
- **[Schema Design](data-modeling/schema.md)** - Designing your data model for Mako
- **[Key Design](data-modeling/keys.md)** - Choosing effective partition keys
- **[Sharding Strategy](data-modeling/sharding.md)** - How data is distributed across shards
- **[Data Types](data-modeling/types.md)** - Supported data types and serialization

## Client Interfaces

### Language Drivers
- **[C++ Client](clients/cpp.md)** - Native C++ client library
- **[Python Client](clients/python.md)** - Python bindings and usage
- **[Java Client](clients/java.md)** - Java client library
- **[Go Client](clients/go.md)** - Go client library

### Compatibility Layers
- **[RocksDB-Compatible Interface](clients/rocksdb-interface.md)** - Using Mako as a distributed RocksDB
- **[Redis-Compatible Interface](clients/redis-interface.md)** - Using Mako with Redis API
- **[Native Client API](clients/native-api.md)** - Direct Mako protocol usage

### Connection Management
- **[Connection Pooling](clients/connection-pooling.md)** - Managing client connections
- **[Client Configuration](clients/configuration.md)** - Configuring client behavior
- **[Error Handling](clients/error-handling.md)** - Handling errors and retries

## Sharding & Distribution

### Sharding Concepts
- **[Sharding Overview](sharding/overview.md)** - How Mako partitions data
- **[Shard Keys](sharding/shard-keys.md)** - Choosing and managing shard keys
- **[Shard Management](sharding/management.md)** - Adding and removing shards
- **[Balancing](sharding/balancing.md)** - Data distribution and rebalancing

### Geo-Replication
- **[Replication Overview](replication/overview.md)** - Understanding geo-replication
- **[Replica Configuration](replication/configuration.md)** - Setting up replicas
- **[Consistency Levels](replication/consistency.md)** - Configuring consistency guarantees
- **[Failover & Recovery](replication/failover.md)** - Handling datacenter failures

## Administration

### Cluster Operations
- **[Starting & Stopping](admin/start-stop.md)** - Cluster lifecycle management
- **[Monitoring & Metrics](admin/monitoring.md)** - Tracking system performance and health
- **[Log Management](admin/logs.md)** - Understanding and analyzing logs
- **[Alerts & Notifications](admin/alerts.md)** - Setting up monitoring alerts

### Maintenance
- **[Backup & Recovery](admin/backup.md)** - Data backup and disaster recovery procedures
- **[Upgrades](admin/upgrades.md)** - Upgrading Mako versions
- **[Scaling](admin/scaling.md)** - Horizontal and vertical scaling
- **[Node Management](admin/nodes.md)** - Adding and removing nodes

### Storage Management
- **[Disk Usage](admin/disk-usage.md)** - Managing storage capacity
- **[Data Persistence](admin/persistence.md)** - RocksDB persistence layer
- **[Compaction](admin/compaction.md)** - Background compaction processes
- **[Data Retention](admin/retention.md)** - Data lifecycle policies

## Security

### Authentication & Authorization
- **[Authentication](security/authentication.md)** - User authentication mechanisms
- **[Authorization](security/authorization.md)** - Access control and permissions
- **[User Management](security/users.md)** - Creating and managing users
- **[Role-Based Access](security/rbac.md)** - Role-based access control

### Network Security
- **[TLS/SSL Configuration](security/tls.md)** - Encrypting network traffic
- **[Firewall Setup](security/firewall.md)** - Network security configuration
- **[VPC Deployment](security/vpc.md)** - Deploying in private networks

### Auditing & Compliance
- **[Audit Logging](security/audit.md)** - Tracking access and operations
- **[Compliance](security/compliance.md)** - Meeting regulatory requirements

## Performance

### Optimization
- **[Benchmarking Guide](performance/benchmarks.md)** - Running TPC-C, TPC-A, and custom benchmarks
- **[Performance Tuning](performance/tuning.md)** - Optimizing throughput and latency
- **[Profiling](profile.md)** - CPU profiling and performance analysis
- **[Query Optimization](performance/query-optimization.md)** - Improving query performance

### Monitoring
- **[Metrics Collection](performance/metrics.md)** - Key performance indicators
- **[Performance Analysis](performance/analysis.md)** - Diagnosing performance issues
- **[Resource Usage](performance/resources.md)** - CPU, memory, network, disk utilization

## Migration & Integration

### Migration Guides
- **[Migrating from RocksDB](migration/from-rocksdb.md)** - Moving from single-node RocksDB
- **[Migrating from Redis](migration/from-redis.md)** - Transitioning from Redis
- **[Migrating from MongoDB](migration/from-mongodb.md)** - Moving from MongoDB
- **[Migrating from PostgreSQL](migration/from-postgresql.md)** - Transitioning from PostgreSQL

### Integration
- **[Application Integration](integration/applications.md)** - Integrating Mako into applications
- **[ETL Pipelines](integration/etl.md)** - Extract, transform, load workflows
- **[Backup Integration](integration/backup.md)** - Integrating with backup systems

## Reference

### Command Reference
- **[Server Commands](reference/server-commands.md)** - Server management commands
- **[Client Commands](reference/client-commands.md)** - Client operations reference
- **[Configuration Options](reference/config-options.md)** - Complete configuration reference
- **[Error Codes](reference/error-codes.md)** - Error code reference

### Limits & Thresholds
- **[System Limits](reference/limits.md)** - Maximum values and constraints
- **[Default Settings](reference/defaults.md)** - Default configuration values
- **[Glossary](reference/glossary.md)** - Terminology and definitions

## Troubleshooting

### Common Issues
- **[Troubleshooting Guide](troubleshooting/guide.md)** - Solutions to frequent problems
- **[Connection Issues](troubleshooting/connection.md)** - Debugging connectivity problems
- **[Performance Issues](troubleshooting/performance.md)** - Diagnosing slow performance
- **[Replication Issues](troubleshooting/replication.md)** - Geo-replication problems

### Debugging
- **[Debugging Guide](troubleshooting/debugging.md)** - Tools and techniques for debugging
- **[Log Analysis](troubleshooting/logs.md)** - Understanding log messages
- **[Core Dumps](troubleshooting/core-dumps.md)** - Analyzing crashes

### FAQ
- **[General FAQ](faq/general.md)** - Frequently asked questions
- **[Performance FAQ](faq/performance.md)** - Performance-related questions
- **[Deployment FAQ](faq/deployment.md)** - Deployment and operations questions

---

# Developer Manual

## Architecture & Design

### System Overview
- **[Architecture Overview](architecture.md)** - High-level system architecture
- **[Speculative 2PC Protocol](speculative-2pc.md)** - Core innovation explained
- **[Transaction Execution](transaction-execution.md)** - How transactions are processed
- **[Replication & Consensus](replication.md)** - Geo-replication and Paxos implementation

### Core Components
- **[Transaction Coordinator](coordinator.md)** - Coordinating distributed transactions
- **[Transaction Scheduler](scheduler.md)** - Scheduling and execution logic
- **[Storage Engine - Masstree](masstree.md)** - In-memory index structure
- **[Disk Persistence - RocksDB](disk_persistence.md)** - Asynchronous persistence layer
- **[RPC Framework - RRR](rrr-rpc.md)** - Custom RPC implementation

## Development Guide

### Getting Started
- **[Development Setup](dev-setup.md)** - Setting up development environment
- **[Code Organization](code-organization.md)** - Understanding the codebase structure
- **[Build System](build-system.md)** - CMake, Makefile, and build configurations
- **[Testing Framework](testing.md)** - Unit tests, integration tests, and CI

### Core Programming Concepts
- **[Coroutines & Reactor Pattern](coroutines_guide.md)** - Understanding RRR's async model
- **[Memory Management](memory-management.md)** - RustyCpp smart pointers and safety
  - [RustyCpp Migration Plan](rrr-rustycpp-migration-plan.md)
  - [RRR Safety Roadmap](RRR_SAFETY_ROADMAP.md)
- **[Thread Model](threading.md)** - Multi-threading and concurrency patterns
- **[Event System](event-system.md)** - Event-driven programming in RRR

### Implementation Details
- **[Transaction Protocols](protocols.md)** - Implementing concurrency control protocols
  - 2PL (Two-Phase Locking)
  - OCC (Optimistic Concurrency Control)
  - Mako's Speculative 2PC
  - Paxos & Raft
- **[Workload Implementation](workloads.md)** - Adding new benchmarks
- **[Table Allocation](table-allocation.md)** - Memory allocation strategies
- **[RPC Design](rpc-design.md)** - Adding new RPC services
  - [RPC Benchmarking](rpc-benchmark.md)

### Advanced Topics
- **[Network Transport Layers](transports.md)** - TCP, DPDK, RDMA, eRPC
- **[Crash Recovery](recovery.md)** - Fault tolerance and recovery mechanisms
- **[Dependency Tracking](dependency-tracking.md)** - Managing transaction dependencies
- **[Client Types](Open-loop-vs-Closed-loop-clients.md)** - Open-loop vs closed-loop clients

## Contributing

### Development Workflow
- **[Contributing Guide](../CONTRIBUTING.md)** - How to contribute to Mako
- **[Code Review Process](code-review.md)** - Submitting and reviewing pull requests
- **[Coding Standards](coding-standards.md)** - C++ style guide and best practices
- **[Writing Tests](writing-tests.md)** - Test writing guidelines
- **[Documentation Guide](documentation.md)** - Contributing to documentation

### CI/CD
- **[Continuous Integration](ci.md)** - GitHub Actions and automated testing
- **[Release Process](release.md)** - Version management and releases

## Reference

### API Reference
- **[Client API Reference](api/client.md)** - Complete client API documentation
- **[Server API Reference](api/server.md)** - Server-side API documentation
- **[Configuration Schema](api/config-schema.md)** - YAML configuration schema

### Benchmarks
- **[TPC-C Implementation](bench/tpcc.md)** - TPC-C benchmark details
- **[TPC-A Implementation](bench/tpca.md)** - TPC-A benchmark details
- **[Read-Write Workload](bench/rw.md)** - RW benchmark details
- **[Micro-benchmarks](bench/micro.md)** - Micro-benchmark suite

### Tools & Utilities
- **[Plotting Results](plot.md)** - Generating graphs from benchmark data
- **[Log Analysis](log-analysis.md)** - Analyzing system logs
- **[Performance Tools](perf-tools.md)** - gperf, perf, and other profiling tools

---

## Additional Resources

### Papers & Publications
- [OSDI'25 Paper](https://www.usenix.org/conference/osdi25/presentation/shen-weihai) - Mako: Speculative Distributed Transactions with Geo-Replication

### Community
- [GitHub Repository](https://github.com/makodb/mako)
- [Issue Tracker](https://github.com/makodb/mako/issues)
- [Discussions](https://github.com/makodb/mako/discussions)

---

## Quick Navigation

### I want to...
- **Get started quickly** → [Quick Start Tutorial](quickstart.md) → [CRUD Operations](crud/insert.md)
- **Use Mako as a distributed database** → [Installation Guide](install.md) → [Configuration Reference](config.md) → [RocksDB Interface](clients/rocksdb-interface.md)
- **Migrate from another database** → See [Migration Guides](migration/from-rocksdb.md)
- **Deploy Mako on AWS** → [EC2 Deployment](ec2.md) → [Production Deployment](production.md)
- **Run benchmarks** → [Benchmarking Guide](performance/benchmarks.md) → [Performance Tuning](performance/tuning.md)
- **Set up geo-replication** → [Multi-Datacenter Setup](multi-dc.md) → [Replication Overview](replication/overview.md)
- **Understand transactions** → [Transaction Basics](transactions/basics.md) → [ACID Guarantees](transactions/acid.md)
- **Optimize performance** → [Performance Tuning](performance/tuning.md) → [Query Optimization](performance/query-optimization.md)
- **Secure my deployment** → [Authentication](security/authentication.md) → [TLS/SSL Configuration](security/tls.md)
- **Monitor and maintain** → [Monitoring & Metrics](admin/monitoring.md) → [Backup & Recovery](admin/backup.md)
- **Understand how Mako works** → [Architecture Overview](architecture.md) → [Speculative 2PC Protocol](speculative-2pc.md)
- **Add a new transaction protocol** → [Transaction Protocols](protocols.md) → [Development Setup](dev-setup.md)
- **Contribute code** → [Development Setup](dev-setup.md) → [Contributing Guide](../CONTRIBUTING.md)
- **Debug an issue** → [Troubleshooting Guide](troubleshooting/guide.md) → [Debugging Guide](troubleshooting/debugging.md)

---

## Documentation Status

Legend: ✅ Complete | 🚧 In Progress | 📝 Planned

### User Manual Status

**Existing Documentation:**
- ✅ Build Instructions (build.md)
- ✅ Running Tests (run.md)
- ✅ EC2 Deployment (ec2.md)
- ✅ Profiling (profile.md)
- ✅ Docker Build (DOCKER_BUILD.md)

**Introduction (📝 Planned):**
- What is Mako?, Key Concepts, Comparison, Use Cases

**Getting Started (📝 Planned):**
- Installation Guide, Quick Start Tutorial, Storage Configuration, Capacity Planning

**Working with Data (📝 Planned):**
- CRUD Operations (Insert, Query, Update, Delete)
- Transactions (Basics, ACID, Read-Write, Best Practices, Conflicts)
- Data Modeling (Schema Design, Key Design, Sharding Strategy, Data Types)

**Client Interfaces (📝 Planned):**
- Language Drivers (C++, Python, Java, Go)
- Compatibility Layers (RocksDB, Redis, Native API)
- Connection Management (Pooling, Configuration, Error Handling)

**Sharding & Distribution (📝 Planned):**
- Sharding (Overview, Shard Keys, Management, Balancing)
- Geo-Replication (Overview, Configuration, Consistency, Failover)

**Administration (📝 Planned):**
- Cluster Operations (Start/Stop, Monitoring, Logs, Alerts)
- Maintenance (Backup, Upgrades, Scaling, Node Management)
- Storage Management (Disk Usage, Persistence, Compaction, Retention)

**Security (📝 Planned):**
- Authentication & Authorization (Auth, Users, RBAC)
- Network Security (TLS/SSL, Firewall, VPC)
- Auditing & Compliance

**Performance (📝 Planned):**
- Optimization (Benchmarking, Tuning, Query Optimization)
- Monitoring (Metrics, Analysis, Resource Usage)

**Migration & Integration (📝 Planned):**
- Migration Guides (from RocksDB, Redis, MongoDB, PostgreSQL)
- Integration (Applications, ETL, Backup)

**Reference (📝 Planned):**
- Command Reference (Server, Client, Configuration, Error Codes)
- Limits & Thresholds (System Limits, Defaults, Glossary)

**Troubleshooting (📝 Planned):**
- Common Issues (Guide, Connection, Performance, Replication)
- Debugging (Guide, Log Analysis, Core Dumps)
- FAQ (General, Performance, Deployment)

### Developer Manual Status
- ✅ Coroutines Guide
- ✅ Disk Persistence
- ✅ RRR RPC Framework
- ✅ RustyCpp Migration Plan
- ✅ RRR Safety Roadmap
- ✅ Table Allocation
- ✅ RPC Benchmarking
- ✅ Client Types
- ✅ Plotting
- 📝 Architecture Overview
- 📝 Speculative 2PC Protocol
- 📝 Transaction Execution
- 📝 Replication & Consensus
- 📝 Core Components
- 📝 Development Setup
- 📝 Code Organization
- 📝 Build System
- 📝 Testing Framework
- 📝 Memory Management
- 📝 Thread Model
- 📝 Event System
- 📝 Transaction Protocols
- 📝 Workload Implementation
- 📝 RPC Design
- 📝 Network Transports
- 📝 Crash Recovery
- 📝 Dependency Tracking
- 📝 Contributing Guide
- 📝 Code Review Process
- 📝 Coding Standards
- 📝 Writing Tests
- 📝 CI/CD
- 📝 API Reference

---

*Last updated: October 2024*
