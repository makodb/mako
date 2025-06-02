# check resolution
# sudo systemctl start systemd-resolved
# sleep 1

ulimit -n 20000

# path=$(pwd)
# export RTE_SDK=$path/dpdk-stable-19.11.5
# export RTE_TARGET=x86_64-native-linuxapp-gcc

# TCP reset
sudo sysctl -w net.core.rmem_max=8388608
sudo sysctl -w net.core.wmem_max=8388608
sudo sysctl -w net.ipv4.tcp_rmem="4096 65535 8388608"
sudo sysctl -w net.ipv4.tcp_wmem="4096 65535 8388608"
sudo sysctl -w net.ipv4.route.flush=1
sudo sysctl -w net.ipv4.tcp_window_scaling=8

skill memcached
ps aux|grep -i memcached|awk '{print $2}'|xargs sudo kill -9
sleep 1
# or /usr/bin/memcached
nohup /usr/local/bin/memcached -m 64 -U 6001 -u memcache &
sleep 1
nohup /usr/local/bin/memcached -m 64 -U 6002 -u memcache &
sleep 1
nohup /usr/local/bin/memcached -m 64 -U 6003 -u memcache &
# echo "flush_all" | nc -q 2 localhost 6001
# echo "flush_all" | nc -q 2 localhost 6002
# echo "flush_all" | nc -q 2 localhost 6003

# route table is not static on the Azure platform
# for v5, force the traffic to route to eth0 instead of eth1
# sudo ifmetric eth1 1

# for v5, have to reorder the DPDK driver version
echo "start python script..."
python <<EOF
import subprocess
import re
import os

dpdk_drivers = subprocess.check_output("sudo dpdk-devbind --status | head -7", shell=True);

# parse the drivers 
drivers = []
for d in dpdk_drivers.split("\n"):
  if "if=" in d:
    items = d.strip().split(" ")
    driver = [items[0]]    
    for item in items[1:]:
      if "if=" in item:
        driver.append(item.replace("if=",""))
    drivers.append(driver)

# the first one has to be conform to "enxxxs2"
exist=False
pattern="en.*s2"
for d in drivers:
  if re.match(pattern, d[1]):
    exist=True

if not exist:
  print("can not find the correct driver, all drivers are:", drivers)
else:
  # unbind all other drivers
  for d in drivers:
      if not re.match(pattern, d[1]):
        cmd = "sudo dpdk-devbind -u {u}".format(u=d[0])
        print(cmd)
        os.system(cmd)
EOF
echo "DONE"
