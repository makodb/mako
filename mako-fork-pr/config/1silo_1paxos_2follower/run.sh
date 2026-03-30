# ./run.sh "10.1.0.7" "10.1.0.8" "10.1.0.9"
leader=$1
p1=$2
p2=$3
sed -i "s/localhost: 10.1.0.7 /localhost: $leader/g" `grep "localhost: 10.1.0.7" -rl ./*.yml`
sed -i "s/p1: 10.1.0.8/p1: $p1/g" `grep "p1: 10.1.0.8" -rl ./*.yml`
sed -i "s/p2: 10.1.0.9/p2: $p2/g" `grep "p2: 10.1.0.9" -rl ./*.yml`
