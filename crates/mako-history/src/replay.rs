use std::fmt::Write;

use crate::{
    History, Observation, Operation, ScanDirection, TerminalKind, TerminalOutcome, TimedOperation,
};

pub(crate) fn hex(bytes: &[u8]) -> String {
    let mut output = String::with_capacity(2 + bytes.len() * 2);
    output.push_str("0x");
    for byte in bytes {
        write!(&mut output, "{byte:02x}").expect("writing to String cannot fail");
    }
    output
}

pub(crate) fn render(history: &History) -> String {
    let mut output = String::from("mako-history-v1\n");
    for (key, value) in &history.initial_state {
        writeln!(
            &mut output,
            "initial {} {} {}",
            key.table,
            hex(&key.key),
            hex(value)
        )
        .expect("writing to String cannot fail");
    }
    let mut transactions: Vec<_> = history.transactions.iter().collect();
    transactions.sort_by_key(|transaction| transaction.id);
    for transaction in transactions {
        writeln!(
            &mut output,
            "txn {} begin {} {}",
            transaction.id,
            transaction.begin.invocation,
            response(transaction.begin.response)
        )
        .expect("writing to String cannot fail");
        for (index, operation) in transaction.operations.iter().enumerate() {
            writeln!(
                &mut output,
                "txn {} op {} {} {} {} => {}",
                transaction.id,
                index,
                operation.interval.invocation,
                response(operation.interval.response),
                render_operation(operation),
                render_observation(operation.observation.as_ref())
            )
            .expect("writing to String cannot fail");
        }
        match &transaction.terminal {
            Some(terminal) => {
                writeln!(
                    &mut output,
                    "txn {} terminal {} {} {} {}",
                    transaction.id,
                    match terminal.kind {
                        TerminalKind::Commit => "commit",
                        TerminalKind::Abort => "abort",
                    },
                    terminal.interval.invocation,
                    response(terminal.interval.response),
                    terminal
                        .outcome
                        .map(render_terminal_outcome)
                        .unwrap_or("pending")
                )
                .expect("writing to String cannot fail");
            }
            None => {
                writeln!(&mut output, "txn {} active", transaction.id)
                    .expect("writing to String cannot fail");
            }
        }
    }
    if let Some(final_state) = &history.observed_final_state {
        for (key, value) in final_state {
            writeln!(
                &mut output,
                "final {} {} {}",
                key.table,
                hex(&key.key),
                hex(value)
            )
            .expect("writing to String cannot fail");
        }
    } else {
        output.push_str("final unobserved\n");
    }
    output
}

fn response(response: Option<u64>) -> String {
    response
        .map(|tick| tick.to_string())
        .unwrap_or_else(|| "pending".to_owned())
}

fn render_operation(operation: &TimedOperation) -> String {
    match &operation.operation {
        Operation::Get { table, key } => format!("get {table} {}", hex(key)),
        Operation::Put { table, key, value } => {
            format!("put {table} {} {}", hex(key), hex(value))
        }
        Operation::Insert { table, key, value } => {
            format!("insert {table} {} {}", hex(key), hex(value))
        }
        Operation::Remove { table, key } => format!("remove {table} {}", hex(key)),
        Operation::Scan {
            table,
            lower,
            upper,
            direction,
        } => format!(
            "scan {table} {} {} {}",
            hex(lower),
            upper
                .as_deref()
                .map(hex)
                .unwrap_or_else(|| "none".to_owned()),
            match direction {
                ScanDirection::Forward => "forward",
                ScanDirection::Reverse => "reverse",
            }
        ),
    }
}

fn render_observation(observation: Option<&Observation>) -> String {
    match observation {
        None => "pending".to_owned(),
        Some(Observation::Get(None)) => "get missing".to_owned(),
        Some(Observation::Get(Some(value))) => format!("get {}", hex(value)),
        Some(Observation::Put { created }) => format!("put created={created}"),
        Some(Observation::Insert { inserted }) => format!("insert inserted={inserted}"),
        Some(Observation::Remove { existed }) => format!("remove existed={existed}"),
        Some(Observation::Scan(rows)) => {
            let rows = rows
                .iter()
                .map(|row| format!("{}={}", hex(&row.key), hex(&row.value)))
                .collect::<Vec<_>>()
                .join(",");
            format!("scan [{rows}]")
        }
        Some(Observation::Conflict) => "conflict".to_owned(),
    }
}

fn render_terminal_outcome(outcome: TerminalOutcome) -> &'static str {
    match outcome {
        TerminalOutcome::Committed => "committed",
        TerminalOutcome::Aborted => "aborted",
        TerminalOutcome::Conflict => "conflict",
    }
}
