//! Sequential, binary-safe gateway client. This executable does not link storage.
use mako_runtime::{
    gateway_client::GatewayClient,
    wire::{ReplicaSet, RouteRequest},
};
use serde::Deserialize;
use std::{
    io::{BufRead, Read, Write},
    time::Duration,
};

#[derive(Deserialize)]
#[serde(rename_all = "snake_case")]
enum Operation {
    Get,
    Put,
    Insert,
    Remove,
    Begin,
    Commit,
    Rollback,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Request {
    op: Operation,
    #[serde(default)]
    table: Option<String>,
    #[serde(default)]
    warehouse: u32,
    #[serde(default)]
    key: Vec<u8>,
    #[serde(default)]
    value: Vec<u8>,
}
fn main() {
    srpc::logging::Log::set_level(srpc::logging::Log::WARN);
    if let Err(error) = run() {
        eprintln!("mako_client: {error}");
        std::process::exit(1);
    }
}
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args().skip(1);
    let (mut address, mut group, mut owner, mut endpoints) = (None, None, None, Vec::new());
    let mut timeout = 10u64;
    while let Some(argument) = args.next() {
        if argument == "--help" || argument == "-h" {
            println!("Usage: mako_client --address HOST:PORT [--timeout SECONDS] [--owner N --group N --replica HOST:PORT ...]\nRead one JSON request per stdin line; write one JSON result per line. Keys and values are byte arrays.\nOperations: get, put, insert, remove; supply table by name and warehouse when applicable.\nExample: {{\"op\":\"put\",\"table\":\"records\",\"warehouse\":1,\"key\":[107],\"value\":[118]}}\nSession begin/commit/rollback group auto-commit operations; rollback does not undo earlier writes.\nAn error stops the client without issuing later requests.");
            return Ok(());
        }
        let value = args
            .next()
            .ok_or_else(|| format!("{argument} requires a value"))?;
        match argument.as_str() {
            "--address" => address = Some(value),
            "--timeout" => timeout = value.parse()?,
            "--owner" => owner = Some(value.parse::<u32>()?),
            "--group" => group = Some(value.parse::<u64>()?),
            "--replica" => endpoints.push(value),
            _ => return Err(format!("unknown argument: {argument}").into()),
        }
    }
    if timeout == 0 || timeout > 3600 {
        return Err("timeout must be 1..3600 seconds".into());
    }
    let mut client = GatewayClient::connect(
        address.ok_or("provide --address")?,
        Duration::from_secs(timeout),
    )?;
    match (owner, group, endpoints.is_empty()) {
        (None, None, true) => (),
        (Some(owner), Some(group), false) => {
            client.set_replicas(owner, ReplicaSet { group, endpoints })?
        }
        _ => return Err("replica discovery requires owner, group, and replica endpoints".into()),
    }
    let stdin = std::io::stdin();
    let mut input = stdin.lock();
    let stdout = std::io::stdout();
    loop {
        let mut line = Vec::new();
        let read = (&mut input).take(65537).read_until(b'\n', &mut line)?;
        if read == 0 {
            break;
        }
        if read > 65536 {
            return Err("request line exceeds 64 KiB".into());
        }
        let request: Request = serde_json::from_slice(&line)?;
        let result = execute(&mut client, request);
        let mut output = stdout.lock();
        match result {
            Ok(result) => {
                serde_json::to_writer(&mut output, &result)?;
                output.write_all(b"\n")?;
                output.flush()?;
            }
            Err(error) => {
                serde_json::to_writer(
                    &mut output,
                    &serde_json::json!({"error":error.to_string()}),
                )?;
                output.write_all(b"\n")?;
                output.flush()?;
                return Err(error);
            }
        }
    }
    Ok(())
}
fn execute(
    client: &mut GatewayClient,
    request: Request,
) -> Result<serde_json::Value, Box<dyn std::error::Error>> {
    if matches!(
        request.op,
        Operation::Begin | Operation::Commit | Operation::Rollback
    ) {
        if request.table.is_some()
            || request.warehouse != 0
            || !request.key.is_empty()
            || !request.value.is_empty()
        {
            return Err("session operations do not take a table, key or value".into());
        }
        match request.op {
            Operation::Begin => client.begin_session()?,
            Operation::Commit => client.end_session(false)?,
            Operation::Rollback => client.end_session(true)?,
            _ => unreachable!(),
        }
        return Ok(serde_json::json!({"ok":true}));
    }
    if matches!(request.op, Operation::Get | Operation::Remove) && !request.value.is_empty() {
        return Err("get/remove do not take a value".into());
    }
    let target = client.resolve(RouteRequest {
        table: None,
        name: Some(request.table.ok_or("operation requires table")?),
        warehouse: request.warehouse,
        key: request.key.clone(),
    })?;
    Ok(match request.op {
        Operation::Get => serde_json::json!({"value":client.get(target, &request.key)?}),
        Operation::Put => {
            client.put(target, &request.key, &request.value)?;
            serde_json::json!({"ok":true})
        }
        Operation::Insert => {
            serde_json::json!({"inserted":client.insert(target, &request.key, &request.value)?})
        }
        Operation::Remove => serde_json::json!({"removed":client.remove(target, &request.key)?}),
        _ => unreachable!(),
    })
}
