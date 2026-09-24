#pragma once

// C++ compile-time counterpart for rustc-only procedural marker imports.
// The name has no definition or runtime symbol; rusty-cpp consumes the actual
// attribute before emission, leaving only a private using-declaration.
namespace rusty {
struct cpp_inherit;
}
