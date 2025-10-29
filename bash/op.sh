#!/usr/bin/bash
repos="mako"  # repos name, default
workdir="~"  # we default put our repos under the root

# util
wait_jobs() {
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
}

# East US 2
ipLeaders=(
  10.1.0.11  # nfs server
  10.1.0.10
  10.1.0.13
  10.1.0.12
  10.1.0.14
  10.1.0.15
)
# East US 2
ipLearners=(
  10.1.0.4
  10.1.0.5
  10.1.0.8
  10.1.0.7
  10.1.0.6
  10.1.0.9
)
# West US 2
ipP1=(
  10.2.0.4  # nfs server
  10.2.0.15
  10.2.0.6
  10.2.0.5
  10.2.0.8
  10.2.0.7
)
# West US 3
ipP2=(
  10.11.0.4  # nfs server
  10.11.0.14
  10.11.0.6
  10.11.0.5
  10.11.0.7
  10.11.0.8
)

allHosts=( "${ipLeaders[@]}" "${ipLearners[@]}" "${ipP1[@]}" "${ipP2[@]}" )
allHosts=( 127.0.0.1 )

cmd1="ps aux|grep -i shard|awk '{print \$2}'|xargs sudo kill -9; sleep 1;"
cmd2="ps aux|grep -i dbtest|awk '{print \$2}'|xargs sudo kill -9; sleep 1;"

for host in ${allHosts[@]}
do
  if [ $1 == 'kill' ]; then
    echo "on $host cmd: $cmd1"
    eval $cmd1
    ssh $host "$cmd1" &
    ssh $host "$cmd2" &
  fi
done


wait_jobs
