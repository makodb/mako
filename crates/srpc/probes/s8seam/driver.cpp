// The C++ half of the seam proof: import the transpiled module, set up a
// fiber stack, switch to it and back. The assembly comes from the SAME
// .S file Rust used — no twin, nothing to drift.
import s8seam;
#include <cstdint>
#include <cstdio>
#include <vector>

static s8seam::Ctx g_main{0};
static s8seam::Ctx g_fiber{0};
static volatile uint64_t g_mark = 0;

extern "C" void fiber_entry() {
    g_mark = 0xC0FFEE;
    s8seam::swap(&g_fiber, &g_main);
    g_mark = 0xDEAD;  // reached only if the switch back failed
}

int main() {
    std::vector<uint64_t> stack(64 * 1024);
    auto top = reinterpret_cast<uintptr_t>(stack.data() + stack.size());
    auto* sp = reinterpret_cast<uint64_t*>(top & ~uintptr_t{0xF});
    *--sp = reinterpret_cast<uint64_t>(&fiber_entry);
    sp -= 6;
    for (int i = 0; i < 6; ++i) sp[i] = 0;
    g_fiber.sp = reinterpret_cast<uint64_t>(sp);

    s8seam::swap(&g_main, &g_fiber);
    if (g_mark != 0xC0FFEE) { std::printf("FAIL mark=%lx\n", (unsigned long)g_mark); return 1; }
    std::printf("C++ SIDE OK: switched stacks and returned (mark=%lx)\n", (unsigned long)g_mark);
    return 0;
}
