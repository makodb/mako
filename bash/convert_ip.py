
def convert_prefix(prefix=""):
    p1 = []
    for e in open("./ips_p1"+prefix,"r").readlines():
        p1.append(e.strip())

    p2 = []
    for e in open("./ips_p2"+prefix,"r").readlines():
        p2.append(e.strip())

    leader = []
    for e in open("./ips_leader"+prefix,"r").readlines():
        leader.append(e.strip())

    learner = []
    for e in open("./ips_learner"+prefix,"r").readlines():
        learner.append(e.strip())

   
    partitions=10
    with open('n_partitions', 'r') as file:
        file_contents = file.read()
        partitions = int(file_contents)
        print("using partitions: ", partitions)
    
    # Previous:
    #   learner -> leader -> p1 -> p2, e.g, nshards: 6
    #   0-5: learner
    #   6-11: leader
    #   12-17: p1
    #   18-23: p2
    #for i in range(partitions):
    #    data=""
    #    data+="localhost {ip}\n".format(ip=arr[partitions*1+i])
    #    data+="p1 {ip}\n".format(ip=arr[partitions*2+i])
    #    data+="p2 {ip}\n".format(ip=arr[partitions*3+i])
    #    data+="learner {ip}".format(ip=arr[i])
    #   
    #    f = open("shard{sIdx}.config{prefix}".format(sIdx=i,prefix=prefix), "w")
    #    f.write(data)
    #    f.close()

    for i in range(partitions):
        data=""
        data+="localhost {ip}\n".format(ip=leader[i])
        data+="p1 {ip}\n".format(ip=p1[i])
        data+="p2 {ip}\n".format(ip=p2[i])
        data+="learner {ip}".format(ip=learner[i])
       
        f = open("shard{sIdx}.config{prefix}".format(sIdx=i,prefix=prefix), "w")
        f.write(data)
        f.close()

if __name__ == "__main__":
    convert_prefix("")
    convert_prefix(".pub")
