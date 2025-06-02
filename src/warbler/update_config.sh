cd $HOME/srolis

# Update ips, ips.pub, n_partitions (last line leaves an enter!)

# Generate configurations
cd $HOME/srolis/bash
python3 convert_ip.py

cd $HOME/srolis/config
python generator.py

cd $HOME/srolis/third-party/paxos/config/1leader_2followers
python generator.py

cd $HOME/srolis
