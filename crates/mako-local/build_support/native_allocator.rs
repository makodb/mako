use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const CMAKE_KEY: &str = "USE_MALLOC_MODE";
const CMAKE_TYPE: &str = "STRING";
const CONTRACT_SCHEMA: &str = "1";

pub(crate) const CONTRACT_RELATIVE_PATH: &str = "generated/mako_allocator_contract.txt";

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum NativeAllocator {
    System,
    Jemalloc,
    Tcmalloc,
    Flow,
}

impl NativeAllocator {
    pub(crate) fn from_cmake_cache(cache: &str) -> Result<Self, String> {
        let mut configured = None;
        for (index, line) in cache.lines().enumerate() {
            let Some((entry, value)) = line.split_once('=') else {
                continue;
            };
            let Some((name, cache_type)) = entry.split_once(':') else {
                continue;
            };
            if name != CMAKE_KEY {
                continue;
            }
            if configured.is_some() {
                return Err(format!(
                    "{CMAKE_KEY} appears more than once (second entry at line {})",
                    index + 1
                ));
            }
            if cache_type != CMAKE_TYPE {
                return Err(format!(
                    "{CMAKE_KEY} must have CMake type {CMAKE_TYPE}, found {cache_type:?}"
                ));
            }
            configured = Some(value);
        }

        Self::from_mode(configured.ok_or_else(|| format!("{CMAKE_KEY} is missing"))?)
    }

    fn from_mode(value: &str) -> Result<Self, String> {
        match value {
            "0" => Ok(Self::System),
            "1" => Ok(Self::Jemalloc),
            "2" => Ok(Self::Tcmalloc),
            "3" => Ok(Self::Flow),
            value => Err(format!(
                "{CMAKE_KEY} must be one of 0, 1, 2, or 3, found {value:?}"
            )),
        }
    }

    pub(crate) const fn mode(self) -> &'static str {
        match self {
            Self::System => "0",
            Self::Jemalloc => "1",
            Self::Tcmalloc => "2",
            Self::Flow => "3",
        }
    }

    pub(crate) const fn kind(self) -> &'static str {
        match self {
            Self::System => "system",
            Self::Jemalloc => "jemalloc",
            Self::Tcmalloc => "tcmalloc",
            Self::Flow => "flow",
        }
    }

    pub(crate) const fn link_library(self) -> Option<&'static str> {
        match self {
            Self::System => None,
            Self::Jemalloc => Some("jemalloc"),
            Self::Tcmalloc => Some("tcmalloc"),
            Self::Flow => Some("flow"),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct ResolvedAllocator {
    mode: NativeAllocator,
    library_path: Option<PathBuf>,
    link_name: Option<String>,
    soname: Option<String>,
    sha256: Option<String>,
}

impl ResolvedAllocator {
    pub(crate) fn from_contract(contract: &str) -> Result<Self, String> {
        const KEYS: [&str; 8] = [
            "schema",
            "mode",
            "kind",
            "linkage",
            "link_name",
            "library_path",
            "soname",
            "sha256",
        ];
        let mut fields = BTreeMap::new();
        for (index, line) in contract.lines().enumerate() {
            if line.is_empty() {
                return Err(format!(
                    "allocator contract has a blank line at {}",
                    index + 1
                ));
            }
            let (key, value) = line.split_once('=').ok_or_else(|| {
                format!(
                    "allocator contract line {} must be a key=value field",
                    index + 1
                )
            })?;
            if !KEYS.contains(&key) {
                return Err(format!("allocator contract has unknown field {key:?}"));
            }
            if fields.insert(key, value).is_some() {
                return Err(format!("allocator contract repeats field {key:?}"));
            }
        }
        for key in KEYS {
            if !fields.contains_key(key) {
                return Err(format!("allocator contract is missing field {key:?}"));
            }
        }

        if fields["schema"] != CONTRACT_SCHEMA {
            return Err(format!(
                "allocator contract schema must be {CONTRACT_SCHEMA}, found {:?}",
                fields["schema"]
            ));
        }
        let mode = NativeAllocator::from_mode(fields["mode"])?;
        if fields["kind"] != mode.kind() {
            return Err(format!(
                "allocator contract mode {} requires kind {:?}, found {:?}",
                mode.mode(),
                mode.kind(),
                fields["kind"]
            ));
        }

        if mode == NativeAllocator::System {
            if fields["linkage"] != "none" {
                return Err(format!(
                    "system allocator linkage must be \"none\", found {:?}",
                    fields["linkage"]
                ));
            }
            for key in ["link_name", "library_path", "soname", "sha256"] {
                if !fields[key].is_empty() {
                    return Err(format!(
                        "system allocator contract field {key:?} must be empty"
                    ));
                }
            }
            return Ok(Self {
                mode,
                library_path: None,
                link_name: None,
                soname: None,
                sha256: None,
            });
        }

        if fields["linkage"] != "shared" {
            return Err(format!(
                "allocator mode {} requires shared linkage, found {:?}",
                mode.mode(),
                fields["linkage"]
            ));
        }
        let expected_link_name = mode
            .link_library()
            .expect("non-system allocator must have a link name");
        if fields["link_name"] != expected_link_name {
            return Err(format!(
                "allocator mode {} requires link_name {expected_link_name:?}, found {:?}",
                mode.mode(),
                fields["link_name"]
            ));
        }
        let library_path = PathBuf::from(fields["library_path"]);
        if !library_path.is_absolute() {
            return Err(format!(
                "allocator library_path must be canonical and absolute, found {:?}",
                fields["library_path"]
            ));
        }
        let soname = fields["soname"];
        if soname.is_empty() || soname.contains(['/', '\\']) {
            return Err(format!("allocator SONAME is not a basename: {soname:?}"));
        }
        let sha256 = fields["sha256"];
        if sha256.len() != 64
            || !sha256
                .bytes()
                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
        {
            return Err(format!(
                "allocator sha256 must be 64 lowercase hexadecimal characters, found {sha256:?}"
            ));
        }

        Ok(Self {
            mode,
            library_path: Some(library_path),
            link_name: Some(fields["link_name"].to_owned()),
            soname: Some(soname.to_owned()),
            sha256: Some(sha256.to_owned()),
        })
    }

    pub(crate) fn validate_mode(self, configured: NativeAllocator) -> Result<Self, String> {
        if self.mode != configured {
            return Err(format!(
                "allocator contract mode {} ({}) does not match CMakeCache mode {} ({})",
                self.mode.mode(),
                self.mode.kind(),
                configured.mode(),
                configured.kind()
            ));
        }
        Ok(self)
    }

    pub(crate) fn validate_library_path(&self) -> Result<(), String> {
        let Some(library_path) = self.library_path() else {
            return Ok(());
        };
        let canonical = fs::canonicalize(library_path).map_err(|error| {
            format!(
                "cannot canonicalize allocator library {}: {error}",
                library_path.display()
            )
        })?;
        if canonical != library_path {
            return Err(format!(
                "allocator library_path is not canonical: {} resolves to {}",
                library_path.display(),
                canonical.display()
            ));
        }
        let directory = library_path.parent().ok_or_else(|| {
            format!(
                "allocator library has no parent directory: {}",
                library_path.display()
            )
        })?;
        let link_name = self
            .link_name()
            .expect("resolved allocator library must have a link name");
        let linker_spelling = directory.join(format!("lib{link_name}.so"));
        let linker_target = fs::canonicalize(&linker_spelling).map_err(|error| {
            format!(
                "allocator link name -l{link_name} is unavailable as {}: {error}",
                linker_spelling.display()
            )
        })?;
        if linker_target != canonical {
            return Err(format!(
                "allocator link name -l{link_name} resolves to {}, not verified library {}",
                linker_target.display(),
                canonical.display()
            ));
        }
        let soname = self
            .soname
            .as_deref()
            .expect("resolved allocator library must have a SONAME");
        let runtime_spelling = directory.join(soname);
        let runtime_target = fs::canonicalize(&runtime_spelling).map_err(|error| {
            format!(
                "allocator SONAME {soname:?} is unavailable as {}: {error}",
                runtime_spelling.display()
            )
        })?;
        if runtime_target != canonical {
            return Err(format!(
                "allocator SONAME {soname:?} resolves to {}, not verified library {}",
                runtime_target.display(),
                canonical.display()
            ));
        }
        let expected_sha256 = self
            .sha256
            .as_deref()
            .expect("resolved allocator library must have a SHA-256");
        let output = Command::new("sha256sum")
            .arg("--")
            .arg(library_path)
            .output()
            .map_err(|error| {
                format!(
                    "cannot start sha256sum for allocator library {}: {error}",
                    library_path.display()
                )
            })?;
        if !output.status.success() {
            return Err(format!(
                "sha256sum failed for allocator library {}: {}",
                library_path.display(),
                String::from_utf8_lossy(&output.stderr).trim()
            ));
        }
        let stdout = String::from_utf8(output.stdout).map_err(|error| {
            format!(
                "sha256sum returned non-UTF-8 output for {}: {error}",
                library_path.display()
            )
        })?;
        let actual_sha256 = stdout.split_ascii_whitespace().next().ok_or_else(|| {
            format!(
                "sha256sum returned no digest for allocator library {}",
                library_path.display()
            )
        })?;
        if actual_sha256 != expected_sha256 {
            return Err(format!(
                "allocator library hash changed at {}: contract has {}, current bytes have {}",
                library_path.display(),
                expected_sha256,
                actual_sha256
            ));
        }
        Ok(())
    }

    pub(crate) fn library_path(&self) -> Option<&Path> {
        self.library_path.as_deref()
    }

    pub(crate) fn library_directory(&self) -> Option<&Path> {
        self.library_path().and_then(Path::parent)
    }

    pub(crate) fn link_name(&self) -> Option<&str> {
        self.link_name.as_deref()
    }
}
