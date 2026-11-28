//! Safe owners for native Masstree rows. Only this module crosses the row ABI.
//!
//! Row addresses originate from a private tree. Borrowed views retain that
//! tree through a Rust borrow; owning guards retain an Arc lease. Values have
//! independent native references and may outlive the tree.
//!
//! SAFETY: private pointers come from checked native allocations or this
//! module's inaccessible tree. Copyable row/leaf tokens carry no dereference
//! permission; every use checks the exact live table incarnation. That checked
//! owner accompanies every private native call, so rows need no backpointer. Prepared
//! writes own their value/history, and destruction never dereferences their row
//! token. Installation requires a guard or ticket for that same live row.
//! Protocol tickets require their owner to retain Rows through release or
//! quarantine. Slices borrow a pinned immutable value. Native latches serialize
//! snapshot pins and history mutation; copied embedded bytes use atomic loads
//! and a post-copy version check. Input/output borrows span each FFI call.
//! Destructors release each owner once, and ABI negotiation precedes allocation.
//! Native cursors never escape; an optional worker-owned RCU region may cover
//! several synchronous accesses but must close before a wait or commit phase.
#![allow(unsafe_code)]
use super::*;
use crate::{Runtime, ScanRequest, Tree, Worker};
use std::{
    cmp::Ordering,
    hash::{Hash, Hasher},
    sync::{
        atomic::{AtomicU64, Ordering as AtomicOrdering},
        Arc,
    },
};

#[repr(C)]
#[derive(Default)]
struct View {
    data: *const u8,
    size: usize,
    version: u64,
    time_term: u32,
    present: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct LeafStamp {
    leaf: *const c_void,
    version: u64,
}
#[repr(C)]
struct Insertion {
    before: LeafStamp,
    after: LeafStamp,
    new_count: usize,
    new_leaves: [LeafStamp; 130],
}
impl Default for Insertion {
    fn default() -> Self {
        Self {
            before: LeafStamp::default(),
            after: LeafStamp::default(),
            new_count: 0,
            new_leaves: [LeafStamp::default(); 130],
        }
    }
}
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct Visibility {
    pub term: u32,
    pub stable: [u32; 10],
    pub failed: u32,
}
unsafe extern "C" {
    fn mako_rows_abi() -> u64;
    fn mako_row_budget_create(maximum: u64, out: *mut *mut c_void) -> i32;
    fn mako_row_budget_drop(budget: *mut c_void);
    fn mako_row_budget_used(budget: *mut c_void) -> u64;
    fn mako_rows_bind(table: *mut c_void, tree: *mut mtree_sys::Tree);
    fn mako_rows_create(budget: *mut c_void, rows: u64, keys: u64, out: *mut *mut c_void) -> i32;
    fn mako_rows_destroy(table: *mut c_void);
    fn mako_rows_resolve(
        table: *mut c_void,
        worker: *mut mtree_sys::Thread,
        key: *const u8,
        n: usize,
        out: *mut *mut c_void,
        change: *mut Insertion,
    ) -> i32;
    fn mako_rows_find(
        table: *mut c_void,
        worker: *mut mtree_sys::Thread,
        key: *const u8,
        size: usize,
        row: *mut *mut c_void,
        stamp: *mut LeafStamp,
    ) -> i32;
    fn mako_row_index_check(stamp: LeafStamp) -> i32;
    fn mako_row_index_seal(tree: *mut mtree_sys::Tree) -> i32;
    fn mako_row_index_scan(
        tree: *mut mtree_sys::Tree,
        worker: *mut mtree_sys::Thread,
        direction: u32,
        lower: *const mtree_sys::ScanBound,
        upper: *const mtree_sys::ScanBound,
        rows: *mut u64,
        capacity: usize,
        key_budget: usize,
        leaves: *mut LeafStamp,
        leaf_capacity: usize,
        count: *mut usize,
        leaf_count: *mut usize,
        end: *mut u32,
    ) -> i32;
    fn mako_rows_generation(table: *mut c_void) -> u64;
    fn mako_rows_count(table: *mut c_void) -> u64;
    fn mako_rows_floor(table: *mut c_void, floor: u32);
    fn mako_row_key(row: *mut c_void, data: *mut *const u8, size: *mut usize);
    fn mako_row_read(
        table: *mut c_void,
        row: *mut c_void,
        owner: u32,
        visibility: *const Visibility,
        checkpoint: u32,
        out: *mut *mut c_void,
        view: *mut View,
    ) -> i32;
    fn mako_row_copy(
        table: *mut c_void,
        row: *mut c_void,
        data: *mut u8,
        capacity: usize,
        view: *mut View,
    ) -> i32;
    fn mako_value_new(
        bytes: *const u8,
        n: usize,
        present: u32,
        out: *mut *mut c_void,
        view: *mut View,
    ) -> i32;
    fn mako_value_clone(value: *mut c_void) -> *mut c_void;
    fn mako_value_drop(value: *mut c_void);
    fn mako_row_check(table: *mut c_void, row: *mut c_void, version: u64, owner: u32) -> i32;
    fn mako_row_lock(table: *mut c_void, row: *mut c_void, owner: u32) -> i32;
    fn mako_write_prepare(
        table: *mut c_void,
        row: *mut c_void,
        owner: u32,
        value: *mut c_void,
        out: *mut *mut c_void,
    ) -> i32;
    fn mako_write_drop(write: *mut c_void);
    fn mako_row_install(
        table: *mut c_void,
        row: *mut c_void,
        owner: u32,
        write: *mut c_void,
        version: u64,
        time: u32,
    ) -> i32;
    fn mako_row_unlock(table: *mut c_void, row: *mut c_void, owner: u32, disposition: u32) -> i32;
}
#[inline]
fn owner_tag(owner: Option<u32>) -> Result<u32, Error> {
    match owner {
        None => Ok(0),
        Some(owner) => owner
            .checked_add(1)
            .ok_or(Error::Native(NativeStatus::Invalid)),
    }
}
#[inline]
fn verify_abi() -> Result<(), Error> {
    // The linked private ABI cannot change during this process. Cache both a
    // match and a mismatch before allowing any allocation through that ABI.
    static MATCHES: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    let expected = (4u64 << 48)
        | ((std::mem::size_of::<View>() as u64) << 32)
        | ((std::mem::align_of::<View>() as u64) << 24)
        | ((std::mem::size_of::<Visibility>() as u64) << 8)
        | std::mem::align_of::<Visibility>() as u64;
    if *MATCHES.get_or_init(|| unsafe { mako_rows_abi() } == expected) {
        Ok(())
    } else {
        Err(Error::AbiMismatch("native row ABI/layout mismatch"))
    }
}
#[derive(Debug)]
struct BudgetOwner(NonZeroUsize);
impl BudgetOwner {
    fn ptr(&self) -> *mut c_void {
        ptr::with_exposed_provenance_mut(self.0.get())
    }
}
impl Drop for BudgetOwner {
    fn drop(&mut self) {
        unsafe { mako_row_budget_drop(self.ptr()) }
    }
}
/// Shared bound for native row headers and retained keys. Payload and version
/// history buffers are separate; this is not a process RSS limit.
#[derive(Clone, Debug)]
pub struct Budget(Arc<BudgetOwner>);
impl Budget {
    pub fn new(maximum: u64) -> Result<Self, Error> {
        verify_abi()?;
        let mut raw = ptr::null_mut();
        status_result(unsafe { mako_row_budget_create(maximum, &mut raw) })?;
        Ok(Self(Arc::new(BudgetOwner(
            NonZeroUsize::new(raw.expose_provenance()).expect("native budget"),
        ))))
    }
    pub fn used(&self) -> u64 {
        unsafe { mako_row_budget_used(self.0.ptr()) }
    }
}
#[derive(Debug)]
struct Store {
    identity: u64,
    raw: NonZeroUsize,
    tree: Tree,
}
impl Store {
    fn ptr(&self) -> *mut c_void {
        ptr::with_exposed_provenance_mut(self.raw.get())
    }
}
impl Drop for Store {
    fn drop(&mut self) {
        unsafe { mako_rows_destroy(self.ptr()) }
    }
}
#[derive(Clone, Debug)]
pub struct Rows(Arc<Store>);
impl Rows {
    pub fn new(
        runtime: &Runtime,
        worker: &Worker,
        max_rows: u64,
        max_keys: u64,
    ) -> Result<Self, Error> {
        Self::with_budget(runtime, worker, max_rows, max_keys, &Budget::new(u64::MAX)?)
    }
    pub fn with_budget(
        runtime: &Runtime,
        worker: &Worker,
        max_rows: u64,
        max_keys: u64,
        budget: &Budget,
    ) -> Result<Self, Error> {
        static NEXT_STORE: AtomicU64 = AtomicU64::new(1);
        let identity = NEXT_STORE
            .fetch_update(AtomicOrdering::Relaxed, AtomicOrdering::Relaxed, |id| {
                id.checked_add(1)
            })
            .map_err(|_| Error::Native(NativeStatus::OutOfMemory))?;
        let mut raw = ptr::null_mut();
        // Charge native row ownership before consuming a process-lived directory.
        status_result(unsafe { mako_rows_create(budget.0.ptr(), max_rows, max_keys, &mut raw) })?;
        let tree = match runtime.create_tree(worker) {
            Ok(tree) => tree,
            Err(e) => {
                unsafe { mako_rows_destroy(raw) };
                return Err(e);
            }
        };
        unsafe { mako_rows_bind(raw, tree.inner.raw.as_mut_ptr()) };
        Ok(Self(Arc::new(Store {
            identity,
            raw: NonZeroUsize::new(raw.expose_provenance()).expect("native rows"),
            tree,
        })))
    }
    #[inline]
    pub fn locate(&self, worker: &Worker, key: &[u8]) -> Result<Located, Error> {
        worker.ensure(&self.0.tree.inner.runtime)?;
        let mut raw = ptr::null_mut();
        let mut stamp = LeafStamp::default();
        status_result(unsafe {
            mako_rows_find(
                self.0.ptr(),
                worker.raw.as_mut_ptr(),
                key.as_ptr(),
                key.len(),
                &mut raw,
                &mut stamp,
            )
        })?;
        Ok(
            if let Some(raw) = NonZeroUsize::new(raw.expose_provenance()) {
                Located::Row(RowToken {
                    raw,
                    table: self.0.identity,
                })
            } else {
                Located::Missing(
                    LeafToken {
                        raw: NonZeroUsize::new(stamp.leaf.expose_provenance())
                            .expect("native leaf"),
                        table: self.0.identity,
                    },
                    stamp.version,
                )
            },
        )
    }
    #[inline]
    pub fn lookup(&self, worker: &Worker, key: &[u8]) -> Result<Lookup<'_>, Error> {
        Ok(match self.locate(worker, key)? {
            Located::Row(token) => Lookup {
                row: Some(self.borrow(token)?),
                observed: None,
            },
            Located::Missing(leaf, version) => Lookup {
                row: None,
                observed: Some(LeafObservation {
                    leaf: Leaf {
                        raw: leaf.raw,
                        store: self.0.clone(),
                    },
                    version,
                }),
            },
        })
    }
    pub fn resolve(&self, worker: &Worker, key: &[u8]) -> Result<(Row, bool), Error> {
        let (row, change) = self.resolve_observed(worker, key)?;
        Ok((row.into_owned(), change.is_some()))
    }
    #[inline]
    pub fn resolve_observed(
        &self,
        worker: &Worker,
        key: &[u8],
    ) -> Result<(RowRef<'_>, Option<LeafChange>), Error> {
        let lookup = self.lookup(worker, key)?;
        if let Some(row) = lookup.row {
            return Ok((row, None));
        }
        let mut raw = ptr::null_mut();
        let mut change = Insertion::default();
        status_result(unsafe {
            mako_rows_resolve(
                self.0.ptr(),
                worker.raw.as_mut_ptr(),
                key.as_ptr(),
                key.len(),
                &mut raw,
                &mut change,
            )
        })?;
        let inserted = (!change.before.leaf.is_null()).then(|| LeafChange {
            before: self.observation(change.before),
            after: change.after.version,
            created: change.new_leaves[..change.new_count]
                .iter()
                .map(|s| self.observation(*s))
                .collect(),
        });
        Ok((self.borrowed_row(raw.expose_provenance()), inserted))
    }
    fn observation(&self, stamp: LeafStamp) -> LeafObservation {
        LeafObservation {
            leaf: Leaf {
                raw: NonZeroUsize::new(stamp.leaf.expose_provenance()).expect("native leaf"),
                store: self.0.clone(),
            },
            version: stamp.version,
        }
    }
    fn row(&self, raw: usize) -> Row {
        Row {
            raw: NonZeroUsize::new(raw).expect("native row"),
            store: RowOwner::Owned(store_lease(&self.0)),
        }
    }
    fn borrowed_row(&self, raw: usize) -> RowRef<'_> {
        RowRef {
            raw: NonZeroUsize::new(raw).expect("native row"),
            store: RowOwner::Borrowed(&self.0),
        }
    }
    /// A token carries identity, not permission to dereference an address. The
    /// table incarnation is checked before returning a lifetime-bound view.
    #[inline]
    pub fn borrow(&self, token: RowToken) -> Result<RowRef<'_>, Error> {
        if token.table != self.0.identity {
            return Err(Error::WrongRuntime);
        }
        Ok(self.borrowed_row(token.raw.get()))
    }
    /// Validates a leaf identity only within the exact live table incarnation.
    #[inline]
    pub fn check_leaf(&self, token: LeafToken, version: u64) -> Result<(), Error> {
        if token.table != self.0.identity {
            return Err(Error::WrongRuntime);
        }
        status_result(unsafe {
            mako_row_index_check(LeafStamp {
                leaf: ptr::with_exposed_provenance(token.raw.get()),
                version,
            })
        })
    }
    /// Acquires a lock for a protocol that retains this Rows owner itself.
    /// Dropping a ticket never unlocks; the owner must explicitly release it
    /// or retain the whole table as quarantined state on an uncertain outcome.
    pub fn lock_ticket(&self, token: RowToken, owner: u32) -> Result<LockTicket, Error> {
        let row = self.borrow(token)?;
        let owner = owner_tag(Some(owner))?;
        status_result(unsafe { mako_row_lock(self.0.ptr(), row.ptr(), owner) })?;
        Ok(LockTicket { row: token, owner })
    }
    pub fn install_ticket(
        &self,
        ticket: &mut LockTicket,
        write: &mut Write,
        version: u64,
        time: u32,
    ) -> Result<(), Error> {
        if ticket.owner == 0 || ticket.row != write.row {
            return Err(Error::Native(NativeStatus::Invalid));
        }
        let row = self.borrow(ticket.row)?;
        status_result(unsafe {
            mako_row_install(
                self.0.ptr(),
                row.ptr(),
                ticket.owner,
                ptr::with_exposed_provenance_mut(write.raw.get()),
                version,
                time,
            )
        })
    }
    pub fn release_ticket(&self, ticket: &mut LockTicket, disposition: u32) -> Result<(), Error> {
        if ticket.owner == 0 {
            return Err(Error::Native(NativeStatus::Invalid));
        }
        let row = self.borrow(ticket.row)?;
        let result = status_result(unsafe {
            mako_row_unlock(self.0.ptr(), row.ptr(), ticket.owner, disposition)
        });
        ticket.owner = 0;
        result
    }
    pub fn generation(&self) -> u64 {
        unsafe { mako_rows_generation(self.0.ptr()) }
    }
    pub fn len(&self) -> u64 {
        unsafe { mako_rows_count(self.0.ptr()) }
    }
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
    pub fn advance_floor(&self, floor: u32) {
        unsafe { mako_rows_floor(self.0.ptr(), floor) }
    }
    pub fn seal(&self) -> Result<(), Error> {
        status_result(unsafe { mako_row_index_seal(self.0.tree.inner.raw.as_mut_ptr()) })
    }
    /// Returns native row owners and leaf observations without copying keys.
    pub fn scan(&self, worker: &Worker, request: ScanRequest<'_>) -> Result<RowChunk, Error> {
        worker.ensure(&self.0.tree.inner.runtime)?;
        self.0.tree.check_bound(request.lower)?;
        self.0.tree.check_bound(request.upper)?;
        let capacity = request.entry_capacity;
        let leaf_capacity = capacity
            .checked_mul(2)
            .and_then(|n| n.checked_add(130))
            .ok_or(Error::Native(NativeStatus::OutOfMemory))?;
        let mut addresses = Vec::new();
        let mut stamps = Vec::new();
        addresses
            .try_reserve_exact(capacity)
            .map_err(|_| Error::Native(NativeStatus::OutOfMemory))?;
        stamps
            .try_reserve_exact(leaf_capacity)
            .map_err(|_| Error::Native(NativeStatus::OutOfMemory))?;
        addresses.resize(capacity, 0u64);
        stamps.resize(leaf_capacity, LeafStamp::default());
        let lower = encode_bound(request.lower);
        let upper = encode_bound(request.upper);
        let direction = match request.direction {
            ScanDirection::Forward => 1,
            ScanDirection::Reverse => 2,
        };
        let (mut count, mut leaf_count, mut end) = (0, 0, 0);
        status_result(unsafe {
            mako_row_index_scan(
                self.0.tree.inner.raw.as_mut_ptr(),
                worker.raw.as_mut_ptr(),
                direction,
                &lower,
                &upper,
                addresses.as_mut_ptr(),
                capacity,
                request.key_arena_capacity,
                stamps.as_mut_ptr(),
                leaf_capacity,
                &mut count,
                &mut leaf_count,
                &mut end,
            )
        })?;
        Ok(RowChunk {
            entries: addresses[..count]
                .iter()
                .map(|p| self.row(*p as usize))
                .collect(),
            observations: stamps[..leaf_count]
                .iter()
                .map(|s| self.observation(*s))
                .collect(),
            end: end != 0,
        })
    }
}
pub struct RowChunk {
    pub entries: Vec<Row>,
    pub observations: Vec<LeafObservation>,
    pub end: bool,
}
/// Allocation-free identities for a table-bound point access.
#[derive(Clone, Copy, Debug)]
pub enum Located {
    Row(RowToken),
    Missing(LeafToken, u64),
}
pub struct Lookup<'a> {
    pub row: Option<RowRef<'a>>,
    pub observed: Option<LeafObservation>,
}
pub struct LeafChange {
    pub before: LeafObservation,
    pub after: u64,
    pub created: Vec<LeafObservation>,
}
pub struct LeafObservation {
    pub leaf: Leaf,
    pub version: u64,
}
/// Append-only tree leaves remain allocated while this native table owner lives.
/// The handle retains lifetime, not an RCU region or a structural lock.
#[derive(Clone, Debug)]
pub struct Leaf {
    raw: NonZeroUsize,
    store: Arc<Store>,
}
/// Copyable leaf identity, usable only through its exact live Rows owner.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct LeafToken {
    raw: NonZeroUsize,
    table: u64,
}
impl Hash for LeafToken {
    #[inline]
    fn hash<H: Hasher>(&self, hasher: &mut H) {
        self.raw.hash(hasher);
    }
}

impl Leaf {
    pub fn token(&self) -> LeafToken {
        LeafToken {
            raw: self.raw,
            table: self.store.identity,
        }
    }

    #[inline]
    pub fn check(&self, version: u64) -> Result<(), Error> {
        // Retain the native table even for a predicate held across a 2PC wait.
        let _owner = &self.store;
        status_result(unsafe {
            mako_row_index_check(LeafStamp {
                leaf: ptr::with_exposed_provenance(self.raw.get()),
                version,
            })
        })
    }
}
impl PartialEq for Leaf {
    fn eq(&self, other: &Self) -> bool {
        self.raw == other.raw
    }
}
impl Eq for Leaf {}
impl PartialOrd for Leaf {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}
impl Ord for Leaf {
    fn cmp(&self, other: &Self) -> Ordering {
        self.raw.cmp(&other.raw)
    }
}
impl Hash for Leaf {
    fn hash<H: Hasher>(&self, h: &mut H) {
        self.raw.hash(h);
    }
}
/// Copyable identity for STO's item set. Every dereference requires its exact
/// still-live table. The monotone incarnation prevents allocator-address reuse
/// from making a token valid in a later table.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
pub struct RowToken {
    raw: NonZeroUsize,
    table: u64,
}
impl Hash for RowToken {
    #[inline]
    fn hash<H: Hasher>(&self, hasher: &mut H) {
        self.raw.hash(hasher);
    }
}

// Owning guards created by one worker share a lifetime lease. The weak slot
// holds no table alive and stores neither keys nor values. It avoids bouncing
// the table's single Arc counter for every owning guard clone on every CPU.
#[derive(Debug)]
struct StoreLease {
    store: Arc<Store>,
}
fn store_lease(store: &Arc<Store>) -> Arc<StoreLease> {
    thread_local! {
        static LAST: std::cell::RefCell<std::sync::Weak<StoreLease>> = const { std::cell::RefCell::new(std::sync::Weak::new()) };
    }
    LAST.with(|last| {
        let mut last = last.borrow_mut();
        if let Some(lease) = last.upgrade() {
            if Arc::ptr_eq(&lease.store, store) {
                return lease;
            }
        }
        let lease = Arc::new(StoreLease {
            store: store.clone(),
        });
        *last = Arc::downgrade(&lease);
        lease
    })
}
#[derive(Clone, Debug)]
enum RowOwner<'a> {
    Owned(Arc<StoreLease>),
    Borrowed(&'a Arc<Store>),
}
impl RowOwner<'_> {
    fn arc(&self) -> &Arc<Store> {
        match self {
            Self::Owned(s) => &s.store,
            Self::Borrowed(s) => s,
        }
    }
}
/// An owning row is available for callers that need it beyond a table borrow.
pub type Row = RowRef<'static>;
#[derive(Clone, Debug)]
pub struct RowRef<'a> {
    raw: NonZeroUsize,
    store: RowOwner<'a>,
}
impl RowRef<'_> {
    #[inline]
    pub fn token(&self) -> RowToken {
        RowToken {
            raw: self.raw,
            table: self.store.arc().identity,
        }
    }
    pub fn into_owned(self) -> Row {
        let store = match self.store {
            RowOwner::Owned(s) => s,
            RowOwner::Borrowed(s) => store_lease(s),
        };
        RowRef {
            raw: self.raw,
            store: RowOwner::Owned(store),
        }
    }
    fn ptr(&self) -> *mut c_void {
        ptr::with_exposed_provenance_mut(self.raw.get())
    }
    pub fn key(&self) -> &[u8] {
        let mut data = ptr::null();
        let mut size = 0;
        unsafe { mako_row_key(self.ptr(), &mut data, &mut size) };
        if size == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(data, size) }
        }
    }
    /// Allocates private publication scratch before lock acquisition. This
    /// does not mutate the row; only its exact guard may install this write.
    pub fn prepare(&self, value: &Value) -> Result<Write, Error> {
        self.prepare_owned(0, value)
    }
    fn prepare_owned(&self, owner: u32, value: &Value) -> Result<Write, Error> {
        let mut raw = ptr::null_mut();
        status_result(unsafe {
            mako_write_prepare(
                self.store.arc().ptr(),
                self.ptr(),
                owner,
                value.ptr(),
                &mut raw,
            )
        })?;
        Ok(Write {
            raw: NonZeroUsize::new(raw.expose_provenance()).expect("native write"),
            row: self.token(),
        })
    }
    pub fn identity(&self) -> u64 {
        self.raw.get() as u64
    }
    #[inline]
    pub fn read(
        &self,
        owner: Option<u32>,
        visibility: Option<Visibility>,
        checkpoint: bool,
    ) -> Result<Snapshot, Error> {
        let mut raw = ptr::null_mut();
        let mut view = View::default();
        status_result(unsafe {
            mako_row_read(
                self.store.arc().ptr(),
                self.ptr(),
                owner_tag(owner)?,
                visibility.as_ref().map_or(ptr::null(), |v| v),
                u32::from(checkpoint),
                &mut raw,
                &mut view,
            )
        })?;
        let version = view.version;
        let time_term = view.time_term;
        Ok(Snapshot {
            value: Value::from_view(raw, view),
            version,
            time_term,
        })
    }
    /// Copies the latest value into a reusable caller buffer. Native validates
    /// atomic embedded bytes or holds the latch while copying a heap buffer.
    /// No native buffer reference has to be pinned.
    /// A resize retries the read, so only the completed copy's version escapes.
    #[inline]
    pub fn read_into(&self, buffer: &mut Vec<u8>) -> Result<(bool, u64), Error> {
        buffer.clear();
        loop {
            let mut view = View::default();
            // SAFETY: the allocation remains exclusively borrowed throughout
            // the call. Native writes at most capacity initialized bytes and
            // reports their exact count after native version validation or
            // a latched heap-buffer copy.
            let status = unsafe {
                mako_row_copy(
                    self.store.arc().ptr(),
                    self.ptr(),
                    buffer.as_mut_ptr(),
                    buffer.capacity(),
                    &mut view,
                )
            };
            if status_result(status) == Err(Error::Native(NativeStatus::BufferTooSmall)) {
                buffer
                    .try_reserve(view.size)
                    .map_err(|_| Error::Native(NativeStatus::OutOfMemory))?;
                continue;
            }
            status_result(status)?;
            assert!(view.size <= buffer.capacity());
            // SAFETY: success initialized exactly view.size bytes above.
            unsafe { buffer.set_len(view.size) };
            return Ok((view.present != 0, view.version));
        }
    }
    #[inline]
    pub fn check(&self, version: u64, owner: Option<u32>) -> Result<(), Error> {
        status_result(unsafe {
            mako_row_check(
                self.store.arc().ptr(),
                self.ptr(),
                version,
                owner_tag(owner)?,
            )
        })
    }
    pub fn lock(&self, owner: u32) -> Result<Guard, Error> {
        status_result(unsafe {
            mako_row_lock(self.store.arc().ptr(), self.ptr(), owner_tag(Some(owner))?)
        })?;
        Ok(Guard {
            row: self.clone().into_owned(),
            owner: owner_tag(Some(owner))?,
            held: true,
        })
    }
}
impl PartialEq for RowRef<'_> {
    fn eq(&self, r: &Self) -> bool {
        self.raw == r.raw && Arc::ptr_eq(self.store.arc(), r.store.arc())
    }
}
impl Eq for RowRef<'_> {}
impl PartialOrd for RowRef<'_> {
    fn partial_cmp(&self, r: &Self) -> Option<Ordering> {
        Some(self.cmp(r))
    }
}
impl Ord for RowRef<'_> {
    fn cmp(&self, r: &Self) -> Ordering {
        self.raw.cmp(&r.raw)
    }
}
impl Hash for RowRef<'_> {
    fn hash<H: Hasher>(&self, h: &mut H) {
        self.raw.hash(h)
    }
}
/// An owning native buffer handle. A borrowed slice cannot outlive its owner.
/// ```compile_fail
/// let bytes = {
///     let value = masstree::rows::Value::new(Some(b"owned")).unwrap();
///     value.bytes().unwrap()
/// };
/// assert_eq!(bytes, b"owned");
/// ```
#[derive(Debug)]
pub struct Value {
    raw: NonZeroUsize,
    data: usize,
    size: usize,
    present: bool,
}
impl Value {
    #[inline]
    fn from_view(raw: *mut c_void, view: View) -> Self {
        Self {
            raw: NonZeroUsize::new(raw.expose_provenance()).expect("native value"),
            data: view.data.expose_provenance(),
            size: view.size,
            present: view.present != 0,
        }
    }
    fn ptr(&self) -> *mut c_void {
        ptr::with_exposed_provenance_mut(self.raw.get())
    }
    #[inline]
    pub fn new(value: Option<&[u8]>) -> Result<Self, Error> {
        verify_abi()?;
        let mut raw = ptr::null_mut();
        let mut view = View::default();
        let bytes = value.unwrap_or_default();
        status_result(unsafe {
            mako_value_new(
                bytes.as_ptr(),
                bytes.len(),
                u32::from(value.is_some()),
                &mut raw,
                &mut view,
            )
        })?;
        Ok(Self::from_view(raw, view))
    }
    #[inline]
    pub fn bytes(&self) -> Option<&[u8]> {
        self.present.then(|| {
            if self.size == 0 {
                &[]
            } else {
                // SAFETY: this handle owns a native reference to the immutable
                // allocation described at creation; the slice borrows that owner.
                unsafe {
                    std::slice::from_raw_parts(ptr::with_exposed_provenance(self.data), self.size)
                }
            }
        })
    }
}
impl Clone for Value {
    fn clone(&self) -> Self {
        unsafe { mako_value_clone(self.ptr()) };
        Self {
            raw: self.raw,
            data: self.data,
            size: self.size,
            present: self.present,
        }
    }
}
impl Drop for Value {
    fn drop(&mut self) {
        unsafe { mako_value_drop(self.ptr()) }
    }
}
#[derive(Clone, Debug)]
pub struct Snapshot {
    pub value: Value,
    pub version: u64,
    pub time_term: u32,
}
/// A move-only lock capability for an explicitly retained protocol owner.
/// Every use validates its unforgeable token against the live table. It stores
/// no dereferenceable pointer independently and has no destructor. In contrast,
/// Guard owns its table and fences on abandonment for standalone callers.
#[must_use = "explicitly release the ticket or retain its table as quarantined state"]
pub struct LockTicket {
    row: RowToken,
    owner: u32,
}
pub struct Guard {
    row: Row,
    owner: u32,
    held: bool,
}
impl Guard {
    pub fn prepare(&self, value: &Value) -> Result<Write, Error> {
        self.row.prepare_owned(self.owner, value)
    }
    pub fn install(&mut self, write: &mut Write, version: u64, time: u32) -> Result<(), Error> {
        if !self.held || self.row.token() != write.row {
            return Err(Error::Native(NativeStatus::Invalid));
        }
        status_result(unsafe {
            mako_row_install(
                self.row.store.arc().ptr(),
                self.row.ptr(),
                self.owner,
                ptr::with_exposed_provenance_mut(write.raw.get()),
                version,
                time,
            )
        })
    }
    /// 0 abort, 1 committed, 2 indeterminate (fences the native table).
    pub fn release(&mut self, disposition: u32) -> Result<(), Error> {
        if !self.held {
            return Err(Error::Native(NativeStatus::Invalid));
        }
        let result = status_result(unsafe {
            mako_row_unlock(
                self.row.store.arc().ptr(),
                self.row.ptr(),
                self.owner,
                disposition,
            )
        });
        self.held = false;
        result
    }
}
impl Drop for Guard {
    fn drop(&mut self) {
        if self.held {
            let _ = self.release(2);
        }
    }
}
pub struct Write {
    raw: NonZeroUsize,
    // Native scratch owns buffers and history nodes, but never dereferences
    // its row during destruction. Installation requires a still-live guard
    // with this exact table incarnation and row identity.
    row: RowToken,
}
impl Drop for Write {
    fn drop(&mut self) {
        unsafe { mako_write_drop(ptr::with_exposed_provenance_mut(self.raw.get())) }
    }
}
