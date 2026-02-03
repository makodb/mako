#include <iostream>

#include "varint.h"
#include "macros.h"
#include "util.h"

using namespace std;
using namespace util;

// @unsafe: uses pointer arithmetic
static void
do_test(uint32_t v)
{
  uint8_t buf[5];
  uint8_t *p = &buf[0];
  p = write_uvint32(p, v);
  ALWAYS_ASSERT(size_t(p - &buf[0]) == size_uvint32(v));

  const uint8_t *p0 = &buf[0]; // @safe: taking address of local array is safe
  uint32_t v0 = 0;
  p0 = read_uvint32(p0, &v0); // @safe: read_uvint32 is safe
  ALWAYS_ASSERT(v == v0);
  ALWAYS_ASSERT(p == p0);
}

// @unsafe: calls unsafe do_test
void
varint::Test()
{
  fast_random r(2043859); // @safe: fast_random is safe
  for (int i = 0; i < 1000; i++)
    do_test(r.next_u32());
  cerr << "varint tests passed" << endl;
}