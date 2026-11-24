use mako_runtime::{
    rpc::Network,
    wire::{Message, Reply},
};
use mako_sharding::{
    types::{Status, TxnId},
    wire as control,
};
use std::time::Duration;
const HELP: &str = "Usage:\n  mako_admin begin ADDRESS CLIENT SEQUENCE TABLE LO_HEX HI_HEX|inf SOURCE DESTINATION\n  mako_admin poll ADDRESS CLIENT SEQUENCE\n  mako_admin abort ADDRESS CLIENT SEQUENCE\n\nADDRESS is the native coordinator's shard listener, without a port offset.\nCLIENT/SEQUENCE identify the intent. Preserve both on every retry.\nTABLE is the canonical catalog name. Bounds are hexadecimal bytes; an empty\nargument is the empty bound, and inf is unbounded. Warehouse coordinates use\nfour big-endian bytes. Poll retains the result. Abort cannot undo publication.";
fn hex(text: &str) -> Result<Vec<u8>, Box<dyn std::error::Error>> {
    if !text.is_ascii() || !text.len().is_multiple_of(2) || text.len() > 2048 {
        return Err("invalid hexadecimal range bound".into());
    }
    (0..text.len())
        .step_by(2)
        .map(|at| u8::from_str_radix(&text[at..at + 2], 16).map_err(Into::into))
        .collect()
}
fn main() {
    srpc::logging::Log::set_level(srpc::logging::Log::WARN);
    if let Err(error) = run() {
        eprintln!("mako_admin: {error}");
        std::process::exit(1);
    }
}
fn run() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = std::env::args().skip(1).collect();
    if args.as_slice() == ["--help"] || args.as_slice() == ["-h"] {
        println!("{HELP}");
        return Ok(());
    }
    let action = args.first().map(String::as_str);
    if !matches!(
        (action, args.len()),
        (Some("begin"), 9) | (Some("poll" | "abort"), 4)
    ) {
        return Err(HELP.into());
    }
    let nonce = TxnId {
        client: args[2].parse()?,
        sequence: args[3].parse()?,
    };
    let (operation, payload) = match action {
        Some("begin") => (
            control::ADMIN_BEGIN,
            control::encode_begin(&control::Begin {
                nonce,
                table_name: args[4].as_bytes().to_vec(),
                lo: hex(&args[5])?,
                hi: if args[6] == "inf" {
                    None
                } else {
                    Some(hex(&args[6])?)
                },
                source: args[7].parse()?,
                destination: args[8].parse()?,
            }),
        ),
        Some("poll") => (control::ADMIN_POLL, control::encode_nonce(nonce)),
        _ => (control::ADMIN_ABORT, control::encode_nonce(nonce)),
    };
    let payload = payload.map_err(|status| format!("invalid request: {status:?}"))?;
    let reply = Network::default()
        .call(
            &args[1],
            Message::Migration {
                owner: 0,
                operation,
                payload,
            },
            Duration::from_secs(10),
        )
        .map_err(|error| {
            format!(
                "{error}; retain nonce={}:{} and poll or repeat the same intent",
                nonce.client, nonce.sequence
            )
        })?;
    let (generation, outcome) = match reply {
        Reply::Migration { status, payload } if status == Status::Ok as u32 => {
            control::decode_outcome(&payload)
                .map_err(|status| format!("invalid outcome: {status:?}"))?
        }
        Reply::Migration { status, .. } => {
            return Err(format!(
                "migration {:?}; nonce={}:{}; retain the nonce for retry",
                control::status(status),
                nonce.client,
                nonce.sequence
            )
            .into())
        }
        reply => {
            return Err(format!(
                "unexpected migration reply {reply:?}; retain nonce={}:{}",
                nonce.client, nonce.sequence
            )
            .into())
        }
    };
    println!(
        "nonce={}:{} generation={} outcome={}",
        nonce.client,
        nonce.sequence,
        generation,
        match outcome {
            0 => "pending",
            1 => "committed",
            _ => "aborted",
        }
    );
    if outcome == 2 {
        return Err("migration aborted".into());
    }
    Ok(())
}
