#ifndef MAKO_ROCKSDB_RAFT_H
#define MAKO_ROCKSDB_RAFT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native Rust client; no Mako engine, C++ STO, or Masstree initialization. */
typedef struct MakoRocksClient MakoRocksClient;
typedef struct MakoRocksBuffer {
    uint8_t* data;
    size_t len;
} MakoRocksBuffer;

/* @unsafe - config_json references len readable bytes; out/error are writable.
 * Configuration is the native Rust ClientConfig JSON. On success, *out owns a
 * client and the return value is zero. On failure, the return value is -1 and
 * error contains a JSON wire::Response::Error. Free every returned buffer.
 * Output buffers must not contain an earlier unfreed allocation.
 */
int mako_rocksdb_raft_client_open(const uint8_t* config_json, size_t len,
                                MakoRocksClient** out, MakoRocksBuffer* error);

/* @unsafe - client is live; request_json references len readable bytes;
 * response is writable and contains no earlier unfreed allocation.
 * Input is wire::Envelope JSON: {"version":1,"request":{...}}. The request
 * has kind: snake_case; binary keys/values are arrays of bytes. A zero return
 * means a structured response was returned, NOT that a transaction committed.
 * Inspect its response/error kind: unknown_outcome is not an abort and requires
 * outcome lookup. Malformed ABI/JSON returns -1; transport/application errors
 * return zero with a typed error response.
 * Calls on one client are serialized. Closing a client concurrently is invalid.
 */
int mako_rocksdb_raft_request(MakoRocksClient* client,
                            const uint8_t* request_json, size_t len,
                            MakoRocksBuffer* response);

/* @unsafe - buffer is an unchanged returned allocation, freed exactly once. */
void mako_rocksdb_raft_buffer_free(MakoRocksBuffer buffer);

/* @unsafe - client is a live owned handle, closed exactly once without callers. */
void mako_rocksdb_raft_client_close(MakoRocksClient* client);

#ifdef __cplusplus
}
#endif
#endif
