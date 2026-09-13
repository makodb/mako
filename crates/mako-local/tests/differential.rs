//! Three-way differential: the C++ engine called directly, the C++ engine
//! behind the `mako_local_*` C ABI, and this safe Rust facade over that ABI.
//!
//! The corpus, transcript format, generic replay, and subprocess helpers live
//! in `mako-differential` and are shared with the native Rust STO engine's
//! fourth executor. All native roles run as children so deterministic failure
//! replay can spawn fresh processes.
#![cfg(have_mako)]

use std::io::{Read, Write as _};
use std::path::{Path, PathBuf};
use std::process::Command;

use mako_differential::{
    assert_default_coverage, checked_stdout, driver_path, first_difference, generated_corpus,
    parse_corpus, replay, run_cpp, run_cpp_injected, run_with_input, temp_directory, REPLAY_ENV,
    TRANSCRIPT_HEADER,
};
use mako_local::{features, LocalDb};

const SAFE_ROLE_ENV: &str = "MAKO_LOCAL_DIFFERENTIAL_SAFE_ROLE";
const SAFE_OUTPUT_ENV: &str = "MAKO_LOCAL_DIFFERENTIAL_SAFE_OUTPUT";
const FIXTURE: &str = include_str!("../../../tests/data/mako_local_differential_v1.txt");

fn run_safe_child(corpus: &[u8]) -> Result<Vec<u8>, String> {
    let directory = temp_directory("safe-role")?;
    let output_path = directory.join("transcript.txt");
    let mut command = Command::new(std::env::current_exe().map_err(|error| error.to_string())?);
    command
        .arg("--ignored")
        .arg("--exact")
        .arg("differential_safe_replay_role")
        .arg("--test-threads=1")
        .env(SAFE_ROLE_ENV, "1")
        .env(SAFE_OUTPUT_ENV, &output_path);
    let child = run_with_input(&mut command, corpus)?;
    checked_stdout("safe Rust child", child)?;
    let transcript = std::fs::read(&output_path)
        .map_err(|error| format!("safe child did not write {output_path:?}: {error}"));
    if transcript
        .as_ref()
        .is_ok_and(|bytes| !bytes.starts_with(format!("{TRANSCRIPT_HEADER}\n").as_bytes()))
    {
        return Err("safe child wrote a malformed transcript header".to_owned());
    }
    let _ = std::fs::remove_file(&output_path);
    let _ = std::fs::remove_dir(&directory);
    transcript
}

fn compare_three_way(direct: &[u8], abi: &[u8], safe: &[u8]) -> Result<(), String> {
    if direct != abi {
        Err(format!(
            "direct C++ vs C ABI:\n{}",
            first_difference(direct, abi)
        ))
    } else if abi != safe {
        Err(format!(
            "C ABI vs safe Rust:\n{}",
            first_difference(abi, safe)
        ))
    } else {
        Ok(())
    }
}

fn write_failure_artifacts(
    directory: &Path,
    direct: &[u8],
    abi: &[u8],
    safe: &[u8],
) -> Result<(), String> {
    for (name, bytes) in [("direct.txt", direct), ("abi.txt", abi), ("safe.txt", safe)] {
        std::fs::write(directory.join(name), bytes)
            .map_err(|error| format!("cannot write differential artifact {name}: {error}"))?;
    }
    Ok(())
}

#[test]
fn three_way_deterministic_transactions_agree() {
    let driver: PathBuf = match driver_path() {
        Ok(Some(driver)) => driver,
        Ok(None) => {
            eprintln!(
                "skipping differential subprocess gate: MAKO_LOCAL_DIFFERENTIAL_DRIVER is not set"
            );
            return;
        }
        Err(error) => panic!("{error}"),
    };

    let explicit_replay = std::env::var_os(REPLAY_ENV).is_some();
    let corpus_text = generated_corpus(FIXTURE).unwrap_or_else(|error| panic!("{error}"));
    let artifact_directory = temp_directory("pending").unwrap_or_else(|error| panic!("{error}"));
    std::fs::write(
        artifact_directory.join("corpus.txt"),
        corpus_text.as_bytes(),
    )
    .unwrap_or_else(|error| panic!("cannot stage replay corpus: {error}"));
    let parsed = parse_corpus(&corpus_text).unwrap_or_else(|error| {
        let _ = std::fs::write(artifact_directory.join("parser-error.txt"), &error);
        panic!(
            "{error}\nreplay corpus retained at {}",
            artifact_directory.display()
        )
    });
    if !explicit_replay {
        assert!(parsed.scripts.len() >= 3, "corpus lost seeded coverage");
        assert_default_coverage(&parsed);
    }
    let corpus = corpus_text.as_bytes();

    // All native roles are children. The orchestrator itself never initializes
    // STO, so deterministic failure replay can safely spawn fresh processes.
    let direct = run_cpp(&driver, "direct", corpus).unwrap_or_else(|error| {
        let _ = std::fs::write(artifact_directory.join("direct-error.txt"), &error);
        panic!(
            "{error}\nreplay corpus retained at {}",
            artifact_directory.display()
        )
    });
    let abi = run_cpp(&driver, "abi", corpus).unwrap_or_else(|error| {
        let _ = std::fs::write(artifact_directory.join("direct.txt"), &direct);
        let _ = std::fs::write(artifact_directory.join("abi-error.txt"), &error);
        panic!(
            "{error}\nreplay corpus retained at {}",
            artifact_directory.display()
        )
    });
    let safe = run_safe_child(corpus).unwrap_or_else(|error| {
        let _ = std::fs::write(artifact_directory.join("direct.txt"), &direct);
        let _ = std::fs::write(artifact_directory.join("abi.txt"), &abi);
        let _ = std::fs::write(artifact_directory.join("safe-error.txt"), &error);
        panic!(
            "{error}\nreplay corpus retained at {}",
            artifact_directory.display()
        )
    });

    if let Err(comparison) = compare_three_way(&direct, &abi, &safe) {
        let artifacts = write_failure_artifacts(&artifact_directory, &direct, &abi, &safe)
            .map(|()| format!("replay artifacts: {}", artifact_directory.display()))
            .unwrap_or_else(|error| format!("could not write replay artifacts: {error}"));
        panic!(
            "three-way local transaction differential failed\n{comparison}\n{artifacts}\nreplay with {REPLAY_ENV}=<artifact>/corpus.txt"
        );
    }

    if !explicit_replay {
        // Exercise the actual subprocess boundary, not merely the comparator
        // in memory. The default corpus starts with a get; the driver makes
        // that observation wrong and the harness must distinguish the real
        // child-process transcript from the verified baseline.
        let injected = run_cpp_injected(&driver, "direct", corpus, 0).unwrap_or_else(|error| {
            panic!(
                "differential tripwire could not run: {error}\nreplay corpus retained at {}",
                artifact_directory.display()
            )
        });
        let diagnostic = match compare_three_way(&injected, &abi, &safe) {
            Err(diagnostic) => diagnostic,
            Ok(()) => panic!(
                "the end-to-end divergence tripwire failed to turn the harness red; corpus retained at {}",
                artifact_directory.display()
            ),
        };
        assert!(diagnostic.contains("direct C++ vs C ABI"));
    }

    let _ = std::fs::remove_file(artifact_directory.join("corpus.txt"));
    let _ = std::fs::remove_dir(&artifact_directory);
}

#[test]
#[ignore = "subprocess role used by three_way_deterministic_transactions_agree"]
fn differential_safe_replay_role() {
    if std::env::var_os(SAFE_ROLE_ENV).is_none() {
        return;
    }
    let mut corpus_text = String::new();
    std::io::stdin()
        .read_to_string(&mut corpus_text)
        .expect("read differential corpus from parent");
    let corpus = parse_corpus(&corpus_text).expect("parse differential corpus");
    let capabilities = features().expect("native feature bits");
    let transcript =
        replay::<LocalDb>(&corpus, capabilities).expect("safe Rust differential replay");
    let output = std::env::var_os(SAFE_OUTPUT_ENV).expect("safe output path from parent");
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(output)
        .expect("create safe differential transcript exactly once");
    file.write_all(transcript.as_bytes())
        .expect("write safe differential transcript");
}

#[test]
fn cpp_parser_rejects_noncanonical_or_ill_nested_input() {
    let malformed_inputs = [
        "mako-local-differential-v1\nscript x\nend\n",
        "mako-local-differential-v1\nscript x\ntable 1 1 61\nend\n",
        "mako-local-differential-v1\nscript x\ntable 0 1 61\nbegin\nget 0 zz\ncommit\nend\n",
        "mako-local-differential-v1\nscript x\ntable 0 1 61\nbegin\nget 0 aéa\ncommit\nend\n",
        "mako-local-differential-v1\nscript x\ntable 0 1 61\nbegin\ncommit\n",
        "mako-local-differential-v1\nscript  x\ntable 0 1 61\nbegin\ncommit\nend\n",
        "mako-local-differential-v1\nscript x\ntable 00 1 61\nbegin\ncommit\nend\n",
        "mako-local-differential-v1\nscript x\ntable 0 1 61\ntable 1 1 62\nbegin\ncommit\nend\n",
        "mako-local-differential-v1\nscript x\ntable 0 1 61\ntable 1 2 61\nbegin\ncommit\nend\n",
        "mako-local-differential-v1\nscript x\ntable 0 1 61\nbegin\ncommit\ntable 1 2 62\nend\n",
        "mako-local-differential-v1\nscript x\ntable 0 1 61\nbegin\ncommit\nend\nscript x\ntable 0 2 62\nbegin\ncommit\nend\n",
        "mako-local-differential-v1\r\nscript x\r\n",
    ];
    for malformed in malformed_inputs {
        assert!(parse_corpus(malformed).is_err(), "accepted {malformed:?}");
    }

    if let Ok(Some(driver)) = driver_path() {
        for malformed in malformed_inputs {
            assert!(
                run_cpp(&driver, "direct", malformed.as_bytes()).is_err(),
                "C++ parser accepted {malformed:?}"
            );
        }
    }
}
