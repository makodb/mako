use mako_runtime::{
    configuration,
    process::{self, Options},
    transactions,
    wire::{Error, ErrorCode},
};
fn main() {
    srpc::logging::Log::set_level(srpc::logging::Log::WARN);
    if let Err(error) = run() {
        eprintln!("mako_transactions: {error}");
        std::process::exit(1);
    }
}
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut arguments = std::env::args().skip(1);
    let (mut path, mut exit, mut stdin, mut serve, mut print) = (None, false, false, false, false);
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--config" => path = Some(arguments.next().ok_or("--config requires a file")?),
            "--exit-after-run" => exit = true,
            "--exit-on-stdin-close" => stdin = true,
            "--serve-only" => serve = true,
            "--print-config" => print = true,
            "--help" | "-h" => {
                println!("Usage: mako_transactions --config FILE [--exit-after-run | --serve-only] [--exit-on-stdin-close] [--print-config]\nRun CRUD, scan, overwrite, abort and concurrent counter transactions on the configured shards.\nOnly shard zero runs transactions; the other shards serve participants. Servers stay up until SIGINT/SIGTERM.\nSet transactions.seconds for continuous traffic, or transactions.rounds_per_worker for a fixed commit count.");
                return Ok(());
            }
            _ => return Err(format!("unknown argument: {argument}").into()),
        }
    }
    let file: transactions::File = configuration::read(path.ok_or("provide --config FILE")?)?;
    let encoded = print
        .then(|| serde_json::to_string_pretty(&file))
        .transpose()?;
    let (configs, settings) = file.into_parts()?;
    if let Some(encoded) = encoded {
        println!("{encoded}");
        return Ok(());
    }
    process::run(
        configs,
        Options {
            startup_timeout_seconds: settings.timeout_seconds,
            stdin_close: stdin,
            exit_after_run: exit,
            serve_only: serve,
            owner: Some(0),
            done_marker: "MAKO_TRANSACTIONS_DONE",
        },
        |host, config, stop| {
            let stats = transactions::run(host, config, &settings, stop)?;
            println!(
                "MAKO_TRANSACTIONS_RESULT {}",
                serde_json::to_string(&stats)
                    .map_err(|error| Error::new(ErrorCode::Fatal, error.to_string()))?
            );
            Ok(())
        },
    )
}
