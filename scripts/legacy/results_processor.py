import os

import argparse

def sorting_key(item):
    return item[:4] 


parser = argparse.ArgumentParser(description="Description of your script.")
parser.add_argument('exptime', type=str, help='Name of the directory to open under directory "results"')
args = parser.parse_args()
exptime = args.exptime

# exptime = "2023-10-10-03:38:03"
directory_path = os.path.join("results", exptime)

files = os.listdir(directory_path)

file_names = [file for file in files if os.path.isfile(os.path.join(directory_path, file))]

datas = []
rows = []

for file_name in file_names:
    parts = file_name.split("-")
    # print(parts)
    latency = parts[0]
    mode = parts[1]
    site = parts[2]
    workload = parts[3]
    conc = int(parts[4][parts[4].find("_")+1:])
    finish_countdown = parts[5]
    fastpath_timeout = parts[6]
    wait_commit_timeout = parts[7]
    instance_commit_timeout = parts[8]
    fastpath_rate = "adapative" if parts[9] == "101" else parts[9]
    duration = parts[10][:parts[10].find(".")]
    if "plus" not in mode:
        finish_countdown = None
        fastpath_timeout = None
        wait_commit_timeout = None
        instance_commit_timeout = None

    throughtput = None
    mid_throughtput = None

    fastpath_count = None
    fastpath_50pct = None
    coordinatoraccept_count = None
    coordinatoraccept_50pct = None
    fast_original_count = None
    fast_original_50pct = None
    slow_original_count = None
    slow_original_50pct = None
    original_protocol_count = None
    original_protocol_50pct = None

    latency50pct = None
    latency90pct = None
    latency99pct = None

    # fastpath_count = None
    # coordinatoraccept_count = None
    # original_count = None

    cpu0_medium = None
    cpu1_medium = None
    cpu2_medium = None
    clientall_medium = None

    with open(os.path.join(directory_path, file_name), "r") as file:
        for line in file:
            if "Total throughtput is" in line:
                throughtput = float(line[line.find("is")+3:].strip())
            if "Mid throughput is" in line:
                mid_throughtput = float(line[line.find("is")+3:].strip())
            if "Fastpath" in line:
                fastpath_count = int(line[line.find("count")+6:line.find("50pct")])
                fastpath_50pct = float(line[line.find("50pct")+6:line.find("90pct")]) if fastpath_count > 0 else None
            if "CoordinatorAccept" in line:
                coordinatoraccept_count = int(line[line.find("count")+6:line.find("50pct")])
                coordinatoraccept_50pct = float(line[line.find("50pct")+6:line.find("90pct")]) if coordinatoraccept_count > 0 else None
            if "Fast-Original" in line:
                fast_original_count = int(line[line.find("count")+6:line.find("50pct")])
                fast_original_50pct = float(line[line.find("50pct")+6:line.find("90pct")]) if fast_original_count > 0 else None
            if "Slow-Original" in line:
                slow_original_count = int(line[line.find("count")+6:line.find("50pct")])
                slow_original_50pct = float(line[line.find("50pct")+6:line.find("90pct")]) if slow_original_count > 0 else None
            if "Original-Protocol" in line:
                original_protocol_count = int(line[line.find("count")+6:line.find("50pct")])
                original_protocol_50pct = float(line[line.find("50pct")+6:line.find("90pct")]) if original_protocol_count > 0 else None
            if "Latency-50pct is" in line:
                latency50pct = float(line[line.find("Latency-50pct is")+17:line.find("Latency-90pct is")-4])
                latency90pct = float(line[line.find("Latency-90pct is")+17:line.find("Latency-99pct is")-4])
                latency99pct = float(line[line.find("Latency-99pct is")+17:-4])
            # if "plus" in mode:
            #     if "FastPath-count" in line:
            #         fastpath_count = int(line[line.find("FastPath-count =")+16:line.find("CoordinatorAccept-count")])
            #         coordinatoraccept_count = int(line[line.find("CoordinatorAccept-count =")+26:line.find("OriginalProtocol-count")])
            #         original_count = int(line[line.find("OriginalProtocol-count =")+25:])
            if "cpu0" in line:
                cpu0_medium = float(line[line.find("medium")+8:line.find("mean")])
            if "cpu1" in line:
                cpu1_medium = float(line[line.find("medium")+8:line.find("mean")])
            if "cpu2" in line:
                cpu2_medium = float(line[line.find("medium")+8:line.find("mean")])
            if "clientall" in line:
                clientall_medium = float(line[line.find("medium")+8:line.find("mean")])
    # print(mode, site, workload, conc, throughtput, latency50pct, latency90pct, latency99pct, fastpath_count, coordinatoraccept_count, original_count, max_gap)
    datas.append((latency, site, mode, workload, fastpath_rate, duration, conc, finish_countdown, fastpath_timeout, wait_commit_timeout, instance_commit_timeout,\
                  throughtput, mid_throughtput, fastpath_count, fastpath_50pct, coordinatoraccept_count, coordinatoraccept_50pct, fast_original_count,\
                  fast_original_50pct, slow_original_count, slow_original_50pct, original_protocol_count, original_protocol_50pct,\
                  latency50pct, latency90pct, latency99pct, cpu0_medium, cpu1_medium, cpu2_medium, clientall_medium))
    rows.append([latency, site, mode, workload, fastpath_rate, duration, conc, finish_countdown, fastpath_timeout, wait_commit_timeout, instance_commit_timeout,\
                  throughtput, mid_throughtput, fastpath_count, fastpath_50pct, coordinatoraccept_count, coordinatoraccept_50pct, fast_original_count,\
                  fast_original_50pct, slow_original_count, slow_original_50pct, original_protocol_count, original_protocol_50pct,\
                  latency50pct, latency90pct, latency99pct, cpu0_medium, cpu1_medium, cpu2_medium, clientall_medium])

datas = sorted(datas, key=sorting_key)
# print(datas)
for data in datas:
    print(data)

fields = ["latency", "site", "mode", "workload", "fastpath_rate", "duration", "conc", "finish_countdown", "fastpath_timeout", "wait_commit_timeout", "instance_commit_timeout",\
            "throughtput", "mid_throughtput", "fastpath_count", "fastpath_50pct", "coordinatoraccept_count", "coordinatoraccept_50pct", "fast_original_count",\
            "fast_original_50pct", "slow_original_count", "slow_original_50pct", "original_protocol_count", "original_protocol_50pct",\
            "latency50pct", "latency90pct", "latency99pct", "cpu0_medium", "cpu1_medium", "cpu2_medium", "clientall_medium"]

import csv 
    
filename = os.path.join("results", "curp_results-" + exptime + ".csv")
    
# writing to csv file 
with open(filename, 'w') as csvfile: 
    # creating a csv writer object 
    csvwriter = csv.writer(csvfile) 
        
    # writing the fields 
    csvwriter.writerow(fields) 
        
    # writing the data rows 
    csvwriter.writerows(rows)