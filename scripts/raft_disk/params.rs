// The disk plan's measured parameters (docs/verus/disk-persistence-plan.md §5):
// write+fdatasync on tmpfs, copy and CRC32C rates. std only, as raft-store.
// Build: rustc -O --edition 2021 -o /tmp/params scripts/raft_disk/params.rs
// Run:   d=$(mktemp -d /dev/shm/raft-wal-$USER-bench.XXXX); /tmp/params $d; rm -rf $d
use std::fs::OpenOptions;
use std::io::Write;
use std::time::Instant;

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

fn main() {
    let dir = std::env::args().nth(1).expect("dir");
    let path = format!("{}/wal.bench", dir);
    let mut f = OpenOptions::new().create(true).write(true).truncate(true).open(&path).unwrap();
    let buf = vec![0xA5u8; 16 << 20];
    println!("write+fdatasync on {} (tmpfs), microseconds: size  n  p50  p99  [fdatasync alone p50]", dir);
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
}
