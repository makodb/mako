//! Process supervision shared by benchmark and transaction examples.
use crate::node::{Config, Handle, RunningNode};
use std::{
    io::{BufRead, Write},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc, Arc,
    },
    time::{Duration, Instant},
};

static STOP: AtomicBool = AtomicBool::new(false);
extern "C" fn stop(_: libc::c_int) {
    STOP.store(true, Ordering::Relaxed);
}

pub struct Options {
    pub startup_timeout_seconds: u64,
    pub stdin_close: bool,
    pub exit_after_run: bool,
    pub serve_only: bool,
    pub owner: Option<u32>,
    pub done_marker: &'static str,
}

/// Called once by a process entry point. The signal handler and stdin reader
/// belong to that process, not to individual database instances.
pub fn run(
    configs: Vec<Config>,
    options: Options,
    application: impl Fn(&Handle, &Config, Arc<AtomicBool>) -> crate::wire::Result<()> + Sync,
) -> Result<(), Box<dyn std::error::Error>> {
    if configs.is_empty() || options.startup_timeout_seconds == 0 {
        return Err("empty node configuration or readiness timeout".into());
    }
    let expected = configs
        .iter()
        .filter(|node| options.owner.is_none_or(|owner| owner == node.owner))
        .count();
    if options.exit_after_run
        && (options.serve_only
            || expected == 0
            || configs.len() != configs[0].nodes as usize
            || configs.iter().any(|node| node.replication.is_some()))
    {
        return Err("--exit-after-run requires all unreplicated shards in this process and an enabled application".into());
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
    if options.stdin_close {
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
    let mut nodes = Vec::new();
    for config in &configs {
        let node = RunningNode::start(config.clone())?;
        println!("MAKO_LISTEN {}", node.address());
        nodes.push(node);
    }
    std::io::stdout().flush()?;
    let hosts: Vec<_> = nodes.iter().map(Handle::from).collect();
    let deadline = Instant::now() + Duration::from_secs(options.startup_timeout_seconds);
    let result = std::thread::scope(|scope| -> Result<(), Box<dyn std::error::Error>> {
        let (completed, completion) = mpsc::channel();
        let mut launched = vec![false; nodes.len()];
        let mut announced = vec![false; nodes.len()];
        let mut tasks = Vec::new();
        let mut finished = 0;
        let result = (|| -> Result<(), Box<dyn std::error::Error>> {
            loop {
                if STOP.load(Ordering::Relaxed) || (options.stdin_close && input.try_recv().is_ok())
                {
                    break;
                }
                for (index, node) in nodes.iter().enumerate() {
                    node.health()?;
                    if !announced[index] && node.ready() {
                        println!("MAKO_READY {}", node.address());
                        std::io::stdout().flush()?;
                        announced[index] = true;
                    }
                    if !announced[index] && Instant::now() >= deadline {
                        return Err("shard readiness deadline elapsed".into());
                    }
                    let status = node.store.log.replica_status(configs[index].owner);
                    if announced[index]
                        && status.leader
                        && status.healthy
                        && !options.serve_only
                        && options
                            .owner
                            .is_none_or(|owner| owner == configs[index].owner)
                        && !launched[index]
                    {
                        launched[index] = true;
                        let host = &hosts[index];
                        let config = &configs[index];
                        let application = &application;
                        let cancelled = cancelled.clone();
                        let completed = completed.clone();
                        tasks.push(Some(scope.spawn(move || {
                            let task = application(host, config, cancelled);
                            let _ = completed.send(task);
                        })));
                    }
                }
                match completion.try_recv() {
                    Ok(result) => {
                        result?;
                        println!("{}", options.done_marker);
                        std::io::stdout().flush()?;
                        finished += 1;
                        if options.exit_after_run && finished == expected {
                            break;
                        }
                    }
                    Err(mpsc::TryRecvError::Empty) => (),
                    Err(mpsc::TryRecvError::Disconnected) => {
                        return Err("benchmark completion channel closed".into())
                    }
                }
                for task in &mut tasks {
                    if task.as_ref().is_some_and(|task| task.is_finished()) {
                        task.take()
                            .unwrap()
                            .join()
                            .map_err(|_| "benchmark task panicked")?;
                    }
                }
                std::thread::sleep(Duration::from_millis(20));
            }
            Ok(())
        })();
        cancelled.store(true, Ordering::Relaxed);
        let mut failed = false;
        for task in tasks.into_iter().flatten() {
            if task.join().is_err() {
                failed = true;
            }
        }
        if failed {
            return Err("benchmark task panicked".into());
        }
        result
    });
    for node in &mut nodes {
        node.shutdown();
    }
    result
}
