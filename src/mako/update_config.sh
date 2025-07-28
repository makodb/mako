cd $HOME/janus

# Update ips_{p1|p2|leader|learner}, ips_{p1|p2|leader|learner}.pub, n_partitions (last line leaves an enter!)

# Generate configurations
cd $HOME/janus/bash
python3 convert_ip.py

cd $HOME/janus/src/mako/config
python generator.py

cd $HOME/janus/config/1leader_2followers
python generator.py

cd $HOME/janus
