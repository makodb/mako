#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import socket
import time
from pathlib import Path
from typing import Any, Callable

import redis

from harness_common import RedisTarget, fail, main_guard


ROOT = Path(__file__).resolve().parents[2]
TIERS_FILE = ROOT / "tools/redis_compat/command_tiers.json"
OUT_FILE = Path(os.environ.get("REDIS_COMPAT_PROBE_OUT", ROOT / "tools/redis_compat/probe_results.json"))


def target_from_env(prefix: str, default_port: int) -> RedisTarget:
    return RedisTarget(
        host=os.environ.get(f"{prefix}_HOST", "127.0.0.1"),
        port=int(os.environ.get(f"{prefix}_PORT", str(default_port))),
    )


def client(target: RedisTarget) -> redis.Redis:
    return redis.Redis(host=target.host, port=target.port, decode_responses=False, socket_timeout=3)


def classify(exc: BaseException | None) -> str:
    if exc is None:
        return "OK"
    msg = str(exc).lower()
    if "unknown command" in msg:
        return "UNKNOWN_COMMAND"
    if "wrong number" in msg or "syntax error" in msg:
        return "WRONG_ARITY"
    if isinstance(exc, redis.TimeoutError):
        return "TIMEOUT"
    if isinstance(exc, redis.ConnectionError):
        return "CONNECTION_ERROR"
    return "ERROR"


def raw_resp(target: RedisTarget, *args: object) -> bytes:
    payload = [f"*{len(args)}\r\n".encode()]
    for arg in args:
        data = str(arg).encode() if not isinstance(arg, bytes) else arg
        payload.append(f"${len(data)}\r\n".encode())
        payload.append(data + b"\r\n")
    with socket.create_connection((target.host, target.port), timeout=3) as sock:
        sock.sendall(b"".join(payload))
        sock.settimeout(3)
        return sock.recv(65536)


def raw_case(*args: object) -> Callable[[redis.Redis, str], Any]:
    def inner(r: redis.Redis, _p: str) -> bytes:
        target = RedisTarget(host=r.connection_pool.connection_kwargs["host"], port=r.connection_pool.connection_kwargs["port"])
        reply = raw_resp(target, *args)
        if reply.startswith(b"-"):
            raise redis.ResponseError(reply.decode(errors="replace").strip())
        return reply

    return inner


def call(fn: Callable[[redis.Redis, str], Any], target: RedisTarget, prefix: str) -> dict[str, Any]:
    r = client(target)
    try:
        value = fn(r, prefix)
        return {"status": "OK", "detail": repr(value)[:160]}
    except BaseException as exc:  # noqa: BLE001
        return {"status": classify(exc), "detail": str(exc)[:240]}
    finally:
        try:
            r.close()
        except Exception:
            pass


def command_cases() -> dict[str, Callable[[redis.Redis, str], Any]]:
    def tx(r: redis.Redis, p: str) -> Any:
        pipe = r.pipeline(transaction=True)
        pipe.set(f"{p}:tx", b"1")
        pipe.get(f"{p}:tx")
        return pipe.execute()

    return {
        "PING": lambda r, p: r.ping(),
        "HELLO": raw_case("HELLO", 3),
        "CLIENT": lambda r, p: r.execute_command("CLIENT", "ID"),
        "COMMAND": raw_case("COMMAND", "COUNT"),
        "INFO": lambda r, p: r.execute_command("INFO", "server"),
        "SELECT": lambda r, p: r.execute_command("SELECT", 0),
        "AUTH": lambda r, p: r.execute_command("AUTH", "default", ""),
        "QUIT": lambda r, p: r.execute_command("QUIT"),
        "RESET": lambda r, p: r.execute_command("RESET"),
        "ECHO": lambda r, p: r.execute_command("ECHO", b"probe"),
        "WAIT": lambda r, p: r.execute_command("WAIT", 0, 0),
        "TIME": lambda r, p: r.execute_command("TIME"),
        "GET": lambda r, p: r.get(f"{p}:string"),
        "SET": lambda r, p: r.set(f"{p}:string", b"value"),
        "MGET": lambda r, p: r.mget([f"{p}:string", f"{p}:missing"]),
        "MSET": lambda r, p: r.mset({f"{p}:m1": b"1", f"{p}:m2": b"2"}),
        "MSETNX": lambda r, p: r.execute_command("MSETNX", f"{p}:nx1", b"1", f"{p}:nx2", b"2"),
        "GETSET": lambda r, p: r.getset(f"{p}:getset", b"new"),
        "SETEX": lambda r, p: r.execute_command("SETEX", f"{p}:setex", 60, b"v"),
        "PSETEX": lambda r, p: r.execute_command("PSETEX", f"{p}:psetex", 60000, b"v"),
        "SETNX": lambda r, p: r.setnx(f"{p}:setnx", b"1"),
        "APPEND": lambda r, p: r.append(f"{p}:append", b"x"),
        "STRLEN": lambda r, p: r.strlen(f"{p}:append"),
        "DEL": lambda r, p: r.delete(f"{p}:m1"),
        "UNLINK": lambda r, p: r.execute_command("UNLINK", f"{p}:m2"),
        "EXISTS": lambda r, p: r.exists(f"{p}:string", f"{p}:missing"),
        "INCR": lambda r, p: r.incr(f"{p}:counter"),
        "INCRBY": lambda r, p: r.incrby(f"{p}:counter", 2),
        "DECR": lambda r, p: r.decr(f"{p}:counter"),
        "DECRBY": lambda r, p: r.decrby(f"{p}:counter", 2),
        "INCRBYFLOAT": lambda r, p: r.incrbyfloat(f"{p}:float", 1.5),
        "MULTI": lambda r, p: tx(r, p),
        "EXEC": lambda r, p: tx(r, p),
        "DISCARD": lambda r, p: r.pipeline(transaction=True).discard(),
        "EXPIRE": lambda r, p: r.expire(f"{p}:string", 60),
        "PEXPIRE": lambda r, p: r.pexpire(f"{p}:string", 60000),
        "EXPIREAT": lambda r, p: r.expireat(f"{p}:string", int(time.time()) + 60),
        "PEXPIREAT": lambda r, p: r.pexpireat(f"{p}:string", int(time.time() * 1000) + 60000),
        "TTL": lambda r, p: r.ttl(f"{p}:string"),
        "PTTL": lambda r, p: r.pttl(f"{p}:string"),
        "EXPIRETIME": lambda r, p: r.execute_command("EXPIRETIME", f"{p}:string"),
        "PEXPIRETIME": lambda r, p: r.execute_command("PEXPIRETIME", f"{p}:string"),
        "PERSIST": lambda r, p: r.persist(f"{p}:string"),
        "KEYS": lambda r, p: r.keys(f"{p}:*"),
        "SCAN": lambda r, p: r.scan(0, match=f"{p}:*", count=10),
        "TYPE": lambda r, p: r.type(f"{p}:string"),
        "DBSIZE": lambda r, p: r.dbsize(),
        "FLUSHDB": lambda r, p: r.execute_command("FLUSHDB"),
        "FLUSHALL": lambda r, p: r.execute_command("FLUSHALL"),
        "SADD": lambda r, p: r.sadd(f"{p}:set", b"a", b"b"),
        "SREM": lambda r, p: r.srem(f"{p}:set", b"a"),
        "SISMEMBER": lambda r, p: r.sismember(f"{p}:set", b"b"),
        "SMISMEMBER": lambda r, p: r.execute_command("SMISMEMBER", f"{p}:set", b"b", b"missing"),
        "SCARD": lambda r, p: r.scard(f"{p}:set"),
        "SMEMBERS": lambda r, p: r.smembers(f"{p}:set"),
        "SMOVE": lambda r, p: r.smove(f"{p}:set", f"{p}:set2", b"b"),
        "SPOP": lambda r, p: r.spop(f"{p}:set2"),
        "SRANDMEMBER": lambda r, p: r.srandmember(f"{p}:set2"),
        "SINTER": lambda r, p: r.sinter(f"{p}:set", f"{p}:set2"),
        "SINTERCARD": lambda r, p: r.execute_command("SINTERCARD", 2, f"{p}:set", f"{p}:set2"),
        "SUNION": lambda r, p: r.sunion(f"{p}:set", f"{p}:set2"),
        "SDIFF": lambda r, p: r.sdiff(f"{p}:set", f"{p}:set2"),
        "SINTERSTORE": lambda r, p: r.sinterstore(f"{p}:set3", f"{p}:set", f"{p}:set2"),
        "SUNIONSTORE": lambda r, p: r.sunionstore(f"{p}:set3", f"{p}:set", f"{p}:set2"),
        "SDIFFSTORE": lambda r, p: r.sdiffstore(f"{p}:set3", f"{p}:set", f"{p}:set2"),
        "SSCAN": lambda r, p: r.sscan(f"{p}:set", 0),
        "LPUSH": lambda r, p: r.lpush(f"{p}:list", b"a", b"b"),
        "RPUSH": lambda r, p: r.rpush(f"{p}:list", b"c"),
        "LPOP": lambda r, p: r.lpop(f"{p}:list"),
        "RPOP": lambda r, p: r.rpop(f"{p}:list"),
        "LLEN": lambda r, p: r.llen(f"{p}:list"),
        "LINDEX": lambda r, p: r.lindex(f"{p}:list", 0),
        "LRANGE": lambda r, p: r.lrange(f"{p}:list", 0, -1),
        "LSET": lambda r, p: r.lset(f"{p}:list", 0, b"x"),
        "LREM": lambda r, p: r.lrem(f"{p}:list", 0, b"x"),
        "LTRIM": lambda r, p: r.ltrim(f"{p}:list", 0, 1),
        "LINSERT": lambda r, p: r.linsert(f"{p}:list", "BEFORE", b"c", b"b"),
        "LPUSHX": lambda r, p: r.lpushx(f"{p}:list", b"left"),
        "RPUSHX": lambda r, p: r.rpushx(f"{p}:list", b"right"),
        "LPOS": lambda r, p: r.execute_command("LPOS", f"{p}:list", b"c"),
        "LMOVE": lambda r, p: r.execute_command("LMOVE", f"{p}:list", f"{p}:list2", "RIGHT", "LEFT"),
        "RPOPLPUSH": lambda r, p: r.rpoplpush(f"{p}:list2", f"{p}:list"),
        "HSET": lambda r, p: r.hset(f"{p}:hash", "f", b"v"),
        "HSETNX": lambda r, p: r.hsetnx(f"{p}:hash", "nx", b"v"),
        "HMSET": lambda r, p: r.execute_command("HMSET", f"{p}:hash", "hm", b"v"),
        "HGET": lambda r, p: r.hget(f"{p}:hash", "f"),
        "HMGET": lambda r, p: r.hmget(f"{p}:hash", ["f", "missing"]),
        "HGETALL": lambda r, p: r.hgetall(f"{p}:hash"),
        "HDEL": lambda r, p: r.hdel(f"{p}:hash", "f"),
        "HSCAN": lambda r, p: r.hscan(f"{p}:hash", 0),
        "HEXISTS": lambda r, p: r.hexists(f"{p}:hash", "f"),
        "HLEN": lambda r, p: r.hlen(f"{p}:hash"),
        "HKEYS": lambda r, p: r.hkeys(f"{p}:hash"),
        "HVALS": lambda r, p: r.hvals(f"{p}:hash"),
        "HSTRLEN": lambda r, p: r.execute_command("HSTRLEN", f"{p}:hash", "f"),
        "HINCRBY": lambda r, p: r.hincrby(f"{p}:hash", "n", 1),
        "HINCRBYFLOAT": lambda r, p: r.hincrbyfloat(f"{p}:hash", "n", 1.5),
        "HRANDFIELD": lambda r, p: r.execute_command("HRANDFIELD", f"{p}:hash", 1, "WITHVALUES"),
        "ZADD": lambda r, p: r.zadd(f"{p}:zset", {b"a": 1.0}),
        "ZSCORE": lambda r, p: r.zscore(f"{p}:zset", b"a"),
        "ZINCRBY": lambda r, p: r.zincrby(f"{p}:zset", 1.0, b"a"),
        "ZREM": lambda r, p: r.zrem(f"{p}:zset", b"a"),
        "ZCARD": lambda r, p: r.zcard(f"{p}:zset"),
        "ZRANGE": lambda r, p: r.zrange(f"{p}:zset", 0, -1),
        "ZREVRANGE": lambda r, p: r.zrevrange(f"{p}:zset", 0, -1),
        "ZRANGEBYSCORE": lambda r, p: r.zrangebyscore(f"{p}:zset", "-inf", "+inf"),
        "ZRANK": lambda r, p: r.zrank(f"{p}:zset", b"a"),
        "ZREVRANK": lambda r, p: r.zrevrank(f"{p}:zset", b"a"),
        "ZCOUNT": lambda r, p: r.zcount(f"{p}:zset", "-inf", "+inf"),
        "ZPOPMIN": lambda r, p: r.zpopmin(f"{p}:zset"),
        "ZPOPMAX": lambda r, p: r.zpopmax(f"{p}:zset"),
        "ZSCAN": lambda r, p: r.zscan(f"{p}:zset", 0),
        "PUBLISH": lambda r, p: r.publish(f"{p}:channel", b"message"),
        "PUBSUB": lambda r, p: r.execute_command("PUBSUB", "NUMPAT"),
    }


def mako_specific_checks(target: RedisTarget) -> dict[str, Any]:
    p = f"probe:mako:{int(time.time() * 1000)}"
    r = client(target)
    out: dict[str, Any] = {}
    try:
        try:
            r.set(b"\x01evil", b"value")
            out["reserved_prefix_rejected"] = False
        except redis.ResponseError:
            out["reserved_prefix_rejected"] = True
        info = r.info()
        out["info_mako_metrics"] = all(
            field in info for field in ["mako_txn_commits", "mako_txn_aborts", "mako_txn_retries"]
        )
        out["info_connection_metrics"] = all(
            field in info for field in ["connected_clients", "total_connections_received"]
        )
    finally:
        try:
            r.delete(p)
            r.close()
        except Exception:
            pass
    return out


def main() -> None:
    tiers = json.loads(TIERS_FILE.read_text())
    command_to_tier = {cmd: tier for tier, commands in tiers.items() for cmd in commands}
    cases = command_cases()
    missing_cases = sorted(set(command_to_tier) - set(cases) - {"SUBSCRIBE", "UNSUBSCRIBE", "PSUBSCRIBE", "PUNSUBSCRIBE"})
    if missing_cases:
        fail(f"missing probe cases: {', '.join(missing_cases)}")

    redis_target = target_from_env("REDIS", 6379)
    mako_target = target_from_env("MAKO", 6380)
    prefix = f"probe:{int(time.time() * 1000)}"
    results: dict[str, Any] = {"commands": {}, "summary": {}, "mako_checks": mako_specific_checks(mako_target)}

    for command in sorted(command_to_tier):
        if command not in cases:
            continue
        results["commands"][command] = {
            "tier": command_to_tier[command],
            "redis": call(cases[command], redis_target, f"{prefix}:redis"),
            "mako": call(cases[command], mako_target, f"{prefix}:mako"),
        }

    for tier, commands in tiers.items():
        scoped = [cmd for cmd in commands if cmd in results["commands"]]
        ok = sum(1 for cmd in scoped if results["commands"][cmd]["mako"]["status"] == "OK")
        results["summary"][tier] = {"ok": ok, "total": len(scoped)}

    OUT_FILE.parent.mkdir(parents=True, exist_ok=True)
    OUT_FILE.write_text(json.dumps(results, indent=2, sort_keys=True) + "\n")
    print(
        "probe results "
        + " ".join(f"{tier}={value['ok']}/{value['total']}" for tier, value in results["summary"].items())
        + f" out={OUT_FILE}"
    )
    p0 = results["summary"].get("P0", {"ok": 0, "total": 0})
    if p0["ok"] != p0["total"]:
        fail("P0 probe coverage has failures")
    if not all(results["mako_checks"].values()):
        fail("Mako-specific probe checks failed")


if __name__ == "__main__":
    main_guard(main)
