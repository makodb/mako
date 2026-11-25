use crate::TransactionId;

/// Familiar RocksDB error categories, with an explicit distributed uncertainty.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ErrorKind {
    Busy,
    TryAgain,
    InvalidArgument,
    NotSupported,
    IOError,
    Aborted,
    Incomplete,
    UnknownCommit,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Error {
    kind: ErrorKind,
    message: String,
    transaction: Option<TransactionId>,
}
impl Error {
    pub fn new(kind: ErrorKind, message: impl Into<String>) -> Self {
        Self {
            kind,
            message: message.into(),
            transaction: None,
        }
    }
    pub fn kind(&self) -> ErrorKind {
        self.kind
    }
    /// Save this identity before retrying resolution. Never replay an unknown body.
    pub fn transaction_id(&self) -> Option<TransactionId> {
        self.transaction
    }
    pub fn with_transaction(mut self, id: TransactionId) -> Self {
        self.transaction = Some(id);
        self
    }
    pub(crate) fn invalid(message: impl Into<String>) -> Self {
        Self::new(ErrorKind::InvalidArgument, message)
    }
    pub(crate) fn unsupported(message: impl Into<String>) -> Self {
        Self::new(ErrorKind::NotSupported, message)
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}: {}", self.kind, self.message)?;
        if let Some(id) = self.transaction {
            write!(f, " [{id:?}]")?;
        }
        Ok(())
    }
}
impl std::error::Error for Error {}
pub type Result<T> = std::result::Result<T, Error>;
