//! Deterministic workload data. This is a benchmark PRNG, not a security API.
pub struct Random(u64);
impl Random {
    pub fn new(seed: u64) -> Self {
        Self(seed)
    }
    pub fn draw(&mut self) -> u64 {
        self.0 = self.0.wrapping_add(0x9e3779b97f4a7c15);
        let mut value = self.0;
        value = (value ^ (value >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
        value = (value ^ (value >> 27)).wrapping_mul(0x94d049bb133111eb);
        value ^ (value >> 31)
    }
    pub fn between(&mut self, low: u32, high: u32) -> u32 {
        assert!(low <= high);
        low + (self.draw() % (u64::from(high) - u64::from(low) + 1)) as u32
    }
    pub fn nonuniform(&mut self, a: u32, c: u32, low: u32, high: u32) -> u32 {
        ((self.between(0, a) | self.between(low, high)) + c) % (high - low + 1) + low
    }
    pub(super) fn text(&mut self, low: u32, high: u32) -> Vec<u8> {
        let count = self.between(low, high);
        (0..count)
            .map(|_| b'A' + self.between(0, 25) as u8)
            .collect()
    }
    pub(super) fn data(&mut self) -> Vec<u8> {
        let mut data = self.text(26, 50);
        if self.between(1, 100) <= 10 {
            let start = self.between(0, data.len() as u32 - 8) as usize;
            data[start..start + 8].copy_from_slice(b"ORIGINAL");
        }
        data
    }
}
pub fn last_name(number: u32) -> Vec<u8> {
    const PARTS: [&str; 10] = [
        "BAR", "OUGHT", "ABLE", "PRI", "PRES", "ESE", "ANTI", "CALLY", "ATION", "EING",
    ];
    let n = number % 1000;
    format!(
        "{}{}{}",
        PARTS[(n / 100) as usize],
        PARTS[(n / 10 % 10) as usize],
        PARTS[(n % 10) as usize]
    )
    .into_bytes()
}
