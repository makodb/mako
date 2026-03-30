leaders=(
10.1.0.48
10.1.0.49
10.1.0.50
10.1.0.51
10.1.0.53
10.1.0.52
10.1.0.54
10.1.0.55
10.1.0.56
10.1.0.57
)
learners=(
10.1.0.68
10.1.0.69
10.1.0.70
10.1.0.71
10.1.0.72
10.1.0.73
10.1.0.75
10.1.0.74
10.1.0.76
10.1.0.77
)
p1s=(
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
p2s=(
10.1.0.88
10.1.0.89
10.1.0.91
10.1.0.90
10.1.0.92
10.1.0.93
10.1.0.94
10.1.0.95
10.1.0.96
10.1.0.97
)

n_partitions=$1
cmd=$2
leaders=("${leaders[@]:0:$n_partitions}")
learners=("${learners[@]:0:$n_partitions}")
p1s=("${p1s[@]:0:$n_partitions}")
p2s=("${p2s[@]:0:$n_partitions}")
allHosts=( "${leaders[@]}" "${learners[@]}" "${p1s[@]}" "${p2s[@]}" )

leaders_pub=("${leaders_pub[@]:0:$n_partitions}")
learners_pub=("${learners_pub[@]:0:$n_partitions}")
p1s_pub=("${p1s_pub[@]:0:$n_partitions}")
p2s_pub=("${p2s_pub[@]:0:$n_partitions}")
allHosts_pub=( "${leaders_pub[@]}" "${learners_pub[@]}" "${p1s_pub[@]}" "${p2s_pub[@]}" )

#sudo dpdk-devbind --status
#ls /usr/local/lib/libmemcached.so.11
#ls -lh
#ls -lh | wc -l
#sudo dpdk-devbind --status
#ssh-keygen -f "/home/azureuser/.ssh/known_hosts" -R "127.0.0.1"
#sudo systemctl status systemd-resolved
cmd1="bash /home/azureuser/mako/initial.sh" 
cmd2=""
cmd3="bash ~/mako/bash/op.sh kill"
cmd4=""
cmd5="bash ~/nfs.sh"
cmd6="bash ~/mako/nfs-client.sh" # re-mount, for jauns or other new folders
cmd7=""

for i in "${!allHosts[@]}"
do
  host=${allHosts[$i]}

  if [ $cmd == 'init' ]; then
    echo "init host $host"
    ssh $host "$cmd1" &
  elif [ $cmd == 'kill' ]; then
    echo "kill: $host"
    ssh $host "$cmd3" &
    echo ""
  elif [ $cmd == 'mount' ]; then
    if [ $host == '10.1.0.48' ]; then    
          :
    else
      echo "mount: $host"
      ssh $host "$cmd5"
    fi 
  elif [ $cmd == 'remount' ]; then
    if [ $host == '10.1.0.48' ]; then    
          :
    else
      echo "remount: $host"
      ssh $host "$cmd6"
    fi 
  elif [ $cmd == 'cmd' ]; then
    echo "cmd: $host, $3"
    ssh $host "$3"
  else
	  :
  fi 


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
