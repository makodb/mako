use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};

use bindgen::callbacks::{IntKind, ParseCallbacks, Token, TokenKind};

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

const ABI_V0_TYPE_NAMES: [&str; 8] = [
    "mako_local_db",
    "mako_local_db_options",
    "mako_local_post_validate_hook",
    "mako_local_scan_entry",
    "mako_local_scan_options",
    "mako_local_table",
    "mako_local_test_commit_observer",
    "mako_local_txn",
];

const ABI_V0_EXPORT_NAMES: [&str; 34] = [
    "mako_local_abi_version",
    "mako_local_advance_mako_timestamp_past",
    "mako_local_build_fingerprint",
    "mako_local_build_fingerprint_size",
    "mako_local_bytes_free",
    "mako_local_db_close",
    "mako_local_db_open",
    "mako_local_db_open_with_options",
    "mako_local_db_options_size",
    "mako_local_engine_id",
    "mako_local_feature_bits",
    "mako_local_quarantined_worker_count",
    "mako_local_scan_entry_size",
    "mako_local_scan_options_size",
    "mako_local_status_string",
    "mako_local_table_id",
    "mako_local_table_open",
    "mako_local_test_arm_cleanup_failure",
    "mako_local_test_clear_cleanup_failure",
    "mako_local_test_clear_commit_observer",
    "mako_local_test_set_commit_observer",
    "mako_local_thread_attach",
    "mako_local_txn_abort",
    "mako_local_txn_begin",
    "mako_local_txn_commit",
    "mako_local_txn_commit_with_hook",
    "mako_local_txn_destroy",
    "mako_local_txn_get",
    "mako_local_txn_insert",
    "mako_local_txn_put",
    "mako_local_txn_remove",
    "mako_local_txn_rscan_chunk",
    "mako_local_txn_scan_chunk",
    "mako_local_worker_health",
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

#[derive(Debug)]
struct BindingCallbacks {
    status_symbols: BTreeSet<String>,
}

impl ParseCallbacks for BindingCallbacks {
    fn modify_macro(&self, name: &str, tokens: &mut Vec<Token>) {
        if !name.starts_with("MAKO_LOCAL_") {
            return;
        }

        // bindgen's integer-expression parser does not expand the UINT*_C
        // helpers supplied by <stdint.h>. Rewrite only the tokens of our
        // public macros; this affects generated Rust, never the C header seen
        // by the native compiler.
        let mut index = 0;
        while index < tokens.len() {
            if tokens[index].raw.as_ref() == b"UINT32_MAX" {
                tokens[index] = literal_token(b"4294967295U");
                index += 1;
                continue;
            }

            let suffix = match tokens[index].raw.as_ref() {
                b"UINT32_C" => Some(b"U".as_slice()),
                b"UINT64_C" => Some(b"ULL".as_slice()),
                _ => None,
            };
            let Some(suffix) = suffix else {
                index += 1;
                continue;
            };
            if index + 3 >= tokens.len()
                || tokens[index + 1].raw.as_ref() != b"("
                || tokens[index + 2].kind != TokenKind::Literal
                || tokens[index + 3].raw.as_ref() != b")"
            {
                index += 1;
                continue;
            }

            let mut value = tokens[index + 2].raw.to_vec();
            value.extend_from_slice(suffix);
            tokens.splice(
                index..index + 4,
                [Token {
                    kind: TokenKind::Literal,
                    raw: value.into_boxed_slice(),
                }],
            );
            index += 1;
        }
    }

    fn int_macro(&self, name: &str, _value: i64) -> Option<IntKind> {
        if self.status_symbols.contains(name) {
            return Some(IntKind::Int);
        }
        if name.starts_with("MAKO_LOCAL_FEATURE_") {
            return Some(IntKind::U64);
        }
        name.starts_with("MAKO_LOCAL_").then_some(IntKind::U32)
    }
}

fn literal_token(value: &[u8]) -> Token {
    Token {
        kind: TokenKind::Literal,
        raw: value.to_owned().into_boxed_slice(),
    }
}

fn main() {
    if let Err(error) = run() {
        panic!("mako-local-sys binding generation failed: {error}");
    }
}

fn run() -> Result<(), String> {
    let manifest_dir = PathBuf::from(
        env::var_os("CARGO_MANIFEST_DIR")
            .ok_or_else(|| "Cargo did not set CARGO_MANIFEST_DIR".to_owned())?,
    );
    let header = manifest_dir.join("../../src/mako/storage/mako_local_abi.h");
    println!("cargo:rerun-if-changed={}", header.display());
    println!("cargo:rerun-if-env-changed=LIBCLANG_PATH");

    let source = fs::read_to_string(&header)
        .map_err(|error| format!("could not read {}: {error}", header.display()))?;
    verify_abi_version(&source)?;
    verify_db_options_v0_size(&source)?;
    verify_scan_options_v0_size(&source)?;

    let definition_region = marked_region(&source, DEFINITIONS_BEGIN, DEFINITIONS_END)?;
    let manifest_region = marked_region(&source, MANIFEST_BEGIN, MANIFEST_END)?;
    let definitions = parse_definitions(definition_region)?;
    let rows = parse_manifest(manifest_region)?;
    let statuses = validate_statuses(&definitions, rows)?;

    let out_dir = PathBuf::from(
        env::var_os("OUT_DIR").ok_or_else(|| "Cargo did not set OUT_DIR".to_owned())?,
    );
    let target = env::var("TARGET").map_err(|_| "Cargo did not set TARGET".to_owned())?;

    let bindings = generate_bindings(&header, &source, &target, &statuses)?;
    write_generated(out_dir.join("mako_local_bindings.rs"), bindings)?;
    write_generated(
        out_dir.join("mako_local_statuses.rs"),
        generate_rust(&statuses),
    )
}

fn write_generated(destination: PathBuf, generated: String) -> Result<(), String> {
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

fn verify_scan_options_v0_size(source: &str) -> Result<(), String> {
    const EXPECTED: &str = "#define MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE \
        ((uint32_t)(offsetof(mako_local_scan_options, resume_len) + \
        sizeof(((mako_local_scan_options *)0)->resume_len)))";
    let declarations: Vec<_> = logical_preprocessor_lines(source)
        .into_iter()
        .map(|line| collapse_whitespace(&line))
        .filter(|line| line.starts_with("#define MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE "))
        .collect();
    let expected = collapse_whitespace(EXPECTED);
    if declarations.as_slice() != [expected.as_str()] {
        return Err(format!(
            "expected the revision-0 scan-options prefix to end at `resume_len`; \
             expected `{expected}`, found {declarations:?}"
        ));
    }
    Ok(())
}

fn verify_db_options_v0_size(source: &str) -> Result<(), String> {
    const EXPECTED: &str = "#define MAKO_LOCAL_DB_OPTIONS_V0_SIZE \
        ((uint32_t)(offsetof(mako_local_db_options, flags) + \
        sizeof(((mako_local_db_options *)0)->flags)))";
    let declarations: Vec<_> = logical_preprocessor_lines(source)
        .into_iter()
        .map(|line| collapse_whitespace(&line))
        .filter(|line| line.starts_with("#define MAKO_LOCAL_DB_OPTIONS_V0_SIZE "))
        .collect();
    let expected = collapse_whitespace(EXPECTED);
    if declarations.as_slice() != [expected.as_str()] {
        return Err(format!(
            "expected the revision-0 database-options prefix to end at `flags`; \
             expected `{expected}`, found {declarations:?}"
        ));
    }
    Ok(())
}

fn logical_preprocessor_lines(source: &str) -> Vec<String> {
    let mut lines = Vec::new();
    let mut logical = String::new();
    for raw_line in source.lines() {
        let trimmed = raw_line.trim();
        let (part, continued) = match trimmed.strip_suffix('\\') {
            Some(part) => (part.trim_end(), true),
            None => (trimmed, false),
        };
        if !logical.is_empty() {
            logical.push(' ');
        }
        logical.push_str(part);
        if !continued {
            lines.push(std::mem::take(&mut logical));
        }
    }
    if !logical.is_empty() {
        lines.push(logical);
    }
    lines
}

fn collapse_whitespace(value: &str) -> String {
    value.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn generate_bindings(
    header: &Path,
    source: &str,
    target: &str,
    statuses: &[Status],
) -> Result<String, String> {
    let header = header
        .to_str()
        .ok_or_else(|| "mako-local header path is not valid UTF-8".to_owned())?;
    let status_symbols = statuses
        .iter()
        .map(|status| status.c_symbol.clone())
        .collect();
    let generated = bindgen::Builder::default()
        .header(header)
        .clang_args(["-x", "c", "-std=c11"])
        .clang_arg(format!("--target={target}"))
        .allowlist_type("^mako_local_.*$")
        .allowlist_function("^mako_local_.*$")
        .allowlist_var("^MAKO_LOCAL_.*$")
        .blocklist_var("^MAKO_LOCAL_DB_OPTIONS_V0_SIZE$")
        .blocklist_var("^MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE$")
        .use_core()
        .ctypes_prefix("core::ffi")
        .layout_tests(true)
        .derive_default(true)
        .no_default("^mako_local_(db|table|txn|scan_options)$")
        .no_copy("^mako_local_(db|table|txn)$")
        .no_debug("^mako_local_(db|table|txn)$")
        .merge_extern_blocks(true)
        // Track every transitive header and every bindgen-consumed environment
        // variable (including target-specific BINDGEN_EXTRA_CLANG_ARGS), while
        // retaining our ABI-specific macro callbacks below.
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .parse_callbacks(Box::new(BindingCallbacks { status_symbols }))
        .generate()
        .map_err(|error| format!("bindgen could not parse {header}: {error}"))?
        .to_string();

    finalize_bindings(source, generated)
}

fn finalize_bindings(source: &str, mut generated: String) -> Result<String, String> {
    writeln!(generated).expect("writing to a String cannot fail");
    writeln!(
        generated,
        "/// Fixed revision-0 database-options prefix size."
    )
    .expect("writing to a String cannot fail");
    writeln!(generated, "pub const MAKO_LOCAL_DB_OPTIONS_V0_SIZE: u32 =")
        .expect("writing to a String cannot fail");
    writeln!(
        generated,
        "    (core::mem::offset_of!(mako_local_db_options, flags) +"
    )
    .expect("writing to a String cannot fail");
    writeln!(generated, "        core::mem::size_of::<u32>()) as u32;")
        .expect("writing to a String cannot fail");
    writeln!(generated).expect("writing to a String cannot fail");
    writeln!(
        generated,
        "/// Fixed revision-0 prefix size, derived from the header's verified last prefix field."
    )
    .expect("writing to a String cannot fail");
    writeln!(
        generated,
        "pub const MAKO_LOCAL_SCAN_OPTIONS_V0_SIZE: u32 ="
    )
    .expect("writing to a String cannot fail");
    writeln!(
        generated,
        "    (core::mem::offset_of!(mako_local_scan_options, resume_len) +"
    )
    .expect("writing to a String cannot fail");
    writeln!(generated, "        core::mem::size_of::<usize>()) as u32;")
        .expect("writing to a String cannot fail");

    verify_generated_constants(source, &generated)?;
    verify_generated_types(&generated)?;
    let exports = verify_generated_exports(&generated)?;
    append_link_probe(&mut generated, &exports);
    Ok(generated)
}

fn verify_generated_constants(source: &str, generated: &str) -> Result<(), String> {
    let expected = header_constant_names(source);
    let actual = generated_item_names(generated, "pub const ");
    if actual != expected {
        return Err(set_mismatch("public constants", &expected, &actual));
    }
    Ok(())
}

fn header_constant_names(source: &str) -> BTreeSet<String> {
    logical_preprocessor_lines(source)
        .into_iter()
        .filter_map(|line| {
            let rest = line.strip_prefix("#define ")?;
            let mut fields = rest.split_whitespace();
            let name = fields.next()?;
            if !name.starts_with("MAKO_LOCAL_")
                || name.contains('(')
                || matches!(name, "MAKO_LOCAL_ABI_H" | "MAKO_LOCAL_NOEXCEPT")
                || fields.next().is_none()
            {
                return None;
            }
            Some(name.to_owned())
        })
        .collect()
}

fn verify_generated_types(generated: &str) -> Result<(), String> {
    let mut actual = generated_item_names(generated, "pub struct ");
    actual.extend(generated_item_names(generated, "pub type "));
    let expected = ABI_V0_TYPE_NAMES.into_iter().map(str::to_owned).collect();
    if actual != expected {
        return Err(set_mismatch("public types", &expected, &actual));
    }
    Ok(())
}

fn verify_generated_exports(generated: &str) -> Result<Vec<String>, String> {
    let actual = generated_item_names(generated, "pub fn ");
    let expected: BTreeSet<_> = ABI_V0_EXPORT_NAMES.into_iter().map(str::to_owned).collect();
    if actual != expected {
        return Err(set_mismatch("public functions", &expected, &actual));
    }
    Ok(actual.into_iter().collect())
}

fn generated_item_names(generated: &str, prefix: &str) -> BTreeSet<String> {
    generated
        .lines()
        .filter_map(|line| {
            let rest = line.trim().strip_prefix(prefix)?;
            let end = rest
                .find(|character: char| !character.is_ascii_alphanumeric() && character != '_')
                .unwrap_or(rest.len());
            (end != 0).then(|| rest[..end].to_owned())
        })
        .collect()
}

fn set_mismatch(kind: &str, expected: &BTreeSet<String>, actual: &BTreeSet<String>) -> String {
    let missing: Vec<_> = expected.difference(actual).collect();
    let unexpected: Vec<_> = actual.difference(expected).collect();
    format!("generated {kind} do not match ABI v0; missing {missing:?}, unexpected {unexpected:?}")
}

fn append_link_probe(generated: &mut String, exports: &[String]) {
    writeln!(generated).expect("writing to a String cannot fail");
    writeln!(generated, "/// Every public C export expected by ABI v0.")
        .expect("writing to a String cannot fail");
    writeln!(
        generated,
        "pub const MAKO_LOCAL_EXPORT_NAMES: [&str; {}] = [",
        exports.len()
    )
    .expect("writing to a String cannot fail");
    for export in exports {
        writeln!(generated, "    \"{export}\",").expect("writing to a String cannot fail");
    }
    writeln!(generated, "];\n").expect("writing to a String cannot fail");
    writeln!(
        generated,
        "/// References every public ABI symbol without calling it."
    )
    .expect("writing to a String cannot fail");
    writeln!(generated, "///").expect("writing to a String cannot fail");
    writeln!(
        generated,
        "/// Expanding this macro in a required-native test turns a missing"
    )
    .expect("writing to a String cannot fail");
    writeln!(
        generated,
        "/// archive export into a deterministic link failure."
    )
    .expect("writing to a String cannot fail");
    writeln!(generated, "#[macro_export]").expect("writing to a String cannot fail");
    writeln!(
        generated,
        "macro_rules! mako_local_link_probe_all_exports {{"
    )
    .expect("writing to a String cannot fail");
    writeln!(generated, "    () => {{{{").expect("writing to a String cannot fail");
    writeln!(
        generated,
        "        let symbols: [*const (); {}] = [",
        exports.len()
    )
    .expect("writing to a String cannot fail");
    for export in exports {
        writeln!(generated, "            $crate::{export} as *const (),")
            .expect("writing to a String cannot fail");
    }
    writeln!(generated, "        ];").expect("writing to a String cannot fail");
    writeln!(generated, "        let _ = core::hint::black_box(symbols);")
        .expect("writing to a String cannot fail");
    writeln!(generated, "    }}}};").expect("writing to a String cannot fail");
    writeln!(generated, "}}").expect("writing to a String cannot fail");
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
