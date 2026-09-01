const CMAKE_KEY: &str = "USE_MALLOC_MODE";
const CMAKE_TYPE: &str = "STRING";

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

        match configured.ok_or_else(|| format!("{CMAKE_KEY} is missing"))? {
            "0" => Ok(Self::System),
            "1" => Ok(Self::Jemalloc),
            "2" => Ok(Self::Tcmalloc),
            "3" => Ok(Self::Flow),
            value => Err(format!(
                "{CMAKE_KEY} must be one of 0, 1, 2, or 3, found {value:?}"
            )),
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
