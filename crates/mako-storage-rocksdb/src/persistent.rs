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
