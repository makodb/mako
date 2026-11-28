use super::*;
pub(super) struct Application(pub Arc<sto::Database>);
fn position(p: api::LogPosition) -> sto::Position {
    sto::Position {
        index: p.index,
        term: p.term,
    }
}
fn external(p: sto::Position) -> api::LogPosition {
    api::LogPosition {
        index: p.index,
        term: p.term,
    }
}
impl api::PersistentApplication for Application {
    fn initial_state_id(&self) -> api::Result<[u8;32]> { self.0.initial_state_id().map_err(error) }
    fn complete_bootstrap(&self) -> api::Result<()> { self.0.complete_bootstrap().map_err(error) }
    fn durable(&self) -> bool {
        self.0.distributed_durable()
    }
    fn bind_identity(&self, identity: &[u8]) -> api::Result<()> {
        self.0.bind_distributed_identity(identity).map_err(error)
    }
    fn position(&self) -> api::Result<api::LogPosition> {
        self.0.applied().map(external).map_err(error)
    }
    fn table_ids(&self) -> api::Result<Vec<u64>> {
        self.0
            .list_tables()
            .map(|tables| tables.into_iter().map(|t| t.id).collect())
            .map_err(error)
    }
    fn ensure_tables(&self, ids: &[u16]) -> api::Result<()> {
        for id in ids {
            self.0.distributed_table(u64::from(*id)).map_err(error)?;
        }
        Ok(())
    }
    fn page(
        &self,
        table: u16,
        after: Option<&[u8]>,
        limit: usize,
    ) -> api::Result<api::CheckpointPage> {
        let (rows, end) = self
            .0
            .distributed_page(table.into(), after, limit)
            .map_err(error)?;
        Ok(api::CheckpointPage {
            rows: rows
                .into_iter()
                .map(|r| api::CheckpointRow {
                    key: r.key,
                    value: r.value,
                    time_term: r.time_term,
                })
                .collect(),
            end,
        })
    }
    fn bootstrap(&self, rows: &[api::RedoRow]) -> api::Result<()> {
        if self.position()?.index != 0 {
            return Err(api::Error::invalid("bootstrap after log application"));
        }
        self.ensure_tables(&rows.iter().map(|r| r.table).collect::<Vec<_>>())?;
        self.0
            .apply_distributed(
                None,
                &rows.iter().map(native_row).collect::<Vec<_>>(),
                &Vec::new(),
            )
            .map_err(error)
    }
    fn apply_batches(&self, batches: &[api::ApplicationBatch]) -> api::Result<()> {
        let prefixes = batches
            .iter()
            .map(|b| sto::distributed::Prefix {
                position: position(b.position),
                fingerprint: b.fingerprint,
            })
            .collect::<Vec<_>>();
        let rows = batches
            .iter()
            .map(|b| b.rows.iter().map(native_row).collect::<Vec<_>>())
            .collect::<Vec<_>>();
        let effects = batches
            .iter()
            .enumerate()
            .map(|(i, b)| sto::distributed::Batch {
                prefix: &prefixes[i],
                rows: &rows[i],
                metadata: &b.metadata,
            })
            .collect::<Vec<_>>();
        self.0.apply_distributed_batch(&effects).map_err(error)
    }
    fn metadata(&self) -> api::Result<api::Metadata> {
        self.0.distributed_metadata().map_err(error)
    }
    fn apply(
        &self,
        p: api::LogPosition,
        fingerprint: [u8; 32],
        rows: &[api::RedoRow],
        metadata: &api::Metadata,
    ) -> api::Result<()> {
        let rows = rows
            .iter()
            .map(|row| sto::distributed::Row {
                table: u64::from(row.table),
                key: row.key.clone(),
                value: row.value.clone(),
                time_term: row.time_term,
            })
            .collect::<Vec<_>>();
        self.0
            .apply_distributed(
                Some(&sto::distributed::Prefix {
                    position: position(p),
                    fingerprint,
                }),
                &rows,
                metadata,
            )
            .map_err(error)
    }
    fn create_checkpoint(&self, path: &Path, p: api::LogPosition) -> api::Result<()> {
        self.0.create_checkpoint(path, position(p)).map_err(error)
    }
    fn checkpoint_metadata(
        &self,
        path: &Path,
        p: api::LogPosition,
        identity: &[u8],
    ) -> api::Result<(api::LogPosition, api::Metadata)> {
        self.0
            .distributed_checkpoint_state(path, position(p), identity)
            .map(|(p, m)| (external(p), m))
            .map_err(error)
    }
    fn publish_checkpoint(&self, path: &Path, p: api::LogPosition) -> api::Result<()> {
        self.0.publish_checkpoint(path, position(p)).map_err(error)
    }
}

fn native_row(row: &api::RedoRow) -> sto::distributed::Row {
    sto::distributed::Row {
        table: row.table.into(),
        key: row.key.clone(),
        value: row.value.clone(),
        time_term: row.time_term,
    }
}
