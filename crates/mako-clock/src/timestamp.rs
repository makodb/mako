//! Mako's hybrid logical commit timestamp.

use std::fmt;
use std::num::NonZeroU32;

/// Mako's 16-byte hybrid logical transaction timestamp.
///
/// Timestamp order is the unsigned tuple `(physical_us, logical, origin)`.
/// It is distinct from the cache commit sequence, consensus position, and any
/// engine-internal record-version clock. The local allocator currently emits
/// millisecond-aligned physical values, while the durable representation keeps
/// microsecond precision for a future clock source.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct MakoTimestamp {
    physical_us: u64,
    logical: u32,
    origin: NonZeroU32,
}

impl MakoTimestamp {
    /// Number of logical-counter bits in the private 63-bit local stamp.
    pub const HOT_LOGICAL_BITS: u32 = 19;
    /// Mask selecting the logical counter of a private local stamp.
    pub const HOT_LOGICAL_MASK: u64 = (1u64 << Self::HOT_LOGICAL_BITS) - 1;
    /// Largest physical millisecond value a private local stamp can carry.
    pub const HOT_PHYSICAL_MS_MAX: u64 = (1u64 << 44) - 1;
    /// Largest private local stamp value; bit 63 is never part of a stamp.
    pub const HOT_STAMP_MAX: u64 = u64::MAX >> 1;

    /// Construct a timestamp. A live timestamp always has a nonzero allocator
    /// origin; physical time and the logical counter may independently be zero.
    pub const fn new(physical_us: u64, logical: u32, origin: u32) -> Option<Self> {
        match NonZeroU32::new(origin) {
            Some(origin) => Some(Self {
                physical_us,
                logical,
                origin,
            }),
            None => None,
        }
    }

    /// Approximate Unix commit time in microseconds.
    pub const fn physical_us(self) -> u64 {
        self.physical_us
    }

    /// Logical order within the physical clock reading.
    pub const fn logical(self) -> u32 {
        self.logical
    }

    /// Nonzero identity of the allocator which minted this timestamp.
    pub const fn origin(self) -> u32 {
        self.origin.get()
    }

    /// Nonzero allocator origin as its checked type.
    pub const fn origin_nonzero(self) -> NonZeroU32 {
        self.origin
    }

    /// Encode the canonical 16-byte wire and disk representation.
    pub fn to_be_bytes(self) -> [u8; 16] {
        let mut encoded = [0u8; 16];
        encoded[..8].copy_from_slice(&self.physical_us.to_be_bytes());
        encoded[8..12].copy_from_slice(&self.logical.to_be_bytes());
        encoded[12..].copy_from_slice(&self.origin.get().to_be_bytes());
        encoded
    }

    /// Decode the canonical 16-byte wire and disk representation.
    pub fn from_be_bytes(encoded: [u8; 16]) -> Option<Self> {
        let physical_us = u64::from_be_bytes(encoded[..8].try_into().ok()?);
        let logical = u32::from_be_bytes(encoded[8..12].try_into().ok()?);
        let origin = u32::from_be_bytes(encoded[12..].try_into().ok()?);
        Self::new(physical_us, logical, origin)
    }

    /// Reconstruct a timestamp from the private 63-bit local allocator stamp
    /// `(physical_ms << 19) | logical`.
    ///
    /// Engines share this packing so a stamp minted by one engine and a
    /// stamp minted by another decode identically.
    #[doc(hidden)]
    #[inline(always)]
    pub const fn from_hot_stamp(stamp: u64, origin: NonZeroU32) -> Option<Self> {
        if stamp == 0 || stamp > Self::HOT_STAMP_MAX {
            return None;
        }
        let physical_ms = stamp >> Self::HOT_LOGICAL_BITS;
        let logical = (stamp & Self::HOT_LOGICAL_MASK) as u32;
        let Some(physical_us) = physical_ms.checked_mul(1_000) else {
            return None;
        };
        Some(Self {
            physical_us,
            logical,
            origin,
        })
    }

    /// Return the private local allocator stamp when this timestamp is exactly
    /// representable by the millisecond clock.
    #[doc(hidden)]
    #[inline]
    pub const fn hot_stamp(self) -> Option<u64> {
        if !self.physical_us.is_multiple_of(1_000) || self.logical as u64 > Self::HOT_LOGICAL_MASK {
            return None;
        }
        let physical_ms = self.physical_us / 1_000;
        if physical_ms > Self::HOT_PHYSICAL_MS_MAX {
            return None;
        }
        let stamp = (physical_ms << Self::HOT_LOGICAL_BITS) | self.logical as u64;
        if stamp == 0 {
            None
        } else {
            Some(stamp)
        }
    }

    /// Reconstruct a public timestamp from the private local allocator stamp.
    #[doc(hidden)]
    pub const fn from_local_stamp(stamp: u64, origin: u32) -> Option<Self> {
        let Some(origin) = NonZeroU32::new(origin) else {
            return None;
        };
        Self::from_hot_stamp(stamp, origin)
    }

    /// Return the private local allocator stamp when this timestamp is exactly
    /// representable by the current millisecond clock.
    #[doc(hidden)]
    pub const fn local_stamp(self) -> Option<u64> {
        self.hot_stamp()
    }
}

impl fmt::Display for MakoTimestamp {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}:{}", self.physical_us, self.logical, self.origin)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hot_stamp_round_trips_millisecond_aligned_timestamps() {
        let origin = NonZeroU32::new(1).unwrap();
        let stamp = (1_700_000_000_000u64 << MakoTimestamp::HOT_LOGICAL_BITS) | 7;
        let timestamp = MakoTimestamp::from_hot_stamp(stamp, origin).unwrap();
        assert_eq!(timestamp.physical_us(), 1_700_000_000_000 * 1_000);
        assert_eq!(timestamp.logical(), 7);
        assert_eq!(timestamp.hot_stamp(), Some(stamp));
        assert_eq!(MakoTimestamp::from_hot_stamp(0, origin), None);
        assert_eq!(MakoTimestamp::from_hot_stamp(u64::MAX, origin), None);
    }

    #[test]
    fn be_bytes_round_trip_and_reject_zero_origin() {
        let timestamp = MakoTimestamp::new(123_456, 9, 3).unwrap();
        assert_eq!(
            MakoTimestamp::from_be_bytes(timestamp.to_be_bytes()),
            Some(timestamp)
        );
        assert_eq!(MakoTimestamp::new(1, 1, 0), None);
    }
}
