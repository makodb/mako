#pragma once
namespace mako {
// @unsafe - Call once on attachment; end after every Rust transaction settles.
void benchmark_thread_init(bool loader);
void benchmark_thread_end();
}
