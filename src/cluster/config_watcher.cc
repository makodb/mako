// ConfigWatcher is authored in the inline-Rust DSL in config_watcher.h
// (struct + Poll + accessors + thread lifecycle generate there; the
// stop-then-join on destruction is a DSL impl Drop, and only cw_spawn stays
// a C++ kernel). This TU is intentionally just the include.
#include "config_watcher.h"
