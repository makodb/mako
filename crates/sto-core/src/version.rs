//! Native, checked OCC version and exclusive-owner state machine.
//!
//! This representation is private to native Rust STO. It intentionally does
//! not reuse or expose the legacy C++ bit layout in `legacy_tid`.

use std::fmt;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use crate::error::{AcquireError, Conflict, VersionError};
use crate::identity::{OccCommitId, OccVersion, OwnerId};
use crate::lock::{AcquireContext, LockDisposition, ReleaseContext, TransactionLock};

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
        Arc::clone(self).try_acquire_owned(owner)
    }

    /// Makes one bounded attempt to acquire a stable inline version without
    /// retaining an [`Arc`] or borrowing this value.
    ///
    /// The returned token records this value's address and every release
    /// operation must be given the same, unmoved `AtomicVersion`. Passing a
    /// different or moved target is rejected and leaves the lock held. This
    /// permits a canonical owner with a longer lifetime to keep versions
    /// inline while still giving the transaction core a `'static` guard.
    pub fn try_acquire_detached(
        &self,
        owner: OwnerId,
    ) -> Result<DetachedVersionGuard, AcquireError> {
        let before = self.try_acquire_version(owner)?;
        Ok(DetachedVersionGuard {
            target_address: self.address(),
            before,
            owner,
            held: true,
        })
    }

    fn try_acquire_owned(self: Arc<Self>, owner: OwnerId) -> Result<VersionGuard, AcquireError> {
        let before = self.try_acquire_version(owner)?;
        Ok(VersionGuard {
            target: self,
            before,
            owner,
            held: true,
        })
    }

    fn try_acquire_version(&self, owner: OwnerId) -> Result<OccVersion, AcquireError> {
        let current = self.word.load(Ordering::Acquire);
        let VersionState::Unlocked(version) = decode(current) else {
            return Err(Conflict::LockBusy.into());
        };
        let desired = encode_locked(version, owner);

        self.word
            .compare_exchange(current, desired, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| AcquireError::from(Conflict::LockBusy))?;

        Ok(version)
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

    fn address(&self) -> usize {
        std::ptr::from_ref(self).addr()
    }

    fn release_word(
        &self,
        before: OccVersion,
        owner: OwnerId,
        new_word: u64,
    ) -> Result<(), VersionError> {
        let expected = encode_locked(before, owner);
        self.word
            .compare_exchange(expected, new_word, Ordering::Release, Ordering::Acquire)
            .map(|_| ())
            .map_err(|_| VersionError::LostOwnership {
                expected_owner: owner,
            })
    }
}

impl Default for AtomicVersion {
    fn default() -> Self {
        Self::new(OccVersion::INITIAL)
    }
}

impl TransactionLock for AtomicVersion {
    type Guard = DetachedVersionGuard;

    fn try_acquire(
        &self,
        _identity: &crate::identity::LockIdentity,
        cx: &AcquireContext<'_>,
    ) -> Result<Self::Guard, AcquireError> {
        self.try_acquire_detached(cx.owner())
    }

    fn release(
        &self,
        guard: &mut Self::Guard,
        disposition: LockDisposition,
        cx: &ReleaseContext<'_>,
    ) {
        if guard.owner() != cx.owner() || !guard.is_for(self) {
            panic!("sto-core invariant: mismatched AtomicVersion guard");
        }

        let result = match disposition {
            LockDisposition::Aborted => guard.release_abort(self).map(|()| None),
            LockDisposition::Committed {
                occ_commit_id: Some(commit_id),
            } => guard.release_commit(self, commit_id).map(Some),
            LockDisposition::Committed {
                occ_commit_id: None,
            } => panic!("sto-core invariant: committed AtomicVersion write has no OCC commit ID"),
            LockDisposition::Indeterminate { occ_commit_id } => {
                guard.release_indeterminate(self, occ_commit_id).map(Some)
            }
        };

        if let Err(error) = result {
            panic!("sto-core invariant: AtomicVersion release failed: {error}");
        }
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
        self.target.release_word(self.before, self.owner, new_word)
    }
}

/// Detached proof that one stable inline [`AtomicVersion`] is locked.
///
/// This token owns no reference to its target. Every release method therefore
/// requires the original, unmoved `AtomicVersion` and checks its address before
/// touching the version word. The owner of the containing allocation must keep
/// that target alive and at a stable address until the token is released.
///
/// Dropping a held guard deliberately does not unlock the version. The core
/// must select an explicit abort, commit, or indeterminate disposition; an
/// accidentally dropped or quarantined guard therefore fails closed.
#[derive(Debug)]
pub struct DetachedVersionGuard {
    target_address: usize,
    before: OccVersion,
    owner: OwnerId,
    held: bool,
}

impl DetachedVersionGuard {
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

    /// Tests whether this token was acquired from `target` at its current
    /// address. A target moved after acquisition no longer matches.
    pub fn is_for(&self, target: &AtomicVersion) -> bool {
        self.target_address == target.address()
    }

    /// Abort-unlocks to the exact pre-acquisition generation.
    ///
    /// Success uses Release ordering. A released token, wrong target, or lost
    /// ownership leaves the supplied atomic word unchanged and keeps this
    /// token's held state unchanged.
    pub fn release_abort(&mut self, target: &AtomicVersion) -> Result<(), VersionError> {
        self.ensure_releasable(target)?;
        target.release_word(self.before, self.owner, encode_unlocked(self.before))?;
        self.held = false;
        Ok(())
    }

    /// Commit-unlocks at an ordered commit generation strictly newer than the
    /// pre-acquisition generation.
    pub fn release_commit(
        &mut self,
        target: &AtomicVersion,
        commit_id: OccCommitId,
    ) -> Result<OccVersion, VersionError> {
        self.ensure_releasable(target)?;
        let version = commit_id.to_version();
        if version <= self.before {
            return Err(VersionError::CommitVersionNotNewer {
                current: self.before,
                proposed: commit_id,
            });
        }
        target.release_word(self.before, self.owner, encode_unlocked(version))?;
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
        target: &AtomicVersion,
        commit_id: Option<OccCommitId>,
    ) -> Result<OccVersion, VersionError> {
        self.ensure_releasable(target)?;
        let version = match commit_id.map(OccCommitId::to_version) {
            Some(candidate) if candidate > self.before => candidate,
            _ => self
                .before
                .checked_next()
                .ok_or(VersionError::GenerationExhausted(self.before))?,
        };
        target.release_word(self.before, self.owner, encode_unlocked(version))?;
        self.held = false;
        Ok(version)
    }

    fn ensure_releasable(&self, target: &AtomicVersion) -> Result<(), VersionError> {
        if !self.held {
            Err(VersionError::GuardAlreadyReleased)
        } else if !self.is_for(target) {
            Err(VersionError::GuardTargetMismatch)
        } else {
            Ok(())
        }
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
        let guard = AtomicVersion::try_acquire(&atomic, owner(3)).unwrap();

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
        let mut guard = AtomicVersion::try_acquire(&atomic, owner(0)).unwrap();
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
        let mut stale = AtomicVersion::try_acquire(&atomic, owner(1)).unwrap();
        assert_eq!(
            stale.release_commit(commit(10)),
            Err(VersionError::CommitVersionNotNewer {
                current: version(10),
                proposed: commit(10),
            })
        );
        assert!(stale.is_held());
        stale.release_abort().unwrap();

        let mut guard = AtomicVersion::try_acquire(&atomic, owner(1)).unwrap();
        assert_eq!(guard.release_commit(commit(12)).unwrap(), version(12));
        assert_eq!(atomic.observe().unwrap(), version(12));
    }

    #[test]
    fn indeterminate_release_advances_and_never_abort_restores() {
        let atomic = Arc::new(AtomicVersion::new(version(20)));
        let mut guard = AtomicVersion::try_acquire(&atomic, owner(2)).unwrap();
        assert_eq!(guard.release_indeterminate(None).unwrap(), version(21));
        assert_eq!(atomic.observe().unwrap(), version(21));

        let mut guard = AtomicVersion::try_acquire(&atomic, owner(2)).unwrap();
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
        let mut guard = AtomicVersion::try_acquire(&atomic, owner(5)).unwrap();
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
                AtomicVersion::try_acquire(&atomic, owner(owner_value))
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
        let mut guard = AtomicVersion::try_acquire(&atomic, max_owner).unwrap();
        assert_eq!(atomic.state().owner(), Some(max_owner));
        guard.release_abort().unwrap();
    }

    #[test]
    fn detached_guard_acquires_and_abort_releases_the_exact_target() {
        let atomic = AtomicVersion::new(version(31));
        let mut guard = atomic.try_acquire_detached(owner(7)).unwrap();

        assert_eq!(guard.before(), version(31));
        assert_eq!(guard.owner(), owner(7));
        assert!(guard.is_held());
        assert!(guard.is_for(&atomic));
        assert_eq!(
            atomic.state(),
            VersionState::Locked {
                version: version(31),
                owner: owner(7),
            }
        );

        guard.release_abort(&atomic).unwrap();
        assert!(!guard.is_held());
        assert_eq!(atomic.observe().unwrap(), version(31));
        assert_eq!(
            guard.release_abort(&atomic),
            Err(VersionError::GuardAlreadyReleased)
        );
    }

    #[test]
    fn detached_guard_rejects_a_different_target_without_touching_either_word() {
        let target = AtomicVersion::new(version(41));
        let other = AtomicVersion::new(version(52));
        let mut guard = target.try_acquire_detached(owner(8)).unwrap();

        assert!(!guard.is_for(&other));
        assert_eq!(
            guard.release_abort(&other),
            Err(VersionError::GuardTargetMismatch)
        );
        assert!(guard.is_held());
        assert_eq!(other.observe().unwrap(), version(52));
        assert_eq!(
            target.state(),
            VersionState::Locked {
                version: version(41),
                owner: owner(8),
            }
        );

        guard.release_abort(&target).unwrap();
    }

    #[test]
    fn detached_guard_fails_closed_if_the_atomic_is_moved() {
        let mut original_slot = AtomicVersion::new(version(61));
        let mut replacement_slot = AtomicVersion::new(version(62));
        let mut guard = original_slot.try_acquire_detached(owner(9)).unwrap();

        std::mem::swap(&mut original_slot, &mut replacement_slot);
        assert!(!guard.is_for(&replacement_slot));
        assert_eq!(
            guard.release_abort(&replacement_slot),
            Err(VersionError::GuardTargetMismatch)
        );
        assert_eq!(
            guard.release_abort(&original_slot),
            Err(VersionError::LostOwnership {
                expected_owner: owner(9),
            })
        );
        assert!(guard.is_held());
        assert!(matches!(
            replacement_slot.state(),
            VersionState::Locked { .. }
        ));

        std::mem::swap(&mut original_slot, &mut replacement_slot);
        guard.release_abort(&original_slot).unwrap();
    }

    #[test]
    fn detached_commit_and_indeterminate_release_match_owned_guards() {
        let atomic = AtomicVersion::new(version(70));
        let mut stale = atomic.try_acquire_detached(owner(10)).unwrap();
        assert_eq!(
            stale.release_commit(&atomic, commit(70)),
            Err(VersionError::CommitVersionNotNewer {
                current: version(70),
                proposed: commit(70),
            })
        );
        assert!(stale.is_held());
        assert_eq!(
            stale.release_commit(&atomic, commit(72)).unwrap(),
            version(72)
        );

        let mut indeterminate = atomic.try_acquire_detached(owner(10)).unwrap();
        assert_eq!(
            indeterminate.release_indeterminate(&atomic, None).unwrap(),
            version(73)
        );
        assert_eq!(atomic.observe().unwrap(), version(73));
    }

    #[test]
    fn dropping_a_held_detached_guard_deliberately_leaves_the_lock_held() {
        let atomic = AtomicVersion::new(version(80));
        {
            let _guard = atomic.try_acquire_detached(owner(11)).unwrap();
        }

        assert!(matches!(
            atomic.try_acquire_detached(owner(12)),
            Err(AcquireError::Conflict(Conflict::LockBusy))
        ));
        assert_eq!(
            atomic.state(),
            VersionState::Locked {
                version: version(80),
                owner: owner(11),
            }
        );
    }

    #[test]
    fn detached_guard_is_a_plain_static_token_with_bounded_layout() {
        fn assert_static_send_sync<T: 'static + Send + Sync>() {}

        assert_static_send_sync::<DetachedVersionGuard>();
        assert!(!std::mem::needs_drop::<DetachedVersionGuard>());
        assert_eq!(std::mem::size_of::<AtomicVersion>(), 8);
        #[cfg(target_pointer_width = "64")]
        {
            assert_eq!(std::mem::size_of::<DetachedVersionGuard>(), 24);
            assert_eq!(std::mem::align_of::<DetachedVersionGuard>(), 8);
        }
    }
}
