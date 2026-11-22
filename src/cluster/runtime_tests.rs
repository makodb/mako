//! Runtime cache races: engine identity and retained whole-snapshot semantics.
#![allow(unsafe_code)]
use super::*;
use crate::types::Boundary;
use std::ffi::c_void;
use std::sync::Barrier;

#[derive(Default)]
struct Indexes {
    // Keep actual distinct allocations alive until every borrowed handle ends.
    entries: Mutex<Vec<Box<HandleKey>>>,
}

// SAFETY: scoped threads borrow the live Indexes; the output belongs to Opener.
unsafe extern "C" fn open_index(context: *mut c_void, table: u64, warehouse: u32,
    owner: u32, proxy: u32, output: *mut usize) -> u32
{
    let indexes = unsafe { &*(context as *const Indexes) };
    let index = Box::new(HandleKey { table, warehouse, owner, proxy: proxy != 0 });
    let handle = (&*index as *const HandleKey) as usize;
    indexes.entries.lock().push(index);
    unsafe { *output = handle; }
    Status::Ok as u32
}

fn runtime_with_indexes(indexes: &Indexes) -> Runtime {
    let runtime = Runtime::new(true);
    for table in [1, 2] {
        assert_eq!(runtime.catalog_register(table, &[b'a' + table as u8],
            TableKind::Governed, Coordinates::Warehouse, 0), Status::Ok);
    }
    runtime.warehouse_init(1, 2).unwrap();
    for owner in [0, 1] {
        // SAFETY: each test keeps indexes alive beyond its runtime and joins readers.
        let opener = unsafe { Opener::new(indexes as *const Indexes as *mut c_void, open_index) };
        runtime.warehouse_opener(owner, opener).unwrap();
    }
    *runtime.members.write() = vec![0, 1];
    let mut cache = RoutingCache::new(vec![0, 1]).ok().unwrap();
    assert_eq!(cache.install(Snapshot { version: 0,
        tables: runtime.initial_tables(2).unwrap() }), Status::Ok);
    *runtime.cache.write() = Some(cache);
    runtime
}

fn snapshot(version: u64, owner: u32) -> Snapshot {
    Snapshot { version, tables: [1, 2].into_iter().map(|table| RouteTable {
        table, boundaries: vec![Boundary { start: vec![],
            grant: Grant { owner, ownership_version: version } }],
    }).collect() }
}

#[test]
fn racing_warehouse_misses_share_one_index_without_aliasing_participants_or_proxies() {
    let indexes = Indexes::default();
    let runtime = runtime_with_indexes(&indexes);
    let barrier = Barrier::new(16);
    for (warehouse, participant, proxy) in [(1, 0, false), (2, 0, true), (2, 1, false)] {
        let handles = thread::scope(|scope| {
            let threads: Vec<_> = (0..16).map(|_| scope.spawn(|| {
                barrier.wait();
                runtime.warehouse_resolve(1, warehouse, participant).unwrap().0
            })).collect();
            threads.into_iter().map(|t| t.join().unwrap()).collect::<Vec<_>>()
        });
        assert!(handles.iter().all(|h| *h == handles[0]));
        let entries = indexes.entries.lock();
        let matching: Vec<_> = entries.iter().filter(|index| index.table == 1
            && index.warehouse == warehouse && index.owner == participant
            && index.proxy == proxy).collect();
        assert_eq!(matching.len(), 1, "a racing miss opened duplicate physical indexes");
        assert_eq!((&***matching.first().unwrap() as *const HandleKey) as usize, handles[0]);
    }
    let entries = indexes.entries.lock();
    assert_eq!(entries.len(), 3);
    let handles: HashSet<_> = entries.iter().map(|index| (&**index as *const HandleKey) as usize).collect();
    assert_eq!(handles.len(), 3, "local participants and proxies must not alias");
}

#[test]
fn cached_warehouse_handle_never_caches_its_ownership_grant() {
    let indexes = Indexes::default();
    let runtime = runtime_with_indexes(&indexes);
    let initial = runtime.warehouse_resolve(1, 1, 0).unwrap();
    let remote = snapshot(1, 1);
    runtime.install(&routing_codec::encode(&remote).unwrap()).unwrap();
    let moved = runtime.warehouse_resolve(1, 1, 0).unwrap();
    assert_ne!(initial.0, moved.0);
    assert!(moved.1 == Grant { owner: 1, ownership_version: 1 });
    for version in [2, 3] {
        runtime.install(&routing_codec::encode(&snapshot(version, 0)).unwrap()).unwrap();
        let returned = runtime.warehouse_resolve(1, 1, 0).unwrap();
        assert_eq!(returned.0, initial.0);
        assert!(returned.1 == Grant { owner: 0, ownership_version: version });
    }
    assert_eq!(indexes.entries.lock().len(), 2);
}

#[test]
fn captured_warehouse_selection_survives_departure_and_return_without_recapture() {
    let indexes = Indexes::default();
    let runtime = runtime_with_indexes(&indexes);
    let original = runtime.route(1, &warehouse_coordinate(1)).unwrap();
    let local = runtime.warehouse_select(1, 1, 0, original.grant).unwrap();
    runtime.install(&routing_codec::encode(&snapshot(1, 1)).unwrap()).unwrap();
    let departed = runtime.route(1, &warehouse_coordinate(1)).unwrap();
    let proxy = runtime.warehouse_select(1, 1, 0, departed.grant).unwrap();
    assert_ne!(local.0, proxy.0);
    runtime.install(&routing_codec::encode(&snapshot(2, 0)).unwrap()).unwrap();
    for (captured, expected) in [(original, local), (departed, proxy)] {
        let selected = runtime.warehouse_select(1, 1, 0, captured.grant).unwrap();
        assert_eq!(selected.0, expected.0);
        assert!(selected.1 == captured.grant, "selection replaced a captured incarnation");
    }
    let current = runtime.warehouse_resolve(1, 1, 0).unwrap();
    assert_eq!(current.0, local.0);
    assert!(current.1 == Grant { owner: 0, ownership_version: 2 });
    assert_eq!(indexes.entries.lock().len(), 2);
}

#[test]
fn captured_warehouse_selection_does_not_require_a_routing_snapshot() {
    let indexes = Indexes::default();
    let runtime = runtime_with_indexes(&indexes);
    let grant = Grant { owner: 1, ownership_version: 17 };
    *runtime.cache.write() = None;
    // Both miss and hit are selection only, not recapture or lease authorization.
    let selected = runtime.warehouse_select(1, 1, 0, grant).unwrap();
    let cached = runtime.warehouse_select(1, 1, 0, grant).unwrap();
    assert_eq!(cached.0, selected.0);
    assert!(selected.1 == grant && cached.1 == grant);
    let entries = indexes.entries.lock();
    assert_eq!(entries.len(), 1);
    assert_eq!(entries[0].owner, 0);
    assert!(entries[0].proxy);
}

#[test]
fn concurrent_publication_keeps_retained_snapshots_whole_and_routes_current() {
    let indexes = Indexes::default();
    let runtime = runtime_with_indexes(&indexes);
    let barrier = Barrier::new(9);
    thread::scope(|scope| {
        let readers: Vec<_> = (0..8).map(|_| scope.spawn(|| {
            for version in 1..=64 {
                barrier.wait();
                let retained = runtime.snapshot().unwrap();
                barrier.wait();
                assert!(retained.version == version - 1 || retained.version == version);
                for table in [1, 2] {
                    let route = retained.capture(table, &warehouse_coordinate(1)).unwrap();
                    assert_eq!(route.snapshot_version, retained.version);
                    assert_eq!(route.grant.ownership_version, retained.version);
                    assert_eq!(route.grant.owner, (retained.version % 2) as u32);
                }
                let current = runtime.route(1, &warehouse_coordinate(1)).unwrap();
                assert_eq!(current.snapshot_version, version);
                assert!(current.grant == Grant { owner: (version % 2) as u32,
                    ownership_version: version });
            }
        })).collect();
        for version in 1..=64 {
            barrier.wait();
            runtime.install(&routing_codec::encode(&snapshot(version, (version % 2) as u32)).unwrap()).unwrap();
            barrier.wait();
        }
        for reader in readers { reader.join().unwrap(); }
    });
}
