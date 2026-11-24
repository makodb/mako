use mako_runtime::{
    configuration,
    node::Config,
    process::{self, Options},
    redis, transactions,
};
use serde::Deserialize;
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct File {
    node: Config,
    redis_listen: String,
}
fn main() {
    srpc::logging::Log::set_level(srpc::logging::Log::WARN);
    if let Err(error) = run() {
        eprintln!("mako_redis: {error}");
        std::process::exit(1);
    }
}
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let (mut path, mut stdin) = (None, false);
    while let Some(argument) = args.next() {
        match argument.as_str() {
            "--config" => path = Some(args.next().ok_or("--config requires a file")?),
            "--exit-on-stdin-close" => stdin = true,
            "--help" | "-h" => {
                println!("Usage: mako_redis --config FILE [--exit-on-stdin-close]\nServe PING, GET, SET, DEL, MULTI, EXEC and DISCARD over RESP arrays.\nMULTI/EXEC uses one native transaction; conflicts return a nil EXEC result.\nEach shard uses one records warehouse. Keys route by a stable byte hash.\nSIGINT/SIGTERM stops the listener and joins all workers.");
                return Ok(());
            }
            _ => return Err(format!("unknown argument: {argument}").into()),
        }
    }
    let file: File = configuration::read(path.ok_or("provide --config FILE")?)?;
    transactions::Settings::default().validate(&file.node)?;
    process::run(
        vec![file.node],
        Options {
            startup_timeout_seconds: 120,
            stdin_close: stdin,
            exit_after_run: false,
            serve_only: false,
            owner: None,
            done_marker: "MAKO_REDIS_DONE",
        },
        |host, node, stop| redis::serve(host, node, &file.redis_listen, stop),
    )
}
