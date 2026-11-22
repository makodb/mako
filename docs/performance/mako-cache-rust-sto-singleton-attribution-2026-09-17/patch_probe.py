#!/usr/bin/env python3
"""Copy the residual probe harness and add an --items knob (engine mode only)."""
import pathlib, shutil, sys

src = pathlib.Path("/var/tmp/mako-residual-gap.IvBRsm/probe")
dst = pathlib.Path("/var/tmp/probe-mine/probe-items")
if dst.exists():
    shutil.rmtree(dst)
shutil.copytree(src, dst)
m = dst / "src/main.rs"
text = m.read_text()

def sub(old, new, count=1):
    global text
    assert text.count(old) == count, (text.count(old), old[:60])
    text = text.replace(old, new)

sub(
    """fn number(flags: &mut BTreeMap<String, String>, name: &str, default: u64) -> Result<u64> {""",
    """/// Last iteration that writes `key_index` when every iteration stages
/// `items` distinct keys `key(worker, iteration * items + item)`.
fn expected_iteration_multi(key_index: u64, final_count: u64, items: u64) -> u64 {
    let mut iteration = final_count;
    let mut budget = KEYS + items;
    while iteration > 0 && budget > 0 {
        iteration -= 1;
        budget -= 1;
        let base = iteration * items;
        for item in 0..items {
            if (base + item) % KEYS == key_index {
                return iteration;
            }
        }
    }
    iteration
}

fn number(flags: &mut BTreeMap<String, String>, name: &str, default: u64) -> Result<u64> {""",
)

sub(
    """    let use_perf = number(&mut flags, "--perf", 1)? != 0;
    let path = flags.remove("--path").map(PathBuf::from);
    if !flags.is_empty() || workers == 0 || workers > 32 || iterations == 0 || warmup < KEYS {
        return Err("invalid arguments, workers must be 1..32 and warmup at least 256".into());
    }""",
    """    let use_perf = number(&mut flags, "--perf", 1)? != 0;
    let items = number(&mut flags, "--items", 1)?;
    let path = flags.remove("--path").map(PathBuf::from);
    if !flags.is_empty()
        || workers == 0
        || workers > 32
        || iterations == 0
        || warmup < KEYS
        || items == 0
        || items > KEYS
    {
        return Err(
            "invalid arguments, workers must be 1..32, warmup at least 256, items 1..=256".into(),
        );
    }
    if items != 1 && mode != "engine" {
        return Err("--items above one is available in engine mode only".into());
    }""",
)

sub(
    """                        tx.put(&table, &key, &value)?;
                        if logged {""",
    """                        tx.put(&table, &key, &value)?;
                        for item in 1..items {
                            let item_key = key(worker, iteration * items + item);
                            tx.put(&table, &item_key, &value)?;
                        }
                        if logged {""",
)

sub(
    """                    let expected = value(worker, expected_iteration(index, total_per_worker));""",
    """                    let expected = value(
                        worker,
                        if items == 1 {
                            expected_iteration(index, total_per_worker)
                        } else {
                            expected_iteration_multi(index, total_per_worker, items)
                        },
                    );""",
)

sub(
    r'\"verified_keys\":{},\"drained\":false}}",',
    r'\"verified_keys\":{},\"drained\":false,\"items_per_transaction\":{items}}",',
)

m.write_text(text)
print("patched", m)
