//! Capability negotiation.

/// Capability bits reported by an engine.
///
/// Bit positions are part of the engine contract; a native facade must assert
/// that its linked ABI uses the same positions.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Features(u64);

impl Features {
    /// Local point reads and writes inside atomic transactions are available.
    pub const POINT_TRANSACTIONS: u64 = 1 << 0;
    /// Reads in a transaction observe its earlier writes.
    pub const READ_MY_WRITES: u64 = 1 << 1;
    /// The engine was built with opacity checks enabled.
    pub const OPACITY: u64 = 1 << 2;
    /// Transactional forward and reverse scans are available.
    pub const TRANSACTIONAL_SCANS: u64 = 1 << 3;
    /// Transactional scans observe earlier writes from the same transaction.
    pub const SCAN_READ_MY_WRITES: u64 = 1 << 4;
    /// Test-only synchronous commit crash-seam observation is compiled in.
    pub const TEST_COMMIT_OBSERVER: u64 = 1 << 5;
    /// Deterministic cleanup-failure injection is compiled in for tests.
    pub const TEST_CLEANUP_FAILURES: u64 = 1 << 6;
    /// The process-global physical-clock override is compiled in for tests.
    pub const TEST_TIMESTAMP_CLOCK: u64 = 1 << 7;

    /// Wrap raw capability bits reported by an engine.
    pub const fn from_bits(bits: u64) -> Self {
        Self(bits)
    }

    /// Local point reads and writes inside atomic transactions are available.
    pub const fn point_transactions(self) -> bool {
        self.0 & Self::POINT_TRANSACTIONS != 0
    }

    /// Reads in a transaction are guaranteed to observe its earlier writes.
    pub const fn read_my_writes(self) -> bool {
        self.0 & Self::READ_MY_WRITES != 0
    }

    /// The engine was built with opacity checks enabled.
    pub const fn opacity(self) -> bool {
        self.0 & Self::OPACITY != 0
    }

    /// Transactional forward and reverse chunk scans are available.
    pub const fn transactional_scans(self) -> bool {
        self.0 & Self::TRANSACTIONAL_SCANS != 0
    }

    /// Transactional scans observe earlier writes from the same transaction.
    pub const fn scan_read_my_writes(self) -> bool {
        self.0 & Self::SCAN_READ_MY_WRITES != 0
    }

    /// Test-only synchronous local commit crash-seam observation is compiled in.
    pub const fn test_commit_observer(self) -> bool {
        self.0 & Self::TEST_COMMIT_OBSERVER != 0
    }

    /// Deterministic cleanup-failure injection is compiled in for tests.
    #[doc(hidden)]
    pub const fn test_cleanup_failures(self) -> bool {
        self.0 & Self::TEST_CLEANUP_FAILURES != 0
    }

    /// The process-global physical-clock override is compiled in for tests.
    #[doc(hidden)]
    pub const fn test_timestamp_clock(self) -> bool {
        self.0 & Self::TEST_TIMESTAMP_CLOCK != 0
    }

    /// Raw feature bits, including future bits unknown to this crate.
    pub const fn bits(self) -> u64 {
        self.0
    }
}
