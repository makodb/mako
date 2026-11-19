// A scanned import keeps the consumer runtime BMI and its dependencies current.
// This object is a build check; it is not part of libsrpc.a.
//
// It is also the one scanned consumer of srpc's own `std` variant.  No
// generated provider and no rusty-cpp port imports `std`, so without these two
// imports nothing in the build graph asks for the @cmake_cxx_std@synth_0 BMIs.
// goal0-battery-modules.modmap still names them (CMake lists `std` and
// `std.compat` among srpc's module references), and every battery program and
// rpcbench import `std` through that map.  A fresh build tree then failed in
// every battery TU with "module file ... @cmake_cxx_std@synth_0.dir/....bmi
// not found".  This target carries srpc's flags and resolves `std` to the same
// synth variant as srpc, so these imports make the scanner build exactly the
// BMIs the modmap names, before the modmap is emitted.
import std;
import std.compat;
import rusty;
