//! Compare Rust's public engine API with an independent ordered-map model.
//! This keeps the binary-key, scan, abort, and exhaustive operation-composition
//! corpus after retirement of the C++ executor.
use mako_differential::{
    assert_default_coverage, first_difference, generated_corpus, parse_corpus, replay,
    replay_reference, strip_feature_lines, temp_directory, REPLAY_ENV,
};
use mako_engine_sto::{features, LocalDb};

const FIXTURE: &str = include_str!("../../../tests/data/mako_local_differential_v1.txt");

#[test]
fn rust_engine_agrees_with_ordered_map_model() {
    let text = generated_corpus(FIXTURE).expect("generate corpus");
    let corpus = parse_corpus(&text).expect("parse corpus");
    if std::env::var_os(REPLAY_ENV).is_none() {
        assert_default_coverage(&corpus);
    }
    let expected = replay_reference(&corpus);
    let actual = replay::<LocalDb>(&corpus, features().expect("engine features"))
        .expect("replay through Rust engine");
    let actual = strip_feature_lines(actual.as_bytes());
    if expected.as_bytes() != actual {
        let directory = temp_directory("rust-model-divergence").unwrap();
        std::fs::write(directory.join("corpus.txt"), text).unwrap();
        std::fs::write(directory.join("expected.txt"), &expected).unwrap();
        std::fs::write(directory.join("actual.txt"), &actual).unwrap();
        panic!(
            "{}\nreplay with {REPLAY_ENV}={}/corpus.txt",
            first_difference(expected.as_bytes(), &actual),
            directory.display()
        );
    }
}
