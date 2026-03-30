servers=(
  127.0.0.1
)

# kill all leader servers
for i in "${!servers[@]}"
do
  ip=${servers[$i]}
  echo "ssh to reqest to $ip"
  cmd="ps aux|grep -i localhost|awk '{print \$2}'|xargs sudo kill -9; sleep 1;"
  ssh $ip "$cmd" &
done

# Await for a while, to avoid throwing an error on the last forwardToLearner call on the p1 datacenter servers 
sleep 5

# kill all learner servers
servers=(
  127.0.0.1
)
for i in "${!servers[@]}"
do
  ip=${servers[$i]}
  echo "ssh to reqest to $ip"
  cmd="ps aux|grep -i learner|awk '{print \$2}'|xargs sudo kill -9; sleep 1;"
  ssh $ip "$cmd" &
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
