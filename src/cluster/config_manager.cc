// ConfigManager is authored entirely in the inline-Rust DSL in
// config_manager.h (struct + methods generate as inline functions there,
// plus the cm_* C++ kernels). This translation unit is intentionally
// empty apart from the include, kept on the build's source list so the
// header is compiled in a non-test context too.
#include "config_manager.h"
