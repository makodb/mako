"""Exercise the installed C ABI against a live replicated shard, without Mako headers."""
import json
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading


CONSUMER = r'''
#define _POSIX_C_SOURCE 200809L
#include "rocks_interface/rocksdb_raft.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* @unsafe - Native stdio/allocation and the explicitly owned Rust C ABI. */
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) return 3;
    long size = ftell(file);
    if (size <= 0 || size > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) return 4;
    uint8_t *config = malloc((size_t)size);
    if (!config || fread(config, 1, (size_t)size, file) != (size_t)size) return 5;
    fclose(file);
    MakoRocksClient *client = NULL;
    MakoRocksBuffer error = {0};
    int opened = mako_rocksdb_raft_client_open(config, (size_t)size, &client, &error);
    free(config);
    if (opened != 0 || !client) {
        if (error.data) fwrite(error.data, 1, error.len, stderr);
        mako_rocksdb_raft_buffer_free(error);
        return 6;
    }
    mako_rocksdb_raft_buffer_free(error);
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, stdin)) >= 0) {
        MakoRocksBuffer response = {0};
        int rc = mako_rocksdb_raft_request(client, (const uint8_t *)line,
                                          (size_t)length, &response);
        if (!response.data || (rc != 0 && rc != -1)) return 7;
        printf("{\"return_code\":%d,\"response\":", rc);
        fwrite(response.data, 1, response.len, stdout);
        fputs("}\n", stdout);
        fflush(stdout);
        mako_rocksdb_raft_buffer_free(response);
    }
    free(line);
    mako_rocksdb_raft_client_close(client);
    return 0;
}
'''


def run(binaries, client_config, group=1, table="alpha"):
    root = Path(__file__).resolve().parents[1]
    binaries = Path(binaries).resolve()
    compiler = shutil.which("cc")
    if compiler is None:
        raise RuntimeError("C compiler required to qualify the engine-neutral consumer")
    with tempfile.TemporaryDirectory(prefix="rocksdb-raft-c-client-") as directory:
        directory = Path(directory)
        source = directory / "consumer.c"
        executable = directory / "consumer"
        source.write_text(CONSUMER)
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(root / "src"), str(source), "-L", str(binaries),
                        "-lrocksdb_raft_service", f"-Wl,-rpath,{binaries}",
                        "-o", str(executable)], check=True)
        process = subprocess.Popen([str(executable), str(client_config)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=None, text=True, bufsize=1)
        responses = queue.Queue()

        def reader():
            for line in process.stdout:
                try:
                    value = json.loads(line)
                    if isinstance(value, dict) and "return_code" in value:
                        responses.put(value)
                except ValueError:
                    pass
            responses.put(None)

        thread = threading.Thread(target=reader, daemon=True)
        thread.start()

        def call(request, version=1, return_code=0, kind=None):
            process.stdin.write(json.dumps({"version": version, "request": request}) + "\n")
            process.stdin.flush()
            received = responses.get(timeout=30)
            assert received is not None, "C consumer exited before response"
            assert received["return_code"] == return_code, received
            response = received["response"]
            if kind is not None:
                assert response["kind"] == kind, response
            return response

        sequence = 0

        def begin():
            nonlocal sequence
            sequence += 1
            identity = {"client": 0x434142490001, "sequence": sequence}
            response = call({"kind": "begin", "group": group, "request": identity}, kind="begun")
            return response["handle"]

        def execute(handle, operation):
            return call({"kind": "execute", "handle": handle, "group": group,
                         "table": table, "operation": operation})

        key = list(b"a-c-abi")
        value = [0, 255, 1, 0, 128]
        try:
            rejected = call({"kind": "status", "group": group}, version=999,
                            return_code=-1, kind="error")
            assert rejected["error"]["code"] == "invalid", rejected
            handle = begin()
            assert execute(handle, {"op": "put", "key": key, "value": value})["kind"] == "ok"
            assert execute(handle, {"op": "get", "key": key}) == {"kind": "value", "value": value}
            call({"kind": "rollback", "handle": handle}, kind="ok")
            handle = begin()
            assert execute(handle, {"op": "get", "key": key}) == {"kind": "value", "value": None}
            assert execute(handle, {"op": "insert", "key": key, "value": value})["kind"] == "ok"
            committed = call({"kind": "commit", "handle": handle}, kind="committed")
            assert committed["position"]["index"] > 0 and not committed["read_only"], committed
            handle = begin()
            assert execute(handle, {"op": "get", "key": key}) == {"kind": "value", "value": value}
            call({"kind": "commit", "handle": handle}, kind="committed")
            handle = begin()
            assert execute(handle, {"op": "delete", "key": key})["kind"] == "ok"
            call({"kind": "commit", "handle": handle}, kind="committed")
            print("PASS compiled C consumer: version rejection, binary read-your-writes, rollback and commit", flush=True)
        finally:
            process.stdin.close()
            try:
                status = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
            process.stdout.close()
            thread.join(timeout=2)
            if status != 0:
                raise RuntimeError(f"C consumer exited {status}")
