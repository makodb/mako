
config={
    # each line is one shard
    # for based-7: ["s101:11501", "s201:11601", "s301:11701", "s401:11801"]
    "base_0": [(101, 7001),(201, 7101),(301, 7201),(401, 7301)],
    "base_1": [(101, 8001),(201, 8101),(301, 8201),(401, 8301)],
    "base_2": [(101, 9001),(201, 9101),(301, 9201),(401, 9301)],
    "base_3": [(101, 7501),(201, 7601),(301, 7701),(401, 7801)],
    "base_4": [(101, 8501),(201, 8601),(301, 8701),(401, 8801)],
    "base_5": [(101, 9501),(201, 9601),(301, 9701),(401, 9801)],
    "base_6": [(101, 10501),(201, 10601),(301, 10701),(401, 10801)],
    "base_7": [(101, 11501),(201, 11601),(301, 11701),(401, 11801)],
    "base_8": [(101, 12501),(201, 12601),(301, 12701),(401, 12801)],
    "base_9": [(101, 13501),(201, 13601),(301, 13701),(401, 13801)],
}
nshards=10
with open('../../../../bash/n_partitions', 'r') as file:
    file_contents = file.read()
    nshards = int(file_contents)
    print("using partitions: ", nshards)
map_ip=[{} for _ in range(nshards)]

def loader():
    for shardIdx in range(nshards):
        file="../../../../bash/shard{shardIdx}.config.pub".format(shardIdx=shardIdx)
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
