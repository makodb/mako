## Commands
```
sudo ip link set dev enp65s0f0 up
sudo ip link set dev enp65s0f1 up
```

```bash
cd third-party/rusty-cpp
cargo build --release

cd third-party/erpc
bash run.sh

# Enable disk
cmake -S . -B build -DENABLE_BORROW_CHECKING=OFF -DDISABLE_DISK=OFF -DDISK_PATH=/home/weihai/mako/disk_data
cmake --build build -j32

sudo bash ./examples/test_1shard_replication.sh
 - 323521

# Disable disk
cmake -S . -B build -DENABLE_BORROW_CHECKING=OFF -DDISABLE_DISK=ON -DDISK_PATH=/tmp
cmake --build build -j32

sudo bash ./examples/test_1shard_no_replication.sh
 - 482821

sudo bash ./examples/test_1shard_replication.sh
 - 326907

```