/**
 * @file coroutine.h
 * @brief Compatibility header - includes fiber_impl.h
 *
 * This header includes fiber_impl.h which defines the Fiber class.
 * For the full API including this_fiber namespace and Future/Promise,
 * use fiber.h instead.
 *
 * The primary class is Fiber (defined in fiber_impl.h).
 */

#pragma once

#include "fiber_impl.h"
