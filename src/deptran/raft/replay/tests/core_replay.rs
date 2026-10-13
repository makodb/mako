// core_replay (docs/verus/modification-plan.md A.4 item 4): every recording
// under $MAKO_RAFT_REPLAY_DIR, each one node's history, replayed through this
// build's core. Each record's actions, log lines and reply must come out
// byte for byte as recorded. With the variable unset there is nothing to
// replay, and the test says so and passes.
//
//   MAKO_RAFT_REPLAY_DIR=$RESULTS/replay/<commit> cargo test -p raft-replay \
//       --release --test core_replay -- --nocapture

use std::path::PathBuf;

#[test]
fn recordings_replay_byte_for_byte() {
    let Ok(dir) = std::env::var("MAKO_RAFT_REPLAY_DIR") else {
        eprintln!("core_replay: MAKO_RAFT_REPLAY_DIR unset, nothing to replay");
        return;
    };
    let mut files: Vec<PathBuf> = std::fs::read_dir(&dir)
        .unwrap_or_else(|e| panic!("core_replay: cannot read {dir}: {e}"))
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.extension().is_some_and(|x| x == "rec"))
        .collect();
    files.sort();
    assert!(!files.is_empty(), "core_replay: no .rec files in {dir}");
    let (mut steps, mut tainted, mut failed) = (0usize, 0usize, 0usize);
    for path in &files {
        let text = std::fs::read_to_string(path).unwrap();
        match raft_replay::replay(&text) {
            Ok(r) => {
                steps += r.steps;
                if let Some(line) = r.tainted_at {
                    tainted += 1;
                    eprintln!("core_replay: {} stops at line {line} (a write outside step)",
                              path.display());
                }
            }
            Err(m) => {
                failed += 1;
                eprintln!("core_replay: {} line {}: {}\n  recorded: {}\n  replayed: {}",
                          path.display(), m.line, m.event, m.recorded, m.replayed);
            }
        }
    }
    eprintln!("core_replay: {} recordings, {steps} steps matched, {tainted} stopped at a \
               write outside step, {failed} mismatched", files.len());
    assert_eq!(failed, 0, "core_replay: {failed} recordings did not replay byte for byte");
}
