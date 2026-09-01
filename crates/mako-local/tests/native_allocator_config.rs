#[path = "../build_support/native_allocator.rs"]
mod native_allocator;

use native_allocator::NativeAllocator;

#[test]
fn maps_every_supported_cmake_allocator_mode() {
    for (mode, expected, library) in [
        ("0", NativeAllocator::System, None),
        ("1", NativeAllocator::Jemalloc, Some("jemalloc")),
        ("2", NativeAllocator::Tcmalloc, Some("tcmalloc")),
        ("3", NativeAllocator::Flow, Some("flow")),
    ] {
        let cache = format!(
            "CMAKE_BUILD_TYPE:STRING=Release\nUSE_MALLOC_MODE:STRING={mode}\nOTHER:BOOL=ON\n"
        );
        let allocator = NativeAllocator::from_cmake_cache(&cache).unwrap();
        assert_eq!(allocator, expected);
        assert_eq!(allocator.link_library(), library);
    }
}

#[test]
fn rejects_missing_malformed_and_unsupported_modes() {
    for (cache, diagnostic) in [
        ("CMAKE_BUILD_TYPE:STRING=Release\n", "is missing"),
        ("USE_MALLOC_MODE:BOOL=1\n", "must have CMake type STRING"),
        ("USE_MALLOC_MODE:STRING=\n", "must be one of"),
        ("USE_MALLOC_MODE:STRING=01\n", "must be one of"),
        ("USE_MALLOC_MODE:STRING=4\n", "must be one of"),
        (
            "USE_MALLOC_MODE:STRING=1\nUSE_MALLOC_MODE:STRING=1\n",
            "appears more than once",
        ),
    ] {
        let error = NativeAllocator::from_cmake_cache(cache).unwrap_err();
        assert!(
            error.contains(diagnostic),
            "expected {diagnostic:?} in {error:?}"
        );
    }
}
