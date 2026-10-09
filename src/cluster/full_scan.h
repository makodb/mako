#ifndef MAKO_CLUSTER_FULL_SCAN_H
#define MAKO_CLUSTER_FULL_SCAN_H
#include "cluster/native_sharding.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MakoFullScan MakoFullScan;
typedef struct MakoScanPage MakoScanPage;
typedef struct MakoScanBounds {
    MakoShardBytes lo;
    MakoShardBytes hi;
    MakoShardBytes cursor;
    uint32_t has_hi;
    uint32_t has_cursor;
    uint32_t reverse;
} MakoScanBounds;
/* Identity bytes are borrowed only for the constructor/comparison call. */
typedef struct MakoScanIdentity {
    MakoShardTxn transaction;
    MakoShardGrant grant;
    uint64_t table;
    uint32_t fixed_coordinate;
    MakoShardBytes coordinate;
} MakoScanIdentity;
/* Runtime pins one immutable snapshot through the complete visitor traversal.
 * The visitor may block/reenter: no cache/participant mutex may remain held.
 * Visitor: 0 continue, 1 stop, 2 error. Bounds are borrowed during the call. */
typedef uint32_t (*MakoScanSegmentCallback)(void*, MakoShardBytes, uint32_t,
                                           MakoShardBytes, MakoShardGrant);
uint32_t mako_sharding_scan_segments(uint64_t table, MakoShardBytes lo,
                                      uint32_t has_hi, MakoShardBytes hi,
                                      uint32_t reverse,
                                      MakoScanSegmentCallback callback,
                                      void* context);
/* Callback bytes are borrowed only for the call; exceptions must not cross Rust. */
typedef uint32_t (*MakoScanRowCallback)(void*, MakoShardBytes, MakoShardBytes);
uint32_t mako_full_scan_new(MakoScanBounds bounds,
                            const MakoScanIdentity* identity,
                            MakoFullScan** output);
void mako_full_scan_free(MakoFullScan* scan);
uint32_t mako_full_scan_request(const MakoFullScan* scan, uint8_t* output,
                                size_t capacity, size_t* length);
uint32_t mako_full_scan_consume(MakoFullScan* scan, MakoShardBytes page,
                                MakoScanRowCallback callback, void* context,
                                uint32_t* done);
uint32_t mako_scan_page_new(MakoShardBytes request, MakoScanPage** output);
uint32_t mako_scan_page_identity_matches(const MakoScanPage* page,
                                        const MakoScanIdentity* identity);
void mako_scan_page_free(MakoScanPage* page);
MakoScanBounds mako_scan_page_bounds(const MakoScanPage* page);
/* Reverse without hi/cursor first observes every key of a forward native scan
 * under the same transaction and full-range lease, then starts reverse at this
 * actual maximum inclusively. present distinguishes empty key from no rows.
 * The maximum bytes remain borrowed from page until it is freed. */
uint32_t mako_scan_page_observe_max(MakoScanPage* page, MakoShardBytes key);
uint32_t mako_scan_page_maximum(const MakoScanPage* page,
                                MakoShardBytes* maximum, uint32_t* present);
/* continue_scan is false only after a full page has a lookahead row, or error. */
uint32_t mako_scan_page_add(MakoScanPage* page, MakoShardBytes key,
                            MakoShardBytes value, uint32_t* continue_scan);
uint32_t mako_scan_page_finish(const MakoScanPage* page, uint8_t* output,
                               size_t capacity, size_t* length);
#ifdef __cplusplus
}
#endif
#endif
