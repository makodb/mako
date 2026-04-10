
# Raft config generator — generates raftN_shardidxS.yml for N=1..32, S=0..nshards
# Modeled after generator.py (Paxos) with these differences:
#   - 3 replicas per group (no learner)
#   - Port range: 27xxx+ (vs Paxos 17xxx+)
#   - Template: template_raft1_shardidxS.yml
#   - Skips "learner" from shard*.config.pub

config={
    # each line is one shard
    # 3 replicas: localhost (s1xx), p1 (s2xx), p2 (s3xx) — no learner (s4xx)
    # Port range starts at 27xxx to avoid conflicts with Paxos (17xxx-26xxx)
    "base_0": [(101, 27001),(201, 27101),(301, 27201)],
    "base_1": [(101, 28001),(201, 28101),(301, 28201)],
    "base_2": [(101, 29001),(201, 29101),(301, 29201)],
    "base_3": [(101, 30001),(201, 30101),(301, 30201)],
    "base_4": [(101, 31001),(201, 31101),(301, 31201)],
    "base_5": [(101, 32001),(201, 32101),(301, 32201)],
    "base_6": [(101, 33001),(201, 33101),(301, 33201)],
    "base_7": [(101, 34001),(201, 34101),(301, 34201)],
    "base_8": [(101, 35001),(201, 35101),(301, 35201)],
    "base_9": [(101, 36001),(201, 36101),(301, 36201)],
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
            # Skip learner entries — Raft uses only 3 replicas
            if items[0] == "learner":
                continue
            map_ip[shardIdx][items[0]]=items[1].strip()

def generate_shard(shardIdx):
    template="template_raft1_shardidx{sIdx}.yml".format(sIdx=shardIdx)
    base = config["base_"+str(shardIdx)]

    for w_id in range(1, 32+1):
        file_name="raft{w_id}_shardidx{sIdx}.yml".format(w_id=w_id,sIdx=shardIdx)
        content = ""
        for line in open(template, "r").readlines():
            skip=False
            for p in ["localhost","p1","p2"]:
                if p in line:
                    skip=True

            if not skip:
                content += line
            if "server:" in line:
                servers = ""
                for i in range(w_id):
                    servers += '    - ["s{n0}:{p0}", "s{n1}:{p1}", "s{n2}:{p2}"]\n'.format(
                        n0=base[0][0]+i, p0=base[0][1]+i,
                        n1=base[1][0]+i, p1=base[1][1]+i,
                        n2=base[2][0]+i, p2=base[2][1]+i,
                    )
                content += servers

            if "process:" in line:
                processes = ""
                for i in range(w_id):
                    processes += "  s{n0}: localhost\n".format(n0=base[0][0]+i)
                    processes += "  s{n1}: p1\n".format(n1=base[1][0]+i)
                    processes += "  s{n2}: p2\n".format(n2=base[2][0]+i)
                content += processes

            for p in ["localhost","p1","p2"]:
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
