//! Fixed cross-engine recovery cases using only the public cache API.

use std::collections::BTreeMap;
use std::path::Path;
use std::thread;
use std::time::{Duration, Instant};

use mako_cache::{Db, ForegroundMode, MakoTimestamp, Options, SingleProducer, Transaction};
use mrx_rocks::RocksBlobs;

use crate::{error_string, key_bytes, parse_record_checksum, value_bytes, AnyResult};

type Producer<'db> = Option<SingleProducer<'db, RocksBlobs>>;

fn transact(
    db: &Db,
    producer: &mut Producer<'_>,
    action: impl FnOnce(&mut Transaction<'_, RocksBlobs>) -> AnyResult<()>,
) -> AnyResult<()> {
    let mut tx = match producer {
        Some(producer) => producer.transaction(),
        None => db.transaction(),
    }
    .map_err(error_string)?;
    action(&mut tx)?;
    tx.commit().map_err(error_string)
}

fn expected_rows(checkpoint_only: bool, updated: bool) -> BTreeMap<Vec<u8>, Vec<u8>> {
    let mut rows = BTreeMap::new();
    if !checkpoint_only {
        for index in 2..8 {
            let value = match index {
                2 => 22,
                3 => 30,
                _ => index as u64,
            };
            rows.insert(key_bytes(index).to_vec(), value_bytes(value));
        }
    }
    if updated {
        rows.insert(key_bytes(0).to_vec(), value_bytes(100));
        rows.remove(key_bytes(2).as_slice());
        rows.insert(key_bytes(3).to_vec(), value_bytes(300));
        rows.insert(key_bytes(9).to_vec(), value_bytes(900));
        rows.insert(key_bytes(10).to_vec(), value_bytes(1000));
    }
    rows
}

fn validate_rows(
    db: &Db,
    producer: &mut Producer<'_>,
    expected: &BTreeMap<Vec<u8>, Vec<u8>>,
) -> AnyResult<()> {
    transact(db, producer, |tx| {
        for index in 0..12 {
            let key = key_bytes(index);
            if tx.get(&key).map_err(error_string)?.as_ref() != expected.get(key.as_slice()) {
                return Err(format!("cross-engine row mismatch at key {index}"));
            }
        }
        let scanned: BTreeMap<_, _> = tx
            .scan(&[], None)
            .map_err(error_string)?
            .collect::<Result<_, _>>()
            .map_err(error_string)?;
        if &scanned != expected {
            return Err("cross-engine scan disagrees with exact expected rows".to_owned());
        }
        let reverse: Vec<_> = tx
            .rscan(&[], None)
            .map_err(error_string)?
            .collect::<Result<_, _>>()
            .map_err(error_string)?;
        if reverse
            != expected
                .iter()
                .rev()
                .map(|(k, v)| (k.clone(), v.clone()))
                .collect::<Vec<_>>()
        {
            return Err("cross-engine reverse scan disagrees with expected rows".to_owned());
        }
        Ok(())
    })
}

fn seed(db: &Db, producer: &mut Producer<'_>, checkpoint_only: bool) -> AnyResult<()> {
    for index in 0..8 {
        transact(db, producer, |tx| {
            tx.put(&key_bytes(index), &value_bytes(index as u64))
                .map_err(error_string)?;
            Ok(())
        })?;
    }
    transact(db, producer, |tx| {
        for index in [0, 1] {
            if !tx.remove(&key_bytes(index)).map_err(error_string)? {
                return Err("cross-engine delete lost an existing key".to_owned());
            }
        }
        tx.put(&key_bytes(2), &value_bytes(20))
            .map_err(error_string)?;
        tx.put(&key_bytes(3), &value_bytes(30))
            .map_err(error_string)?;
        tx.put(&key_bytes(8), &value_bytes(80))
            .map_err(error_string)?;
        if !tx.remove(&key_bytes(8)).map_err(error_string)? {
            return Err("cross-engine insert/remove lost the staged key".to_owned());
        }
        Ok(())
    })?;
    transact(db, producer, |tx| {
        let current = tx.get(&key_bytes(2)).map_err(error_string)?;
        if current != Some(value_bytes(20)) {
            return Err("cross-engine RMW read disagrees with prior write".to_owned());
        }
        tx.put(&key_bytes(2), &value_bytes(22))
            .map_err(error_string)?;
        Ok(())
    })?;
    if checkpoint_only {
        transact(db, producer, |tx| {
            for index in 2..8 {
                if !tx.remove(&key_bytes(index)).map_err(error_string)? {
                    return Err("checkpoint-only delete missed a live row".to_owned());
                }
            }
            Ok(())
        })?;
    }
    Ok(())
}

fn update(db: &Db, producer: &mut Producer<'_>, checkpoint_only: bool) -> AnyResult<()> {
    transact(db, producer, |tx| {
        if tx.remove(&key_bytes(2)).map_err(error_string)? == checkpoint_only {
            return Err("cross-engine delete returned the wrong presence bit".to_owned());
        }
        for (index, value) in [(0, 100), (3, 300), (9, 900)] {
            tx.put(&key_bytes(index), &value_bytes(value))
                .map_err(error_string)?;
        }
        Ok(())
    })?;
    // Keep a separate one-put commit after reopening, including the unchecked
    // holder terminal in the single-producer/none profile.
    transact(db, producer, |tx| {
        tx.put(&key_bytes(10), &value_bytes(1000))
            .map_err(error_string)?;
        Ok(())
    })
}

fn timestamp(text: &str) -> AnyResult<MakoTimestamp> {
    let fields = text.split(',').collect::<Vec<_>>();
    if fields.len() != 3 {
        return Err("expected physical_us,logical,origin timestamp".to_owned());
    }
    MakoTimestamp::new(
        fields[0].parse().map_err(error_string)?,
        fields[1].parse().map_err(error_string)?,
        fields[2].parse().map_err(error_string)?,
    )
    .ok_or_else(|| "cross-engine timestamp has zero origin".to_owned())
}

pub(crate) fn run(arguments: &[String]) -> AnyResult<()> {
    if arguments.len() != 5 && arguments.len() != 7 {
        return Err("__cross_engine write|reopen|verify PATH crc32c|none concurrent|single_producer retained|checkpoint [SEQUENCE TIMESTAMP]".to_owned());
    }
    let role = arguments[0].as_str();
    if !matches!(role, "write" | "reopen" | "verify") || (role == "write") != (arguments.len() == 5)
    {
        return Err("cross-engine role and expected progress arguments disagree".to_owned());
    }
    let path = Path::new(&arguments[1]);
    if (role == "write" && path.exists()) || (role != "write" && !path.is_dir()) {
        return Err("cross-engine database path has the wrong initial state".to_owned());
    }
    let mut options = Options::default();
    options.cache.record_checksum = parse_record_checksum(&arguments[2])?;
    options.cache.foreground_mode = match arguments[3].as_str() {
        "concurrent" => ForegroundMode::Concurrent,
        "single_producer" => ForegroundMode::SingleProducer,
        _ => return Err("unknown cross-engine foreground mode".to_owned()),
    };
    let checkpoint_only = match arguments[4].as_str() {
        "retained" => false,
        "checkpoint" => true,
        _ => return Err("unknown cross-engine retention case".to_owned()),
    };
    options.cache.log_retention = if checkpoint_only {
        Duration::ZERO
    } else {
        Duration::from_secs(3600)
    };
    options.cache.gc_interval = Duration::from_millis(1);
    let expected_progress = if role == "write" {
        None
    } else {
        Some((
            arguments[5].parse::<u64>().map_err(error_string)?,
            timestamp(&arguments[6])?,
        ))
    };
    let db = Db::open(path, options).map_err(error_string)?;
    let before = db.applied_watermark();
    if let Some((sequence, timestamp)) = expected_progress {
        if before.sequence() != sequence || before.mako_timestamp() != Some(timestamp) {
            return Err(format!(
                "cross-engine recovered progress differs: {before:?}"
            ));
        }
    }
    let mut producer = if options.cache.foreground_mode == ForegroundMode::SingleProducer {
        Some(db.single_producer().map_err(error_string)?)
    } else {
        None
    };
    if role == "write" {
        validate_rows(&db, &mut producer, &BTreeMap::new())?;
        seed(&db, &mut producer, checkpoint_only)?;
    } else {
        validate_rows(
            &db,
            &mut producer,
            &expected_rows(checkpoint_only, role == "verify"),
        )?;
        if role == "reopen" {
            update(&db, &mut producer, checkpoint_only)?;
        }
    }
    let expected = expected_rows(checkpoint_only, role != "write");
    validate_rows(&db, &mut producer, &expected)?;
    drop(producer);
    db.wait_applied().map_err(error_string)?;
    let after = db.applied_watermark();
    let after_timestamp = after
        .mako_timestamp()
        .ok_or_else(|| "cross-engine writes have no HLC".to_owned())?;
    if role == "reopen" {
        let (sequence, timestamp) = expected_progress.expect("reopen progress checked");
        if after.sequence() <= sequence || after_timestamp <= timestamp {
            return Err("cross-engine new commits failed to advance sequence and HLC".to_owned());
        }
    } else if role == "verify" && after != before {
        return Err("cross-engine read-only verification changed applied progress".to_owned());
    }
    let deadline = Instant::now() + Duration::from_secs(20);
    let status = loop {
        let status = db.status().map_err(error_string)?;
        if !status.is_healthy() || status.log_gc.active_error {
            return Err(format!("cross-engine cache is unhealthy: {status:?}"));
        }
        if !checkpoint_only || status.log_gc.retained_records == 0 {
            break status;
        }
        if Instant::now() >= deadline {
            return Err("cross-engine checkpoint GC exceeded 20 seconds".to_owned());
        }
        thread::sleep(Duration::from_millis(1));
    };
    if checkpoint_only && status.log_gc.reclaimed_records != after.sequence() {
        return Err("checkpoint-only case did not reclaim every applied record".to_owned());
    }
    if !checkpoint_only && status.log_gc.retained_records != after.sequence() {
        return Err("retained-log case lost an applied record".to_owned());
    }
    db.close().map_err(error_string)?;
    println!(
        "mako-cross-engine-state-v1 engine={} role={} checksum_policy={} foreground={} retention={} sequence={} timestamp={},{},{} rows={} retained={} reclaimed={}",
        mako_cache::engine_name(), role, arguments[2], arguments[3], arguments[4],
        after.sequence(), after_timestamp.physical_us(), after_timestamp.logical(), after_timestamp.origin(),
        expected.len(), status.log_gc.retained_records, status.log_gc.reclaimed_records,
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn progress_timestamp_rejects_invalid_identity() {
        assert_eq!(
            timestamp("1,2,3").unwrap(),
            MakoTimestamp::new(1, 2, 3).unwrap()
        );
        for invalid in ["1,2", "1,2,3,4", "1,2,0", "1,-1,3", "x,2,3"] {
            assert!(timestamp(invalid).is_err());
        }
    }
}
