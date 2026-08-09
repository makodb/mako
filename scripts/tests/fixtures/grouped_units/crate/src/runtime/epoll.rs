pub struct PollMode(pub i32);

impl PollMode {
    pub const WRITE: PollMode = PollMode(2);
}
