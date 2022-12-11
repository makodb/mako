
config={
    # base_{shardIdx}
    "base_0": [(101, 7001),(201, 7101),(301, 7201),(401, 7301)],
    "base_1": [(101, 8001),(201, 8101),(301, 8201),(401, 8301)],
}
def generate_shard(shardIdx):
    template="template_paxos1_shardidx{sIdx}.yml".format(sIdx=shardIdx)
    base = config["base_"+str(shardIdx)]

    for w_id in range(1, 32+1):
        file_name="paxos{w_id}_shardidx{sIdx}.yml".format(w_id=w_id,sIdx=shardIdx)
        content = ""
        for line in open(template, "r").readlines():
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

        f = open(file_name, "w")
        f.write(content)
        f.close()


if __name__ == "__main__":
    for shardIdx in range(2):
        generate_shard(shardIdx)