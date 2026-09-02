#[path = "../build_support/native_allocator.rs"]
mod native_allocator;

use native_allocator::{NativeAllocator, ResolvedAllocator, CONTRACT_RELATIVE_PATH};

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

#[test]
fn parses_system_and_shared_allocator_contracts() {
    assert_eq!(
        CONTRACT_RELATIVE_PATH,
        "generated/mako_allocator_contract.txt"
    );
    let system = ResolvedAllocator::from_contract(
        "schema=1\n\
         mode=0\n\
         kind=system\n\
         linkage=none\n\
         link_name=\n\
         library_path=\n\
         soname=\n\
         sha256=\n",
    )
    .unwrap()
    .validate_mode(NativeAllocator::System)
    .unwrap();
    assert_eq!(system.library_path(), None);
    assert_eq!(system.library_directory(), None);
    assert_eq!(system.link_name(), None);
    system.validate_library_path().unwrap();

    let jemalloc = ResolvedAllocator::from_contract(&shared_jemalloc_contract())
        .unwrap()
        .validate_mode(NativeAllocator::Jemalloc)
        .unwrap();
    assert_eq!(
        jemalloc.library_path().unwrap().to_str(),
        Some("/opt/mako/lib/libjemalloc.so.2")
    );
    assert_eq!(jemalloc.link_name(), Some("jemalloc"));
}

#[cfg(unix)]
#[test]
fn validates_that_link_name_reaches_the_verified_shared_object() {
    use std::fs;
    use std::os::unix::fs::symlink;
    use std::time::{SystemTime, UNIX_EPOCH};

    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let directory = std::env::temp_dir().join(format!(
        "mako-native-allocator-contract-{}-{nonce}",
        std::process::id()
    ));
    fs::create_dir(&directory).unwrap();
    let library = directory.join("libjemalloc.so.2");
    fs::write(&library, b"test shared-object identity").unwrap();
    symlink("libjemalloc.so.2", directory.join("libjemalloc.so")).unwrap();
    let digest_output = std::process::Command::new("sha256sum")
        .arg("--")
        .arg(&library)
        .output()
        .unwrap();
    assert!(digest_output.status.success());
    let digest_output = String::from_utf8(digest_output.stdout).unwrap();
    let digest = digest_output.split_ascii_whitespace().next().unwrap();

    let contract = shared_jemalloc_contract()
        .replace("/opt/mako/lib/libjemalloc.so.2", library.to_str().unwrap())
        .replace(&"a".repeat(64), digest);
    let allocator = ResolvedAllocator::from_contract(&contract).unwrap();
    allocator.validate_library_path().unwrap();

    fs::write(&library, b"mutated shared-object identity").unwrap();
    let error = allocator.validate_library_path().unwrap_err();
    assert!(error.contains("hash changed"), "{error}");
    fs::write(&library, b"test shared-object identity").unwrap();

    fs::remove_file(directory.join("libjemalloc.so")).unwrap();
    symlink("different.so", directory.join("libjemalloc.so")).unwrap();
    let error = allocator.validate_library_path().unwrap_err();
    assert!(error.contains("unavailable"), "{error}");
    fs::remove_dir_all(directory).unwrap();
}

#[test]
fn rejects_malformed_or_mismatched_allocator_contracts() {
    for (contract, diagnostic) in [
        (
            shared_jemalloc_contract().replace("schema=1\n", ""),
            "missing field \"schema\"",
        ),
        (
            shared_jemalloc_contract().replace("schema=1", "schema=2"),
            "schema must be 1",
        ),
        (
            shared_jemalloc_contract().replace("kind=jemalloc", "kind=tcmalloc"),
            "requires kind",
        ),
        (
            shared_jemalloc_contract().replace("linkage=shared", "linkage=static"),
            "requires shared linkage",
        ),
        (
            shared_jemalloc_contract().replace("link_name=jemalloc", "link_name=flow"),
            "requires link_name",
        ),
        (
            shared_jemalloc_contract()
                .replace("/opt/mako/lib/libjemalloc.so.2", "libjemalloc.so.2"),
            "canonical and absolute",
        ),
        (
            shared_jemalloc_contract().replace(&"a".repeat(64), "ABC"),
            "64 lowercase hexadecimal",
        ),
        (
            shared_jemalloc_contract() + "unknown=value\n",
            "unknown field",
        ),
    ] {
        let error = ResolvedAllocator::from_contract(&contract).unwrap_err();
        assert!(
            error.contains(diagnostic),
            "expected {diagnostic:?} in {error:?}"
        );
    }

    let error = ResolvedAllocator::from_contract(&shared_jemalloc_contract())
        .unwrap()
        .validate_mode(NativeAllocator::Tcmalloc)
        .unwrap_err();
    assert!(error.contains("does not match CMakeCache"), "{error}");
}

fn shared_jemalloc_contract() -> String {
    format!(
        "schema=1\n\
         mode=1\n\
         kind=jemalloc\n\
         linkage=shared\n\
         link_name=jemalloc\n\
         library_path=/opt/mako/lib/libjemalloc.so.2\n\
         soname=libjemalloc.so.2\n\
         sha256={}\n",
        "a".repeat(64)
    )
}
