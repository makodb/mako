//! The masstree-backed [`KeyIndex`], against a real tree.
//!
//! Skipped entirely when no mako build is present — see `build.rs`. The
//! gate is `cfg(have_mako)` rather than a runtime check so that a missing
//! build shows up as "0 tests run", not as a green suite that tested
//! nothing.

#![cfg(have_mako)]

use std::collections::BTreeSet;
use std::sync::{Arc, Barrier};

use mrx_core::KeyIndex;
use mrx_masstree::MasstreeIndex;

#[test]
fn words_round_trip_bit_identical() {
    // The tripwire for the whole design: this crate stores an entry
    // pointer in a tree that thinks it is storing its own value type. If
    // masstree ever interprets, tags, or normalises the word, everything
    // above it is reading corrupted handles.
    let t = MasstreeIndex::new().expect("create");
    let words: [u64; 13] = [
        1,
        2,
        0xFF,
        0x100,
        0xDEAD_BEEF,
        0x8000_0000_0000_0000,
        0x7FFF_FFFF_FFFF_FFFF,
        u64::MAX,
        u64::MAX - 1,
        0x5555_5555_5555_5555,
        0xAAAA_AAAA_AAAA_AAAA,
        0x0000_0000_FFFF_FFFF,
        0xFFFF_FFFF_0000_0000,
    ];
    for (i, w) in words.iter().enumerate() {
        let k = format!("w{i:02}");
        assert_eq!(t.get_or_insert(k.as_bytes(), *w), *w);
    }
    // Point path.
    for (i, w) in words.iter().enumerate() {
        let k = format!("w{i:02}");
        assert_eq!(
            t.get(k.as_bytes()),
            Some(*w),
            "point read altered word {w:#x}"
        );
    }
    // Scan path, which copies through a different code path entirely.
    let mut out = Vec::new();
    t.scan_chunk(b"", 64, &mut out);
    assert_eq!(out.len(), words.len());
    for (i, (k, w)) in out.iter().enumerate() {
        assert_eq!(k, format!("w{i:02}").as_bytes());
        assert_eq!(*w, words[i], "scan altered word {:#x}", words[i]);
    }
}

#[test]
fn absent_keys_report_none() {
    let t = MasstreeIndex::new().expect("create");
    assert_eq!(t.get(b"nothing"), None);
    t.get_or_insert(b"something", 7);
    assert_eq!(t.get(b"nothing"), None);
    assert_eq!(t.get(b"something"), Some(7));
}

#[test]
fn get_or_insert_reports_the_winner() {
    let t = MasstreeIndex::new().expect("create");
    assert_eq!(t.get_or_insert(b"k", 11), 11);
    assert_eq!(t.get_or_insert(b"k", 22), 11, "the first writer wins");
    assert_eq!(t.get(b"k"), Some(11));
}

#[test]
fn concurrent_inserts_agree_on_one_word() {
    let t = Arc::new(MasstreeIndex::new().expect("create"));
    let barrier = Arc::new(Barrier::new(8));
    let mut seen = std::sync::Mutex::new(BTreeSet::new());
    std::thread::scope(|scope| {
        for id in 1..=8u64 {
            let t = Arc::clone(&t);
            let barrier = Arc::clone(&barrier);
            let seen = &seen;
            scope.spawn(move || {
                barrier.wait();
                let won = t.get_or_insert(b"contested", id);
                seen.lock().unwrap().insert(won);
            });
        }
    });
    let set = seen.get_mut().unwrap();
    assert_eq!(
        set.len(),
        1,
        "threads disagreed about the stored word: {set:?}"
    );
    assert_eq!(t.get(b"contested"), set.iter().copied().next());
}

#[test]
fn scan_chunks_cover_the_whole_range() {
    // A short chunk is a chunk boundary, not end-of-range. The adapter
    // has to keep walking, and a key long enough to fill the arena must
    // not silently truncate the walk.
    let t = MasstreeIndex::new().expect("create");
    let long = "x".repeat(400);
    for i in 0..1000 {
        t.get_or_insert(format!("{long}{i:04}").as_bytes(), i as u64 + 1);
    }

    let mut all: Vec<Vec<u8>> = Vec::new();
    let mut cursor: Vec<u8> = Vec::new();
    let mut first = true;
    loop {
        let mut chunk = Vec::new();
        let n = t.scan_chunk(&cursor, 64, &mut chunk);
        if n == 0 {
            break;
        }
        let last = chunk[n - 1].0.clone();
        for (k, _) in chunk.into_iter().skip(usize::from(!first)) {
            all.push(k);
        }
        first = false;
        if n < 64 || last == cursor {
            break;
        }
        cursor = last;
    }
    assert_eq!(all.len(), 1000, "a chunk boundary dropped keys");
    let unique: BTreeSet<_> = all.iter().collect();
    assert_eq!(unique.len(), 1000, "a chunk boundary duplicated keys");
    assert!(all.windows(2).all(|w| w[0] < w[1]), "must be ordered");
}

#[test]
fn rscan_walks_backwards() {
    let t = MasstreeIndex::new().expect("create");
    for i in 0..20 {
        t.get_or_insert(format!("k{i:02}").as_bytes(), i as u64 + 1);
    }
    let mut out = Vec::new();
    t.rscan_chunk(b"k10", 5, &mut out);
    assert_eq!(out[0].0, b"k10".to_vec(), "the bound is inclusive");
    assert!(
        out.windows(2).all(|w| w[0].0 > w[1].0),
        "must be descending"
    );
}

#[test]
fn len_counts_inserted_keys() {
    let t = MasstreeIndex::new().expect("create");
    assert_eq!(t.len(), 0);
    for i in 0..100 {
        t.get_or_insert(format!("k{i:03}").as_bytes(), i as u64 + 1);
    }
    assert_eq!(t.len(), 100);
}

#[test]
fn empty_and_binary_keys_are_ordinary() {
    let t = MasstreeIndex::new().expect("create");
    t.get_or_insert(b"", 1);
    t.get_or_insert(&[0u8, 1, 2, 0xFF], 2);
    t.get_or_insert(&[0xFFu8; 300], 3);
    assert_eq!(t.get(b""), Some(1));
    assert_eq!(t.get(&[0u8, 1, 2, 0xFF]), Some(2));
    assert_eq!(t.get(&[0xFFu8; 300]), Some(3));
    assert_eq!(t.len(), 3);
}
