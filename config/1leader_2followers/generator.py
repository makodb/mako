
config={
    # each line is one shard
    # Using 17xxx+ ports to avoid conflicts with common services (e.g., 8006 Proxmox)
    "base_0": [(101, 17001),(201, 17101),(301, 17201),(401, 17301)],
    "base_1": [(101, 18001),(201, 18101),(301, 18201),(401, 18301)],
    "base_2": [(101, 19001),(201, 19101),(301, 19201),(401, 19301)],
    "base_3": [(101, 20001),(201, 20101),(301, 20201),(401, 20301)],
    "base_4": [(101, 21001),(201, 21101),(301, 21201),(401, 21301)],
    "base_5": [(101, 22001),(201, 22101),(301, 22201),(401, 22301)],
    "base_6": [(101, 23001),(201, 23101),(301, 23201),(401, 23301)],
    "base_7": [(101, 24001),(201, 24101),(301, 24201),(401, 24301)],
    "base_8": [(101, 25001),(201, 25101),(301, 25201),(401, 25301)],
    "base_9": [(101, 26001),(201, 26101),(301, 26201),(401, 26301)],
}
nshards=10
with open('../../bash/n_partitions', 'r') as file:
    file_contents = file.read()
    nshards = int(file_contents)
    print("using partitions: ", nshards)
map_ip=[{} for _ in range(nshards)]

def loader():
    for shardIdx in range(nshards):
        file="../../bash/shard{shardIdx}.config.pub".format(shardIdx=shardIdx)
        for line in open(file, "r").readlines():
            items=[e for e in line.split(" ") if e]
            map_ip[shardIdx][items[0]]=items[1].strip()

def generate_shard(shardIdx):
    template="template_paxos1_shardidx{sIdx}.yml".format(sIdx=shardIdx)
    base = config["base_"+str(shardIdx)]

    for w_id in range(1, 32+1):
        file_name="paxos{w_id}_shardidx{sIdx}.yml".format(w_id=w_id,sIdx=shardIdx)
        content = ""
        for line in open(template, "r").readlines():
            skip=False
            for p in ["localhost","p1","p2","learner"]:
                if p in line:
                    skip=True
            
            if not skip:
                content += line
            if "server:" in line:
                servers = ""
                for i in range(w_id): 
                    servers += '    - ["s{n0}:{p0}", "s{n1}:{p1}", "s{n2}:{p2}", "s{n3}:{p3}"]\n'.format(
                        n0=base[0][0]+i, p0=base[0][1]+i,
                        n1=base[1][0]+i, p1=base[1][1]+i,
                        n2=base[2][0]+i, p2=base[2][1]+i,
                        n3=base[3][0]+i, p3=base[3][1]+i,
                    )
                content += servers    
            
            if "process:" in line:
                processes = ""
                for i in range(w_id):
                    processes += "  s{n0}: localhost\n".format(n0=base[0][0]+i)
                    processes += "  s{n1}: p1\n".format(n1=base[1][0]+i)
                    processes += "  s{n2}: p2\n".format(n2=base[2][0]+i)
                    processes += "  s{n3}: learner\n".format(n3=base[3][0]+i)
                content += processes

            for p in ["localhost","p1","p2","learner"]:
                if p in line:
                    line = line.replace("127.0.0.1", map_ip[shardIdx][p])
                    content += line

        f = open(file_name, "w")
        f.write(content)
        f.close()


if __name__ == "__main__":
    loader()

    for shardIdx in range(nshards):
        generate_shard(shardIdx)
