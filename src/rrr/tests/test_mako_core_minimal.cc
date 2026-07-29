// Minimal test for mako_core library
// Tests basic functionality without gtest dependency

#include <stdint.h>
#include <stddef.h>

#include "../rrr.hpp"
#include "mako/varint.h"
#include "mako/macros.h"

import std;

using namespace rrr;

int main() {
    Log_info("Mako core minimal test starting...");

    // Test 1: Varint encoding/decoding (using C-style functions)
    Log_info("Testing varint encoding/decoding...");
    {
        uint32_t test_values[] = {0, 1, 127, 128, 255, 256, 16383, 16384, 0x7FFFFFFF, 0xFFFFFFFF};
        uint8_t buffer[16];

        for (uint32_t val : test_values) {
            // Encode
            uint8_t *end = write_uvint32(buffer, val);
            size_t encoded_len = end - buffer;

            // Decode
            uint32_t decoded = 0;
            read_uvint32(buffer, &decoded);

            if (decoded != val) {
                Log_error("Varint test failed: expected {}, got {}", val, decoded);
                return 1;
            }
            Log_info("  Varint {}: encoded {} bytes, decoded correctly", val, encoded_len);
        }
    }

    // Test 2: Timer test (from librrr)
    Log_info("Testing timer...");
    {
        auto timer = Timer::new_();
        timer.start();

        volatile int sum = 0;
        for (int i = 0; i < 100000; i++) {
            sum += i;
        }

        timer.stop();
        Log_info("  Sum = {}, elapsed = {:f} seconds", sum, timer.elapsed());
    }

    // Test 3: ALWAYS_ASSERT macro (from mako/macros.h)
    Log_info("Testing ALWAYS_ASSERT macro...");
    {
        ALWAYS_ASSERT(true);
        Log_info("  ALWAYS_ASSERT(true) passed");

        // Test that INVARIANT works
        INVARIANT(1 + 1 == 2);
        Log_info("  INVARIANT(1+1==2) passed");
    }

    Log_info("All mako_core tests passed!");
    return 0;
}
