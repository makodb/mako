// Lightweight Masstree scan test binary
//
// @external_unsafe_type: std::*
// @external_unsafe: std::*
// @external_unsafe: circular_int::*
// @external_unsafe: lcdf::String_base::*
// @external_unsafe: lcdf::String::*
// @external_unsafe: lcdf::String_generic::*
// @external_unsafe: Masstree::*
// @external_unsafe: threadinfo::*

#include "query_masstree.hh"

using namespace Masstree;

kvepoch_t global_log_epoch = 0;
volatile mrcu_epoch_type globalepoch = 1; // global epoch, updated by main thread regularly
volatile bool recovering = false; // so don't add log entries, and free old value immediately
kvtimestamp_t initial_timestamp;

// @unsafe - uses global volatile variables and unsafe threadinfo::make() allocator
int
main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    threadinfo* ti = threadinfo::make(threadinfo::TI_MAIN, -1);
    default_table::test(*ti);
}
