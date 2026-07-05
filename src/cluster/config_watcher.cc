// ConfigWatcher is authored ENTIRELY in the inline-Rust DSL in
// config_watcher.h (struct + Poll + accessors + thread lifecycle; the
// stop-then-join on destruction is a DSL impl Drop, and Start() spawns the
// poll thread inline via a DSL closure). No C++ kernels. This TU is
// intentionally just the include.
#include "config_watcher.h"
