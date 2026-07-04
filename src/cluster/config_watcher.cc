// ConfigWatcher is authored in the inline-Rust DSL in config_watcher.h
// (struct + Poll + accessors generate there; the thread lifecycle lives in
// the CwPollThread RAII helper). This TU is intentionally just the include.
#include "config_watcher.h"
