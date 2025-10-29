learners=(
20.161.16.46
128.24.105.139
13.68.88.232
172.177.198.129
128.24.82.21
68.154.27.17
9.169.169.205
68.154.26.205
13.68.93.159
128.24.96.2
)
leaders=(
68.154.26.160
128.24.106.135
20.57.41.240
68.154.27.5
68.154.26.213
20.36.215.115
20.161.16.57
68.154.25.39
68.154.26.173
20.57.124.158
)
p1s=(
172.203.56.162
13.68.93.157
172.203.56.172
172.203.57.23
13.68.93.104
20.161.16.68
20.57.123.60
52.232.181.213
68.154.26.219
128.24.104.43
)
p2s=(
135.119.129.26
68.154.26.152
13.68.95.140
172.200.138.1
68.154.26.126
128.24.106.62
20.49.21.236
20.57.43.12
172.203.57.58
128.24.105.142
)

n_partitions=$1
isreplicated=$2
ver=${3:-0}
leaders=("${leaders[@]:0:$n_partitions}")
learners=("${learners[@]:0:$n_partitions}")
p1s=("${p1s[@]:0:$n_partitions}")
p2s=("${p2s[@]:0:$n_partitions}")

passwd="Rolisrolis1!"
needToRuns=(1 4 8 12 16 20 24)
needToRuns=(24)
for thds in "${needToRuns[@]}"
do

runtime=30
for i in "${!leaders[@]}"
do
  ip=${leaders[$i]}
  cmd="ulimit -n 20000;cd mako;bash bash/shard.sh $n_partitions $i $thds $runtime localhost > ./results/exp-localhost-v14-$n_partitions-$thds-$i-$ver.log 2>&1 &"
  echo "ssh to reqest to $ip, $cmd"
  sshpass -p$passwd ssh -o StrictHostKeyChecking=no rolis@$ip "$cmd" &
  sleep 0.3
done

if [ "$isreplicated" -eq 1 ]; then
    for i in "${!learners[@]}"
    do
    ip=${learners[$i]}
    cmd="ulimit -n 20000;cd mako;bash bash/shard.sh $n_partitions $i $thds $runtime learner > ./results/exp-learner-v14-$n_partitions-$thds-$i-$ver.log 2>&1 &"
    echo "ssh to reqest to $ip, $cmd"
    sshpass -p$passwd ssh -o StrictHostKeyChecking=no rolis@$ip "$cmd" &
    sleep 0.3
    done

    for i in "${!p1s[@]}"
    do
    ip=${p1s[$i]}
    cmd="ulimit -n 20000;cd mako;bash bash/shard.sh $n_partitions $i $thds $runtime p1 > ./results/exp-p1-v14-$n_partitions-$thds-$i-$ver.log 2>&1 &"
    echo "ssh to reqest to $ip, $cmd"
    sshpass -p$passwd ssh -o StrictHostKeyChecking=no rolis@$ip "$cmd" &
    sleep 0.3
    done

    for i in "${!p2s[@]}"
    do
    ip=${p2s[$i]}
    cmd="ulimit -n 20000;cd mako;bash bash/shard.sh $n_partitions $i $thds $runtime p2 > ./results/exp-p2-v14-$n_partitions-$thds-$i-$ver.log 2>&1 &"
    echo "ssh to reqest to $ip, $cmd"
    sshpass -p$passwd ssh -o StrictHostKeyChecking=no rolis@$ip "$cmd" &
    sleep 0.3
    done
else
    echo "not replicated"
fi




echo "127.0.0.1" "echo 'waiting...'"

echo "Wait for jobs..."
FAIL=0
for job in `jobs -p`
do
    wait $job || let "FAIL+=1"
done

if [ "$FAIL" == "0" ];
then
    echo "YAY!"
else
    echo "FAIL! ($FAIL)"
fi

let sTime=$thds+20
sleep $sTime
#nnn=$thds
#cat ~/mako/results/localhost-u-$n_partitions-$nnn-*.log | ag 'NewOrder_remote_commit_latency'
#cat ~/mako/results/localhost-u-$n_partitions-$nnn-*.log | ag 'agg_throughput'
#cat ~/mako/results/localhost-u-$n_partitions-$nnn-*.log | ag 'agg_throughput' | wc -l
#cat ~/mako/results/localhost-u-$n_partitions-$nnn-*.log | ag 'agg_throughput'| awk '{sum += $2} END {print sum}'
##echo ""

done
