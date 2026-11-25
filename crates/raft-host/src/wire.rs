use crate::Position;
use bincode::Options;
use raft_core::{InboundBatch, RaftEntry};
use serde::{Deserialize, Serialize};
use std::sync::Arc;

pub(crate) const RPC: i32 = 0x4d52_4801;
pub(crate) const FRAME_LIMIT: u64 = 16 << 20;

// Keep the existing durable protocol version. Async peers use a distinct
// version so even an older sync host rejects their votes/appends/snapshots.
pub(crate) fn version(sync_writes: bool) -> u32 {
    if sync_writes {
        1
    } else {
        2
    }
}
#[derive(Clone, Debug, Serialize, Deserialize)]
pub(crate) struct Command(pub Option<Arc<[u8]>>);
impl Command {
    pub fn entry(&self, term: u64) -> RaftEntry<Self> {
        // `has_value` describes an existing log entry, including internal no-ops.
        RaftEntry::new(
            term as i64,
            self.clone(),
            true,
            false,
            0,
            self.0.as_ref().map_or(0, |b| b.len() as u64),
        )
    }
}
#[derive(Clone, Serialize, Deserialize)]
pub(crate) struct Batch(pub Vec<(u64, Command)>);
impl InboundBatch<Command> for Batch {
    fn decode_terms(&self, _: u64, terms: &mut Vec<i64>) -> bool {
        terms.clear();
        for (term, _) in &self.0 {
            if *term >= raft_core::RAFT_INDEX_LIMIT {
                return false;
            }
            terms.push(*term as i64);
        }
        true
    }
    fn entry_at(&self, k: u64) -> RaftEntry<Command> {
        self.0[k as usize].1.entry(self.0[k as usize].0)
    }
}
#[derive(Serialize, Deserialize)]
pub(crate) struct Envelope {
    pub version: u32,
    pub group: u64,
    pub sender: u16,
    pub request: Request,
}
#[derive(Serialize, Deserialize)]
pub(crate) enum Request {
    Vote {
        term: u64,
        last: Position,
    },
    Append {
        term: u64,
        previous: Position,
        commit: u64,
        batch: Batch,
    },
    SnapshotBegin {
        term: u64,
        position: Position,
        digest: [u8; 32],
    },
    SnapshotChunk {
        term: u64,
        position: Position,
        file: String,
        offset: u64,
        // Bulk byte-buffer encoding avoids visiting every payload byte in
        // bincode's sizing, writing and reading passes. The wire bytes match
        // the original Vec<u8> encoding exactly.
        #[serde(with = "serde_bytes")]
        bytes: Vec<u8>,
        last: bool,
    },
    SnapshotFinish {
        term: u64,
        position: Position,
    },
    SnapshotContact {
        term: u64,
        position: Position,
    },
}
#[derive(Serialize, Deserialize, Debug)]
pub(crate) enum Response {
    Vote {
        term: u64,
        granted: bool,
    },
    Append {
        term: u64,
        accepted: bool,
        last: u64,
    },
    Snapshot {
        term: u64,
        accepted: bool,
    },
    SnapshotCurrent {
        term: u64,
    },
    Error(String),
}
pub(crate) fn encode<T: Serialize>(value: &T) -> Result<Vec<u8>, String> {
    bincode::DefaultOptions::new()
        .with_fixint_encoding()
        .with_limit(FRAME_LIMIT)
        .serialize(value)
        .map_err(|e| e.to_string())
}
pub(crate) fn decode<T: serde::de::DeserializeOwned>(bytes: &[u8]) -> Result<T, String> {
    bincode::DefaultOptions::new()
        .with_fixint_encoding()
        .with_limit(FRAME_LIMIT)
        .reject_trailing_bytes()
        .deserialize(bytes)
        .map_err(|e| e.to_string())
}
pub(crate) struct CommandCodec;
impl raft_store::Codec<Command> for CommandCodec {
    fn encode(&self, command: &Command, out: &mut Vec<u8>) {
        out.extend_from_slice(&raft_store::journal::encode_command(command.0.as_deref()));
    }
    fn decode(&self, bytes: &[u8]) -> Result<Command, String> {
        Ok(Command(
            raft_store::journal::decode_command(bytes)?.map(Arc::from),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn positions_above_u32_survive_wire_and_core_restore() {
        let position = Position {
            index: (1u64 << 45) + 11,
            term: (1u64 << 40) + 7,
        };
        let request = Envelope {
            version: 1,
            group: (1u64 << 55) + 9,
            sender: 17,
            request: Request::Append {
                term: position.term,
                previous: position,
                commit: position.index,
                batch: Batch(Vec::new()),
            },
        };
        let decoded: Envelope = decode(&encode(&request).unwrap()).unwrap();
        assert_eq!(decoded.group, (1u64 << 55) + 9);
        match decoded.request {
            Request::Append {
                term,
                previous,
                commit,
                ..
            } => {
                assert_eq!(term, position.term);
                assert_eq!(previous, position);
                assert_eq!(commit, position.index);
            }
            _ => panic!("wrong wire variant"),
        }
        let mut core = raft_core::RaftCore::<Command>::new();
        core.step::<Batch>(
            raft_core::Event::SetIdentity {
                loc_id: 0,
                site_id: 17,
                partition_id: 0,
            },
            &mut raft_core::CoreOutput::new(),
        );
        core.step::<Batch>(
            raft_core::Event::Configure { members: &[17] },
            &mut raft_core::CoreOutput::new(),
        );
        assert!(core
            .step::<Batch>(
                raft_core::Event::Restore {
                    term: position.term,
                    vote: u16::MAX,
                    commit: position.index,
                    d: 1,
                    snap_index: position.index,
                    snap_term: position.term as i64,
                    entries_rev: Vec::new()
                },
                &mut raft_core::CoreOutput::new()
            )
            .into_restored());
        assert_eq!(core.raft_log_.last_index(), position.index);
        assert_eq!(core.current_term_, position.term);
    }
    #[test]
    fn snapshot_bulk_bytes_preserve_wire_encoding_and_chunk_contents() {
        let position = Position {
            index: 1u64 << 42,
            term: 19,
        };
        let bytes = (0..256 * 1024).map(|i| (i % 251) as u8).collect::<Vec<_>>();
        let fields =
            encode(&(23u64, position, "checkpoint/data", 1u64 << 35, &bytes, true)).unwrap();
        let request = Request::SnapshotChunk {
            term: 23,
            position,
            file: "checkpoint/data".into(),
            offset: 1u64 << 35,
            bytes: bytes.clone(),
            last: true,
        };
        let encoded = encode(&request).unwrap();
        // Fixed-width enum tag precedes the fields; bulk bytes retain the
        // original sequence length prefix followed by the identical payload.
        assert_eq!(&encoded[..4], &3u32.to_le_bytes());
        assert_eq!(&encoded[4..], fields);
        match decode::<Request>(&encoded).unwrap() {
            Request::SnapshotChunk {
                term,
                position: decoded,
                file,
                offset,
                bytes: received,
                last,
            } => {
                assert_eq!(term, 23);
                assert_eq!(decoded, position);
                assert_eq!(file, "checkpoint/data");
                assert_eq!(offset, 1u64 << 35);
                assert_eq!(received, bytes);
                assert!(last);
            }
            _ => panic!("wrong wire variant"),
        }
    }
    #[test]
    fn noops_and_empty_commands_have_distinct_durable_encodings() {
        use raft_store::Codec;
        let mut noop = Vec::new();
        let mut user = Vec::new();
        CommandCodec.encode(&Command(None), &mut noop);
        CommandCodec.encode(&Command(Some(Arc::from([]))), &mut user);
        assert_ne!(noop, user);
        assert!(CommandCodec.decode(&noop).unwrap().0.is_none());
        assert_eq!(CommandCodec.decode(&user).unwrap().0.unwrap().len(), 0);
        assert!(CommandCodec.decode(&[0, 1]).is_err());
    }
}
