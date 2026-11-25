fn main() {
    if let Err(error) = run() {
        eprintln!("mako_convert: {error}");
        std::process::exit(1);
    }
}
fn run() -> Result<(), String> {
    let mut args = std::env::args().skip(1);
    let (mut config, mut destination, mut engine) = (None, None, None);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--config" => config = Some(args.next().ok_or("--config requires a file")?),
            "--destination" => {
                destination = Some(
                    args.next()
                        .ok_or("--destination requires a new directory")?,
                )
            }
            "--engine" => {
                engine = Some(args.next().ok_or("--engine requires masstree or rocksdb")?)
            }
            "--help" | "-h" => {
                println!("Usage: mako_convert --config FILE --destination NEW_DIRECTORY --engine masstree|rocksdb\nStop every replica before conversion. Convert the whole group before restart. The source is retained; the new node.json selects the converted directory.");
                return Ok(());
            }
            _ => return Err(format!("unknown argument: {arg}")),
        }
    }
    let rocksdb = match engine.as_deref() {
        Some("rocksdb") => true,
        Some("masstree") => false,
        _ => return Err("--engine must be masstree or rocksdb".into()),
    };
    let config = mako_runtime::configuration::read(config.ok_or("--config is required")?)
        .map_err(|e| e.to_string())?;
    let report = mako_runtime::conversion::convert(
        config,
        std::path::Path::new(&destination.ok_or("--destination is required")?),
        rocksdb,
    )?;
    println!(
        "{}",
        serde_json::to_string_pretty(&report).map_err(|e| e.to_string())?
    );
    Ok(())
}
