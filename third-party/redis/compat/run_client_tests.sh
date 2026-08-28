#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT_FILE="${CLIENT_TEST_RESULTS:-${ROOT_DIR}/third-party/redis/compat/client_test_results.csv}"
LOG_DIR="${CLIENT_TEST_LOG_DIR:-${ROOT_DIR}/third-party/redis/compat/client_logs}"
MAKO_HOST="${MAKO_HOST:-${MAKO_REDIS_HOST:-127.0.0.1}}"
MAKO_PORT="${MAKO_PORT:-${MAKO_REDIS_PORT:-6380}}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"
TOOLS_PATH="/home/users/ssoumojit/.local/bin:${PATH}"
JAVA_HOME="${JAVA_HOME:-/home/users/ssoumojit/.local/opt/jdk-21}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
FAILURES=0

mkdir -p "$(dirname "${OUT_FILE}")" "${LOG_DIR}"
printf 'tool,status,detail\n' >"${OUT_FILE}"

csv_escape() {
    "${PYTHON_BIN}" - "$1" <<'PY'
import csv
import io
import sys

buf = io.StringIO()
csv.writer(buf).writerow([sys.argv[1]])
print(buf.getvalue().strip())
PY
}

row() {
    local tool="$1"
    local status="$2"
    local detail="$3"
    if [[ "${status}" == "FAIL" ]]; then
        FAILURES=$((FAILURES + 1))
    fi
    printf '%s,%s,%s\n' "$(csv_escape "${tool}")" "${status}" "$(csv_escape "${detail}")" | tee -a "${OUT_FILE}" >/dev/null
    printf '%-16s %-6s %s\n' "${tool}" "${status}" "${detail}"
}

redis_ready() {
    local host="$1"
    local port="$2"
    command -v redis-cli >/dev/null 2>&1 &&
        redis-cli -h "${host}" -p "${port}" PING >/dev/null 2>&1
}

require_targets() {
    if ! redis_ready "${REDIS_HOST}" "${REDIS_PORT}"; then
        row "$1" "N/A" "reference Redis not reachable at ${REDIS_HOST}:${REDIS_PORT}"
        return 1
    fi
    if ! redis_ready "${MAKO_HOST}" "${MAKO_PORT}"; then
        row "$1" "N/A" "makoCon not reachable at ${MAKO_HOST}:${MAKO_PORT}"
        return 1
    fi
    return 0
}

run_checked() {
    local name="$1"
    local logfile="${LOG_DIR}/${name}.log"
    shift
    if "$@" >"${logfile}" 2>&1; then
        row "${name}" "PASS" "$(tail -n 1 "${logfile}")"
    else
        row "${name}" "FAIL" "$(tail -n 1 "${logfile}")"
    fi
}

run_pytest() {
    require_targets "pytest" || return
    run_checked "pytest" bash -lc "cd '${ROOT_DIR}' && MAKO_REDIS_HOST='${MAKO_HOST}' MAKO_REDIS_PORT='${MAKO_PORT}' '${PYTHON_BIN}' -m pytest third-party/redis/compat -q --ignore=third-party/redis/compat/_client_tmp"
}

run_redis_cli() {
    require_targets "redis-cli" || return
    local logfile="${LOG_DIR}/redis-cli.log"
    {
        for target in "redis ${REDIS_HOST} ${REDIS_PORT}" "mako ${MAKO_HOST} ${MAKO_PORT}"; do
            read -r label host port <<<"${target}"
            key="g1:cli:${label}:${port}"
            redis-cli -h "${host}" -p "${port}" PING
            redis-cli -h "${host}" -p "${port}" HELLO 3 >/dev/null
            redis-cli -h "${host}" -p "${port}" SET "${key}" "value"
            redis-cli -h "${host}" -p "${port}" GET "${key}"
            redis-cli -h "${host}" -p "${port}" MSET "${key}:m1" one "${key}:m2" two
            redis-cli -h "${host}" -p "${port}" MGET "${key}:m1" "${key}:missing" "${key}:m2"
            redis-cli -h "${host}" -p "${port}" ECHO "payload"
            redis-cli -h "${host}" -p "${port}" --pipe <<EOF
MULTI
SET ${key}:tx 1
GET ${key}:tx
EXEC
EOF
            redis-cli -h "${host}" -p "${port}" DEL "${key}" "${key}:m1" "${key}:m2" "${key}:tx"
        done
    } >"${logfile}" 2>&1
    local status=$?
    if [[ "${status}" -eq 0 ]]; then
        row "redis-cli" "PASS" "RESP3/PING/SET/MGET/ECHO/MULTI/DEL completed on both ports"
    else
        row "redis-cli" "FAIL" "$(tail -n 1 "${logfile}")"
    fi
}

run_redis_py() {
    require_targets "redis-py" || return
    run_checked "redis-py" env REDIS_HOST="${REDIS_HOST}" REDIS_PORT="${REDIS_PORT}" MAKO_HOST="${MAKO_HOST}" MAKO_PORT="${MAKO_PORT}" "${PYTHON_BIN}" - <<'PY'
import os
import redis

for label, host, port in [
    ("redis", os.environ["REDIS_HOST"], int(os.environ["REDIS_PORT"])),
    ("mako", os.environ["MAKO_HOST"], int(os.environ["MAKO_PORT"])),
]:
    r = redis.Redis(host=host, port=port, decode_responses=False)
    assert r.ping() is True
    key = f"g1:redis-py:{label}".encode()
    assert r.set(key, b"value") is True
    assert r.get(key) == b"value"
    assert r.get(key + b":missing") is None
    assert r.set(key + b":binary", b"\x00\x01payload\xff") is True
    assert r.get(key + b":binary") == b"\x00\x01payload\xff"
    assert r.mset({key + b":m1": b"one", key + b":m2": b"two"}) is True
    assert r.mget(key + b":m1", key + b":missing", key + b":m2") == [b"one", None, b"two"]
    pipe = r.pipeline(transaction=False)
    pipe.set(key + b":pipe", b"p")
    pipe.get(key + b":pipe")
    assert pipe.execute() == [True, b"p"]
    pipe = r.pipeline(transaction=True)
    pipe.set(key + b":tx", b"1")
    pipe.get(key + b":tx")
    assert pipe.execute() == [True, b"1"]
print("redis-py smoke completed on both targets")
PY
}

run_node_clients() {
    require_targets "node-clients" || return
    if ! PATH="${TOOLS_PATH}" command -v npm >/dev/null 2>&1 || ! PATH="${TOOLS_PATH}" command -v node >/dev/null 2>&1; then
        row "node-clients" "N/A" "node/npm not installed"
        return
    fi
    local dir="${ROOT_DIR}/third-party/redis/compat/_client_tmp/node"
    local logfile="${LOG_DIR}/node-clients.log"
    mkdir -p "${dir}"
    if [[ ! -d "${dir}/node_modules/redis" || ! -d "${dir}/node_modules/ioredis" ]]; then
        (cd "${dir}" && npm init -y >/dev/null 2>&1 && npm install redis@^4 ioredis@^5 >/dev/null 2>&1)
    fi
    cat >"${dir}/g1_node_clients.js" <<'JS'
const { createClient } = require('redis');
const IORedis = require('ioredis');

const targets = [
  ['redis', process.env.REDIS_HOST, Number(process.env.REDIS_PORT)],
  ['mako', process.env.MAKO_HOST, Number(process.env.MAKO_PORT)],
];

async function runNodeRedis(label, host, port) {
  const client = createClient({ socket: { host, port } });
  await client.connect();
  const key = `g1:node-redis:${label}`;
  if (await client.ping() !== 'PONG') throw new Error('PING failed');
  if (await client.set(key, 'value') !== 'OK') throw new Error('SET failed');
  if (await client.get(key) !== 'value') throw new Error('GET failed');
  if (await client.get(`${key}:missing`) !== null) throw new Error('nil GET failed');
  if (await client.mSet({ [`${key}:m1`]: 'one', [`${key}:m2`]: 'two' }) !== 'OK') throw new Error('MSET failed');
  const mget = await client.mGet([`${key}:m1`, `${key}:missing`, `${key}:m2`]);
  if (JSON.stringify(mget) !== JSON.stringify(['one', null, 'two'])) throw new Error(`MGET failed ${JSON.stringify(mget)}`);
  const tx = client.multi();
  tx.set(`${key}:tx`, '1');
  tx.get(`${key}:tx`);
  const result = await tx.exec();
  if (result[0] !== 'OK' || result[1] !== '1') throw new Error(`MULTI failed ${JSON.stringify(result)}`);
  await client.quit();
}

async function runIORedis(label, host, port) {
  const client = new IORedis(port, host, { lazyConnect: true, maxRetriesPerRequest: 1 });
  await client.connect();
  const key = `g1:ioredis:${label}`;
  if (await client.ping() !== 'PONG') throw new Error('PING failed');
  if (await client.set(key, 'value') !== 'OK') throw new Error('SET failed');
  if (await client.get(key) !== 'value') throw new Error('GET failed');
  if (await client.get(`${key}:missing`) !== null) throw new Error('nil GET failed');
  if (await client.set(Buffer.from(`${key}:binary`), Buffer.from([0, 1, 2, 255])) !== 'OK') throw new Error('binary SET failed');
  const binary = await client.getBuffer(`${key}:binary`);
  if (!Buffer.from([0, 1, 2, 255]).equals(binary)) throw new Error('binary GET failed');
  if (await client.mset(`${key}:m1`, 'one', `${key}:m2`, 'two') !== 'OK') throw new Error('MSET failed');
  const mget = await client.mget(`${key}:m1`, `${key}:missing`, `${key}:m2`);
  if (JSON.stringify(mget) !== JSON.stringify(['one', null, 'two'])) throw new Error(`MGET failed ${JSON.stringify(mget)}`);
  const pipe = await client.pipeline().set(`${key}:pipe`, 'p').get(`${key}:pipe`).exec();
  if (pipe[0][1] !== 'OK' || pipe[1][1] !== 'p') throw new Error(`pipeline failed ${JSON.stringify(pipe)}`);
  const result = await client.multi().set(`${key}:tx`, '1').get(`${key}:tx`).exec();
  if (result[0][1] !== 'OK' || result[1][1] !== '1') throw new Error(`MULTI failed ${JSON.stringify(result)}`);
  client.disconnect();
}

(async () => {
  for (const [label, host, port] of targets) {
    await runNodeRedis(label, host, port);
    await runIORedis(label, host, port);
  }
  console.log('node-redis and ioredis smoke completed on both targets');
})().catch((err) => {
  console.error(err.stack || err.message || String(err));
  process.exit(1);
});
JS
    if (cd "${dir}" && env REDIS_HOST="${REDIS_HOST}" REDIS_PORT="${REDIS_PORT}" MAKO_HOST="${MAKO_HOST}" MAKO_PORT="${MAKO_PORT}" PATH="${TOOLS_PATH}" node g1_node_clients.js >"${logfile}" 2>&1); then
        row "node-clients" "PASS" "$(tail -n 1 "${logfile}")"
    else
        row "node-clients" "FAIL" "$(tail -n 1 "${logfile}")"
    fi
}

run_jedis() {
    require_targets "jedis" || return
    if ! PATH="${TOOLS_PATH}" command -v mvn >/dev/null 2>&1; then
        row "jedis" "N/A" "mvn not installed"
        return
    fi
    local dir="${ROOT_DIR}/third-party/redis/compat/_client_tmp/jedis"
    local logfile="${LOG_DIR}/jedis.log"
    mkdir -p "${dir}/src/main/java"
    cat >"${dir}/pom.xml" <<'XML'
<project xmlns="http://maven.apache.org/POM/4.0.0"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 https://maven.apache.org/xsd/maven-4.0.0.xsd">
  <modelVersion>4.0.0</modelVersion>
  <groupId>mako.redis.compat</groupId>
  <artifactId>jedis-smoke</artifactId>
  <version>1.0.0</version>
  <properties>
    <maven.compiler.release>17</maven.compiler.release>
    <maven.compiler.source>17</maven.compiler.source>
    <maven.compiler.target>17</maven.compiler.target>
  </properties>
  <dependencies>
    <dependency>
      <groupId>redis.clients</groupId>
      <artifactId>jedis</artifactId>
      <version>5.2.0</version>
    </dependency>
  </dependencies>
</project>
XML
    cat >"${dir}/src/main/java/JedisSmoke.java" <<'JAVA'
import redis.clients.jedis.JedisPooled;
import java.util.List;

public final class JedisSmoke {
    private static void check(boolean condition, String message) {
        if (!condition) throw new RuntimeException(message);
    }

    private static void run(String label, String host, int port) {
        try (JedisPooled jedis = new JedisPooled(host, port)) {
            check("PONG".equals(jedis.ping()), "PING failed");
            String key = "g1:jedis:" + label;
            check("OK".equals(jedis.set(key, "value")), "SET failed");
            check("value".equals(jedis.get(key)), "GET failed");
            check(jedis.get(key + ":missing") == null, "nil GET failed");
            check("OK".equals(jedis.mset(key + ":m1", "one", key + ":m2", "two")), "MSET failed");
            List<String> mget = jedis.mget(key + ":m1", key + ":missing", key + ":m2");
            check("one".equals(mget.get(0)) && mget.get(1) == null && "two".equals(mget.get(2)), "MGET failed");
            var tx = jedis.multi();
            tx.set(key + ":tx", "1");
            tx.get(key + ":tx");
            List<Object> result = tx.exec();
            check("OK".equals(result.get(0)), "MULTI SET failed");
            check("1".equals(result.get(1)), "MULTI GET failed");
        }
    }

    public static void main(String[] args) {
        run("redis", System.getenv("REDIS_HOST"), Integer.parseInt(System.getenv("REDIS_PORT")));
        run("mako", System.getenv("MAKO_HOST"), Integer.parseInt(System.getenv("MAKO_PORT")));
        System.out.println("jedis smoke completed on both targets");
    }
}
JAVA
    if (cd "${dir}" && env REDIS_HOST="${REDIS_HOST}" REDIS_PORT="${REDIS_PORT}" MAKO_HOST="${MAKO_HOST}" MAKO_PORT="${MAKO_PORT}" JAVA_HOME="${JAVA_HOME}" PATH="${TOOLS_PATH}" mvn -q compile exec:java -Dexec.mainClass=JedisSmoke >"${logfile}" 2>&1); then
        row "jedis" "PASS" "$(tail -n 1 "${logfile}")"
    else
        row "jedis" "FAIL" "$(tail -n 1 "${logfile}")"
    fi
}

run_redis_rs() {
    require_targets "redis-rs" || return
    if ! command -v cargo >/dev/null 2>&1; then
        row "redis-rs" "N/A" "cargo not installed"
        return
    fi
    local dir="${ROOT_DIR}/third-party/redis/compat/_client_tmp/redis-rs"
    local logfile="${LOG_DIR}/redis-rs.log"
    mkdir -p "${dir}/src"
    cat >"${dir}/Cargo.toml" <<'TOML'
[package]
name = "redis-rs-smoke"
version = "0.1.0"
edition = "2021"

[dependencies]
redis = "0.27"
TOML
    cat >"${dir}/src/main.rs" <<'RS'
use redis::{Commands, RedisResult};

fn run(label: &str, host: &str, port: &str) -> RedisResult<()> {
    let client = redis::Client::open(format!("redis://{}:{}/", host, port))?;
    let mut con = client.get_connection()?;
    let pong: String = redis::cmd("PING").query(&mut con)?;
    assert_eq!(pong, "PONG");
    let key = format!("g1:redis-rs:{}", label);
    let ok: String = con.set(&key, "value")?;
    assert_eq!(ok, "OK");
    let value: String = con.get(&key)?;
    assert_eq!(value, "value");
    let missing: Option<String> = con.get(format!("{}:missing", key))?;
    assert_eq!(missing, None);
    let _: () = redis::cmd("SET").arg(format!("{}:binary", key)).arg(b"\x00\x01payload\xff".as_slice()).query(&mut con)?;
    let binary: Vec<u8> = con.get(format!("{}:binary", key))?;
    assert_eq!(binary, b"\x00\x01payload\xff");
    let _: () = redis::cmd("MSET")
        .arg(format!("{}:m1", key)).arg("one")
        .arg(format!("{}:m2", key)).arg("two")
        .query(&mut con)?;
    let mget: Vec<Option<String>> = redis::cmd("MGET")
        .arg(format!("{}:m1", key))
        .arg(format!("{}:missing", key))
        .arg(format!("{}:m2", key))
        .query(&mut con)?;
    assert_eq!(mget, vec![Some("one".to_string()), None, Some("two".to_string())]);
    let pipe_result: Vec<String> = redis::pipe()
        .cmd("SET").arg(format!("{}:pipe", key)).arg("p")
        .cmd("GET").arg(format!("{}:pipe", key))
        .query(&mut con)?;
    assert_eq!(pipe_result, vec!["OK".to_string(), "p".to_string()]);
    let result: Vec<String> = redis::pipe()
        .atomic()
        .cmd("SET").arg(format!("{}:tx", key)).arg("1")
        .cmd("GET").arg(format!("{}:tx", key))
        .query(&mut con)?;
    assert_eq!(result, vec!["OK".to_string(), "1".to_string()]);
    Ok(())
}

fn main() -> RedisResult<()> {
    run("redis", &std::env::var("REDIS_HOST").unwrap(), &std::env::var("REDIS_PORT").unwrap())?;
    run("mako", &std::env::var("MAKO_HOST").unwrap(), &std::env::var("MAKO_PORT").unwrap())?;
    println!("redis-rs smoke completed on both targets");
    Ok(())
}
RS
    if (cd "${dir}" && env REDIS_HOST="${REDIS_HOST}" REDIS_PORT="${REDIS_PORT}" MAKO_HOST="${MAKO_HOST}" MAKO_PORT="${MAKO_PORT}" cargo run --quiet >"${logfile}" 2>&1); then
        row "redis-rs" "PASS" "$(tail -n 1 "${logfile}")"
    else
        row "redis-rs" "FAIL" "$(tail -n 1 "${logfile}")"
    fi
}

run_redis_exporter() {
    require_targets "redis_exporter" || return
    if ! PATH="${TOOLS_PATH}" command -v redis_exporter >/dev/null 2>&1; then
        row "redis_exporter" "N/A" "redis_exporter not installed"
        return
    fi
    local logfile="${LOG_DIR}/redis_exporter.log"
    local port="${REDIS_EXPORTER_WEB_PORT:-9121}"
    PATH="${TOOLS_PATH}" redis_exporter --redis.addr="redis://${MAKO_HOST}:${MAKO_PORT}" --web.listen-address="127.0.0.1:${port}" >"${logfile}" 2>&1 &
    local pid=$!
    sleep 1
    local status=1
    if curl -fsS "http://127.0.0.1:${port}/metrics" | grep -q '^redis_up 1'; then
        status=0
    fi
    kill "${pid}" >/dev/null 2>&1 || true
    wait "${pid}" >/dev/null 2>&1 || true
    if [[ "${status}" -eq 0 ]]; then
        row "redis_exporter" "PASS" "scrape returned redis_up 1"
    else
        row "redis_exporter" "FAIL" "$(tail -n 1 "${logfile}")"
    fi
}

start_fakeredis_forwarder() {
    local target_host="$1"
    local target_port="$2"
    local listen_port="$3"

    "${PYTHON_BIN}" - "${target_host}" "${target_port}" "${listen_port}" >"${LOG_DIR}/fakeredis-forwarder.log" 2>&1 <<'PY' &
import select
import socket
import sys
import threading

target_host = sys.argv[1]
target_port = int(sys.argv[2])
listen_port = int(sys.argv[3])


def pump(left: socket.socket, right: socket.socket) -> None:
    sockets = [left, right]
    try:
        while True:
            readable, _, _ = select.select(sockets, [], [])
            for src in readable:
                data = src.recv(65536)
                if not data:
                    return
                dst = right if src is left else left
                dst.sendall(data)
    finally:
        for sock in sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()


with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", listen_port))
    server.listen()
    while True:
        client, _ = server.accept()
        try:
            upstream = socket.create_connection((target_host, target_port), timeout=5)
        except OSError:
            client.close()
            continue
        threading.Thread(target=pump, args=(client, upstream), daemon=True).start()
PY
    echo "$!"
}

run_fakeredis_py() {
    require_targets "fakeredis-py" || return

    local dir="${ROOT_DIR}/third-party/redis/compat/_client_tmp/fakeredis-py"
    local repo="${dir}/src"
    local deps="${dir}/deps"
    local logfile="${LOG_DIR}/fakeredis-py.log"
    local listen_port="${FAKEREDIS_REAL_PORT:-6390}"
    local pytest_k="${FAKEREDIS_PY_K:-Strict2 or Strict3}"
    local forward_pid=""
    local status=1

    mkdir -p "${dir}"
    if [[ ! -d "${repo}/.git" ]]; then
        rm -rf "${repo}"
        if ! git clone --depth 1 "${FAKEREDIS_PY_REPO:-https://github.com/cunla/fakeredis-py.git}" "${repo}" >"${logfile}" 2>&1; then
            row "fakeredis-py" "FAIL" "$(tail -n 1 "${logfile}")"
            return
        fi
    fi

    if [[ ! -f "${deps}/.mako-fakeredis-deps-installed" ]]; then
        rm -rf "${deps}"
        mkdir -p "${deps}"
        if ! "${PYTHON_BIN}" -m pip install --target "${deps}" redis sortedcontainers pytest pytest-asyncio pytest-timeout pytest-mock hypothesis valkey >"${logfile}" 2>&1; then
            row "fakeredis-py" "FAIL" "$(tail -n 1 "${logfile}")"
            return
        fi
        touch "${deps}/.mako-fakeredis-deps-installed"
    fi
    local fakeredis_version
    fakeredis_version="$(sed -n 's/^version = "\(.*\)"/\1/p' "${repo}/pyproject.toml" | head -n 1)"
    mkdir -p "${deps}/fakeredis-${fakeredis_version}.dist-info"
    printf 'Metadata-Version: 2.1\nName: fakeredis\nVersion: %s\n' "${fakeredis_version}" >"${deps}/fakeredis-${fakeredis_version}.dist-info/METADATA"
    sed -i 's/db=2/db=0/g' "${repo}/test/conftest.py"
    sed -i 's/rconn\.flushall()/_mako_flush(rconn)/g' "${repo}/test/conftest.py"
    if ! grep -q 'def _mako_flush' "${repo}/test/conftest.py"; then
        printf '\n\ndef _mako_flush(client):\n    keys = client.keys("*")\n    if keys:\n        client.delete(*keys)\n' >>"${repo}/test/conftest.py"
    fi

    if [[ "${MAKO_HOST}" != "127.0.0.1" || "${MAKO_PORT}" != "${listen_port}" ]]; then
        forward_pid="$(start_fakeredis_forwarder "${MAKO_HOST}" "${MAKO_PORT}" "${listen_port}")"
    fi

    for _ in $(seq 1 50); do
        if redis_ready "127.0.0.1" "${listen_port}"; then
            status=0
            break
        fi
        sleep 0.1
    done

    if [[ "${status}" -ne 0 ]]; then
        [[ -n "${forward_pid}" ]] && kill "${forward_pid}" >/dev/null 2>&1 || true
        row "fakeredis-py" "FAIL" "fakeredis real-server port 127.0.0.1:${listen_port} did not become reachable"
        return
    fi

    # Keep the default fakeredis slice to upstream real-server RESP2/RESP3 cases that
    # match the implemented G1 command surface and do not require FLUSHALL.
    local tests=(
        "test/test_mixins/test_string_commands.py::test_mget"
        "test/test_mixins/test_string_commands.py::test_mset"
        "test/test_mixins/test_string_commands.py::test_setnx"
        "test/test_mixins/test_string_commands.py::test_set_nx"
        "test/test_mixins/test_string_commands.py::test_append"
        "test/test_mixins/test_string_commands.py::test_strlen"
        "test/test_mixins/test_generic_commands.py::test_set_then_get"
        "test/test_mixins/test_generic_commands.py::test_exists"
        "test/test_mixins/test_generic_commands.py::test_delete"
    )

    status=0
    (
        cd "${repo}" &&
            PYTHONPATH="${deps}:${repo}:${PYTHONPATH:-}" "${PYTHON_BIN}" -m pytest -q -m real -k "${pytest_k}" "${tests[@]}"
    ) >"${logfile}" 2>&1 || status=$?

    [[ -n "${forward_pid}" ]] && kill "${forward_pid}" >/dev/null 2>&1 || true
    [[ -n "${forward_pid}" ]] && wait "${forward_pid}" >/dev/null 2>&1 || true

    if [[ "${status}" -eq 0 ]]; then
        row "fakeredis-py" "PASS" "$(tail -n 1 "${logfile}")"
    else
        row "fakeredis-py" "FAIL" "$(tail -n 1 "${logfile}")"
    fi
}

run_pytest
run_redis_cli
run_redis_py
run_node_clients
run_jedis
run_redis_rs
run_redis_exporter
run_fakeredis_py

echo "Wrote ${OUT_FILE}"
if [[ "${FAILURES}" -gt 0 ]]; then
    exit 1
fi
