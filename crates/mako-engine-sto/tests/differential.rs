//! Fourth differential executor: the native Rust STO engine against the C++
//! engine behind the C ABI.
//!
//! The corpus and transcript format are shared with `mako-local`'s
//! three-way differential. The C++ executor runs in a child process named by
//! `MAKO_LOCAL_DIFFERENTIAL_DRIVER`; this engine replays the same corpus in
//! this process. Only the `features` lines may differ, because optional
//! test-hook capability bits are build-specific; every operation result,
//! commit outcome, and final table state must agree byte for byte.

use std::path::PathBuf;

use mako_differential::{
    assert_default_coverage, driver_path, first_difference, generated_corpus, parse_corpus, replay,
    run_cpp, run_cpp_injected, strip_feature_lines, temp_directory, REPLAY_ENV,
};
use mako_engine_sto::{features, LocalDb};

const FIXTURE: &str = include_str!("../../../tests/data/mako_local_differential_v1.txt");

fn compare(abi: &[u8], sto: &[u8]) -> Result<(), String> {
    let abi = strip_feature_lines(abi);
    let sto = strip_feature_lines(sto);
    if abi != sto {
        Err(format!(
            "C ABI vs Rust STO:\n{}",
            first_difference(&abi, &sto)
        ))
    } else {
        Ok(())
    }
}

#[test]
fn rust_sto_engine_agrees_with_the_c_abi_engine() {
    let driver: PathBuf = match driver_path() {
        Ok(Some(driver)) => driver,
        Ok(None) => {
            eprintln!("skipping engine differential: MAKO_LOCAL_DIFFERENTIAL_DRIVER is not set");
            return;
        }
        Err(error) => panic!("{error}"),
    };
    let explicit_replay = std::env::var_os(REPLAY_ENV).is_some();
    let corpus_text = generated_corpus(FIXTURE).unwrap_or_else(|error| panic!("{error}"));
    let artifact_directory =
        temp_directory("sto-pending").unwrap_or_else(|error| panic!("{error}"));
    std::fs::write(
        artifact_directory.join("corpus.txt"),
        corpus_text.as_bytes(),
    )
    .unwrap_or_else(|error| panic!("cannot stage replay corpus: {error}"));
    let parsed = parse_corpus(&corpus_text).unwrap_or_else(|error| panic!("{error}"));
    if !explicit_replay {
        assert_default_coverage(&parsed);
    }

    let abi = run_cpp(&driver, "abi", corpus_text.as_bytes()).unwrap_or_else(|error| {
        panic!(
            "{error}\nreplay corpus retained at {}",
            artifact_directory.display()
        )
    });
    let capabilities = features().expect("engine capabilities");
    let sto = replay::<LocalDb>(&parsed, capabilities).unwrap_or_else(|error| {
        let _ = std::fs::write(artifact_directory.join("abi.txt"), &abi);
        panic!(
            "Rust STO replay failed: {error}\nreplay corpus retained at {}",
            artifact_directory.display()
        )
    });

    if let Err(comparison) = compare(&abi, sto.as_bytes()) {
        let _ = std::fs::write(artifact_directory.join("abi.txt"), &abi);
        let _ = std::fs::write(artifact_directory.join("sto.txt"), sto.as_bytes());
        panic!(
            "engine differential failed\n{comparison}\nreplay artifacts: {}\nreplay with {REPLAY_ENV}=<artifact>/corpus.txt",
            artifact_directory.display()
        );
    }

    if !explicit_replay {
        // Prove the comparator sees the real child transcript: corrupt the
        // first operation on the C++ side and require a red result.
        let injected = run_cpp_injected(&driver, "abi", corpus_text.as_bytes(), 0)
            .unwrap_or_else(|error| panic!("differential tripwire could not run: {error}"));
        assert!(
            compare(&injected, sto.as_bytes()).is_err(),
            "the divergence tripwire failed to turn the engine differential red"
        );
    }

    let _ = std::fs::remove_file(artifact_directory.join("corpus.txt"));
    let _ = std::fs::remove_dir(&artifact_directory);
}
