//! Logical engine checkpoints. Files become usable only after the checksummed
//! manifest is durably renamed. The log driver owns retention/publication of
//! this directory; a successfully written directory alone permits no log trim.
use crate::{
    application::{Index, Position},
    redo::Row,
    wire::{self, Error, ErrorCode, Result},
};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs::{self, File, OpenOptions},
    io::{BufReader, BufWriter, Read, Write},
    path::Path,
};

#[derive(Serialize, Deserialize)]
struct Manifest {
    version: u32,
    owner: u32,
    engine: String,
    schema: [u8; 32],
    durable: bool,
    position: Position,
    index_hash: [u8; 32],
    rows_hash: [u8; 32],
    rows: u64,
    row_cut: Position,
    required_prepares: u64,
}
fn fail(error: impl std::fmt::Display) -> Error {
    Error::new(ErrorCode::Fatal, format!("checkpoint: {error}"))
}
fn hash_file(path: &Path) -> Result<[u8; 32]> {
    let mut file = File::open(path).map_err(fail)?;
    let mut hash = Sha256::new();
    let mut bytes = [0; 65536];
    loop {
        let n = file.read(&mut bytes).map_err(fail)?;
        if n == 0 {
            break;
        }
        hash.update(&bytes[..n]);
    }
    Ok(hash.finalize().into())
}
fn create(path: &Path) -> Result<File> {
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(fail)
}
fn sync_dir(path: &Path) -> Result<()> {
    File::open(path)
        .and_then(|file| file.sync_all())
        .map_err(fail)
}
fn validate_row(row: &Row) -> Result<()> {
    // Bootstrap values have no distributed decision. Validate all other fields
    // against the same bounded ordered-byte contract used by logged effects.
    if row.time_term == 0 {
        let mut ordinary = row.clone();
        ordinary.time_term = 10;
        ordinary.validate()
    } else {
        row.validate()
    }
}
pub(crate) struct Identity<'a> {
    pub owner: u32,
    pub engine: &'a str,
    pub schema: &'a [u8],
    pub durable: bool,
}
pub(crate) fn write(
    path: &Path,
    identity: Identity<'_>,
    index: &Index,
    export: impl FnOnce(&mut dyn FnMut(Row) -> Result<()>) -> Result<()>,
) -> Result<()> {
    index.validate(identity.owner, identity.durable)?;
    fs::create_dir(path).map_err(fail)?;
    let mut metadata = BufWriter::new(create(&path.join("index"))?);
    bincode::serialize_into(&mut metadata, index).map_err(fail)?;
    metadata.flush().map_err(fail)?;
    metadata.get_ref().sync_all().map_err(fail)?;
    let mut rows = BufWriter::new(create(&path.join("rows"))?);
    let mut count = 0u64;
    export(&mut |row| {
        validate_row(&row)?;
        let bytes = wire::encode(&row)?;
        let len = u32::try_from(bytes.len()).map_err(fail)?;
        rows.write_all(&len.to_le_bytes())
            .and_then(|()| rows.write_all(&bytes))
            .map_err(fail)?;
        count = count
            .checked_add(1)
            .ok_or_else(|| fail("row count overflow"))?;
        Ok(())
    })?;
    rows.flush().map_err(fail)?;
    rows.get_ref().sync_all().map_err(fail)?;
    let manifest = Manifest {
        version: 1,
        owner: identity.owner,
        engine: identity.engine.into(),
        schema: Sha256::digest(identity.schema).into(),
        durable: identity.durable,
        position: index.position,
        index_hash: hash_file(&path.join("index"))?,
        rows_hash: hash_file(&path.join("rows"))?,
        rows: count,
        row_cut: index.position,
        required_prepares: index
            .transactions
            .values()
            .filter(|entry| entry.unresolved())
            .count() as u64,
    };
    let mut file = create(&path.join("MANIFEST.tmp"))?;
    file.write_all(&serde_json::to_vec(&manifest).map_err(fail)?)
        .map_err(fail)?;
    file.sync_all().map_err(fail)?;
    fs::rename(path.join("MANIFEST.tmp"), path.join("MANIFEST")).map_err(fail)?;
    sync_dir(path)?;
    if let Some(parent) = path.parent() {
        sync_dir(parent)?;
    }
    Ok(())
}
pub(crate) fn validate(path: &Path, identity: Identity<'_>, position: Position) -> Result<Index> {
    if fs::metadata(path.join("MANIFEST")).map_err(fail)?.len() > 65536 {
        return Err(fail("oversized manifest"));
    }
    let manifest: Manifest =
        serde_json::from_slice(&fs::read(path.join("MANIFEST")).map_err(fail)?).map_err(fail)?;
    let schema: [u8; 32] = Sha256::digest(identity.schema).into();
    if manifest.version != 1
        || manifest.owner != identity.owner
        || manifest.engine != identity.engine
        || manifest.schema != schema
        || manifest.durable != identity.durable
        || manifest.position != position
    {
        return Err(fail("checkpoint identity or application cut mismatch"));
    }
    if hash_file(&path.join("index"))? != manifest.index_hash
        || hash_file(&path.join("rows"))? != manifest.rows_hash
    {
        return Err(fail("checksum mismatch"));
    }
    use bincode::Options;
    let length = fs::metadata(path.join("index")).map_err(fail)?.len();
    let mut input = BufReader::new(File::open(path.join("index")).map_err(fail)?);
    let index: Index = bincode::DefaultOptions::new()
        .with_fixint_encoding()
        .with_limit(length)
        .deserialize_from(&mut input)
        .map_err(fail)?;
    if input.read(&mut [0u8; 1]).map_err(fail)? != 0 {
        return Err(fail("trailing index bytes"));
    }
    index.validate(identity.owner, identity.durable)?;
    if !mako_protocol_core::recovery::checkpoint_ready(
        position.index,
        manifest.row_cut.index,
        index.position.index,
        manifest.required_prepares,
        index
            .transactions
            .values()
            .filter(|entry| entry.unresolved())
            .count() as u64,
        true,
    ) || manifest.row_cut != position
    {
        return Err(fail("checkpoint lost row, metadata or prepared coverage"));
    }
    if index.position != position {
        return Err(fail("index position mismatch"));
    }
    let mut count = 0u64;
    visit(path, |_| {
        count += 1;
        Ok(())
    })?;
    if count != manifest.rows {
        return Err(fail("row count mismatch"));
    }
    Ok(index)
}
pub(crate) fn visit(path: &Path, mut row: impl FnMut(Row) -> Result<()>) -> Result<()> {
    let mut input = BufReader::new(File::open(path.join("rows")).map_err(fail)?);
    loop {
        let mut length = [0u8; 4];
        if input.read(&mut length[..1]).map_err(fail)? == 0 {
            return Ok(());
        }
        input.read_exact(&mut length[1..]).map_err(fail)?;
        let length = u32::from_le_bytes(length) as usize;
        if length == 0 || length > wire::MAX_BYTES {
            return Err(fail("invalid row frame length"));
        }
        let mut bytes = vec![0; length];
        input.read_exact(&mut bytes).map_err(fail)?;
        let value: Row = wire::decode(&bytes)?;
        validate_row(&value)?;
        row(value)?;
    }
}
