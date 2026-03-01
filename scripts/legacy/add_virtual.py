file_path = "./src/deptran/rcc_rpc.h"
keywords = [
    "MultiPaxosPlusService",
    "FpgaRaftPlusService",
    "MenciusPlusService",
    "CopilotPlusService",
    "CurpService",
]

lines = []
with open(file_path, "r") as file:
    lines = file.readlines()

with open(file_path, "w") as file:
    for line in lines:
        if ": public rrr::Service" in line:
            keyword_detected = False
            for keyword in keywords:
                if keyword in line:
                    keyword_detected = True
            if keyword_detected:
                print("before modification: " + line.rstrip());
                line = line.replace("public", "virtual public")
                print("after modification: " + line.rstrip());
        file.write(line)
