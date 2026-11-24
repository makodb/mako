use mako_runtime::{
    benchmark,
    node::{Config, RunningNode},
};
use serde::Deserialize;
use std::{
    io::{BufRead, Write},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc, Arc,
    },
    time::{Duration, Instant},
};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct FileConfig {
    node: Config,
    #[serde(default)]
    benchmark: benchmark::Config,
}
static STOP: AtomicBool = AtomicBool::new(false);
extern "C" fn stop(_: libc::c_int) {
    STOP.store(true, Ordering::Relaxed);
}
fn main() {
    if let Err(error) = run() {
        eprintln!("mako_bench: {error}");
        std::process::exit(1);
    }
}
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut path = None;
    let mut load_only = false;
    let mut serve_only = false;
    let mut exit_after_run = false;
    let mut stdin_close = false;
    let mut args = std::env::args().skip(1);
    while let Some(argument) = args.next() {
        match argument.as_str() {
            "--config" => path = Some(args.next().ok_or("--config requires a file")?),
            "--load-only" => load_only = true,
            "--serve-only" => serve_only = true,
            "--exit-after-run" => exit_after_run = true,
            "--exit-on-stdin-close" => stdin_close = true,
            "--help" | "-h" => {
                println!("Usage: mako_bench --config FILE [--load-only | --serve-only] [--exit-after-run] [--exit-on-stdin-close]\nRuns native TPC-C workers on each shard leader; replicas remain available.\nAfter the run, keeps serving until SIGINT, SIGTERM or controlled stdin shutdown.\n--exit-after-run is restricted to a single unreplicated shard.");
                return Ok(());
            }
            _ => return Err(format!("unknown argument: {argument}").into()),
        }
    }
    if load_only && serve_only {
        return Err("choose --load-only or --serve-only".into());
    }
    if serve_only && exit_after_run {
        return Err("--serve-only has no run to exit after".into());
    }
    let config: FileConfig =
        serde_json::from_reader(std::fs::File::open(path.ok_or("--config is required")?)?)?;
    config.benchmark.validate(&config.node)?;
    if exit_after_run && (config.node.nodes != 1 || config.node.replication.is_some()) {
        return Err("--exit-after-run requires one unreplicated shard; stop distributed servers after all clients finish".into());
    }
    // SAFETY: callback only writes a lock-free atomic at a static address.
    unsafe {
        if libc::signal(libc::SIGTERM, stop as *const () as libc::sighandler_t) == libc::SIG_ERR
            || libc::signal(libc::SIGINT, stop as *const () as libc::sighandler_t) == libc::SIG_ERR
        {
            return Err(std::io::Error::last_os_error().into());
        }
    }
    let cancelled = Arc::new(AtomicBool::new(false));
    let (send_input, input) = mpsc::channel();
    if stdin_close {
        std::thread::spawn(move || {
            for line in std::io::stdin().lock().lines() {
                match line {
                    Ok(line) if line.trim() != "stop" => (),
                    _ => break,
                }
            }
            let _ = send_input.send(());
        });
    }
    let mut node = RunningNode::start(config.node.clone())?;
    println!("MAKO_LISTEN {}", node.address());
    std::io::stdout().flush()?;
    let host = benchmark::Host::from(&node);
    let deadline = Instant::now() + Duration::from_secs(config.benchmark.startup_timeout_seconds);
    let result = std::thread::scope(|scope| -> Result<(), Box<dyn std::error::Error>> {
        let (completed, completion) = mpsc::channel();
        let mut launched = false;
        let mut announced = false;
        let mut task_handle = None;
        let result = (|| -> Result<(), Box<dyn std::error::Error>> {
            loop {
                if STOP.load(Ordering::Relaxed) || (stdin_close && input.try_recv().is_ok()) {
                    break;
                }
                node.health()?;
                if !announced && node.ready() {
                    println!("MAKO_READY {}", node.address());
                    std::io::stdout().flush()?;
                    announced = true;
                }
                if !announced && Instant::now() >= deadline {
                    return Err("shard readiness deadline elapsed".into());
                }
                let status = node.store.log.replica_status(config.node.owner);
                if announced && status.leader && status.healthy && !serve_only && !launched {
                    launched = true;
                    let host = &host;
                    let config = &config;
                    let cancelled = cancelled.clone();
                    let completed = completed.clone();
                    task_handle = Some(scope.spawn(move || {
                        let task: mako_runtime::wire::Result<Option<benchmark::Stats>> = (|| {
                            let mut printed = 0;
                            let loaded = benchmark::prepare(
                                host,
                                &config.node,
                                &config.benchmark,
                                &cancelled,
                                |progress| {
                                    if progress.rows >= printed + 100_000 || progress.complete {
                                        println!(
                                            "MAKO_LOAD_PROGRESS {} {}",
                                            progress.rows, progress.complete
                                        );
                                        printed = progress.rows;
                                    }
                                    Ok(())
                                },
                            )?;
                            println!("MAKO_LOADED {} {}", loaded.rows, loaded.already_loaded);
                            if load_only {
                                Ok(None)
                            } else {
                                benchmark::run(host, &config.node, &config.benchmark, cancelled)
                                    .map(Some)
                            }
                        })(
                        );
                        let _ = completed.send(task);
                    }));
                }
                match completion.try_recv() {
                    Ok(result) => {
                        if let Some(stats) = result? {
                            println!("MAKO_BENCH_RESULT {}", serde_json::to_string(&stats)?);
                            println!("transactions/sec: {:.2}", stats.transactions_per_second());
                        }
                        println!("MAKO_BENCH_DONE");
                        std::io::stdout().flush()?;
                        if exit_after_run {
                            break;
                        }
                    }
                    Err(mpsc::TryRecvError::Empty) => (),
                    Err(mpsc::TryRecvError::Disconnected) => {
                        return Err("benchmark completion channel closed".into())
                    }
                }
                if task_handle.as_ref().is_some_and(|task| task.is_finished()) {
                    task_handle
                        .take()
                        .unwrap()
                        .join()
                        .map_err(|_| "benchmark task panicked")?;
                }
                std::thread::sleep(Duration::from_millis(20));
            }
            Ok(())
        })();
        cancelled.store(true, Ordering::Relaxed);
        if let Some(task) = task_handle {
            task.join().map_err(|_| "benchmark task panicked")?;
        }
        result
    });
    node.shutdown();
    result
}
