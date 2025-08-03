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

Config hugepage
```bash
sudo bash -c "echo 2048 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages"
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
```

Config hosts with 127.0.0.1 by default
```bash
# Multi-servers: Update bash/ips_{p1|p2|leader|learner}, bash/ips_{p1|p2|leader|learner}.pub, n_partitions 
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
./run_experiment.py --shards 1 --threads 6 --runtime 30 --ssh-user $USER --dry-run --only-compile
bash experiment_s1_norepl_t6_tpcc_r30s.sh

# run
./run_experiment.py --shards 1 --threads 6 --runtime 30 --ssh-user $USER --dry-run --skip-compile
bash experiment_s1_norepl_t6_tpcc_r30s.sh
# all results are under ./results/*.log

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
```

