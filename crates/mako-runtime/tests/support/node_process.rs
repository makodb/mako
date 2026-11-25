#![allow(dead_code)] // Different process suites exercise different lifecycle operations.
use mako_runtime::node::Config;
use std::{
    io::{BufRead, BufReader},
    path::PathBuf,
    process::{Child, Command, Stdio},
    sync::mpsc,
    time::{Duration, Instant},
};
pub struct Process {
    child: Child,
    config: PathBuf,
    pub address: String,
    ready: mpsc::Receiver<String>,
}
impl Process {
    pub fn start(config: Config) -> Self {
        let process = Self::spawn(config);
        process.wait_ready();
        process
    }
    pub fn spawn(config: Config) -> Self {
        Self::spawn_with_env(config, &[])
    }
    pub fn spawn_with_env(config: Config, env: &[(&str, &str)]) -> Self {
        static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "mako-node-{}-{}-{}.json",
            std::process::id(),
            config.owner,
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        ));
        std::fs::write(&path, serde_json::to_vec(&config).unwrap()).unwrap();
        let mut child = Command::new(env!("CARGO_BIN_EXE_mako_node"))
            .arg("--config")
            .arg(&path)
            .arg("--exit-on-stdin-close")
            .envs(env.iter().copied())
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .unwrap();
        let output = child.stdout.take().unwrap();
        let (send, recv) = mpsc::channel();
        let (listening, bound) = mpsc::channel();
        let mut process = Self {
            child,
            config: path,
            address: String::new(),
            ready: recv,
        };
        std::thread::spawn(move || {
            for line in BufReader::new(output).lines() {
                let Ok(line) = line else { break };
                if let Some(address) = line.strip_prefix("MAKO_READY ") {
                    let _ = send.send(address.to_owned());
                }
                if let Some(address) = line.strip_prefix("MAKO_LISTEN ") {
                    let _ = listening.send(address.to_owned());
                }
            }
        });
        process.address = bound
            .recv_timeout(Duration::from_secs(30))
            .expect("child listener");
        process
    }
    pub fn wait_ready(&self) {
        assert_eq!(
            self.ready
                .recv_timeout(Duration::from_secs(30))
                .expect("child readiness"),
            self.address
        );
    }
    pub fn stop(&mut self) {
        self.child.stdin.take();
        let deadline = Instant::now() + Duration::from_secs(10);
        loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                assert!(status.success(), "child exit: {status}");
                break;
            }
            assert!(Instant::now() < deadline, "child shutdown deadline");
            std::thread::sleep(Duration::from_millis(20));
        }
    }
    #[cfg(feature = "logging")]
    pub fn crash(&mut self) {
        self.child.kill().unwrap();
        assert!(!self.child.wait().unwrap().success());
    }
}

impl Drop for Process {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_file(&self.config);
    }
}
