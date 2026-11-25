//! Exhaustive small histories over explicit row/predicate storage contracts.
//! No Masstree, RocksDB, SRPC, Raft, clock sleeps or performance workload.
use mako_protocol_core::two_phase::{Coordinator, Decision};
use std::collections::HashSet;

#[derive(Clone, Debug, Hash, PartialEq, Eq)]
struct Attempt {
    step: usize,
    outcome: u8,
}
#[derive(Clone, Debug, Hash, PartialEq, Eq)]
struct State {
    rows: Vec<[Option<u8>; 2]>,
    locks: Vec<[Option<usize>; 2]>,
    attempts: Vec<Attempt>,
}
#[derive(Clone)]
struct Plan {
    read_shard: usize,
    read_keys: Vec<usize>,
    write_shard: usize,
    write_key: usize,
}
struct History {
    initial: Vec<[Option<u8>; 2]>,
    plans: Vec<Plan>,
}
impl History {
    fn coordinator(&self, state: &State, tx: usize) -> Coordinator {
        let n = self.initial.len();
        let step = state.attempts[tx].step;
        let mut c = Coordinator::new(n).unwrap();
        for member in 0..step.min(n) {
            assert!(c.record_locked(member));
        }
        for member in 0..step.saturating_sub(n).min(n) {
            assert!(c.record_prepared(member));
        }
        if state.attempts[tx].outcome == 1 {
            assert!(c.record_decision(Decision::Commit, true));
        }
        c
    }
    fn abort(&self, state: &mut State, tx: usize) {
        for shard in &mut state.locks {
            for key in shard {
                if *key == Some(tx) {
                    *key = None;
                }
            }
        }
        state.attempts[tx] = Attempt {
            step: 3 * self.initial.len() + 1,
            outcome: 2,
        };
    }
    fn step(&self, state: &mut State, tx: usize) {
        let n = self.initial.len();
        let step = state.attempts[tx].step;
        let plan = &self.plans[tx];
        let mut c = self.coordinator(state, tx);
        if step < n {
            if step == plan.write_shard {
                let lock = &mut state.locks[step][plan.write_key];
                if lock.is_some_and(|owner| owner != tx) {
                    self.abort(state, tx);
                    return;
                }
                *lock = Some(tx);
            }
            assert!(c.record_locked(step));
        } else if step < 2 * n {
            assert!(c.all_locked());
            let member = step - n;
            if member == plan.read_shard
                && plan.read_keys.iter().any(|&key| {
                    state.rows[member][key] != self.initial[member][key]
                        || state.locks[member][key].is_some_and(|owner| owner != tx)
                })
            {
                c.refuse();
                assert!(c.record_decision(Decision::Abort, true));
                self.abort(state, tx);
                return;
            }
            assert!(c.record_prepared(member));
        } else if step == 2 * n {
            assert_eq!(c.proposed(), Decision::Commit);
            assert!(c.record_decision(Decision::Commit, true));
            state.attempts[tx].outcome = 1;
        } else {
            assert_eq!(c.decision(), Decision::Commit);
            let member = step - (2 * n + 1);
            if member == plan.write_shard {
                assert_eq!(state.locks[member][plan.write_key], Some(tx));
                state.rows[member][plan.write_key] = Some(1);
                state.locks[member][plan.write_key] = None;
            }
            assert!(c.acknowledge(member, Decision::Commit));
        }
        state.attempts[tx].step += 1;
    }
    fn serializable(&self, state: &State) -> bool {
        fn order(
            history: &History,
            state: &State,
            rows: Vec<[Option<u8>; 2]>,
            remaining: Vec<usize>,
        ) -> bool {
            if remaining.is_empty() {
                return rows == state.rows;
            }
            for &tx in &remaining {
                let plan = &history.plans[tx];
                if plan
                    .read_keys
                    .iter()
                    .any(|&key| rows[plan.read_shard][key] != history.initial[plan.read_shard][key])
                {
                    continue;
                }
                let mut next = rows.clone();
                next[plan.write_shard][plan.write_key] = Some(1);
                if order(
                    history,
                    state,
                    next,
                    remaining
                        .iter()
                        .copied()
                        .filter(|&other| other != tx)
                        .collect(),
                ) {
                    return true;
                }
            }
            false
        }
        order(
            self,
            state,
            self.initial.clone(),
            state
                .attempts
                .iter()
                .enumerate()
                .filter_map(|(tx, attempt)| (attempt.outcome == 1).then_some(tx))
                .collect(),
        )
    }
    fn explore(&self) -> (usize, usize) {
        let initial = State {
            rows: self.initial.clone(),
            locks: vec![[None; 2]; self.initial.len()],
            attempts: vec![
                Attempt {
                    step: 0,
                    outcome: 0
                };
                self.plans.len()
            ],
        };
        let mut visited = HashSet::new();
        let mut pending = vec![initial];
        let mut terminal = 0;
        let mut most_commits = 0;
        while let Some(state) = pending.pop() {
            if !visited.insert(state.clone()) {
                continue;
            }
            if state
                .attempts
                .iter()
                .all(|a| a.step == 3 * self.initial.len() + 1)
            {
                assert!(
                    self.serializable(&state),
                    "nonserializable committed bytes: {state:?}"
                );
                assert!(state.locks.iter().flatten().all(Option::is_none));
                terminal += 1;
                most_commits =
                    most_commits.max(state.attempts.iter().filter(|a| a.outcome == 1).count());
                continue;
            }
            for tx in 0..state.attempts.len() {
                if state.attempts[tx].step < 3 * self.initial.len() + 1 {
                    let mut next = state.clone();
                    self.step(&mut next, tx);
                    pending.push(next);
                }
            }
        }
        (terminal, most_commits)
    }
}
#[test]
fn point_absence_and_range_cycles_have_a_serial_order_under_every_small_schedule() {
    for n in [2, 3] {
        for keys in [vec![0], vec![1], vec![0, 1]] {
            let key = *keys.last().unwrap();
            let history = History {
                initial: vec![[Some(0), None]; n],
                plans: (0..n)
                    .map(|tx| Plan {
                        read_shard: tx,
                        read_keys: keys.clone(),
                        write_shard: (tx + 1) % n,
                        write_key: key,
                    })
                    .collect(),
            };
            let (terminal, commits) = history.explore();
            assert!(terminal > 0 && commits > 0 && commits < n);
            // Check the oracle itself against the forbidden all-commit cycle.
            let mut invalid = State {
                rows: history.initial.clone(),
                locks: vec![[None; 2]; n],
                attempts: vec![
                    Attempt {
                        step: 3 * n + 1,
                        outcome: 1
                    };
                    n
                ],
            };
            for row in &mut invalid.rows {
                row[key] = Some(1);
            }
            assert!(!history.serializable(&invalid));
        }
    }
}
#[test]
fn independent_transactions_can_all_commit_without_a_shard_wide_reservation() {
    let history = History {
        initial: vec![[Some(0), None]; 3],
        plans: (0..3)
            .map(|tx| Plan {
                read_shard: tx,
                read_keys: vec![0],
                write_shard: tx,
                write_key: 0,
            })
            .collect(),
    };
    assert_eq!(history.explore().1, 3);
}
