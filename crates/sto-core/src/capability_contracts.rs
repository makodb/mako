//! Compiler-enforced capability contracts, deliberately independent of rustc's
//! diagnostic wording. Each forbidden operation has its own compile-fail case
//! so, for example, a Send error cannot hide a regressed lifetime restriction.
//! Positive controls compile the same datatype support and valid borrow shapes.
//! This module exists only during rustdoc testing; it adds no production API.

// rustfmt re-indents the `#[doc = concat!(...)]` arguments inside these macro
// bodies by eight more columns on every pass, so `cargo fmt -- --check` can
// never agree with `cargo fmt` here. Keep the macros out of its reach.
#[rustfmt::skip]
macro_rules! borrowed_capability {
    ($name:ident, $borrowed:ty, $static:ty) => {
        #[doc = concat!(
            "A borrow with its original lifetime is legal.\n\n```\n",
            "mod support {\n", include_str!("../tests/ui/support/mod.rs"), "\n}\n",
            "fn identity<'scope>(value: ", stringify!($borrowed), ") -> ",
            stringify!($borrowed), " { value }\nfn main() {}\n```\n\n",
            "The same borrow cannot be widened to static.\n\n```compile_fail\n",
            "mod support {\n", include_str!("../tests/ui/support/mod.rs"), "\n}\n",
            "fn escape<'scope>(value: ", stringify!($borrowed), ") -> ",
            stringify!($static), " { value }\nfn main() {}\n```\n",
        )]
        pub struct $name;
    };
}

#[rustfmt::skip]
macro_rules! thread_affinity {
    ($name:ident, $ty:ty) => {
        #[doc = concat!(
            "This capability cannot be sent to another thread.\n\n```compile_fail\n",
            "mod support {\n", include_str!("../tests/ui/support/mod.rs"), "\n}\n",
            "fn require_send<T: Send>() {}\n",
            "fn main() { require_send::<", stringify!($ty), ">(); }\n```\n\n",
            "Nor can it be shared between threads.\n\n```compile_fail\n",
            "mod support {\n", include_str!("../tests/ui/support/mod.rs"), "\n}\n",
            "fn require_sync<T: Sync>() {}\n",
            "fn main() { require_sync::<", stringify!($ty), ">(); }\n```\n",
        )]
        pub struct $name;
    };
}

borrowed_capability!(
    AcquireBorrow,
    sto_core::AcquireContext<'scope>,
    sto_core::AcquireContext<'static>
);
thread_affinity!(AcquireAffinity, sto_core::AcquireContext<'static>);
borrowed_capability!(
    ExecutionBorrow,
    sto_core::ExecutionCheckContext<'scope>,
    sto_core::ExecutionCheckContext<'static>
);
thread_affinity!(ExecutionAffinity, sto_core::ExecutionCheckContext<'static>);
borrowed_capability!(
    CleanupBorrow,
    sto_core::CleanupContext<'scope>,
    sto_core::CleanupContext<'static>
);
thread_affinity!(CleanupAffinity, sto_core::CleanupContext<'static>);
borrowed_capability!(
    InstallBorrow,
    sto_core::InstallContext<'scope>,
    sto_core::InstallContext<'static>
);
thread_affinity!(InstallAffinity, sto_core::InstallContext<'static>);
borrowed_capability!(
    LockBorrow,
    sto_core::LockContext<'scope>,
    sto_core::LockContext<'static>
);
thread_affinity!(LockAffinity, sto_core::LockContext<'static>);
borrowed_capability!(
    PredicateBorrow,
    sto_core::PredicateContext<'scope>,
    sto_core::PredicateContext<'static>
);
thread_affinity!(PredicateAffinity, sto_core::PredicateContext<'static>);
borrowed_capability!(
    ReleaseBorrow,
    sto_core::ReleaseContext<'scope>,
    sto_core::ReleaseContext<'static>
);
thread_affinity!(ReleaseAffinity, sto_core::ReleaseContext<'static>);
borrowed_capability!(
    ValidationBorrow,
    sto_core::ValidationContext<'scope>,
    sto_core::ValidationContext<'static>
);
thread_affinity!(ValidationAffinity, sto_core::ValidationContext<'static>);
borrowed_capability!(
    PreflightFreeBorrow,
    sto_core::PreflightFreeValidationContext<'scope>,
    sto_core::PreflightFreeValidationContext<'static>
);
thread_affinity!(
    PreflightFreeAffinity,
    sto_core::PreflightFreeValidationContext<'static>
);
borrowed_capability!(
    ProxyBorrow,
    sto_core::TransProxy<'scope, support::Adapter>,
    sto_core::TransProxy<'static, support::Adapter>
);
thread_affinity!(
    ProxyAffinity,
    sto_core::TransProxy<'static, support::Adapter>
);
borrowed_capability!(
    CleanupItemBorrow,
    sto_core::CleanupItem<'scope, support::Adapter>,
    sto_core::CleanupItem<'static, support::Adapter>
);
thread_affinity!(
    CleanupItemAffinity,
    sto_core::CleanupItem<'static, support::Adapter>
);
borrowed_capability!(
    InstallItemBorrow,
    sto_core::InstallItem<'scope, support::Adapter>,
    sto_core::InstallItem<'static, support::Adapter>
);
thread_affinity!(
    InstallItemAffinity,
    sto_core::InstallItem<'static, support::Adapter>
);
borrowed_capability!(
    PreflightItemBorrow,
    sto_core::PreflightItem<'scope, support::Adapter>,
    sto_core::PreflightItem<'static, support::Adapter>
);
thread_affinity!(
    PreflightItemAffinity,
    sto_core::PreflightItem<'static, support::Adapter>
);
borrowed_capability!(
    ResolvedSessionBorrow,
    sto_core::ResolvedItemSession<'scope, support::Adapter>,
    sto_core::ResolvedItemSession<'static, support::Adapter>
);
thread_affinity!(WorkerAffinity, sto_core::WorkerContext);
thread_affinity!(GuardAffinity, support::LocalGuard);

#[doc = concat!(
    "A held guard can be borrowed for the context lifetime.\n\n```\n",
    "mod support {\n", include_str!("../tests/ui/support/mod.rs"), "\n}\n",
    "fn borrow_guard<'a>(context: &'a sto_core::ValidationContext<'a>,\n",
    "    lock: &'a sto_core::LockUse<support::LocalLock>) -> &'a support::LocalGuard {\n",
    "    context.guard(lock).unwrap()\n}\nfn main() {}\n```\n\n",
    "A held guard cannot escape its context.\n\n```compile_fail\n",
    "mod support {\n", include_str!("../tests/ui/support/mod.rs"), "\n}\n",
    "fn escape_guard<'a>(context: &'a sto_core::ValidationContext<'a>,\n",
    "    lock: &'a sto_core::LockUse<support::LocalLock>) -> &'static support::LocalGuard {\n",
    "    context.guard(lock).unwrap()\n}\nfn main() {}\n```\n",
)]
pub struct GuardBorrow;

/// Key uniqueness proofs borrow their original key storage.
///
/// ```
/// fn borrowed(keys: &[()]) -> sto_core::UniqueItemKeys<'_, ()> {
///     sto_core::UniqueItemKeys::try_new(keys).unwrap()
/// }
/// ```
///
/// ```compile_fail
/// fn escape(keys: &[()]) -> sto_core::UniqueItemKeys<'static, ()> {
///     sto_core::UniqueItemKeys::try_new(keys).unwrap()
/// }
/// ```
pub struct UniqueKeysBorrow;

/// Prepared-free validation does not confer a held-guard capability.
///
/// ```compile_fail
/// fn cannot_resolve_a_guard<L: sto_core::TransactionLock>(
///     context: &sto_core::PreflightFreeValidationContext<'_>,
///     lock: &sto_core::LockUse<L>,
/// ) {
///     let _ = context.guard(lock);
/// }
/// ```
pub struct PreflightFreeAuthority;

/// Terminal-read transactions never expose ordinary item access, including
/// before a terminal read has been performed.
///
/// ```compile_fail
/// fn ordinary_access_is_unavailable<A: sto_core::TObject>(
///     transaction: &mut sto_core::TerminalReadTransaction<'_, sto_core::TerminalReadOpen>,
///     resource: &sto_core::RegisteredResource<A>,
///     key: A::Key,
/// ) {
///     let _ = transaction.with_item(resource, key, |_| Ok::<(), sto_core::AccessError>(()));
/// }
/// ```
///
/// ```compile_fail
/// fn later_access_is_unavailable<A: sto_core::TObject>(
///     transaction: &mut sto_core::TerminalReadTransaction<'_, sto_core::TerminalReadReady>,
///     resource: &sto_core::RegisteredResource<A>,
///     key: A::Key,
/// ) {
///     let _ = transaction.with_item(resource, key, |_| Ok::<(), sto_core::AccessError>(()));
/// }
/// ```
pub struct TerminalReadAuthority;
