## Install on Windows

Platform: 
  - wsl (we don't use docker as wsl is more convenient)
  - windows 10
  - wsl limit: 16GB RAM, 8 CPUs (you can config via .wslconfig)

```
git clone --recursive https://github.com/makodb/mako.git

bash apt_packages.sh
bash install_rustc.sh

make -j8
```

### Test cases
./ci/ci.sh simpleTransaction OK

./ci/ci.sh simplePaxos OK

./ci/ci.sh shardNoReplication OK

./ci/ci.sh shardNoReplicationErpc --> abort rate too high due to too many CPU

./ci/ci.sh shard1Replication --> consume too many RAM

./ci/ci.sh shard2Replication --> consume too many RAM

./ci/ci.sh shard2ReplicationErpc --> consume too many RAM and too many CPU 

./ci/ci.sh shard1ReplicationSimple OK

./ci/ci.sh shard2ReplicationSimple OK

./ci/ci.sh rocksdbTests OK


## Install on Macbook

Hard to run on arm chip, as eprc depends on amd64 a lot. Please go for windows.
