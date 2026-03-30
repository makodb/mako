ps aux|grep -i shard|awk '{print $2}'|xargs sudo kill -9;
ps aux|grep -i dbtest|awk '{print $2}'|xargs sudo kill -9;
ps aux|grep -i paxos|awk '{print $2}'|xargs sudo kill -9;
ps aux|grep -i deptran_server|awk '{print $2}'|xargs sudo kill -9;
ps aux|grep -i paxos_async_commit_test|awk '{print $2}'|xargs sudo kill -9; sleep 1;
