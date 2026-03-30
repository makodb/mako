fn main() -> miette::Result<()> {
    let mut build = cc::Build::new();
    build
        .cpp(true)
        .compiler("g++")
        .std("c++17");

    let path = std::path::PathBuf::from("src"); // include path

    let mut b = autocxx_build::Builder::new("src/main.rs", &[&path])
        .extra_clang_args(&[
            "-std=c++17", 
            "-I/usr/include/c++/11", 
            "-I/usr/include/x86_64-linux-gnu/c++/11",
            "-I/usr/include/x86_64-linux-gnu"
        ])
        .build()
        .unwrap();
        
    b.flag_if_supported("-std=c++17")// use "-std:c++17" here if using msvc on windows
        .flag_if_supported("-stdlib=libstdc++")
        .file("src/wrapper.cpp") 
        .compile("simpleKV"); // arbitrary library name, pick anything

    // Link the KV store static library
    println!("cargo:rustc-link-search=native=third-party");
    println!("cargo:rustc-link-lib=static=kv_store");
    
    // Link Boost libraries (needed even with static lib because static lib only contains our compiled code, not boost dependencies)
    println!("cargo:rustc-link-lib=boost_thread");
    println!("cargo:rustc-link-lib=boost_system");
    println!("cargo:rustc-link-lib=boost_chrono");
    
    println!("cargo:rerun-if-changed=src/main.rs");
    println!("cargo:rerun-if-changed=src/wrapper.cpp");
    println!("cargo:rerun-if-changed=src/wrapper.h");
    println!("cargo:rerun-if-changed=third-party/libkv_store.a");

    Ok(())
}