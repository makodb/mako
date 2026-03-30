## Mako: Speculative Distributed Transactions with Geo-Replication (OSDI'25)

### How to setup environment step by step
https://aju9mlkupe.feishu.cn/docx/SafudaEdzostF6xFXr2cGgxBnpg?from=from_copylink


### mako OSDI'25 revision
### Different ratio on Microbenchmark
### In the same DC or different DC
1. We run different with ratio [0, 5, 20, 50, 90, 100]
2. We need to disable warmup in benchmarks/tpcc.cc, otherwise too many erpc connections are created!
3. Run examples 
```
bash ~/mako/bash/run_x.sh
```

### Failure recovery for COCO
In COCO implementation, it uses execution-to-completion model like us, no changes. Generally, COCO can be considered as a special case for mako where all vectors are compressed into a single timestamp with different advancing strategies: mako uses two `min` operations to advance watermark more fine-grained than epoch in COCO.

In this implementation, we basically make two signficants changes: (1) Just using a single-timestamp `st`, which presents `Epoch` in COCO; (2) Keep tracking of the `st` from 10ms ago.

If there is a shard failure with a timeout 10seconds with the epoch `e`, other healthy shards are unware of this shard failure (this is our assumption, the same as our failure recovery experiment; and we can't have perfect a detcor, and do an expensive failure recovery frequently in practice), and would still execute transcations in `e`, but just waste of time as the commit unit in COCO is the Epoch, all transactions within the epoch `e` would be abandoned. Or during this 10seconds timeout, other shards would attempt to advance the `epoch`, but fail to do it as the decision to increase `epoch` has to made among all shard servers. In this case, we can know that the entire system would be blocked without any throughput at least 10seconds(heartbeat timeout)+1RTT coordination.

```
rm ./results/*.log
# run this on shard leader 0 to kill the process at the time
ifconfig |grep '172.19.0.33'
ifconfig |grep '172.19.0.33' && cd ~/mako && rm results/*.log && bash kill-coco.sh

# To kill all running servers 
bash batch_op_zoo2.sh async_op "bash ~/mako/kill.sh"

# compile
make paxos
bash compile-coco.sh 4

# run it on zoo-002
bash ../batch-runner.sh 4 1 coco-failure

# get the results
# cd ./results && python2 extractor.py 
# to find the point where the throughput is drop to Zero, please check the line with pattern with "Key: {timestamp}, Value: {Throughput, GoodPut}", and then find the first GoodPut is 0, and then take them off from the figures. 
```