servers=(
  10.1.0.28
  10.1.0.29
  10.1.0.37
  10.1.0.36
  10.1.0.39
  10.1.0.38
  10.1.0.35
  10.1.0.34
  10.1.0.45
  10.1.0.32
)
for i in "${!servers[@]}"
do
  ip=${servers[$i]}
  thds=28
  echo "ssh to reqest to $ip"
  cmd="ulimit -n 10000;cd mako;bash bash/shard.sh 10 $i $thds 20 localhost > ./results/localhost-u-10-$thds-$i.log 2>&1 &"
  ssh $ip "$cmd" &
  sleep 0.2
done

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
