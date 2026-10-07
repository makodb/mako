#ifndef _MAKO_TRANSACTION_FFI_H_
#define _MAKO_TRANSACTION_FFI_H_

#include <cstdint>
#include <cstddef>

/**
 * Transaction FFI Interface for Rust-C++ communication
 *
 * This header defines the data structures and functions for executing
 * batched transactions between the Rust Redis protocol layer and the
 * C++ Mako database layer.
 *
 * Flow:
 *   1. Client sends MULTI -> Rust starts buffering commands
 *   2. Client sends GET/SET commands -> Rust buffers them
 *   3. Client sends EXEC -> Rust calls cpp_execute_transaction()
 *   4. C++ executes all operations in a single database transaction
 *   5. C++ returns results for each operation
 *   6. Rust sends RESP array with all results to client
 *
 * Encoding boundary:
 *   Values stay encoded/decoded in the C++ Mako storage layer. Rust treats
 *   keys and values as opaque Redis bytes and uses value_present to distinguish
 *   an existing empty bulk string from a missing key.
 *
 * Reserved internal keys:
 *   Redis-visible keys must not use the 0x01 prefix. The Redis layer stores
 *   TTL metadata under "\x01TTL:<key>", set internals under "\x01S:" /
 *   "\x01S#:", list internals under "\x01L:" / "\x01L#:", hash internals under
 *   "\x01H:" / "\x01H#:" plus per-field expirations under "\x01HX:", and
 *   sorted-set internals under "\x01Z:" / "\x01ZS:" / "\x01Z#:", and stream
 *   internals under "\x01X#:" (one meta record per stream) / "\x01X:" (one per
 *   entry) / "\x01XG:" (one per consumer group) / "\x01XC:" (one per consumer)
 *   / "\x01XP:" (one per pending-entry-list entry). `X` is the family tag for
 *   streams; every family above reserves its own letter after the 0x01 byte.
 *   These records are hidden from Redis keyspace commands.
 *
 * Sorted-set score encoding:
 *   Sorted-set score indexes use order-preserving IEEE-754 double encoding:
 *   positives flip the sign bit, negatives flip all bits, then bytes are
 *   stored big-endian. NaN is rejected at command parse/execute time.
 */

#ifdef __cplusplus
extern "C" {
#endif

bool rust_init(size_t new_max);

/**
 * Operation codes for transaction operations
 */
typedef enum {
    TXN_OP_GET = 1,
    TXN_OP_SET = 2,
    TXN_OP_DELETE = 3,
    TXN_OP_DEL = TXN_OP_DELETE,
    TXN_OP_EXISTS = 4,
    TXN_OP_APPEND = 5,
    TXN_OP_STRLEN = 6,
    TXN_OP_INCRBY = 7,
    TXN_OP_INCRBYFLOAT = 8,
    TXN_OP_EXPIRE = 9,
    TXN_OP_TTL = 10,
    TXN_OP_PERSIST = 11,
    TXN_OP_SCAN = 12,
    TXN_OP_SADD = 13,
    TXN_OP_SREM = 14,
    TXN_OP_SISMEMBER = 15,
    TXN_OP_SCARD = 16,
    TXN_OP_SMEMBERS = 17,
    TXN_OP_SPOP = 18,
    TXN_OP_SRANDMEMBER = 19,
    TXN_OP_SMOVE = 20,
    TXN_OP_SET_ALGEBRA = 21,
    TXN_OP_TYPE = 22,
    TXN_OP_LPUSH = 23,
    TXN_OP_RPUSH = 24,
    TXN_OP_LPOP = 25,
    TXN_OP_RPOP = 26,
    TXN_OP_LLEN = 27,
    TXN_OP_LINDEX = 28,
    TXN_OP_LRANGE = 29,
    TXN_OP_LSET = 30,
    TXN_OP_LREM = 31,
    TXN_OP_LTRIM = 32,
    TXN_OP_LINSERT = 33,
    TXN_OP_LMOVE = 34,
    TXN_OP_LPOS = 35,
    TXN_OP_ZADD = 36,
    TXN_OP_ZSCORE = 37,
    TXN_OP_ZREM = 38,
    TXN_OP_ZCARD = 39,
    TXN_OP_ZRANGE = 40,
    TXN_OP_ZRANK = 41,
    TXN_OP_ZPOPMIN = 42,
    TXN_OP_ZCOUNT = 43,
    TXN_OP_ZSCAN = 44,
    TXN_OP_FLUSHDB = 45,
    TXN_OP_HSET = 46,
    TXN_OP_HGET = 47,
    TXN_OP_HMGET = 48,
    TXN_OP_HGETALL = 49,
    TXN_OP_HDEL = 50,
    TXN_OP_HEXISTS = 51,
    TXN_OP_HLEN = 52,
    TXN_OP_HKEYS = 53,
    TXN_OP_HVALS = 54,
    TXN_OP_HSTRLEN = 55,
    TXN_OP_HINCRBY = 56,
    TXN_OP_HINCRBYFLOAT = 57,
    TXN_OP_HSCAN = 58,
    TXN_OP_SETBIT = 59,
    TXN_OP_GETBIT = 60,
    TXN_OP_SETRANGE = 61,
    TXN_OP_GETRANGE = 62,
    TXN_OP_BPOP = 63,
    TXN_OP_RENAME = 64,
    TXN_OP_SORT = 65,
    TXN_OP_DUMP = 66,
    TXN_OP_RESTORE_LIST = 67,
    TXN_OP_ZRANGEBYLEX = 68,
    TXN_OP_ZLEXCOUNT = 69,
    TXN_OP_ZREMRANGEBYSCORE = 70,
    TXN_OP_ZREMRANGEBYRANK = 71,
    TXN_OP_ZREMRANGEBYLEX = 72,
    TXN_OP_ZRANGESTORE = 73,
    TXN_OP_ZSET_ALGEBRA = 74,
    TXN_OP_ZMPOP = 75,
    TXN_OP_ZRANDMEMBER = 76,
    TXN_OP_COPY = 77,
    // BITOP: key = destination, value = packed [AND|OR|XOR|NOT, src...].
    TXN_OP_BITOP = 78,
    // HyperLogLog. The sketch is a plain string value at the normal string
    // storage key, so TYPE/DEL/EXPIRE/DUMP/RESTORE/GET keep working on it.
    // HLL_ADD: key = target, value = packed [element ...] (may be empty);
    //   int_value = 1 when a register changed or the key was created.
    // HLL_COUNT: key = first key, value = packed [key ...] (all of them);
    //   read-only, int_value = the cardinality of the union.
    // HLL_MERGE: key = destination, value = packed [source key ...].
    TXN_OP_HLL_ADD = 79,
    TXN_OP_HLL_COUNT = 80,
    TXN_OP_HLL_MERGE = 81,
    // BITFIELD, read-modify-write form. An all-GET BITFIELD is read-only and
    // runs through the BITFIELD_RO path in Rust, so it never reaches this op.
    // key = the string key. value = a packed list (pack_bytes_list) holding
    // groups of four items, one group per subcommand, in command order:
    //   [0] kind     "GET" | "SET" | "INCRBY" | "OVERFLOW"
    //   [1] encoding canonical "i<bits>" (1..64) or "u<bits>" (1..63) for
    //                GET/SET/INCRBY; "WRAP" | "SAT" | "FAIL" for OVERFLOW
    //   [2] offset   absolute bit offset in decimal, with the "#<index>" form
    //                already multiplied out; empty string for OVERFLOW
    //   [3] value    decimal int64 value (SET) or increment (INCRBY); empty
    //                string for GET and OVERFLOW
    // OVERFLOW yields no reply item and applies to the subcommands after it
    // (WRAP until the first OVERFLOW). Rust validates every field, so a
    // malformed payload fails the transaction instead of erroring per Redis.
    // The result value is a packed list with one item per GET/SET/INCRBY in
    // command order: the decimal result, or an empty string for the nil an
    // OVERFLOW FAIL subcommand returns when it performed no write.
    TXN_OP_BITFIELD = 82,
    // Redis 7.4 hash field expiration (HEXPIRE/HTTL/HPERSIST families). A
    // field's expiration is a side key next to the field key, in the hidden
    // "\x01HX:" namespace, holding the absolute Unix millisecond time as
    // decimal text. All three ops touch only op.key, so they keep the default
    // single-key lock stripe. Rust validates syntax, conditions and time
    // conversion, so the executor only ever sees absolute milliseconds and a
    // well-formed field list.
    //
    // HFIELD_EXPIRE: key = the hash; expire_at_ms = the absolute Unix ms;
    //   value = packed [mode, field ...] where mode is one of
    //   "NONE" | "NX" | "XX" | "GT" | "LT". The result value is a packed list
    //   with one decimal code per field, in order:
    //     -2 no such field (also when the key does not exist)
    //      0 the NX/XX/GT/LT condition was not met
    //      1 the expiration was set or updated
    //      2 the time was already in the past, so the field was deleted
    // HFIELD_TTL: key = the hash; value = packed [field ...]. Read-only.
    //   The result value is a packed list with one decimal per field: -2 for
    //   no such field, -1 for a field with no expiration, otherwise the
    //   absolute Unix ms. Rust converts that to the remaining seconds or
    //   milliseconds (HTTL/HPTTL) or to absolute seconds or milliseconds
    //   (HEXPIRETIME/HPEXPIRETIME).
    // HFIELD_PERSIST: key = the hash; value = packed [field ...]. The result
    //   value is a packed list of decimal codes: -2 no such field, -1 no
    //   expiration to remove, 1 removed.
    TXN_OP_HFIELD_EXPIRE = 83,
    TXN_OP_HFIELD_TTL = 84,
    TXN_OP_HFIELD_PERSIST = 85,
    // MOVE key db. The two keys are two logical-database spellings of the same
    // Redis-visible name (see "Logical databases" below), so this is a copy of
    // the whole object followed by deletion of the source, in one transaction.
    // key = the source key, already carrying the connection's database prefix;
    // value = packed [destination key], already carrying the target database's
    // prefix. int_value is 1 when the object moved and 0 when the source does
    // not exist or the destination name is already taken. It touches a second
    // key, so it is in redis_op_uses_only_primary_lock_key and adds the packed
    // destination in redis_request_lock_stripes.
    TXN_OP_MOVE = 86,
    // Redis Streams. A stream is an ordered composite-key family, laid out so
    // that one prefix range scan of storage walks the entries in ID order:
    //
    //   meta      "\x01X#:"  + u64le(len(key)) + key
    //             -> length, last-generated-id, recorded-first-entry-id,
    //                entries-added, max-deleted-entry-id, group count, as nine
    //                little-endian 64-bit fields.
    //   entry     "\x01X:"   + u64le(len(key)) + key + be64(ms) + be64(seq)
    //             -> pack_bytes_list([field, value, ...]).
    //   group     "\x01XG:"  + u64le(len(key)) + key
    //                        + u64le(len(group)) + group
    //             -> last-delivered-id, entries-read (-1 when it cannot be
    //                known, Redis's SCG_INVALID_ENTRIES_READ), consumer count,
    //                PEL count.
    //   consumer  "\x01XC:"  + <stream> + <group> + consumer
    //             -> seen-time, active-time (-1 until an entry is handed over),
    //                pending count.
    //   PEL       "\x01XP:"  + <stream> + <group> + be64(ms) + be64(seq)
    //             -> delivery-time, delivery-count, owning consumer name. A
    //                per-consumer PEL view is this list filtered by owner.
    //
    // The 128-bit IDs are written big-endian and fixed-width, so lexicographic
    // key order is ID order: an XRANGE start/end pair becomes a key range and
    // COUNT stops the walk instead of filtering afterwards. A stream exists for
    // as long as its meta record does, which is what lets XADD MAXLEN 0 and
    // XGROUP CREATE MKSTREAM leave an empty but existing stream, as Redis does.
    //
    // Every ID crosses this interface as decimal "<ms>-<seq>" text, both halves
    // unsigned 64-bit as in Redis; the executor parses and formats them, so no
    // ID has to be squeezed into the signed int64 of a TxnOpResult. Rust has
    // already expanded "-", "+", the bare "<ms>" forms, the "(" exclusive
    // markers and "$", so the executor only ever sees both halves written out.
    //
    // Payloads are pack_bytes_list of decimal/text items, per op:
    //
    // XADD: key = the stream. value = packed
    //   [id spec ("*", "<ms>-*" or "<ms>-<seq>"), NOMKSTREAM ("0"/"1"),
    //    trim strategy ("" | "MAXLEN" | "MINID"), trim threshold, trim LIMIT
    //    ("0" for none), field, value, ...]. The insert and the trim are one op
    //   because a request's op list is built before the executor runs it.
    //   On success int_value is 0 and the data is the ID that was assigned.
    // XRANGE (also XREVRANGE and each stream of an XREAD): value = packed
    //   [start id, end id, COUNT ("0" for all), reverse ("0"/"1")]. Read-only.
    //   The result data is packed [last-generated-id, id, fields, id, fields...]
    //   where each `fields` is the entry's own packed [field, value, ...] blob
    //   copied out verbatim. Item 0 is what a blocking XREAD resolves "$" and
    //   "+" to when the first attempt found nothing. int_value is 1 when the
    //   stream exists and 0 when it does not.
    // XLEN: int_value is the length. Read-only.
    // XDEL: value = packed [id, ...]; int_value is the number removed.
    // XTRIM: value = packed [strategy, threshold, LIMIT]; int_value is the
    //   number removed.
    // XSETID: value = packed [last id, ENTRIESADDED ("" when absent),
    //   MAXDELETEDID ("" when absent)].
    // XINFO: value = packed ["STREAM", FULL ("0"/"1"), COUNT] for XINFO STREAM,
    //   ["GROUPS"] or ["CONSUMERS", group] for the other two. Read-only. The
    //   result data is a flat packed list Rust walks with a cursor; the shapes
    //   are described where they are built in makoCon.cc.
    // XRESTORE: value = packed [family letter, key suffix, value, ...] as DUMP
    //   produced it under the "MAKO_STREAM_DUMP" magic, so a restore brings
    //   back the entries and the consumer groups with their PELs.
    TXN_OP_XADD = 87,
    TXN_OP_XRANGE = 88,
    TXN_OP_XLEN = 89,
    TXN_OP_XDEL = 90,
    TXN_OP_XTRIM = 91,
    TXN_OP_XSETID = 92,
    TXN_OP_XINFO = 93,
    TXN_OP_XRESTORE = 94,
    // Consumer groups. Each is one atomic op, because each is a read of the
    // group record followed by a write that depends on it.
    //
    // XGROUP: value = packed [subcommand, group, ...]:
    //   ["CREATE", group, id or "$", MKSTREAM ("0"/"1"), ENTRIESREAD ("" when
    //    absent or -1)], ["SETID", group, id or "$", ENTRIESREAD],
    //   ["DESTROY", group], ["CREATECONSUMER", group, consumer],
    //   ["DELCONSUMER", group, consumer]. int_value is the reply (0/1, or the
    //   pending count DELCONSUMER removed) or one of the sentinels below.
    // XREADGROUP: one op per stream, as XREAD is. value = packed
    //   [group, consumer, mode, start id, COUNT ("0" for all),
    //    NOACK ("0"/"1")] where mode is "NEW" (the ">" ID: hand over entries
    //   after the group's last-delivered ID and record them in the PEL),
    //   "HISTORY" (replay this consumer's pending entries from `start id`) or
    //   "NONE" (nothing to replay, but still a reply for this stream). The
    //   result data is packed [id, fields, ...]; an empty `fields` is a
    //   pending entry whose stream entry has been deleted, which Redis answers
    //   with a null field list.
    // XACK: value = packed [group, id, ...]; int_value is the number removed
    //   from the pending-entry list.
    // XPENDING: value = packed [group, "SUMMARY"] or [group, "RANGE",
    //   min-idle, start, end, COUNT, consumer ("" for every consumer)].
    //   Read-only. The summary result is packed [count, smallest id, greatest
    //   id, consumer count, (name, count)...]; the range result is packed
    //   [row count, (id, consumer, idle, delivery count)...].
    // XCLAIM: value = packed [group, consumer, min-idle, JUSTID ("0"/"1"),
    //   FORCE ("0"/"1"), IDLE (""), TIME (""), RETRYCOUNT (""), LASTID (""),
    //   id, ...] -- the five option slots carry "" when the option was absent.
    // XAUTOCLAIM: value = packed [group, consumer, min-idle, start, COUNT,
    //   JUSTID]. The result is packed [next cursor, claimed count,
    //   (id, fields)..., deleted count, id...]; XCLAIM's result is the same
    //   without the cursor and the deleted list. `fields` is empty under
    //   JUSTID, which asks for the IDs alone.
    TXN_OP_XGROUP = 95,
    TXN_OP_XREADGROUP = 96,
    TXN_OP_XACK = 97,
    TXN_OP_XPENDING = 98,
    TXN_OP_XCLAIM = 99,
    TXN_OP_XAUTOCLAIM = 100,
} TxnOpCode;

/**
 * Logical databases (Redis SELECT / MOVE / COPY ... DB).
 *
 * Database 0 keys are stored under exactly the bytes the client sent, so the
 * on-disk layout of every keyspace that existed before logical databases is
 * unchanged. A key in database 1..15 is stored under
 *
 *     0x02 <db as one byte> ':' <key>
 *
 * where the prefix is applied to the Redis-visible key before any storage
 * prefix ("table_key_", the hidden 0x01 collection namespaces, the "\x01TTL:"
 * metadata) is added. Every type, TTL, WATCH, lock-stripe and DUMP/RESTORE
 * mechanism therefore works per database without knowing databases exist. Rust
 * rejects a user key whose first byte is 0x02 exactly as it rejects 0x01, so
 * database 0 cannot spell a database-n key.
 *
 * Two ops have to know about the layout, and both learn it from the prefix
 * bytes in val_ptr rather than a flag (all 32 flag bits are taken):
 *
 *   TXN_OP_SCAN (KEYS, SCAN, DBSIZE, RANDOMKEY): the val payload is the
 *     user-key prefix to walk. A prefix whose first byte is 0x02 selects one
 *     logical database and the scan returns its keys verbatim (Rust strips the
 *     three prefix bytes before replying). Any other prefix, including the
 *     empty one, walks database 0, and the scan then skips every key whose
 *     first byte is 0x02 so database 0 never sees another database's keys.
 *
 *   TXN_OP_FLUSHDB: the val payload names what to clear.
 *       empty              FLUSHALL: every key in the table, as before.
 *       0x02               FLUSHDB on database 0: every key that does not
 *                          belong to databases 1..15.
 *       0x02 <db> ':'      FLUSHDB on database <db>: only that database.
 *     The one-byte 0x02 sentinel is not a legal database prefix (those are
 *     always three bytes), which is what makes "database 0" distinguishable
 *     from "everything".
 */

/**
 * HyperLogLog result convention: when an HLL op fails because one of its keys
 * holds a string that is not a valid sketch (bad magic or wrong length), the
 * op reports success=false with int_value set to this sentinel so Rust can
 * emit the Redis "not a valid HyperLogLog string value" error instead of the
 * generic WRONGTYPE text. Any other success=false means plain WRONGTYPE.
 */
#define TXN_HLL_ERR_NOT_HLL (-2)

/**
 * INCR-family result convention (TXN_OP_INCRBY and TXN_OP_INCRBYFLOAT, which
 * carry INCR, INCRBY, DECR, DECRBY and INCRBYFLOAT). A failed increment is a
 * command error, not a transaction error: the op reports success=false and
 * puts one of these sentinels in int_value so Rust can emit the exact Redis
 * text instead of failing the whole transaction (which reaches the client as
 * "ERR backend" after the retry loop). int_value 0, the default, means the key
 * holds a list, set, hash or zset and the reply is plain WRONGTYPE.
 */
#define TXN_INCR_ERR_NOT_INTEGER (-1)  /* value is not an integer or out of range */
#define TXN_INCR_ERR_OVERFLOW (-2)     /* increment or decrement would overflow */
#define TXN_INCR_ERR_NOT_FLOAT (-3)    /* value is not a valid float */
#define TXN_INCR_ERR_NAN_OR_INF (-4)   /* increment would produce NaN or Infinity */

/**
 * Stream result convention. A stream op that fails because of a command-level
 * condition rather than a storage failure reports success=true with one of
 * these sentinels in int_value and no value, so Rust can write Redis's exact
 * error (or, for NOMKSTREAM, a null) instead of failing the whole transaction.
 * success=false still means plain WRONGTYPE.
 */
#define TXN_STREAM_ERR_NOMKSTREAM (-1)          /* XADD NOMKSTREAM, no such key -> null */
#define TXN_STREAM_ERR_SMALLER_ID (-2)          /* "The ID specified in XADD is equal or smaller than the target stream top item" */
#define TXN_STREAM_ERR_NO_SUCH_KEY (-3)         /* "no such key" */
#define TXN_STREAM_ERR_SETID_SMALLER (-4)       /* "The ID specified in XSETID is smaller than the target stream top item" */
#define TXN_STREAM_ERR_SETID_ENTRIES_ADDED (-5) /* "The entries_added specified in XSETID is smaller than the target stream length" */
#define TXN_STREAM_ERR_SETID_TOMBSTONE (-6)     /* "The ID specified in XSETID is smaller than the provided max_deleted_entry_id" */
#define TXN_STREAM_ERR_NOGROUP (-7)             /* no such key or no such consumer group -> NOGROUP */
#define TXN_STREAM_ERR_BUSYGROUP (-8)           /* "BUSYGROUP Consumer Group name already exists" */
#define TXN_STREAM_ERR_NO_KEY_FOR_GROUP (-9)    /* XGROUP CREATE without MKSTREAM on a missing key */

typedef enum {
    TXN_FLAG_NONE = 0,
    TXN_FLAG_SET_NX = 1u << 0,
    TXN_FLAG_SET_XX = 1u << 1,
    TXN_FLAG_SET_RETURN_OLD = 1u << 2,
    TXN_FLAG_SET_INTEGER_REPLY = 1u << 3,
    TXN_FLAG_SET_REQUIRE_ABSENT_GROUP = 1u << 4,
    TXN_FLAG_SET_KEEP_TTL = 1u << 5,
    TXN_FLAG_TTL_MILLISECONDS = 1u << 6,
    TXN_FLAG_EXPIRE_NX = 1u << 7,
    TXN_FLAG_EXPIRE_XX = 1u << 8,
    TXN_FLAG_EXPIRE_GT = 1u << 9,
    TXN_FLAG_EXPIRE_LT = 1u << 10,
    TXN_FLAG_SCAN_COUNT_ONLY = 1u << 11,
    TXN_FLAG_SET_COUNT_GIVEN = 1u << 12,
    TXN_FLAG_SET_ALLOW_DUPLICATES = 1u << 13,
    TXN_FLAG_SET_ALGEBRA_UNION = 1u << 14,
    TXN_FLAG_SET_ALGEBRA_DIFF = 1u << 15,
    TXN_FLAG_SET_ALGEBRA_STORE = 1u << 16,
    TXN_FLAG_LIST_PUSH_IF_EXISTS = 1u << 17,
    TXN_FLAG_LIST_INSERT_BEFORE = 1u << 18,
    TXN_FLAG_LIST_SOURCE_LEFT = 1u << 19,
    TXN_FLAG_LIST_DEST_LEFT = 1u << 20,
    TXN_FLAG_LIST_COUNT_GIVEN = 1u << 21,
    TXN_FLAG_ZADD_NX = 1u << 22,
    TXN_FLAG_ZADD_XX = 1u << 23,
    TXN_FLAG_ZADD_CH = 1u << 24,
    TXN_FLAG_ZADD_INCR = 1u << 25,
    TXN_FLAG_ZADD_GT = 1u << 26,
    TXN_FLAG_ZADD_LT = 1u << 27,
    TXN_FLAG_Z_WITHSCORES = 1u << 28,
    TXN_FLAG_Z_REV = 1u << 29,
    TXN_FLAG_Z_BYSCORE = 1u << 30,
    TXN_FLAG_Z_COUNT_GIVEN = 1u << 31,
} TxnOpFlags;

/**
 * Single operation within a transaction request
 *
 * Memory layout is flat for easy FFI serialization:
 *   - op: operation code (GET=1, SET=2)
 *   - key_ptr/key_len: pointer to key bytes
 *   - val_ptr/val_len: pointer to value bytes (NULL for GET)
 *   - flags: command-specific flags; currently used by SET variants
 *   - expire_at_ms: absolute Unix millisecond expiry for SET/EXPIRE, or -1
 *     for no expiry metadata
 *   - group_id: non-zero when ops belong to one all-or-nothing group, such as MSETNX
 *
 * For TXN_OP_SCAN:
 *   - key_ptr/key_len carries the decoded cursor user key, or empty for cursor 0
 *   - val_ptr/val_len carries the literal scan prefix derived from MATCH, with
 *     the selected database's prefix in front of it (see "Logical databases")
 *   - expire_at_ms carries the COUNT work hint
 *   - TXN_FLAG_SCAN_COUNT_ONLY returns DBSIZE in int_value instead of key bytes
 *
 * For set operations:
 *   - SADD/SREM val_ptr carries a length-prefixed list of members
 *   - SISMEMBER val_ptr carries one member
 *   - SMOVE key carries the source set; val_ptr carries [destination, member]
 *   - SPOP/SRANDMEMBER expire_at_ms carries the requested count
 *   - SET_ALGEBRA key carries the destination for *STORE or the first source
 *     for non-store; val_ptr carries source set names
 *
 * For list operations:
 *   - LPUSH/RPUSH val_ptr carries a length-prefixed list of elements
 *   - LPOP/RPOP expire_at_ms carries count; LIST_COUNT_GIVEN controls
 *     bulk-vs-array response formatting in Rust
 *   - LINDEX expire_at_ms carries the signed list index
 *   - LRANGE/LTRIM val_ptr carries [start, stop] as byte strings
 *   - LSET/LREM/LINSERT val_ptr carries command-specific byte lists
 *   - LMOVE key carries source; val_ptr carries [destination]
 *   - The C++ executor stages list contents per transaction and flushes dirty
 *     lists once before commit.
 *
 * For sorted-set operations:
 *   - ZADD val_ptr carries [score, member, ...]
 *   - ZSCORE/ZRANK val_ptr carries one member
 *   - ZREM val_ptr carries members
 *   - ZRANGE val_ptr carries [start, stop] or [min, max, offset, count]
 *   - ZPOPMIN/ZPOPMAX expire_at_ms carries count; Z_COUNT_GIVEN controls
 *     whether the client supplied count
 */
typedef struct {
    uint32_t op;           // TxnOpCode
    const uint8_t* key_ptr;
    size_t key_len;
    const uint8_t* val_ptr;  // NULL for GET operations
    size_t val_len;          // 0 for GET operations
    uint32_t flags;          // TxnOpFlags
    int64_t expire_at_ms;    // -1 when no TTL metadata should be written
    uint32_t group_id;       // 0 for no command group
} TxnOperation;

/**
 * Transaction request: array of operations to execute atomically
 */
typedef struct {
    size_t num_ops;           // Number of operations
    const TxnOperation* ops;  // Array of operations
} TxnRequest;

/**
 * Result for a single operation
 *
 * For GET:
 *   - success=true, value_present=true: hit, data contains value bytes
 *     (data_len may be 0 for an empty Redis bulk string)
 *   - success=true, value_present=false: miss (key not found)
 * For SET:
 *   - success=true: write succeeded
 *   - success=false: write failed (conflict, etc.)
 * For DELETE / EXISTS:
 *   - success=true, value_present=true: key existed
 *   - success=true, value_present=false: key did not exist
 * For integer-returning operations, including TTL-family commands:
 *   - success=true, int_value carries the Redis integer reply
 */
typedef struct {
    bool success;
    bool value_present;
    uint8_t* data_ptr;   // malloc'd buffer for GET results, NULL for SET
    size_t data_len;
    int64_t int_value;
} TxnOpResult;

/**
 * Transaction response: results for all operations
 *
 * If transaction_success is false, the entire transaction was aborted
 * and individual results may not be meaningful.
 */
typedef struct {
    bool transaction_success;  // True if transaction committed
    size_t num_results;
    TxnOpResult* results;      // Array of results (malloc'd by C++)
} TxnResponse;

/**
 * Result status for the allocation-light plain GET/SET executor.
 * FALLBACK asks Rust to use cpp_execute_transaction() for semantics that need
 * the full collection-aware executor (currently SET of a non-string key).
 */
typedef enum {
    FAST_MAKO_ABORTED = 0,
    FAST_MAKO_GET_MISS = 1,
    FAST_MAKO_GET_HIT = 2,
    FAST_MAKO_SET_OK = 3,
    FAST_MAKO_FALLBACK = 4,
} FastMakoStatus;

/**
 * Borrowed fast-path result. GET data remains valid on the calling thread
 * until the next cpp_execute_fast_mako_string() call or worker cleanup.
 */
typedef struct {
    FastMakoStatus status;
    const uint8_t* data_ptr;
    size_t data_len;
} FastMakoStringResult;

/**
 * Lightweight server metrics exposed to the Redis INFO formatter.
 */
typedef struct {
    uint64_t txn_commits;
    uint64_t txn_aborts;
    uint64_t txn_retries;
    uint64_t uptime_seconds;
    uint64_t cache_enabled;
    uint64_t cache_capacity_bytes;
    uint64_t cache_entries;
    uint64_t cache_bytes;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_inserts;
    uint64_t cache_evictions;
    uint64_t cache_invalidations;
} MakoMetrics;

/**
 * Execute one plain GET or unconditional, no-TTL SET without constructing the
 * generic transaction executor's collection state or heap-owned response.
 * Existing strings use the direct SET path; missing/non-string keys fall back
 * so Redis collection replacement semantics stay centralized.
 */
bool cpp_execute_fast_mako_string(
    uint32_t op,
    const uint8_t* key_ptr,
    size_t key_len,
    const uint8_t* val_ptr,
    size_t val_len,
    FastMakoStringResult* result);

/**
 * Execute a batch of operations as a single database transaction
 *
 * @param request  Pointer to transaction request
 * @param response Pointer to response struct (C++ fills this in)
 * @return true if the call succeeded (check response->transaction_success for commit status)
 *
 * Rust is responsible for:
 *   - Allocating and filling TxnRequest
 *   - Calling cpp_free_transaction_response() after processing results
 *
 * C++ is responsible for:
 *   - Executing all operations in a single DB transaction
 *   - Allocating response->results array
 *   - Allocating data_ptr buffers for GET results
 */
bool cpp_execute_transaction(const TxnRequest* request, TxnResponse* response);

/**
 * Free response resources allocated by cpp_execute_transaction
 */
void cpp_free_transaction_response(TxnResponse* response);

/**
 * Interactive (resumable) transactions.
 *
 * cpp_execute_transaction builds the whole operation list before it is called,
 * so an operation cannot depend on the result of an earlier one in the same
 * transaction. A session inverts that: it opens one storage transaction, hands
 * back an opaque handle, and lets the caller run op lists inside it as many
 * times as it likes before committing. Everything the executor buffers in the
 * session -- the deferred deletes, the buffered writes, the staged collections
 * and the exists/value caches -- is visible to the next call, so a Lua script's
 *
 *     redis.call('set', k, v)   then   redis.call('get', k)
 *
 * reads back v without either operation reaching storage. That read-your-writes
 * behaviour is the whole point; it is the same mechanism a MULTI/EXEC batch
 * already relies on, exposed one call at a time.
 *
 * Usage:
 *     void* s = cpp_txn_begin(keys, key_lens, num_keys);
 *     if (!s) -> could not start, the caller reports an error
 *     while (...) if (!cpp_txn_execute(s, &request, &response)) { cpp_txn_abort(s); ... }
 *     bool ok = cpp_txn_commit(s);   // false = OCC abort, the caller retries
 *
 * Exactly one of cpp_txn_commit / cpp_txn_abort must be called, and it frees
 * the session. Every call for one session must happen on the thread that
 * called cpp_txn_begin: the storage transaction lives in thread-local STO
 * state. Sessions carry a magic word so a stale or foreign handle is refused
 * rather than dereferenced.
 *
 * Locking. The batch path takes the key-stripe lock of every key in the
 * request up front, which is what serializes concurrent writers to the same
 * key. A session can only lock what the caller declares in cpp_txn_begin: for
 * EVAL that is the KEYS array. A key the session touches without declaring it
 * is not stripe-locked, so two sessions writing the same undeclared key are
 * serialized only by STO's optimistic concurrency -- one of them fails at
 * commit and the caller has to retry the whole unit of work (which is what the
 * Lua path does: an OCC abort re-runs the script from scratch). Declaring the
 * keys a script writes is therefore what turns retry-on-conflict into
 * wait-for-the-lock, and undeclared keys stay correct but can livelock a
 * pathological workload.
 */

/**
 * Open a session. keys/key_lens describe num_keys Redis-visible (already
 * database-prefixed) keys whose stripes are locked for the life of the
 * session, in stripe order so this cannot deadlock against the batch path.
 * Returns an opaque session handle, or NULL if no transaction could be
 * started.
 */
void* cpp_txn_begin(const uint8_t* const* keys, const size_t* key_lens, size_t num_keys);

/**
 * Run one operation list inside an open session. The response is filled and
 * owned exactly as cpp_execute_transaction fills it, and must be released with
 * cpp_free_transaction_response. Per-operation success flags mean what they
 * always mean (false is usually WRONGTYPE for a typed op).
 *
 * Returns false only on an internal failure: a bad handle, a storage error, an
 * STO abort, or a write attempted in a session on a follower. After a false
 * return the session can no longer commit; the caller must abort it.
 */
bool cpp_txn_execute(void* session, const TxnRequest* request, TxnResponse* response);

/**
 * Flush and commit the session, then free it. Returns whether the storage
 * commit succeeded; false means an OCC abort and nothing the session did is
 * visible.
 */
bool cpp_txn_commit(void* session);

/**
 * Roll the session back and free it. Nothing it did becomes visible.
 */
void cpp_txn_abort(void* session);

/**
 * Fill metrics for INFO server / INFO mako.
 */
bool cpp_get_metrics(MakoMetrics* metrics);

/**
 * Record one Redis-layer retry attempt. The retry loop lives in Rust, while
 * INFO mako metrics live in the C++ executor.
 */
void cpp_record_txn_retry(void);

#ifdef __cplusplus
}
#endif

#endif // _MAKO_TRANSACTION_FFI_H_
