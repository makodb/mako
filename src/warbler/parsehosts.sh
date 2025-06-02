#!/bin/bash

# Initialize an empty array
hosts=()

# Read the file into the array, line by line
while IFS= read -r line; do
    hosts+=("$line")
done < "hosts"

result=result.txt

# Loop through each number in the array
for host in "${hosts[@]}"; do
    # Use a timeout to limit the function execution to 3 seconds
    if timeout 5 bash -c "sshpass -pRolisrolis1! ssh -o StrictHostKeyChecking=no rolis@$host exit"; then
        echo -ne "$host\tOK\t" >> $result
        sshpass -pRolisrolis1! ssh -o StrictHostKeyChecking=no rolis@$host "sudo apt-get --assume-yes install net-tools"
        ifaces=$(sshpass -pRolisrolis1! ssh -o StrictHostKeyChecking=no rolis@$host "ifconfig | grep 'inet 172.' | awk '{print \$2}' | paste -sd '\t'")
        hostname=$(sshpass -pRolisrolis1! ssh -o StrictHostKeyChecking=no rolis@$host "hostname")
        echo -ne "$ifaces\t" >> $result
        echo -e "$hostname" >> $result
    else
        echo -e "$host\ttimeout" >> $result
    fi
done