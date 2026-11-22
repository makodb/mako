use rocksdb_raft_service::{
    client::Session,
    config::ClientConfig,
    wire::{Error, ErrorCode, Request, Response, MAX_WIRE_BYTES},
};
use std::{
    io::{BufRead, Write},
    sync::Arc,
    time::Duration,
};
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let mut config = None;
    let mut endpoint = None;
    let mut request = None;
    let mut wait_group = None;
    let mut timeout_ms = None;
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--config" => config = Some(args.next().ok_or("--config needs a path")?),
            "--endpoint" => endpoint = Some(args.next().ok_or("--endpoint needs an address")?),
            "--request" => request = Some(args.next().ok_or("--request needs JSON")?),
            "--wait-leader" => {
                wait_group = Some(
                    args.next()
                        .ok_or("--wait-leader needs a group")?
                        .parse::<u64>()?,
                )
            }
            "--timeout-ms" => {
                timeout_ms = Some(
                    args.next()
                        .ok_or("--timeout-ms needs milliseconds")?
                        .parse::<u64>()?,
                )
            }
            "--stdio" => {}
            "--help" => {
                println!("rocksdb_raft_client --config client.json [--endpoint HOST:PORT] [--request REQUEST_JSON | --stdio | --wait-leader GROUP] [--timeout-ms N]\nstdin/stdout use one Request/Response JSON object per line; raw byte arrays preserve NULs. Handles remain pinned to their original endpoint. The configuration is connection-only ClientConfig, not a server configuration.");
                return Ok(());
            }
            _ => return Err(format!("unknown option {arg}").into()),
        }
    }
    let mut session = Session::new(Arc::new(ClientConfig::read(
        config.ok_or("--config is required")?,
    )?));
    if let Some(ms) = timeout_ms {
        session.client.set_timeout(Duration::from_millis(ms));
    }
    if let Some(group) = wait_group {
        let leader = session
            .client
            .wait_for_leader(group, Duration::from_millis(timeout_ms.unwrap_or(30_000)))?;
        println!("{}", serde_json::json!({"leader_endpoint": leader}));
        return Ok(());
    }
    let mut execute = |line: &str| -> Result<(), Box<dyn std::error::Error>> {
        let result = if line.len() > MAX_WIRE_BYTES {
            Err(Error::new(ErrorCode::TooLarge, "request line too large"))
        } else {
            serde_json::from_str::<Request>(line)
                .map_err(|e| Error::new(ErrorCode::Invalid, e.to_string()))
                .and_then(|request| {
                    if let Some(endpoint) = &endpoint {
                        session.client.call_endpoint(endpoint, request)
                    } else {
                        session.request(request)
                    }
                })
        };
        println!("{}", serde_json::to_string(&Response::from_result(result))?);
        std::io::stdout().flush()?;
        Ok(())
    };
    if let Some(request) = request {
        execute(&request)?;
    } else {
        for line in std::io::stdin().lock().lines() {
            execute(&line?)?;
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
