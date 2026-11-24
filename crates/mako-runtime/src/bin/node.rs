use mako_runtime::node::{Config, RunningNode};
use std::{
    io::{BufRead, Write},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    time::{Duration, Instant},
};

static STOP: AtomicBool = AtomicBool::new(false);
extern "C" fn stop(_: libc::c_int) {
    STOP.store(true, Ordering::Relaxed);
}
fn main() {
    if let Err(error) = run() {
        eprintln!("mako_node: {error}");
        std::process::exit(1);
    }
}
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut config = None;
    let mut exit_on_stdin = false;
    let mut arguments = std::env::args().skip(1);
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--config" => config = Some(arguments.next().ok_or("--config requires a file")?),
            "--exit-on-stdin-close" => exit_on_stdin = true,
            "--help" | "-h" => {
                println!("Usage: mako_node --config FILE [--exit-on-stdin-close]");
                return Ok(());
            }
            _ => return Err(format!("unknown argument: {argument}").into()),
        }
    }
    let config: Config =
        serde_json::from_reader(std::fs::File::open(config.ok_or("--config is required")?)?)?;
    // SAFETY: the signal callback only writes a lock-free atomic flag. Its
    // static address remains valid throughout this standalone process.
    unsafe {
        if libc::signal(libc::SIGTERM, stop as *const () as libc::sighandler_t) == libc::SIG_ERR
            || libc::signal(libc::SIGINT, stop as *const () as libc::sighandler_t) == libc::SIG_ERR
        {
            return Err(std::io::Error::last_os_error().into());
        }
    }
    let (closed, input) = mpsc::sync_channel(1);
    if exit_on_stdin {
        std::thread::spawn(move || {
            for line in std::io::stdin().lock().lines() {
                match line {
                    Ok(line) if line.trim() != "stop" => (),
                    _ => break,
                }
            }
            let _ = closed.send(());
        });
    }
    let mut node = RunningNode::start(config)?;
    println!("MAKO_LISTEN {}", node.address());
    std::io::stdout().flush()?;
    let deadline = Instant::now() + Duration::from_secs(300);
    let mut announced = false;
    while !STOP.load(Ordering::Relaxed) {
        node.health()?;
        if !announced && node.ready() {
            println!("MAKO_READY {}", node.address());
            std::io::stdout().flush()?;
            announced = true;
        }
        if !announced && Instant::now() >= deadline {
            return Err("shard readiness deadline elapsed".into());
        }
        if exit_on_stdin {
            match input.recv_timeout(Duration::from_millis(50)) {
                Ok(()) | Err(mpsc::RecvTimeoutError::Disconnected) => break,
                Err(mpsc::RecvTimeoutError::Timeout) => (),
            }
        } else {
            std::thread::sleep(Duration::from_millis(50));
        }
    }
    node.shutdown();
    Ok(())
}
