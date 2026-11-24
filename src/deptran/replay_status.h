#pragma once
namespace janus {
// Application replay status encoded in the low decimal digit of callbacks.
enum ReplayStatus {
  STATUS_NORMAL = 0,
  STATUS_INIT = 1,
  STATUS_ENDING = 2,
  STATUS_SAFETY_FAIL = 3,
  STATUS_REPLAY_DONE = 4,
  STATUS_NOOPS = 5
};
}  // namespace janus
