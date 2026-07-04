#pragma once
#include "config_manager.h"
#include "cluster_config.h"

#include <functional>
#include <cstdint>
#include <chrono>

#include <rusty/sync/atomic.hpp>
#include <rusty/thread.hpp>
#include <rusty/option.hpp>
#include <rusty/slice.hpp>   // deref_if_pointer_like

namespace janus {

/**
 * ConfigWatcher - Background polling that watches shard 0's config
 * version and refreshes a local ClusterConfig when it changes.
 *
 * Authored in the inline-Rust DSL (docs/storage-interface.md): the struct,
 * the Poll() logic, and the accessors are the `#if RUSTYCPP_RUST` block
 * (regenerate with scripts/regen_storage_dsl.sh). The one thing that stays
 * C++ is the THREAD LIFECYCLE: a background poll thread must be
 * stop-then-joined when the watcher is destroyed, and the DSL cannot emit
 * that custom RAII destructor (rusty::thread::JoinHandle detaches on drop,
 * not joins). So the thread + stop flag live in a small CwPollThread RAII
 * helper held as a field; the DSL struct's IMPLICIT destructor destroys it
 * and joins. Start()/Stop() delegate to it; the poll loop calls back into
 * the DSL Poll().
 */

struct ConfigWatcher;  // forward (the poll thread calls owner->Poll())

using CwCallback = std::function<void(const ClusterConfig&)>;
using CwAtomicBool = rusty::sync::atomic::AtomicBool;

// @unsafe - RAII controller for the background poll thread: owns the stop
// flag + JoinHandle, and stop-then-joins on drop so the thread never
// outlives its owner.
class CwPollThread {
    CwAtomicBool stop_{false};
    CwAtomicBool running_{false};
    rusty::Option<rusty::thread::JoinHandle<void>> handle_{rusty::None};
public:
    CwPollThread() = default;
    CwPollThread(CwPollThread&&) noexcept = default;
    CwPollThread& operator=(CwPollThread&&) noexcept = default;
    CwPollThread(const CwPollThread&) = delete;
    CwPollThread& operator=(const CwPollThread&) = delete;
    ~CwPollThread() { stop_and_join(); }

    bool is_running() const { return running_.load(); }

    // Defined below the GEN block (needs a complete ConfigWatcher for Poll()).
    // Takes a reference: the DSL lowers `self` to `(*this)` (the object).
    void start(ConfigWatcher& owner, uint64_t interval_ms);

    void stop_and_join() {
        stop_.store(true);
        if (handle_.is_some()) {
            handle_.take().unwrap().join();
        }
    }
};

class ConfigWatcher;
inline ConfigWatcher cw_new(ConfigManager* cm, ClusterConfig* local, uint64_t ms);
// @safe - invoke the (possibly empty) update callback.
inline void cw_invoke(const CwCallback& cb, const ClusterConfig& cfg) {
    if (cb) cb(cfg);
}

#if RUSTYCPP_RUST
pub struct ConfigWatcher {
    cm: *mut ConfigManager,
    local_config: *mut ClusterConfig,
    poll_interval_ms: u64,
    last_version: u64,
    poll_count: u64,
    update_callback: CwCallback,
    poll_ctl: CwPollThread,
}
impl ConfigWatcher {
    fn new(cm: *mut ConfigManager, local_config: *mut ClusterConfig, poll_interval_ms: u64) -> ConfigWatcher {
        unsafe { cw_new(cm, local_config, poll_interval_ms) }
    }
    // One poll: reload the local ClusterConfig iff the version changed.
    fn Poll(&mut self) -> bool {
        (*self).poll_count = (*self).poll_count + 1;
        let current_version: u64 = unsafe { (*(*self).cm).GetVersion() };
        if current_version == (*self).last_version {
            return false;
        }
        let ok: bool = unsafe { (*(*self).local_config).LoadFromConfigManager((*self).cm) };
        if !ok {
            return false;
        }
        (*self).last_version = current_version;
        unsafe { cw_invoke((*self).update_callback, (*(*self).local_config)) };
        true
    }
    fn Start(&mut self) {
        (*self).poll_ctl.start(self, (*self).poll_interval_ms);
    }
    fn Stop(&mut self) {
        (*self).poll_ctl.stop_and_join();
    }
    fn SetUpdateCallback(&mut self, cb: CwCallback) {
        (*self).update_callback = cb;
    }
    fn IsRunning(&self) -> bool {
        (*self).poll_ctl.is_running()
    }
    fn GetLastVersion(&self) -> u64 {
        (*self).last_version
    }
    fn GetPollCount(&self) -> u64 {
        (*self).poll_count
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=config_watcher.1 version=1 rust_sha256=dafa44a07210db56d214c3bc3f1d71ee222794c5d148dc920a10c8147034e512*/
struct ConfigWatcher;

struct ConfigWatcher {
    ConfigManager* cm;
    ClusterConfig* local_config;
    uint64_t poll_interval_ms;
    uint64_t last_version;
    uint64_t poll_count;
    CwCallback update_callback;
    CwPollThread poll_ctl;

    static ConfigWatcher new_(ConfigManager* cm, ClusterConfig* local_config, uint64_t poll_interval_ms);
    bool Poll();
    void Start();
    void Stop();
    void SetUpdateCallback(CwCallback cb);
    bool IsRunning() const;
    uint64_t GetLastVersion() const;
    uint64_t GetPollCount() const;
};


inline ConfigWatcher ConfigWatcher::new_(ConfigManager* cm, ClusterConfig* local_config, uint64_t poll_interval_ms) {
    // @unsafe
    {
        return cw_new(cm, local_config, std::move(poll_interval_ms));
    }
}

inline bool ConfigWatcher::Poll() {
    ((*this)).poll_count = rusty::detail::deref_if_pointer_like(((*this)).poll_count) + 1;
    const uint64_t current_version = ((rusty::detail::deref_if_pointer_like(((*this)).cm))).GetVersion();
    if (rusty::detail::deref_if_pointer_like(current_version) == rusty::detail::deref_if_pointer_like(((*this)).last_version)) {
        return false;
    }
    const bool ok = ((rusty::detail::deref_if_pointer_like(((*this)).local_config))).LoadFromConfigManager(((*this)).cm);
    if (!ok) {
        return false;
    }
    ((*this)).last_version = std::move(current_version);
    // @unsafe
    {
        cw_invoke(((*this)).update_callback, (rusty::detail::deref_if_pointer_like(((*this)).local_config)));
    }
    return true;
}

inline void ConfigWatcher::Start() {
    ((*this)).poll_ctl.start((*this), ((*this)).poll_interval_ms);
}

inline void ConfigWatcher::Stop() {
    ((*this)).poll_ctl.stop_and_join();
}

inline void ConfigWatcher::SetUpdateCallback(CwCallback cb) {
    ((*this)).update_callback = std::move(cb);
}

inline bool ConfigWatcher::IsRunning() const {
    return ((*this)).poll_ctl.is_running();
}

inline uint64_t ConfigWatcher::GetLastVersion() const {
    return ((*this)).last_version;
}

inline uint64_t ConfigWatcher::GetPollCount() const {
    return ((*this)).poll_count;
}
/*RUSTYCPP:GEN-END id=config_watcher.1*/

// @safe - factory (aggregate init; ConfigWatcher complete here).
inline ConfigWatcher cw_new(ConfigManager* cm, ClusterConfig* local, uint64_t ms) {
    return ConfigWatcher{cm, local, ms, /*last_version*/0, /*poll_count*/0,
                         CwCallback(), CwPollThread()};
}

// @unsafe - spawn the poll loop (owner is a complete ConfigWatcher here).
inline void CwPollThread::start(ConfigWatcher& owner, uint64_t interval_ms) {
    if (running_.load()) return;
    stop_.store(false);
    running_.store(true);
    CwPollThread* self = this;
    ConfigWatcher* op = &owner;
    handle_ = rusty::Some(rusty::thread::spawn([self, op, interval_ms]() {
        while (!self->stop_.load()) {
            op->Poll();
            rusty::thread::sleep(std::chrono::milliseconds(interval_ms));
        }
        self->running_.store(false);
    }));
}

}  // namespace janus
