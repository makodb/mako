
def convert_prefix(prefix=""):
    arr = []
    for e in open("./ips"+prefix,"r").readlines():
        arr.append(e.strip())

   
    partitions=10
    with open('n_partitions', 'r') as file:
        file_contents = file.read()
        partitions = int(file_contents)
        print("using partitions: ", partitions)
    # learner -> leader -> p1 -> p2, e.g, nshards: 6
    # 0-5: learner
    # 6-11: leader
    # 12-17: p1
    # 18-23: p2
    for i in range(partitions):
        data=""
        data+="localhost {ip}\n".format(ip=arr[partitions*1+i])
        data+="p1 {ip}\n".format(ip=arr[partitions*2+i])
        data+="p2 {ip}\n".format(ip=arr[partitions*3+i])
        data+="learner {ip}".format(ip=arr[i])
       
        f = open("shard{sIdx}.config{prefix}".format(sIdx=i,prefix=prefix), "w")
        f.write(data)
        f.close()

if __name__ == "__main__":
    convert_prefix("")
    convert_prefix(".pub")
