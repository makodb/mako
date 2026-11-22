use rocksdb_raft_service::{config::Config, server::RunningServer};
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let mut config = None;
    let mut bootstrap = false;
    let mut stdin_shutdown = false;
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--config" => config = Some(args.next().ok_or("--config needs a path")?),
            "--bootstrap" => bootstrap = true,
            "--stdin-shutdown" => stdin_shutdown = true,
            "--help" => {
                println!(
                    "rocksdb_raft_server --config CONFIG.json [--bootstrap] [--stdin-shutdown]"
                );
                return Ok(());
            }
            _ => return Err(format!("unknown option {arg}").into()),
        }
    }
    let config = Config::read(config.ok_or("--config is required")?)?;
    let endpoint = config.listen.clone();
    let _server = RunningServer::start(config, bootstrap)?;
    println!("{{\"listening\":{}}}", serde_json::to_string(&endpoint)?);
    if stdin_shutdown {
        let mut line = String::new();
        std::io::stdin().read_line(&mut line)?;
    } else {
        loop {
            std::thread::park();
        }
    }
    Ok(())
}
fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
