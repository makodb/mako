#!/bin/bash

PROJECT=$(pwd)

# Update ips_{p1|p2|leader|learner}, ips_{p1|p2|leader|learner}.pub, n_partitions (last line leaves an enter!)

# Generate configurations
cd $PROJECT/bash
python3 convert_ip.py

cd $PROJECT/src/mako/config
python generator.py

cd $PROJECT/config/1leader_2followers
python generator.py

cd $PROJECT
