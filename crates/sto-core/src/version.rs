//! Native, checked OCC version and exclusive-owner state machine.
//!
//! This representation is private to native Rust STO. It intentionally does
//! not reuse or expose the legacy C++ bit layout in `legacy_tid`.

use std::fmt;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use crate::error::{AcquireError, Conflict, VersionError};
use crate::identity::{OccCommitId, OccVersion, OwnerId};

const OWNER_BITS: u32 = 16;
const OWNER_MASK: u64 = (1_u64 << OWNER_BITS) - 1;

/// A classified atomic version-word observation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum VersionState {
    Unlocked(OccVersion),
    Locked { version: OccVersion, owner: OwnerId },
}

impl VersionState {
    pub const fn version(self) -> OccVersion {
        match self {
            Self::Unlocked(version) | Self::Locked { version, .. } => version,
        }
    }

    pub const fn owner(self) -> Option<OwnerId> {
        match self {
            Self::Unlocked(_) => None,
            Self::Locked { owner, .. } => Some(owner),
        }
    }

    pub const fn is_locked(self) -> bool {
        matches!(self, Self::Locked { .. })
    }
}

/// An attempted observation encountered an exclusively held version.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct VersionLocked {
    version: OccVersion,
    owner: OwnerId,
}

impl VersionLocked {
    pub const fn version(&self) -> OccVersion {
        self.version
    }

    pub const fn owner(&self) -> OwnerId {
        self.owner
    }
}

impl fmt::Display for VersionLocked {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "version {} is locked by owner {}",
            self.version, self.owner
        )
    }
}

impl std::error::Error for VersionLocked {}

/// One native OCC generation plus an optional exclusive owner.
///
/// The high 48 bits contain a checked, nonzero generation. The low 16 bits
/// contain `OwnerId + 1`, with zero denoting the unlocked state. The layout is
/// an implementation detail and is unrelated to the compatibility oracle.
#[derive(Debug)]
pub struct AtomicVersion {
    word: AtomicU64,
}

impl AtomicVersion {
    /// Constructs an unlocked atomic version at `version`.
    pub const fn new(version: OccVersion) -> Self {
        Self {
            word: AtomicU64::new(encode_unlocked(version)),
        }
    }

    /// Constructs an unlocked version from ordered commit metadata.
    pub const fn from_commit_id(commit_id: OccCommitId) -> Self {
        Self::new(commit_id.to_version())
    }

    /// Loads and classifies the current state with Acquire ordering.
    pub fn state(&self) -> VersionState {
        decode(self.word.load(Ordering::Acquire))
    }

    /// Observes an unlocked generation with Acquire ordering.
    pub fn observe(&self) -> Result<OccVersion, VersionLocked> {
        match self.state() {
            VersionState::Unlocked(version) => Ok(version),
            VersionState::Locked { version, owner } => Err(VersionLocked { version, owner }),
        }
    }

    /// Makes one bounded attempt to acquire exclusive ownership.
    ///
    /// Success uses AcqRel ordering and failure uses Acquire ordering. The
    /// returned owned guard retains the `Arc`, so it remains valid and
    /// `'static` without borrowing a movable atomic object.
    pub fn try_acquire(self: &Arc<Self>, owner: OwnerId) -> Result<VersionGuard, AcquireError> {
        let current = self.word.load(Ordering::Acquire);
        let VersionState::Unlocked(version) = decode(current) else {
            return Err(Conflict::LockBusy.into());
        };
        let desired = encode_locked(version, owner);

        self.word
            .compare_exchange(current, desired, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| AcquireError::from(Conflict::LockBusy))?;

        Ok(VersionGuard {
            target: Arc::clone(self),
            before: version,
            owner,
            held: true,
        })
    }

    /// Validates only an unchanged, unlocked observation.
    pub fn validate(&self, observed: OccVersion) -> bool {
        self.word.load(Ordering::Acquire) == encode_unlocked(observed)
    }

    /// Validates an unchanged observation that may be locked by `owner`.
    pub fn validate_own(&self, observed: OccVersion, owner: OwnerId) -> bool {
        let current = self.word.load(Ordering::Acquire);
        current == encode_unlocked(observed) || current == encode_locked(observed, owner)
    }
}

impl Default for AtomicVersion {
    fn default() -> Self {
        Self::new(OccVersion::INITIAL)
    }
}

/// Owned proof that one [`AtomicVersion`] is locked by one owner.
///
/// Dropping a held guard deliberately does not unlock the version. The core
/// must select an explicit abort, commit, or indeterminate disposition; an
/// accidentally dropped or quarantined guard therefore fails closed.
#[derive(Debug)]
pub struct VersionGuard {
    target: Arc<AtomicVersion>,
    before: OccVersion,
    owner: OwnerId,
    held: bool,
}

impl VersionGuard {
    /// Generation observed when ownership was acquired.
    pub const fn before(&self) -> OccVersion {
        self.before
    }

    /// Owner encoded in the held atomic version.
    pub const fn owner(&self) -> OwnerId {
        self.owner
    }

    /// Whether this guard still owns the version.
    pub const fn is_held(&self) -> bool {
        self.held
    }

    /// Tests whether this guard belongs to `target` without exposing pointers.
    pub fn is_for(&self, target: &Arc<AtomicVersion>) -> bool {
        Arc::ptr_eq(&self.target, target)
    }

    /// Abort-unlocks to the exact pre-acquisition generation.
    ///
    /// Success uses Release ordering. Failure uses Acquire ordering, retains
    /// this guard's held state, and leaves the atomic word unchanged.
    pub fn release_abort(&mut self) -> Result<(), VersionError> {
        self.ensure_held()?;
        self.release_word(encode_unlocked(self.before))?;
        self.held = false;
        Ok(())
    }

    /// Commit-unlocks at an ordered commit generation strictly newer than the
    /// pre-acquisition generation.
    pub fn release_commit(&mut self, commit_id: OccCommitId) -> Result<OccVersion, VersionError> {
        self.ensure_held()?;
        let version = commit_id.to_version();
        if version <= self.before {
            return Err(VersionError::CommitVersionNotNewer {
                current: self.before,
                proposed: commit_id,
            });
        }
        self.release_word(encode_unlocked(version))?;
        self.held = false;
        Ok(version)
    }

    /// Conservatively publishes an unlocked generation after an indeterminate
    /// post-irrevocable failure.
    ///
    /// A supplied commit ID is used when it advances the old generation;
    /// otherwise the old generation is checked-incremented. Exhaustion leaves
    /// the version locked for quarantine rather than restoring the old value.
    pub fn release_indeterminate(
        &mut self,
        commit_id: Option<OccCommitId>,
    ) -> Result<OccVersion, VersionError> {
        self.ensure_held()?;
        let version = match commit_id.map(OccCommitId::to_version) {
            Some(candidate) if candidate > self.before => candidate,
            _ => self
                .before
                .checked_next()
                .ok_or(VersionError::GenerationExhausted(self.before))?,
        };
        self.release_word(encode_unlocked(version))?;
        self.held = false;
        Ok(version)
    }

    fn ensure_held(&self) -> Result<(), VersionError> {
        if self.held {
            Ok(())
        } else {
            Err(VersionError::GuardAlreadyReleased)
        }
    }

    fn release_word(&self, new_word: u64) -> Result<(), VersionError> {
        let expected = encode_locked(self.before, self.owner);
        self.target
            .word
            .compare_exchange(expected, new_word, Ordering::Release, Ordering::Acquire)
            .map(|_| ())
            .map_err(|_| VersionError::LostOwnership {
                expected_owner: self.owner,
            })
    }
}

const fn encode_unlocked(version: OccVersion) -> u64 {
    version.get() << OWNER_BITS
}

const fn encode_locked(version: OccVersion, owner: OwnerId) -> u64 {
    encode_unlocked(version) | owner.encoded_tag() as u64
}

fn decode(word: u64) -> VersionState {
    let generation = word >> OWNER_BITS;
    let version = OccVersion::from_validated(generation);
    let owner_tag = (word & OWNER_MASK) as u16;
    if owner_tag == 0 {
        VersionState::Unlocked(version)
    } else {
        VersionState::Locked {
            version,
            owner: OwnerId::from_encoded_tag(owner_tag),
        }
    }
}

#[cfg(test)]
mod tests {
    use std::sync::Barrier;
    use std::thread;

    use super::*;

    fn version(value: u64) -> OccVersion {
        OccVersion::new(value).unwrap()
    }

    fn commit(value: u64) -> OccCommitId {
        OccCommitId::new(value).unwrap()
    }

    fn owner(value: u32) -> OwnerId {
        OwnerId::new(value).unwrap()
    }

    #[test]
    fn acquire_and_own_validation_preserve_the_observed_generation() {
        let atomic = Arc::new(AtomicVersion::new(version(7)));
        let guard = atomic.try_acquire(owner(3)).unwrap();

        assert_eq!(guard.before(), version(7));
        assert!(guard.is_for(&atomic));
        assert_eq!(
            atomic.state(),
            VersionState::Locked {
                version: version(7),
                owner: owner(3),
            }
        );
        assert!(!atomic.validate(version(7)));
        assert!(atomic.validate_own(version(7), owner(3)));
        assert!(!atomic.validate_own(version(7), owner(4)));
        assert!(atomic.observe().is_err());
    }

    #[test]
    fn abort_release_restores_the_exact_observation() {
        let atomic = Arc::new(AtomicVersion::new(version(9)));
        let mut guard = atomic.try_acquire(owner(0)).unwrap();
        guard.release_abort().unwrap();

        assert!(!guard.is_held());
        assert_eq!(atomic.observe().unwrap(), version(9));
        assert_eq!(
            guard.release_abort(),
            Err(VersionError::GuardAlreadyReleased)
        );
    }

    #[test]
    fn commit_release_requires_and_publishes_a_newer_generation() {
        let atomic = Arc::new(AtomicVersion::new(version(10)));
        let mut stale = atomic.try_acquire(owner(1)).unwrap();
        assert_eq!(
            stale.release_commit(commit(10)),
            Err(VersionError::CommitVersionNotNewer {
                current: version(10),
                proposed: commit(10),
            })
        );
        assert!(stale.is_held());
        stale.release_abort().unwrap();

        let mut guard = atomic.try_acquire(owner(1)).unwrap();
        assert_eq!(guard.release_commit(commit(12)).unwrap(), version(12));
        assert_eq!(atomic.observe().unwrap(), version(12));
    }

    #[test]
    fn indeterminate_release_advances_and_never_abort_restores() {
        let atomic = Arc::new(AtomicVersion::new(version(20)));
        let mut guard = atomic.try_acquire(owner(2)).unwrap();
        assert_eq!(guard.release_indeterminate(None).unwrap(), version(21));
        assert_eq!(atomic.observe().unwrap(), version(21));

        let mut guard = atomic.try_acquire(owner(2)).unwrap();
        assert_eq!(
            guard.release_indeterminate(Some(commit(25))).unwrap(),
            version(25)
        );
        assert_eq!(atomic.observe().unwrap(), version(25));
    }

    #[test]
    fn indeterminate_exhaustion_fails_closed_with_the_lock_held() {
        let max = version(OccVersion::MAX_VALUE);
        let atomic = Arc::new(AtomicVersion::new(max));
        let mut guard = atomic.try_acquire(owner(5)).unwrap();
        assert_eq!(
            guard.release_indeterminate(None),
            Err(VersionError::GenerationExhausted(max))
        );
        assert!(guard.is_held());
        assert!(matches!(atomic.state(), VersionState::Locked { .. }));
        guard.release_abort().unwrap();
    }

    #[test]
    fn only_one_contending_owner_acquires_the_version() {
        let atomic = Arc::new(AtomicVersion::new(version(3)));
        let barrier = Arc::new(Barrier::new(3));
        let mut joins = Vec::new();

        for owner_value in [1, 2] {
            let atomic = Arc::clone(&atomic);
            let barrier = Arc::clone(&barrier);
            joins.push(thread::spawn(move || {
                barrier.wait();
                atomic.try_acquire(owner(owner_value))
            }));
        }
        barrier.wait();

        let mut acquired = Vec::new();
        let mut conflicts = 0;
        for join in joins {
            match join.join().unwrap() {
                Ok(guard) => acquired.push(guard),
                Err(AcquireError::Conflict(Conflict::LockBusy)) => conflicts += 1,
                Err(error) => panic!("unexpected acquisition error: {error:?}"),
            }
        }

        assert_eq!(acquired.len(), 1);
        assert_eq!(conflicts, 1);
        acquired[0].release_abort().unwrap();
    }

    #[test]
    fn maximum_owner_round_trips_through_the_packed_state() {
        let atomic = Arc::new(AtomicVersion::default());
        let max_owner = owner(OwnerId::MAX_VALUE);
        let mut guard = atomic.try_acquire(max_owner).unwrap();
        assert_eq!(atomic.state().owner(), Some(max_owner));
        guard.release_abort().unwrap();
    }
}
