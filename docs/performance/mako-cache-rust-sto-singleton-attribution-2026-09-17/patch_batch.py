#!/usr/bin/env python3
"""Turn the residual whole-loop RCU diagnostic into a --rcu-batch diagnostic.

Copies `rcu-source` to a scratch tree and reopens the native RCU scope every
`--rcu-batch` transactions (0 keeps the whole-loop behaviour). At K=1 this is a
per-transaction scope, which is the cell that decides whether per-transaction
scoping is worth anything.
"""
import pathlib, shutil

src = pathlib.Path("/var/tmp/mako-residual-gap.IvBRsm/rcu-source")
dst = pathlib.Path("/var/tmp/probe-mine/batch-source")
if dst.exists():
    shutil.rmtree(dst)
shutil.copytree(src, dst)
m = dst / "crates/mako-cache-bench/src/bin/engine_compare.rs"
text = m.read_text()


def sub(old, new, count=1):
    global text
    assert text.count(old) == count, (text.count(old), old[:70])
    text = text.replace(old, new)


sub(
    """fn main() {
    if let Err(error) = run() {""",
    """/// Private diagnostic: open one native RCU scope for the current backend.
#[cfg(feature = "engine-sto")]
fn open_diagnostic_scope<'a>(
    cache: &'a Option<Arc<Db>>,
    engine: &'a Option<Arc<LocalDb>>,
) -> Result<mako_engine::DiagnosticRcuScope<'a>> {
    if let Some(db) = cache {
        Ok(db.diagnostic_rcu_scope()?)
    } else {
        Ok(engine.as_ref().unwrap().diagnostic_rcu_scope()?)
    }
}

fn main() {
    if let Err(error) = run() {""",
)

sub(
    """    let use_perf = number(&mut flags, "--perf", 1)? != 0;
    let path = flags.remove("--path").map(PathBuf::from);""",
    """    let use_perf = number(&mut flags, "--perf", 1)? != 0;
    let rcu_batch = number(&mut flags, "--rcu-batch", 0)?;
    let path = flags.remove("--path").map(PathBuf::from);""",
)

sub(
    """                #[cfg(feature = "engine-sto")]
                let diagnostic_rcu_scope = if let Some(db) = &cache {
                    db.diagnostic_rcu_scope()?
                } else {
                    engine.as_ref().unwrap().diagnostic_rcu_scope()?
                };""",
    """                #[cfg(feature = "engine-sto")]
                let mut diagnostic_rcu_scope = Some(open_diagnostic_scope(&cache, &engine)?);""",
)

sub(
    """                let start = Instant::now();
                for iteration in warmup..total_per_worker {
                    put(iteration)?;
                }
                let nanoseconds = start.elapsed().as_nanos();""",
    """                let start = Instant::now();
                for iteration in warmup..total_per_worker {
                    put(iteration)?;
                    // Private diagnostic: reopen the native RCU region every
                    // `--rcu-batch` transactions. 0 keeps one region for the
                    // whole measured loop; 1 is a per-transaction scope.
                    #[cfg(feature = "engine-sto")]
                    if rcu_batch > 0 && (iteration + 1) % rcu_batch == 0 {
                        if let Some(scope) = diagnostic_rcu_scope.take() {
                            scope.close()?;
                        }
                        diagnostic_rcu_scope = Some(open_diagnostic_scope(&cache, &engine)?);
                    }
                }
                let nanoseconds = start.elapsed().as_nanos();""",
)

sub(
    """                #[cfg(feature = "engine-sto")]
                diagnostic_rcu_scope.close()?;""",
    """                #[cfg(feature = "engine-sto")]
                if let Some(scope) = diagnostic_rcu_scope.take() {
                    scope.close()?;
                }""",
)

sub(
    """    eprintln!(
        "private diagnostic: native RCU scope = {}",""",
    """    eprintln!(
        "private diagnostic: native RCU scope = {} (rcu_batch={rcu_batch})",""",
)

m.write_text(text)
print("patched", m)
