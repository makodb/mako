# Python script to read a file line by line


#20241021-23:15.40-736170(us) 470063 ! SendBatchRequestToAll (fasttransport.cc:432): SendBatchRequestToAll, req_type:10, shard_idx:1, id:6, req_msg_len:336
#20241021-23:15.40-736250(us) 470063 ! SendRequestToAll (fasttransport.cc:363): SendRequestToAll, reqType:8, shards_to_send_bit_set:2, id:6, respMsgLen:16, reqMsgLen:8, forceCenter:-1
#20241021-23:15.40-736303(us) 470063 ! SendRequestToAll (fasttransport.cc:363): SendRequestToAll, reqType:3, shards_to_send_bit_set:2, id:6, respMsgLen:8, reqMsgLen:8, forceCenter:-1
#20241021-23:15.40-736356(us) 470063 ! SendRequestToAll (fasttransport.cc:363): SendRequestToAll, reqType:4, shards_to_send_bit_set:2, id:6, respMsgLen:8, reqMsgLen:36, forceCenter:-1
#20241021-23:15.40-736499(us) 470063 ! SendRequestToShard (fasttransport.cc:322): SendRequestToShard, reqType:1, shardIdx:1, id:6, msgLen:20
def read_file_line_by_line(file_path):
    m = -1
    try:
        with open(file_path, 'r') as file:
            for line in file:
                line = line.strip()
                if "req_msg_len" in line:
                    m = max(m, int(line.split(":")[-1]))
                if "msgLen" in line:
                    m = max(m, int(line.split(":")[-1]))
                if "reqMsgLen" in line:
                    m = max(m, int(line.split(":")[-2].split(",")[0]))
    except FileNotFoundError:
        print(f"Error: The file '{file_path}' was not found.")
    except Exception as e:
        print(f"An error occurred: {e}")
    print(m)

# Example usage
if __name__ == "__main__":
    read_file_line_by_line("leader.log")
    read_file_line_by_line("leader-1.log")
    read_file_line_by_line("leader-2.log")
