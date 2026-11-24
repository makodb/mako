//! Adapt the existing checked scan cursor/page implementation to native SRPC.
//! Page bounds, ordering, continuation and completion remain in full_scan_core.
use crate::{
    client::Target,
    storage::{engine, shard_error, Protection, Store},
    wire::*,
};
use mako_sharding::{
    full_scan_core::{self as core, BoundRequest, FullScan, Page, ScanIdentity},
    types::{Grant, TxnId, NO_SNAPSHOT},
};
use sto_tpcc_ffi::native as sto;

/// Normalize bounds only. Continuation cursors always retain the exact last
/// binary key and exclude it; appending a byte to a cursor would skip keys.
pub fn bounds(lower: Bound, upper: Bound) -> (Vec<u8>, Option<Vec<u8>>) {
    let lower = match lower {
        Bound::Unbounded => Vec::new(),
        Bound::Included(key) => key,
        Bound::Excluded(mut key) => {
            key.push(0);
            key
        }
    };
    let upper = match upper {
        Bound::Unbounded => None,
        Bound::Excluded(key) => Some(key),
        Bound::Included(mut key) => {
            key.push(0);
            Some(key)
        }
    };
    (lower, upper)
}
fn identity(target: &Target, identity: Identity) -> ScanIdentity {
    ScanIdentity {
        transaction: TxnId {
            client: identity.incarnation,
            sequence: identity.attempt,
        },
        grant: Grant {
            owner: target.owner,
            ownership_version: target
                .access
                .route
                .map_or(0, |route| route.ownership_version),
        },
        snapshot_version: target
            .access
            .route
            .map_or(NO_SNAPSHOT, |route| route.snapshot_version),
        table: target.access.table.table,
        fixed_coordinate: target.access.table.warehouse != 0,
        coordinate: if target.access.table.warehouse != 0 {
            target.access.table.warehouse.to_be_bytes().to_vec()
        } else {
            Vec::new()
        },
    }
}
pub struct Driver(FullScan);
impl Driver {
    pub fn new(
        target: &Target,
        transaction: Identity,
        lower: Vec<u8>,
        upper: Option<Vec<u8>>,
        reverse: bool,
    ) -> Result<Self> {
        let request = BoundRequest {
            identity: identity(target, transaction),
            bounds: core::Request {
                lo: lower,
                hi: upper,
                cursor: None,
                reverse,
            },
        };
        if !request.bounds.valid() {
            return Err(Error::new(ErrorCode::Invalid, "invalid scan range"));
        }
        Ok(Self(FullScan::new(request)))
    }
    pub fn request(&self) -> Result<Vec<u8>> {
        let mut bytes = vec![0; core::REQUEST_CAPACITY];
        let length = self.0.request.encode(&mut bytes).map_err(shard_error)?;
        bytes.truncate(length);
        Ok(bytes)
    }
    pub fn consume(&mut self, bytes: &[u8], output: &mut Vec<Row>, limit: usize) -> Result<bool> {
        let mut page = self.0.begin_page(bytes).map_err(shard_error)?;
        while let Some((key, value)) = page.next().map_err(shard_error)? {
            let timestamp: [u8; 4] = value
                .get(..4)
                .ok_or_else(|| Error::new(ErrorCode::Transport, "scan row has no version"))?
                .try_into()
                .expect("four-byte span");
            output.push(Row {
                key: key.to_vec(),
                value: value[4..].to_vec(),
                time_term: u32::from_le_bytes(timestamp),
            });
            if output.len() == limit {
                page.stop();
                break;
            }
        }
        self.0.finish_page(&page).map_err(shard_error)
    }
}

pub fn produce(
    store: &Store,
    protection: &Protection,
    participant: &mut sto::Participant<'_>,
    transaction: Identity,
    access: &Access,
    bytes: &[u8],
) -> Result<Value> {
    let request = BoundRequest::decode(bytes).map_err(shard_error)?;
    let expected = identity(
        &Target {
            owner: store.owner,
            access: access.clone(),
            log_id: 0,
        },
        transaction,
    );
    if !request.identity.matches(
        expected.transaction,
        expected.grant,
        expected.snapshot_version,
        expected.table,
        expected.fixed_coordinate,
        &expected.coordinate,
    ) {
        return Err(Error::new(
            ErrorCode::Invalid,
            "scan page identity differs from participant request",
        ));
    }
    // Authorize the complete captured range on every page, not merely its
    // current cursor. A stale or cross-owner range cannot become a partial scan.
    let range_lower = Bound::Included(request.bounds.lo.clone());
    let range_upper = request
        .bounds
        .hi
        .clone()
        .map_or(Bound::Unbounded, Bound::Excluded);
    protection.range(access, &range_lower, &range_upper)?;
    let lower = if !request.bounds.reverse {
        request
            .bounds
            .cursor
            .clone()
            .map_or(range_lower, Bound::Excluded)
    } else {
        range_lower
    };
    let upper = if request.bounds.reverse {
        request
            .bounds
            .cursor
            .clone()
            .map_or(range_upper, Bound::Excluded)
    } else {
        range_upper
    };
    let table = store.table(access.table)?;
    let rows = participant
        .scan(
            &table.tree,
            lower.as_ref(),
            upper.as_ref(),
            request.bounds.reverse,
            core::PAGE_ROWS + 1,
        )
        .map_err(engine)?;
    let mut page = Page::new(request).map_err(shard_error)?;
    for row in rows.rows {
        let mut value = Vec::with_capacity(row.value.len() + 4);
        value.extend_from_slice(&row.time_term.to_le_bytes());
        value.extend_from_slice(&row.value);
        if !page.add(&row.key, &value).map_err(shard_error)? {
            break;
        }
    }
    let mut bytes = vec![0; core::PAGE_CAPACITY];
    let length = page.finish(&mut bytes).map_err(shard_error)?;
    bytes.truncate(length);
    Ok(Value::Page {
        bytes,
        read_dependency: rows.read_dependency,
    })
}
