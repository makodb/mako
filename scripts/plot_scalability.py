#!/usr/bin/env python3
"""Plot scalability: Paxos vs Raft-multi vs Raft-single (TPC-C, 1 shard, batch=400)."""
import matplotlib.pyplot as plt
import matplotlib
matplotlib.use("Agg")

paxos = {
    "threads":   [1, 2, 4, 6, 8],
    "tps":       [47203, 88423, 175093, 188175, 218134],
    "abort":     [0.0, 14.3, 25.6, 28.7, 34.3],
}
raft_multi = {
    "threads":   [1, 2, 4, 6, 8],
    "tps":       [40581, 78017, 132226, 194140, 167713],
    "abort":     [0.0, 12.7, 20.7, 28.7, 26.2],
}
raft_single = {
    "threads":   [1, 2, 4, 6, 8, 12, 16],
    "tps":       [42700, 82218, 92228, 283219, 366939, 529908, 662725],
    "abort":     [0.0, 13.2, 13.0, 42.8, 56.8, 79.8, 107.9],
}

ideal_threads = list(range(1, 17))
ideal_tps     = [paxos["tps"][0] * t for t in ideal_threads]

fig, (ax1, ax2, ax3) = plt.subplots(1, 3, figsize=(18, 5))

ax1.plot(ideal_threads, [v/1000 for v in ideal_tps], "k--", alpha=0.4,
         label="Ideal linear (Paxos 1-thread baseline)")
ax1.plot(paxos["threads"],       [v/1000 for v in paxos["tps"]],       "o-",
         label="Paxos", linewidth=2, markersize=8)
ax1.plot(raft_multi["threads"],  [v/1000 for v in raft_multi["tps"]],  "s-",
         label="Raft-multi", linewidth=2, markersize=8)
ax1.plot(raft_single["threads"], [v/1000 for v in raft_single["tps"]], "^-",
         label="Raft-single", linewidth=2, markersize=8)
ax1.set_xlabel("# of worker threads")
ax1.set_ylabel("Throughput (thousand ops/sec)")
ax1.set_title("Absolute Throughput vs Threads")
ax1.legend()
ax1.grid(True, alpha=0.3)
ax1.set_xticks([1, 2, 4, 6, 8, 12, 16])

def scaling_eff(data):
    base = data["tps"][0]
    return [(data["tps"][i] / (base * data["threads"][i])) * 100
            for i in range(len(data["threads"]))]

ax2.axhline(y=100, color="k", linestyle="--", alpha=0.4, label="Perfect linear")
ax2.plot(paxos["threads"],       scaling_eff(paxos),       "o-",
         label="Paxos", linewidth=2, markersize=8)
ax2.plot(raft_multi["threads"],  scaling_eff(raft_multi),  "s-",
         label="Raft-multi", linewidth=2, markersize=8)
ax2.plot(raft_single["threads"], scaling_eff(raft_single), "^-",
         label="Raft-single", linewidth=2, markersize=8)
ax2.set_xlabel("# of worker threads")
ax2.set_ylabel("Scaling efficiency (%)")
ax2.set_title("Scaling Efficiency (100% = linear)")
ax2.legend()
ax2.grid(True, alpha=0.3)
ax2.set_xticks([1, 2, 4, 6, 8, 12, 16])
ax2.set_ylim(0, 140)

ax3.plot(paxos["threads"],       paxos["abort"],       "o-",
         label="Paxos", linewidth=2, markersize=8)
ax3.plot(raft_multi["threads"],  raft_multi["abort"],  "s-",
         label="Raft-multi", linewidth=2, markersize=8)
ax3.plot(raft_single["threads"], raft_single["abort"], "^-",
         label="Raft-single", linewidth=2, markersize=8)
ax3.set_xlabel("# of worker threads")
ax3.set_ylabel("Aggregate abort rate (%)")
ax3.set_title("Abort Rate vs Threads")
ax3.legend()
ax3.grid(True, alpha=0.3)
ax3.set_xticks([1, 2, 4, 6, 8, 12, 16])

plt.suptitle("Mako scalability: TPC-C, 1 shard, batch=400 (16-Apr-2026 sweeps)",
             fontsize=14, fontweight="bold")
plt.tight_layout()
out = "/home/users/mmakadia/mako/results/scalability_comparison.png"
plt.savefig(out, dpi=130, bbox_inches="tight")
print(f"Saved: {out}")
