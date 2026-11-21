//! Bit-for-bit differential between the C++ hybrid logical clock in
//! `src/mako/sto/Transaction.cc` (reached through the `mako_local` C ABI) and
//! the safe Rust port in `mako-clock`.
//!
//! Production builds read real time, so the script first raises both clocks
//! to a recovery floor far above any wall-clock reading. From then on every
//! allocation on both sides is `previous + 1` and the two clocks must stay in
//! lockstep through every floor rule: a hot logical bump, a foreign origin at
//! the same tuple, sub-millisecond physical time, a logical value outside the
//! hot range, the logical ceiling, an older observation, and exhaustion at
//! the final stamp. One write commit on the native side allocates exactly
//! one stamp, so a native commit report is the native clock's output.
#![cfg(have_mako)]

use mako_clock::{Clock, LOGICAL_MASK, PHYSICAL_MS_MAX, STAMP_MAX};
use mako_local::{
    advance_mako_timestamp_past, CommitDisposition, Error, LocalDb, MakoTimestamp, Table,
};

const PHYSICAL_UNIT_US: u64 = 1_000;
/// Roughly the year 2096: above any wall clock this test can run under, far
/// below the 44-bit ceiling.
const BASE_MS: u64 = 4_000_000_000_000;

fn native_commit(db: &LocalDb, table: &Table<'_>, key: &[u8]) -> Result<MakoTimestamp, Error> {
    let mut tx = db.transaction().unwrap();
    tx.put(table, key, b"v").unwrap();
    let mut seen = None;
    let report = tx.commit_report_with_hook(|timestamp| {
        seen = Some(timestamp);
        true
    });
    match report.disposition {
        CommitDisposition::Committed => Ok(seen.expect("a write commit reports its timestamp")),
        CommitDisposition::Aborted(error) => Err(error),
        CommitDisposition::Unknown(error) => panic!("native commit outcome unknown: {error:?}"),
    }
}

fn rust_commit(clock: &Clock) -> Result<MakoTimestamp, Error> {
    let stamp = clock
        .try_allocate_timestamp()
        .ok_or(Error::TimestampExhausted)?;
    Ok(mako_clock::expand(stamp).expect("allocated stamps expand"))
}

fn timestamp(physical_us: u64, logical: u32, origin: u32) -> MakoTimestamp {
    MakoTimestamp::new(physical_us, logical, origin).unwrap()
}

#[test]
fn rust_clock_matches_the_native_clock_bit_for_bit() {
    let db = LocalDb::open().unwrap();
    let table = db.open_table("rust_clock_differential", 20_099).unwrap();
    let rust = Clock::new();
    // Any reading below the first floor keeps the Rust clock on the
    // `previous + 1` path, exactly like the native clock under BASE_MS.
    assert!(rust.physical().set_test_unix_ms(1_000));

    struct Scenario {
        name: &'static str,
        observed: MakoTimestamp,
        accepted: bool,
        commits: usize,
    }
    let scenarios = [
        Scenario {
            name: "hot logical bump from our own origin",
            observed: timestamp(BASE_MS * PHYSICAL_UNIT_US, 42, 1),
            accepted: true,
            commits: 4,
        },
        Scenario {
            name: "same tuple from a foreign origin still bumps logical",
            observed: timestamp((BASE_MS + 10) * PHYSICAL_UNIT_US, 0, 7),
            accepted: true,
            commits: 3,
        },
        Scenario {
            name: "sub-millisecond physical time rolls to the next millisecond",
            observed: timestamp((BASE_MS + 20) * PHYSICAL_UNIT_US + 1, 5, 1),
            accepted: true,
            commits: 3,
        },
        Scenario {
            name: "logical above the hot range rolls to the next millisecond",
            observed: timestamp(
                (BASE_MS + 30) * PHYSICAL_UNIT_US,
                (LOGICAL_MASK + 1) as u32,
                1,
            ),
            accepted: true,
            commits: 3,
        },
        Scenario {
            name: "logical at the hot ceiling rolls to the next millisecond",
            observed: timestamp((BASE_MS + 40) * PHYSICAL_UNIT_US, LOGICAL_MASK as u32, 1),
            accepted: true,
            commits: 3,
        },
        Scenario {
            name: "an older observation never lowers either clock",
            observed: timestamp((BASE_MS - 1_000) * PHYSICAL_UNIT_US, 0, 1),
            accepted: true,
            commits: 3,
        },
        Scenario {
            name: "the zero tuple from a later origin is a no-op after the floor",
            observed: timestamp(0, 0, 2),
            accepted: true,
            commits: 2,
        },
        Scenario {
            name: "observing past the final stamp is refused without moving the clock",
            observed: timestamp(PHYSICAL_MS_MAX * PHYSICAL_UNIT_US, LOGICAL_MASK as u32, 1),
            accepted: false,
            commits: 2,
        },
        Scenario {
            name: "observing the penultimate stamp leaves exactly one mintable",
            observed: timestamp(
                PHYSICAL_MS_MAX * PHYSICAL_UNIT_US,
                LOGICAL_MASK as u32 - 1,
                1,
            ),
            accepted: true,
            commits: 3,
        },
    ];

    let mut previous: Option<MakoTimestamp> = None;
    let mut exhausted = false;
    for scenario in &scenarios {
        let native_accepted = advance_mako_timestamp_past(scenario.observed);
        let rust_accepted = rust.advance_past(scenario.observed);
        assert_eq!(
            native_accepted.is_ok(),
            scenario.accepted,
            "{}: native floor acceptance",
            scenario.name
        );
        assert_eq!(
            rust_accepted, scenario.accepted,
            "{}: rust floor acceptance",
            scenario.name
        );
        if !scenario.accepted {
            assert_eq!(
                native_accepted,
                Err(Error::TimestampExhausted),
                "{}",
                scenario.name
            );
        }
        for index in 0..scenario.commits {
            let key = format!("{}-{index}", scenario.name);
            let native = native_commit(&db, &table, key.as_bytes());
            let ported = rust_commit(&rust);
            assert_eq!(native, ported, "{}: commit {index}", scenario.name);
            match native {
                Ok(stamp) => {
                    assert!(!exhausted, "{}: minted after exhaustion", scenario.name);
                    assert_eq!(stamp.origin(), 1);
                    if scenario.accepted {
                        assert!(
                            stamp > scenario.observed,
                            "{}: not after the observation",
                            scenario.name
                        );
                    }
                    if let Some(previous) = previous {
                        assert!(
                            stamp > previous,
                            "{}: not after the previous commit",
                            scenario.name
                        );
                    }
                    let hot = stamp.hot_stamp().expect("local stamps are hot");
                    assert_eq!(
                        hot,
                        rust.current_stamp(),
                        "{}: private stamp",
                        scenario.name
                    );
                    previous = Some(stamp);
                }
                Err(Error::TimestampExhausted) => {
                    assert_eq!(previous.and_then(MakoTimestamp::hot_stamp), Some(STAMP_MAX));
                    exhausted = true;
                }
                Err(other) => panic!("{}: unexpected native failure {other:?}", scenario.name),
            }
        }
    }
    assert!(exhausted, "the script must end at the final mintable stamp");
    assert_eq!(rust.current_stamp(), STAMP_MAX);
}
