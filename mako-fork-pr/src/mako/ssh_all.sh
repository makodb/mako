learners_pub=(
172.177.160.160
172.177.162.91
172.177.161.213
172.177.161.6
172.177.161.192
172.177.161.247
172.177.165.66
172.177.160.246
172.177.162.2
172.177.161.235
)
leaders_pub=(
172.177.163.33
172.177.161.119
172.177.163.248
172.177.162.9
172.177.162.7
172.177.164.176
172.177.162.213
172.177.162.209
172.177.160.236
172.177.160.150
)
p1s_pub=(
172.177.162.228
172.172.60.217
172.177.161.219
172.172.60.91
172.177.161.166
172.177.162.160
172.177.163.78
172.177.163.184
172.177.162.19
172.177.161.66
)
p2s_pub=(
172.172.60.70
172.177.163.235
172.177.164.106
172.177.163.170
172.177.165.5
172.177.164.129
172.177.163.52
172.177.164.246
172.177.164.14
172.177.163.228
)
learners=(
172.16.0.72
172.16.0.40
172.16.0.13
172.16.0.70
172.16.0.18
172.16.0.34
172.16.0.26
172.16.0.8
172.16.0.19
172.16.0.16
)
leaders=(
172.16.0.50
172.16.0.4
172.16.0.56
172.16.0.32
172.16.0.35
172.16.0.30
172.16.0.12
172.16.0.44
172.16.0.81
172.16.0.80
)
p1s=(
172.16.0.45
172.16.0.78
172.16.0.17
172.16.0.76
172.16.0.74
172.16.0.42
172.16.0.53
172.16.0.62
172.16.0.33
172.16.0.10
)
p2s=(
172.16.0.6
172.16.0.63
172.16.0.68
172.16.0.57
172.16.0.27
172.16.0.58
172.16.0.43
172.16.0.20
172.16.0.64
172.16.0.52
)

n_partitions=$1
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

#cmd="ssh-keyscan $host >> $HOME/.ssh/known_hosts"
for i in "${!allHosts[@]}"
do
  host=${allHosts[$i]}
  echo "ssh to reqest to $host"
  cmd="echo 'connected'"
  sshpass -pRolisrolis1! ssh -o StrictHostKeyChecking=no rolis@$host $cmd
done

for i in "${!allHosts_pub[@]}"
do
  host=${allHosts_pub[$i]}
  echo "ssh to reqest to $host"
  cmd="echo 'connected'"
  sshpass -pRolisrolis1! ssh -o StrictHostKeyChecking=no rolis@$host $cmd
done
