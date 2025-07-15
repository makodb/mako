import os.path
from os import path
import sys
import re
import pprint

def parse_by_kw(file, kw, replaces=[]):
    values = []
    if path.exists(file):
        with open(file) as handler:
            lines = handler.readlines()
            for line in lines:
                if kw in line:
                    for r in replaces:
                        line = line.replace(r, "")
                    values.append(line)
    return values


def exp_0_0():
    for trd in range(1, 12+1):
        file = "exp0_0_{trd}.log".format(trd=trd)
        values = parse_by_kw(file, "agg_throughput", ["agg_throughput","\n","ops/sec",":"," "])
        #print "{trd}\t{tput}".format(trd=trd, tput=values[0] if values else 0)
        print "{tput}".format(tput=values[0] if values else 0)

def exp_0_1():
    for trd in range(1, 12+1):
        file = "exp0_1_{trd}.log".format(trd=trd)
        values = parse_by_kw(file, "agg_throughput", ["agg_throughput","\n","ops/sec",":"," "])
        #print "{trd}\t{tput}".format(trd=trd, tput=values[0] if values else 0)
        print "{tput}".format(tput=values[0] if values else 0)

def exp_1_0():
    for trd in range(1, 12+1):
        file = "exp1_0_{trd}.log".format(trd=trd)
        values = parse_by_kw(file, "agg_throughput", ["agg_throughput","\n","ops/sec",":"," "])
        #print "{trd}\t{tput}".format(trd=trd, tput=values[0] if values else 0)
        print "{tput}".format(tput=values[0] if values else 0)

def exp_1_1():
    for trd in range(1, 12+1):
        file = "exp1_1_{trd}.log".format(trd=trd)
        values = parse_by_kw(file, "agg_throughput", ["agg_throughput","\n","ops/sec",":"," "])
        #print "{trd}\t{tput}".format(trd=trd, tput=values[0] if values else 0)
        print "{tput}".format(tput=values[0] if values else 0)


def exp_2_0():
    for nshards in range(1, 6+1):
        data=""
        tput = []
        for sIdx in range(nshards):
            file = "exp2_0_nshard{nshards}_sIdx{c}_trd{trd}.log".format(nshards=nshards, c=sIdx, trd=12)
            values = parse_by_kw(file, "agg_throughput", ["agg_throughput","\n","ops/sec",":"," "])
            tput.append(int(values[0]) if values else 0)
        data+="{avg}".format(avg=sum(tput)/len(tput))

        file0 = "exp2_0_nshard${nshards}_sIdx${c}_trd{trd}.log".format(nshards=nshards, c=0, trd=12)
        # abort per shard
        values = parse_by_kw(file, "agg_abort_rate", ["agg_abort_rate","\n","aborts/sec",":"," "])
        data+="\t"+str(values[0] if values else 0)
        # Abort ratio for Remote - new order
        values = parse_by_kw(file, "NewOrder_remote_abort_ratio", ["NewOrder_remote_abort_ratio","\n",":"," "])
        data+="\t"+str(values[0] if values else 0)
        # Abort ratio for Local - new order
        values = parse_by_kw(file, "NewOrder_local_abort_ratio", ["NewOrder_local_abort_ratio","\n",":"," "])
        data+="\t"+str(values[0] if values else 0)
        # Abort ratio for Remote - payment
        values = parse_by_kw(file, "Payment_remote_abort_ratio", ["Payment_remote_abort_ratio","\n",":"," "])
        data+="\t"+str(values[0] if values else 0)
        # Abort ratio for Local - payment
        values = parse_by_kw(file, "Payment_local_abort_ratio", ["Payment_local_abort_ratio","\n",":"," "])
        data+="\t"+str(values[0] if values else 0)

        print data


def exp_2_1():
    for nshards in range(1, 6+1):
        data=""
        tput = []
        for sIdx in range(nshards):
            file = "exp2_1_nshard${nshards}_sIdx${c}_trd{trd}.log".format(nshards=nshards, c=sIdx, trd=12)
            values = parse_by_kw(file, "agg_throughput", ["agg_throughput","\n","ops/sec",":"," "])
            tput.append(int(values[0]) if values else 0)
        data+="{avg}".format(avg=sum(tput)/len(tput))

        file0 = "exp2_1_nshard${nshards}_sIdx${c}_trd{trd}.log".format(nshards=nshards, c=0, trd=12)
        # abort per shard
        values = parse_by_kw(file, "agg_abort_rate", ["agg_abort_rate","\n","aborts/sec",":"," "])
        data+="\t"+str(values[0] if values else 0)
        # Abort ratio for Remote - new order
        values = parse_by_kw(file, "NewOrder_remote_abort_ratio", ["NewOrder_remote_abort_ratio","\n",":"," "])
        data+="\t"+str(values[0] if values else 0)
        # Abort ratio for Local - new order
        values = parse_by_kw(file, "NewOrder_local_abort_ratio", ["NewOrder_local_abort_ratio","\n",":"," "])
        data+="\t"+str(values[0] if values else 0)
        # Abort ratio for Remote - payment
        values = parse_by_kw(file, "Payment_remote_abort_ratio", ["Payment_remote_abort_ratio","\n",":"," "])
        data+="\t"+str(values[0] if values else 0)
        # Abort ratio for Local - payment
        values = parse_by_kw(file, "Payment_local_abort_ratio", ["Payment_local_abort_ratio","\n",":"," "])
        data+="\t"+str(values[0] if values else 0)

        print data


def extract_time_ncommits(f):
    # Open the file and read its contents
    with open(f, 'r') as file:
        content = file.readlines()

    # Define the regex pattern to match
    pattern = re.compile(r'Time:(.*), n_comits:(.*)')

    ret = []
    # Print out the values for all lines that match the pattern
    for line in content:
        match = re.search(pattern, line)
        if match:
            a, b = match.groups()
            ret.append((int(a),int(b)))
    
    return ret
    

def extract_failover():
    # Prompt the user to enter a filename
    # extract_time_ncommits("./leader.log")
    ret = extract_time_ncommits("./exp-localhost-v14-4-24-1-0.log")

    ret_update = []
    interval = 10 # 10ms
    for i, (t, n) in enumerate(ret[1:]):
        t, n = int(t), int(n)
        ret_update.append(((t-ret[0][0])/interval, (1000/interval)*(n-ret[i][1])))
    for i, (t, n) in enumerate(ret_update):
        #if 900 <= t <= 1400:
        print("{a}\t{b}\t{c}\t{d}".format(a=t, b=n, c=ret[i][0], d=ret[i][1]))

def extract_failover_complete():
    leader = extract_time_ncommits("./leader-1.log")
    p1 = extract_time_ncommits("./follower-p1.log")
    ret = leader + p1
    ret_update = []
    interval = 10 # 10ms

    gap = p1[0][0] - leader[-1][0]

    cnt = 1
    for i, (t, n) in enumerate(leader[1:]):
        t, n = int(t), int(n)
        ret_update.append((cnt, (1000/interval)*(n-leader[i][1])))
        cnt += 1

    for j in range(gap/interval):
        ret_update.append((cnt, 0))
        cnt += 1
    
    prev = 0
    for i, (t, n) in enumerate(p1):
        t, n = int(t), int(n)
        ret_update.append((cnt, (1000/interval)*(n-prev)))
        cnt += 1
        prev = p1[i][1]
   
    for i, (t, n) in enumerate(ret_update):
        print("{a}\t{b}".format(a=t, b=n))

if __name__ == "__main__":
    # print "exp_0_0"
    # exp_0_0()
    # print "\n"

    # print "exp_0_1"
    # exp_0_1()
    # print "\n"

    # print "exp_1_0"
    # exp_1_0()
    # print "\n"

    # print "exp_1_1"
    # exp_1_1()
    # print "\n"

    # print "exp_2_0"
    # exp_2_0()
    # print "\n"

    # print "exp_2_1"
    # exp_2_1()
    # print "\n"

    extract_failover()
    #extract_failover_complete()
