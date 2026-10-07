#!/usr/bin/env python3
"""Functional checks for the phase-2 Redis commands (no third-party deps).

Run against a live makoCon:  python3 test_phase2_commands.py HOST PORT [cluster]

The optional third argument (or MAKO_REDIS_CLUSTER_MODE=emulated in the
environment) says the server under test was started with
MAKO_REDIS_CLUSTER_MODE=emulated, so the CLUSTER block asserts the emulated
shapes instead of the "cluster support disabled" errors. Everything else is
identical in both modes.

Exits non-zero on the first failing assertion and prints a summary otherwise.
"""
import hashlib
import os
import re
import socket
import sys
import time


class Resp:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.buf = b""

    def send(self, *args):
        out = b"*%d\r\n" % len(args)
        for arg in args:
            if isinstance(arg, str):
                arg = arg.encode()
            out += b"$%d\r\n%s\r\n" % (len(arg), arg)
        self.sock.sendall(out)

    def _line(self):
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def _exact(self, n):
        while len(self.buf) < n + 2:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk
        data, self.buf = self.buf[:n], self.buf[n + 2:]
        return data

    def read(self):
        line = self._line()
        t, rest = line[:1], line[1:]
        if t == b"+":
            return rest.decode()
        if t == b"-":
            return Exception(rest.decode())
        if t == b":":
            return int(rest)
        if t == b"$":
            n = int(rest)
            return None if n < 0 else self._exact(n)
        if t == b"*":
            n = int(rest)
            return None if n < 0 else [self.read() for _ in range(n)]
        if t == b"%":
            n = int(rest)
            return {self.read(): self.read() for _ in range(n)}
        if t == b"_":
            return None
        raise ValueError("unexpected reply %r" % line)

    def cmd(self, *args):
        self.send(*args)
        return self.read()

    def read_pushed(self, timeout=0.6):
        """Collect whole CRLF-terminated lines that arrive within `timeout`.

        MONITOR pushes status lines with no request of their own, and every
        byte in one is printable ASCII (arguments are escaped), so splitting on
        CRLF is exact.
        """
        deadline = time.time() + timeout
        self.sock.settimeout(0.05)
        try:
            while time.time() < deadline:
                try:
                    chunk = self.sock.recv(65536)
                except socket.timeout:
                    continue
                if not chunk:
                    break
                self.buf += chunk
        finally:
            self.sock.settimeout(10)
        lines = []
        while b"\r\n" in self.buf:
            line, self.buf = self.buf.split(b"\r\n", 1)
            lines.append(line.decode("latin-1"))
        return lines


FAILED = []
PASSED = 0


def check(name, got, want=None, pred=None):
    global PASSED
    ok = pred(got) if pred else got == want
    if ok:
        PASSED += 1
    else:
        FAILED.append((name, got, want))
        print("FAIL %-40s got=%r want=%r" % (name, got, want))


def is_err(got, prefix=""):
    return isinstance(got, Exception) and str(got).startswith(prefix)


def cluster_emulated():
    """True when the server under test runs MAKO_REDIS_CLUSTER_MODE=emulated."""
    if len(sys.argv) > 3 and sys.argv[3].lower() in ("cluster", "emulated"):
        return True
    return os.environ.get("MAKO_REDIS_CLUSTER_MODE", "off").lower() == "emulated"


CLUSTER_DISABLED = "ERR This instance has cluster support disabled"


def info_section(c, section):
    """INFO <section> as a dict of its field:value lines."""
    body = c.cmd("INFO", section)
    if isinstance(body, bytes):
        body = body.decode()
    fields = {}
    for line in body.replace("\r\n", "\n").split("\n"):
        if line and not line.startswith("#") and ":" in line:
            name, value = line.split(":", 1)
            fields[name] = value
    return fields


def check_cluster(c, emulated):
    """CLUSTER, READONLY and READWRITE in whichever mode the server runs."""
    if not emulated:
        for args in (("CLUSTER", "INFO"), ("CLUSTER", "SLOTS"), ("CLUSTER", "MYID"),
                     ("CLUSTER", "NODES"), ("CLUSTER", "KEYSLOT", "foo"),
                     ("CLUSTER", "BOGUS")):
            check("%s with cluster support off" % " ".join(args), c.cmd(*args),
                  pred=lambda g: is_err(g, CLUSTER_DISABLED))
        check("READONLY with cluster support off", c.cmd("READONLY"),
              pred=lambda g: is_err(g, CLUSTER_DISABLED))
        check("READWRITE with cluster support off", c.cmd("READWRITE"),
              pred=lambda g: is_err(g, CLUSTER_DISABLED))
        check("INFO cluster reports cluster_enabled:0",
              info_section(c, "cluster").get("cluster_enabled"), "0")
        check("INFO default carries the cluster section",
              info_section(c, "default").get("cluster_enabled"), "0")
        check("CLUSTER with no subcommand is an arity error", c.cmd("CLUSTER"),
              pred=lambda g: is_err(g, "ERR wrong number of arguments"))
        return

    check("INFO cluster reports cluster_enabled:1",
          info_section(c, "cluster").get("cluster_enabled"), "1")
    check("INFO default carries the cluster section",
          info_section(c, "default").get("cluster_enabled"), "1")

    fields = {}
    body = c.cmd("CLUSTER", "INFO")
    for line in body.decode().replace("\r\n", "\n").split("\n"):
        if ":" in line:
            name, value = line.split(":", 1)
            fields[name] = value
    check("CLUSTER INFO cluster_state", fields.get("cluster_state"), "ok")
    check("CLUSTER INFO cluster_slots_assigned", fields.get("cluster_slots_assigned"), "16384")
    check("CLUSTER INFO cluster_slots_ok", fields.get("cluster_slots_ok"), "16384")
    check("CLUSTER INFO cluster_slots_pfail", fields.get("cluster_slots_pfail"), "0")
    check("CLUSTER INFO cluster_slots_fail", fields.get("cluster_slots_fail"), "0")
    check("CLUSTER INFO cluster_known_nodes", fields.get("cluster_known_nodes"), "1")
    check("CLUSTER INFO cluster_size", fields.get("cluster_size"), "1")
    check("CLUSTER INFO cluster_current_epoch", fields.get("cluster_current_epoch"), "1")
    check("CLUSTER INFO cluster_my_epoch", fields.get("cluster_my_epoch"), "1")
    check("CLUSTER INFO cluster_stats_messages_sent",
          fields.get("cluster_stats_messages_sent"), "0")
    check("CLUSTER INFO cluster_stats_messages_received",
          fields.get("cluster_stats_messages_received"), "0")

    node_id = c.cmd("CLUSTER", "MYID")
    check("CLUSTER MYID is 40 hex characters", node_id,
          pred=lambda g: isinstance(g, bytes) and re.fullmatch(rb"[0-9a-f]{40}", g))
    check("CLUSTER MYID is stable across calls", c.cmd("CLUSTER", "MYID"), node_id)

    slots = c.cmd("CLUSTER", "SLOTS")
    check("CLUSTER SLOTS has one range", slots, pred=lambda g: isinstance(g, list) and len(g) == 1)
    entry = slots[0]
    check("CLUSTER SLOTS range starts at 0", entry[0], 0)
    check("CLUSTER SLOTS range ends at 16383", entry[1], 16383)
    check("CLUSTER SLOTS names one node", len(entry[2]), 3)
    check("CLUSTER SLOTS node carries the advertised id", entry[2][2], node_id)
    slot_host, slot_port = entry[2][0], entry[2][1]
    check("CLUSTER SLOTS node port is an integer", slot_port, pred=lambda g: isinstance(g, int))

    shards = c.cmd("CLUSTER", "SHARDS")
    check("CLUSTER SHARDS has one shard", shards,
          pred=lambda g: isinstance(g, list) and len(g) == 1)
    shard = dict(zip(shards[0][0::2], shards[0][1::2]))
    check("CLUSTER SHARDS slots", shard.get(b"slots"), [0, 16383])
    nodes = shard.get(b"nodes")
    check("CLUSTER SHARDS lists one node", nodes,
          pred=lambda g: isinstance(g, list) and len(g) == 1)
    node = dict(zip(nodes[0][0::2], nodes[0][1::2]))
    check("CLUSTER SHARDS node id", node.get(b"id"), node_id)
    check("CLUSTER SHARDS node port", node.get(b"port"), slot_port)
    check("CLUSTER SHARDS node ip", node.get(b"ip"), slot_host)
    check("CLUSTER SHARDS node endpoint", node.get(b"endpoint"), slot_host)
    check("CLUSTER SHARDS node role", node.get(b"role"), b"master")
    check("CLUSTER SHARDS node replication-offset", node.get(b"replication-offset"), 0)
    check("CLUSTER SHARDS node health", node.get(b"health"), b"online")

    lines = c.cmd("CLUSTER", "NODES").decode().split("\n")
    check("CLUSTER NODES has one line and a trailing newline", lines,
          pred=lambda g: len(g) == 2 and g[1] == "")
    want = "%s %s:%d@%d myself,master - 0 0 1 connected 0-16383" % (
        node_id.decode(), slot_host.decode(), slot_port, slot_port + 10000)
    check("CLUSTER NODES line", lines[0], want)

    # Redis documents these three CLUSTER KEYSLOT results.
    check("CLUSTER KEYSLOT foo", c.cmd("CLUSTER", "KEYSLOT", "foo"), 12182)
    check("CLUSTER KEYSLOT somekey", c.cmd("CLUSTER", "KEYSLOT", "somekey"), 11058)
    check("CLUSTER KEYSLOT foo{hash_tag}", c.cmd("CLUSTER", "KEYSLOT", "foo{hash_tag}"), 2515)
    # A hash tag puts two different keys in the same slot.
    check("CLUSTER KEYSLOT {user1000}.following",
          c.cmd("CLUSTER", "KEYSLOT", "{user1000}.following"), 3443)
    check("CLUSTER KEYSLOT {user1000}.followers",
          c.cmd("CLUSTER", "KEYSLOT", "{user1000}.followers"), 3443)
    check("CLUSTER KEYSLOT {user1000}", c.cmd("CLUSTER", "KEYSLOT", "{user1000}"), 3443)
    # An empty or unterminated tag hashes the whole key.
    check("CLUSTER KEYSLOT {}", c.cmd("CLUSTER", "KEYSLOT", "{}"), 15257)
    check("CLUSTER KEYSLOT foo{}{bar}", c.cmd("CLUSTER", "KEYSLOT", "foo{}{bar}"), 8363)
    check("CLUSTER KEYSLOT {}{foo}", c.cmd("CLUSTER", "KEYSLOT", "{}{foo}"), 2263)
    check("CLUSTER KEYSLOT of an empty key", c.cmd("CLUSTER", "KEYSLOT", ""), 0)
    check("CLUSTER KEYSLOT arity", c.cmd("CLUSTER", "KEYSLOT"),
          pred=lambda g: is_err(g, "ERR wrong number of arguments"))

    check("CLUSTER COUNTKEYSINSLOT", c.cmd("CLUSTER", "COUNTKEYSINSLOT", "0"), 0)
    check("CLUSTER COUNTKEYSINSLOT out of range",
          c.cmd("CLUSTER", "COUNTKEYSINSLOT", "16384"),
          pred=lambda g: is_err(g, "ERR Invalid slot"))
    check("CLUSTER GETKEYSINSLOT", c.cmd("CLUSTER", "GETKEYSINSLOT", "0", "10"), [])
    check("CLUSTER HELP", c.cmd("CLUSTER", "HELP"),
          pred=lambda g: isinstance(g, list) and g and g[0].startswith(b"CLUSTER <subcommand>"))
    check("CLUSTER unknown subcommand", c.cmd("CLUSTER", "NOSUCHTHING"),
          pred=lambda g: is_err(
              g, "ERR unknown subcommand 'NOSUCHTHING'. Try CLUSTER HELP."))

    check("READONLY", c.cmd("READONLY"), "OK")
    check("READWRITE", c.cmd("READWRITE"), "OK")
    check("READONLY arity", c.cmd("READONLY", "x"),
          pred=lambda g: is_err(g, "ERR wrong number of arguments"))

    # Both are queued inside MULTI like the other admin commands.
    check("MULTI before the cluster commands", c.cmd("MULTI"), "OK")
    check("CLUSTER MYID queues", c.cmd("CLUSTER", "MYID"), "QUEUED")
    check("READONLY queues", c.cmd("READONLY"), "QUEUED")
    check("READWRITE queues", c.cmd("READWRITE"), "QUEUED")
    check("EXEC replies to the queued cluster commands", c.cmd("EXEC"),
          [node_id, "OK", "OK"])


MONITOR_LINE = re.compile(r"^\+(\d+)\.(\d{6}) \[(\d+) (\S+:\d+)\] (.*)$")


def monitor_commands(lines):
    """The command name of each well-formed monitor line, in order."""
    names = []
    for line in lines:
        match = MONITOR_LINE.match(line)
        if match:
            names.append(match.group(5).split(" ", 1)[0])
    return names


def check_monitor(host, port, c):
    """MONITOR: two extra connections, one watching the other."""
    watcher = Resp(host, port)
    worker = Resp(host, port)

    check("MONITOR replies OK", watcher.cmd("MONITOR"), "OK")
    check("INFO clients counts the monitor",
          info_section(c, "clients").get("monitor_clients"), "1")
    # INFO above was itself a command, so drain whatever it produced.
    watcher.read_pushed(0.3)

    worker.cmd("SET", "mon:k", "v")
    worker.cmd("GET", "mon:k")
    worker.cmd("MULTI")
    worker.cmd("SET", "mon:t", "1")
    worker.cmd("EXEC")
    worker.cmd("SET", "mon:bin", b"bin\x00\xffz")
    worker.cmd("SET", "mon:q", b'say "hi"\n')
    # A plain two-part GET is what the raw fast path would otherwise answer
    # without ever building a command; with a monitor attached it has to fall
    # through to the general parser and be reported like everything else.
    worker.cmd("GET", "mon:k")

    lines = watcher.read_pushed()
    check("every monitor line matches the Redis format", lines,
          pred=lambda got: got and all(MONITOR_LINE.match(line) for line in got))
    check("MONITOR reports MULTI, the queued command and EXEC",
          monitor_commands(lines),
          ['"SET"', '"GET"', '"MULTI"', '"SET"', '"EXEC"', '"SET"', '"SET"', '"GET"'])
    bodies = [MONITOR_LINE.match(line).group(5) for line in lines
              if MONITOR_LINE.match(line)]
    check("MONITOR quotes a plain command", bodies[0], '"SET" "mon:k" "v"')
    check("MONITOR escapes a binary argument as \\xHH", bodies[5],
          '"SET" "mon:bin" "bin\\x00\\xffz"')
    check("MONITOR escapes quotes and newlines", bodies[6],
          '"SET" "mon:q" "say \\"hi\\"\\n"')
    check("MONITOR reports the fast-path GET", bodies[7], '"GET" "mon:k"')

    timestamps = [float("%s.%s" % (MONITOR_LINE.match(line).group(1),
                                   MONITOR_LINE.match(line).group(2)))
                  for line in lines if MONITOR_LINE.match(line)]
    check("monitor timestamps are plausible unix seconds", timestamps,
          pred=lambda got: all(1_700_000_000 < value < 4_000_000_000 for value in got))
    check("monitor lines report db 0", lines,
          pred=lambda got: all(MONITOR_LINE.match(line).group(3) == "0"
                               for line in got if MONITOR_LINE.match(line)))
    peers = {MONITOR_LINE.match(line).group(4) for line in lines
             if MONITOR_LINE.match(line)}
    check("monitor lines carry one peer address, the issuing client's", peers,
          pred=lambda got: len(got) == 1 and got.pop().startswith("127.0.0.1:"))

    # Redis redacts credentials before they reach a monitor.
    worker.cmd("AUTH", "user", "hunter2")
    worker.cmd("HELLO", "2", "AUTH", "user", "hunter2")
    redacted = [MONITOR_LINE.match(line).group(5) for line in watcher.read_pushed()
                if MONITOR_LINE.match(line)]
    check("MONITOR redacts AUTH arguments", redacted,
          pred=lambda got: len(got) == 2
          and got[0] == '"AUTH" "(redacted)" "(redacted)"')
    check("MONITOR redacts HELLO AUTH arguments", redacted[1] if len(redacted) > 1 else None,
          '"HELLO" "2" "AUTH" "(redacted)" "(redacted)"')

    # The monitor still gets its own replies, and does not see its own commands.
    check("the monitor's own PING is answered", watcher.cmd("PING"), "PONG")
    check("the monitor's own PING is not echoed to it", watcher.read_pushed(0.4), [])

    # RESET leaves monitor mode; nothing more reaches the watcher.
    check("RESET on the monitor", watcher.cmd("RESET"), "RESET")
    check("INFO clients no longer counts a monitor",
          info_section(c, "clients").get("monitor_clients"), "0")
    worker.cmd("SET", "mon:after", "v")
    worker.cmd("GET", "mon:after")
    check("no line reaches the client after RESET", watcher.read_pushed(0.5), [])

    # MONITOR is queued inside MULTI and refused at EXEC, as in Redis.
    check("MULTI before MONITOR", watcher.cmd("MULTI"), "OK")
    check("MONITOR queues", watcher.cmd("MONITOR"), "QUEUED")
    check("EXEC refuses the queued MONITOR", watcher.cmd("EXEC"),
          pred=lambda got: isinstance(got, list) and len(got) == 1
          and is_err(got[0], "ERR MONITOR isn't allowed for DENY BLOCKING client"))
    check("the refused MONITOR left no monitor attached",
          info_section(c, "clients").get("monitor_clients"), "0")

    # A disconnect unregisters the monitor even without RESET or QUIT.
    dropped = Resp(host, port)
    check("MONITOR on a connection about to drop", dropped.cmd("MONITOR"), "OK")
    check("INFO clients counts it", info_section(c, "clients").get("monitor_clients"), "1")
    dropped.sock.close()
    deadline = time.time() + 5
    while time.time() < deadline:
        c.cmd("PING")
        if info_section(c, "clients").get("monitor_clients") == "0":
            break
        time.sleep(0.1)
    check("a disconnect unregisters the monitor",
          info_section(c, "clients").get("monitor_clients"), "0")

    # QUIT leaves monitor mode too.
    quitter = Resp(host, port)
    check("MONITOR before QUIT", quitter.cmd("MONITOR"), "OK")
    check("QUIT from a monitor", quitter.cmd("QUIT"), "OK")
    quitter.sock.close()
    deadline = time.time() + 5
    while time.time() < deadline:
        if info_section(c, "clients").get("monitor_clients") == "0":
            break
        time.sleep(0.1)
    check("QUIT leaves monitor mode",
          info_section(c, "clients").get("monitor_clients"), "0")

    # With no monitor attached the raw GET/SET fast path is back: the same two
    # commands still answer correctly and INFO stats still counts them.
    before = int(info_section(c, "stats").get("total_commands_processed", "0"))
    worker.cmd("SET", "mon:fast", "v")
    check("SET still works with the fast path restored", worker.cmd("GET", "mon:fast"), b"v")
    after = int(info_section(c, "stats").get("total_commands_processed", "0"))
    check("the fast path still counts its commands", after - before,
          pred=lambda got: got >= 3)

    watcher.sock.close()
    worker.sock.close()


def check_logical_databases(host, port):
    """SELECT, MOVE, COPY ... DB and per-database keyspace scoping.

    Every block opens its own connection so the selected database is explicit
    and nothing later in the file inherits a database other than 0.
    """
    def fresh(db=0):
        conn = Resp(host, port)
        if db:
            conn.cmd("SELECT", str(db))
        return conn

    c0 = fresh()
    c0.cmd("FLUSHALL")

    # ----- SELECT and the basic separation -----
    check("CONFIG GET databases reports 16", c0.cmd("CONFIG", "GET", "databases"),
          [b"databases", b"16"])
    check("SELECT 0", c0.cmd("SELECT", "0"), "OK")
    check("SELECT 15", c0.cmd("SELECT", "15"), "OK")
    check("SELECT 16 is out of range", c0.cmd("SELECT", "16"),
          pred=lambda got: is_err(got, "ERR DB index is out of range"))
    check("SELECT -1 is out of range", c0.cmd("SELECT", "-1"),
          pred=lambda got: is_err(got, "ERR DB index is out of range"))
    check("SELECT of a non-number is an integer error", c0.cmd("SELECT", "abc"),
          pred=lambda got: is_err(got, "ERR value is not an integer"))
    c0.cmd("SELECT", "0")

    c1 = fresh(1)
    check("SET in database 1", c1.cmd("SET", "dbk", "v1"), "OK")
    check("database 0 does not see it", c0.cmd("GET", "dbk"), None)
    check("SET the same name in database 0", c0.cmd("SET", "dbk", "v0"), "OK")
    check("database 1 still has its own value", c1.cmd("GET", "dbk"), b"v1")
    check("database 0 has its own value", c0.cmd("GET", "dbk"), b"v0")
    check("EXISTS is per database", [c0.cmd("EXISTS", "dbk"), c1.cmd("EXISTS", "dbk")], [1, 1])
    check("TYPE is per database", c1.cmd("TYPE", "dbk"), "string")
    c1.cmd("EXPIRE", "dbk", "100")
    check("TTL is per database", [c1.cmd("TTL", "dbk"), c0.cmd("TTL", "dbk")], [100, -1])
    c1.cmd("PERSIST", "dbk")

    # ----- KEYS, SCAN, DBSIZE, RANDOMKEY are scoped and report raw names -----
    check("DBSIZE counts only database 0", c0.cmd("DBSIZE"), 1)
    check("DBSIZE counts only database 1", c1.cmd("DBSIZE"), 1)
    c1.cmd("SET", "only1", "x")
    check("KEYS in database 1 lists its own names unprefixed",
          sorted(c1.cmd("KEYS", "*")), [b"dbk", b"only1"])
    check("KEYS in database 0 does not see database 1", c0.cmd("KEYS", "*"), [b"dbk"])
    check("KEYS MATCH still means what the client wrote",
          c1.cmd("KEYS", "only*"), [b"only1"])
    check("SCAN in database 1 lists its own names",
          sorted(c1.cmd("SCAN", "0")[1]), [b"dbk", b"only1"])
    check("SCAN in database 0 does not see database 1", c0.cmd("SCAN", "0")[1], [b"dbk"])
    check("SCAN MATCH is applied to the unprefixed name",
          c1.cmd("SCAN", "0", "MATCH", "only*")[1], [b"only1"])
    check("RANDOMKEY in database 0 never returns a database 1 key",
          [c0.cmd("RANDOMKEY") for _ in range(20)],
          pred=lambda got: all(value == b"dbk" for value in got))
    check("RANDOMKEY in database 1 never returns a database 0 key",
          [c1.cmd("RANDOMKEY") for _ in range(20)],
          pred=lambda got: all(value in (b"dbk", b"only1") for value in got))
    c1.cmd("DEL", "only1")

    # ----- every type works in database 1 and is invisible from database 0 -----
    c1.cmd("SADD", "t:set", "a", "b")
    c1.cmd("RPUSH", "t:list", "x", "y")
    c1.cmd("HSET", "t:hash", "f", "v")
    c1.cmd("ZADD", "t:zset", "1", "m")
    c1.cmd("PFADD", "t:hll", "a", "b", "c")
    c1.cmd("GEOADD", "t:geo", "13.361389", "38.115556", "Palermo")
    c1.cmd("BITFIELD", "t:bf", "SET", "u8", "0", "255")
    check("SMEMBERS in database 1", sorted(c1.cmd("SMEMBERS", "t:set")), [b"a", b"b"])
    check("LRANGE in database 1", c1.cmd("LRANGE", "t:list", "0", "-1"), [b"x", b"y"])
    check("HGETALL in database 1", c1.cmd("HGETALL", "t:hash"), [b"f", b"v"])
    check("ZRANGE in database 1", c1.cmd("ZRANGE", "t:zset", "0", "-1"), [b"m"])
    check("PFCOUNT in database 1", c1.cmd("PFCOUNT", "t:hll"), 3)
    check("GEOPOS in database 1", c1.cmd("GEOPOS", "t:geo", "Palermo"),
          pred=lambda got: got and got[0] and abs(float(got[0][0]) - 13.361389) < 1e-4)
    check("BITFIELD in database 1", c1.cmd("BITFIELD", "t:bf", "GET", "u8", "0"), [255])
    check("database 0 sees none of the typed keys in database 1",
          [c0.cmd("SCARD", "t:set"), c0.cmd("LLEN", "t:list"), c0.cmd("HLEN", "t:hash"),
           c0.cmd("ZCARD", "t:zset"), c0.cmd("PFCOUNT", "t:hll"),
           c0.cmd("EXISTS", "t:geo"), c0.cmd("EXISTS", "t:bf")],
          [0, 0, 0, 0, 0, 0, 0])

    # ----- FLUSHDB is per database, FLUSHALL clears everything -----
    c0.cmd("SET", "f0", "v")
    c0.cmd("SADD", "f0set", "m")
    check("FLUSHDB in database 1", c1.cmd("FLUSHDB"), "OK")
    check("database 1 is empty afterwards",
          [c1.cmd("DBSIZE"), c1.cmd("SCARD", "t:set"), c1.cmd("LLEN", "t:list"),
           c1.cmd("HLEN", "t:hash"), c1.cmd("ZCARD", "t:zset")], [0, 0, 0, 0, 0])
    check("database 0 is untouched by a FLUSHDB in database 1",
          [c0.cmd("GET", "f0"), c0.cmd("SCARD", "f0set"), c0.cmd("GET", "dbk")],
          [b"v", 1, b"v0"])
    c1.cmd("SET", "f1", "v")
    c1.cmd("SADD", "f1set", "m")
    check("FLUSHDB in database 0", c0.cmd("FLUSHDB"), "OK")
    check("database 0 is empty afterwards", c0.cmd("DBSIZE"), 0)
    check("database 1 is untouched by a FLUSHDB in database 0",
          [c1.cmd("GET", "f1"), c1.cmd("SCARD", "f1set")], [b"v", 1])
    check("FLUSHALL", c0.cmd("FLUSHALL"), "OK")
    check("FLUSHALL cleared database 1 too",
          [c1.cmd("DBSIZE"), c1.cmd("SCARD", "f1set")], [0, 0])

    # ----- MOVE -----
    c0.cmd("SET", "mv:str", "value")
    c0.cmd("EXPIRE", "mv:str", "100")
    c0.cmd("HSET", "mv:hash", "f", "v")
    c0.cmd("SADD", "mv:set", "a", "b")
    c0.cmd("ZADD", "mv:zset", "1.5", "m")
    c0.cmd("RPUSH", "mv:list", "x", "y")
    for name in ("mv:str", "mv:hash", "mv:set", "mv:zset", "mv:list"):
        check("MOVE %s to database 1" % name, c0.cmd("MOVE", name, "1"), 1)
    check("the moved keys are gone from database 0",
          [c0.cmd("EXISTS", name) for name in
           ("mv:str", "mv:hash", "mv:set", "mv:zset", "mv:list")], [0, 0, 0, 0, 0])
    check("the moved string arrived with its value", c1.cmd("GET", "mv:str"), b"value")
    check("the moved string kept its type", c1.cmd("TYPE", "mv:str"), "string")
    check("the moved string kept its TTL", c1.cmd("TTL", "mv:str"),
          pred=lambda got: 90 < got <= 100)
    check("the moved hash arrived", c1.cmd("HGETALL", "mv:hash"), [b"f", b"v"])
    check("the moved set arrived", sorted(c1.cmd("SMEMBERS", "mv:set")), [b"a", b"b"])
    check("the moved zset arrived with its score",
          c1.cmd("ZRANGE", "mv:zset", "0", "-1", "WITHSCORES"), [b"m", b"1.5"])
    check("the moved list arrived in order",
          c1.cmd("LRANGE", "mv:list", "0", "-1"), [b"x", b"y"])
    check("the moved types are right in database 1",
          [c1.cmd("TYPE", "mv:hash"), c1.cmd("TYPE", "mv:set"),
           c1.cmd("TYPE", "mv:zset"), c1.cmd("TYPE", "mv:list")],
          ["hash", "set", "zset", "list"])

    c0.cmd("SET", "mv:taken", "from0")
    c1.cmd("SET", "mv:taken", "from1")
    check("MOVE onto an existing destination returns 0", c0.cmd("MOVE", "mv:taken", "1"), 0)
    check("the source is untouched after a refused MOVE", c0.cmd("GET", "mv:taken"), b"from0")
    check("the destination is untouched after a refused MOVE",
          c1.cmd("GET", "mv:taken"), b"from1")
    check("MOVE of a missing key returns 0", c0.cmd("MOVE", "mv:missing", "1"), 0)
    check("MOVE to the current database is an error", c0.cmd("MOVE", "mv:taken", "0"),
          pred=lambda got: is_err(got, "ERR source and destination objects are the same"))
    check("MOVE to a database out of range", c0.cmd("MOVE", "mv:taken", "16"),
          pred=lambda got: is_err(got, "ERR DB index is out of range"))
    check("MOVE to a negative database", c0.cmd("MOVE", "mv:taken", "-1"),
          pred=lambda got: is_err(got, "ERR DB index is out of range"))
    check("MOVE arity", c0.cmd("MOVE", "mv:taken"),
          pred=lambda got: is_err(got, "ERR wrong number of arguments"))

    # ----- COPY ... DB -----
    c2 = fresh(2)
    c0.cmd("SET", "cp:src", "payload")
    check("COPY into database 2", c0.cmd("COPY", "cp:src", "cp:dst", "DB", "2"), 1)
    check("the copy is not in database 0", c0.cmd("EXISTS", "cp:dst"), 0)
    check("the copy is not in database 1", c1.cmd("EXISTS", "cp:dst"), 0)
    check("the copy is in database 2", c2.cmd("GET", "cp:dst"), b"payload")
    check("the source stayed in database 0", c0.cmd("GET", "cp:src"), b"payload")
    check("COPY onto an existing name in database 2 returns 0",
          c0.cmd("COPY", "cp:src", "cp:dst", "DB", "2"), 0)
    c0.cmd("SET", "cp:src", "second")
    check("COPY ... DB ... REPLACE overwrites",
          c0.cmd("COPY", "cp:src", "cp:dst", "DB", "2", "REPLACE"), 1)
    check("the destination holds the new value", c2.cmd("GET", "cp:dst"), b"second")
    check("COPY onto itself in the same database is refused",
          c0.cmd("COPY", "cp:src", "cp:src", "DB", "0"),
          pred=lambda got: is_err(got, "ERR source and destination objects are the same"))
    check("COPY ... DB out of range", c0.cmd("COPY", "cp:src", "cp:dst", "DB", "16"),
          pred=lambda got: is_err(got, "ERR DB index is out of range"))

    # ----- the reserved first byte -----
    check("SET of a key starting with 0x02 is refused", c0.cmd("SET", b"\x02bad", "v"),
          pred=lambda got: is_err(got, "ERR invalid key: reserved internal prefix"))
    check("GET of a key starting with 0x02 is refused", c0.cmd("GET", b"\x02bad"),
          pred=lambda got: is_err(got, "ERR invalid key: reserved internal prefix"))
    check("a key starting with 0x01 is still refused", c0.cmd("SET", b"\x01bad", "v"),
          pred=lambda got: is_err(got, "ERR invalid key: reserved internal prefix"))

    # ----- multi-key commands stay inside one database -----
    c1.cmd("FLUSHDB")
    c1.cmd("SET", "mk:rename", "v")
    check("RENAME within database 1", c1.cmd("RENAME", "mk:rename", "mk:renamed"), "OK")
    check("the renamed key is in database 1", c1.cmd("GET", "mk:renamed"), b"v")
    check("the renamed key is not in database 0", c0.cmd("EXISTS", "mk:renamed"), 0)
    c1.cmd("RPUSH", "mk:sort", "3", "1", "2")
    check("SORT STORE within database 1",
          c1.cmd("SORT", "mk:sort", "STORE", "mk:sorted"), 3)
    check("the sorted copy is in database 1",
          c1.cmd("LRANGE", "mk:sorted", "0", "-1"), [b"1", b"2", b"3"])
    check("the sorted copy is not in database 0", c0.cmd("EXISTS", "mk:sorted"), 0)
    c1.cmd("SADD", "mk:sa", "x")
    c1.cmd("SADD", "mk:sb", "y")
    check("SUNIONSTORE within database 1",
          c1.cmd("SUNIONSTORE", "mk:sdst", "mk:sa", "mk:sb"), 2)
    check("the union is in database 1", sorted(c1.cmd("SMEMBERS", "mk:sdst")), [b"x", b"y"])
    check("the union is not in database 0", c0.cmd("EXISTS", "mk:sdst"), 0)

    # ----- MULTI/EXEC and WATCH inside a database -----
    check("MULTI in database 1", c1.cmd("MULTI"), "OK")
    check("queued SET", c1.cmd("SET", "mk:txn", "1"), "QUEUED")
    check("queued INCR", c1.cmd("INCR", "mk:txn"), "QUEUED")
    check("EXEC in database 1", c1.cmd("EXEC"), ["OK", 2])
    check("the transaction wrote into database 1", c1.cmd("GET", "mk:txn"), b"2")
    check("the transaction did not write into database 0", c0.cmd("EXISTS", "mk:txn"), 0)

    watcher = fresh(1)
    writer0 = fresh(0)
    writer1 = fresh(1)
    watcher.cmd("SET", "wk", "start")
    check("WATCH in database 1", watcher.cmd("WATCH", "wk"), "OK")
    writer0.cmd("SET", "wk", "written in database 0")
    watcher.cmd("MULTI")
    watcher.cmd("GET", "wk")
    check("a write to the same name in database 0 does not break the watch",
          watcher.cmd("EXEC"), [b"start"])
    check("WATCH in database 1 again", watcher.cmd("WATCH", "wk"), "OK")
    writer1.cmd("SET", "wk", "written in database 1")
    watcher.cmd("MULTI")
    watcher.cmd("GET", "wk")
    check("a write in database 1 does break the watch", watcher.cmd("EXEC"), None)

    # ----- blocking pops return the unprefixed key name -----
    popper = fresh(1)
    pusher = fresh(1)
    popper.cmd("DEL", "bl")
    pusher.send("RPUSH", "bl", "val")
    pusher.read()
    check("BLPOP in database 1 returns the raw key name",
          popper.cmd("BLPOP", "bl", "2"), [b"bl", b"val"])
    pusher.cmd("RPUSH", "bl2", "q")
    check("LMPOP in database 1 returns the raw key name",
          popper.cmd("LMPOP", "1", "bl2", "LEFT"), [b"bl2", [b"q"]])
    pusher.cmd("ZADD", "bz", "1", "m")
    check("BZPOPMIN in database 1 returns the raw key name",
          popper.cmd("BZPOPMIN", "bz", "2"), [b"bz", b"m", b"1"])
    pusher.cmd("ZADD", "bz2", "1", "m")
    check("ZMPOP in database 1 returns the raw key name",
          popper.cmd("ZMPOP", "1", "bz2", "MIN"), [b"bz2", [[b"m", b"1"]]])

    # A blocking pop that actually parks and is woken from another connection
    # still comes back with the raw name: it resumes on whichever worker picks
    # the client up, so the database has to travel with the connection.
    popper.cmd("DEL", "bl3")
    parked = fresh(1)
    parked.send("BLPOP", "bl3", "5")
    time.sleep(0.3)
    pusher.cmd("RPUSH", "bl3", "late")
    check("a parked BLPOP in database 1 wakes with the raw key name",
          parked.read(), [b"bl3", b"late"])

    # ----- MONITOR reports the issuing connection's database and the raw key -----
    watch_mon = Resp(host, port)
    check("MONITOR for the database check", watch_mon.cmd("MONITOR"), "OK")
    watch_mon.read_pushed(0.3)
    worker_mon = fresh(1)
    worker_mon.cmd("SET", "mon:db1", "v")
    mon_lines = [line for line in watch_mon.read_pushed() if MONITOR_LINE.match(line)]
    bodies = [MONITOR_LINE.match(line) for line in mon_lines]
    check("MONITOR reports SELECT on the database the client was in",
          [(m.group(3), m.group(5)) for m in bodies if m.group(5).startswith('"SELECT"')],
          [("0", '"SELECT" "1"')])
    check("MONITOR reports the command in database 1 with the raw key",
          [(m.group(3), m.group(5)) for m in bodies if m.group(5).startswith('"SET"')],
          [("1", '"SET" "mon:db1" "v"')])
    watch_mon.cmd("RESET")
    watch_mon.sock.close()
    worker_mon.sock.close()

    # ----- INFO keyspace -----
    info = c0.cmd("INFO", "keyspace")
    if isinstance(info, bytes):
        info = info.decode()
    info = info.replace("\r\n", "\n")
    check("INFO keyspace reports db0", info,
          pred=lambda got: re.search(r"^db0:keys=\d+,expires=0,avg_ttl=0$", got,
                                     re.M) is not None)
    if os.environ.get("MAKO_REDIS_INFO_ALL_DBS") == "1":
        # Only a server started with MAKO_REDIS_INFO_ALL_DBS=1 scans the other
        # fifteen databases; the default reports database 0 alone.
        c1.cmd("SET", "info:db1", "v")
        time.sleep(2.1)  # the per-database count is cached for two seconds
        info = c0.cmd("INFO", "keyspace")
        if isinstance(info, bytes):
            info = info.decode()
        info = info.replace("\r\n", "\n")
        check("INFO keyspace reports db1 when asked for every database", info,
              pred=lambda got: re.search(r"^db1:keys=[1-9]\d*,", got, re.M) is not None)
        c1.cmd("DEL", "info:db1")
    else:
        check("INFO keyspace reports no other database by default", info,
              pred=lambda got: re.search(r"^db[1-9]", got, re.M) is None)

    # ----- RESET returns the connection to database 0 -----
    resetter = fresh(3)
    resetter.cmd("SET", "reset:k", "in3")
    check("RESET from database 3", resetter.cmd("RESET"), "RESET")
    check("the connection is back on database 0 after RESET",
          resetter.cmd("EXISTS", "reset:k"), 0)
    c4 = fresh(4)
    check("CLIENT LIST reports the selected database", c4.cmd("CLIENT", "LIST"),
          pred=lambda got: isinstance(got, bytes) and b"db=4" in got)
    c4.sock.close()
    resetter.sock.close()

    # Leave the keyspace as the rest of the file expects: one database, empty.
    c0.cmd("FLUSHALL")
    for conn in (c1, c2, watcher, writer0, writer1, popper, pusher, parked):
        conn.sock.close()
    c0.sock.close()


def check_scripting(host, port, c):
    """EVAL, EVALSHA, EVAL_RO, EVALSHA_RO and SCRIPT.

    Scripts run inside one interactive transaction, so everything here is also
    a check on that transaction: read-your-writes between two redis.call
    invocations, nothing visible until the script returns, and nothing at all
    visible when it fails.
    """
    # ----- the redis.io EVAL examples -----
    check("EVAL returns KEYS and ARGV",
          c.cmd("EVAL", "return {KEYS[1],KEYS[2],ARGV[1],ARGV[2]}", "2",
                "key1", "key2", "first", "second"),
          [b"key1", b"key2", b"first", b"second"])
    c.cmd("DEL", "lua:foo")
    check("EVAL SET returns the status reply",
          c.cmd("EVAL", "return redis.call('set', KEYS[1], 'bar')", "1", "lua:foo"),
          "OK")
    check("EVAL GET returns the value the script wrote",
          c.cmd("EVAL", "return redis.call('get', KEYS[1])", "1", "lua:foo"),
          b"bar")
    check("the write a script made is visible outside it",
          c.cmd("GET", "lua:foo"), b"bar")

    # ----- Lua to RESP -----
    check("number truncates to an integer", c.cmd("EVAL", "return 3.9", "0"), 3)
    check("negative number truncates toward zero",
          c.cmd("EVAL", "return -3.9", "0"), -3)
    check("string returns a bulk", c.cmd("EVAL", "return 'hello'", "0"), b"hello")
    check("true returns 1", c.cmd("EVAL", "return true", "0"), 1)
    check("false returns nil", c.cmd("EVAL", "return false", "0"), None)
    check("no return value is nil", c.cmd("EVAL", "local x = 1", "0"), None)
    check("a table returns an array",
          c.cmd("EVAL", "return {1,2,3,'ciao',{1,2}}", "0"),
          [1, 2, 3, b"ciao", [1, 2]])
    check("an array stops at the first nil",
          c.cmd("EVAL", "return {1,2,nil,4}", "0"), [1, 2])
    check("a table with ok returns a status reply",
          c.cmd("EVAL", "return {ok='fine'}", "0"), "fine")
    check("a table with err returns an error",
          c.cmd("EVAL", "return {err='My Error'}", "0"),
          pred=lambda g: is_err(g, "My Error"))
    check("redis.status_reply builds a status reply",
          c.cmd("EVAL", "return redis.status_reply('TEST')", "0"), "TEST")
    check("redis.error_reply builds an error",
          c.cmd("EVAL", "return redis.error_reply('My Error')", "0"),
          pred=lambda g: is_err(g, "My Error"))

    # ----- RESP to Lua -----
    c.cmd("DEL", "lua:missing")
    check("a missing key reaches Lua as false",
          c.cmd("EVAL", "return type(redis.call('get', KEYS[1]))", "1", "lua:missing"),
          b"boolean")
    check("a status reply reaches Lua as a table with ok",
          c.cmd("EVAL", "return redis.call('set', KEYS[1], 'v')['ok']", "1", "lua:conv"),
          b"OK")
    check("an integer reply reaches Lua as a number",
          c.cmd("EVAL",
                "redis.call('set', KEYS[1], '41') "
                "return redis.call('incr', KEYS[1]) + 1", "1", "lua:conv"),
          43)
    check("a bulk reply reaches Lua as a string",
          c.cmd("EVAL", "return redis.call('get', KEYS[1]) .. '!'", "1", "lua:conv"),
          b"42!")
    c.cmd("DEL", "lua:list")
    c.cmd("RPUSH", "lua:list", "a", "b")
    check("a multi-bulk reply reaches Lua as a table",
          c.cmd("EVAL", "return redis.call('lrange', KEYS[1], 0, -1)", "1", "lua:list"),
          [b"a", b"b"])
    check("redis.pcall hands an error back as a table",
          c.cmd("EVAL", "local reply = redis.pcall('incr', KEYS[1]) return reply.err",
                "1", "lua:list"),
          pred=lambda g: isinstance(g, bytes) and g.startswith(b"WRONGTYPE"))
    check("redis.call raises the command's error",
          c.cmd("EVAL", "return redis.call('incr', KEYS[1])", "1", "lua:list"),
          pred=lambda g: is_err(g, "WRONGTYPE"))
    check("an unknown command from a script is an error",
          c.cmd("EVAL", "return redis.call('nosuchcommand')", "0"),
          pred=lambda g: is_err(g, "ERR Unknown Redis command called from script"))
    check("a command with the wrong number of arguments is an error",
          c.cmd("EVAL", "return redis.call('get')", "0"),
          pred=lambda g: is_err(
              g, "ERR Wrong number of args calling Redis command from script"))
    check("a command that cannot run in a script is refused",
          c.cmd("EVAL", "return redis.call('subscribe', 'ch')", "0"),
          pred=lambda g: is_err(g, "ERR This Redis command is not allowed from script"))
    check("EVAL from a script is refused",
          c.cmd("EVAL", "return redis.call('eval', 'return 1', '0')", "0"),
          pred=lambda g: is_err(g, "ERR This Redis command is not allowed from script"))
    c.cmd("DEL", "lua:empty")
    check("a blocking pop from a script does not block",
          c.cmd("EVAL", "return redis.call('blpop', KEYS[1], '0')", "1", "lua:empty"),
          None)
    check("and it pops when there is something to pop",
          c.cmd("EVAL", "return redis.call('blpop', KEYS[1], '0')", "1", "lua:list"),
          [b"lua:list", b"a"])
    c.cmd("DEL", "lua:list")
    c.cmd("RPUSH", "lua:list", "a", "b")

    # ----- the helper library -----
    check("redis.sha1hex of the empty string",
          c.cmd("EVAL", "return redis.sha1hex('')", "0"),
          b"da39a3ee5e6b4b0d3255bfef95601890afd80709")
    check("redis.REDIS_VERSION names the version the adapter targets",
          c.cmd("EVAL", "return redis.REDIS_VERSION", "0"), b"7.4.0")
    check("redis.setresp(2) is accepted",
          c.cmd("EVAL", "redis.setresp(2) return 1", "0"), 1)
    check("redis.setresp(3) is refused",
          c.cmd("EVAL", "redis.setresp(3) return 1", "0"),
          pred=lambda g: is_err(g, "ERR"))
    check("redis.log is callable",
          c.cmd("EVAL", "redis.log(redis.LOG_WARNING, 'from a test') return 1", "0"), 1)
    check("cjson encodes an array", c.cmd("EVAL", "return cjson.encode({1,2,3})", "0"),
          b"[1,2,3]")
    check("cjson decodes an array",
          c.cmd("EVAL", "return cjson.decode('[1,2,3]')[2]", "0"), 2)
    check("cjson round-trips an object",
          c.cmd("EVAL",
                "local t = cjson.decode(ARGV[1]) "
                "return {t['name'], tostring(t['n']), cjson.encode(t['list'])}",
                "0", '{"name":"mako","n":7,"list":[1,2]}'),
          [b"mako", b"7", b"[1,2]"])
    check("cmsgpack is not provided",
          c.cmd("EVAL", "return type(cmsgpack)", "0"),
          pred=lambda g: isinstance(g, Exception)
          and "nonexistent global variable 'cmsgpack'" in str(g))

    # The sandbox: nothing a script can reach may leave the process, and a
    # global it never declared is an error rather than a silent nil.
    check("os.execute is gone",
          c.cmd("EVAL", "return os.execute('true')", "0"),
          pred=lambda g: is_err(g, "ERR"))
    check("os.time survives", c.cmd("EVAL", "return type(os.time())", "0"), b"number")
    check("io is gone",
          c.cmd("EVAL", "return io.open('/etc/passwd')", "0"),
          pred=lambda g: isinstance(g, Exception) and "nonexistent global" in str(g))
    check("loadstring is gone",
          c.cmd("EVAL", "return loadstring('return 1')()", "0"),
          pred=lambda g: isinstance(g, Exception) and "nonexistent global" in str(g))
    check("reading an undeclared global is an error",
          c.cmd("EVAL", "return nosuchglobal", "0"),
          pred=lambda g: isinstance(g, Exception)
          and "attempted to access nonexistent global variable" in str(g))
    check("creating a global is an error",
          c.cmd("EVAL", "created = 1 return 1", "0"),
          pred=lambda g: isinstance(g, Exception)
          and "attempted to create global variable" in str(g))
    check("locals are unaffected",
          c.cmd("EVAL", "local ok = 1 return ok", "0"), 1)

    # A script can return a table that contains itself. The conversion has to
    # stop on its own rather than recurse until the stack gives out.
    recursive = c.cmd("EVAL", "local a = {} local b = {a} a[1] = b return a", "0")
    depth = 0
    while isinstance(recursive, list) and recursive:
        recursive = recursive[0]
        depth += 1
    check("a recursive table nests before it stops", depth, pred=lambda g: g > 1)
    check("and the reply ends by saying so", recursive,
          pred=lambda g: is_err(g, "ERR reached lua stack limit"))
    check("the connection still works after it", c.cmd("PING"), "PONG")

    # ----- SCRIPT LOAD / EVALSHA / EXISTS / FLUSH -----
    source = "return redis.call('get', KEYS[1])"
    sha = hashlib.sha1(source.encode()).hexdigest()
    check("SCRIPT LOAD returns the lowercase hex sha1",
          c.cmd("SCRIPT", "LOAD", source), sha.encode())
    check("SCRIPT EXISTS reports the loaded script",
          c.cmd("SCRIPT", "EXISTS", sha), [1])
    check("SCRIPT EXISTS reports several shas",
          c.cmd("SCRIPT", "EXISTS", sha, "0" * 40), [1, 0])
    check("SCRIPT EXISTS accepts an uppercase sha",
          c.cmd("SCRIPT", "EXISTS", sha.upper()), [1])
    check("EVALSHA runs the loaded script",
          c.cmd("EVALSHA", sha, "1", "lua:foo"), b"bar")
    check("EVALSHA_RO runs a read-only script",
          c.cmd("EVALSHA_RO", sha, "1", "lua:foo"), b"bar")
    check("EVAL caches the script it ran",
          c.cmd("SCRIPT", "EXISTS", hashlib.sha1(b"return 1").hexdigest()),
          pred=lambda g: g == [0] or g == [1])
    c.cmd("EVAL", "return 1", "0")
    check("EVAL has cached the script by now",
          c.cmd("SCRIPT", "EXISTS", hashlib.sha1(b"return 1").hexdigest()), [1])
    check("SCRIPT FLUSH empties the cache", c.cmd("SCRIPT", "FLUSH"), "OK")
    check("SCRIPT EXISTS after FLUSH", c.cmd("SCRIPT", "EXISTS", sha), [0])
    check("EVALSHA of a flushed script is NOSCRIPT",
          c.cmd("EVALSHA", sha, "1", "lua:foo"),
          pred=lambda g: is_err(g, "NOSCRIPT No matching script. Please use EVAL."))
    check("SCRIPT FLUSH ASYNC is accepted", c.cmd("SCRIPT", "FLUSH", "ASYNC"), "OK")
    check("SCRIPT FLUSH SYNC is accepted", c.cmd("SCRIPT", "FLUSH", "SYNC"), "OK")
    check("SCRIPT HELP returns lines",
          c.cmd("SCRIPT", "HELP"), pred=lambda g: isinstance(g, list) and len(g) > 1)
    check("an unknown SCRIPT subcommand is an error",
          c.cmd("SCRIPT", "BOGUS"),
          pred=lambda g: is_err(g, "ERR Unknown SCRIPT subcommand"))

    # ----- read-only scripts -----
    check("EVAL_RO reads",
          c.cmd("EVAL_RO", "return redis.call('get', KEYS[1])", "1", "lua:foo"),
          b"bar")
    check("EVAL_RO refuses a write",
          c.cmd("EVAL_RO", "return redis.call('set', KEYS[1], 'x')", "1", "lua:foo"),
          pred=lambda g: is_err(
              g, "ERR Write commands are not allowed from read-only scripts."))
    check("the refused write did not happen", c.cmd("GET", "lua:foo"), b"bar")

    # ----- numkeys validation -----
    check("numkeys greater than the arguments given",
          c.cmd("EVAL", "return 1", "3", "k1"),
          pred=lambda g: is_err(
              g, "ERR Number of keys can't be greater than number of args"))
    check("negative numkeys", c.cmd("EVAL", "return 1", "-1"),
          pred=lambda g: is_err(g, "ERR Number of keys can't be negative"))
    check("numkeys that is not a number", c.cmd("EVAL", "return 1", "notanumber"),
          pred=lambda g: is_err(g, "ERR value is not an integer or out of range"))
    check("EVAL with no numkeys is an arity error", c.cmd("EVAL", "return 1"),
          pred=lambda g: is_err(g, "ERR wrong number of arguments"))

    # ----- a failing script leaves nothing behind -----
    c.cmd("SET", "lua:atomic", "before")
    check("a script that errors after writing",
          c.cmd("EVAL", "redis.call('set', KEYS[1], 'after') error('boom')",
                "1", "lua:atomic"),
          pred=lambda g: is_err(g, "ERR"))
    check("the write it made is not visible", c.cmd("GET", "lua:atomic"), b"before")
    check("a script whose redis.call fails writes nothing either",
          c.cmd("EVAL",
                "redis.call('set', KEYS[1], 'after') return redis.call('incr', KEYS[2])",
                "2", "lua:atomic", "lua:list"),
          pred=lambda g: is_err(g, "WRONGTYPE"))
    check("still the old value", c.cmd("GET", "lua:atomic"), b"before")

    # ----- logical databases -----
    db1 = Resp(host, port)
    db1.cmd("SELECT", "1")
    db1.cmd("SET", "lua:db", "in-db-1")
    c.cmd("SET", "lua:db", "in-db-0")
    check("a script in database 1 reads database 1",
          db1.cmd("EVAL", "return redis.call('get', KEYS[1])", "1", "lua:db"),
          b"in-db-1")
    check("a script in database 0 reads database 0",
          c.cmd("EVAL", "return redis.call('get', KEYS[1])", "1", "lua:db"),
          b"in-db-0")
    db1.cmd("EVAL", "redis.call('set', KEYS[1], 'written-in-1') return 1", "1", "lua:db")
    check("the write stayed in database 1", c.cmd("GET", "lua:db"), b"in-db-0")
    check("and database 1 has it",
          db1.cmd("GET", "lua:db"), b"written-in-1")
    db1.cmd("FLUSHDB")
    db1.sock.close()

    # ----- PUBLISH from a script -----
    subscriber = Resp(host, port)
    subscriber.cmd("SUBSCRIBE", "lua:channel")
    check("a script publishes",
          c.cmd("EVAL", "return redis.call('publish', ARGV[1], ARGV[2])",
                "0", "lua:channel", "from-lua"),
          pred=lambda g: isinstance(g, int))
    delivered = subscriber.read_pushed(1.0)
    check("the subscriber got the message the script published",
          delivered, pred=lambda g: "from-lua" in g)
    subscriber.sock.close()

    # ----- a read-then-write script is atomic under concurrent clients -----
    check_script_atomicity(host, port, c)

    # ----- BUSY, SCRIPT KILL, UNKILLABLE, NOTBUSY -----
    check_script_kill(host, port, c)


def check_script_atomicity(host, port, c):
    """Eight clients run the same read-then-write script on one key.

    The script is `v = GET k; SET k v .. 'x'; return v`, which is only correct
    if the read and the write are one atomic unit. Every successful run appends
    exactly one character, so the final length has to equal the number of
    successful runs -- a torn read would make two clients write the same value
    and lose one.
    """
    import threading

    key = "lua:rmw"
    c.cmd("SET", key, "")
    script = ("local v = redis.call('get', KEYS[1]) "
              "redis.call('set', KEYS[1], v .. 'x') "
              "return v")
    clients = 8
    deadline = time.time() + 2.0
    counts = [0] * clients
    errors = []

    def hammer(index):
        conn = Resp(host, port)
        try:
            while time.time() < deadline:
                reply = conn.cmd("EVAL", script, "1", key)
                if isinstance(reply, Exception):
                    errors.append(str(reply))
                    break
                counts[index] += 1
        except Exception as exc:  # noqa: BLE001 - reported as a failure below
            errors.append(repr(exc))
        finally:
            conn.sock.close()

    threads = [threading.Thread(target=hammer, args=(index,)) for index in range(clients)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=30)

    total = sum(counts)
    final = c.cmd("GET", key)
    check("the concurrent read-then-write script never errored", errors, [])
    check("eight clients each ran the script", min(counts),
          pred=lambda g: g > 0)
    check("every successful run appended exactly one character",
          len(final) if isinstance(final, bytes) else final, total)


def check_script_kill(host, port, c):
    """SCRIPT KILL, and the BUSY state other connections see meanwhile.

    A script occupies the worker thread that read it, so the interesting case
    is the connection that shares that worker: it is only answered because the
    script's busy hook drains it. Connections are handed to workers round
    robin, so eight fillers between the runner and the killer put the two on
    the same worker for any of 1, 2, 4 or 8 worker threads.
    """
    import threading

    check("SCRIPT KILL with nothing running", c.cmd("SCRIPT", "KILL"),
          pred=lambda g: is_err(g, "NOTBUSY No scripts in execution right now."))

    check("the busy threshold is readable", c.cmd("CONFIG", "GET", "lua-time-limit"),
          pred=lambda g: isinstance(g, list) and g[0] == b"lua-time-limit")
    check("the busy threshold is settable",
          c.cmd("CONFIG", "SET", "lua-time-limit", "10"), "OK")
    check("busy-reply-threshold is the same setting",
          c.cmd("CONFIG", "GET", "busy-reply-threshold"), [b"busy-reply-threshold", b"10"])

    runner = Resp(host, port)
    fillers = [Resp(host, port) for _ in range(8)]
    killer = Resp(host, port)
    watcher = Resp(host, port)
    replies = {}

    def run_forever():
        replies["script"] = runner.cmd("EVAL", "while true do end", "0")

    thread = threading.Thread(target=run_forever)
    thread.start()
    time.sleep(0.5)

    check("another connection gets BUSY while a script runs",
          watcher.cmd("GET", "lua:foo"),
          pred=lambda g: is_err(
              g, "BUSY Redis is busy running a script. You can only call SCRIPT KILL"))
    latecomer = Resp(host, port)
    check("a connection made while a script runs is still accepted",
          latecomer.cmd("PING"),
          pred=lambda g: is_err(g, "BUSY Redis is busy running a script"))
    latecomer.sock.close()
    check("SCRIPT KILL is answered while a script runs",
          killer.cmd("SCRIPT", "KILL"), "OK")
    thread.join(timeout=20)
    check("the killed script reported it to its own connection",
          replies.get("script"),
          pred=lambda g: is_err(g, "ERR Script killed by user with SCRIPT KILL"))
    check("the server is not busy any more", watcher.cmd("GET", "lua:foo"), b"bar")

    # A script that wraps its work in pcall must not be able to swallow the
    # kill and carry on.
    def run_pcall_loop():
        replies["pcall"] = runner.cmd(
            "EVAL",
            "local f = function() while 1 do redis.call('ping') end end "
            "while 1 do pcall(f) end",
            "0",
        )

    thread = threading.Thread(target=run_pcall_loop)
    thread.start()
    time.sleep(0.5)
    check("a script that catches everything is still killable",
          killer.cmd("SCRIPT", "KILL"), "OK")
    thread.join(timeout=20)
    check("and it reported the kill",
          replies.get("pcall"),
          pred=lambda g: is_err(g, "ERR Script killed by user with SCRIPT KILL"))
    check("the server is usable again", watcher.cmd("PING"), "PONG")

    # A script that has written cannot be killed; this one stops on its own so
    # the suite can carry on.
    c.cmd("SET", "lua:unkillable", "before")
    spin = ("redis.call('set', KEYS[1], 'after') "
            "local n = 0 while n < 40000000 do n = n + 1 end "
            "return redis.call('get', KEYS[1])")

    def run_spin():
        replies["spin"] = runner.cmd("EVAL", spin, "1", "lua:unkillable")

    thread = threading.Thread(target=run_spin)
    thread.start()
    time.sleep(0.5)
    check("a script that has written is UNKILLABLE",
          killer.cmd("SCRIPT", "KILL"),
          pred=lambda g: is_err(g, "UNKILLABLE Sorry the script already executed"))
    thread.join(timeout=60)
    check("the unkillable script finished on its own", replies.get("spin"), b"after")
    check("and its write is visible", c.cmd("GET", "lua:unkillable"), b"after")
    check("nothing is running again", killer.cmd("SCRIPT", "KILL"),
          pred=lambda g: is_err(g, "NOTBUSY"))

    check("a connection opened while nothing runs still works",
          Resp(host, port).cmd("PING"), "PONG")

    c.cmd("CONFIG", "SET", "lua-time-limit", "5000")
    runner.sock.close()
    killer.sock.close()
    watcher.sock.close()
    for filler in fillers:
        filler.sock.close()

def as_map(reply):
    """A Redis map-style reply as a dict: RESP2 flattens it into an array."""
    if isinstance(reply, dict):
        return reply
    return {reply[i]: reply[i + 1] for i in range(0, len(reply), 2)}


def check_streams_basic(host, port, c):
    """XADD/XRANGE/XREVRANGE/XLEN/XDEL/XTRIM/XSETID/XINFO STREAM."""
    # ----- XADD and the IDs it assigns -----
    c.cmd("DEL", "st")
    generated = c.cmd("XADD", "st", "*", "item", "1", "value", "a")
    check("XADD * returns an ms-seq ID", generated,
          pred=lambda g: isinstance(g, bytes) and re.fullmatch(rb"\d+-\d+", g))
    second = c.cmd("XADD", "st", "*", "item", "2", "value", "b")
    check("XADD * IDs are increasing", (generated, second),
          pred=lambda g: stream_id_tuple(g[0]) < stream_id_tuple(g[1]))
    check("XLEN after two XADDs", c.cmd("XLEN", "st"), 2)
    check("XRANGE returns [id, [field, value...]]", c.cmd("XRANGE", "st", "-", "+"),
          pred=lambda g: len(g) == 2 and g[0][0] == generated
          and g[0][1] == [b"item", b"1", b"value", b"a"]
          and g[1][1] == [b"item", b"2", b"value", b"b"])

    # Redis doc example: explicit IDs, and the ordering rule
    c.cmd("DEL", "st")
    check("XADD with an explicit ID", c.cmd("XADD", "st", "1-1", "a", "1"), b"1-1")
    check("XADD with a larger explicit ID", c.cmd("XADD", "st", "1-2", "b", "2"), b"1-2")
    check("XADD with a smaller ID is refused", c.cmd("XADD", "st", "1-1", "c", "3"),
          pred=lambda g: is_err(g, "ERR The ID specified in XADD is equal or smaller "
                                   "than the target stream top item"))
    check("XADD with the same ID is refused", c.cmd("XADD", "st", "1-2", "c", "3"),
          pred=lambda g: is_err(g, "ERR The ID specified in XADD is equal or smaller"))
    check("a bare ms is an explicit ms-0", c.cmd("XADD", "st", "2", "c", "3"), b"2-0")
    check("a bare ms is refused once it is the top item",
          c.cmd("XADD", "st", "2", "d", "4"),
          pred=lambda g: is_err(g, "ERR The ID specified in XADD is equal or smaller"))
    check("XADD 0-0 is refused", c.cmd("XADD", "st0", "0-0", "a", "1"),
          pred=lambda g: is_err(g, "ERR The ID specified in XADD must be greater than 0-0"))
    check("the refused 0-0 XADD created no key", c.cmd("EXISTS", "st0"), 0)
    check("XADD rejects a malformed ID", c.cmd("XADD", "st", "bogus", "a", "1"),
          pred=lambda g: is_err(g, "ERR Invalid stream ID specified as stream command argument"))
    check("XADD needs at least one field", c.cmd("XADD", "st", "*"),
          pred=lambda g: is_err(g, "ERR wrong number of arguments"))
    check("XADD needs a value for every field", c.cmd("XADD", "st", "*", "f"),
          pred=lambda g: is_err(g, "ERR wrong number of arguments"))

    # ms-* picks the next sequence, and restarts at zero for a new millisecond
    c.cmd("DEL", "st")
    c.cmd("XADD", "st", "123-456", "item", "1")
    check("ms-* continues the sequence", c.cmd("XADD", "st", "123-*", "item", "2"), b"123-457")
    check("ms-* restarts the sequence for a later ms",
          c.cmd("XADD", "st", "789-*", "item", "3"), b"789-0")
    check("ms-* cannot go backwards", c.cmd("XADD", "st", "42-*", "item", "4"),
          pred=lambda g: is_err(g, "ERR The ID specified in XADD is equal or smaller"))
    c.cmd("DEL", "st")
    check("0-* starts at 0-1", c.cmd("XADD", "st", "0-*", "a", "b"), b"0-1")
    c.cmd("DEL", "st")
    c.cmd("XADD", "st", "1-18446744073709551615", "a", "b")
    check("ms-* refuses to overflow the sequence", c.cmd("XADD", "st", "1-*", "c", "d"),
          pred=lambda g: is_err(g, "ERR The ID specified in XADD is equal or smaller"))

    # NOMKSTREAM
    c.cmd("DEL", "st")
    check("XADD NOMKSTREAM on a missing key is nil",
          c.cmd("XADD", "st", "NOMKSTREAM", "*", "item", "1"), None)
    check("XADD NOMKSTREAM created no key", c.cmd("EXISTS", "st"), 0)
    c.cmd("XADD", "st", "*", "item", "1")
    check("XADD NOMKSTREAM on an existing stream adds",
          c.cmd("XADD", "st", "NOMKSTREAM", "*", "item", "2"),
          pred=lambda g: isinstance(g, bytes))
    check("XLEN after NOMKSTREAM", c.cmd("XLEN", "st"), 2)

    # MAXLEN, MAXLEN = and MAXLEN ~, MINID
    c.cmd("DEL", "st")
    for j in range(10):
        c.cmd("XADD", "st", "MAXLEN", "5", "*", "j", str(j))
    check("XADD MAXLEN keeps the newest entries", c.cmd("XLEN", "st"), 5)
    check("the surviving entries are the newest", [e[1][1] for e in c.cmd("XRANGE", "st", "-", "+")],
          [b"5", b"6", b"7", b"8", b"9"])
    c.cmd("DEL", "st")
    for j in range(10):
        c.cmd("XADD", "st", "MAXLEN", "=", "3", "*", "j", str(j))
    check("XADD MAXLEN = is the same as MAXLEN", c.cmd("XLEN", "st"), 3)
    c.cmd("DEL", "st")
    for j in range(10):
        c.cmd("XADD", "st", "MAXLEN", "~", "4", "*", "j", str(j))
    check("XADD MAXLEN ~ trims to the threshold here", c.cmd("XLEN", "st"), 4)
    check("XADD MAXLEN 0 leaves an existing but empty stream",
          c.cmd("XADD", "st", "MAXLEN", "0", "*", "a", "b"),
          pred=lambda g: isinstance(g, bytes))
    check("the emptied stream still exists", c.cmd("EXISTS", "st"), 1)
    check("and reports length 0", c.cmd("XLEN", "st"), 0)
    check("and is still a stream", c.cmd("TYPE", "st"), "stream")
    check("XADD MAXLEN rejects a negative threshold",
          c.cmd("XADD", "st", "MAXLEN", "-1", "*", "a", "b"),
          pred=lambda g: is_err(g, "ERR The MAXLEN argument must be >= 0."))
    check("LIMIT needs the ~ option",
          c.cmd("XADD", "st", "MAXLEN", "5", "LIMIT", "3", "*", "a", "b"),
          pred=lambda g: is_err(g, "ERR syntax error, LIMIT cannot be used without "
                                   "the special ~ option"))

    c.cmd("DEL", "st")
    for j in range(1, 6):
        c.cmd("XADD", "st", "%d-0" % j, "f", "v")
    check("XADD MINID drops older entries",
          c.cmd("XADD", "st", "MINID", "4", "6-0", "f", "v"),
          b"6-0")
    check("only the entries at or after MINID remain",
          [e[0] for e in c.cmd("XRANGE", "st", "-", "+")], [b"4-0", b"5-0", b"6-0"])

    # ----- XRANGE / XREVRANGE -----
    c.cmd("DEL", "st")
    for spec in ("1-1", "1-2", "2-1", "3-0"):
        c.cmd("XADD", "st", spec, "f", spec)
    check("XRANGE - +", [e[0] for e in c.cmd("XRANGE", "st", "-", "+")],
          [b"1-1", b"1-2", b"2-1", b"3-0"])
    check("XREVRANGE + - is the reverse", [e[0] for e in c.cmd("XREVRANGE", "st", "+", "-")],
          [b"3-0", b"2-1", b"1-2", b"1-1"])
    check("XRANGE with a partial start ID means ms-0",
          [e[0] for e in c.cmd("XRANGE", "st", "2", "+")], [b"2-1", b"3-0"])
    check("XRANGE with a partial end ID means ms-max",
          [e[0] for e in c.cmd("XRANGE", "st", "-", "1")], [b"1-1", b"1-2"])
    check("XRANGE exclusive start", [e[0] for e in c.cmd("XRANGE", "st", "(1-1", "+")],
          [b"1-2", b"2-1", b"3-0"])
    check("XRANGE exclusive end", [e[0] for e in c.cmd("XRANGE", "st", "-", "(3-0")],
          [b"1-1", b"1-2", b"2-1"])
    check("XRANGE COUNT", [e[0] for e in c.cmd("XRANGE", "st", "-", "+", "COUNT", "2")],
          [b"1-1", b"1-2"])
    check("XREVRANGE COUNT takes from the end",
          [e[0] for e in c.cmd("XREVRANGE", "st", "+", "-", "COUNT", "2")], [b"3-0", b"2-1"])
    check("XRANGE of a missing stream is empty", c.cmd("XRANGE", "st-missing", "-", "+"), [])
    check("XRANGE rejects an exclusive -", c.cmd("XRANGE", "st", "(-", "+"),
          pred=lambda g: is_err(g, "ERR Invalid stream ID specified as stream command argument"))
    check("XRANGE rejects an exclusive +", c.cmd("XRANGE", "st", "-", "(+"),
          pred=lambda g: is_err(g, "ERR Invalid stream ID specified as stream command argument"))
    check("XRANGE rejects an unincrementable exclusive start",
          c.cmd("XRANGE", "st", "(18446744073709551615-18446744073709551615", "+"),
          pred=lambda g: is_err(g, "ERR invalid start offset"))
    check("XRANGE rejects an undecrementable exclusive end",
          c.cmd("XRANGE", "st", "-", "(0-0"),
          pred=lambda g: is_err(g, "ERR invalid end offset"))
    check("XLEN of a missing stream is 0", c.cmd("XLEN", "st-missing"), 0)

    # ----- XDEL -----
    c.cmd("DEL", "st")
    for spec in ("1-1", "1-2", "1-3", "1-4", "1-5"):
        c.cmd("XADD", "st", spec, "f", spec)
    check("XDEL counts only the IDs it removed",
          c.cmd("XDEL", "st", "1-1", "1-4", "1-5", "2-1"), 3)
    check("XLEN after XDEL", c.cmd("XLEN", "st"), 2)
    check("XRANGE after XDEL", [e[0] for e in c.cmd("XRANGE", "st", "-", "+")],
          [b"1-2", b"1-3"])
    info = as_map(c.cmd("XINFO", "STREAM", "st"))
    check("XDEL moved recorded-first-entry-id", info[b"recorded-first-entry-id"], b"1-2")
    check("XDEL recorded the largest deleted ID", info[b"max-deleted-entry-id"], b"1-5")
    check("XDEL did not change last-generated-id", info[b"last-generated-id"], b"1-5")
    check("XDEL rejects a malformed ID", c.cmd("XDEL", "st", "nope"),
          pred=lambda g: is_err(g, "ERR Invalid stream ID specified as stream command argument"))

    # ----- XTRIM -----
    c.cmd("DEL", "st")
    for j in range(1, 11):
        c.cmd("XADD", "st", "%d-0" % j, "f", "v")
    check("XTRIM MAXLEN returns the number removed", c.cmd("XTRIM", "st", "MAXLEN", "6"), 4)
    check("XLEN after XTRIM MAXLEN", c.cmd("XLEN", "st"), 6)
    check("XTRIM MAXLEN above the length removes nothing",
          c.cmd("XTRIM", "st", "MAXLEN", "100"), 0)
    check("XTRIM MINID removes by ID", c.cmd("XTRIM", "st", "MINID", "8-0"), 3)
    check("XRANGE after XTRIM MINID", [e[0] for e in c.cmd("XRANGE", "st", "-", "+")],
          [b"8-0", b"9-0", b"10-0"])
    check("XTRIM MINID = accepts the exact marker", c.cmd("XTRIM", "st", "MINID", "=", "9-0"), 1)
    check("XTRIM MAXLEN ~ LIMIT caps the deletions",
          c.cmd("XTRIM", "st", "MAXLEN", "~", "0", "LIMIT", "1"), 1)
    check("XTRIM without ~ refuses LIMIT", c.cmd("XTRIM", "st", "MAXLEN", "1", "LIMIT", "3"),
          pred=lambda g: is_err(g, "ERR syntax error, LIMIT cannot be used without "
                                   "the special ~ option"))
    check("XTRIM of a missing stream is 0", c.cmd("XTRIM", "st-missing", "MAXLEN", "1"), 0)

    # ----- XSETID -----
    c.cmd("DEL", "st")
    c.cmd("XADD", "st", "1-0", "a", "b")
    check("XSETID sets last-generated-id", c.cmd("XSETID", "st", "200-0"), "OK")
    check("XINFO reports the new last-generated-id",
          as_map(c.cmd("XINFO", "STREAM", "st"))[b"last-generated-id"], b"200-0")
    check("XSETID refuses an ID below the top item", c.cmd("XSETID", "st", "0-1"),
          pred=lambda g: is_err(g, "ERR The ID specified in XSETID is smaller than "
                                   "the target stream top item"))
    check("XSETID on a missing key", c.cmd("XSETID", "st-missing", "1-1"),
          pred=lambda g: is_err(g, "ERR no such key"))
    check("XSETID rejects a lone positional argument", c.cmd("XSETID", "st", "300-0", "0"),
          pred=lambda g: is_err(g, "ERR") and "syntax error" in str(g))
    check("XSETID rejects a negative ENTRIESADDED",
          c.cmd("XSETID", "st", "300-0", "ENTRIESADDED", "-1", "MAXDELETEDID", "0-0"),
          pred=lambda g: is_err(g, "ERR value for ENTRIESADDED must be positive"))
    check("XSETID refuses ENTRIESADDED below the length",
          c.cmd("XSETID", "st", "300-0", "ENTRIESADDED", "0", "MAXDELETEDID", "0-0"),
          pred=lambda g: is_err(g, "ERR The entries_added specified in XSETID is smaller "
                                   "than the target stream length"))
    check("XSETID refuses a MAXDELETEDID above the new ID",
          c.cmd("XSETID", "st", "300-0", "ENTRIESADDED", "5", "MAXDELETEDID", "400-0"),
          pred=lambda g: is_err(g, "ERR The ID specified in XSETID is smaller than "
                                   "the provided max_deleted_entry_id"))
    check("XSETID accepts ENTRIESADDED and MAXDELETEDID together",
          c.cmd("XSETID", "st", "300-0", "ENTRIESADDED", "7", "MAXDELETEDID", "250-0"), "OK")
    info = as_map(c.cmd("XINFO", "STREAM", "st"))
    check("XSETID wrote entries-added", info[b"entries-added"], 7)
    check("XSETID wrote max-deleted-entry-id", info[b"max-deleted-entry-id"], b"250-0")
    c.cmd("DEL", "st")
    c.cmd("XADD", "st", "1-1", "a", "1")
    c.cmd("XADD", "st", "1-2", "b", "2")
    c.cmd("XADD", "st", "1-3", "c", "3")
    c.cmd("XDEL", "st", "1-2")
    c.cmd("XDEL", "st", "1-3")
    check("XSETID refuses an ID below the recorded tombstone",
          c.cmd("XSETID", "st", "1-2"),
          pred=lambda g: is_err(g, "ERR The ID specified in XSETID is smaller than "
                                   "the provided max_deleted_entry_id"))

    # ----- XINFO STREAM -----
    c.cmd("DEL", "st")
    c.cmd("XADD", "st", "1-0", "data", "a")
    c.cmd("XADD", "st", "2-0", "data", "b")
    info = as_map(c.cmd("XINFO", "STREAM", "st"))
    check("XINFO STREAM has every documented field", sorted(info.keys()),
          sorted([b"length", b"radix-tree-keys", b"radix-tree-nodes", b"last-generated-id",
                  b"max-deleted-entry-id", b"entries-added", b"recorded-first-entry-id",
                  b"groups", b"first-entry", b"last-entry"]))
    check("XINFO STREAM length", info[b"length"], 2)
    check("XINFO STREAM last-generated-id", info[b"last-generated-id"], b"2-0")
    check("XINFO STREAM entries-added", info[b"entries-added"], 2)
    check("XINFO STREAM recorded-first-entry-id", info[b"recorded-first-entry-id"], b"1-0")
    check("XINFO STREAM max-deleted-entry-id", info[b"max-deleted-entry-id"], b"0-0")
    check("XINFO STREAM groups", info[b"groups"], 0)
    check("XINFO STREAM first-entry", info[b"first-entry"], [b"1-0", [b"data", b"a"]])
    check("XINFO STREAM last-entry", info[b"last-entry"], [b"2-0", [b"data", b"b"]])
    full = as_map(c.cmd("XINFO", "STREAM", "st", "FULL"))
    check("XINFO STREAM FULL lists the entries", full[b"entries"],
          [[b"1-0", [b"data", b"a"]], [b"2-0", [b"data", b"b"]]])
    check("XINFO STREAM FULL has no groups yet", full[b"groups"], [])
    check("XINFO STREAM FULL COUNT limits the entries",
          as_map(c.cmd("XINFO", "STREAM", "st", "FULL", "COUNT", "1"))[b"entries"],
          [[b"1-0", [b"data", b"a"]]])
    check("XINFO STREAM on a missing key", c.cmd("XINFO", "STREAM", "st-missing"),
          pred=lambda g: is_err(g, "ERR no such key"))
    check("XINFO HELP lists its subcommands", c.cmd("XINFO", "HELP"),
          pred=lambda g: isinstance(g, list) and any("STREAM" in line for line in g))
    check("XINFO HELP takes no argument", c.cmd("XINFO", "HELP", "xxx"),
          pred=lambda g: is_err(g, "ERR wrong number of arguments for 'xinfo|help' command"))
    check("XINFO rejects an unknown subcommand", c.cmd("XINFO", "NOPE", "st"),
          pred=lambda g: is_err(g, "ERR Unknown XINFO subcommand"))

    # ----- streams and the generic key commands -----
    c.cmd("DEL", "st", "st2")
    c.cmd("XADD", "st", "1-0", "f", "v")
    c.cmd("XADD", "st", "2-0", "f", "v")
    check("TYPE of a stream", c.cmd("TYPE", "st"), "stream")
    check("EXISTS sees a stream", c.cmd("EXISTS", "st"), 1)
    check("OBJECT ENCODING of a stream", c.cmd("OBJECT", "ENCODING", "st"), b"stream")
    check("EXPIRE on a stream", c.cmd("EXPIRE", "st", "100"), 1)
    check("TTL of a stream", c.cmd("TTL", "st"), pred=lambda g: 0 < g <= 100)
    check("PERSIST on a stream", c.cmd("PERSIST", "st"), 1)
    check("RENAME a stream", c.cmd("RENAME", "st", "st2"), "OK")
    check("the renamed stream kept its entries",
          [e[0] for e in c.cmd("XRANGE", "st2", "-", "+")], [b"1-0", b"2-0"])
    check("the old name is gone", c.cmd("EXISTS", "st"), 0)
    check("COPY a stream", c.cmd("COPY", "st2", "st3"), 1)
    check("the copy holds the same entries",
          [e[0] for e in c.cmd("XRANGE", "st3", "-", "+")], [b"1-0", b"2-0"])
    check("the copy is a stream", c.cmd("TYPE", "st3"), "stream")
    dump = c.cmd("DUMP", "st2")
    check("DUMP of a stream returns a payload", dump, pred=lambda g: isinstance(g, bytes) and g)
    c.cmd("DEL", "st4")
    check("RESTORE a stream", c.cmd("RESTORE", "st4", "0", dump), "OK")
    check("the restored stream holds the entries",
          c.cmd("XRANGE", "st4", "-", "+"),
          [[b"1-0", [b"f", b"v"]], [b"2-0", [b"f", b"v"]]])
    check("the restored stream reports the same length",
          as_map(c.cmd("XINFO", "STREAM", "st4"))[b"entries-added"], 2)
    check("DEL removes a stream", c.cmd("DEL", "st4"), 1)
    check("and the stream is gone", c.cmd("EXISTS", "st4"), 0)
    check("XLEN after DEL", c.cmd("XLEN", "st4"), 0)

    # MOVE carries the stream to another database
    c.cmd("DEL", "stmove")
    c.cmd("XADD", "stmove", "5-5", "f", "v")
    check("MOVE a stream to database 3", c.cmd("MOVE", "stmove", "3"), 1)
    check("the source name is gone after MOVE", c.cmd("EXISTS", "stmove"), 0)
    moved = Resp(host, port)
    moved.cmd("SELECT", "3")
    check("the moved stream arrived", moved.cmd("XRANGE", "stmove", "-", "+"),
          [[b"5-5", [b"f", b"v"]]])
    moved.cmd("DEL", "stmove")
    moved.sock.close()

    # ----- WRONGTYPE in both directions -----
    c.cmd("DEL", "st", "plain")
    c.cmd("XADD", "st", "1-0", "f", "v")
    c.cmd("SET", "plain", "x")
    # Every string, list, set, hash and sorted-set command that consults the
    # type guard refuses a stream key. GET, APPEND, STRLEN, GETEX and GETDEL do
    # not consult it for any collection type, which known_divergences.txt
    # records, so they are checked for the behaviour they actually have.
    for name, args in (("INCR", ("INCR", "st")),
                       ("INCRBYFLOAT", ("INCRBYFLOAT", "st", "1.5")),
                       ("SETRANGE", ("SETRANGE", "st", "0", "x")),
                       ("GETRANGE", ("GETRANGE", "st", "0", "1")),
                       ("SETBIT", ("SETBIT", "st", "0", "1")),
                       ("GETBIT", ("GETBIT", "st", "0")),
                       ("BITCOUNT", ("BITCOUNT", "st")),
                       ("BITFIELD", ("BITFIELD", "st", "GET", "u8", "0")),
                       ("PFADD", ("PFADD", "st", "x")),
                       ("LPUSH", ("LPUSH", "st", "x")),
                       ("LRANGE", ("LRANGE", "st", "0", "-1")),
                       ("SADD", ("SADD", "st", "x")),
                       ("SMEMBERS", ("SMEMBERS", "st")),
                       ("ZADD", ("ZADD", "st", "1", "x")),
                       ("ZRANGE", ("ZRANGE", "st", "0", "-1")),
                       ("HSET", ("HSET", "st", "f", "v")),
                       ("HGETALL", ("HGETALL", "st"))):
        check("%s on a stream is WRONGTYPE" % name, c.cmd(*args),
              pred=lambda g: is_err(g, "WRONGTYPE"))
    check("the stream survived the wrong-type attempts", c.cmd("TYPE", "st"), "stream")
    check("GET on a stream answers nil, as it does for every collection",
          c.cmd("GET", "st"), None)
    check("STRLEN on a stream answers 0, as it does for every collection",
          c.cmd("STRLEN", "st"), 0)
    for name, args in (("XADD", ("XADD", "plain", "*", "f", "v")),
                       ("XLEN", ("XLEN", "plain")),
                       ("XRANGE", ("XRANGE", "plain", "-", "+")),
                       ("XREVRANGE", ("XREVRANGE", "plain", "+", "-")),
                       ("XDEL", ("XDEL", "plain", "1-1")),
                       ("XTRIM", ("XTRIM", "plain", "MAXLEN", "1")),
                       ("XSETID", ("XSETID", "plain", "1-1")),
                       ("XINFO", ("XINFO", "STREAM", "plain"))):
        check("%s on a string is WRONGTYPE" % name, c.cmd(*args),
              pred=lambda g: is_err(g, "WRONGTYPE"))

    # ----- logical database isolation -----
    zero = Resp(host, port)
    one = Resp(host, port)
    one.cmd("SELECT", "1")
    zero.cmd("DEL", "stdb")
    one.cmd("DEL", "stdb")
    zero.cmd("XADD", "stdb", "1-0", "db", "0")
    one.cmd("XADD", "stdb", "9-0", "db", "1")
    check("a stream in database 0 is the database-0 one",
          zero.cmd("XRANGE", "stdb", "-", "+"), [[b"1-0", [b"db", b"0"]]])
    check("a stream in database 1 is its own", one.cmd("XRANGE", "stdb", "-", "+"),
          [[b"9-0", [b"db", b"1"]]])
    check("XLEN is per database", (zero.cmd("XLEN", "stdb"), one.cmd("XLEN", "stdb")), (1, 1))
    one.cmd("DEL", "stdb")
    check("deleting the database-1 stream leaves database 0 alone",
          zero.cmd("XLEN", "stdb"), 1)
    zero.cmd("DEL", "stdb")
    zero.sock.close()
    one.sock.close()

    for key in ("st", "st0", "st2", "st3", "st4", "plain", "stdb"):
        c.cmd("DEL", key)


def check_streams_read(host, port, c):
    """XREAD: several streams at once, and BLOCK on the blocked-client path."""
    c.cmd("DEL", "xr1", "xr2", "xr3", "xrplain")
    c.cmd("XADD", "xr1", "1-1", "a", "1")
    c.cmd("XADD", "xr1", "1-2", "b", "2")
    check("XREAD reads the entries after an exclusive ID",
          c.cmd("XREAD", "STREAMS", "xr1", "0-0"),
          [[b"xr1", [[b"1-1", [b"a", b"1"]], [b"1-2", [b"b", b"2"]]]]])
    check("XREAD COUNT limits the entries",
          c.cmd("XREAD", "COUNT", "1", "STREAMS", "xr1", "0-0"),
          [[b"xr1", [[b"1-1", [b"a", b"1"]]]]])
    check("XREAD from a partial ID means ms-0",
          c.cmd("XREAD", "STREAMS", "xr1", "1"),
          [[b"xr1", [[b"1-1", [b"a", b"1"]], [b"1-2", [b"b", b"2"]]]]])
    check("XREAD from the last ID is nil", c.cmd("XREAD", "STREAMS", "xr1", "1-2"), None)
    check("XREAD $ with no new entry is nil", c.cmd("XREAD", "STREAMS", "xr1", "$"), None)
    check("XREAD of a missing stream is nil", c.cmd("XREAD", "STREAMS", "xr-missing", "0-0"), None)
    check("XREAD from the largest ID there is stays nil",
          c.cmd("XREAD", "STREAMS", "xr1", "18446744073709551615-18446744073709551615"), None)

    c.cmd("XADD", "xr2", "5-0", "c", "3")
    check("XREAD reads several streams in the order they were named",
          c.cmd("XREAD", "STREAMS", "xr1", "xr2", "0-0", "0-0"),
          [[b"xr1", [[b"1-1", [b"a", b"1"]], [b"1-2", [b"b", b"2"]]]],
           [b"xr2", [[b"5-0", [b"c", b"3"]]]]])
    check("XREAD leaves out a stream with nothing to give",
          c.cmd("XREAD", "STREAMS", "xr1", "xr2", "1-2", "0-0"),
          [[b"xr2", [[b"5-0", [b"c", b"3"]]]]])
    check("XREAD is nil when no stream has anything",
          c.cmd("XREAD", "STREAMS", "xr1", "xr2", "1-2", "5-0"), None)
    check("XREAD + reads the last entry", c.cmd("XREAD", "STREAMS", "xr1", "+"),
          [[b"xr1", [[b"1-2", [b"b", b"2"]]]]])
    check("XREAD + ignores COUNT", c.cmd("XREAD", "COUNT", "5", "STREAMS", "xr1", "+"),
          [[b"xr1", [[b"1-2", [b"b", b"2"]]]]])
    check("XREAD + on a stream that never had an entry is nil",
          c.cmd("XREAD", "STREAMS", "xr3", "+"), None)
    c.cmd("XADD", "xr3", "1-0", "k", "v")
    c.cmd("XDEL", "xr3", "1-0")
    check("XREAD + on a stream that ran dry is nil",
          c.cmd("XREAD", "STREAMS", "xr3", "+"), None)

    c.cmd("SET", "xrplain", "x")
    check("XREAD on a string is WRONGTYPE", c.cmd("XREAD", "STREAMS", "xrplain", "0"),
          pred=lambda g: is_err(g, "WRONGTYPE"))
    check("XREAD needs STREAMS", c.cmd("XREAD", "COUNT", "1", "xr1", "0"),
          pred=lambda g: is_err(g, "ERR") and "syntax error" in str(g))
    check("XREAD needs one ID per key", c.cmd("XREAD", "COUNT", "1", "STREAMS", "xr1"),
          pred=lambda g: is_err(g, "ERR Unbalanced 'xread' list of streams: for each "
                                   "stream key an ID or '$' must be specified."))
    check("XREAD refuses the group-only > ID", c.cmd("XREAD", "STREAMS", "xr1", ">"),
          pred=lambda g: is_err(g, "ERR The > ID can be specified only when calling "
                                   "XREADGROUP using the GROUP <group> <consumer> option."))
    check("XREAD refuses a negative BLOCK", c.cmd("XREAD", "BLOCK", "-1", "STREAMS", "xr1", "0"),
          pred=lambda g: is_err(g, "ERR timeout is negative"))
    check("XREAD refuses a non-numeric BLOCK",
          c.cmd("XREAD", "BLOCK", "abc", "STREAMS", "xr1", "0"),
          pred=lambda g: is_err(g, "ERR timeout is not an integer or out of range"))
    check("XREAD reports wrong arity", c.cmd("XREAD", "STREAMS"),
          pred=lambda g: is_err(g, "ERR wrong number of arguments for 'xread' command"))

    # ----- BLOCK -----
    reader = Resp(host, port)
    writer = Resp(host, port)

    started = time.time()
    check("a blocking XREAD that nothing satisfies times out with nil",
          reader.cmd("XREAD", "BLOCK", "400", "STREAMS", "xr1", "$"), None)
    waited = time.time() - started
    check("the timeout waited roughly as long as it was asked to", waited,
          pred=lambda g: 0.3 <= g < 5.0)

    reader.send("XREAD", "BLOCK", "10000", "STREAMS", "xr1", "$")
    time.sleep(0.4)
    started = time.time()
    writer.cmd("XADD", "xr1", "9-9", "woken", "yes")
    woken = reader.read()
    waited = time.time() - started
    check("an XADD from another connection wakes a blocked XREAD", woken,
          [[b"xr1", [[b"9-9", [b"woken", b"yes"]]]]])
    check("the wake arrived well inside the 10 s BLOCK timeout", waited,
          pred=lambda g: g < 3.0)

    # "$" names the position the stream had when the command arrived, so an
    # entry added while the client is parked is delivered rather than skipped.
    reader.send("XREAD", "BLOCK", "10000", "STREAMS", "xr1", "$")
    time.sleep(0.4)
    writer.cmd("XADD", "xr1", "10-1", "first", "1")
    writer.cmd("XADD", "xr1", "10-2", "second", "2")
    woken = reader.read()
    check("the entry added right after $ was resolved is the one delivered", woken,
          pred=lambda g: g[0][0] == b"xr1" and g[0][1][0][0] == b"10-1")

    reader.send("XREAD", "BLOCK", "0", "STREAMS", "xr1", "$")
    time.sleep(0.4)
    writer.cmd("XADD", "xr1", "11-0", "forever", "no")
    check("BLOCK 0 waits until an entry arrives", reader.read(),
          [[b"xr1", [[b"11-0", [b"forever", b"no"]]]]])

    reader.send("XREAD", "BLOCK", "10000", "STREAMS", "xr1", "xr2", "$", "$")
    time.sleep(0.4)
    writer.cmd("XADD", "xr2", "7-0", "second", "stream")
    check("a multi-stream blocking XREAD reports only the stream that moved",
          reader.read(), [[b"xr2", [[b"7-0", [b"second", b"stream"]]]]])

    # An XADD that is immediately undone must not wake the client with an
    # empty reply.
    reader.send("XREAD", "BLOCK", "10000", "STREAMS", "xr4", "$")
    time.sleep(0.4)
    writer.cmd("MULTI")
    writer.cmd("XADD", "xr4", "*", "gone", "1")
    writer.cmd("DEL", "xr4")
    writer.cmd("EXEC")
    time.sleep(0.3)
    writer.cmd("XADD", "xr4", "1-1", "real", "1")
    check("an XADD undone in the same transaction does not wake the client",
          reader.read(), [[b"xr4", [[b"1-1", [b"real", b"1"]]]]])

    check("MULTI before a blocking read", c.cmd("MULTI"), "OK")
    check("the blocking read queues", c.cmd("XREAD", "BLOCK", "0", "STREAMS", "xr1", "$"),
          "QUEUED")
    check("a blocking XREAD inside MULTI answers nil instead of blocking",
          c.cmd("EXEC"), [None])

    reader.sock.close()
    writer.sock.close()
    for key in ("xr1", "xr2", "xr3", "xr4", "xrplain"):
        c.cmd("DEL", key)

def check_streams_groups(host, port, c):
    """XGROUP, XREADGROUP, XACK, XPENDING, XCLAIM, XAUTOCLAIM, XINFO GROUPS."""
    # ----- XGROUP CREATE and friends -----
    c.cmd("DEL", "cg")
    check("XGROUP CREATE on a missing key without MKSTREAM",
          c.cmd("XGROUP", "CREATE", "cg", "g1", "$"),
          pred=lambda g: is_err(g, "ERR The XGROUP subcommand requires the key to exist. "
                                   "Note that for CREATE you may want to use the MKSTREAM "
                                   "option to create an empty stream automatically."))
    check("the refused XGROUP CREATE created no key", c.cmd("EXISTS", "cg"), 0)
    check("XGROUP CREATE MKSTREAM", c.cmd("XGROUP", "CREATE", "cg", "g1", "$", "MKSTREAM"), "OK")
    check("MKSTREAM left an existing stream", c.cmd("EXISTS", "cg"), 1)
    check("the stream it left is empty", c.cmd("XLEN", "cg"), 0)
    check("and it is typed stream", c.cmd("TYPE", "cg"), "stream")
    check("XGROUP CREATE twice is BUSYGROUP", c.cmd("XGROUP", "CREATE", "cg", "g1", "$"),
          pred=lambda g: is_err(g, "BUSYGROUP Consumer Group name already exists"))
    check("XINFO STREAM counts the group",
          as_map(c.cmd("XINFO", "STREAM", "cg"))[b"groups"], 1)
    check("XGROUP CREATE rejects a negative ENTRIESREAD",
          c.cmd("XGROUP", "CREATE", "cg", "gbad", "$", "ENTRIESREAD", "-3"),
          pred=lambda g: is_err(g, "ERR value for ENTRIESREAD must be positive or -1"))
    check("XGROUP CREATE accepts ENTRIESREAD",
          c.cmd("XGROUP", "CREATE", "cg", "g2", "$", "ENTRIESREAD", "3"), "OK")
    groups = {g[b"name"]: g for g in (as_map(row) for row in c.cmd("XINFO", "GROUPS", "cg"))}
    check("XINFO GROUPS reports the ENTRIESREAD it was given",
          groups[b"g2"][b"entries-read"], 3)
    check("XINFO GROUPS reports an unknown read counter as nil",
          groups[b"g1"][b"entries-read"], None)
    check("XGROUP DESTROY removes it", c.cmd("XGROUP", "DESTROY", "cg", "g2"), 1)
    check("XGROUP DESTROY of a gone group", c.cmd("XGROUP", "DESTROY", "cg", "g2"), 0)
    check("XGROUP DESTROY on a missing key", c.cmd("XGROUP", "DESTROY", "cg-missing", "g"), 0)
    check("XGROUP SETID on a missing group", c.cmd("XGROUP", "SETID", "cg", "gone", "0"),
          pred=lambda g: is_err(g, "NOGROUP No such consumer group 'gone' for key name 'cg'"))
    check("XGROUP CREATECONSUMER on a missing group",
          c.cmd("XGROUP", "CREATECONSUMER", "cg", "gone", "alice"),
          pred=lambda g: is_err(g, "NOGROUP No such consumer group 'gone' for key name 'cg'"))
    check("XGROUP HELP lists its subcommands", c.cmd("XGROUP", "HELP"),
          pred=lambda g: isinstance(g, list) and any("CREATECONSUMER" in line for line in g))
    check("XGROUP HELP takes no argument", c.cmd("XGROUP", "HELP", "xxx"),
          pred=lambda g: is_err(g, "ERR wrong number of arguments for 'xgroup|help' command"))
    check("XGROUP rejects an unknown subcommand", c.cmd("XGROUP", "NOPE", "cg", "g"),
          pred=lambda g: is_err(g, "ERR Unknown XGROUP subcommand"))

    # ----- the consumer-group flow -----
    c.cmd("DEL", "cg")
    for spec, value in (("1-1", "a"), ("1-2", "b"), ("1-3", "c"), ("1-4", "d")):
        c.cmd("XADD", "cg", spec, "f", value)
    check("XGROUP CREATE from the start", c.cmd("XGROUP", "CREATE", "cg", "g1", "0"), "OK")
    check("XREADGROUP > hands the first entries to the first consumer",
          c.cmd("XREADGROUP", "GROUP", "g1", "alice", "COUNT", "2", "STREAMS", "cg", ">"),
          [[b"cg", [[b"1-1", [b"f", b"a"]], [b"1-2", [b"f", b"b"]]]]])
    check("XREADGROUP > hands the next entries to the second consumer",
          c.cmd("XREADGROUP", "GROUP", "g1", "bob", "COUNT", "2", "STREAMS", "cg", ">"),
          [[b"cg", [[b"1-3", [b"f", b"c"]], [b"1-4", [b"f", b"d"]]]]])
    check("XREADGROUP > has nothing left to hand over",
          c.cmd("XREADGROUP", "GROUP", "g1", "alice", "STREAMS", "cg", ">"), None)
    check("XPENDING summary counts every pending entry",
          c.cmd("XPENDING", "cg", "g1"),
          [4, b"1-1", b"1-4", [[b"alice", b"2"], [b"bob", b"2"]]])
    check("XPENDING extended reports one row per entry",
          [[row[0], row[1], row[3]] for row in c.cmd("XPENDING", "cg", "g1", "-", "+", "10")],
          [[b"1-1", b"alice", 1], [b"1-2", b"alice", 1],
           [b"1-3", b"bob", 1], [b"1-4", b"bob", 1]])
    check("XPENDING extended can name one consumer",
          [row[0] for row in c.cmd("XPENDING", "cg", "g1", "-", "+", "10", "bob")],
          [b"1-3", b"1-4"])
    check("XPENDING extended honours COUNT",
          [row[0] for row in c.cmd("XPENDING", "cg", "g1", "-", "+", "2")], [b"1-1", b"1-2"])
    check("XPENDING extended honours an exclusive range",
          [row[0] for row in c.cmd("XPENDING", "cg", "g1", "(1-1", "(1-4", "10")],
          [b"1-2", b"1-3"])
    check("XPENDING IDLE filters out entries younger than the threshold",
          c.cmd("XPENDING", "cg", "g1", "IDLE", "99999999", "-", "+", "10"), [])
    check("XPENDING IDLE 0 keeps them all",
          len(c.cmd("XPENDING", "cg", "g1", "IDLE", "0", "-", "+", "10")), 4)
    check("XREADGROUP with an explicit ID replays what this consumer holds",
          c.cmd("XREADGROUP", "GROUP", "g1", "alice", "STREAMS", "cg", "0"),
          [[b"cg", [[b"1-1", [b"f", b"a"]], [b"1-2", [b"f", b"b"]]]]])
    check("the replay starts after the ID it was given",
          c.cmd("XREADGROUP", "GROUP", "g1", "alice", "STREAMS", "cg", "1-1"),
          [[b"cg", [[b"1-2", [b"f", b"b"]]]]])
    check("a replay past the last pending entry still reports the stream",
          c.cmd("XREADGROUP", "GROUP", "g1", "alice", "STREAMS", "cg", "9-9"),
          [[b"cg", []]])
    check("a replay never blocks, even with BLOCK 0",
          c.cmd("XREADGROUP", "GROUP", "g1", "alice", "BLOCK", "0", "STREAMS", "cg", "9-9"),
          [[b"cg", []]])
    check("XACK removes an entry from the pending list",
          c.cmd("XACK", "cg", "g1", "1-1"), 1)
    check("XACK cannot remove the same entry twice", c.cmd("XACK", "cg", "g1", "1-1"), 0)
    check("XACK counts only what it removed", c.cmd("XACK", "cg", "g1", "1-1", "1-2"), 1)
    check("the pending list shrank", c.cmd("XPENDING", "cg", "g1")[0], 2)
    check("XACK rejects a malformed ID", c.cmd("XACK", "cg", "g1", "1-3", "nope"),
          pred=lambda g: is_err(g, "ERR Invalid stream ID specified as stream command argument"))
    check("XACK on a missing group is 0", c.cmd("XACK", "cg", "gone", "1-3"), 0)
    check("XPENDING on a missing group", c.cmd("XPENDING", "cg", "gone"),
          pred=lambda g: is_err(g, "NOGROUP No such key 'cg' or consumer group 'gone'"))

    # NOACK hands entries over without recording them
    c.cmd("DEL", "cg2")
    c.cmd("XADD", "cg2", "1-0", "f", "v")
    c.cmd("XGROUP", "CREATE", "cg2", "g", "0")
    check("XREADGROUP NOACK still delivers",
          c.cmd("XREADGROUP", "GROUP", "g", "alice", "NOACK", "STREAMS", "cg2", ">"),
          [[b"cg2", [[b"1-0", [b"f", b"v"]]]]])
    check("XREADGROUP NOACK recorded nothing", c.cmd("XPENDING", "cg2", "g")[0], 0)
    check("but it did create the consumer",
          [as_map(row)[b"name"] for row in c.cmd("XINFO", "CONSUMERS", "cg2", "g")], [b"alice"])

    # XGROUP SETID rewinds the group, XGROUP DELCONSUMER drops what it held
    check("XGROUP SETID rewinds to the beginning", c.cmd("XGROUP", "SETID", "cg2", "g", "0"), "OK")
    check("the rewound group hands the entry over again",
          c.cmd("XREADGROUP", "GROUP", "g", "bob", "STREAMS", "cg2", ">"),
          [[b"cg2", [[b"1-0", [b"f", b"v"]]]]])
    check("XGROUP CREATECONSUMER on an existing consumer",
          c.cmd("XGROUP", "CREATECONSUMER", "cg2", "g", "bob"), 0)
    check("XGROUP CREATECONSUMER on a new one",
          c.cmd("XGROUP", "CREATECONSUMER", "cg2", "g", "carol"), 1)
    check("XINFO GROUPS counts the consumers",
          as_map(c.cmd("XINFO", "GROUPS", "cg2")[0])[b"consumers"], 3)
    check("XGROUP DELCONSUMER returns what it was holding",
          c.cmd("XGROUP", "DELCONSUMER", "cg2", "g", "bob"), 1)
    check("the group's pending list shrank with it", c.cmd("XPENDING", "cg2", "g")[0], 0)
    check("XGROUP DELCONSUMER of a gone consumer",
          c.cmd("XGROUP", "DELCONSUMER", "cg2", "g", "bob"), 0)

    # ----- XREADGROUP history reports deleted entries as a nil field list ----
    c.cmd("DEL", "cg3")
    c.cmd("XGROUP", "CREATE", "cg3", "g", "$", "MKSTREAM")
    c.cmd("XADD", "cg3", "1-0", "field1", "A")
    c.cmd("XREADGROUP", "GROUP", "g", "alice", "STREAMS", "cg3", ">")
    c.cmd("XADD", "cg3", "MAXLEN", "1", "2-0", "field1", "B")
    c.cmd("XREADGROUP", "GROUP", "g", "alice", "STREAMS", "cg3", ">")
    check("a pending entry that was trimmed away replays with no fields",
          c.cmd("XREADGROUP", "GROUP", "g", "alice", "STREAMS", "cg3", "0"),
          [[b"cg3", [[b"1-0", None], [b"2-0", [b"field1", b"B"]]]]])

    # ----- XCLAIM -----
    c.cmd("DEL", "cl")
    for spec in ("1-0", "2-0", "3-0"):
        c.cmd("XADD", "cl", spec, "f", "v")
    c.cmd("XGROUP", "CREATE", "cl", "g", "0")
    c.cmd("XREADGROUP", "GROUP", "g", "alice", "STREAMS", "cl", ">")
    check("XCLAIM moves an entry to another consumer",
          c.cmd("XCLAIM", "cl", "g", "bob", "0", "1-0"), [[b"1-0", [b"f", b"v"]]])
    check("the entry is now bob's",
          [row[1] for row in c.cmd("XPENDING", "cl", "g", "-", "+", "10")],
          [b"bob", b"alice", b"alice"])
    check("XCLAIM incremented the delivery count",
          c.cmd("XPENDING", "cl", "g", "-", "+", "10")[0][3], 2)
    check("XCLAIM with a min-idle-time nothing meets claims nothing",
          c.cmd("XCLAIM", "cl", "g", "carol", "99999999", "2-0"), [])
    check("XCLAIM JUSTID answers the IDs alone",
          c.cmd("XCLAIM", "cl", "g", "carol", "0", "2-0", "JUSTID"), [b"2-0"])
    check("XCLAIM JUSTID left the delivery count alone",
          [row[3] for row in c.cmd("XPENDING", "cl", "g", "-", "+", "10", "carol")], [1])
    check("XCLAIM RETRYCOUNT sets the counter",
          c.cmd("XCLAIM", "cl", "g", "carol", "0", "2-0", "RETRYCOUNT", "7", "JUSTID"), [b"2-0"])
    check("the counter is what RETRYCOUNT said",
          [row[3] for row in c.cmd("XPENDING", "cl", "g", "-", "+", "10", "carol")], [7])
    check("XCLAIM IDLE backdates the delivery time",
          c.cmd("XCLAIM", "cl", "g", "carol", "0", "2-0", "IDLE", "5000", "JUSTID"), [b"2-0"])
    check("the entry now looks idle",
          c.cmd("XPENDING", "cl", "g", "-", "+", "10", "carol")[0][2],
          pred=lambda g: g >= 5000)
    check("XCLAIM TIME sets the delivery time outright",
          c.cmd("XCLAIM", "cl", "g", "carol", "0", "2-0", "TIME", "1", "JUSTID"), [b"2-0"])
    check("the entry is as idle as that time is old",
          c.cmd("XPENDING", "cl", "g", "-", "+", "10", "carol")[0][2],
          pred=lambda g: g > 1000000000)
    check("XCLAIM of an entry no consumer holds claims nothing without FORCE",
          c.cmd("XACK", "cl", "g", "3-0"), 1)
    check("and really claims nothing", c.cmd("XCLAIM", "cl", "g", "carol", "0", "3-0"), [])
    check("XCLAIM FORCE creates the pending entry",
          c.cmd("XCLAIM", "cl", "g", "carol", "0", "3-0", "FORCE"), [[b"3-0", [b"f", b"v"]]])
    check("the forced entry is in the pending list",
          [row[0] for row in c.cmd("XPENDING", "cl", "g", "-", "+", "10", "carol")],
          [b"2-0", b"3-0"])
    check("XCLAIM LASTID advances the group",
          c.cmd("XCLAIM", "cl", "g", "carol", "0", "3-0", "LASTID", "9-9", "JUSTID"), [b"3-0"])
    check("the group's last-delivered-id moved",
          as_map(c.cmd("XINFO", "GROUPS", "cl")[0])[b"last-delivered-id"], b"9-9")
    check("XCLAIM drops a pending entry whose stream entry is gone",
          c.cmd("XDEL", "cl", "2-0"), 1)
    check("and reports nothing for it", c.cmd("XCLAIM", "cl", "g", "dave", "0", "2-0"), [])
    check("the pending entry went with it",
          [row[0] for row in c.cmd("XPENDING", "cl", "g", "-", "+", "10")], [b"1-0", b"3-0"])
    check("XCLAIM on a missing group", c.cmd("XCLAIM", "cl", "gone", "x", "0", "1-0"),
          pred=lambda g: is_err(g, "NOGROUP No such key 'cl' or consumer group 'gone'"))

    # ----- XAUTOCLAIM -----
    c.cmd("DEL", "ac")
    ids = [c.cmd("XADD", "ac", "%d-0" % j, "f", str(j)) for j in range(1, 6)]
    c.cmd("XGROUP", "CREATE", "ac", "g", "0")
    c.cmd("XREADGROUP", "GROUP", "g", "alice", "COUNT", "90", "STREAMS", "ac", ">")
    reply = c.cmd("XAUTOCLAIM", "ac", "g", "bob", "0", "-", "COUNT", "2")
    check("XAUTOCLAIM answers a cursor, the claims and the dropped IDs",
          [reply[0], [row[0] for row in reply[1]], reply[2]],
          [b"3-0", [b"1-0", b"2-0"], []])
    reply = c.cmd("XAUTOCLAIM", "ac", "g", "bob", "0", reply[0], "COUNT", "2")
    check("the cursor continues the walk",
          [reply[0], [row[0] for row in reply[1]]], [b"5-0", [b"3-0", b"4-0"]])
    reply = c.cmd("XAUTOCLAIM", "ac", "g", "bob", "0", reply[0], "COUNT", "2")
    check("the walk ends with a zero cursor",
          [reply[0], [row[0] for row in reply[1]]], [b"0-0", [b"5-0"]])
    check("XAUTOCLAIM claimed everything for bob",
          len(c.cmd("XPENDING", "ac", "g", "-", "+", "10", "bob")), 5)
    check("XAUTOCLAIM JUSTID answers IDs alone",
          c.cmd("XAUTOCLAIM", "ac", "g", "carol", "0", "-", "JUSTID")[1],
          [b"1-0", b"2-0", b"3-0", b"4-0", b"5-0"])
    c.cmd("XDEL", "ac", "1-0")
    c.cmd("XDEL", "ac", "2-0")
    reply = c.cmd("XAUTOCLAIM", "ac", "g", "dave", "0", "-")
    check("XAUTOCLAIM reports the pending entries whose entries are gone",
          [reply[0], [row[0] for row in reply[1]], reply[2]],
          [b"0-0", [b"3-0", b"4-0", b"5-0"], [b"1-0", b"2-0"]])
    check("and it dropped them from the pending list",
          len(c.cmd("XPENDING", "ac", "g", "-", "+", "10")), 3)
    check("XAUTOCLAIM refuses COUNT 0",
          c.cmd("XAUTOCLAIM", "ac", "g", "x", "0", "-", "COUNT", "0"),
          pred=lambda g: is_err(g, "ERR COUNT must be > 0"))
    check("XAUTOCLAIM refuses an out-of-range COUNT",
          c.cmd("XAUTOCLAIM", "ac", "g", "x", "0", "-", "COUNT", "8070450532247928833"),
          pred=lambda g: is_err(g, "ERR COUNT"))
    check("XAUTOCLAIM on a missing group", c.cmd("XAUTOCLAIM", "ac", "gone", "x", "0", "-"),
          pred=lambda g: is_err(g, "NOGROUP No such key 'ac' or consumer group 'gone'"))

    # ----- XINFO GROUPS, XINFO CONSUMERS and the lag arithmetic -----
    c.cmd("DEL", "lg")
    for j in range(1, 6):
        c.cmd("XADD", "lg", "%d-0" % j, "data", str(j))
    c.cmd("XGROUP", "CREATE", "lg", "g1", "0")
    info = as_map(c.cmd("XINFO", "GROUPS", "lg")[0])
    check("XINFO GROUPS has every documented field", sorted(info.keys()),
          sorted([b"name", b"consumers", b"pending", b"last-delivered-id",
                  b"entries-read", b"lag"]))
    check("a fresh group has read nothing", info[b"entries-read"], None)
    check("and its lag is the whole stream", info[b"lag"], 5)
    c.cmd("XREADGROUP", "GROUP", "g1", "c11", "COUNT", "1", "STREAMS", "lg", ">")
    info = as_map(c.cmd("XINFO", "GROUPS", "lg")[0])
    check("reading one entry makes the read counter knowable", info[b"entries-read"], 1)
    check("and drops the lag by one", info[b"lag"], 4)
    c.cmd("XREADGROUP", "GROUP", "g1", "c12", "COUNT", "10", "STREAMS", "lg", ">")
    info = as_map(c.cmd("XINFO", "GROUPS", "lg")[0])
    check("reading the rest catches the group up", [info[b"entries-read"], info[b"lag"]], [5, 0])
    c.cmd("XADD", "lg", "6-0", "data", "6")
    info = as_map(c.cmd("XINFO", "GROUPS", "lg")[0])
    check("a new entry is one entry of lag", [info[b"entries-read"], info[b"lag"]], [5, 1])

    # A tombstone in front of the group makes both counters unknowable, exactly
    # as it does in Redis.
    c.cmd("DEL", "lg2")
    for j in range(1, 6):
        c.cmd("XADD", "lg2", "%d-0" % j, "data", str(j))
    c.cmd("XDEL", "lg2", "3-0")
    c.cmd("XGROUP", "CREATE", "lg2", "g1", "0")
    c.cmd("XGROUP", "CREATE", "lg2", "g2", "0")
    info = as_map(c.cmd("XINFO", "GROUPS", "lg2")[0])
    check("a group behind a tombstone has no knowable lag",
          [info[b"entries-read"], info[b"lag"]], [None, None])
    for _ in range(4):
        c.cmd("XREADGROUP", "GROUP", "g1", "c11", "COUNT", "1", "STREAMS", "lg2", ">")
    info = as_map(c.cmd("XINFO", "GROUPS", "lg2")[0])
    check("reaching the end of the stream makes it knowable again",
          [info[b"entries-read"], info[b"lag"]], [5, 0])
    c.cmd("XTRIM", "lg2", "MINID", "=", "3-0")
    rows = {as_map(row)[b"name"]: as_map(row) for row in c.cmd("XINFO", "GROUPS", "lg2")}
    check("trimming past the tombstone makes the untouched group's lag knowable",
          [rows[b"g2"][b"entries-read"], rows[b"g2"][b"lag"]], [None, 2])

    check("XINFO CONSUMERS has every documented field",
          sorted(as_map(c.cmd("XINFO", "CONSUMERS", "lg", "g1")[0]).keys()),
          sorted([b"name", b"pending", b"idle", b"inactive"]))
    c.cmd("DEL", "cs")
    c.cmd("XGROUP", "CREATE", "cs", "g", "$", "MKSTREAM")
    c.cmd("XREADGROUP", "GROUP", "g", "alice", "COUNT", "1", "STREAMS", "cs", ">")
    row = as_map(c.cmd("XINFO", "CONSUMERS", "cs", "g")[0])
    check("a consumer that has never been handed an entry reports inactive -1",
          row[b"inactive"], -1)
    c.cmd("XADD", "cs", "1-0", "f", "v")
    c.cmd("XREADGROUP", "GROUP", "g", "alice", "COUNT", "1", "STREAMS", "cs", ">")
    row = as_map(c.cmd("XINFO", "CONSUMERS", "cs", "g")[0])
    check("once it has, inactive is a real age", row[b"inactive"],
          pred=lambda g: 0 <= g < 5000)
    check("and it holds the entry it was handed", row[b"pending"], 1)
    check("XINFO CONSUMERS on a missing group", c.cmd("XINFO", "CONSUMERS", "cs", "gone"),
          pred=lambda g: is_err(g, "NOGROUP No such consumer group 'gone' for key name 'cs'"))
    check("XINFO GROUPS on a missing key", c.cmd("XINFO", "GROUPS", "cs-missing"),
          pred=lambda g: is_err(g, "ERR no such key"))

    # XINFO STREAM FULL carries the groups, their pending lists and consumers
    full = as_map(c.cmd("XINFO", "STREAM", "cs", "FULL"))
    check("XINFO STREAM FULL lists the groups", len(full[b"groups"]), 1)
    group = as_map(full[b"groups"][0])
    check("the group row names itself", group[b"name"], b"g")
    check("the group row carries its pending list",
          [row[0] for row in group[b"pending"]], [b"1-0"])
    consumer = as_map(group[b"consumers"][0])
    check("the consumer row names itself", consumer[b"name"], b"alice")
    check("the consumer row carries its own pending list",
          [row[0] for row in consumer[b"pending"]], [b"1-0"])

    # ----- XREADGROUP errors -----
    check("XREADGROUP on a missing group",
          c.cmd("XREADGROUP", "GROUP", "gone", "x", "STREAMS", "cs", ">"),
          pred=lambda g: is_err(g, "NOGROUP No such key 'cs' or consumer group 'gone' "
                                   "in XREADGROUP with GROUP option"))
    check("XREADGROUP refuses $",
          c.cmd("XREADGROUP", "GROUP", "g", "x", "STREAMS", "cs", "$"),
          pred=lambda g: is_err(g, "ERR The $ ID is meaningless in the context of "
                                   "XREADGROUP: you want to read the history of this "
                                   "consumer by specifying a proper ID, or use the > ID "
                                   "to get new messages. The $ ID would just return an "
                                   "empty result set."))
    check("XREADGROUP needs the GROUP option",
          c.cmd("XREADGROUP", "STREAMS", "cs", ">"),
          pred=lambda g: is_err(g, "ERR Missing GROUP keyword or consumer/group name in "
                                   "XREADGROUP context"))
    check("XREADGROUP needs one ID per key",
          c.cmd("XREADGROUP", "GROUP", "g", "x", "COUNT", "1", "STREAMS", "cs"),
          pred=lambda g: is_err(g, "ERR Unbalanced 'xreadgroup' list of streams: for each "
                                   "stream key an ID or '>' must be specified."))
    check("XREAD refuses the GROUP option",
          c.cmd("XREAD", "GROUP", "g", "x", "STREAMS", "cs", "0"),
          pred=lambda g: is_err(g, "ERR The GROUP option is only supported by XREADGROUP. "
                                   "You called XREAD instead."))
    check("XREAD refuses the NOACK option",
          c.cmd("XREAD", "NOACK", "STREAMS", "cs", "0"),
          pred=lambda g: is_err(g, "ERR The NOACK option is only supported by XREADGROUP. "
                                   "You called XREAD instead."))
    c.cmd("SET", "cgplain", "x")
    for name, args in (("XREADGROUP", ("XREADGROUP", "GROUP", "g", "x", "STREAMS", "cgplain", ">")),
                       ("XGROUP", ("XGROUP", "CREATE", "cgplain", "g", "$")),
                       ("XACK", ("XACK", "cgplain", "g", "1-1")),
                       ("XPENDING", ("XPENDING", "cgplain", "g")),
                       ("XCLAIM", ("XCLAIM", "cgplain", "g", "x", "0", "1-1")),
                       ("XAUTOCLAIM", ("XAUTOCLAIM", "cgplain", "g", "x", "0", "-")),
                       ("XINFO GROUPS", ("XINFO", "GROUPS", "cgplain"))):
        check("%s on a string is WRONGTYPE" % name, c.cmd(*args),
              pred=lambda g: is_err(g, "WRONGTYPE"))

    # ----- blocking XREADGROUP -----
    reader = Resp(host, port)
    writer = Resp(host, port)
    c.cmd("DEL", "bg")
    c.cmd("XGROUP", "CREATE", "bg", "g", "$", "MKSTREAM")
    started = time.time()
    check("a blocking XREADGROUP with nothing to hand over times out with nil",
          reader.cmd("XREADGROUP", "GROUP", "g", "alice", "BLOCK", "400", "STREAMS", "bg", ">"),
          None)
    check("and it waited roughly as long as it was asked to", time.time() - started,
          pred=lambda g: 0.3 <= g < 5.0)
    reader.send("XREADGROUP", "GROUP", "g", "alice", "BLOCK", "10000", "STREAMS", "bg", ">")
    time.sleep(0.4)
    started = time.time()
    writer.cmd("XADD", "bg", "1-0", "woken", "yes")
    check("an XADD wakes a blocked XREADGROUP", reader.read(),
          [[b"bg", [[b"1-0", [b"woken", b"yes"]]]]])
    check("the wake arrived well inside the BLOCK timeout", time.time() - started,
          pred=lambda g: g < 3.0)
    check("the woken read recorded the entry it was handed",
          c.cmd("XPENDING", "bg", "g")[0], 1)
    reader.send("XREADGROUP", "GROUP", "g", "alice", "BLOCK", "10000", "STREAMS", "bg", ">")
    time.sleep(0.4)
    writer.cmd("XGROUP", "DESTROY", "bg", "g")
    check("destroying the group unblocks the reader with NOGROUP", reader.read(),
          pred=lambda g: is_err(g, "NOGROUP"))
    reader.sock.close()
    writer.sock.close()

    for key in ("cg", "cg2", "cg3", "cl", "ac", "lg", "lg2", "cs", "cgplain", "bg"):
        c.cmd("DEL", key)

def zrange_scores(reply):
    """ZRANGE ... WITHSCORES as {member: float}."""
    return {reply[i]: float(reply[i + 1]) for i in range(0, len(reply), 2)}


def check_geo_store(host, port, c, near, coord_near):
    """GEOSEARCHSTORE and the STORE/STOREDIST forms of the GEORADIUS pair.

    The search reads and the destination replace are one storage transaction
    (the interactive session), so the destination is never seen half written.
    """
    c.cmd("DEL", "GeoSrc", "GeoDst")
    check("GEOADD store source",
          c.cmd("GEOADD", "GeoSrc", "13.361389", "38.115556", "Palermo",
                "15.087269", "37.502669", "Catania"), 2)

    # Redis documentation example for GEOSEARCHSTORE.
    check("GEOSEARCHSTORE STOREDIST doc example",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "15", "37",
                "BYBOX", "400", "400", "km", "ASC", "COUNT", "3", "STOREDIST"), 2)
    scores = zrange_scores(c.cmd("ZRANGE", "GeoDst", "0", "-1", "WITHSCORES"))
    check("GEOSEARCHSTORE STOREDIST members", sorted(scores), [b"Catania", b"Palermo"])
    check("GEOSEARCHSTORE STOREDIST Catania km", scores.get(b"Catania"),
          pred=lambda g: near(g, 56.4413, 5e-4))
    check("GEOSEARCHSTORE STOREDIST Palermo km", scores.get(b"Palermo"),
          pred=lambda g: near(g, 190.4424, 5e-4))
    check("GEOSEARCHSTORE destination is a zset", c.cmd("TYPE", "GeoDst"), "zset")

    # Without STOREDIST the score is the member's own geohash, so the geo
    # readers all work on the destination.
    check("GEOSEARCHSTORE stores geohash scores",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "15", "37",
                "BYBOX", "400", "400", "km", "ASC"), 2)
    check("GEOSEARCHSTORE geohash scores match the source",
          c.cmd("ZRANGE", "GeoDst", "0", "-1", "WITHSCORES"),
          c.cmd("ZRANGE", "GeoSrc", "0", "-1", "WITHSCORES"))
    check("GEOPOS decodes the stored geohash", c.cmd("GEOPOS", "GeoDst", "Palermo")[0],
          pred=lambda g: coord_near(g, 13.361389, 38.115556))
    check("GEODIST works on the destination", c.cmd("GEODIST", "GeoDst", "Palermo", "Catania", "km"),
          b"166.2742")

    # BYRADIUS, and a COUNT that really truncates.
    check("GEOSEARCHSTORE BYRADIUS",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "15", "37",
                "BYRADIUS", "200", "km"), 2)
    check("GEOSEARCHSTORE BYRADIUS COUNT 1 keeps the closest",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "15", "37",
                "BYRADIUS", "200", "km", "COUNT", "1"), 1)
    check("GEOSEARCHSTORE COUNT 1 stored Catania", c.cmd("ZRANGE", "GeoDst", "0", "-1"), [b"Catania"])
    check("GEOSEARCHSTORE DESC COUNT 1 stored Palermo",
          [c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "15", "37",
                 "BYRADIUS", "200", "km", "DESC", "COUNT", "1"),
           c.cmd("ZRANGE", "GeoDst", "0", "-1")],
          [1, [b"Palermo"]])

    # FROMMEMBER: the center is the member's own position, read in the same
    # transaction as the boxes and the write.
    check("GEOSEARCHSTORE FROMMEMBER",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMMEMBER", "Palermo",
                "BYRADIUS", "200", "km", "ASC"), 2)
    check("GEOSEARCHSTORE FROMMEMBER ordering", c.cmd("ZRANGE", "GeoDst", "0", "-1"),
          pred=lambda g: sorted(g) == [b"Catania", b"Palermo"])
    check("GEOSEARCHSTORE FROMMEMBER missing member",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMMEMBER", "NoSuch",
                "BYRADIUS", "200", "km"),
          pred=lambda g: is_err(g, "ERR") and "could not decode requested zset member" in str(g))
    check("GEOSEARCHSTORE left the destination alone after that error",
          c.cmd("ZCARD", "GeoDst"), 2)

    # The destination is replaced whatever type it held.
    c.cmd("DEL", "GeoDst")
    c.cmd("RPUSH", "GeoDst", "a", "b", "c")
    check("GEOSEARCHSTORE replaces a list destination",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "15", "37",
                "BYRADIUS", "200", "km"), 2)
    check("replaced destination is a zset", c.cmd("TYPE", "GeoDst"), "zset")
    check("replaced destination holds the matches", sorted(c.cmd("ZRANGE", "GeoDst", "0", "-1")),
          [b"Catania", b"Palermo"])
    c.cmd("DEL", "GeoDst")
    c.cmd("SET", "GeoDst", "a string")
    check("GEOSEARCHSTORE replaces a string destination",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "15", "37",
                "BYRADIUS", "200", "km"), 2)
    check("replaced string destination is a zset", c.cmd("TYPE", "GeoDst"), "zset")

    # Zero matches delete the destination and answer 0.
    check("GEOSEARCHSTORE with no matches answers 0",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "15", "37",
                "BYRADIUS", "1", "m"), 0)
    check("GEOSEARCHSTORE deleted the destination", c.cmd("EXISTS", "GeoDst"), 0)
    check("GEOSEARCHSTORE missing source answers 0",
          [c.cmd("GEOADD", "GeoDst", "1", "1", "leftover"),
           c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoNoSuchSource", "FROMLONLAT", "15", "37",
                 "BYRADIUS", "200", "km"),
           c.cmd("EXISTS", "GeoDst")],
          [1, 0, 0])
    check("GEOSEARCHSTORE FROMMEMBER on a missing source answers 0",
          [c.cmd("GEOADD", "GeoDst", "1", "1", "leftover"),
           c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoNoSuchSource", "FROMMEMBER", "m",
                 "BYRADIUS", "200", "km"),
           c.cmd("EXISTS", "GeoDst")],
          [1, 0, 0])

    # A source of another type is a WRONGTYPE, and nothing is written.
    c.cmd("DEL", "GeoStringSrc")
    c.cmd("SET", "GeoStringSrc", "not-a-geo-set")
    c.cmd("GEOADD", "GeoDst", "1", "1", "untouched")
    check("GEOSEARCHSTORE WRONGTYPE source",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoStringSrc", "FROMLONLAT", "15", "37",
                "BYRADIUS", "200", "km"),
          pred=lambda g: is_err(g, "WRONGTYPE"))
    check("GEOSEARCHSTORE WRONGTYPE FROMMEMBER source",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoStringSrc", "FROMMEMBER", "m",
                "BYRADIUS", "200", "km"),
          pred=lambda g: is_err(g, "WRONGTYPE"))
    check("GEOSEARCHSTORE left the destination alone on WRONGTYPE",
          c.cmd("ZRANGE", "GeoDst", "0", "-1"), [b"untouched"])

    # GEORADIUS STORE / STOREDIST over the same data.
    check("GEORADIUS STORE", c.cmd("GEORADIUS", "GeoSrc", "15", "37", "200", "km",
                                   "STORE", "GeoDst"), 2)
    check("GEORADIUS STORE wrote geohash scores",
          c.cmd("ZRANGE", "GeoDst", "0", "-1", "WITHSCORES"),
          c.cmd("ZRANGE", "GeoSrc", "0", "-1", "WITHSCORES"))
    check("GEORADIUS STOREDIST", c.cmd("GEORADIUS", "GeoSrc", "15", "37", "200", "km",
                                       "STOREDIST", "GeoDst"), 2)
    dist = zrange_scores(c.cmd("ZRANGE", "GeoDst", "0", "-1", "WITHSCORES"))
    check("GEORADIUS STOREDIST Catania km", dist.get(b"Catania"), pred=lambda g: near(g, 56.4413, 5e-4))
    check("GEORADIUS STOREDIST Palermo km", dist.get(b"Palermo"), pred=lambda g: near(g, 190.4424, 5e-4))
    check("GEORADIUS STOREDIST m uses the unit asked for",
          [c.cmd("GEORADIUS", "GeoSrc", "15", "37", "200000", "m", "STOREDIST", "GeoDst"),
           zrange_scores(c.cmd("ZRANGE", "GeoDst", "0", "-1", "WITHSCORES")).get(b"Catania")],
          pred=lambda g: g[0] == 2 and near(g[1], 56441.2579, 1.0))
    check("GEORADIUSBYMEMBER STORE",
          c.cmd("GEORADIUSBYMEMBER", "GeoSrc", "Palermo", "200", "km", "STORE", "GeoDst"), 2)
    check("GEORADIUSBYMEMBER STORE members", sorted(c.cmd("ZRANGE", "GeoDst", "0", "-1")),
          [b"Catania", b"Palermo"])
    check("GEORADIUSBYMEMBER STOREDIST puts the center first",
          [c.cmd("GEORADIUSBYMEMBER", "GeoSrc", "Catania", "500", "km", "STOREDIST", "GeoDst"),
           c.cmd("ZRANGE", "GeoDst", "0", "-1")],
          [2, [b"Catania", b"Palermo"]])
    check("GEORADIUSBYMEMBER STOREDIST center distance is zero",
          zrange_scores(c.cmd("ZRANGE", "GeoDst", "0", "-1", "WITHSCORES")).get(b"Catania"),
          pred=lambda g: near(g, 0.0, 1e-3))

    # GEOSEARCHSTORE inside MULTI/EXEC: queued, then run with the rest of the
    # transaction and answered in order.
    c.cmd("DEL", "GeoDst")
    check("MULTI before GEOSEARCHSTORE", c.cmd("MULTI"), "OK")
    check("GEOSEARCHSTORE is queued",
          c.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "15", "37",
                "BYRADIUS", "200", "km", "STOREDIST"), "QUEUED")
    check("ZCARD is queued", c.cmd("ZCARD", "GeoDst"), "QUEUED")
    check("SET is queued", c.cmd("SET", "GeoMultiWitness", "yes"), "QUEUED")
    check("EXEC runs GEOSEARCHSTORE with the rest", c.cmd("EXEC"), [2, 2, "OK"])
    check("MULTI GEOSEARCHSTORE really stored", sorted(c.cmd("ZRANGE", "GeoDst", "0", "-1")),
          [b"Catania", b"Palermo"])
    check("MULTI companion command also ran", c.cmd("GET", "GeoMultiWitness"), b"yes")

    # A WATCH on the destination is invalidated by a GEOSEARCHSTORE elsewhere,
    # and a WATCH on the source it only reads is not.
    other = Resp(host, port)
    c.cmd("WATCH", "GeoDst")
    other.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "15", "37", "BYRADIUS", "200", "km")
    c.cmd("MULTI")
    c.cmd("PING")
    check("WATCH on the destination is invalidated", c.cmd("EXEC"), None)
    c.cmd("WATCH", "GeoSrc")
    other.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "15", "37", "BYRADIUS", "200", "km")
    c.cmd("MULTI")
    c.cmd("PING")
    check("WATCH on the source it only reads survives", c.cmd("EXEC"), ["PONG"])
    other.cmd("QUIT")

    # Database 1 stores into database 1 and leaves database 0 alone.
    c1 = Resp(host, port)
    check("SELECT 1 for the geo store", c1.cmd("SELECT", "1"), "OK")
    c1.cmd("DEL", "GeoSrc", "GeoDst")
    c1.cmd("GEOADD", "GeoSrc", "13.361389", "38.115556", "Palermo")
    before = c.cmd("ZRANGE", "GeoDst", "0", "-1")
    check("GEOSEARCHSTORE in database 1",
          c1.cmd("GEOSEARCHSTORE", "GeoDst", "GeoSrc", "FROMLONLAT", "13.361389", "38.115556",
                 "BYRADIUS", "10", "km"), 1)
    check("database 1 destination holds its own member", c1.cmd("ZRANGE", "GeoDst", "0", "-1"),
          [b"Palermo"])
    check("database 0 destination is untouched", c.cmd("ZRANGE", "GeoDst", "0", "-1"), before)
    c1.cmd("DEL", "GeoSrc", "GeoDst")
    c1.cmd("QUIT")

    c.cmd("DEL", "GeoSrc", "GeoDst", "GeoStringSrc", "GeoMultiWitness")


def stream_id_tuple(raw):
    ms, _, seq = raw.decode().partition("-")
    return (int(ms), int(seq))

def main():
    host, port = sys.argv[1], int(sys.argv[2])
    c = Resp(host, port)
    c.cmd("FLUSHALL")

    # TOUCH
    c.cmd("SET", "t1", "v")
    check("TOUCH counts existing keys", c.cmd("TOUCH", "t1", "t-missing"), 1)

    # SORT LIMIT / SORT_RO
    c.cmd("RPUSH", "sl", "3", "1", "2")
    check("SORT LIMIT 1 1", c.cmd("SORT", "sl", "LIMIT", "1", "1"), [b"2"])
    check("SORT LIMIT 1 -1", c.cmd("SORT", "sl", "LIMIT", "1", "-1"), [b"2", b"3"])
    check("SORT_RO", c.cmd("SORT_RO", "sl"), [b"1", b"2", b"3"])
    # The adapter reports parser syntax errors as "ERR protocol error: syntax error".
    check("SORT_RO rejects STORE", c.cmd("SORT_RO", "sl", "STORE", "x"), pred=lambda g: is_err(g, "ERR") and "syntax error" in str(g))
    check("SORT LIMIT STORE", c.cmd("SORT", "sl", "LIMIT", "0", "2", "STORE", "sl2"), 2)
    check("SORT LIMIT STORE contents", c.cmd("LRANGE", "sl2", "0", "-1"), [b"1", b"2"])

    # BITCOUNT (values from the Redis documentation examples)
    c.cmd("SET", "bc", "foobar")
    check("BITCOUNT all", c.cmd("BITCOUNT", "bc"), 26)
    check("BITCOUNT 0 0", c.cmd("BITCOUNT", "bc", "0", "0"), 4)
    check("BITCOUNT 1 1", c.cmd("BITCOUNT", "bc", "1", "1"), 6)
    check("BITCOUNT 1 1 BYTE", c.cmd("BITCOUNT", "bc", "1", "1", "BYTE"), 6)
    check("BITCOUNT 5 30 BIT", c.cmd("BITCOUNT", "bc", "5", "30", "BIT"), 17)
    check("BITCOUNT missing", c.cmd("BITCOUNT", "bc-missing"), 0)
    c.cmd("RPUSH", "bc-list", "x")
    check("BITCOUNT wrongtype", c.cmd("BITCOUNT", "bc-list"), pred=lambda g: is_err(g, "WRONGTYPE"))

    # BITPOS (Redis documentation examples)
    c.cmd("SET", "bp1", b"\xff\xf0\x00")
    check("BITPOS first 0", c.cmd("BITPOS", "bp1", "0"), 12)
    c.cmd("SET", "bp2", b"\x00\xff\xf0")
    check("BITPOS 1 from byte 0", c.cmd("BITPOS", "bp2", "1", "0"), 8)
    check("BITPOS 1 2 -1 BYTE", c.cmd("BITPOS", "bp2", "1", "2", "-1", "BYTE"), 16)
    check("BITPOS 1 7 15 BIT", c.cmd("BITPOS", "bp2", "1", "7", "15", "BIT"), 8)
    c.cmd("SET", "bp3", b"\x00\x00\x00")
    check("BITPOS 1 none", c.cmd("BITPOS", "bp3", "1"), -1)
    c.cmd("SET", "bp4", b"\xff\xff")
    check("BITPOS 0 all ones no end", c.cmd("BITPOS", "bp4", "0"), 16)
    check("BITPOS 0 all ones with end", c.cmd("BITPOS", "bp4", "0", "0", "1"), -1)
    check("BITPOS missing bit 1", c.cmd("BITPOS", "bp-missing", "1"), -1)
    check("BITPOS missing bit 0", c.cmd("BITPOS", "bp-missing", "0"), 0)

    # BITOP (Redis documentation example)
    c.cmd("SET", "k1", "foobar")
    c.cmd("SET", "k2", "abcdef")
    check("BITOP AND len", c.cmd("BITOP", "AND", "dest", "k1", "k2"), 6)
    check("BITOP AND value", c.cmd("GET", "dest"), b"`bc`ab")
    check("BITOP OR len", c.cmd("BITOP", "OR", "dest2", "k1", "k2"), 6)
    check("BITOP NOT", c.cmd("BITOP", "NOT", "dest3", "k1"), 6)
    check("BITOP NOT value", c.cmd("GET", "dest3"), bytes(~b & 0xFF for b in b"foobar"))
    check("BITOP NOT multi src rejected", c.cmd("BITOP", "NOT", "d", "k1", "k2"), pred=lambda g: is_err(g, "ERR"))
    check("BITOP empty result deletes dest", c.cmd("BITOP", "AND", "dest", "missing1", "missing2"), 0)
    check("BITOP dest gone", c.cmd("EXISTS", "dest"), 0)
    check("BITOP wrongtype source", c.cmd("BITOP", "OR", "d", "bc-list"), pred=lambda g: is_err(g, "WRONGTYPE"))

    # HyperLogLog: PFADD / PFCOUNT / PFMERGE (phase 3 package 1)
    # The sketch is a private dense format kept in a plain string value.
    check("PFADD doc example", c.cmd("PFADD", "hll", "a", "b", "c", "d", "e", "f", "g"), 1)
    check("PFCOUNT doc example", c.cmd("PFCOUNT", "hll"), 7)
    check("PFADD known element", c.cmd("PFADD", "hll", "a"), 0)

    check("PFADD no elements creates", c.cmd("PFADD", "hll-empty"), 1)
    check("PFCOUNT empty sketch", c.cmd("PFCOUNT", "hll-empty"), 0)
    check("PFADD no elements again", c.cmd("PFADD", "hll-empty"), 0)
    check("PFCOUNT missing key", c.cmd("PFCOUNT", "hll-gone"), 0)

    # Multi-key PFCOUNT is the cardinality of the union and touches nothing.
    left = ["u%d" % i for i in range(120)]
    right = ["u%d" % i for i in range(80, 200)]
    c.cmd("PFADD", "hll1", *left)
    c.cmd("PFADD", "hll2", *right)
    true_union = len(set(left) | set(right))
    before = c.cmd("PFCOUNT", "hll1")
    tolerance = max(1, int(round(true_union * 0.02)))
    check(
        "PFCOUNT union of two keys",
        c.cmd("PFCOUNT", "hll1", "hll2"),
        pred=lambda g: isinstance(g, int) and abs(g - true_union) <= tolerance,
    )
    check("PFCOUNT leaves sources alone", c.cmd("PFCOUNT", "hll1"), before)
    check("PFCOUNT union is order independent",
          c.cmd("PFCOUNT", "hll2", "hll1"), c.cmd("PFCOUNT", "hll1", "hll2"))

    # 10,000 distinct elements, within the documented 1.5% band.
    for base in range(0, 10000, 500):
        c.cmd("PFADD", "hll-big", *["e%d" % i for i in range(base, base + 500)])
    check(
        "PFCOUNT 10k within 1.5%",
        c.cmd("PFCOUNT", "hll-big"),
        pred=lambda g: isinstance(g, int) and abs(g - 10000) <= 150,
    )

    # PFMERGE
    c.cmd("PFADD", "hll-src1", *["m%d" % i for i in range(100)])
    c.cmd("PFADD", "hll-src2", *["m%d" % i for i in range(50, 150)])
    check("PFMERGE", c.cmd("PFMERGE", "hll-dest", "hll-src1", "hll-src2"), "OK")
    check(
        "PFMERGE count matches union",
        c.cmd("PFCOUNT", "hll-dest"),
        pred=lambda g: isinstance(g, int) and abs(g - 150) <= 3,
    )
    check("PFMERGE equals multi-key PFCOUNT",
          c.cmd("PFCOUNT", "hll-dest"), c.cmd("PFCOUNT", "hll-src1", "hll-src2"))
    check("PFMERGE is idempotent", c.cmd("PFMERGE", "hll-dest", "hll-src1"), "OK")
    check("PFMERGE idempotent count", c.cmd("PFCOUNT", "hll-dest"),
          c.cmd("PFCOUNT", "hll-src1", "hll-src2"))
    check("PFMERGE onto missing dest", c.cmd("PFMERGE", "hll-dest2", "hll-src1"), "OK")
    check("PFMERGE new dest count", c.cmd("PFCOUNT", "hll-dest2"), c.cmd("PFCOUNT", "hll-src1"))

    # The sketch is an ordinary string value for every other command.
    check("TYPE of a sketch", c.cmd("TYPE", "hll"), "string")
    check("GET of a sketch", c.cmd("GET", "hll"),
          pred=lambda g: isinstance(g, bytes) and g.startswith(b"MHLL"))
    check("STRLEN of a sketch", c.cmd("STRLEN", "hll"), 16400)
    check("DEL of a sketch", c.cmd("DEL", "hll"), 1)

    # A plain string is not a valid sketch; another type is a plain WRONGTYPE.
    c.cmd("SET", "hll-plain", "x")
    check("PFADD on a plain string", c.cmd("PFADD", "hll-plain", "a"),
          pred=lambda g: is_err(g, "WRONGTYPE Key is not a valid HyperLogLog"))
    check("PFCOUNT on a plain string", c.cmd("PFCOUNT", "hll-plain"),
          pred=lambda g: is_err(g, "WRONGTYPE Key is not a valid HyperLogLog"))
    check("PFMERGE from a plain string", c.cmd("PFMERGE", "hll-d3", "hll-plain"),
          pred=lambda g: is_err(g, "WRONGTYPE Key is not a valid HyperLogLog"))
    c.cmd("RPUSH", "hll-list", "x")
    check("PFADD on a list", c.cmd("PFADD", "hll-list", "a"),
          pred=lambda g: is_err(g, "WRONGTYPE Operation against"))
    check("PFCOUNT on a list", c.cmd("PFCOUNT", "hll-list"),
          pred=lambda g: is_err(g, "WRONGTYPE Operation against"))
    check("PFADD arity", c.cmd("PFADD"), pred=lambda g: is_err(g, "ERR wrong number"))
    check("PFCOUNT arity", c.cmd("PFCOUNT"), pred=lambda g: is_err(g, "ERR wrong number"))
    check("PFMERGE arity", c.cmd("PFMERGE"), pred=lambda g: is_err(g, "ERR wrong number"))

    # Hash-tag style key names and TTLs behave like any other string key.
    check("PFADD tagged key", c.cmd("PFADD", "{visits}:2026", "a", "b"), 1)
    check("EXPIRE on a sketch", c.cmd("EXPIRE", "{visits}:2026", "100"), 1)
    check("PFADD after EXPIRE", c.cmd("PFADD", "{visits}:2026", "c"), 1)
    check("PFCOUNT after EXPIRE", c.cmd("PFCOUNT", "{visits}:2026"), 3)
    check("PFADD keeps the TTL", c.cmd("TTL", "{visits}:2026"),
          pred=lambda g: isinstance(g, int) and 0 < g <= 100)

    # BITFIELD_RO
    c.cmd("SET", "bf", b"\x01\x02\xff")
    check("BITFIELD_RO u8 0", c.cmd("BITFIELD_RO", "bf", "GET", "u8", "0"), [1])
    check("BITFIELD_RO u16 0", c.cmd("BITFIELD_RO", "bf", "GET", "u16", "0"), [258])
    check("BITFIELD_RO i8 16", c.cmd("BITFIELD_RO", "bf", "GET", "i8", "16"), [-1])
    check("BITFIELD_RO u4 #1", c.cmd("BITFIELD_RO", "bf", "GET", "u4", "#1"), [1])
    check("BITFIELD_RO multi", c.cmd("BITFIELD_RO", "bf", "GET", "u8", "0", "GET", "u8", "8"), [1, 2])
    check("BITFIELD_RO beyond end", c.cmd("BITFIELD_RO", "bf", "GET", "u8", "100"), [0])
    check("BITFIELD_RO bad type", c.cmd("BITFIELD_RO", "bf", "GET", "u64", "0"), pred=lambda g: is_err(g, "ERR"))
    check("BITFIELD_RO rejects SET", c.cmd("BITFIELD_RO", "bf", "SET", "u8", "0", "1"), pred=lambda g: is_err(g, "ERR"))

    # BITFIELD (phase 3 package 2). Examples from the Redis documentation.
    check("BITFIELD doc example", c.cmd("BITFIELD", "bfw-doc", "INCRBY", "i5", "100", "1", "GET", "u4", "0"), [1, 0])

    # The documented overflow walk: WRAP on the first field, SAT on the second.
    for want in ([1, 1], [2, 2], [3, 3], [0, 3]):
        check("BITFIELD overflow walk %r" % want,
              c.cmd("BITFIELD", "bfw-ovf", "incrby", "u2", "100", "1", "OVERFLOW", "SAT", "incrby", "u2", "102", "1"),
              want)
    check("BITFIELD OVERFLOW FAIL returns nil", c.cmd("BITFIELD", "bfw-ovf", "OVERFLOW", "FAIL", "incrby", "u2", "102", "1"), [None])
    check("BITFIELD OVERFLOW FAIL wrote nothing", c.cmd("BITFIELD_RO", "bfw-ovf", "GET", "u2", "102"), [3])

    # SET replies with the previous value, GET with the current one.
    check("BITFIELD SET on a new key", c.cmd("BITFIELD", "bfw-set", "SET", "u8", "0", "255"), [0])
    check("BITFIELD SET returns the old value", c.cmd("BITFIELD", "bfw-set", "SET", "u8", "0", "1"), [255])
    check("BITFIELD GET after SET", c.cmd("BITFIELD", "bfw-set", "GET", "u8", "0"), [1])
    check("BITFIELD SET creates the key", c.cmd("EXISTS", "bfw-set"), 1)
    check("BITFIELD SET sizes the string", c.cmd("STRLEN", "bfw-set"), 1)

    # Signed wraparound, saturation and failure on the same value.
    check("BITFIELD SET i8 127", c.cmd("BITFIELD", "bfw-wrap", "SET", "i8", "0", "127"), [0])
    check("BITFIELD INCRBY wraps", c.cmd("BITFIELD", "bfw-wrap", "INCRBY", "i8", "0", "1"), [-128])
    check("BITFIELD SET i8 127 again", c.cmd("BITFIELD", "bfw-sat", "SET", "i8", "0", "127"), [0])
    check("BITFIELD INCRBY saturates", c.cmd("BITFIELD", "bfw-sat", "OVERFLOW", "SAT", "INCRBY", "i8", "0", "1"), [127])
    check("BITFIELD SAT left the value at the max", c.cmd("BITFIELD", "bfw-sat", "GET", "i8", "0"), [127])
    check("BITFIELD SET i8 127 once more", c.cmd("BITFIELD", "bfw-fail", "SET", "i8", "0", "127"), [0])
    check("BITFIELD INCRBY fails", c.cmd("BITFIELD", "bfw-fail", "OVERFLOW", "FAIL", "INCRBY", "i8", "0", "1"), [None])
    check("BITFIELD FAIL left the value alone", c.cmd("BITFIELD", "bfw-fail", "GET", "i8", "0"), [127])
    # SAT clamps a SET whose value does not fit the type, FAIL refuses it.
    check("BITFIELD SAT clamps a SET", c.cmd("BITFIELD", "bfw-satset", "OVERFLOW", "SAT", "SET", "u2", "0", "9", "GET", "u2", "0"), [0, 3])
    check("BITFIELD WRAP wraps a SET", c.cmd("BITFIELD", "bfw-wrapset", "SET", "u2", "0", "9", "GET", "u2", "0"), [0, 1])

    # "#" offsets address the n-th field of the given width.
    check("BITFIELD # offset", c.cmd("BITFIELD", "bfw-hash", "SET", "u8", "#1", "7", "GET", "u8", "#1"), [0, 7])
    check("BITFIELD # offset is bit 8", c.cmd("BITFIELD", "bfw-hash", "GET", "u8", "8"), [7])

    # OVERFLOW applies to the subcommands after it and produces no reply item.
    check("BITFIELD mixed ordering", c.cmd("BITFIELD", "bfw-mix", "SET", "u8", "0", "250", "OVERFLOW", "SAT", "INCRBY", "u8", "0", "10", "GET", "u8", "0"), [0, 255, 255])

    # An all-GET BITFIELD is read-only and matches BITFIELD_RO; no key is made.
    check("BITFIELD all GET", c.cmd("BITFIELD", "bf", "GET", "u8", "0", "GET", "u8", "8"), [1, 2])
    check("BITFIELD all GET matches BITFIELD_RO", c.cmd("BITFIELD_RO", "bf", "GET", "u8", "0", "GET", "u8", "8"), [1, 2])
    check("BITFIELD all GET on a missing key", c.cmd("BITFIELD", "bfw-missing", "GET", "u8", "0"), [0])
    check("BITFIELD all GET created nothing", c.cmd("EXISTS", "bfw-missing"), 0)
    check("BITFIELD with no subcommands", c.cmd("BITFIELD", "bfw-missing"), [])

    # i64 round-trips a large negative value.
    check("BITFIELD SET i64 min", c.cmd("BITFIELD", "bfw-i64", "SET", "i64", "0", "-9223372036854775808"), [0])
    check("BITFIELD GET i64 min", c.cmd("BITFIELD", "bfw-i64", "GET", "i64", "0"), [-9223372036854775808])

    # Like Redis, a BITFIELD holding any write grows (or creates) the string up
    # front, even when every write then fails its overflow check.
    check("BITFIELD FAIL still creates the key", c.cmd("BITFIELD", "bfw-fresh", "OVERFLOW", "FAIL", "SET", "u8", "0", "300"), [None])
    check("BITFIELD FAIL created an empty byte", c.cmd("GET", "bfw-fresh"), b"\x00")

    # Errors, verbatim from Redis.
    check("BITFIELD bad type", c.cmd("BITFIELD", "bfw-err", "SET", "u64", "0", "1"),
          pred=lambda g: is_err(g, "ERR Invalid bitfield type. Use something like i16 u8. Note that u64 is not supported but i64 is."))
    check("BITFIELD bad offset", c.cmd("BITFIELD", "bfw-err", "SET", "u8", "-1", "1"),
          pred=lambda g: is_err(g, "ERR bit offset is not an integer or out of range"))
    check("BITFIELD bad overflow", c.cmd("BITFIELD", "bfw-err", "OVERFLOW", "BOGUS", "GET", "u8", "0"),
          pred=lambda g: is_err(g, "ERR Invalid OVERFLOW type specified"))
    check("BITFIELD bad value", c.cmd("BITFIELD", "bfw-err", "SET", "u8", "0", "nope"),
          pred=lambda g: is_err(g, "ERR value is not an integer or out of range"))
    check("BITFIELD bad increment", c.cmd("BITFIELD", "bfw-err", "INCRBY", "u8", "0", "1.5"),
          pred=lambda g: is_err(g, "ERR value is not an integer or out of range"))
    check("BITFIELD unknown subcommand", c.cmd("BITFIELD", "bfw-err", "DEL", "u8", "0"),
          pred=lambda g: is_err(g, "ERR syntax error"))
    check("BITFIELD arity", c.cmd("BITFIELD"), pred=lambda g: is_err(g, "ERR wrong number"))
    check("BITFIELD errors write nothing", c.cmd("EXISTS", "bfw-err"), 0)
    check("BITFIELD wrongtype", c.cmd("BITFIELD", "bc-list", "SET", "u8", "0", "1"),
          pred=lambda g: is_err(g, "WRONGTYPE"))
    check("BITFIELD wrongtype read-only", c.cmd("BITFIELD", "bc-list", "GET", "u8", "0"),
          pred=lambda g: is_err(g, "WRONGTYPE"))

    # A write keeps the TTL the key already had, like SETBIT.
    c.cmd("SET", "bfw-ttl", "abc")
    c.cmd("EXPIRE", "bfw-ttl", "100")
    check("BITFIELD SET on a key with a TTL", c.cmd("BITFIELD", "bfw-ttl", "SET", "u8", "0", "65"), [97])
    check("BITFIELD kept the TTL", c.cmd("TTL", "bfw-ttl"), pred=lambda g: isinstance(g, int) and 0 < g <= 100)
    check("BITFIELD wrote the byte", c.cmd("GET", "bfw-ttl"), b"Abc")

    # OBJECT
    check("OBJECT ENCODING string", c.cmd("OBJECT", "ENCODING", "bc"), b"raw")
    check("OBJECT ENCODING list", c.cmd("OBJECT", "ENCODING", "sl"), b"quicklist")
    check("OBJECT ENCODING missing", c.cmd("OBJECT", "ENCODING", "nope"), None)
    check("OBJECT REFCOUNT", c.cmd("OBJECT", "REFCOUNT", "bc"), 1)
    check("OBJECT HELP", c.cmd("OBJECT", "HELP"), pred=lambda g: isinstance(g, list) and len(g) > 3)
    check("OBJECT IDLETIME rejected", c.cmd("OBJECT", "IDLETIME", "bc"), pred=lambda g: is_err(g, "ERR"))

    # MEMORY USAGE
    check("MEMORY USAGE string", c.cmd("MEMORY", "USAGE", "bc"), pred=lambda g: isinstance(g, int) and g > 6)
    check("MEMORY USAGE missing", c.cmd("MEMORY", "USAGE", "nope"), None)

    # SLOWLOG / LATENCY / ACL shims
    check("SLOWLOG GET", c.cmd("SLOWLOG", "GET"), [])
    check("SLOWLOG LEN", c.cmd("SLOWLOG", "LEN"), 0)
    check("SLOWLOG RESET", c.cmd("SLOWLOG", "RESET"), "OK")
    check("LATENCY LATEST", c.cmd("LATENCY", "LATEST"), [])
    check("LATENCY HISTORY", c.cmd("LATENCY", "HISTORY", "command"), [])
    check("LATENCY DOCTOR", c.cmd("LATENCY", "DOCTOR"), pred=lambda g: isinstance(g, bytes) and b"Dave" in g)
    check("ACL WHOAMI", c.cmd("ACL", "WHOAMI"), b"default")
    check("ACL USERS", c.cmd("ACL", "USERS"), [b"default"])
    check("ACL LIST", c.cmd("ACL", "LIST"), pred=lambda g: isinstance(g, list) and g[0].startswith(b"user default on"))
    check("ACL GETUSER default", c.cmd("ACL", "GETUSER", "default"), pred=lambda g: isinstance(g, list) and len(g) == 12)
    check("ACL GETUSER other", c.cmd("ACL", "GETUSER", "nobody"), None)
    check("ACL SETUSER rejected", c.cmd("ACL", "SETUSER", "x"), pred=lambda g: is_err(g, "ERR"))

    # CONFIG GET extras / INFO keyspace
    check("CONFIG GET maxmemory-policy", c.cmd("CONFIG", "GET", "maxmemory-policy"), [b"maxmemory-policy", b"noeviction"])
    check("CONFIG GET *", c.cmd("CONFIG", "GET", "*"), pred=lambda g: isinstance(g, list) and len(g) == 28)
    check("CONFIG GET port", c.cmd("CONFIG", "GET", "port"), [b"port", str(port).encode()])
    info = c.cmd("INFO", "keyspace")
    check("INFO keyspace", info, pred=lambda g: isinstance(g, bytes) and b"# Keyspace" in g and b"db0:keys=" in g)
    info_all = c.cmd("INFO")
    check("INFO default includes keyspace", info_all, pred=lambda g: b"db0:keys=" in g)

    # DUMP / RESTORE for strings, sets, zsets (plus TTL) and regressions for list/hash
    c.cmd("SET", "s1", "hello")
    payload = c.cmd("DUMP", "s1")
    check("DUMP string payload", payload, pred=lambda g: isinstance(g, bytes) and g.startswith(b"MAKO_STRING_DUMP"))
    check("RESTORE string", c.cmd("RESTORE", "s2", "0", payload), "OK")
    check("RESTORE string value", c.cmd("GET", "s2"), b"hello")
    check("RESTORE string ttl", c.cmd("RESTORE", "s3", "5000", payload), "OK")
    check("RESTORE string ttl applied", c.cmd("PTTL", "s3"), pred=lambda g: isinstance(g, int) and 0 < g <= 5000)
    check("RESTORE string ABSTTL", c.cmd("RESTORE", "s4", str(int(time.time() * 1000) + 60000), payload, "ABSTTL"), "OK")
    check("RESTORE string ABSTTL applied", c.cmd("PTTL", "s4"), pred=lambda g: isinstance(g, int) and 50000 < g <= 60000)

    c.cmd("SADD", "st", "a", "b", "c")
    payload = c.cmd("DUMP", "st")
    check("DUMP set payload", payload, pred=lambda g: isinstance(g, bytes) and g.startswith(b"MAKO_SET_DUMP"))
    c.cmd("SET", "st2", "overwritten-string")
    check("RESTORE set replaces", c.cmd("RESTORE", "st2", "0", payload, "REPLACE"), "OK")
    check("RESTORE set members", sorted(c.cmd("SMEMBERS", "st2")), [b"a", b"b", b"c"])
    check("RESTORE set type", c.cmd("TYPE", "st2"), "set")

    c.cmd("ZADD", "z", "1", "a", "2.5", "b")
    payload = c.cmd("DUMP", "z")
    check("DUMP zset payload", payload, pred=lambda g: isinstance(g, bytes) and g.startswith(b"MAKO_ZSET_DUMP"))
    check("RESTORE zset", c.cmd("RESTORE", "z2", "3000", payload), "OK")
    check("RESTORE zset members", c.cmd("ZRANGE", "z2", "0", "-1", "WITHSCORES"), [b"a", b"1", b"b", b"2.5"])
    check("RESTORE zset ttl", c.cmd("PTTL", "z2"), pred=lambda g: isinstance(g, int) and 0 < g <= 3000)

    c.cmd("RPUSH", "l1", "x", "y")
    payload = c.cmd("DUMP", "l1")
    check("RESTORE list (regression)", c.cmd("RESTORE", "l2", "0", payload), "OK")
    check("RESTORE list contents", c.cmd("LRANGE", "l2", "0", "-1"), [b"x", b"y"])
    c.cmd("HSET", "h1", "f", "v")
    payload = c.cmd("DUMP", "h1")
    check("RESTORE hash (regression)", c.cmd("RESTORE", "h2", "0", payload), "OK")
    check("RESTORE hash contents", c.cmd("HGETALL", "h2"), [b"f", b"v"])
    check("RESTORE bad payload", c.cmd("RESTORE", "bad", "0", "garbage"), pred=lambda g: is_err(g, "ERR"))

    # Sharded Pub/Sub on two connections
    sub = Resp(host, port)
    check("SSUBSCRIBE ack", sub.cmd("SSUBSCRIBE", "shard-ch"), [b"ssubscribe", b"shard-ch", 1])
    check("PUBSUB SHARDCHANNELS", c.cmd("PUBSUB", "SHARDCHANNELS"), [b"shard-ch"])
    check("PUBSUB SHARDNUMSUB", c.cmd("PUBSUB", "SHARDNUMSUB", "shard-ch"), [b"shard-ch", 1])
    check("PUBSUB CHANNELS excludes shard-only", c.cmd("PUBSUB", "NUMSUB", "shard-ch"), [b"shard-ch", 1])
    check("SPUBLISH delivers", c.cmd("SPUBLISH", "shard-ch", "hi"), 1)
    check("smessage received", sub.read(), [b"smessage", b"shard-ch", b"hi"])
    check("SUNSUBSCRIBE", sub.cmd("SUNSUBSCRIBE", "shard-ch"), [b"sunsubscribe", b"shard-ch", 0])
    check("PUBSUB SHARDCHANNELS empty", c.cmd("PUBSUB", "SHARDCHANNELS"), [])
    check("subscriber left subscriber mode", sub.cmd("PING"), "PONG")

    # Geo commands (Redis documentation examples: Palermo / Catania / Agrigento)
    def near(got, want, tol):
        return isinstance(got, (int, float)) and abs(got - want) <= tol

    def coord_near(got, lon, lat):
        return (
            isinstance(got, list)
            and len(got) == 2
            and near(float(got[0]), lon, 1e-4)
            and near(float(got[1]), lat, 1e-4)
        )

    check("GEOADD two members", c.cmd("GEOADD", "Sicily", "13.361389", "38.115556", "Palermo",
                                      "15.087269", "37.502669", "Catania"), 2)
    check("GEOADD repeated adds nothing", c.cmd("GEOADD", "Sicily", "13.361389", "38.115556", "Palermo"), 0)
    check("GEOADD CH counts a move", c.cmd("GEOADD", "Sicily", "CH", "13.5", "38.2", "Palermo"), 1)
    check("GEOADD CH restores Palermo", c.cmd("GEOADD", "Sicily", "CH", "13.361389", "38.115556", "Palermo"), 1)
    check("GEOADD NX keeps the old position", c.cmd("GEOADD", "Sicily", "NX", "1", "1", "Palermo"), 0)
    check("GEOADD NX position unchanged", c.cmd("GEOPOS", "Sicily", "Palermo"),
          pred=lambda g: coord_near(g[0], 13.361389, 38.115556))
    check("GEOADD NX adds a new member", c.cmd("GEOADD", "Sicily", "NX", "12.0", "38.0", "Marsala"), 1)
    check("GEOADD XX ignores a new member", c.cmd("GEOADD", "Sicily", "XX", "12.5", "38.5", "Trapani"), 0)
    check("GEOADD XX moves an existing one", c.cmd("GEOADD", "Sicily", "XX", "CH", "12.1", "38.1", "Marsala"), 1)
    check("GEOADD XX did not create Trapani", c.cmd("ZSCORE", "Sicily", "Trapani"), None)
    check("GEOADD cleanup", c.cmd("ZREM", "Sicily", "Marsala"), 1)

    # A geo set is an ordinary sorted set.
    check("TYPE of a geo key", c.cmd("TYPE", "Sicily"), "zset")
    check("ZSCORE Palermo is the 52-bit geohash", c.cmd("ZSCORE", "Sicily", "Palermo"), b"3479099956230698")
    check("ZSCORE Catania is the 52-bit geohash", c.cmd("ZSCORE", "Sicily", "Catania"), b"3479447370796909")

    # GEODIST
    check("GEODIST meters", c.cmd("GEODIST", "Sicily", "Palermo", "Catania"), b"166274.1516")
    check("GEODIST km", c.cmd("GEODIST", "Sicily", "Palermo", "Catania", "km"), b"166.2742")
    check("GEODIST mi", c.cmd("GEODIST", "Sicily", "Palermo", "Catania", "mi"), b"103.3182")
    check("GEODIST ft", c.cmd("GEODIST", "Sicily", "Palermo", "Catania", "ft"),
          pred=lambda g: near(float(g), 166274.1516 / 0.3048, 0.1))
    check("GEODIST missing member", c.cmd("GEODIST", "Sicily", "Palermo", "Foo"), None)
    check("GEODIST missing key", c.cmd("GEODIST", "geo-missing", "a", "b"), None)

    # GEOHASH
    check("GEOHASH strings", c.cmd("GEOHASH", "Sicily", "Palermo", "Catania"),
          [b"sqc8b49rny0", b"sqdtr74hyu0"])
    check("GEOHASH missing member", c.cmd("GEOHASH", "Sicily", "NonExisting"), [None])
    check("GEOHASH missing key", c.cmd("GEOHASH", "geo-missing", "a"), [None])

    # GEOPOS
    pos = c.cmd("GEOPOS", "Sicily", "Palermo", "Catania", "NonExisting")
    check("GEOPOS shape", pos, pred=lambda g: isinstance(g, list) and len(g) == 3 and g[2] is None)
    check("GEOPOS Palermo", pos[0], pred=lambda g: coord_near(g, 13.361389, 38.115556))
    check("GEOPOS Catania", pos[1], pred=lambda g: coord_near(g, 15.087269, 37.502669))
    check("GEOPOS missing key", c.cmd("GEOPOS", "geo-missing", "a"), [None])

    # GEORADIUS (documentation example: centre 15 37, radius 200 km)
    check("GEORADIUS names", sorted(c.cmd("GEORADIUS", "Sicily", "15", "37", "200", "km")),
          [b"Catania", b"Palermo"])
    withdist = c.cmd("GEORADIUS", "Sicily", "15", "37", "200", "km", "WITHDIST")
    dists = {row[0]: float(row[1]) for row in withdist}
    check("GEORADIUS WITHDIST members", sorted(dists), [b"Catania", b"Palermo"])
    check("GEORADIUS WITHDIST Palermo", dists.get(b"Palermo"), pred=lambda g: near(g, 190.4424, 0.01))
    check("GEORADIUS WITHDIST Catania", dists.get(b"Catania"), pred=lambda g: near(g, 56.4413, 0.01))
    withcoord = {row[0]: row[1] for row in c.cmd("GEORADIUS", "Sicily", "15", "37", "200", "km", "WITHCOORD")}
    check("GEORADIUS WITHCOORD Palermo", withcoord.get(b"Palermo"),
          pred=lambda g: coord_near(g, 13.361389, 38.115556))
    check("GEORADIUS WITHCOORD Catania", withcoord.get(b"Catania"),
          pred=lambda g: coord_near(g, 15.087269, 37.502669))
    both = c.cmd("GEORADIUS", "Sicily", "15", "37", "200", "km", "WITHDIST", "WITHCOORD")
    check("GEORADIUS WITHDIST WITHCOORD shape", both,
          pred=lambda g: isinstance(g, list) and len(g) == 2 and all(len(row) == 3 for row in g)
          and all(isinstance(row[2], list) and len(row[2]) == 2 for row in g))
    withhash = {row[0]: row[1] for row in c.cmd("GEORADIUS", "Sicily", "15", "37", "200", "km", "WITHHASH")}
    check("GEORADIUS WITHHASH Palermo", withhash.get(b"Palermo"), 3479099956230698)
    check("GEORADIUS COUNT 1 ASC", c.cmd("GEORADIUS", "Sicily", "15", "37", "200", "km", "COUNT", "1", "ASC"),
          [b"Catania"])
    check("GEORADIUS COUNT 1 picks the closest", c.cmd("GEORADIUS", "Sicily", "15", "37", "200", "km", "COUNT", "1"),
          [b"Catania"])
    check("GEORADIUS DESC", c.cmd("GEORADIUS", "Sicily", "15", "37", "200", "km", "DESC"),
          [b"Palermo", b"Catania"])
    check("GEORADIUS small radius", c.cmd("GEORADIUS", "Sicily", "15", "37", "1", "km"), [])
    check("GEORADIUS missing key", c.cmd("GEORADIUS", "geo-missing", "15", "37", "200", "km"), [])
    check("GEORADIUS_RO", sorted(c.cmd("GEORADIUS_RO", "Sicily", "15", "37", "200", "km")),
          [b"Catania", b"Palermo"])

    # GEOSEARCH
    check("GEOSEARCH BYRADIUS ASC", c.cmd("GEOSEARCH", "Sicily", "FROMLONLAT", "15", "37",
                                          "BYRADIUS", "200", "km", "ASC"),
          [b"Catania", b"Palermo"])
    box = c.cmd("GEOSEARCH", "Sicily", "FROMLONLAT", "15", "37", "BYBOX", "400", "400", "km",
                "ASC", "WITHCOORD", "WITHDIST")
    check("GEOSEARCH BYBOX order", [row[0] for row in box], [b"Catania", b"Palermo"])
    check("GEOSEARCH BYBOX shape", box,
          pred=lambda g: all(len(row) == 3 and isinstance(row[2], list) and len(row[2]) == 2 for row in g))
    check("GEOSEARCH BYBOX Catania distance", float(box[0][1]), pred=lambda g: near(g, 56.4413, 0.01))
    check("GEOSEARCH BYBOX Palermo coord", box[1][2], pred=lambda g: coord_near(g, 13.361389, 38.115556))
    check("GEOSEARCH FROMMEMBER", sorted(c.cmd("GEOSEARCH", "Sicily", "FROMMEMBER", "Palermo",
                                               "BYRADIUS", "200", "km")),
          pred=lambda g: b"Catania" in g and b"Palermo" in g)
    check("GEOSEARCH COUNT 1 ANY", c.cmd("GEOSEARCH", "Sicily", "FROMLONLAT", "15", "37",
                                         "BYRADIUS", "200", "km", "COUNT", "1", "ANY"),
          pred=lambda g: isinstance(g, list) and len(g) == 1)
    check("GEOSEARCH DESC", c.cmd("GEOSEARCH", "Sicily", "FROMLONLAT", "15", "37",
                                  "BYRADIUS", "200", "km", "DESC"),
          [b"Palermo", b"Catania"])
    check("GEOSEARCH missing key", c.cmd("GEOSEARCH", "geo-missing", "FROMLONLAT", "15", "37",
                                         "BYRADIUS", "200", "km"), [])
    check("GEOSEARCH FROMMEMBER missing",
          c.cmd("GEOSEARCH", "Sicily", "FROMMEMBER", "NonExisting", "BYRADIUS", "200", "km"),
          pred=lambda g: is_err(g, "ERR") and "could not decode requested zset member" in str(g))

    # GEORADIUSBYMEMBER (documentation example)
    check("GEOADD Agrigento", c.cmd("GEOADD", "Sicily", "13.583333", "37.316667", "Agrigento"), 1)
    check("GEORADIUSBYMEMBER", sorted(c.cmd("GEORADIUSBYMEMBER", "Sicily", "Agrigento", "100", "km")),
          [b"Agrigento", b"Palermo"])
    check("GEORADIUSBYMEMBER_RO", sorted(c.cmd("GEORADIUSBYMEMBER_RO", "Sicily", "Agrigento", "100", "km")),
          [b"Agrigento", b"Palermo"])
    check("GEORADIUSBYMEMBER missing member", c.cmd("GEORADIUSBYMEMBER", "Sicily", "NonExisting", "100", "km"),
          pred=lambda g: is_err(g, "ERR") and "could not decode requested zset member" in str(g))
    # A key that does not exist searches an empty set; only a missing member of
    # an existing key is an error (Redis tests/unit/geo.tcl).
    check("GEORADIUSBYMEMBER missing key", c.cmd("GEORADIUSBYMEMBER", "geo-missing", "member", "100", "km"), [])
    check("GEORADIUSBYMEMBER_RO missing key", c.cmd("GEORADIUSBYMEMBER_RO", "geo-missing", "member", "1", "km"), [])
    check("GEOSEARCH FROMMEMBER missing key",
          c.cmd("GEOSEARCH", "geo-missing", "FROMMEMBER", "member", "BYBOX", "1", "1", "km"), [])

    # Geo error cases
    check("GEOADD invalid coordinates",
          c.cmd("GEOADD", "Sicily", "200", "100", "bad"),
          pred=lambda g: is_err(g, "ERR invalid longitude,latitude pair 200.000000,100.000000"))
    check("GEODIST bad unit", c.cmd("GEODIST", "Sicily", "Palermo", "Catania", "yards"),
          pred=lambda g: is_err(g, "ERR unsupported unit provided. please use M, KM, FT, MI"))
    check("GEORADIUS bad unit", c.cmd("GEORADIUS", "Sicily", "15", "37", "200", "yards"),
          pred=lambda g: is_err(g, "ERR unsupported unit provided. please use M, KM, FT, MI"))
    check("GEORADIUS negative radius", c.cmd("GEORADIUS", "Sicily", "15", "37", "-5", "km"),
          pred=lambda g: is_err(g, "ERR radius cannot be negative"))
    check("GEORADIUS_RO rejects STORE", c.cmd("GEORADIUS_RO", "Sicily", "15", "37", "200", "km", "STORE", "dst"),
          pred=lambda g: is_err(g, "ERR syntax error"))
    check("GEORADIUSBYMEMBER_RO rejects STOREDIST",
          c.cmd("GEORADIUSBYMEMBER_RO", "Sicily", "Palermo", "200", "km", "STOREDIST", "dst"),
          pred=lambda g: is_err(g, "ERR syntax error"))
    check("GEORADIUS STORE refuses WITHDIST",
          c.cmd("GEORADIUS", "Sicily", "15", "37", "200", "km", "STORE", "dst", "WITHDIST"),
          pred=lambda g: is_err(g, "ERR STORE option in GEORADIUS is not compatible with "
                                  "WITHDIST, WITHHASH and WITHCOORD options"))
    check("GEORADIUS STOREDIST refuses WITHCOORD",
          c.cmd("GEORADIUS", "Sicily", "15", "37", "200", "km", "STOREDIST", "dst", "WITHCOORD"),
          pred=lambda g: is_err(g, "ERR STORE option in GEORADIUS is not compatible with "
                                  "WITHDIST, WITHHASH and WITHCOORD options"))
    check("GEOSEARCHSTORE refuses WITHHASH",
          c.cmd("GEOSEARCHSTORE", "dst", "Sicily", "FROMLONLAT", "15", "37",
                "BYRADIUS", "200", "km", "WITHHASH"),
          pred=lambda g: is_err(g, "ERR syntax error"))
    check("GEOSEARCHSTORE refuses a STORE token",
          c.cmd("GEOSEARCHSTORE", "dst", "Sicily", "FROMLONLAT", "15", "37",
                "BYRADIUS", "200", "km", "STORE", "dst"),
          pred=lambda g: is_err(g, "ERR syntax error"))
    check("GEOSEARCH refuses a bare STOREDIST",
          c.cmd("GEOSEARCH", "Sicily", "FROMLONLAT", "15", "37", "BYRADIUS", "200", "km", "STOREDIST"),
          pred=lambda g: is_err(g, "ERR syntax error"))
    check("GEOSEARCHSTORE ANY needs COUNT",
          c.cmd("GEOSEARCHSTORE", "dst", "Sicily", "FROMLONLAT", "15", "37",
                "BYRADIUS", "200", "km", "ANY"),
          pred=lambda g: is_err(g, "ERR the ANY argument requires COUNT argument"))
    check("GEOSEARCHSTORE needs a shape",
          c.cmd("GEOSEARCHSTORE", "dst", "Sicily", "FROMLONLAT", "15", "37", "ASC"),
          pred=lambda g: is_err(g, "ERR exactly one of BYRADIUS and BYBOX can be specified for GEOSEARCH"))
    check("GEOSEARCHSTORE needs a center",
          c.cmd("GEOSEARCHSTORE", "dst", "Sicily", "BYRADIUS", "200", "km", "ASC"),
          pred=lambda g: is_err(g, "ERR exactly one of FROMMEMBER or FROMLONLAT "
                                  "can be specified for GEOSEARCH"))
    check("GEOSEARCHSTORE did not create the destination on an error",
          c.cmd("EXISTS", "dst"), 0)
    c.cmd("SET", "geo-string", "not-a-geo-set")
    check("GEOADD wrongtype", c.cmd("GEOADD", "geo-string", "13.0", "38.0", "x"),
          pred=lambda g: is_err(g, "WRONGTYPE"))
    check("GEOPOS wrongtype", c.cmd("GEOPOS", "geo-string", "x"), pred=lambda g: is_err(g, "WRONGTYPE"))
    check("GEODIST wrongtype", c.cmd("GEODIST", "geo-string", "a", "b"), pred=lambda g: is_err(g, "WRONGTYPE"))
    check("GEOHASH wrongtype", c.cmd("GEOHASH", "geo-string", "x"), pred=lambda g: is_err(g, "WRONGTYPE"))
    check("GEORADIUS wrongtype", c.cmd("GEORADIUS", "geo-string", "15", "37", "200", "km"),
          pred=lambda g: is_err(g, "WRONGTYPE"))
    check("GEORADIUSBYMEMBER wrongtype", c.cmd("GEORADIUSBYMEMBER", "geo-string", "x", "200", "km"),
          pred=lambda g: is_err(g, "WRONGTYPE"))

    # The geo set stayed a plain sorted set throughout.
    check("TYPE Sicily", c.cmd("TYPE", "Sicily"), "zset")
    check("ZCARD Sicily", c.cmd("ZCARD", "Sicily"), 3)
    check("ZRANGE Sicily by geohash score", c.cmd("ZRANGE", "Sicily", "0", "-1"),
          [b"Agrigento", b"Palermo", b"Catania"])

    check_geo_store(host, port, c, near, coord_near)

    # ----- Hash field expiration (Redis 7.4 HEXPIRE family) -----

    # Documentation example
    c.cmd("DEL", "hfe")
    check("HSET for hfe", c.cmd("HSET", "hfe", "f1", "v1", "f2", "v2", "f3", "v3"), 3)
    check("HEXPIRE sets one field", c.cmd("HEXPIRE", "hfe", "10", "FIELDS", "1", "f1"), [1])
    check("HTTL mixes ttl, no-ttl and missing", c.cmd("HTTL", "hfe", "FIELDS", "3", "f1", "f2", "nofield"),
          pred=lambda g: len(g) == 3 and 8 <= g[0] <= 10 and g[1] == -1 and g[2] == -2)
    check("HPEXPIRE sets milliseconds", c.cmd("HPEXPIRE", "hfe", "1500", "FIELDS", "1", "f2"), [1])
    check("HPTTL in range", c.cmd("HPTTL", "hfe", "FIELDS", "1", "f2"),
          pred=lambda g: len(g) == 1 and 1000 < g[0] <= 1500)
    now_s = int(time.time())
    check("HEXPIRETIME is an absolute unix second", c.cmd("HEXPIRETIME", "hfe", "FIELDS", "1", "f1"),
          pred=lambda g: len(g) == 1 and now_s + 8 <= g[0] <= now_s + 11)
    check("HPEXPIRETIME is an absolute unix millisecond",
          c.cmd("HPEXPIRETIME", "hfe", "FIELDS", "1", "f1"),
          pred=lambda g: len(g) == 1 and (now_s + 8) * 1000 <= g[0] <= (now_s + 11) * 1000)
    check("HEXPIRETIME and HPEXPIRETIME agree",
          (c.cmd("HEXPIRETIME", "hfe", "FIELDS", "1", "f1")[0],
           c.cmd("HPEXPIRETIME", "hfe", "FIELDS", "1", "f1")[0]),
          pred=lambda g: abs(g[0] * 1000 - g[1]) < 1000)
    check("HPERSIST removes and reports missing", c.cmd("HPERSIST", "hfe", "FIELDS", "2", "f1", "nofield"),
          [1, -2])
    check("HTTL after HPERSIST", c.cmd("HTTL", "hfe", "FIELDS", "1", "f1"), [-1])
    check("HPERSIST without expiration", c.cmd("HPERSIST", "hfe", "FIELDS", "1", "f1"), [-1])

    # Conditions
    c.cmd("DEL", "hfc")
    c.cmd("HSET", "hfc", "f1", "v1", "f2", "v2", "f3", "v3")
    check("HEXPIRE NX on a field without a TTL", c.cmd("HEXPIRE", "hfc", "100", "NX", "FIELDS", "1", "f1"), [1])
    check("HEXPIRE NX on a field with a TTL", c.cmd("HEXPIRE", "hfc", "200", "NX", "FIELDS", "1", "f1"), [0])
    check("HEXPIRE XX on a field with a TTL", c.cmd("HEXPIRE", "hfc", "300", "XX", "FIELDS", "1", "f1"), [1])
    check("HEXPIRE XX on a field without one", c.cmd("HEXPIRE", "hfc", "300", "XX", "FIELDS", "1", "f2"), [0])
    check("HEXPIRE GT raises only a larger time",
          c.cmd("HEXPIRE", "hfc", "400", "GT", "FIELDS", "2", "f1", "f2"), [1, 0])
    check("HEXPIRE GT refuses a smaller time", c.cmd("HEXPIRE", "hfc", "100", "GT", "FIELDS", "1", "f1"), [0])
    check("HEXPIRE LT lowers and sets a field with none",
          c.cmd("HEXPIRE", "hfc", "200", "LT", "FIELDS", "2", "f1", "f2"), [1, 1])
    check("HEXPIRE LT refuses a larger time", c.cmd("HEXPIRE", "hfc", "900", "LT", "FIELDS", "1", "f1"), [0])
    check("HEXPIRE mixes a condition with a missing field",
          c.cmd("HEXPIRE", "hfc", "50", "LT", "FIELDS", "2", "f1", "nofield"), [1, -2])

    # Time in the past deletes the field outright
    check("HEXPIRE 0 deletes the field", c.cmd("HEXPIRE", "hfe", "0", "FIELDS", "1", "f3"), [2])
    check("HEXISTS after HEXPIRE 0", c.cmd("HEXISTS", "hfe", "f3"), 0)
    check("HEXPIREAT in the past deletes the field",
          c.cmd("HEXPIREAT", "hfe", "1", "FIELDS", "1", "f2"), [2])
    check("HGET after a past HEXPIREAT", c.cmd("HGET", "hfe", "f2"), None)
    check("HPEXPIREAT in the past deletes the field",
          c.cmd("HPEXPIREAT", "hfe", "1000", "FIELDS", "1", "f1"), [2])
    check("the hash is gone once its last field went", c.cmd("EXISTS", "hfe"), 0)

    # Lazy expiry: an expired field is absent everywhere
    c.cmd("DEL", "hlazy")
    c.cmd("HSET", "hlazy", "f1", "v1", "f2", "v2", "f3", "v3")
    check("HPEXPIRE for the lazy check", c.cmd("HPEXPIRE", "hlazy", "150", "FIELDS", "1", "f1"), [1])
    time.sleep(0.3)
    check("HGET of an expired field", c.cmd("HGET", "hlazy", "f1"), None)
    check("HEXISTS of an expired field", c.cmd("HEXISTS", "hlazy", "f1"), 0)
    check("HSTRLEN of an expired field", c.cmd("HSTRLEN", "hlazy", "f1"), 0)
    check("HMGET skips an expired field", c.cmd("HMGET", "hlazy", "f1", "f2"), [None, b"v2"])
    check("HGETALL omits an expired field", sorted(c.cmd("HGETALL", "hlazy")),
          sorted([b"f2", b"v2", b"f3", b"v3"]))
    check("HLEN excludes an expired field", c.cmd("HLEN", "hlazy"), 2)
    check("HKEYS omits an expired field", sorted(c.cmd("HKEYS", "hlazy")), [b"f2", b"f3"])
    check("HVALS omits an expired field", sorted(c.cmd("HVALS", "hlazy")), [b"v2", b"v3"])
    check("HSCAN omits an expired field", sorted(c.cmd("HSCAN", "hlazy", "0")[1]),
          sorted([b"f2", b"v2", b"f3", b"v3"]))
    check("HTTL of an expired field reports no field", c.cmd("HTTL", "hlazy", "FIELDS", "1", "f1"), [-2])
    randomized = set()
    for _ in range(20):
        randomized.add(c.cmd("HRANDFIELD", "hlazy"))
    check("HRANDFIELD never returns an expired field", randomized, pred=lambda g: g == {b"f2", b"f3"})
    check("HRANDFIELD with a count omits it too", sorted(c.cmd("HRANDFIELD", "hlazy", "10")),
          [b"f2", b"f3"])

    # The key disappears when its last field expires
    c.cmd("DEL", "hsolo")
    c.cmd("HSET", "hsolo", "only", "v")
    check("HPEXPIRE the only field", c.cmd("HPEXPIRE", "hsolo", "100", "FIELDS", "1", "only"), [1])
    time.sleep(0.3)
    check("EXISTS after the last field expired", c.cmd("EXISTS", "hsolo"), 0)
    check("TYPE after the last field expired", c.cmd("TYPE", "hsolo"), "none")
    check("HGETALL after the last field expired", c.cmd("HGETALL", "hsolo"), [])
    check("HLEN after the last field expired", c.cmd("HLEN", "hsolo"), 0)

    # Writes and the field TTL
    c.cmd("DEL", "hw")
    c.cmd("HSET", "hw", "f1", "v1", "f2", "10")
    c.cmd("HEXPIRE", "hw", "100", "FIELDS", "2", "f1", "f2")
    check("HSET discards the field TTL", c.cmd("HSET", "hw", "f1", "v2"), 0)
    check("HTTL after the overwrite", c.cmd("HTTL", "hw", "FIELDS", "1", "f1"), [-1])
    check("HINCRBY preserves the field TTL", c.cmd("HINCRBY", "hw", "f2", "5"), 15)
    check("HTTL after HINCRBY", c.cmd("HTTL", "hw", "FIELDS", "1", "f2"),
          pred=lambda g: len(g) == 1 and 90 <= g[0] <= 100)
    check("HINCRBYFLOAT preserves the field TTL", c.cmd("HINCRBYFLOAT", "hw", "f2", "0.5"), b"15.5")
    check("HTTL after HINCRBYFLOAT", c.cmd("HTTL", "hw", "FIELDS", "1", "f2"),
          pred=lambda g: len(g) == 1 and 90 <= g[0] <= 100)
    check("HDEL removes the field TTL", c.cmd("HDEL", "hw", "f2"), 1)
    check("HSET recreates the field without a TTL", c.cmd("HSET", "hw", "f2", "v2"), 1)
    check("HTTL of the recreated field", c.cmd("HTTL", "hw", "FIELDS", "1", "f2"), [-1])
    c.cmd("HEXPIRE", "hw", "100", "FIELDS", "1", "f2")
    c.cmd("DEL", "hw")
    c.cmd("HSET", "hw", "f2", "v2")
    check("DEL drops every field TTL", c.cmd("HTTL", "hw", "FIELDS", "1", "f2"), [-1])
    check("HSETNX on a new field has no TTL",
          (c.cmd("HSETNX", "hw", "f9", "v9"), c.cmd("HTTL", "hw", "FIELDS", "1", "f9")), (1, [-1]))
    c.cmd("HEXPIRE", "hw", "100", "FIELDS", "1", "f9")
    check("HSETNX leaves an existing field's TTL alone",
          (c.cmd("HSETNX", "hw", "f9", "other"), c.cmd("HTTL", "hw", "FIELDS", "1", "f9")),
          pred=lambda g: g[0] == 0 and 90 <= g[1][0] <= 100)

    # A write over an expired field replaces it in place: the field counts as
    # new, and the hash stays readable afterwards.
    c.cmd("DEL", "hover")
    c.cmd("HSET", "hover", "f1", "v1", "f2", "keep")
    c.cmd("HPEXPIRE", "hover", "10", "FIELDS", "1", "f1")
    time.sleep(0.05)
    check("HSET over an expired field counts it as new", c.cmd("HSET", "hover", "f1", "v2"), 1)
    check("HGET after writing over an expired field", c.cmd("HGET", "hover", "f1"), b"v2")
    check("HTTL after writing over an expired field", c.cmd("HTTL", "hover", "FIELDS", "1", "f1"), [-1])
    check("HLEN after writing over an expired field", c.cmd("HLEN", "hover"), 2)
    check("HGETALL after writing over an expired field", sorted(c.cmd("HGETALL", "hover")),
          sorted([b"f1", b"v2", b"f2", b"keep"]))
    check("DEL after writing over an expired field", c.cmd("DEL", "hover"), 1)
    c.cmd("DEL", "hover2")
    c.cmd("HSET", "hover2", "f1", "10")
    c.cmd("HPEXPIRE", "hover2", "10", "FIELDS", "1", "f1")
    time.sleep(0.05)
    check("HINCRBY over an expired field restarts from zero", c.cmd("HINCRBY", "hover2", "f1", "1"), 1)
    check("HTTL after HINCRBY over an expired field", c.cmd("HTTL", "hover2", "FIELDS", "1", "f1"), [-1])
    check("EXISTS after HINCRBY over an expired field", c.cmd("EXISTS", "hover2"), 1)
    check("DEL after HINCRBY over an expired field", c.cmd("DEL", "hover2"), 1)
    c.cmd("DEL", "hover3")
    c.cmd("HSET", "hover3", "f1", "10")
    c.cmd("HPEXPIRE", "hover3", "10", "FIELDS", "1", "f1")
    time.sleep(0.05)
    check("HINCRBYFLOAT over an expired field restarts from zero",
          c.cmd("HINCRBYFLOAT", "hover3", "f1", "1.5"), b"1.5")
    check("HSETNX writes over an expired field",
          (c.cmd("HPEXPIRE", "hover3", "10", "FIELDS", "1", "f1"), time.sleep(0.05),
           c.cmd("HSETNX", "hover3", "f1", "fresh"), c.cmd("HGET", "hover3", "f1")),
          pred=lambda g: g[0] == [1] and g[2] == 1 and g[3] == b"fresh")
    check("HLEN after HSETNX over an expired field", c.cmd("HLEN", "hover3"), 1)
    check("DEL after HSETNX over an expired field", c.cmd("DEL", "hover3"), 1)

    # A key-level TTL and field TTLs coexist
    c.cmd("DEL", "hboth")
    c.cmd("HSET", "hboth", "f1", "v1", "f2", "v2")
    check("EXPIRE on the hash", c.cmd("EXPIRE", "hboth", "500"), 1)
    check("HEXPIRE on one of its fields", c.cmd("HEXPIRE", "hboth", "100", "FIELDS", "1", "f1"), [1])
    check("the key TTL is untouched", c.cmd("TTL", "hboth"), pred=lambda g: 490 <= g <= 500)
    check("the field TTL is untouched", c.cmd("HTTL", "hboth", "FIELDS", "2", "f1", "f2"),
          pred=lambda g: 90 <= g[0] <= 100 and g[1] == -1)
    check("PERSIST leaves the field TTLs alone",
          (c.cmd("PERSIST", "hboth"), c.cmd("TTL", "hboth")), (1, -1))
    check("the field TTL survives PERSIST", c.cmd("HTTL", "hboth", "FIELDS", "1", "f1"),
          pred=lambda g: 90 <= g[0] <= 100)

    # Missing key, wrong type and argument errors
    check("HEXPIRE on a missing key", c.cmd("HEXPIRE", "hfe-missing", "10", "FIELDS", "2", "a", "b"),
          [-2, -2])
    check("HTTL on a missing key", c.cmd("HTTL", "hfe-missing", "FIELDS", "1", "a"), [-2])
    check("HPTTL on a missing key", c.cmd("HPTTL", "hfe-missing", "FIELDS", "1", "a"), [-2])
    check("HEXPIRETIME on a missing key", c.cmd("HEXPIRETIME", "hfe-missing", "FIELDS", "1", "a"), [-2])
    check("HPEXPIRETIME on a missing key", c.cmd("HPEXPIRETIME", "hfe-missing", "FIELDS", "1", "a"), [-2])
    check("HPERSIST on a missing key", c.cmd("HPERSIST", "hfe-missing", "FIELDS", "1", "a"), [-2])
    c.cmd("SET", "hfe-string", "plain")
    for name, args in (("HEXPIRE", ("10", "FIELDS", "1", "f")),
                       ("HPEXPIRE", ("10", "FIELDS", "1", "f")),
                       ("HEXPIREAT", (str(int(time.time()) + 100), "FIELDS", "1", "f")),
                       ("HPEXPIREAT", (str(int(time.time() * 1000) + 100000), "FIELDS", "1", "f")),
                       ("HTTL", ("FIELDS", "1", "f")),
                       ("HPTTL", ("FIELDS", "1", "f")),
                       ("HEXPIRETIME", ("FIELDS", "1", "f")),
                       ("HPEXPIRETIME", ("FIELDS", "1", "f")),
                       ("HPERSIST", ("FIELDS", "1", "f"))):
        check("%s wrongtype" % name, c.cmd(name, "hfe-string", *args),
              pred=lambda g: is_err(g, "WRONGTYPE"))
    check("HEXPIRE numfields 0", c.cmd("HEXPIRE", "hfc", "10", "FIELDS", "0", "f1"),
          pred=lambda g: is_err(g, "ERR Parameter `numFields` should be greater than 0"))
    check("HEXPIRE numfields too large", c.cmd("HEXPIRE", "hfc", "10", "FIELDS", "4", "f1", "f2", "f3"),
          pred=lambda g: is_err(g, "ERR The `numfields` parameter must match the number of arguments"))
    check("HEXPIRE numfields too small", c.cmd("HEXPIRE", "hfc", "10", "FIELDS", "2", "f1", "f2", "f3"),
          pred=lambda g: is_err(g, "ERR The `numfields` parameter must match the number of arguments"))
    check("HTTL numfields mismatch", c.cmd("HTTL", "hfc", "FIELDS", "3", "f1", "f2"),
          pred=lambda g: is_err(g, "ERR The `numfields` parameter must match the number of arguments"))
    check("HPERSIST numfields mismatch", c.cmd("HPERSIST", "hfc", "FIELDS", "4", "f1", "f2", "f3"),
          pred=lambda g: is_err(g, "ERR The `numfields` parameter must match the number of arguments"))
    check("HEXPIRE without FIELDS", c.cmd("HEXPIRE", "hfc", "10", "NOTFIELDS", "1", "f1"),
          pred=lambda g: is_err(g, "ERR Mandatory argument FIELDS is missing or not at the right position"))
    check("HTTL without FIELDS", c.cmd("HTTL", "hfc", "COUNT", "1", "f1"),
          pred=lambda g: is_err(g, "ERR Mandatory argument FIELDS is missing or not at the right position"))
    check("HEXPIRE negative time", c.cmd("HEXPIRE", "hfc", "-1", "FIELDS", "1", "f1"),
          pred=lambda g: is_err(g, "ERR invalid expire time, must be >= 0"))
    check("HEXPIRE beyond the expire-time ceiling",
          c.cmd("HEXPIRE", "hfc", str((1 << 48) // 1000), "FIELDS", "1", "f1"),
          pred=lambda g: is_err(g, "ERR invalid expire time in 'hexpire' command"))
    check("HPEXPIRE beyond the expire-time ceiling",
          c.cmd("HPEXPIRE", "hfc", str(1 << 48), "FIELDS", "1", "f1"),
          pred=lambda g: is_err(g, "ERR invalid expire time in 'hpexpire' command"))
    check("HEXPIREAT beyond the expire-time ceiling",
          c.cmd("HEXPIREAT", "hfc", str((1 << 48) // 1000 + int(time.time()) + 100), "FIELDS", "1", "f1"),
          pred=lambda g: is_err(g, "ERR invalid expire time in 'hexpireat' command"))
    check("HPEXPIREAT beyond the expire-time ceiling",
          c.cmd("HPEXPIREAT", "hfc", str((1 << 48) + int(time.time() * 1000) + 100), "FIELDS", "1", "f1"),
          pred=lambda g: is_err(g, "ERR invalid expire time in 'hpexpireat' command"))
    check("HPEXPIRE just below the ceiling",
          c.cmd("HPEXPIRE", "hfc", str((1 << 46) - int(time.time() * 1000) - 1000), "FIELDS", "1", "f1"),
          [1])
    check("HPEXPIRE just above the ceiling",
          c.cmd("HPEXPIRE", "hfc", str((1 << 46) - int(time.time() * 1000) + 100000), "FIELDS", "1", "f1"),
          pred=lambda g: is_err(g, "ERR invalid expire time in 'hpexpire' command"))
    check("HEXPIRE NX with XX", c.cmd("HEXPIRE", "hfc", "10", "NX", "XX", "FIELDS", "1", "f1"),
          pred=lambda g: is_err(g, "ERR NX and XX, GT or LT options at the same time are not compatible"))
    check("HEXPIRE GT with LT", c.cmd("HEXPIRE", "hfc", "10", "GT", "LT", "FIELDS", "1", "f1"),
          pred=lambda g: is_err(g, "ERR GT and LT options at the same time are not compatible"))
    check("HEXPIRE arity", c.cmd("HEXPIRE", "hfc", "10", "FIELDS", "1"),
          pred=lambda g: is_err(g, "ERR wrong number of arguments for 'hexpire' command"))
    check("HPERSIST arity", c.cmd("HPERSIST", "hfc"),
          pred=lambda g: is_err(g, "ERR wrong number of arguments for 'hpersist' command"))

    # Repeated fields are processed in order
    c.cmd("DEL", "hrep")
    c.cmd("HSET", "hrep", "f", "v")
    check("HEXPIRE repeats a field in order",
          c.cmd("HEXPIRE", "hrep", "100", "NX", "FIELDS", "2", "f", "f"), [1, 0])
    check("HTTL repeats a field", c.cmd("HTTL", "hrep", "FIELDS", "2", "f", "f"),
          pred=lambda g: len(g) == 2 and g[0] == g[1] and 90 <= g[0] <= 100)

    # RENAME and COPY move the whole object, so field TTLs travel with it
    c.cmd("DEL", "hcopy", "hdst")
    c.cmd("HSET", "hcopy", "f", "v", "g", "v")
    c.cmd("HEXPIRE", "hcopy", "100", "FIELDS", "1", "f")
    c.cmd("COPY", "hcopy", "hdst")
    check("COPY carries the field TTL to the destination",
          c.cmd("HTTL", "hdst", "FIELDS", "2", "f", "g"),
          pred=lambda g: 90 <= g[0] <= 100 and g[1] == -1)
    check("COPY keeps the source field TTL", c.cmd("HTTL", "hcopy", "FIELDS", "1", "f"),
          pred=lambda g: 90 <= g[0] <= 100)
    c.cmd("DEL", "hren", "hren2")
    c.cmd("HSET", "hren", "f", "v", "g", "v")
    c.cmd("HEXPIRE", "hren", "100", "FIELDS", "1", "f")
    c.cmd("RENAME", "hren", "hren2")
    check("RENAME carries the field TTL", c.cmd("HTTL", "hren2", "FIELDS", "2", "f", "g"),
          pred=lambda g: 90 <= g[0] <= 100 and g[1] == -1)
    check("RENAME leaves nothing behind", c.cmd("EXISTS", "hren"), 0)
    c.cmd("DEL", "hdump")
    c.cmd("HSET", "hdump", "f", "v")
    c.cmd("HEXPIRE", "hdump", "100", "FIELDS", "1", "f")
    payload = c.cmd("DUMP", "hdump")
    c.cmd("DEL", "hdump")
    c.cmd("RESTORE", "hdump", "0", payload)
    check("RESTORE brings back a hash without field TTLs",
          c.cmd("HTTL", "hdump", "FIELDS", "1", "f"), [-1])
    c.cmd("DEL", "hsortsrc", "hsortdst")
    c.cmd("RPUSH", "hsortsrc", "2", "1")
    c.cmd("HSET", "hsortdst", "f", "v")
    c.cmd("HEXPIRE", "hsortdst", "100", "FIELDS", "1", "f")
    check("SORT STORE replaces the hash", c.cmd("SORT", "hsortsrc", "STORE", "hsortdst"), 2)
    check("SORT STORE dropped the field TTLs", c.cmd("TYPE", "hsortdst"), "list")
    c.cmd("DEL", "hsortdst")
    c.cmd("HSET", "hsortdst", "f", "v")
    check("the recreated hash has no leftover field TTL",
          c.cmd("HTTL", "hsortdst", "FIELDS", "1", "f"), [-1])
    c.cmd("DEL", "hflush")
    c.cmd("HSET", "hflush", "f", "v")
    c.cmd("HEXPIRE", "hflush", "100", "FIELDS", "1", "f")
    c.cmd("FLUSHALL")
    c.cmd("HSET", "hflush", "f", "v")
    check("FLUSHALL dropped the field TTLs", c.cmd("HTTL", "hflush", "FIELDS", "1", "f"), [-1])
    # ----- Deleting and rewriting one storage key inside one transaction -----
    # A key removed and written again inside a single Mako transaction used to
    # be stranded: the storage layer marked the record invalid and every later
    # access aborted, so the client saw "ERR backend" forever. Each case below
    # reads the key back in a later transaction and then writes and reads it a
    # second time, which is what a stranded key can no longer survive.

    def multi(*commands):
        """Run commands in one MULTI/EXEC and return the EXEC reply."""
        c.cmd("MULTI")
        for command in commands:
            c.cmd(*command)
        return c.cmd("EXEC")

    # MULTI: delete and write the same key, one case per namespace
    c.cmd("DEL", "rw1")
    c.cmd("SET", "rw1", "v0")
    check("MULTI DEL+SET replies", multi(["DEL", "rw1"], ["SET", "rw1", "v1"]), [1, "OK"])
    check("GET after MULTI DEL+SET", c.cmd("GET", "rw1"), b"v1")
    check("EXISTS after MULTI DEL+SET", c.cmd("EXISTS", "rw1"), 1)
    check("TYPE after MULTI DEL+SET", c.cmd("TYPE", "rw1"), "string")
    check("SET again after MULTI DEL+SET", c.cmd("SET", "rw1", "v2"), "OK")
    check("GET the second write", c.cmd("GET", "rw1"), b"v2")
    check("DEL after MULTI DEL+SET", c.cmd("DEL", "rw1"), 1)
    check("GET after the final DEL", c.cmd("GET", "rw1"), None)
    check("SET after the final DEL", c.cmd("SET", "rw1", "v3"), "OK")
    check("GET after the final SET", c.cmd("GET", "rw1"), b"v3")

    c.cmd("DEL", "rw2")
    c.cmd("HSET", "rw2", "f", "v0")
    check("MULTI HDEL+HSET replies", multi(["HDEL", "rw2", "f"], ["HSET", "rw2", "f", "v1"]), [1, 1])
    check("HGET after MULTI HDEL+HSET", c.cmd("HGET", "rw2", "f"), b"v1")
    check("HLEN after MULTI HDEL+HSET", c.cmd("HLEN", "rw2"), 1)
    check("HGETALL after MULTI HDEL+HSET", c.cmd("HGETALL", "rw2"), [b"f", b"v1"])
    check("HSET again after MULTI HDEL+HSET", c.cmd("HSET", "rw2", "f", "v2"), 0)
    check("HGET the second write", c.cmd("HGET", "rw2", "f"), b"v2")

    c.cmd("DEL", "rw3")
    c.cmd("SADD", "rw3", "m")
    check("MULTI SREM+SADD replies", multi(["SREM", "rw3", "m"], ["SADD", "rw3", "m"]), [1, 1])
    check("SISMEMBER after MULTI SREM+SADD", c.cmd("SISMEMBER", "rw3", "m"), 1)
    check("SCARD after MULTI SREM+SADD", c.cmd("SCARD", "rw3"), 1)
    check("SADD again after MULTI SREM+SADD", c.cmd("SADD", "rw3", "m2"), 1)
    check("SMEMBERS after the second write", sorted(c.cmd("SMEMBERS", "rw3")), [b"m", b"m2"])

    c.cmd("DEL", "rw4")
    c.cmd("ZADD", "rw4", "1", "m")
    check("MULTI ZREM+ZADD replies", multi(["ZREM", "rw4", "m"], ["ZADD", "rw4", "2", "m"]), [1, 1])
    check("ZSCORE after MULTI ZREM+ZADD", c.cmd("ZSCORE", "rw4", "m"), b"2")
    check("ZCARD after MULTI ZREM+ZADD", c.cmd("ZCARD", "rw4"), 1)
    check("ZADD again after MULTI ZREM+ZADD", c.cmd("ZADD", "rw4", "3", "m"), 0)
    check("ZSCORE the second write", c.cmd("ZSCORE", "rw4", "m"), b"3")

    c.cmd("DEL", "rw5")
    c.cmd("RPUSH", "rw5", "a")
    check("MULTI LPOP+RPUSH replies", multi(["LPOP", "rw5"], ["RPUSH", "rw5", "a"]), [b"a", 1])
    check("LRANGE after MULTI LPOP+RPUSH", c.cmd("LRANGE", "rw5", "0", "-1"), [b"a"])
    check("RPUSH again after MULTI LPOP+RPUSH", c.cmd("RPUSH", "rw5", "b"), 2)
    check("LRANGE after the second write", c.cmd("LRANGE", "rw5", "0", "-1"), [b"a", b"b"])

    # The key survives many more transactions, including another delete round
    c.cmd("DEL", "rw6")
    c.cmd("SET", "rw6", "v0")
    multi(["DEL", "rw6"], ["SET", "rw6", "v1"])
    check("APPEND on a key deleted and rewritten in one MULTI", c.cmd("APPEND", "rw6", "x"), 3)
    check("GET after APPEND", c.cmd("GET", "rw6"), b"v1x")
    check("SETEX over it", c.cmd("SETEX", "rw6", "100", "9"), "OK")
    check("TTL after SETEX", c.cmd("TTL", "rw6"), pred=lambda g: 90 <= g <= 100)
    check("INCR over it", c.cmd("INCR", "rw6"), 10)
    check("PERSIST over it", c.cmd("PERSIST", "rw6"), 1)
    check("GET after INCR", c.cmd("GET", "rw6"), b"10")
    check("second MULTI DEL+SET round", multi(["DEL", "rw6"], ["SET", "rw6", "v2"]), [1, "OK"])
    check("GET after the second round", c.cmd("GET", "rw6"), b"v2")
    check("a MULTI SET+DEL still deletes", multi(["SET", "rw6", "v3"], ["DEL", "rw6"]), ["OK", 1])
    check("GET after MULTI SET+DEL", c.cmd("GET", "rw6"), None)
    # On a key that already exists, write-delete-write ends with the last
    # write. (On a key the same transaction created, the storage layer keeps
    # the value the record was created with -- see known_divergences.txt.)
    c.cmd("SET", "rw6", "seed")
    check("MULTI SET+DEL+SET keeps the last write",
          multi(["SET", "rw6", "a"], ["DEL", "rw6"], ["SET", "rw6", "b"]), ["OK", 1, "OK"])
    check("GET after MULTI SET+DEL+SET", c.cmd("GET", "rw6"), b"b")
    check("MULTI DEL+SET+DEL+SET keeps the last write",
          multi(["DEL", "rw6"], ["SET", "rw6", "c"], ["DEL", "rw6"], ["SET", "rw6", "d"]),
          [1, "OK", 1, "OK"])
    check("GET after MULTI DEL+SET+DEL+SET", c.cmd("GET", "rw6"), b"d")
    check("MULTI DEL+SET then DEL leaves nothing",
          multi(["DEL", "rw6"], ["SET", "rw6", "e"], ["DEL", "rw6"]), [1, "OK", 1])
    check("GET after MULTI DEL+SET+DEL", c.cmd("GET", "rw6"), None)
    check("EXISTS after MULTI DEL+SET+DEL", c.cmd("EXISTS", "rw6"), 0)
    c.cmd("SET", "rw6", "back")
    check("MULTI FLUSHALL+SET keeps the write", multi(["FLUSHALL"], ["SET", "rw6", "c"]),
          pred=lambda g: g[1] == "OK")
    check("GET after MULTI FLUSHALL+SET", c.cmd("GET", "rw6"), b"c")
    check("DBSIZE after MULTI FLUSHALL+SET", c.cmd("DBSIZE"), 1)

    # COPY dst REPLACE, every type, with elements that collide with the
    # destination's own storage keys
    c.cmd("DEL", "cpa", "cpb")
    c.cmd("SET", "cpa", "A")
    c.cmd("SET", "cpb", "B")
    check("COPY string REPLACE", c.cmd("COPY", "cpa", "cpb", "REPLACE"), 1)
    check("GET the copied string", c.cmd("GET", "cpb"), b"A")
    check("SET over the copied string", c.cmd("SET", "cpb", "C"), "OK")
    check("GET after writing over the copy", c.cmd("GET", "cpb"), b"C")

    c.cmd("DEL", "cha", "chb")
    c.cmd("HSET", "cha", "f", "A")
    c.cmd("HSET", "chb", "f", "B")
    check("COPY hash REPLACE onto the same field", c.cmd("COPY", "cha", "chb", "REPLACE"), 1)
    check("HGET the copied field", c.cmd("HGET", "chb", "f"), b"A")
    check("HGETALL the copied hash", c.cmd("HGETALL", "chb"), [b"f", b"A"])
    check("HLEN the copied hash", c.cmd("HLEN", "chb"), 1)
    check("HSET over the copied field", c.cmd("HSET", "chb", "f", "C"), 0)
    check("HGET after writing over the copy", c.cmd("HGET", "chb", "f"), b"C")

    c.cmd("DEL", "csa", "csb")
    c.cmd("SADD", "csa", "m")
    c.cmd("SADD", "csb", "m")
    check("COPY set REPLACE onto the same member", c.cmd("COPY", "csa", "csb", "REPLACE"), 1)
    check("SMEMBERS the copied set", c.cmd("SMEMBERS", "csb"), [b"m"])
    check("SISMEMBER the copied member", c.cmd("SISMEMBER", "csb", "m"), 1)
    check("SADD over the copied set", c.cmd("SADD", "csb", "m2"), 1)
    check("SMEMBERS after writing over the copy", sorted(c.cmd("SMEMBERS", "csb")), [b"m", b"m2"])

    c.cmd("DEL", "cza", "czb")
    c.cmd("ZADD", "cza", "1", "m")
    c.cmd("ZADD", "czb", "2", "m")
    check("COPY zset REPLACE onto the same member", c.cmd("COPY", "cza", "czb", "REPLACE"), 1)
    check("ZSCORE the copied member", c.cmd("ZSCORE", "czb", "m"), b"1")
    check("ZRANGE the copied zset", c.cmd("ZRANGE", "czb", "0", "-1", "WITHSCORES"), [b"m", b"1"])
    check("ZADD over the copied zset", c.cmd("ZADD", "czb", "5", "m"), 0)
    check("ZSCORE after writing over the copy", c.cmd("ZSCORE", "czb", "m"), b"5")

    c.cmd("DEL", "cla", "clb")
    c.cmd("RPUSH", "cla", "A")
    c.cmd("RPUSH", "clb", "B")
    check("COPY list REPLACE onto the same index", c.cmd("COPY", "cla", "clb", "REPLACE"), 1)
    check("LRANGE the copied list", c.cmd("LRANGE", "clb", "0", "-1"), [b"A"])
    check("RPUSH over the copied list", c.cmd("RPUSH", "clb", "Z"), 2)
    check("LRANGE after writing over the copy", c.cmd("LRANGE", "clb", "0", "-1"), [b"A", b"Z"])

    # RENAME onto an existing key of the same type
    c.cmd("DEL", "rna", "rnb")
    c.cmd("SET", "rna", "A")
    c.cmd("SET", "rnb", "B")
    check("RENAME string onto a string", c.cmd("RENAME", "rna", "rnb"), "OK")
    check("GET the renamed string", c.cmd("GET", "rnb"), b"A")
    check("SET over the renamed string", c.cmd("SET", "rnb", "C"), "OK")
    check("GET after writing over the rename", c.cmd("GET", "rnb"), b"C")

    c.cmd("DEL", "rha", "rhb")
    c.cmd("HSET", "rha", "f", "A")
    c.cmd("HSET", "rhb", "f", "B")
    check("RENAME hash onto a hash with the same field", c.cmd("RENAME", "rha", "rhb"), "OK")
    check("HGET the renamed field", c.cmd("HGET", "rhb", "f"), b"A")
    check("HLEN the renamed hash", c.cmd("HLEN", "rhb"), 1)
    check("HSET over the renamed field", c.cmd("HSET", "rhb", "f", "C"), 0)
    check("HGET after writing over the rename", c.cmd("HGET", "rhb", "f"), b"C")

    c.cmd("DEL", "rsa", "rsb")
    c.cmd("SADD", "rsa", "m")
    c.cmd("SADD", "rsb", "m")
    check("RENAME set onto a set with the same member", c.cmd("RENAME", "rsa", "rsb"), "OK")
    check("SMEMBERS the renamed set", c.cmd("SMEMBERS", "rsb"), [b"m"])
    check("SADD over the renamed set", c.cmd("SADD", "rsb", "m2"), 1)
    check("SCARD after writing over the rename", c.cmd("SCARD", "rsb"), 2)

    c.cmd("DEL", "rza", "rzb")
    c.cmd("ZADD", "rza", "1", "m")
    c.cmd("ZADD", "rzb", "2", "m")
    check("RENAME zset onto a zset with the same member", c.cmd("RENAME", "rza", "rzb"), "OK")
    check("ZSCORE the renamed member", c.cmd("ZSCORE", "rzb", "m"), b"1")
    check("ZADD over the renamed zset", c.cmd("ZADD", "rzb", "9", "m"), 0)
    check("ZSCORE after writing over the rename", c.cmd("ZSCORE", "rzb", "m"), b"9")

    # A destination record that has to grow in place makes the storage layer
    # abort the transaction once; the command has to retry it rather than
    # report ERR backend.
    c.cmd("DEL", "rgs", "rgd")
    c.cmd("HSET", "rgs", "f", "a much longer field value than the destination has")
    c.cmd("HSET", "rgd", "f", "a")
    check("RENAME hash onto a hash whose value grows", c.cmd("RENAME", "rgs", "rgd"), "OK")
    check("HGET the grown field", c.cmd("HGET", "rgd", "f"),
          b"a much longer field value than the destination has")
    check("HSET over the grown field", c.cmd("HSET", "rgd", "f", "short"), 0)
    check("HGET after writing over the grown field", c.cmd("HGET", "rgd", "f"), b"short")
    c.cmd("DEL", "rgs2", "rgd2")
    c.cmd("SET", "rgs2", "a much longer string value than the destination has")
    c.cmd("SET", "rgd2", "a")
    check("RENAME string onto a string whose value grows", c.cmd("RENAME", "rgs2", "rgd2"), "OK")
    check("GET the grown string", c.cmd("GET", "rgd2"),
          b"a much longer string value than the destination has")

    # Commands whose destination is their own source
    c.cmd("DEL", "srt")
    c.cmd("RPUSH", "srt", "2", "1", "3")
    check("SORT l STORE l", c.cmd("SORT", "srt", "STORE", "srt"), 3)
    check("LRANGE after SORT STORE onto itself", c.cmd("LRANGE", "srt", "0", "-1"),
          [b"1", b"2", b"3"])
    check("RPUSH after SORT STORE onto itself", c.cmd("RPUSH", "srt", "9"), 4)
    check("LRANGE after the second write", c.cmd("LRANGE", "srt", "0", "-1"),
          [b"1", b"2", b"3", b"9"])
    check("SORT l STORE l a second time", c.cmd("SORT", "srt", "STORE", "srt"), 4)
    check("LRANGE after the second SORT STORE", c.cmd("LRANGE", "srt", "0", "-1"),
          [b"1", b"2", b"3", b"9"])

    c.cmd("DEL", "sud", "sux")
    c.cmd("SADD", "sud", "a", "b")
    c.cmd("SADD", "sux", "c")
    check("SUNIONSTORE d d x", c.cmd("SUNIONSTORE", "sud", "sud", "sux"), 3)
    check("SMEMBERS after SUNIONSTORE onto itself", sorted(c.cmd("SMEMBERS", "sud")),
          [b"a", b"b", b"c"])
    check("SADD after SUNIONSTORE onto itself", c.cmd("SADD", "sud", "z"), 1)
    check("SCARD after the second write", c.cmd("SCARD", "sud"), 4)
    check("SINTERSTORE d d x", c.cmd("SINTERSTORE", "sud", "sud", "sux"), 1)
    check("SMEMBERS after SINTERSTORE onto itself", c.cmd("SMEMBERS", "sud"), [b"c"])
    check("SDIFFSTORE d d x", c.cmd("SDIFFSTORE", "sud", "sud", "sux"), 0)
    check("EXISTS after SDIFFSTORE emptied it", c.cmd("EXISTS", "sud"), 0)
    check("SADD after SDIFFSTORE emptied it", c.cmd("SADD", "sud", "again"), 1)
    check("SMEMBERS after refilling it", c.cmd("SMEMBERS", "sud"), [b"again"])

    c.cmd("DEL", "zud", "zux")
    c.cmd("ZADD", "zud", "1", "a")
    c.cmd("ZADD", "zux", "2", "b")
    check("ZUNIONSTORE d 2 d x", c.cmd("ZUNIONSTORE", "zud", "2", "zud", "zux"), 2)
    check("ZRANGE after ZUNIONSTORE onto itself", c.cmd("ZRANGE", "zud", "0", "-1", "WITHSCORES"),
          [b"a", b"1", b"b", b"2"])
    check("ZADD after ZUNIONSTORE onto itself", c.cmd("ZADD", "zud", "7", "c"), 1)
    check("ZCARD after the second write", c.cmd("ZCARD", "zud"), 3)
    check("ZINTERSTORE d 2 d x", c.cmd("ZINTERSTORE", "zud", "2", "zud", "zux"), 1)
    check("ZRANGE after ZINTERSTORE onto itself", c.cmd("ZRANGE", "zud", "0", "-1", "WITHSCORES"),
          [b"b", b"4"])

    c.cmd("DEL", "lmv")
    c.cmd("RPUSH", "lmv", "a", "b", "c")
    check("LMOVE l l LEFT RIGHT", c.cmd("LMOVE", "lmv", "lmv", "LEFT", "RIGHT"), b"a")
    check("LRANGE after LMOVE onto itself", c.cmd("LRANGE", "lmv", "0", "-1"),
          [b"b", b"c", b"a"])
    check("RPUSH after LMOVE onto itself", c.cmd("RPUSH", "lmv", "d"), 4)
    check("LRANGE after the second write", c.cmd("LRANGE", "lmv", "0", "-1"),
          [b"b", b"c", b"a", b"d"])
    check("RPOPLPUSH l l", c.cmd("RPOPLPUSH", "lmv", "lmv"), b"d")
    check("LRANGE after RPOPLPUSH onto itself", c.cmd("LRANGE", "lmv", "0", "-1"),
          [b"d", b"b", b"c", b"a"])

    # RESTORE REPLACE onto a key whose elements are the payload's elements
    c.cmd("DEL", "rstla", "rstlb")
    c.cmd("RPUSH", "rstla", "a", "b")
    list_payload = c.cmd("DUMP", "rstla")
    c.cmd("RPUSH", "rstlb", "a", "b")
    check("RESTORE list REPLACE onto matching elements",
          c.cmd("RESTORE", "rstlb", "0", list_payload, "REPLACE"), "OK")
    check("LRANGE the restored list", c.cmd("LRANGE", "rstlb", "0", "-1"), [b"a", b"b"])
    check("RPUSH over the restored list", c.cmd("RPUSH", "rstlb", "z"), 3)
    check("LRANGE after writing over the restore", c.cmd("LRANGE", "rstlb", "0", "-1"),
          [b"a", b"b", b"z"])

    c.cmd("DEL", "rstha", "rsthb")
    c.cmd("HSET", "rstha", "f", "v")
    hash_payload = c.cmd("DUMP", "rstha")
    c.cmd("HSET", "rsthb", "f", "other")
    check("RESTORE hash REPLACE onto the same field",
          c.cmd("RESTORE", "rsthb", "0", hash_payload, "REPLACE"), "OK")
    check("HGETALL the restored hash", c.cmd("HGETALL", "rsthb"), [b"f", b"v"])
    check("HSET over the restored hash", c.cmd("HSET", "rsthb", "g", "w"), 1)
    check("HLEN after writing over the restore", c.cmd("HLEN", "rsthb"), 2)

    c.cmd("DEL", "rstsa", "rstsb")
    c.cmd("SADD", "rstsa", "m")
    set_payload = c.cmd("DUMP", "rstsa")
    c.cmd("SADD", "rstsb", "m")
    check("RESTORE set REPLACE onto the same member",
          c.cmd("RESTORE", "rstsb", "0", set_payload, "REPLACE"), "OK")
    check("SMEMBERS the restored set", c.cmd("SMEMBERS", "rstsb"), [b"m"])
    check("SISMEMBER the restored member", c.cmd("SISMEMBER", "rstsb", "m"), 1)
    check("SADD over the restored set", c.cmd("SADD", "rstsb", "m2"), 1)
    check("SCARD after writing over the restore", c.cmd("SCARD", "rstsb"), 2)

    c.cmd("DEL", "rstza", "rstzb")
    c.cmd("ZADD", "rstza", "1", "m")
    zset_payload = c.cmd("DUMP", "rstza")
    c.cmd("ZADD", "rstzb", "3", "m")
    check("RESTORE zset REPLACE onto the same member",
          c.cmd("RESTORE", "rstzb", "0", zset_payload, "REPLACE"), "OK")
    check("ZSCORE the restored member", c.cmd("ZSCORE", "rstzb", "m"), b"1")
    check("ZADD over the restored zset", c.cmd("ZADD", "rstzb", "2", "m2"), 1)
    check("ZRANGE after writing over the restore", c.cmd("ZRANGE", "rstzb", "0", "-1"),
          [b"m", b"m2"])

    c.cmd("DEL", "rstca")
    c.cmd("SET", "rstca", "sv")
    string_payload = c.cmd("DUMP", "rstca")
    check("RESTORE string REPLACE onto itself",
          c.cmd("RESTORE", "rstca", "0", string_payload, "REPLACE"), "OK")
    check("GET the restored string", c.cmd("GET", "rstca"), b"sv")
    check("SET over the restored string", c.cmd("SET", "rstca", "sv2"), "OK")
    check("GET after writing over the restore", c.cmd("GET", "rstca"), b"sv2")

    # The two reproductions named in the package-4 commit message
    c.cmd("DEL", "hfx")
    c.cmd("HSET", "hfx", "f", "v0")
    c.cmd("HEXPIRE", "hfx", "100", "FIELDS", "1", "f")
    check("MULTI HDEL+HSET over a field with a TTL",
          multi(["HDEL", "hfx", "f"], ["HSET", "hfx", "f", "v1"]), [1, 1])
    check("HGET after MULTI HDEL+HSET with a TTL", c.cmd("HGET", "hfx", "f"), b"v1")
    check("HTTL is gone after the rewrite", c.cmd("HTTL", "hfx", "FIELDS", "1", "f"), [-1])
    c.cmd("DEL", "hfy", "hfz")
    c.cmd("HSET", "hfy", "f", "src")
    c.cmd("HEXPIRE", "hfy", "100", "FIELDS", "1", "f")
    c.cmd("HSET", "hfz", "f", "dst")
    check("COPY REPLACE onto a hash with the same field name",
          c.cmd("COPY", "hfy", "hfz", "REPLACE"), 1)
    check("HGET the copied field", c.cmd("HGET", "hfz", "f"), b"src")
    check("the copied field kept its TTL", c.cmd("HTTL", "hfz", "FIELDS", "1", "f"),
          pred=lambda g: 90 <= g[0] <= 100)
    check("HSET over the copied field", c.cmd("HSET", "hfz", "f", "later"), 0)
    check("HGET after writing over the copy", c.cmd("HGET", "hfz", "f"), b"later")

    # ----- Writing or deleting a key the same transaction created -----
    # With STO built -DREAD_MY_WRITES=OFF, a record this transaction created
    # could not be changed again by it: install() re-published the creation
    # value and ignored every later write, and transDelete on it aborted the
    # transaction. The executor now buffers raw writes in pending_writes and
    # issues one tx_put per key just before commit, so the record is created
    # once with the value the transaction ended up with. Each case below reads
    # the key back in a later transaction and then writes and reads it again.

    # Strings: a second write to a key that did not exist
    c.cmd("DEL", "nw1")
    check("MULTI SET+SET on a new key replies",
          multi(["SET", "nw1", "a"], ["SET", "nw1", "b"]), ["OK", "OK"])
    check("GET keeps the last write", c.cmd("GET", "nw1"), b"b")
    check("STRLEN after MULTI SET+SET", c.cmd("STRLEN", "nw1"), 1)
    check("SET again after MULTI SET+SET", c.cmd("SET", "nw1", "c"), "OK")
    check("GET the later write", c.cmd("GET", "nw1"), b"c")

    c.cmd("DEL", "nw2")
    check("MULTI SET+SET+SET on a new key replies",
          multi(["SET", "nw2", "a"], ["SET", "nw2", "b"], ["SET", "nw2", "c"]),
          ["OK", "OK", "OK"])
    check("GET keeps the third write", c.cmd("GET", "nw2"), b"c")

    # Strings: delete a key the transaction created
    c.cmd("DEL", "nw3")
    check("MULTI SET+DEL on a new key replies",
          multi(["SET", "nw3", "a"], ["DEL", "nw3"]), ["OK", 1])
    check("GET after MULTI SET+DEL", c.cmd("GET", "nw3"), None)
    check("EXISTS after MULTI SET+DEL", c.cmd("EXISTS", "nw3"), 0)
    check("SET after MULTI SET+DEL", c.cmd("SET", "nw3", "later"), "OK")
    check("GET the write after the delete", c.cmd("GET", "nw3"), b"later")

    c.cmd("DEL", "nw4")
    check("MULTI SET+DEL+SET on a new key replies",
          multi(["SET", "nw4", "a"], ["DEL", "nw4"], ["SET", "nw4", "c"]),
          ["OK", 1, "OK"])
    check("GET after MULTI SET+DEL+SET", c.cmd("GET", "nw4"), b"c")
    check("SET again after MULTI SET+DEL+SET", c.cmd("SET", "nw4", "d"), "OK")
    check("GET the second round", c.cmd("GET", "nw4"), b"d")

    # Strings: read-modify-write over a key the transaction created
    c.cmd("DEL", "nw5")
    check("MULTI SET+APPEND on a new key replies",
          multi(["SET", "nw5", "a"], ["APPEND", "nw5", "b"]), ["OK", 2])
    check("GET after MULTI SET+APPEND", c.cmd("GET", "nw5"), b"ab")
    check("APPEND again after the transaction", c.cmd("APPEND", "nw5", "c"), 3)
    check("GET after the second APPEND", c.cmd("GET", "nw5"), b"abc")

    c.cmd("DEL", "nw6")
    check("MULTI SET+INCR on a new key replies",
          multi(["SET", "nw6", "1"], ["INCR", "nw6"]), ["OK", 2])
    check("GET after MULTI SET+INCR", c.cmd("GET", "nw6"), b"2")
    check("INCR again after the transaction", c.cmd("INCR", "nw6"), 3)
    check("GET after the second INCR", c.cmd("GET", "nw6"), b"3")

    c.cmd("DEL", "nw7")
    check("MULTI SET+GETSET on a new key replies",
          multi(["SET", "nw7", "a"], ["GETSET", "nw7", "z"]), ["OK", b"a"])
    check("GET after MULTI SET+GETSET", c.cmd("GET", "nw7"), b"z")

    c.cmd("DEL", "nw8")
    check("MULTI SET+SETRANGE on a new key replies",
          multi(["SET", "nw8", "aaaa"], ["SETRANGE", "nw8", "1", "X"]), ["OK", 4])
    check("GET after MULTI SET+SETRANGE", c.cmd("GET", "nw8"), b"aXaa")

    c.cmd("DEL", "nw9")
    check("MULTI SET+INCRBYFLOAT on a new key replies",
          multi(["SET", "nw9", "10.5"], ["INCRBYFLOAT", "nw9", "0.1"]), ["OK", b"10.6"])
    check("GET after MULTI SET+INCRBYFLOAT", c.cmd("GET", "nw9"), b"10.6")

    # Strings: read back a key the transaction created, in that transaction
    c.cmd("DEL", "nw10")
    check("MULTI SET+GET on a new key replies",
          multi(["SET", "nw10", "a"], ["GET", "nw10"]), ["OK", b"a"])
    check("MULTI SET+STRLEN+TYPE+EXISTS on a new key",
          multi(["SET", "nw10b", "abc"], ["STRLEN", "nw10b"], ["TYPE", "nw10b"],
                ["EXISTS", "nw10b"]),
          pred=lambda g: g == ["OK", 3, "string", 1])
    check("GET the key the same transaction read", c.cmd("GET", "nw10b"), b"abc")

    # TTL meta: created and then rewritten, or created and then removed
    c.cmd("DEL", "nw11")
    check("MULTI SET+EXPIRE on a new key replies",
          multi(["SET", "nw11", "a"], ["EXPIRE", "nw11", "100"]), ["OK", 1])
    check("TTL after MULTI SET+EXPIRE", c.cmd("TTL", "nw11"),
          pred=lambda g: 90 <= g <= 100)
    check("GET after MULTI SET+EXPIRE", c.cmd("GET", "nw11"), b"a")
    check("PERSIST after MULTI SET+EXPIRE", c.cmd("PERSIST", "nw11"), 1)
    check("TTL after the PERSIST", c.cmd("TTL", "nw11"), -1)

    c.cmd("DEL", "nw12")
    check("MULTI SET EX + EXPIRE rewrites the TTL meta",
          multi(["SET", "nw12", "a", "EX", "100"], ["EXPIRE", "nw12", "200"]), ["OK", 1])
    check("TTL takes the second expiration", c.cmd("TTL", "nw12"),
          pred=lambda g: 190 <= g <= 200)
    check("GET the key with the rewritten TTL", c.cmd("GET", "nw12"), b"a")

    c.cmd("DEL", "nw13")
    check("MULTI SET EX + PERSIST deletes the TTL meta it created",
          multi(["SET", "nw13", "a", "EX", "100"], ["PERSIST", "nw13"]), ["OK", 1])
    check("TTL after MULTI SET EX + PERSIST", c.cmd("TTL", "nw13"), -1)
    check("GET after MULTI SET EX + PERSIST", c.cmd("GET", "nw13"), b"a")
    check("EXPIRE still works afterwards", c.cmd("EXPIRE", "nw13", "50"), 1)
    check("TTL after the later EXPIRE", c.cmd("TTL", "nw13"),
          pred=lambda g: 40 <= g <= 50)

    c.cmd("DEL", "nw14")
    check("MULTI SETEX + TTL on a new key",
          multi(["SETEX", "nw14", "100", "a"], ["TTL", "nw14"]),
          pred=lambda g: g[0] == "OK" and 90 <= g[1] <= 100)
    check("GET after MULTI SETEX+TTL", c.cmd("GET", "nw14"), b"a")

    # RENAME of a key the transaction created
    c.cmd("DEL", "nw15", "nw15j")
    check("MULTI SET+RENAME on a new key replies",
          multi(["SET", "nw15", "a"], ["RENAME", "nw15", "nw15j"]), ["OK", "OK"])
    check("GET the renamed key", c.cmd("GET", "nw15j"), b"a")
    check("the source is gone after the rename", c.cmd("EXISTS", "nw15"), 0)
    check("SET over the renamed key", c.cmd("SET", "nw15j", "b"), "OK")
    check("GET after writing over the rename", c.cmd("GET", "nw15j"), b"b")

    c.cmd("DEL", "nw16", "nw16j")
    check("MULTI SET+COPY on a new key replies",
          multi(["SET", "nw16", "a"], ["COPY", "nw16", "nw16j"]), ["OK", 1])
    check("GET the copy of a key made in the same transaction",
          c.cmd("GET", "nw16j"), b"a")
    check("GET the source of that copy", c.cmd("GET", "nw16"), b"a")

    # Hash fields
    c.cmd("DEL", "nh1")
    check("MULTI HSET+HSET on a new field replies",
          multi(["HSET", "nh1", "f", "a"], ["HSET", "nh1", "f", "b"]), [1, 0])
    check("HGET keeps the last write", c.cmd("HGET", "nh1", "f"), b"b")
    check("HLEN after MULTI HSET+HSET", c.cmd("HLEN", "nh1"), 1)
    check("HGETALL after MULTI HSET+HSET", c.cmd("HGETALL", "nh1"), [b"f", b"b"])
    check("HSET again after the transaction", c.cmd("HSET", "nh1", "f", "c"), 0)
    check("HGET the later write", c.cmd("HGET", "nh1", "f"), b"c")

    c.cmd("DEL", "nh2")
    check("MULTI HSET+HDEL on a new field replies",
          multi(["HSET", "nh2", "f", "a"], ["HDEL", "nh2", "f"]), [1, 1])
    check("HGET after MULTI HSET+HDEL", c.cmd("HGET", "nh2", "f"), None)
    check("EXISTS after MULTI HSET+HDEL", c.cmd("EXISTS", "nh2"), 0)
    check("HSET after MULTI HSET+HDEL", c.cmd("HSET", "nh2", "f", "later"), 1)
    check("HGET the write after the delete", c.cmd("HGET", "nh2", "f"), b"later")

    c.cmd("DEL", "nh3")
    check("MULTI HSET+HINCRBY on a new field replies",
          multi(["HSET", "nh3", "f", "1"], ["HINCRBY", "nh3", "f", "1"]), [1, 2])
    check("HGET after MULTI HSET+HINCRBY", c.cmd("HGET", "nh3", "f"), b"2")
    check("HINCRBY again after the transaction", c.cmd("HINCRBY", "nh3", "f", "1"), 3)

    c.cmd("DEL", "nh4")
    check("MULTI HSET+HGETALL on a new field replies",
          multi(["HSET", "nh4", "f", "a"], ["HGETALL", "nh4"]), [1, [b"f", b"a"]])

    c.cmd("DEL", "nh5")
    check("MULTI HSET+HEXPIRE on a new field replies",
          multi(["HSET", "nh5", "f", "a"], ["HEXPIRE", "nh5", "100", "FIELDS", "1", "f"]),
          [1, [1]])
    check("HTTL after MULTI HSET+HEXPIRE", c.cmd("HTTL", "nh5", "FIELDS", "1", "f"),
          pred=lambda g: 90 <= g[0] <= 100)
    check("HGET after MULTI HSET+HEXPIRE", c.cmd("HGET", "nh5", "f"), b"a")
    check("HPERSIST after MULTI HSET+HEXPIRE",
          c.cmd("HPERSIST", "nh5", "FIELDS", "1", "f"), [1])

    c.cmd("DEL", "nh6")
    check("MULTI HSET+HEXPIRE+HPERSIST in one transaction",
          multi(["HSET", "nh6", "f", "a"],
                ["HEXPIRE", "nh6", "100", "FIELDS", "1", "f"],
                ["HPERSIST", "nh6", "FIELDS", "1", "f"]),
          [1, [1], [1]])
    check("HTTL after the in-transaction HPERSIST",
          c.cmd("HTTL", "nh6", "FIELDS", "1", "f"), [-1])
    check("HGET after the in-transaction HPERSIST", c.cmd("HGET", "nh6", "f"), b"a")

    # Sets, zsets and lists (staged in memory, so these were already correct)
    c.cmd("DEL", "ns1")
    check("MULTI SADD+SREM on a new set replies",
          multi(["SADD", "ns1", "m"], ["SREM", "ns1", "m"]), [1, 1])
    check("SMEMBERS after MULTI SADD+SREM", c.cmd("SMEMBERS", "ns1"), [])
    check("EXISTS after MULTI SADD+SREM", c.cmd("EXISTS", "ns1"), 0)
    check("SADD after MULTI SADD+SREM", c.cmd("SADD", "ns1", "z"), 1)
    check("SMEMBERS after the later SADD", c.cmd("SMEMBERS", "ns1"), [b"z"])

    c.cmd("DEL", "ns2")
    check("MULTI SADD+DEL on a new set replies",
          multi(["SADD", "ns2", "a"], ["DEL", "ns2"]), [1, 1])
    check("EXISTS after MULTI SADD+DEL", c.cmd("EXISTS", "ns2"), 0)
    check("SADD after MULTI SADD+DEL", c.cmd("SADD", "ns2", "z"), 1)
    check("SMEMBERS after MULTI SADD+DEL", c.cmd("SMEMBERS", "ns2"), [b"z"])

    c.cmd("DEL", "nz1")
    check("MULTI ZADD+ZADD on a new member replies",
          multi(["ZADD", "nz1", "1", "m"], ["ZADD", "nz1", "2", "m"]), [1, 0])
    check("ZSCORE keeps the last score", c.cmd("ZSCORE", "nz1", "m"), b"2")
    check("ZCARD after MULTI ZADD+ZADD", c.cmd("ZCARD", "nz1"), 1)
    check("ZADD again after the transaction", c.cmd("ZADD", "nz1", "3", "m"), 0)
    check("ZSCORE the later write", c.cmd("ZSCORE", "nz1", "m"), b"3")

    c.cmd("DEL", "nz2")
    check("MULTI ZADD+ZREM on a new member replies",
          multi(["ZADD", "nz2", "1", "m"], ["ZREM", "nz2", "m"]), [1, 1])
    check("ZSCORE after MULTI ZADD+ZREM", c.cmd("ZSCORE", "nz2", "m"), None)
    check("EXISTS after MULTI ZADD+ZREM", c.cmd("EXISTS", "nz2"), 0)

    c.cmd("DEL", "nl1")
    check("MULTI RPUSH+LSET on a new list replies",
          multi(["RPUSH", "nl1", "a"], ["LSET", "nl1", "0", "b"]), [1, "OK"])
    check("LRANGE after MULTI RPUSH+LSET", c.cmd("LRANGE", "nl1", "0", "-1"), [b"b"])
    check("LSET again after the transaction", c.cmd("LSET", "nl1", "0", "c"), "OK")
    check("LRANGE after the later LSET", c.cmd("LRANGE", "nl1", "0", "-1"), [b"c"])

    c.cmd("DEL", "nl2")
    check("MULTI RPUSH+DEL on a new list replies",
          multi(["RPUSH", "nl2", "a"], ["DEL", "nl2"]), [1, 1])
    check("EXISTS after MULTI RPUSH+DEL", c.cmd("EXISTS", "nl2"), 0)
    check("RPUSH after MULTI RPUSH+DEL", c.cmd("RPUSH", "nl2", "z"), 1)
    check("LRANGE after MULTI RPUSH+DEL", c.cmd("LRANGE", "nl2", "0", "-1"), [b"z"])

    # FLUSHALL drops what the same transaction buffered
    c.cmd("DEL", "nf1")
    check("MULTI SET+FLUSHALL replies", multi(["SET", "nf1", "a"], ["FLUSHALL"]),
          ["OK", "OK"])
    check("the buffered write did not survive FLUSHALL", c.cmd("EXISTS", "nf1"), 0)
    check("SET after the FLUSHALL", c.cmd("SET", "nf1", "b"), "OK")
    check("GET after the FLUSHALL", c.cmd("GET", "nf1"), b"b")

    # ----- INCR family error handling -----
    # These used to abort the transaction, which reached the client as
    # "ERR backend" after the 32-attempt retry loop, and an increment on a
    # collection replaced it with a string instead of answering WRONGTYPE.

    c.cmd("SET", "ie1", "abc")
    for name, args in (("INCR", ("INCR", "ie1")),
                       ("INCRBY", ("INCRBY", "ie1", "5")),
                       ("DECR", ("DECR", "ie1")),
                       ("DECRBY", ("DECRBY", "ie1", "5"))):
        check("%s on a non-integer value" % name, c.cmd(*args),
              pred=lambda g: is_err(g, "ERR value is not an integer or out of range"))
    check("INCRBYFLOAT on a non-float value", c.cmd("INCRBYFLOAT", "ie1", "1.0"),
          pred=lambda g: is_err(g, "ERR value is not a valid float"))
    check("the failed increments left the value alone", c.cmd("GET", "ie1"), b"abc")
    check("the key is still usable after the errors", c.cmd("SET", "ie1", "7"), "OK")
    check("INCR works once the value is an integer", c.cmd("INCR", "ie1"), 8)

    c.cmd("SET", "ie2", "9223372036854775807")
    check("INCR overflow", c.cmd("INCR", "ie2"),
          pred=lambda g: is_err(g, "ERR increment or decrement would overflow"))
    check("INCRBY overflow", c.cmd("INCRBY", "ie2", "1"),
          pred=lambda g: is_err(g, "ERR increment or decrement would overflow"))
    check("the overflowed value is unchanged", c.cmd("GET", "ie2"),
          b"9223372036854775807")
    c.cmd("SET", "ie3", "-9223372036854775808")
    check("DECR overflow", c.cmd("DECR", "ie3"),
          pred=lambda g: is_err(g, "ERR increment or decrement would overflow"))
    check("DECRBY overflow", c.cmd("DECRBY", "ie3", "1"),
          pred=lambda g: is_err(g, "ERR increment or decrement would overflow"))

    c.cmd("SET", "ie4", "0")
    check("INCRBY with a non-integer increment", c.cmd("INCRBY", "ie4", "foo"),
          pred=lambda g: is_err(g, "ERR value is not an integer or out of range"))
    check("DECRBY with a non-integer increment", c.cmd("DECRBY", "ie4", "foo"),
          pred=lambda g: is_err(g, "ERR value is not an integer or out of range"))
    check("INCRBY with an out-of-range increment",
          c.cmd("INCRBY", "ie4", "99999999999999999999999"),
          pred=lambda g: is_err(g, "ERR value is not an integer or out of range"))
    # Redis negates DECRBY's argument, and -LLONG_MIN has no counterpart.
    check("DECRBY by LLONG_MIN", c.cmd("DECRBY", "ie4", "-9223372036854775808"),
          pred=lambda g: is_err(g, "ERR decrement would overflow"))
    check("the key survived the bad increments", c.cmd("GET", "ie4"), b"0")

    c.cmd("SET", "ie5", "10.5")
    check("INCRBYFLOAT with a non-float increment", c.cmd("INCRBYFLOAT", "ie5", "foo"),
          pred=lambda g: is_err(g, "ERR value is not a valid float"))
    check("INCRBYFLOAT with a NaN increment", c.cmd("INCRBYFLOAT", "ie5", "nan"),
          pred=lambda g: is_err(g, "ERR value is not a valid float"))
    check("INCRBYFLOAT by infinity", c.cmd("INCRBYFLOAT", "ie5", "+inf"),
          pred=lambda g: is_err(g, "ERR increment would produce NaN or Infinity"))
    check("INCRBYFLOAT by negative infinity", c.cmd("INCRBYFLOAT", "ie5", "-inf"),
          pred=lambda g: is_err(g, "ERR increment would produce NaN or Infinity"))
    check("the float value is unchanged", c.cmd("GET", "ie5"), b"10.5")
    check("INCRBYFLOAT still works", c.cmd("INCRBYFLOAT", "ie5", "0.1"), b"10.6")
    c.cmd("SET", "ie6", "inf")
    check("INCRBYFLOAT on a stored infinity", c.cmd("INCRBYFLOAT", "ie6", "1"),
          pred=lambda g: is_err(g, "ERR increment would produce NaN or Infinity"))

    # WRONGTYPE for every increment on every collection type
    for kind, make in (("list", ("RPUSH", "iw", "x")),
                       ("set", ("SADD", "iw", "x")),
                       ("hash", ("HSET", "iw", "f", "x")),
                       ("zset", ("ZADD", "iw", "1", "x"))):
        c.cmd("DEL", "iw")
        c.cmd(*make)
        for name, args in (("INCR", ("INCR", "iw")),
                           ("INCRBY", ("INCRBY", "iw", "2")),
                           ("DECR", ("DECR", "iw")),
                           ("DECRBY", ("DECRBY", "iw", "2")),
                           ("INCRBYFLOAT", ("INCRBYFLOAT", "iw", "1.5"))):
            check("%s on a %s is WRONGTYPE" % (name, kind), c.cmd(*args),
                  pred=lambda g: is_err(g, "WRONGTYPE"))
        check("the %s survived the increments" % kind, c.cmd("TYPE", "iw"), kind)

    # An increment on a missing key still starts from zero
    c.cmd("DEL", "iz")
    check("INCR on a missing key", c.cmd("INCR", "iz"), 1)
    c.cmd("DEL", "iz")
    check("INCRBY on a missing key", c.cmd("INCRBY", "iz", "5"), 5)
    c.cmd("DEL", "iz")
    check("DECRBY on a missing key", c.cmd("DECRBY", "iz", "5"), -5)
    c.cmd("DEL", "iz")
    check("INCRBYFLOAT on a missing key", c.cmd("INCRBYFLOAT", "iz", "1.5"), b"1.5")

    # ----- RENAME writes zset scores in the canonical format -----
    # RENAME used to store the member's score with std::to_string, which is
    # "%.6f": 1 became "1.000000", 3.141592653589793 became "3.141593" and
    # 0.0000001 became "0.000000". Every other path stores format_zset_score.
    c.cmd("DEL", "zr1", "zr1d")
    c.cmd("ZADD", "zr1", "1", "m", "2.5", "n", "0", "o", "-3", "p",
          "3.141592653589793", "pi", "0.0000001", "tiny", "1e300", "huge")
    members = ("m", "n", "o", "p", "pi", "tiny", "huge")
    scores_before = [c.cmd("ZSCORE", "zr1", member) for member in members]
    check("ZSCORE before RENAME is the canonical format", scores_before,
          [b"1", b"2.5", b"0", b"-3", b"3.1415926535897931",
           b"9.9999999999999995e-08", b"1.0000000000000001e+300"])
    check("RENAME the zset", c.cmd("RENAME", "zr1", "zr1d"), "OK")
    check("ZSCORE after RENAME is the same string as before",
          [c.cmd("ZSCORE", "zr1d", member) for member in members], scores_before)
    check("ZRANGE WITHSCORES after RENAME", c.cmd("ZRANGE", "zr1d", "0", "-1", "WITHSCORES"),
          [b"p", b"-3", b"o", b"0", b"tiny", b"9.9999999999999995e-08",
           b"m", b"1", b"n", b"2.5", b"pi", b"3.1415926535897931",
           b"huge", b"1.0000000000000001e+300"])
    check("a precise score still sorts by score after RENAME",
          c.cmd("ZRANGEBYSCORE", "zr1d", "0.00000005", "0.0000002"), [b"tiny"])
    check("ZINCRBY on the renamed zset", c.cmd("ZINCRBY", "zr1d", "1", "m"), b"2")
    check("ZSCORE after the ZINCRBY", c.cmd("ZSCORE", "zr1d", "m"), b"2")

    c.cmd("DEL", "zr2", "zr2d")
    c.cmd("ZADD", "zr2", "3.141592653589793", "pi")
    check("RENAMENX the zset", c.cmd("RENAMENX", "zr2", "zr2d"), 1)
    check("ZSCORE after RENAMENX", c.cmd("ZSCORE", "zr2d", "pi"),
          b"3.1415926535897931")

    c.cmd("DEL", "zr3", "zr3d")
    c.cmd("ZADD", "zr3", "3.141592653589793", "pi")
    check("COPY the zset", c.cmd("COPY", "zr3", "zr3d"), 1)
    check("ZSCORE after COPY", c.cmd("ZSCORE", "zr3d", "pi"), b"3.1415926535897931")

    # RENAME onto an existing zset, and a zset renamed twice
    c.cmd("DEL", "zr4", "zr4d")
    c.cmd("ZADD", "zr4", "2.5", "m")
    c.cmd("ZADD", "zr4d", "9", "other")
    check("RENAME over an existing zset", c.cmd("RENAME", "zr4", "zr4d"), "OK")
    check("the destination holds only the source's members",
          c.cmd("ZRANGE", "zr4d", "0", "-1", "WITHSCORES"), [b"m", b"2.5"])
    check("RENAME the result again", c.cmd("RENAME", "zr4d", "zr4e"), "OK")
    check("ZSCORE after the second RENAME", c.cmd("ZSCORE", "zr4e", "m"), b"2.5")

    # ----- CLUSTER emulation, READONLY, READWRITE -----
    check_cluster(c, cluster_emulated())

    # ----- MONITOR -----
    check_monitor(host, port, c)

    # ----- Logical databases: SELECT, MOVE, COPY ... DB -----
    check_logical_databases(host, port)

    # ----- Lua scripting -----
    check_scripting(host, port, c)

    # ----- Streams -----
    check_streams_basic(host, port, c)
    check_streams_read(host, port, c)
    check_streams_groups(host, port, c)

    # ----- MODULE shim -----
    # There is no Redis module ABI below this adapter, so LIST is truthfully
    # empty and every loading form answers with Redis's own failure text.
    check("MODULE LIST is empty", c.cmd("MODULE", "LIST"), [])
    check("MODULE LOAD fails", c.cmd("MODULE", "LOAD", "/tmp/whatever.so"),
          pred=lambda g: is_err(g, "ERR Error loading the extension. Please check the server logs."))
    check("MODULE LOAD with arguments fails",
          c.cmd("MODULE", "LOAD", "/tmp/whatever.so", "arg1", "arg2"),
          pred=lambda g: is_err(g, "ERR Error loading the extension. Please check the server logs."))
    check("MODULE LOADEX fails",
          c.cmd("MODULE", "LOADEX", "/tmp/whatever.so", "CONFIG", "k", "v", "ARGS", "a"),
          pred=lambda g: is_err(g, "ERR Error loading the extension. Please check the server logs."))
    check("MODULE UNLOAD has nothing to unload", c.cmd("MODULE", "UNLOAD", "mymodule"),
          pred=lambda g: is_err(g, "ERR Error unloading module: no such module with that name"))
    module_help = c.cmd("MODULE", "HELP")
    check("MODULE HELP shape", module_help,
          pred=lambda g: isinstance(g, list) and len(g) == 11
          and g[0].startswith(b"MODULE <subcommand>")
          and b"LIST" in g and b"UNLOAD <name>" in g
          and g[-1] == b"    Print this help.")
    check("MODULE unknown subcommand", c.cmd("MODULE", "NOSUCHTHING"),
          pred=lambda g: is_err(g, "ERR unknown subcommand 'NOSUCHTHING'. Try MODULE HELP."))
    check("MODULE with no subcommand", c.cmd("MODULE"),
          pred=lambda g: is_err(g, "ERR wrong number of arguments for 'module' command"))
    check("MODULE LIST is still empty after the failed load", c.cmd("MODULE", "LIST"), [])
    # Queued inside MULTI like the other container commands.
    c.cmd("MULTI")
    check("MODULE LIST is queued", c.cmd("MODULE", "LIST"), "QUEUED")
    check("EXEC answers MODULE LIST", c.cmd("EXEC"), [[]])

    c.cmd("SET", "t1", "v")
    c.cmd("SADD", "s1", "a")

    # Untouched basics still work
    check("SET/GET regression", c.cmd("GET", "t1"), b"v")
    check("EXISTS regression", c.cmd("EXISTS", "t1", "s1"), 2)

    print("phase2 checks: %d passed, %d failed" % (PASSED, len(FAILED)))
    sys.exit(1 if FAILED else 0)


if __name__ == "__main__":
    main()
