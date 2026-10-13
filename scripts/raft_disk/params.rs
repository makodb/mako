// The cost model's primitives (scripts/raft_disk/model.py; plan §5), each
// measured in isolation -- no Raft runs here, so the model's inputs cannot
// be fitted to the experiments it predicts. std only, as raft-store.
//
//   write + fdatasync, one writer and three at once (three replicas share
//   the host's device), at 4 KiB .. 16 MiB; memcpy; CRC32C (table, SSE4.2);
//   syncs taken in turns (a round's order); write bandwidth against volume
//   (a write cache, then the media); the overshoot of a thread sleep (the injected delay); the cross-thread
//   hand-off by condvar (a record pushed -> the flusher thread running).
//
// Build: rustc -O --edition 2021 -o params scripts/raft_disk/params.rs
// Run:   d=$(mktemp -d /var/tmp/raft-wal-$USER-bench.XXXX); ./params $d; rm -rf $d
// The last line is `PARAMS <json>` for model.py --params.
use std::fs::OpenOptions;
use std::io::Write;
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

fn pct(v: &mut Vec<f64>, p: f64) -> f64 {
    v.sort_by(|a, b| a.partial_cmp(b).unwrap());
    v[((v.len() as f64 - 1.0) * p).round() as usize]
}

fn table() -> [[u32; 256]; 8] {
    let mut t = [[0u32; 256]; 8];
    for i in 0..256u32 {
        let mut c = i;
        for _ in 0..8 { c = if c & 1 != 0 { (c >> 1) ^ 0x82F6_3B78 } else { c >> 1 }; }
        t[0][i as usize] = c;
    }
    for i in 0..256 { for k in 1..8 { t[k][i] = (t[k - 1][i] >> 8) ^ t[0][(t[k - 1][i] & 0xff) as usize]; } }
    t
}

fn crc_sw(t: &[[u32; 256]; 8], data: &[u8]) -> u32 {
    let mut c = !0u32;
    let mut chunks = data.chunks_exact(8);
    for ch in &mut chunks {
        let v = u64::from_le_bytes(ch.try_into().unwrap()) ^ c as u64;
        c = t[7][(v & 0xff) as usize] ^ t[6][((v >> 8) & 0xff) as usize]
          ^ t[5][((v >> 16) & 0xff) as usize] ^ t[4][((v >> 24) & 0xff) as usize]
          ^ t[3][((v >> 32) & 0xff) as usize] ^ t[2][((v >> 40) & 0xff) as usize]
          ^ t[1][((v >> 48) & 0xff) as usize] ^ t[0][(v >> 56) as usize];
    }
    for &b in chunks.remainder() { c = t[0][((c ^ b as u32) & 0xff) as usize] ^ (c >> 8); }
    !c
}

#[target_feature(enable = "sse4.2")]
unsafe fn crc_hw(data: &[u8]) -> u32 {
    use std::arch::x86_64::_mm_crc32_u64;
    let mut c = !0u64;
    let mut chunks = data.chunks_exact(8);
    for ch in &mut chunks { c = _mm_crc32_u64(c, u64::from_le_bytes(ch.try_into().unwrap())); }
    let mut c = c as u32;
    for &b in chunks.remainder() { c = std::arch::x86_64::_mm_crc32_u8(c, b); }
    !c
}

/// p50 of write(size) + fdatasync per operation, `writers` threads each
/// appending to its own file in `dir` at once.
fn sync_writers(dir: &str, writers: usize, size: usize, n: usize) -> f64 {
    let all = Arc::new(Mutex::new(Vec::new()));
    let threads: Vec<_> = (0..writers).map(|w| {
        let (dir, all) = (dir.to_string(), all.clone());
        std::thread::spawn(move || {
            let path = format!("{dir}/writer{w}.bench");
            let mut f = OpenOptions::new().create(true).write(true).truncate(true).open(&path).unwrap();
            let buf = vec![0x5Au8; size];
            let mut v = Vec::with_capacity(n);
            for _ in 0..n {
                let t0 = Instant::now();
                f.write_all(&buf).unwrap();
                f.sync_data().unwrap();
                v.push(t0.elapsed().as_secs_f64() * 1e6);
            }
            std::fs::remove_file(&path).unwrap();
            all.lock().unwrap().extend(v);
        })
    }).collect();
    for t in threads { t.join().unwrap(); }
    let mut v = all.lock().unwrap().clone();
    pct(&mut v, 0.5)
}

/// p50 of write(size) + fdatasync when `writers` threads take turns, one
/// sync at a time, each after an idle `gap_us`: the order a Raft round
/// syncs in (the leader's, then a follower's, a network hop apart). This is
/// not `sync_writers` with fewer writers: ext4's journal batches syncs that
/// arrive together, and a lone sync after an idle device does not get that.
fn sync_turns(dir: &str, writers: usize, size: usize, n: usize, gap_us: u64) -> f64 {
    let turn = Arc::new((Mutex::new(0usize), Condvar::new()));
    let all = Arc::new(Mutex::new(Vec::new()));
    let threads: Vec<_> = (0..writers).map(|w| {
        let (dir, all, turn) = (dir.to_string(), all.clone(), turn.clone());
        std::thread::spawn(move || {
            let path = format!("{dir}/turn{w}.bench");
            let mut f = OpenOptions::new().create(true).write(true).truncate(true).open(&path).unwrap();
            let buf = vec![0x5Au8; size];
            let mut v = Vec::with_capacity(n);
            for k in 0..n {
                let (m, cv) = &*turn;
                let mut g = m.lock().unwrap();
                while *g != k * writers + w { g = cv.wait(g).unwrap(); }
                drop(g);
                std::thread::sleep(Duration::from_micros(gap_us));
                let t0 = Instant::now();
                f.write_all(&buf).unwrap();
                f.sync_data().unwrap();
                v.push(t0.elapsed().as_secs_f64() * 1e6);
                *m.lock().unwrap() += 1;
                cv.notify_all();
            }
            std::fs::remove_file(&path).unwrap();
            all.lock().unwrap().extend(v);
        })
    }).collect();
    for t in threads { t.join().unwrap(); }
    let mut v = all.lock().unwrap().clone();
    pct(&mut v, 0.5)
}

/// Write bandwidth against volume: three writers append `chunk` + fdatasync
/// until `total` bytes are written. A device with a write cache runs fast
/// until the cache fills, then at what the media sustains. Returns (fast
/// MB/s, cache MB: the volume written before the rate fell below a third of
/// the first window's, slow MB/s after it; = fast and the whole volume if it
/// never fell). It idles a minute first so a cache left full by whatever ran
/// before drains -- a gate run, too, starts after the memory arm's run, which
/// writes nothing (a full cache measured 268 MB here, a drained one 1.6 GB).
fn bandwidth(dir: &str, chunk: usize, total: usize) -> (f64, f64, f64) {
    std::thread::sleep(Duration::from_secs(60));
    let t0 = Instant::now();
    let marks = Arc::new(Mutex::new(Vec::new()));
    let threads: Vec<_> = (0..3).map(|w| {
        let (dir, marks) = (dir.to_string(), marks.clone());
        std::thread::spawn(move || {
            let path = format!("{dir}/bw{w}.bench");
            let mut f = OpenOptions::new().create(true).write(true).truncate(true).open(&path).unwrap();
            let buf = vec![0x5Au8; chunk];
            for _ in 0..total / 3 / chunk {
                f.write_all(&buf).unwrap();
                f.sync_data().unwrap();
                marks.lock().unwrap().push(t0.elapsed().as_secs_f64());
            }
            drop(f);
            std::fs::remove_file(&path).unwrap();
        })
    }).collect();
    for t in threads { t.join().unwrap(); }
    let mut m = marks.lock().unwrap().clone();
    m.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let per = (128 << 20) / chunk; // 128 MiB windows
    let rate = |a: usize, b: usize| ((b - a) * chunk) as f64 / (m[b - 1] - if a == 0 { 0.0 } else { m[a - 1] }) / 1e6;
    let fast = rate(0, per);
    let mut w = per;
    while w + per <= m.len() && rate(w, w + per) > fast / 3.0 { w += per; }
    if w + per > m.len() {
        return (rate(0, m.len()), (m.len() * chunk) as f64 / 1e6, rate(0, m.len()));
    }
    (rate(0, w), (w * chunk) as f64 / 1e6, rate(w, m.len()))
}

/// p50 of how much longer than asked a thread sleep lasts.
fn sleep_overshoot(us: u64, n: usize) -> f64 {
    let mut v: Vec<f64> = (0..n).map(|_| {
        let t0 = Instant::now();
        std::thread::sleep(Duration::from_micros(us));
        t0.elapsed().as_secs_f64() * 1e6 - us as f64
    }).collect();
    pct(&mut v, 0.5)
}

/// p50 of a condvar hand-off: one thread sets a flag and notifies, the
/// waiting one runs; spaced so the waiter is asleep each time.
fn condvar_handoff(n: usize) -> f64 {
    let pair = Arc::new((Mutex::new(None::<Instant>), Condvar::new()));
    let p2 = pair.clone();
    let waiter = std::thread::spawn(move || {
        let mut v = Vec::with_capacity(n);
        for _ in 0..n {
            let (m, cv) = &*p2;
            let mut g = m.lock().unwrap();
            while g.is_none() { g = cv.wait(g).unwrap(); }
            v.push(g.take().unwrap().elapsed().as_secs_f64() * 1e6);
        }
        v
    });
    for _ in 0..n {
        std::thread::sleep(Duration::from_micros(300));
        let (m, cv) = &*pair;
        let mut g = m.lock().unwrap();
        // The waiter has taken the last one (else a hand-off would be lost
        // and the waiter would wait for it forever).
        while g.is_some() {
            drop(g);
            std::thread::sleep(Duration::from_micros(50));
            g = m.lock().unwrap();
        }
        *g = Some(Instant::now());
        drop(g);
        cv.notify_one();
    }
    let mut v = waiter.join().unwrap();
    pct(&mut v, 0.5)
}

fn main() {
    let dir = std::env::args().nth(1).expect("dir");
    let path = format!("{}/wal.bench", dir);
    let mut f = OpenOptions::new().create(true).write(true).truncate(true).open(&path).unwrap();
    let buf = vec![0xA5u8; 16 << 20];
    println!("write+fdatasync on {}, microseconds: size  n  p50  p99  [fdatasync alone p50]", dir);
    let mut written: u64 = 0;
    for &size in &[4096usize, 65536, 1 << 20, 16 << 20] {
        let n = if size >= (16 << 20) { 60 } else if size >= (1 << 20) { 400 } else { 3000 };
        let (mut tot, mut syn) = (Vec::new(), Vec::new());
        for _ in 0..n {
            if written > (256 << 20) { f.set_len(0).unwrap(); f = OpenOptions::new().write(true).truncate(true).open(&path).unwrap(); written = 0; }
            let t0 = Instant::now();
            f.write_all(&buf[..size]).unwrap();
            let t1 = Instant::now();
            f.sync_data().unwrap();
            let t2 = Instant::now();
            written += size as u64;
            tot.push((t2 - t0).as_secs_f64() * 1e6);
            syn.push((t2 - t1).as_secs_f64() * 1e6);
        }
        println!("  {:>9}  {:>4}  {:>8.1}  {:>8.1}  [{:.1}]", size, n, pct(&mut tot.clone(), 0.5), pct(&mut tot, 0.99), pct(&mut syn, 0.5));
    }
    drop(f);
    std::fs::remove_file(&path).unwrap();

    let mut dst = vec![0u8; 16 << 20];
    for &size in &[4096usize, 1 << 20] {
        let reps = (1usize << 31) / size;
        let t0 = Instant::now();
        for i in 0..reps { let o = (i * size) % ((16 << 20) - size + 1); dst[o..o + size].copy_from_slice(&buf[..size]); }
        let s = t0.elapsed().as_secs_f64();
        std::hint::black_box(&dst);
        println!("memcpy {:>8} B: {:.2} GB/s, {:.3} us per copy", size, (reps * size) as f64 / s / 1e9, s / reps as f64 * 1e6);
    }
    let t = table();
    let data: Vec<u8> = (0..(1 << 20)).map(|i| (i * 31 % 251) as u8).collect();
    assert_eq!(crc_sw(&t, b"123456789"), 0xE306_9283);
    assert_eq!(unsafe { crc_hw(b"123456789") }, 0xE306_9283);
    for &size in &[4096usize, 1 << 20] {
        let reps = (1usize << 30) / size;
        let t0 = Instant::now(); let mut x = 0u32;
        for i in 0..reps { x ^= crc_sw(&t, &data[(i % 7)..(i % 7) + size - 8]); }
        let s_sw = t0.elapsed().as_secs_f64();
        let t0 = Instant::now();
        for i in 0..reps { x ^= unsafe { crc_hw(&data[(i % 7)..(i % 7) + size - 8]) }; }
        let s_hw = t0.elapsed().as_secs_f64();
        std::hint::black_box(x);
        println!("crc32c {:>8} B: software {:.2} GB/s ({:.3} us), sse4.2 {:.2} GB/s ({:.3} us)", size,
                 (reps * size) as f64 / s_sw / 1e9, s_sw / reps as f64 * 1e6,
                 (reps * size) as f64 / s_hw / 1e9, s_hw / reps as f64 * 1e6);
    }

    // The primitives model.py reads.
    let s1 = sync_writers(&dir, 1, 4096, 2000);
    let s3 = sync_writers(&dir, 3, 4096, 2000);
    let big1 = sync_writers(&dir, 1, 1 << 20, 200);
    let big3 = sync_writers(&dir, 3, 1 << 20, 200);
    // Per KiB written, from 4 KiB to 1 MiB (the sync's fixed part cancels).
    let cw1 = (big1 - s1) / 1020.0;
    let cw3 = (big3 - s3) / 1020.0;
    // In turns after a 200 us gap (about a hop between replicas; the memory
    // build's send -> accepted is ~190 us): the pattern of a low-load round.
    let st = sync_turns(&dir, 3, 4096, 1000, 200);
    let (bw_fast, bw_cache, bw_slow) = bandwidth(&dir, 256 << 10, 3 << 30);
    let over = sleep_overshoot(1000, 500);
    let cv = condvar_handoff(3000);
    println!("sync 4 KiB p50: 1 writer {s1:.1} us, 3 writers {s3:.1} us; per KiB 1 writer {cw1:.3}, 3 writers {cw3:.3} us");
    println!("sync 4 KiB in turns (3 writers, 200 us apart) p50 {st:.1} us");
    println!("bandwidth, 3 writers x 256 KiB + fdatasync, 3 GiB: {bw_fast:.0} MB/s for the first {bw_cache:.0} MB, then {bw_slow:.0} MB/s");
    println!("sleep(1 ms) overshoot p50 {over:.1} us; condvar hand-off p50 {cv:.1} us");
    println!("PARAMS {{\"dir\": \"{dir}\", \"s1\": {s1:.1}, \"s3\": {s3:.1}, \"st\": {st:.1}, \"bw_fast\": {bw_fast:.0}, \"bw_cache\": {bw_cache:.0}, \"bw_slow\": {bw_slow:.0}, \"cw1\": {cw1:.3}, \"cw3\": {cw3:.3}, \"overshoot\": {over:.1}, \"handoff\": {cv:.1}}}");
}
