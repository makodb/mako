use mako_runtime::{
    benchmark,
    configuration::cli::{Args, HELP},
    process::{self, Options},
    wire::{Error, ErrorCode},
};
fn main() {
    srpc::logging::Log::set_level(srpc::logging::Log::WARN);
    if let Err(error) = run() {
        eprintln!("mako_bench: {error}");
        std::process::exit(1);
    }
}
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let args = Args::parse(std::env::args().skip(1))?;
    if args.help {
        println!("{HELP}");
        return Ok(());
    }
    let (file, notices) = args.load()?;
    for notice in notices {
        eprintln!("{notice}");
    }
    if args.print_config {
        println!("{}", serde_json::to_string_pretty(&file)?);
        return Ok(());
    }
    let (configs, settings) = file.into_parts()?;
    process::run(
        configs,
        Options {
            startup_timeout_seconds: settings.startup_timeout_seconds,
            stdin_close: args.stdin_close,
            exit_after_run: args.exit_after_run,
            serve_only: args.serve_only,
            owner: None,
            done_marker: "MAKO_BENCH_DONE",
        },
        |host, config, cancelled| {
            let mut printed = 0;
            let loaded = benchmark::prepare(host, config, &settings, &cancelled, |progress| {
                if progress.rows >= printed + 100_000 || progress.complete {
                    println!(
                        "MAKO_LOAD_PROGRESS {} {} {}",
                        config.owner, progress.rows, progress.complete
                    );
                    printed = progress.rows;
                }
                Ok(())
            })?;
            println!(
                "MAKO_LOADED {} {} {}",
                config.owner, loaded.rows, loaded.already_loaded
            );
            if !args.load_only {
                let stats = benchmark::run(host, config, &settings, cancelled)?;
                println!(
                    "MAKO_BENCH_RESULT {}",
                    serde_json::to_string(&stats)
                        .map_err(|error| Error::new(ErrorCode::Fatal, error.to_string()))?
                );
                println!("transactions/sec: {:.2}", stats.transactions_per_second());
            }
            Ok(())
        },
    )
}
