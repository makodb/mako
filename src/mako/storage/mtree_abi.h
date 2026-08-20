/* mtree_abi.h - a pure C ABI over concurrent_btree (mbtree<masstree_params>).
 *
 * Exists so a Rust cache can use masstree as an ordered directory without
 * knowing that masstree is C++, and without ever touching RCU.
 *
 * ===========================================================================
 * THE INVARIANT THIS ABI EXISTS TO ENFORCE
 * ===========================================================================
 *
 *   No mtx_* function ever returns -- or passes to a callback -- a pointer
 *   into RCU-managed or tree-managed memory. Every out-value is a scalar, an
 *   opaque word, or bytes copied into caller-provided storage.
 *
 * Consequently every call is SELF-CONTAINED: it opens an RCU epoch, does its
 * work, and closes the epoch before returning, and nothing it returns depends
 * on that epoch still being held. Callers therefore never see RCU at all.
 *
 * Why an epoch is needed in the first place: this repo builds masstree with
 * masstree_params::RcuRespCaller = true (masstree_btree.h:175), which compiles
 * mbtree's own internal rcu_region down to an EMPTY CLASS. The header states
 * the contract at line 267: "the public interface assumes that the caller has
 * taken care of setting up RCU". Silo does this because a transaction spans
 * many tree operations. That is an implementation detail of the C++ side and
 * it stays on the C++ side.
 *
 * ===========================================================================
 * WHY THERE ARE NO CALLBACKS HERE
 * ===========================================================================
 *
 * scoped_rcu_region's constructor takes a PER-CORE SPINLOCK and holds it for
 * the whole epoch, and the ticker daemon must acquire that same spinlock to
 * advance every lagging core. So one thread holding an epoch across IO or a
 * blocking call freezes epoch advancement -- and therefore ALL reclamation --
 * PROCESS-WIDE, not locally. The C++ cache shipped four instances of this and
 * had to fix every one.
 *
 * A scan callback would let arbitrary caller code run inside that window, so
 * the range functions here FILL A CALLER BUFFER instead and copy keys out.
 * This is a structural guarantee, not a documented rule.
 *
 * ===========================================================================
 * THE STORED WORD
 * ===========================================================================
 *
 * The tree is an IMMUTABLE key -> word directory: the word is written once at
 * insert and never rewritten. All value mutation belongs to the caller,
 * behind the word. There is deliberately no update-in-place primitive
 * (masstree's leaf value slot has no atomic-CAS API; rewriting it needs a
 * locked cursor).
 *
 * The ABI never interprets the word, so what it means is entirely the
 * caller's business. MTX_WORD_NULL is reserved to mean "absent" so that
 * mtx_get can report absence in the out-value rather than through a separate
 * channel.
 *
 * ===========================================================================
 * THREADS ARE A PERMANENT, CAPPED RESOURCE
 * ===========================================================================
 *
 * Core IDs are allocated per thread, NEVER recycled, capped at NMAXCORES
 * (512) PER RUNTIME, and a dead thread's deferred-free queue is never
 * drained. So a caller must use a fixed set of long-lived threads, created
 * once and joined at shutdown: no thread pools, no per-request spawn, no
 * per-test-case thread churn against a real tree.
 *
 * The lazy path (coreid::core_id()) ends in ALWAYS_ASSERT, i.e. abort(), when
 * the space is exhausted. mtx_thread_attach() exists to turn that abort into
 * a reportable status, and must be called once per thread before any other
 * mtx_* call from that thread.
 */

#ifndef MAKO_MTREE_ABI_H
#define MAKO_MTREE_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change. The caller should compare this against
 * the value it was compiled with, and check mtx_kv_size() too: a struct
 * layout drift is the failure mode a version number alone will not catch. */
#define MTX_ABI_VERSION 4u

/* Status codes. */
#define MTX_OK 0
#define MTX_ERR_NO_CORE_ID 1     /* core-ID space exhausted (512/runtime)   */
#define MTX_ERR_NOT_ATTACHED 2   /* mtx_thread_attach() not called here     */
#define MTX_ERR_WRONG_RUNTIME 3  /* thread is bound to a non-global runtime */
#define MTX_ERR_INVALID 4        /* bad argument                            */
#define MTX_ERR_NO_SPACE 5       /* caller buffer or arena too small        */
#define MTX_ERR_INTERNAL 6       /* a C++ exception was contained here      */

/* Reserved: never a valid stored word. mtx_get reports absence with it. */
#define MTX_WORD_NULL 0ull

typedef struct mtx_tree mtx_tree;

/* One key/word pair in a range result. Keys live in the caller's arena;
 * key_off is a byte offset into it. */
typedef struct {
  uint32_t key_off;
  uint32_t key_len;
  uint64_t word;
} mtx_kv;

/* --- identity, for compile-time-vs-runtime agreement checks ------------- */
uint32_t mtx_abi_version(void);
size_t mtx_kv_size(void);

/* --- per-thread registration ------------------------------------------- */

/* Register the calling thread. Idempotent. Must be called once per thread
 * before any other mtx_* call from that thread.
 *
 * Returns MTX_ERR_NO_CORE_ID rather than aborting when the per-runtime core
 * ID space is exhausted, and MTX_ERR_WRONG_RUNTIME if the calling thread is
 * bound to a SiloRuntime other than the global default -- masstree's
 * threadinfo hardcodes the global rcu instance regardless of which runtime
 * allocated the core ID, so the mismatch must surface at attach time rather
 * than later as a corrupted traversal. */
int mtx_thread_attach(void);

/* --- lifecycle ---------------------------------------------------------- */
mtx_tree *mtx_create(void);
void mtx_destroy(mtx_tree *t);

/* --- point operations --------------------------------------------------- */

/* Look up a key. *out receives the stored word, or MTX_WORD_NULL if absent.
 * Returns MTX_OK even when absent; a non-OK status means the lookup itself
 * failed. */
int mtx_get(mtx_tree *t, const char *key, size_t klen, uint64_t *out);

/* Install `word` if the key is absent, and report the word now associated
 * with the key -- `word` itself if this call installed it, or the winner's
 * word if another thread got there first.
 *
 * Folding "insert if absent" and "read whoever won" into one call is what
 * lets a caller allocate an entry, try to publish it, and discover it lost,
 * without a second round trip and without a window where the key exists but
 * the caller cannot see which word is live. */
int mtx_get_or_insert(mtx_tree *t, const char *key, size_t klen, uint64_t word,
                      uint64_t *out);

/* As mtx_get_or_insert, but WITHOUT the leading probe.
 *
 * For a caller that has already established the key is absent -- it has just
 * done its own lookup and missed. mtx_get_or_insert's probe would re-walk the
 * tree to re-discover that miss, making the insert path cost three traversals
 * instead of two.
 *
 * Semantics are identical: *out receives whichever word is associated with the
 * key on return, whether this call installed it or a racing writer did. Use
 * mtx_get_or_insert when the key is likely to exist; use this when you already
 * know it does not. */
int mtx_insert_if_absent(mtx_tree *t, const char *key, size_t klen,
                         uint64_t word, uint64_t *out);

/* --- region pinning ------------------------------------------------------ */

/* Hold ONE RCU region across several calls on this thread.
 *
 * Every mtx_* call opens its own region. The FIRST region on a thread is not
 * free: it takes this core's ticker spinlock and reads the clock. Nested ones
 * skip both. So a caller making two or three tree calls in a row pays that
 * setup two or three times unless it pins first.
 *
 * Measured at 16 threads: the masstree rung alone cost 50.7 ns/op called
 * inline from C++ and 76.1 ns/op called through this ABI, and the difference
 * is this. At one thread it is under 2 ns -- the cost is contention on the
 * per-core ticker state, so it only appears under load.
 *
 * Reentrant: pins nest, and only the outermost creates or destroys the region.
 * Unbalanced unpins are ignored rather than corrupting the depth.
 *
 * ===========================================================================
 * DO NOT HOLD A PIN ACROSS IO, A BLOCKING CALL, OR ARBITRARY CALLER CODE
 * ===========================================================================
 *
 * An open region pins this core's ticker slot, and the ticker daemon must take
 * that slot to advance every lagging core. Holding one across a disk read
 * stalls epoch advancement -- and therefore ALL reclamation -- process-wide,
 * not just for the pinning thread. That is the one hazard this ABI otherwise
 * removes by opening and closing a region inside each call, and pinning hands
 * it back to the caller for the duration.
 *
 * The intended use is narrow: a lookup and an insert that must happen back to
 * back, with nothing between them but a memory allocation. */
int mtx_region_pin(void);
void mtx_region_unpin(void);

/* --- range operations --------------------------------------------------- */

/* Visit up to `cap` keys at or after `from`, ascending, copying each key into
 * `arena`. On return *n_out holds how many entries were written to `out`.
 *
 * Stops early -- without error -- when either `cap` or the arena is
 * exhausted, so a caller resumes by re-calling with `from` set just past the
 * last key returned. A short result therefore means "chunk boundary", not
 * "end of range".
 *
 * *arena_used DISCRIMINATES THE TWO REASONS FOR STOPPING, and a caller that
 * ignores it truncates every scan whose keys happen to be long:
 *
 *   *arena_used <= arena_cap : bytes consumed. The walk ended because the
 *                              range ended or `cap` was reached.
 *   *arena_used >  arena_cap : the walk stopped for want of arena space, and
 *                              this is the size one more key would have
 *                              needed. Grow to at least this and re-call
 *                              with the SAME `from`; the result so far is
 *                              still valid and may be kept or discarded.
 *
 * Without this the two are indistinguishable: a 400-byte key against a
 * 4 KiB arena returns ten entries out of a requested sixty-four, which
 * reads exactly like end-of-range. (Found by the Rust adapter's chunk
 * test, which lost 980 of 1000 keys.)
 *
 * MTX_ERR_NO_SPACE is returned only when NOTHING fit, i.e. no progress at
 * all is possible; then *arena_used is likewise the size needed.
 *
 * Buffer-filling rather than callback-driven on purpose: see the header
 * comment about the ticker spinlock. */
int mtx_scan_chunk(mtx_tree *t, const char *from, size_t fromlen, mtx_kv *out,
                   size_t cap, char *arena, size_t arena_cap, size_t *n_out,
                   size_t *arena_used);

/* Descending mirror of mtx_scan_chunk: visits keys at or before `from`. */
int mtx_rscan_chunk(mtx_tree *t, const char *from, size_t fromlen, mtx_kv *out,
                    size_t cap, char *arena, size_t arena_cap, size_t *n_out,
                    size_t *arena_used);

/* --- bookkeeping -------------------------------------------------------- */

/* Approximate key count. Not exact under concurrent modification. */
int mtx_size(mtx_tree *t, size_t *out);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MAKO_MTREE_ABI_H */
