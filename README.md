# Mako

## Compile (Debian 12 / Ubuntu22.04)

Recursive clone everything 

```bash
git clone --recursive https://github.com/makodb/mako.git
git checkout mako-dev
```

Install all dependencies

```
bash apt_packages.sh
```

Install eRPC with socket transport
```bash
cd third-party/erpc
make clean
cmake . -DTRANSPORT=fake -DROCE=off -DPERF=off
make -j$(nproc)
cd ../..
```

Config hosts
```bash
# if Multi-servers: Update bash/ips_{p1|p2|leader|learner}, bash/ips_{p1|p2|leader|learner}.pub, n_partitions 
bash ./src/mako/update_config.sh 
```

## Experiment Runner

The `run_experiment.py` script automates the compilation and execution of distributed system experiments.

### Setup

The script uses `sshpass` by default for SSH authentication. Set your password as an environment variable:

```bash
export SSHPASS="your_password"
```

Make sure can you ssh your all servers with each other without password

### Compile and Run

```bash
# compile
# If the error <command-line>: fatal error: src/mako/masstree/config.h appears on the first run, rerun the script.
./run_experiment.py --shards 1 --threads 6 --runtime 30 --ssh-user $USER --dry-run --only-compile
bash experiment_s1_norepl_t6_tpcc_r30s_compile-only.sh

# run
# all results are under ./results/*.log
./run_experiment.py --shards 1 --threads 6 --runtime 30 --ssh-user $USER --dry-run --skip-compile
bash experiment_s1_norepl_t6_tpcc_r30s_no-compile.sh
sleep 1
tail -f *./results/*.log

# kill
./run_experiment.py --shards 1 --threads 6 --runtime 30 --ssh-user $USER --cleanup-only

# more help
./run_experiment.py --help
```

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

sudo for bash/shard.sh is not rquired for socket-based transport
```

