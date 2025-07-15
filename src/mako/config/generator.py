nshards=10
with open('../bash/n_partitions', 'r') as file:
    file_contents = file.read()
    nshards = int(file_contents)
    print("using partitions: ", nshards)
map_ip=[{} for _ in range(nshards)]

def loader():
    for shardIdx in range(nshards):
        file="../bash/shard{shardIdx}.config".format(shardIdx=shardIdx)
        for line in open(file, "r").readlines():
            items=[e for e in line.split(" ") if e]
            map_ip[shardIdx][items[0]]=items[1].strip()

def generate_shard(s):
    template="template_local-shards{s}-warehouses1.yml".format(s=s)
    print("work on template: ", template)
    for w_id in range(1, 32+1):
        file_name="local-shards{s}-warehouses{w_id}.yml".format(s=s,w_id=w_id)
        content = ""
        for line in open(template, "r").readlines():
            if "w_id" in line:
                line = line.replace("w_id", str(w_id))
            for s_id in range(nshards):
                for p in ["localhost","p1","p2","learner"]:
                    w="ip_shard{s_id}_{p}".format(s_id=s_id,p=p)
                    if w in line:
                        line = line.replace(w, map_ip[s_id][p])

            content += line

        f = open(file_name, "w")
        f.write(content)
        f.close()

if __name__ == "__main__":
    loader()
    print(map_ip)
    for s in range(1,nshards+1):
        generate_shard(s)
