#pragma once

// Transitional shim. The contents of this header now live in the
// rrr.serializable C++23 module (src/rrr/misc/serializable.cpp).
//
// Consumers that #include "serializable.hpp" at TU scope get the
// import propagated to them — works because every existing include
// site is at the top of a .cc/.cpp file or top of a header that is
// itself included at TU top.

import rrr.serializable;
