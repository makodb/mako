/* Pure-C drift probe for the public mako-local header. */

#include "mako/storage/mako_local_abi.h"

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L
#error "the mako-local C probe must be compiled as C11 or newer"
#endif

#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_OK 0
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_CONFLICT 1
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_NOT_ATTACHED 2
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_WRONG_THREAD 3
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_TXN_ALREADY_ACTIVE 4
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_TXN_FINISHED 5
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_WRONG_DB_OR_TABLE 6
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_INVALID_ARGUMENT 7
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_THREAD_LIMIT 8
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_BUSY 9
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_OUT_OF_MEMORY 10
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_INTERNAL 11
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_DUPLICATE_WRITE 12
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_TXN_TOO_LARGE 13
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_VALUE_TOO_LARGE 14
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_COMMIT_HOOK_REJECTED 15
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_TIMESTAMP_EXHAUSTED 16
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_BUFFER_TOO_SMALL 17
#define MAKO_LOCAL_GOLDEN_MAKO_LOCAL_FEATURE_UNAVAILABLE 18

#define MAKO_LOCAL_ASSERT_GOLDEN(short_name, c_symbol, message)                \
  _Static_assert((c_symbol) == MAKO_LOCAL_GOLDEN_##c_symbol,                   \
                 #c_symbol " changed its assigned status number");
MAKO_LOCAL_FOR_EACH_STATUS(MAKO_LOCAL_ASSERT_GOLDEN)
#undef MAKO_LOCAL_ASSERT_GOLDEN

#define MAKO_LOCAL_COUNT_STATUS(short_name, c_symbol, message) +1
enum {
  mako_local_status_manifest_count =
      0 MAKO_LOCAL_FOR_EACH_STATUS(MAKO_LOCAL_COUNT_STATUS)
};
#undef MAKO_LOCAL_COUNT_STATUS

_Static_assert(mako_local_status_manifest_count == 19,
               "mako-local status catalog size changed");

#define MAKO_LOCAL_STATUS_VALUE(short_name, c_symbol, message) c_symbol,
static const int mako_local_status_values[] = {
    MAKO_LOCAL_FOR_EACH_STATUS(MAKO_LOCAL_STATUS_VALUE)};
#undef MAKO_LOCAL_STATUS_VALUE

int main(void) {
  unsigned char seen[mako_local_status_manifest_count] = {0};
  const unsigned long count =
      sizeof(mako_local_status_values) / sizeof(mako_local_status_values[0]);
  if (count != mako_local_status_manifest_count)
    return 1;

  for (unsigned long i = 0; i != count; ++i) {
    const int value = mako_local_status_values[i];
    if (value < 0 || value >= mako_local_status_manifest_count)
      return 2;
    if (seen[value] != 0)
      return 3;
    seen[value] = 1;
  }
  for (int value = 0; value != mako_local_status_manifest_count; ++value) {
    if (seen[value] == 0)
      return 4;
  }
  return 0;
}
