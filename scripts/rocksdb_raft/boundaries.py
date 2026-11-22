#!/usr/bin/env python3
"""Real process crashes and partitions; requires the explicit test-hooks build."""
import argparse
import json
from pathlib import Path
import queue
import tempfile
import time

from smoke import Cluster, Rpc, byte, create_tables, error, good


def gate(cluster, node, stage, paused=None):
    return good(cluster.clients[node].call({"kind": "test_gate", "group": 1,
                "stage": stage, "paused": paused}), "test_gate")


def until(predicate, description, seconds=40):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(.02)
    raise AssertionError(f"{description}; last observation={last!r}")


def waiting(cluster, node, stage):
    def observe():
        state = gate(cluster, node, stage)
        return state if state["waiting"] else None
    return until(observe, f"{stage} was not reached")


def connection(cluster, node):
    return Rpc(cluster.binaries / "rocksdb_raft_client", cluster.client_config, cluster.endpoint(node))


def start_commit(cluster, transaction):
    transaction["rpc"].send({"kind": "commit", "handle": transaction["handle"]})


def no_success(rpc):
    try:
        response = rpc.replies.get(timeout=.15)
    except queue.Empty:
        return None
    assert response is not None, "client exited while a boundary was paused"
    error(response, "unknown_outcome", "transport", "not_leader")
    return response


def completed(cluster, transaction, earlier=None):
    response = earlier if earlier is not None else transaction["rpc"].replies.get(timeout=20)
    if response["kind"] == "error":
        error(response, "unknown_outcome", "transport")
        until(lambda: good(cluster.admin(1, "outcome", request=transaction["request"]))["position"],
              "accepted transaction did not resolve durably")
    else:
        good(response, "committed")


def catch_up(cluster, node):
    def observe():
        states = {peer: cluster.status(peer, 1) for peer in sorted(cluster.processes)}
        if any(state["kind"] != "status" or state["failure"] is not None for state in states.values()):
            return False
        leaders = [(peer, state) for peer, state in states.items() if state["is_leader"]]
        if len(leaders) != 1:
            return False
        leader, state = leaders[0]
        return (all(peer["term"] == state["term"] and peer["leader"] == leader for peer in states.values())
                and states[node]["applied"] >= state["committed"])
    until(observe, f"replica {node} did not reconcile the agreed leader's committed prefix")


def crash_boundaries(cluster):
    for stage in ("before_proposal", "after_append", "before_apply", "after_apply", "before_reply"):
        node = cluster.leader(1)
        rpc = connection(cluster, node)
        try:
            tx = cluster.begin(rpc=rpc)
            keys = [f"a-crash-{stage}-{part}" for part in (0, 1)]
            for key in keys:
                good(cluster.put(tx, key, "atomic-durable-value"))
            good(gate(cluster, node, stage, True))
            if stage == "after_append":
                good(cluster.clients[node].call({"kind": "test_isolate", "group": 1, "isolated": True}))
            start_commit(cluster, tx)
            observed = waiting(cluster, node, stage)
            state = good(cluster.status(node, 1), "status")
            position = observed["position"]
            if stage == "before_proposal":
                assert position is None
            elif stage == "after_append":
                assert state["committed"] < position["index"], (stage, observed, state)
            elif stage in ("before_apply", "after_apply"):
                assert state["committed"] >= position["index"] > state["applied"], (stage, observed, state)
            else:
                assert state["applied"] >= position["index"], (stage, observed, state)
            no_success(rpc)
            cluster.kill(node)
            cluster.leader(1, exclude=(node,))
            committed = stage in ("before_apply", "after_apply", "before_reply")
            expected = byte("atomic-durable-value") if committed else None
            for key in keys:
                assert cluster.value(key) == expected, (stage, key)
            outcome = good(cluster.admin(1, "outcome", request=tx["request"]))["position"]
            assert (outcome is not None) == committed, (stage, outcome)
            cluster.start(node)
            catch_up(cluster, node)
            for key in keys:
                assert cluster.value(key) == expected, (stage, "after recovery", key)
            print(f"PASS SIGKILL at {stage}: no premature success, atomic {'committed' if committed else 'unpublished'} result and recovered prefix", flush=True)
        finally:
            rpc.close()


def pipelined_apply(cluster):
    node = cluster.leader(1)
    clients = [connection(cluster, node) for _ in range(4)]
    try:
        first, second, stale = [cluster.begin(rpc=rpc) for rpc in clients[:3]]
        assert good(cluster.execute(stale, "get", key=byte("a-held-first")), "value")["value"] is None
        good(cluster.put(stale, "a-stale-side-effect", "must-not-appear"))
        good(cluster.put(first, "a-held-first", "first"))
        good(cluster.put(second, "a-held-second", "second"))
        gate(cluster, node, "before_apply", True)
        start_commit(cluster, first)
        position = waiting(cluster, node, "before_apply")["position"]["index"]
        start_commit(cluster, second)
        until(lambda: good(cluster.status(node, 1), "status")["committed"] > position,
              "independent proposal could not enter committed log while apply was paused")
        error(cluster.commit(stale), "conflict")
        first_early, second_early = no_success(clients[0]), no_success(clients[1])
        clients[3].send({"kind": "begin", "group": 1, "request": cluster.request_id()})
        read_early = no_success(clients[3])
        gate(cluster, node, "before_apply", False)
        completed(cluster, first, first_early)
        completed(cluster, second, second_early)
        if read_early is None:
            begun = good(clients[3].replies.get(timeout=20), "begun")
            reader = {"handle": begun["handle"], "rpc": clients[3]}
            assert good(cluster.execute(reader, "get", key=byte("a-held-first")), "value")["value"] == byte("first")
            assert good(cluster.execute(reader, "get", key=byte("a-held-second")), "value")["value"] == byte("second")
            good(cluster.commit(reader), "committed")
        assert cluster.value("a-stale-side-effect") is None
        assert cluster.value("a-held-first") == byte("first")
        assert cluster.value("a-held-second") == byte("second")
        print("PASS paused apply permits independent committed proposals, retains conflicting reservations and blocks fresh read readiness", flush=True)
    finally:
        gate(cluster, node, "before_apply", False)
        for rpc in clients:
            rpc.close()


def stale_leader(cluster):
    old = cluster.leader(1)
    rpc = connection(cluster, old)
    try:
        tx = cluster.begin(rpc=rpc)
        good(cluster.put(tx, "a-partitioned-write", "uncommitted-old-leader"))
        old_term = good(cluster.status(old, 1), "status")["term"]
        good(cluster.clients[old].call({"kind": "test_isolate", "group": 1, "isolated": True}))
        replacement = cluster.leader(1, exclude=(old,))
        assert good(cluster.status(replacement, 1), "status")["term"] > old_term
        error(cluster.clients[old].call({"kind": "begin", "group": 1, "request": cluster.request_id()}),
              "not_leader", "unknown_outcome", "closed")
        error(cluster.commit(tx), "not_leader", "unknown_outcome")
        # Use the explicitly selected new leader: status on the isolated old
        # node may legitimately still say leader until the partition heals.
        replacement_rpc = connection(cluster, replacement)
        try:
            begun = good(replacement_rpc.call({"kind": "begin", "group": 1, "request": cluster.request_id()}), "begun")
            current = {"handle": begun["handle"], "rpc": replacement_rpc}
            assert good(cluster.execute(current, "get", key=byte("a-partitioned-write")), "value")["value"] is None
            good(cluster.put(current, "a-partitioned-write", "new-leader-authority"))
            good(cluster.commit(current), "committed")
        finally:
            replacement_rpc.close()
        good(cluster.clients[old].call({"kind": "test_isolate", "group": 1, "isolated": False}))
        catch_up(cluster, old)
        assert cluster.value("a-partitioned-write") == byte("new-leader-authority")
        assert good(cluster.admin(1, "outcome", request=tx["request"]))["position"] is None
        print("PASS isolated stale leader cannot serve fresh linearizable transactions or publish old intent; healing installs replacement authority", flush=True)
    finally:
        if old in cluster.processes:
            good(cluster.clients[old].call({"kind": "test_isolate", "group": 1, "isolated": False}))
        rpc.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path, required=True)
    parser.add_argument("--data-dir", type=Path)
    args = parser.parse_args()
    temporary = tempfile.TemporaryDirectory(prefix="rocksdb-raft-boundaries-") if args.data_dir is None else None
    root = Path(temporary.name) if temporary else args.data_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if any(root.iterdir()):
        raise ValueError("--data-dir must be empty")
    cluster = Cluster.__new__(Cluster)
    try:
        cluster.__init__(args.bin_dir.resolve(), root)
        create_tables(cluster)
        pipelined_apply(cluster)
        stale_leader(cluster)
        crash_boundaries(cluster)
        print(json.dumps({"suite": "rocksdb_raft_boundaries", "status": "passed"}))
    except BaseException:
        for log in root.glob("node*.log"):
            print(f"--- {log} ---\n{log.read_text(errors='replace')[-20000:]}", flush=True)
        raise
    finally:
        cluster.close()
        if temporary:
            temporary.cleanup()


if __name__ == "__main__":
    main()
