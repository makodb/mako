log_monitor="./results/exp-localhost-v14-4-24-0-coco-failure.log"
functionKillShard() {
   echo "start to check if $log_monitor exists" 
   while [ ! -f "$log_monitor" ]; do
    sleep 0.1
   done
   echo "start monitor $log_monitor"

   while ! grep -q "30000 ms" $log_monitor; do
       sleep 0.1
   done

   echo "kill is processed"
   ./kill.sh
   sleep 15
   echo "done"
}

functionKillShard
