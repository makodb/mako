use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::fmt::Write as _;
use std::fs;
use std::path::PathBuf;

const DEFINITIONS_BEGIN: &str = "/* MAKO_LOCAL_STATUS_DEFINITIONS_BEGIN */";
const DEFINITIONS_END: &str = "/* MAKO_LOCAL_STATUS_DEFINITIONS_END */";
const MANIFEST_BEGIN: &str = "/* MAKO_LOCAL_STATUS_MANIFEST_BEGIN */";
const MANIFEST_END: &str = "/* MAKO_LOCAL_STATUS_MANIFEST_END */";

const ABI_V0_STATUS_NAMES: [&str; 20] = [
    "OK",
    "CONFLICT",
    "NOT_ATTACHED",
    "WRONG_THREAD",
    "TXN_ALREADY_ACTIVE",
    "TXN_FINISHED",
    "WRONG_DB_OR_TABLE",
    "INVALID_ARGUMENT",
    "THREAD_LIMIT",
    "BUSY",
    "OUT_OF_MEMORY",
    "INTERNAL",
    "DUPLICATE_WRITE",
    "TXN_TOO_LARGE",
    "VALUE_TOO_LARGE",
    "COMMIT_HOOK_REJECTED",
    "TIMESTAMP_EXHAUSTED",
    "BUFFER_TOO_SMALL",
    "FEATURE_UNAVAILABLE",
    "WORKER_POISONED",
];

#[derive(Debug)]
struct ManifestRow {
    short_name: String,
    c_symbol: String,
    message: String,
}

#[derive(Debug)]
struct Status {
    short_name: String,
    c_symbol: String,
    message: String,
    rust_variant: String,
    code: i32,
}

fn main() {
    if let Err(error) = run() {
        panic!("mako-local-sys status generation failed: {error}");
    }
}

fn run() -> Result<(), String> {
    let manifest_dir = PathBuf::from(
        env::var_os("CARGO_MANIFEST_DIR")
            .ok_or_else(|| "Cargo did not set CARGO_MANIFEST_DIR".to_owned())?,
    );
    let header = manifest_dir.join("../../src/mako/storage/mako_local_abi.h");
    println!("cargo:rerun-if-changed={}", header.display());

    let source = fs::read_to_string(&header)
        .map_err(|error| format!("could not read {}: {error}", header.display()))?;
    verify_abi_version(&source)?;

    let definition_region = marked_region(&source, DEFINITIONS_BEGIN, DEFINITIONS_END)?;
    let manifest_region = marked_region(&source, MANIFEST_BEGIN, MANIFEST_END)?;
    let definitions = parse_definitions(definition_region)?;
    let rows = parse_manifest(manifest_region)?;
    let statuses = validate_statuses(&definitions, rows)?;

    let generated = generate_rust(&statuses);
    let out_dir = PathBuf::from(
        env::var_os("OUT_DIR").ok_or_else(|| "Cargo did not set OUT_DIR".to_owned())?,
    );
    let destination = out_dir.join("mako_local_statuses.rs");
    fs::write(&destination, generated)
        .map_err(|error| format!("could not write {}: {error}", destination.display()))
}

fn verify_abi_version(source: &str) -> Result<(), String> {
    let declarations: Vec<_> = source
        .lines()
        .map(str::trim)
        .filter(|line| line.starts_with("#define MAKO_LOCAL_ABI_VERSION"))
        .collect();
    if declarations.as_slice() != ["#define MAKO_LOCAL_ABI_VERSION 0u"] {
        return Err(format!(
            "expected exactly `#define MAKO_LOCAL_ABI_VERSION 0u`, found {declarations:?}"
        ));
    }
    Ok(())
}

fn marked_region<'a>(source: &'a str, begin: &str, end: &str) -> Result<&'a str, String> {
    let begin_positions: Vec<_> = source
        .match_indices(begin)
        .map(|(index, _)| index)
        .collect();
    let end_positions: Vec<_> = source.match_indices(end).map(|(index, _)| index).collect();
    if begin_positions.len() != 1 || end_positions.len() != 1 {
        return Err(format!(
            "expected one `{begin}` and one `{end}` marker, found {} and {}",
            begin_positions.len(),
            end_positions.len()
        ));
    }

    let region_start = begin_positions[0] + begin.len();
    let region_end = end_positions[0];
    if region_start >= region_end {
        return Err(format!("status marker `{begin}` must precede `{end}`"));
    }
    Ok(&source[region_start..region_end])
}

fn parse_definitions(region: &str) -> Result<BTreeMap<String, i32>, String> {
    let mut definitions = BTreeMap::new();
    for (line_index, raw_line) in region.lines().enumerate() {
        let line_number = line_index + 1;
        let line = raw_line.trim();
        if line.is_empty()
            || line.starts_with("/*")
            || line.starts_with('*')
            || line.starts_with("*/")
        {
            continue;
        }

        let rest = line.strip_prefix("#define ").ok_or_else(|| {
            format!("status definition line {line_number} is not a `#define`: {line}")
        })?;
        let mut fields = rest.split_whitespace();
        let symbol = fields
            .next()
            .ok_or_else(|| format!("status definition line {line_number} has no symbol"))?;
        let value_text = fields
            .next()
            .ok_or_else(|| format!("status definition `{symbol}` has no value"))?;
        let trailing = fields.collect::<Vec<_>>().join(" ");
        if !trailing.is_empty() && !(trailing.starts_with("/*") && trailing.ends_with("*/")) {
            return Err(format!(
                "status definition `{symbol}` has unsupported trailing text: {trailing}"
            ));
        }
        validate_identifier(symbol, "C status symbol")?;
        if !symbol.starts_with("MAKO_LOCAL_") {
            return Err(format!(
                "status definition symbol `{symbol}` does not start with MAKO_LOCAL_"
            ));
        }
        if !value_text.bytes().all(|byte| byte.is_ascii_digit()) {
            return Err(format!(
                "status definition `{symbol}` must use an unsuffixed decimal integer, found `{value_text}`"
            ));
        }
        let value = value_text
            .parse::<i32>()
            .map_err(|error| format!("invalid value for status `{symbol}`: {error}"))?;
        if definitions.insert(symbol.to_owned(), value).is_some() {
            return Err(format!("duplicate status definition `{symbol}`"));
        }
    }
    Ok(definitions)
}

fn parse_manifest(region: &str) -> Result<Vec<ManifestRow>, String> {
    let lines: Vec<_> = region
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty())
        .collect();
    let Some((directive, physical_body)) = lines.split_first() else {
        return Err("status manifest region is empty".to_owned());
    };
    let directive_without_continuation = directive
        .strip_suffix('\\')
        .map(str::trim_end)
        .ok_or_else(|| "status manifest directive must end in `\\`".to_owned())?;
    if directive_without_continuation != "#define MAKO_LOCAL_FOR_EACH_STATUS(X)" {
        return Err(format!(
            "status manifest must start with `#define MAKO_LOCAL_FOR_EACH_STATUS(X) \\\\`, found `{directive}`"
        ));
    }
    if physical_body.is_empty() {
        return Err("status manifest contains no rows".to_owned());
    }

    let mut logical_body = String::new();
    for (index, line) in physical_body.iter().enumerate() {
        let is_last = index + 1 == physical_body.len();
        let continued = line.strip_suffix('\\');
        let body = if is_last {
            if continued.is_some() {
                return Err(
                    "final physical line of status manifest must not end in `\\`".to_owned(),
                );
            }
            *line
        } else {
            continued.ok_or_else(|| {
                format!(
                    "non-final physical line {} of status manifest must end in `\\`",
                    index + 2
                )
            })?
        };
        logical_body.push_str(body.trim_end());
        logical_body.push(' ');
    }

    parse_manifest_invocations(&logical_body)
}

fn parse_manifest_invocations(body: &str) -> Result<Vec<ManifestRow>, String> {
    if !body.is_ascii() {
        return Err("status manifest must be ASCII".to_owned());
    }
    let bytes = body.as_bytes();
    let mut cursor = 0;
    let mut rows = Vec::new();

    while cursor < bytes.len() {
        while cursor < bytes.len() && bytes[cursor].is_ascii_whitespace() {
            cursor += 1;
        }
        if cursor == bytes.len() {
            break;
        }
        if !body[cursor..].starts_with("X(") {
            return Err(format!(
                "expected status manifest row `X(...)` near `{}`",
                &body[cursor..body.len().min(cursor + 32)]
            ));
        }

        let contents_start = cursor + 2;
        let mut end = contents_start;
        let mut in_string = false;
        while end < bytes.len() {
            match bytes[end] {
                b'\\' => {
                    return Err(
                        "C escapes are not supported in status manifest messages".to_owned()
                    );
                }
                b'"' => in_string = !in_string,
                b')' if !in_string => break,
                _ => {}
            }
            end += 1;
        }
        if end == bytes.len() || in_string {
            return Err("unterminated status manifest row".to_owned());
        }

        rows.push(parse_manifest_row(&body[contents_start..end])?);
        cursor = end + 1;
    }

    Ok(rows)
}

fn parse_manifest_row(contents: &str) -> Result<ManifestRow, String> {
    let mut commas = Vec::new();
    let mut in_string = false;
    for (index, byte) in contents.bytes().enumerate() {
        match byte {
            b'"' => in_string = !in_string,
            b',' if !in_string => commas.push(index),
            _ => {}
        }
    }
    if in_string || commas.len() != 2 {
        return Err(format!(
            "status manifest row must have three arguments, found `{contents}`"
        ));
    }

    let short_name = contents[..commas[0]].trim();
    let c_symbol = contents[commas[0] + 1..commas[1]].trim();
    let message_literal = contents[commas[1] + 1..].trim();
    validate_identifier(short_name, "short status name")?;
    validate_identifier(c_symbol, "C status symbol")?;
    let expected_symbol = format!("MAKO_LOCAL_{short_name}");
    if c_symbol != expected_symbol {
        return Err(format!(
            "status `{short_name}` must use C symbol `{expected_symbol}`, found `{c_symbol}`"
        ));
    }

    let message = message_literal
        .strip_prefix('"')
        .and_then(|value| value.strip_suffix('"'))
        .ok_or_else(|| format!("status `{short_name}` message must be one C string literal"))?;
    if message.is_empty() || message.contains(['"', '\\']) {
        return Err(format!(
            "status `{short_name}` message must be nonempty and contain no quotes or escapes"
        ));
    }

    Ok(ManifestRow {
        short_name: short_name.to_owned(),
        c_symbol: c_symbol.to_owned(),
        message: message.to_owned(),
    })
}

fn validate_identifier(identifier: &str, kind: &str) -> Result<(), String> {
    let mut bytes = identifier.bytes();
    let Some(first) = bytes.next() else {
        return Err(format!("{kind} is empty"));
    };
    if !first.is_ascii_uppercase()
        || !bytes.all(|byte| byte.is_ascii_uppercase() || byte.is_ascii_digit() || byte == b'_')
    {
        return Err(format!(
            "{kind} `{identifier}` must contain only ASCII uppercase letters, digits, and underscores and start with a letter"
        ));
    }
    Ok(())
}

fn validate_statuses(
    definitions: &BTreeMap<String, i32>,
    rows: Vec<ManifestRow>,
) -> Result<Vec<Status>, String> {
    if definitions.len() != ABI_V0_STATUS_NAMES.len() {
        return Err(format!(
            "ABI v0 requires exactly {} marked status definitions, found {}",
            ABI_V0_STATUS_NAMES.len(),
            definitions.len()
        ));
    }
    if rows.len() != ABI_V0_STATUS_NAMES.len() {
        return Err(format!(
            "ABI v0 requires exactly {} manifest rows, found {}",
            ABI_V0_STATUS_NAMES.len(),
            rows.len()
        ));
    }

    let mut seen_names = BTreeSet::new();
    let mut seen_symbols = BTreeSet::new();
    let mut seen_messages = BTreeSet::new();
    let mut seen_codes = BTreeSet::new();
    let mut seen_variants = BTreeSet::new();
    let mut statuses = Vec::with_capacity(rows.len());

    for (expected_code, (row, expected_name)) in
        rows.into_iter().zip(ABI_V0_STATUS_NAMES).enumerate()
    {
        if row.short_name != expected_name {
            return Err(format!(
                "ABI v0 status {expected_code} must be `{expected_name}`, found `{}`",
                row.short_name
            ));
        }
        if !seen_names.insert(row.short_name.clone()) {
            return Err(format!("duplicate short status name `{}`", row.short_name));
        }
        if !seen_symbols.insert(row.c_symbol.clone()) {
            return Err(format!("duplicate C status symbol `{}`", row.c_symbol));
        }
        if !seen_messages.insert(row.message.clone()) {
            return Err(format!("duplicate status message `{}`", row.message));
        }

        let code = *definitions.get(&row.c_symbol).ok_or_else(|| {
            format!(
                "manifest status `{}` has no marked definition",
                row.c_symbol
            )
        })?;
        let expected_code = i32::try_from(expected_code)
            .map_err(|error| format!("status index is not representable as i32: {error}"))?;
        if code != expected_code {
            return Err(format!(
                "ABI v0 status `{}` must have code {expected_code}, found {code}",
                row.c_symbol
            ));
        }
        if !seen_codes.insert(code) {
            return Err(format!("duplicate status code {code}"));
        }

        let rust_variant = rust_variant(&row.short_name)?;
        if !seen_variants.insert(rust_variant.clone()) {
            return Err(format!("duplicate generated Rust variant `{rust_variant}`"));
        }
        statuses.push(Status {
            short_name: row.short_name,
            c_symbol: row.c_symbol,
            message: row.message,
            rust_variant,
            code,
        });
    }

    let extra_definitions: Vec<_> = definitions
        .keys()
        .filter(|symbol| !seen_symbols.contains(*symbol))
        .collect();
    if !extra_definitions.is_empty() {
        return Err(format!(
            "marked status definitions missing from manifest: {extra_definitions:?}"
        ));
    }

    Ok(statuses)
}

fn rust_variant(short_name: &str) -> Result<String, String> {
    let mut variant = String::new();
    for word in short_name.split('_') {
        if word.is_empty() {
            return Err(format!(
                "short status name `{short_name}` contains an empty word"
            ));
        }
        let mut bytes = word.bytes();
        variant.push(char::from(
            bytes
                .next()
                .expect("a nonempty status-name word has a first byte"),
        ));
        for byte in bytes {
            variant.push(char::from(byte.to_ascii_lowercase()));
        }
    }
    Ok(variant)
}

fn generate_rust(statuses: &[Status]) -> String {
    let mut output = String::new();
    writeln!(
        output,
        "// @generated by build.rs from mako_local_abi.h; do not edit."
    )
    .expect("writing to a String cannot fail");
    writeln!(output).expect("writing to a String cannot fail");
    writeln!(output, "/// A recognized revision-0 mako-local status.")
        .expect("writing to a String cannot fail");
    writeln!(output, "#[repr(i32)]").expect("writing to a String cannot fail");
    writeln!(
        output,
        "#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]"
    )
    .expect("writing to a String cannot fail");
    writeln!(output, "pub enum KnownStatus {{").expect("writing to a String cannot fail");
    for status in statuses {
        writeln!(output, "    {} = {},", status.rust_variant, status.code)
            .expect("writing to a String cannot fail");
    }
    writeln!(output, "}}").expect("writing to a String cannot fail");
    writeln!(output).expect("writing to a String cannot fail");

    writeln!(output, "impl KnownStatus {{").expect("writing to a String cannot fail");
    writeln!(
        output,
        "    /// Converts a raw status code when it is known to ABI v0."
    )
    .expect("writing to a String cannot fail");
    writeln!(
        output,
        "    pub const fn from_code(code: core::ffi::c_int) -> Option<Self> {{"
    )
    .expect("writing to a String cannot fail");
    writeln!(output, "        match code {{").expect("writing to a String cannot fail");
    for status in statuses {
        writeln!(
            output,
            "            {} => Some(Self::{}),",
            status.code, status.rust_variant
        )
        .expect("writing to a String cannot fail");
    }
    writeln!(output, "            _ => None,").expect("writing to a String cannot fail");
    writeln!(output, "        }}").expect("writing to a String cannot fail");
    writeln!(output, "    }}").expect("writing to a String cannot fail");
    writeln!(output).expect("writing to a String cannot fail");

    writeln!(output, "    /// Returns this status's stable integer code.")
        .expect("writing to a String cannot fail");
    writeln!(output, "    pub const fn code(self) -> core::ffi::c_int {{")
        .expect("writing to a String cannot fail");
    writeln!(output, "        self as core::ffi::c_int").expect("writing to a String cannot fail");
    writeln!(output, "    }}").expect("writing to a String cannot fail");
    writeln!(output).expect("writing to a String cannot fail");

    generate_string_method(
        &mut output,
        "name",
        "short manifest name",
        statuses,
        |status| &status.short_name,
    );
    generate_string_method(
        &mut output,
        "c_symbol",
        "C constant name",
        statuses,
        |status| &status.c_symbol,
    );
    generate_string_method(
        &mut output,
        "message",
        "canonical message",
        statuses,
        |status| &status.message,
    );
    writeln!(output, "}}").expect("writing to a String cannot fail");
    writeln!(output).expect("writing to a String cannot fail");

    writeln!(output, "impl TryFrom<core::ffi::c_int> for KnownStatus {{")
        .expect("writing to a String cannot fail");
    writeln!(output, "    type Error = core::ffi::c_int;")
        .expect("writing to a String cannot fail");
    writeln!(output).expect("writing to a String cannot fail");
    writeln!(
        output,
        "    fn try_from(code: core::ffi::c_int) -> Result<Self, Self::Error> {{"
    )
    .expect("writing to a String cannot fail");
    writeln!(output, "        Self::from_code(code).ok_or(code)")
        .expect("writing to a String cannot fail");
    writeln!(output, "    }}").expect("writing to a String cannot fail");
    writeln!(output, "}}").expect("writing to a String cannot fail");
    writeln!(output).expect("writing to a String cannot fail");

    writeln!(output, "impl From<KnownStatus> for core::ffi::c_int {{")
        .expect("writing to a String cannot fail");
    writeln!(output, "    fn from(status: KnownStatus) -> Self {{")
        .expect("writing to a String cannot fail");
    writeln!(output, "        status.code()").expect("writing to a String cannot fail");
    writeln!(output, "    }}").expect("writing to a String cannot fail");
    writeln!(output, "}}").expect("writing to a String cannot fail");
    writeln!(output).expect("writing to a String cannot fail");

    for status in statuses {
        writeln!(
            output,
            "pub const {}: core::ffi::c_int = KnownStatus::{}.code();",
            status.c_symbol, status.rust_variant
        )
        .expect("writing to a String cannot fail");
    }
    writeln!(output).expect("writing to a String cannot fail");

    writeln!(
        output,
        "/// Every recognized status, ordered by its ABI v0 integer code."
    )
    .expect("writing to a String cannot fail");
    writeln!(
        output,
        "pub const ALL_KNOWN_STATUSES: [KnownStatus; {}] = [",
        statuses.len()
    )
    .expect("writing to a String cannot fail");
    for status in statuses {
        writeln!(output, "    KnownStatus::{},", status.rust_variant)
            .expect("writing to a String cannot fail");
    }
    writeln!(output, "];").expect("writing to a String cannot fail");

    output
}

fn generate_string_method<'a>(
    output: &mut String,
    method: &str,
    description: &str,
    statuses: &'a [Status],
    value: impl Fn(&'a Status) -> &'a str,
) {
    writeln!(output, "    /// Returns this status's {description}.")
        .expect("writing to a String cannot fail");
    writeln!(output, "    pub const fn {method}(self) -> &'static str {{")
        .expect("writing to a String cannot fail");
    writeln!(output, "        match self {{").expect("writing to a String cannot fail");
    for status in statuses {
        writeln!(
            output,
            "            Self::{} => {:?},",
            status.rust_variant,
            value(status)
        )
        .expect("writing to a String cannot fail");
    }
    writeln!(output, "        }}").expect("writing to a String cannot fail");
    writeln!(output, "    }}").expect("writing to a String cannot fail");
    writeln!(output).expect("writing to a String cannot fail");
}
