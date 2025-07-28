# Mako

## Compile (Debian 12)

Recursive clone everything 

```bash
$ git clone --recursive [git_repo_url]
```

Install all dependencies

```
$ bash apt_packages.sh
```

```bash
$ cd third-party/erpc
$ make
```

# Config hosts 
```bash
# Update ips_{p1|p2|leader|learner}, ips_{p1|p2|leader|learner}.pub, n_partitions 
$ bash ./src/mako/update_config.sh 
```

## Experiment Runner

The `run_experiment.py` script automates the compilation and execution of distributed system experiments.

### Setup

The script uses `sshpass` by default for SSH authentication. Set your password as an environment variable:

```bash
export SSHPASS="your_password"
```

### Usage

```bash
# Basic experiment (compile + run)
./run_experiment.py --shards 3 --threads 6 --runtime 30

# With replication (Paxos)
./run_experiment.py --shards 3 --threads 6 --runtime 30 --replicated

# Microbenchmark instead of TPC-C
./run_experiment.py --shards 3 --threads 6 --runtime 30 --micro

# Skip compilation (run experiment only)
./run_experiment.py --shards 3 --threads 6 --runtime 30 --replicated --skip-compile

# Dry run (generate script without executing)
./run_experiment.py --shards 3 --threads 6 --runtime 30 --replicated --dry-run

# Cleanup only (kill processes and remove logs)
./run_experiment.py --shards 3 --threads 6 --runtime 30 --cleanup-only

# Disable sshpass (use regular SSH keys)
./run_experiment.py --shards 3 --threads 6 --runtime 30 --replicated --no-sshpass
```

### Parameters

- `--shards N`: Number of shards (default: 3)
- `--threads N`: Number of worker threads per shard (default: 6) 
- `--runtime N`: Runtime in seconds (default: 30)
- `--replicated`: Enable replication with Paxos
- `--micro`: Use microbenchmark instead of TPC-C
- `--skip-compile`: Skip compilation phase and only run experiment
- `--dry-run`: Generate bash script without executing
- `--cleanup-only`: Only cleanup processes and logs
- `--ssh-user USER`: SSH username (default: weihai)
- `--no-sshpass`: Disable sshpass and use regular SSH keys

### Output

The script generates a bash file with all commands executed, named with experiment parameters:
- Example: `experiment_s3_repl_t6_tpcc_r30s.sh`
- Contains descriptive header with all configuration details
- Results are logged to `./results/{role}-{shard}.log`

### TODOs
 - TODO replace this script with standard cmake build
 - Manually compile eRPC (TODO: change to auto build)


## Notes
1. we use nfs to sync some data, e.g., we use nfs to control all worker threads execute at the roughly same time (we used memcached in the past and removed this external dependencies)
2. for erpc, we add pure ethernet support so that you can use widely adopted sockets
```
cd ./third-party/erpc
make clean
cmake . -DTRANSPORT=fake -DROCE=off -DPERF=off
make -j10

cd ~/janus
echo "eth" > env.txt
```
