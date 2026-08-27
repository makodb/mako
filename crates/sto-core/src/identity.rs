//! Checked identifiers and canonical physical-lock identities.

use std::fmt;
use std::sync::Arc;

/// Failure to construct a checked STO identifier.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum IdentityError {
    /// Zero is reserved as an uninitialized or absent sentinel.
    Zero {
        /// The identifier type whose constructor rejected zero.
        kind: &'static str,
    },
    /// The value cannot be represented by the selected native encoding.
    OutOfRange {
        /// The identifier type whose constructor rejected the value.
        kind: &'static str,
        /// The rejected numeric value.
        value: u64,
        /// The greatest accepted value.
        max: u64,
    },
}

impl fmt::Display for IdentityError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Zero { kind } => write!(f, "{kind} must be nonzero"),
            Self::OutOfRange { kind, value, max } => {
                write!(f, "{kind} value {value} exceeds maximum {max}")
            }
        }
    }
}

impl std::error::Error for IdentityError {}

macro_rules! nonzero_id {
    ($name:ident, $repr:ty) => {
        #[doc = concat!("A checked, nonzero `", stringify!($name), "`.")]
        #[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
        pub struct $name($repr);

        impl $name {
            /// Constructs an identifier, rejecting the reserved zero value.
            pub const fn new(value: $repr) -> Result<Self, IdentityError> {
                if value == 0 {
                    Err(IdentityError::Zero {
                        kind: stringify!($name),
                    })
                } else {
                    Ok(Self(value))
                }
            }

            /// Returns the numeric identifier.
            pub const fn get(self) -> $repr {
                self.0
            }
        }

        impl TryFrom<$repr> for $name {
            type Error = IdentityError;

            fn try_from(value: $repr) -> Result<Self, Self::Error> {
                Self::new(value)
            }
        }

        impl From<$name> for $repr {
            fn from(value: $name) -> Self {
                value.get()
            }
        }

        impl fmt::Display for $name {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                self.0.fmt(f)
            }
        }
    };
}

nonzero_id!(RuntimeId, u64);
nonzero_id!(ObjectId, u64);
nonzero_id!(ResourceClass, u32);
nonzero_id!(LockNamespaceId, u64);
nonzero_id!(LockClass, u32);

/// A worker owner identifier representable by the native version word.
///
/// Zero is a valid owner identifier. The version word stores `owner + 1`,
/// reserving an all-zero owner tag for the unlocked state.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct OwnerId(u16);

impl OwnerId {
    /// Largest owner identifier supported by the native 16-bit owner tag.
    pub const MAX_VALUE: u32 = u16::MAX as u32 - 1;

    /// Constructs an owner identifier after checking native capacity.
    pub const fn new(value: u32) -> Result<Self, IdentityError> {
        if value > Self::MAX_VALUE {
            Err(IdentityError::OutOfRange {
                kind: "OwnerId",
                value: value as u64,
                max: Self::MAX_VALUE as u64,
            })
        } else {
            Ok(Self(value as u16))
        }
    }

    /// Returns the numeric owner identifier.
    pub const fn get(self) -> u32 {
        self.0 as u32
    }

    pub(crate) const fn encoded_tag(self) -> u16 {
        self.0 + 1
    }

    pub(crate) const fn from_encoded_tag(tag: u16) -> Self {
        debug_assert!(tag != 0);
        Self(tag - 1)
    }
}

impl TryFrom<u32> for OwnerId {
    type Error = IdentityError;

    fn try_from(value: u32) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl From<OwnerId> for u32 {
    fn from(value: OwnerId) -> Self {
        value.get()
    }
}

impl fmt::Display for OwnerId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.get().fmt(f)
    }
}

/// An observed native OCC generation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct OccVersion(u64);

impl OccVersion {
    /// Native versions occupy the high 48 bits of a version word.
    pub const MAX_VALUE: u64 = u64::MAX >> 16;
    /// Initial nonzero generation for newly constructed resources.
    pub const INITIAL: Self = Self(1);

    /// Constructs a nonzero, natively representable OCC version.
    pub const fn new(value: u64) -> Result<Self, IdentityError> {
        if value == 0 {
            Err(IdentityError::Zero { kind: "OccVersion" })
        } else if value > Self::MAX_VALUE {
            Err(IdentityError::OutOfRange {
                kind: "OccVersion",
                value,
                max: Self::MAX_VALUE,
            })
        } else {
            Ok(Self(value))
        }
    }

    /// Returns the numeric generation.
    pub const fn get(self) -> u64 {
        self.0
    }

    /// Returns the next generation, or `None` rather than wrapping.
    pub const fn checked_next(self) -> Option<Self> {
        if self.0 == Self::MAX_VALUE {
            None
        } else {
            Some(Self(self.0 + 1))
        }
    }

    pub(crate) const fn from_validated(value: u64) -> Self {
        debug_assert!(value != 0 && value <= Self::MAX_VALUE);
        Self(value)
    }
}

impl TryFrom<u64> for OccVersion {
    type Error = IdentityError;

    fn try_from(value: u64) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl From<OccVersion> for u64 {
    fn from(value: OccVersion) -> Self {
        value.get()
    }
}

impl fmt::Display for OccVersion {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

/// A commit-clock value suitable for publishing an ordered OCC version.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct OccCommitId(u64);

impl OccCommitId {
    /// Largest commit identifier representable by the native version word.
    pub const MAX_VALUE: u64 = OccVersion::MAX_VALUE;

    /// Constructs a nonzero, natively representable commit identifier.
    pub const fn new(value: u64) -> Result<Self, IdentityError> {
        if value == 0 {
            Err(IdentityError::Zero {
                kind: "OccCommitId",
            })
        } else if value > Self::MAX_VALUE {
            Err(IdentityError::OutOfRange {
                kind: "OccCommitId",
                value,
                max: Self::MAX_VALUE,
            })
        } else {
            Ok(Self(value))
        }
    }

    /// Returns the numeric commit identifier.
    pub const fn get(self) -> u64 {
        self.0
    }

    /// Returns the ordered OCC version published by this commit identifier.
    pub const fn to_version(self) -> OccVersion {
        OccVersion::from_validated(self.0)
    }

    /// Returns the next commit identifier, or `None` rather than wrapping.
    pub const fn checked_next(self) -> Option<Self> {
        if self.0 == Self::MAX_VALUE {
            None
        } else {
            Some(Self(self.0 + 1))
        }
    }
}

impl TryFrom<u64> for OccCommitId {
    type Error = IdentityError;

    fn try_from(value: u64) -> Result<Self, Self::Error> {
        Self::new(value)
    }
}

impl From<OccCommitId> for u64 {
    fn from(value: OccCommitId) -> Self {
        value.get()
    }
}

impl From<OccCommitId> for OccVersion {
    fn from(value: OccCommitId) -> Self {
        value.to_version()
    }
}

impl fmt::Display for OccCommitId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

/// Stable adapter-owned key for one physical transaction lock.
///
/// Byte and integer keys occupy distinct domains. Their derived ordering is
/// total and deterministic: integer keys sort before byte keys.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum LockKey {
    /// An integral physical-lock key.
    U64(u64),
    /// An owned binary physical-lock key.
    Bytes(Arc<[u8]>),
}

impl LockKey {
    /// Constructs an integral lock key.
    pub const fn from_u64(value: u64) -> Self {
        Self::U64(value)
    }

    /// Copies bytes into an owned, immutable lock key.
    pub fn from_bytes(bytes: impl Into<Vec<u8>>) -> Self {
        Self::Bytes(Arc::from(bytes.into().into_boxed_slice()))
    }

    /// Returns the integral value when this is an integer key.
    pub const fn as_u64(&self) -> Option<u64> {
        match self {
            Self::U64(value) => Some(*value),
            Self::Bytes(_) => None,
        }
    }

    /// Returns the byte slice when this is a binary key.
    pub fn as_bytes(&self) -> Option<&[u8]> {
        match self {
            Self::U64(_) => None,
            Self::Bytes(bytes) => Some(bytes),
        }
    }
}

impl From<u64> for LockKey {
    fn from(value: u64) -> Self {
        Self::from_u64(value)
    }
}

impl From<Vec<u8>> for LockKey {
    fn from(value: Vec<u8>) -> Self {
        Self::from_bytes(value)
    }
}

impl From<&[u8]> for LockKey {
    fn from(value: &[u8]) -> Self {
        Self::from_bytes(value)
    }
}

impl<const N: usize> From<&[u8; N]> for LockKey {
    fn from(value: &[u8; N]) -> Self {
        Self::from_bytes(value.as_slice())
    }
}

/// Canonical identity of one non-reentrant physical lock.
///
/// Runtime identity is part of the ordering domain, preventing handles from
/// independent runtimes from comparing equal accidentally.
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct LockIdentity {
    runtime_id: RuntimeId,
    namespace_id: LockNamespaceId,
    class: LockClass,
    key: LockKey,
}

impl LockIdentity {
    /// Constructs a full physical-lock identity.
    pub fn new(
        runtime_id: RuntimeId,
        namespace_id: LockNamespaceId,
        class: LockClass,
        key: impl Into<LockKey>,
    ) -> Self {
        Self {
            runtime_id,
            namespace_id,
            class,
            key: key.into(),
        }
    }

    /// Returns the runtime ordering domain.
    pub const fn runtime_id(&self) -> RuntimeId {
        self.runtime_id
    }

    /// Returns the physical lock namespace.
    pub const fn namespace_id(&self) -> LockNamespaceId {
        self.namespace_id
    }

    /// Returns the adapter-defined physical lock class.
    pub const fn class(&self) -> LockClass {
        self.class
    }

    /// Returns the adapter-owned physical lock key.
    pub const fn key(&self) -> &LockKey {
        &self.key
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn checked_identifiers_reject_reserved_and_unrepresentable_values() {
        assert!(RuntimeId::new(0).is_err());
        assert!(ObjectId::new(0).is_err());
        assert!(ResourceClass::new(0).is_err());
        assert!(LockNamespaceId::new(0).is_err());
        assert!(LockClass::new(0).is_err());
        assert_eq!(OwnerId::new(0).unwrap().get(), 0);
        assert_eq!(OwnerId::new(OwnerId::MAX_VALUE).unwrap().get(), 65_534);
        assert!(OwnerId::new(OwnerId::MAX_VALUE + 1).is_err());
        assert!(OccVersion::new(0).is_err());
        assert!(OccVersion::new(OccVersion::MAX_VALUE + 1).is_err());
        assert!(OccCommitId::new(0).is_err());
        assert!(OccCommitId::new(OccCommitId::MAX_VALUE + 1).is_err());
    }

    #[test]
    fn versions_advance_without_wrapping() {
        assert_eq!(OccVersion::INITIAL.checked_next().unwrap().get(), 2);
        assert!(OccVersion::new(OccVersion::MAX_VALUE)
            .unwrap()
            .checked_next()
            .is_none());
        assert!(OccCommitId::new(OccCommitId::MAX_VALUE)
            .unwrap()
            .checked_next()
            .is_none());
    }

    #[test]
    fn lock_identity_uses_every_field_and_a_total_key_order() {
        let runtime = RuntimeId::new(1).unwrap();
        let namespace = LockNamespaceId::new(2).unwrap();
        let class = LockClass::new(3).unwrap();
        let integer = LockIdentity::new(runtime, namespace, class, 7_u64);
        let bytes = LockIdentity::new(runtime, namespace, class, b"7");

        assert_ne!(integer, bytes);
        assert!(integer < bytes);
        assert_eq!(integer.key().as_u64(), Some(7));
        assert_eq!(bytes.key().as_bytes(), Some(&b"7"[..]));

        let other_runtime = LockIdentity::new(RuntimeId::new(2).unwrap(), namespace, class, 7_u64);
        assert_ne!(integer, other_runtime);
    }
}
