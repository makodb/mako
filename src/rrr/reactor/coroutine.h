/**
 * @file coroutine.h
 * @brief Backward compatibility header - includes fiber_impl.h
 *
 * This header is maintained for backward compatibility. New code should
 * use fiber.h instead, which provides:
 *   - Fiber (alias for the implementation class)
 *   - this_fiber namespace for fiber operations
 *   - Future/Promise for async value delivery
 *
 * The implementation has been moved to fiber_impl.h where the class
 * is named Fiber. Coroutine is a type alias for Fiber.
 */

#pragma once

#include "fiber_impl.h"
