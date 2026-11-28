//! Generation ownership and native-path publication. The public path is a
//! relative symlink into a sibling control directory. Native handles open the
//! resolved generation, so replacing the link never redirects a live handle.
use crate::{
    database::*,
    layout::{Store, META},
    Error, Result, Table,
};
use mako_rocksdb_store::{BlobOp, Blobs, RocksBlobs};
use sha2::{Digest, Sha256};
use std::{
    fs::{self, File, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    sync::Arc,
};

pub(crate) struct Opened {
    pub root: PathBuf,
    pub public: PathBuf,
    pub name: String,
    pub rocks: Arc<Store>,
    pub lock: File,
}
pub fn control_path(path: &Path) -> PathBuf {
    let mut name = path.file_name().unwrap_or_default().to_owned();
    name.push(".mako");
    path.with_file_name(name)
}
fn sync(path: &Path) -> Result<()> {
    File::open(path)?.sync_all()?;
    Ok(())
}
fn write(path: &Path, bytes: &[u8]) -> Result<()> {
    let temporary = path.with_extension("next");
    let mut file = File::create(&temporary)?;
    file.write_all(bytes)?;
    file.sync_all()?;
    fs::rename(&temporary, path)?;
    sync(path.parent().unwrap())
}
fn selector(root: &Path) -> Result<String> {
    let name = fs::read_to_string(root.join("CURRENT"))?;
    if name.is_empty() || !name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-') {
        return Err(Error::Corruption("invalid generation manifest".into()));
    }
    Ok(name)
}
pub(crate) fn publish(root: &Path, public: &Path, name: &str) -> Result<()> {
    write(&root.join("CURRENT"), name.as_bytes())?;
    cut("selector");
    publish_link(root, public, name)?;
    Ok(())
}
fn publish_link(root: &Path, public: &Path, name: &str) -> Result<()> {
    let target = Path::new(
        root.file_name()
            .ok_or(Error::InvalidState("database root name"))?,
    )
    .join(name);
    if fs::read_link(public).ok().as_ref() == Some(&target) {
        return Ok(());
    }
    if public
        .symlink_metadata()
        .is_ok_and(|m| !m.file_type().is_symlink())
    {
        return Err(Error::Corruption(
            "native database path replaced by a directory/file".into(),
        ));
    }
    let mut tmp = public.as_os_str().to_owned();
    tmp.push(".mako-next");
    let tmp = PathBuf::from(tmp);
    if let Ok(m) = tmp.symlink_metadata() {
        if !m.file_type().is_symlink() {
            return Err(Error::Corruption("foreign publication path".into()));
        }
        fs::remove_file(&tmp)?;
    }
    std::os::unix::fs::symlink(target, &tmp)?;
    fs::rename(&tmp, public)?;
    sync(public.parent().unwrap())
}

pub(crate) fn open(
    path: &Path,
    group: u64,
    durable: bool,
    distributed: bool,
    tables: Option<&[Table]>,
) -> Result<Opened> {
    let public = std::path::absolute(path)?;
    let public = public
        .parent()
        .ok_or(Error::InvalidState(
            "database path cannot be filesystem root",
        ))?
        .join(
            public
                .file_name()
                .ok_or(Error::InvalidState("database path needs a name"))?,
        );
    let root = control_path(&public);
    fs::create_dir_all(&root)?;
    let lock = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(root.join("LOCK"))?;
    lock.try_lock()
        .map_err(|e| Error::Storage(format!("database root lock: {e}")))?;

    if root.join("CURRENT").exists() {
        let name = selector(&root)?;
        let rocks = Arc::new(Store::open_existing_strict(
            &root.join(&name),
            durability(durable),
        )?);
        publish_link(&root, &public, &name)?;
        return Ok(Opened {
            root,
            public,
            name,
            rocks,
            lock,
        });
    }
    let mut identity = format_bytes(group, durable);
    identity.push(u8::from(distributed));
    if let Some(tables) = tables {
        let mut ids = std::collections::BTreeSet::new();
        let mut names = std::collections::BTreeSet::new();
        for table in tables {
            if table.id == 0
                || table.name.is_empty()
                || table.name.contains('\0')
                || table.name == META
                || !ids.insert(table.id)
                || !names.insert(&table.name)
            {
                return Err(Error::InvalidCommand("invalid adoption schema"));
            }
            identity.extend_from_slice(&table.id.to_be_bytes());
            identity.extend_from_slice(&(table.name.len() as u64).to_be_bytes());
            identity.extend_from_slice(table.name.as_bytes());
        }
    }
    let pending = root.join("ADOPTING");
    if !pending.exists()
        && fs::read_dir(&root)?.any(|e| {
            e.map_or(true, |e| {
                e.file_name() != "LOCK" && e.file_name() != "ADOPTING.next"
            })
        })
    {
        return Err(Error::Corruption(
            "unidentified database control directory".into(),
        ));
    }
    if pending.exists() && fs::read(&pending)? != identity {
        return Err(Error::Corruption(
            "interrupted adoption identity mismatch".into(),
        ));
    }
    let name = "generation-0".to_owned();
    let generation = root.join(&name);
    if !generation.exists() {
        let existing = public.symlink_metadata().is_ok();
        if existing {
            if !public.is_dir() || public.symlink_metadata()?.file_type().is_symlink() {
                return Err(Error::Corruption("invalid unmanaged database path".into()));
            }
            if public.join("CURRENT").exists() {
                let families = RocksBlobs::list_families(&public)?;
                if families.iter().any(|n| n == META) {
                    return Err(Error::Corruption(
                        "reserved metadata family in unmanaged database".into(),
                    ));
                }
                let tables = tables.ok_or(Error::InvalidState(
                    "native adoption requires an explicit schema",
                ))?;
                if families
                    .iter()
                    .any(|n| !tables.iter().any(|t| &t.name == n))
                    || tables.iter().any(|t| !families.contains(&t.name))
                {
                    return Err(Error::InvalidCommand(
                        "native column families differ from adoption schema",
                    ));
                }
                // Opening checks options and the native writer lease before
                // moving files. Adoption requires the previous writer stopped.
                let native = Arc::new(RocksBlobs::open_existing_strict(
                    &public,
                    durability(durable),
                )?);
                validate_rows(&native, tables)?;
            } else if fs::read_dir(&public)?.next().is_some() {
                return Err(Error::Corruption(
                    "unidentified nonempty database directory".into(),
                ));
            }
        }
        // Failed preflight must leave the source retryable with corrected
        // options. Persist the identity only once validation has succeeded,
        // before the first move or metadata change.
        if !pending.exists() {
            write(&pending, &identity)?;
        }
        if existing {
            fs::rename(&public, &generation)?;
            sync(public.parent().unwrap())?;
        } else {
            fs::create_dir(&generation)?;
        }
        sync(&root)?;
        cut("moved");
    }
    let native = Arc::new(RocksBlobs::open_strict(&generation, durability(durable))?);
    native.ensure_family(META)?;
    let rocks = Arc::new(Store::from_native(native.clone())?);
    if rocks.get(FORMAT)?.is_none() {
        if let Some(tables) = tables {
            initialize_rows(&rocks, tables)?;
        }
        let format = format_bytes(group, durable);
        let position = position_bytes(Default::default());
        let initial_generation = 1u64.to_be_bytes();
        let mut ops = vec![
            BlobOp::Put {
                key: FORMAT,
                val: &format,
            },
            BlobOp::Put {
                key: APPLIED,
                val: &position,
            },
            BlobOp::Put {
                key: GENERATION,
                val: &initial_generation,
            },
        ];
        if distributed {
            ops.push(BlobOp::Put {
                key: crate::distributed::PROFILE,
                val: crate::distributed::PROFILE_VALUE,
            });
        }
        rocks.write_batch(&ops)?;
        cut("ready");
    }
    publish(&root, &public, &name)?;
    // ADOPTING remains until the runtime has established durable log authority.
    // A restart may finish that operation, but may never recreate a lost log
    // after complete_bootstrap removes this marker.
    sync(&root)?;
    Ok(Opened {
        root,
        public,
        name,
        rocks,
        lock,
    })
}
fn validate_rows(native: &Arc<RocksBlobs>, tables: &[Table]) -> Result<()> {
    let snapshot = native.snapshot()?;
    for table in tables {
        snapshot.scan_cf(
            &table.name,
            b"",
            None,
            false,
            usize::MAX,
            &mut |key, value| {
                if key.len() > 1024 || value.len() > 65535 {
                    return Err(mako_rocksdb_store::BlobError(
                        "existing row exceeds Mako protocol limits".into(),
                    ));
                }
                Ok(true)
            },
        )?;
    }
    Ok(())
}
fn initialize_rows(rocks: &Store, tables: &[Table]) -> Result<()> {
    let mut hash = Sha256::new();
    hash.update(b"MAKO-NATIVE-BASELINE-01");
    let mut nonempty = false;
    let mut tables = tables.to_vec();
    tables.sort_by_key(|t| t.id);
    for table in &tables {
        // Tags distinguish table boundaries from arbitrary row bytes. Lengths
        // alone do not delimit a variable number of rows between table headers.
        hash.update([0]);
        hash.update(table.id.to_be_bytes());
        hash.update((table.name.len() as u64).to_be_bytes());
        hash.update(table.name.as_bytes());
        let id = table.id;
        rocks.write_batch(&[
            BlobOp::Put {
                key: &table_key(id),
                val: table.name.as_bytes(),
            },
            BlobOp::Put {
                key: &catalog_key(&table.name),
                val: &id.to_be_bytes(),
            },
        ])?;
        let snapshot = rocks.native.snapshot()?;
        let mut keys = Vec::new();
        let mut count = 0u64;
        let flush = |keys: &mut Vec<Vec<u8>>| -> Result<(), mako_rocksdb_store::BlobError> {
            let version = 1u64.to_be_bytes();
            let time = 0u32.to_be_bytes();
            let ops = keys
                .iter()
                .map(|key| {
                    (
                        crate::layout::META,
                        BlobOp::Put {
                            key,
                            val: if key[1] == 6 { &version[..] } else { &time[..] },
                        },
                    )
                })
                .collect::<Vec<_>>();
            rocks.native.write_families(&ops)?;
            cut("metadata");
            keys.clear();
            Ok(())
        };
        snapshot.scan_cf(
            &table.name,
            b"",
            None,
            false,
            usize::MAX,
            &mut |key, value| {
                if key.len() > 1024 || value.len() > 65535 {
                    return Err(mako_rocksdb_store::BlobError(
                        "existing row exceeds Mako protocol limits".into(),
                    ));
                }
                nonempty = true;
                hash.update([1]);
                hash.update((key.len() as u64).to_be_bytes());
                hash.update(key);
                hash.update((value.len() as u64).to_be_bytes());
                hash.update(value);
                count = count
                    .checked_add(1)
                    .ok_or_else(|| mako_rocksdb_store::BlobError("row count overflow".into()))?;
                keys.push(version_key(id, key));
                keys.push(crate::distributed::time_key(id, key));
                if keys.len() >= 512 {
                    flush(&mut keys)?;
                }
                Ok(true)
            },
        )?;
        hash.update([2]);
        flush(&mut keys)?;
        rocks.write_batch(&[BlobOp::Put {
            key: &count_key(id),
            val: &count.to_be_bytes(),
        }])?;
    }
    if nonempty {
        rocks.write_batch(&[BlobOp::Put {
            key: crate::distributed::BASELINE,
            val: &hash.finalize(),
        }])?;
    }
    Ok(())
}

fn cut(point: &str) {
    #[cfg(feature = "test-hooks")]
    if std::env::var("STO_ROCKS_ADOPTION_CRASH").ok().as_deref() == Some(point) {
        std::process::exit(86);
    }
    #[cfg(not(feature = "test-hooks"))]
    let _ = point;
}
