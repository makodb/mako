//! Sequential transaction specification using copied ordered maps.
//!
//! The model has no transaction engine, record versions, native ABI, or tree.
//! An attempt gets a private map copy; commit replaces the database and abort
//! discards it. This independently specifies point and range API results.
use super::{append_rows, encode_hex, Corpus, Operation, Terminal, TRANSCRIPT_HEADER};
use std::collections::{btree_map::Entry, BTreeMap};
use std::fmt::Write;

/// Replay a parsed corpus against the sequential specification.
pub fn replay_reference(corpus: &Corpus) -> String {
    let mut output = format!("{TRANSCRIPT_HEADER}\n");
    for script in &corpus.scripts {
        writeln!(output, "script {}", script.name).unwrap();
        let mut tables = vec![BTreeMap::<Vec<u8>, Vec<u8>>::new(); script.tables.len()];
        let mut ordinal = 0;
        for (attempt, transaction) in script.transactions.iter().enumerate() {
            let mut working = tables.clone();
            writeln!(output, "txn {attempt} begin ok").unwrap();
            for operation in &transaction.operations {
                match operation {
                    Operation::Get { table, key } => match working[*table].get(key) {
                        Some(value) => {
                            writeln!(output, "op {ordinal} get value {}", encode_hex(value))
                                .unwrap()
                        }
                        None => writeln!(output, "op {ordinal} get absent").unwrap(),
                    },
                    Operation::Put { table, key, value } => {
                        let created = working[*table].insert(key.clone(), value.clone()).is_none();
                        writeln!(output, "op {ordinal} put created {}", u8::from(created)).unwrap();
                    }
                    Operation::Insert { table, key, value } => {
                        let inserted = match working[*table].entry(key.clone()) {
                            Entry::Vacant(slot) => {
                                slot.insert(value.clone());
                                true
                            }
                            Entry::Occupied(_) => false,
                        };
                        writeln!(
                            output,
                            "op {ordinal} insert inserted {}",
                            u8::from(inserted)
                        )
                        .unwrap();
                    }
                    Operation::Remove { table, key } => {
                        let existed = working[*table].remove(key).is_some();
                        writeln!(output, "op {ordinal} remove existed {}", u8::from(existed))
                            .unwrap();
                    }
                    Operation::Scan {
                        table,
                        lower,
                        upper,
                        reverse,
                    } => {
                        let mut rows: Vec<_> = working[*table]
                            .iter()
                            .filter(|(key, _)| {
                                *key >= lower && upper.as_ref().is_none_or(|end| *key < end)
                            })
                            .map(|(key, value)| (key.clone(), value.clone()))
                            .collect();
                        if *reverse {
                            rows.reverse();
                        }
                        write!(
                            output,
                            "op {ordinal} {}",
                            if *reverse { "rscan" } else { "scan" }
                        )
                        .unwrap();
                        append_rows(&mut output, &rows);
                    }
                }
                ordinal += 1;
            }
            match transaction.terminal {
                Terminal::Commit => {
                    tables = working;
                    writeln!(output, "txn {attempt} commit ok").unwrap();
                }
                Terminal::Abort => writeln!(output, "txn {attempt} abort ok").unwrap(),
            }
        }
        for definition in &script.tables {
            write!(output, "final {}", definition.slot).unwrap();
            let rows: Vec<_> = tables[definition.slot]
                .iter()
                .map(|(k, v)| (k.clone(), v.clone()))
                .collect();
            append_rows(&mut output, &rows);
        }
        writeln!(output, "end {}", script.name).unwrap();
    }
    output
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parse_corpus;

    #[test]
    fn abort_restores_state_and_ranges_observe_staged_binary_rows() {
        let corpus = parse_corpus("mako-local-differential-v1\nscript spec\ntable 0 1 74\nbegin\nput 0 00 61\ncommit\nbegin\nput 0 ff 62\ninsert 0 00 63\nrscan 0 - *\nremove 0 00\nabort\nbegin\nget 0 00\nget 0 ff\nscan 0 ff 00\ncommit\nend\n").unwrap();
        assert_eq!(replay_reference(&corpus), "mako-local-transcript-v1\nscript spec\ntxn 0 begin ok\nop 0 put created 1\ntxn 0 commit ok\ntxn 1 begin ok\nop 1 put created 1\nop 2 insert inserted 0\nop 3 rscan rows 2 ff 62 00 61\nop 4 remove existed 1\ntxn 1 abort ok\ntxn 2 begin ok\nop 5 get value 61\nop 6 get absent\nop 7 scan rows 0\ntxn 2 commit ok\nfinal 0 rows 1 00 61\nend spec\n");
    }
}
