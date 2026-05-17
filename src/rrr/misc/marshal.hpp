#pragma once

// Transitional shim. The contents of this header now live in the
// rrr.marshal C++23 module (src/rrr/misc/marshal.cpp).
//
// Consumers that #include "marshal.hpp" at TU scope get the import
// propagated to them — works because every include site is at the top
// of a .cc/.cpp file or top of a header that is itself included at
// TU top.

import rrr.marshal;
