//! Unsafe address-token reconstruction for the private native-directory lane.
//!
//! This module is the only place where an integer returned by Masstree becomes
//! a Rust record pointer. The safe caller must uphold the private-tree
//! invariant established by `Table::new_direct`: every nonzero directory value
//! was minted by `encode` from this exact registry, registry entries never move
//! or get reused, and the owning `TableShared` remains alive for the returned
//! borrow. Public resolved-token operations validate table identity before
//! reaching this module, while transaction resources and direct lock targets
//! retain `Arc<TableShared>` through their last possible dereference.

#![allow(unsafe_code)]

use std::{mem, num::NonZeroUsize, ptr, ptr::NonNull};

use masstree::RecordId;
use sto_core::{AccessError, CapacityError};

use super::{
    table_fault, RecordAccess, Registry, RegistryEntry, RegistrySlotAccess, StableRegistryEntry,
};

// Both concrete registry layouts reserve their low address bits through an
// explicit 64-byte alignment. Keep one bit in the cached pointer to remember
// whether the already-validated entry owns the adjacent bounded-value cell.
// `map_addr` preserves the allocation provenance while adding and removing the
// tag; no tagged pointer is ever dereferenced.
const STABLE_CELL_TAG: usize = 1;

const _: () = {
    assert!(mem::align_of::<RegistryEntry>() & STABLE_CELL_TAG == 0);
    assert!(mem::align_of::<StableRegistryEntry>() & STABLE_CELL_TAG == 0);
    assert!(mem::offset_of!(RegistryEntry, record) == 0);
    assert!(mem::offset_of!(StableRegistryEntry, base) == 0);
    assert!(mem::offset_of!(StableRegistryEntry, cell) == mem::size_of::<RegistryEntry>());
};

/// A record address cached only inside a held direct-lock guard.
///
/// The value cannot expose or dereference its pointer without a simultaneous
/// borrow of the originating registry. `TableShared` supplies that borrow only
/// after core has reborrowed and address-checked the exact target retained by
/// the live transaction item.
#[derive(Clone, Copy, Eq, PartialEq)]
pub(super) struct CachedRecord {
    tagged_entry: NonNull<RegistryEntry>,
}

impl CachedRecord {
    pub(super) fn new(access: RecordAccess<'_>) -> Self {
        // Record and RegistryEntry are both offset-zero fields in every
        // concrete direct-token allocation. The cast therefore keeps the
        // original allocation provenance and recovers the entry address.
        let entry = NonNull::from(access.record).cast::<RegistryEntry>();
        let address = entry.addr().get();
        debug_assert_eq!(address % mem::align_of::<RegistryEntry>(), 0);
        debug_assert_eq!(usize::try_from(access.record_id.get()), Ok(address));

        let tag = usize::from(access.stable.is_some()) * STABLE_CELL_TAG;
        let tagged_entry = entry.map_addr(|address| {
            NonZeroUsize::new(address.get() | tag)
                .expect("a tagged nonnull registry address cannot become zero")
        });
        Self { tagged_entry }
    }

    #[inline(always)]
    fn entry_and_stable_tag(&self) -> (NonNull<RegistryEntry>, bool) {
        let tagged_address = self.tagged_entry.addr().get();
        let has_stable_cell = tagged_address & STABLE_CELL_TAG != 0;
        let entry = self.tagged_entry.map_addr(|address| {
            let untagged = address.get() & !STABLE_CELL_TAG;
            // SAFETY: `new` starts with a nonzero RegistryEntry address whose
            // alignment clears the tag bit. Removing only that bit therefore
            // restores the same nonzero address.
            unsafe { NonZeroUsize::new_unchecked(untagged) }
        });
        (entry, has_stable_cell)
    }

    /// Returns the private directory token encoded by this cached entry.
    pub(super) fn record_id(self) -> RecordId {
        let (entry, _) = self.entry_and_stable_tag();
        let scalar = u64::try_from(entry.addr().get())
            .expect("a cached registry pointer must fit the directory scalar");
        RecordId::new(scalar).expect("a cached registry pointer cannot be zero")
    }

    /// Reborrows the stable record and optional bounded cell under their
    /// owning registry's lifetime.
    pub(super) fn get<'owner>(
        &self,
        _registry: &'owner Registry,
        token: RecordId,
    ) -> Result<RecordAccess<'owner>, AccessError> {
        let (entry_pointer, has_stable_cell) = self.entry_and_stable_tag();

        #[cfg(debug_assertions)]
        {
            let resolved = _registry.resolve_direct_access(token)?;
            if !ptr::eq(resolved.entry, entry_pointer.as_ptr())
                || resolved.stable.is_some() != has_stable_cell
            {
                return Err(table_fault(
                    "cached direct record access differs from its registry token",
                ));
            }
        }

        // SAFETY: `CachedRecord::new` receives only a borrow returned by this
        // registry's direct-token resolver. The stable tag comes from that
        // resolver's concrete arena variant. An untagged pointer therefore
        // names either a RegistryEntry or the offset-zero base of a fully
        // initialized StableRegistryEntry. Registry entries live in stable,
        // append-only boxed arenas and are never moved or reused. The live
        // `registry` borrow proves its owning `TableShared` still retains that
        // arena for the full returned lifetime. Core additionally verifies the
        // borrowed lock-target address before every callback that reaches this
        // method; the guard is the capability boundary, while arena ownership
        // is the memory-lifetime proof.
        let entry: &'owner RegistryEntry = unsafe { entry_pointer.as_ref() };
        let stable = has_stable_cell.then(|| {
            // SAFETY: only StableRegistryEntry resolutions set the tag, and
            // its RegistryEntry base is at offset zero. The same owner borrow
            // that retains the base also retains its adjacent atomic cell.
            let entry: &'owner StableRegistryEntry =
                unsafe { entry_pointer.cast::<StableRegistryEntry>().as_ref() };
            &entry.cell
        });
        Ok(RecordAccess {
            record_id: token,
            record: &entry.record,
            stable,
        })
    }
}

/// Exposes one stable registry-entry address as Masstree's nonzero scalar.
pub(super) fn encode(access: RegistrySlotAccess<'_>) -> Result<RecordId, AccessError> {
    let address = access.element_address;
    let scalar = u64::try_from(address).map_err(|_| CapacityError::BufferLimit)?;
    RecordId::new(scalar).ok_or_else(|| table_fault("record address token is zero"))
}

/// Hints that a private-directory token's stable registry line will be read.
///
/// This deliberately performs no validity load and creates no reference. The
/// ordinary later resolution remains the fail-closed pointer-shape, debug
/// arena-ownership, and slot-state boundary. Rust's x86 prefetch intrinsic is
/// a non-trapping address hint, so even a malformed token is not dereferenced
/// by this helper.
#[inline(always)]
pub(super) fn prefetch(token: RecordId) -> Result<(), AccessError> {
    let address = usize::try_from(token.get())
        .map_err(|_| table_fault("record address token exceeds the pointer domain"))?;
    let entry = ptr::with_exposed_provenance::<RegistryEntry>(address);
    super::record_prefetch::read_address(entry);
    Ok(())
}

/// Reconstructs a registry entry proven to originate in the owner's private
/// native directory.
///
/// The `registry` borrow supplies the maximum lifetime of the result. In debug
/// builds an allocation-range check also catches any accidental cross-table or
/// non-entry token before the unsafe dereference. Release builds rely on the
/// private-tree invariant documented at module scope so the hot path remains a
/// single strict-provenance integer-to-pointer reconstruction.
pub(super) fn resolve(
    registry: &Registry,
    token: RecordId,
) -> Result<RegistrySlotAccess<'_>, AccessError> {
    let address = decode_address(registry, token)?;
    if registry.config.bounded_atomic_values {
        let entry = ptr::with_exposed_provenance::<StableRegistryEntry>(address);
        // SAFETY: the immutable registry mode proves every token names the
        // offset-zero address of a StableRegistryEntry in this registry. The
        // private directory publishes only addresses produced by `encode`,
        // and the owning Registry borrow retains the complete Arc arena.
        let entry = unsafe { &*entry };
        Ok(RegistrySlotAccess {
            entry: &entry.base,
            stable: Some(&entry.cell),
            element_address: address,
        })
    } else {
        let entry = ptr::with_exposed_provenance::<RegistryEntry>(address);
        // SAFETY: the same private-directory invariant proves this token names
        // a standard RegistryEntry in a retained standard arena.
        let entry = unsafe { &*entry };
        Ok(RegistrySlotAccess {
            entry,
            stable: None,
            element_address: address,
        })
    }
}

/// Reconstructs a direct token as an address without dereferencing it.
#[inline(always)]
fn decode_address(_registry: &Registry, token: RecordId) -> Result<usize, AccessError> {
    let address = usize::try_from(token.get())
        .map_err(|_| table_fault("record address token exceeds the pointer domain"))?;
    if address % mem::align_of::<RegistryEntry>() != 0 {
        return Err(table_fault("record address token is misaligned"));
    }

    #[cfg(debug_assertions)]
    if !_registry.owns_entry_address(address) {
        return Err(table_fault(
            "record address token does not belong to this registry",
        ));
    }

    Ok(address)
}
