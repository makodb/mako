#pragma once
#include "config_manager.h"
#include "cluster_config.h"

#include <functional>
#include <cstdint>
#include <chrono>

#include <rusty/sync/atomic.hpp>
#include <rusty/thread.hpp>
#include <rusty/option.hpp>
#include <rusty/function.hpp>   // rusty::Function callback (DSL-invocable)
#include <rusty/slice.hpp>   // deref_if_pointer_like

namespace janus {

/**
 * ConfigWatcher - Background polling that watches shard 0's config
 * version and refreshes a local ClusterConfig when it changes.
 *
 * Authored in the inline-Rust DSL (docs/storage-interface.md): the struct,
 * the Poll() logic, the accessors, AND the thread lifecycle are all the
 * `#if RUSTYCPP_RUST` block (regenerate with scripts/regen_storage_dsl.sh).
 * The stop flag + JoinHandle are DSL fields, and the stop-then-join on
 * destruction is a DSL `impl Drop` — the transpiler emits a real
 * ~ConfigWatcher() that runs the drop body (rusty::thread::JoinHandle
 * detaches on drop, so the join must be explicit). Start() spawns the poll
 * thread inline in the DSL too: `let op: *mut ConfigWatcher = &raw mut *self`
 * recovers `this` as a raw pointer (the DSL otherwise lowers `self` to the
 * object), which a `move ||` closure captures to call op->Poll() until
 * op->stop. No C++ kernels remain.
 */

struct ConfigWatcher;  // forward (the poll thread calls owner->Poll())

using CwCallback = rusty::Function<void(const ClusterConfig&)>;
using CwJoinHandle = rusty::Option<rusty::thread::JoinHandle<void>>;

#if RUSTYCPP_RUST
pub struct ConfigWatcher {
    cm: *mut ConfigManager,
    local_config: *mut ClusterConfig,
    poll_interval_ms: u64,
    last_version: u64,
    poll_count: u64,
    update_callback: rusty::Option<CwCallback>,
    stop: rusty::sync::atomic::AtomicBool,
    running: rusty::sync::atomic::AtomicBool,
    handle: CwJoinHandle,
}
impl ConfigWatcher {
    // Pure-DSL struct literal: AtomicBool has new_(), the callback is an empty
    // Option (None), the handle is None. No factory kernel.
    fn new(cm: *mut ConfigManager, local_config: *mut ClusterConfig, poll_interval_ms: u64) -> ConfigWatcher {
        ConfigWatcher {
            cm: cm,
            local_config: local_config,
            poll_interval_ms: poll_interval_ms,
            last_version: 0,
            poll_count: 0,
            update_callback: rusty::None,
            stop: rusty::sync::atomic::AtomicBool::new_(false),
            running: rusty::sync::atomic::AtomicBool::new_(false),
            handle: rusty::None,
        }
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
        // Invoke the (possibly-empty) update callback directly — rusty::Function
        // is bool-checkable + callable, so no erased-callable kernel.
        if (*self).update_callback.is_some() {
            unsafe { ((*self).update_callback.as_mut().unwrap())((*(*self).local_config)) };
        }
        true
    }
    // Start the background poll thread (idempotent). The thread captures a
    // raw pointer to self (stable: Start runs after the watcher is boxed) and
    // loops until stop is set — the old cw_spawn kernel, now DSL.
    fn Start(&mut self) {
        if (*self).running.load() {
            return;
        }
        (*self).stop.store(false);
        (*self).running.store(true);
        let op: *mut ConfigWatcher = &raw mut *self;
        let interval: u64 = (*self).poll_interval_ms;
        (*self).handle = rusty::Some(rusty::thread::spawn(move || {
            while !unsafe { (*op).stop.load() } {
                unsafe { (*op).Poll() };
                rusty::thread::sleep(std::chrono::milliseconds(interval));
            }
            unsafe { (*op).running.store(false) };
        }));
    }
    // Stop the thread and join it.
    fn Stop(&mut self) {
        (*self).stop.store(true);
        if (*self).handle.is_some() {
            (*self).handle.take().unwrap().join();
        }
        (*self).running.store(false);
    }
    fn SetUpdateCallback(&mut self, cb: CwCallback) {
        (*self).update_callback = rusty::Some(cb);
    }
    fn IsRunning(&self) -> bool {
        (*self).running.load()
    }
    fn GetLastVersion(&self) -> u64 {
        (*self).last_version
    }
    fn GetPollCount(&self) -> u64 {
        (*self).poll_count
    }
}
// Stop-then-join the poll thread on destruction so it never outlives the
// watcher. This is the join that used to live in a C++ CwPollThread RAII
// helper — now a DSL impl Drop (the transpiler emits ~ConfigWatcher()).
impl Drop for ConfigWatcher {
    fn drop(&mut self) {
        (*self).stop.store(true);
        if (*self).handle.is_some() {
            (*self).handle.take().unwrap().join();
        }
    }
}
#endif
/*RUSTYCPP:GEN-BEGIN id=config_watcher.1 version=1 rust_sha256=9ed9b276980be7bc1f10117537a94292b72bb27a83168dce89e9e9a0d4e604c2*/
struct ConfigWatcher;

struct ConfigWatcher {
    ConfigManager* cm;
    ClusterConfig* local_config;
    uint64_t poll_interval_ms;
    uint64_t last_version;
    uint64_t poll_count;
    rusty::Option<CwCallback> update_callback;
    rusty::sync::atomic::AtomicBool stop;
    rusty::sync::atomic::AtomicBool running;
    CwJoinHandle handle;
    mutable bool _rusty_forgotten = false;
    ConfigWatcher(ConfigManager* cm_init, ClusterConfig* local_config_init, uint64_t poll_interval_ms_init, uint64_t last_version_init, uint64_t poll_count_init, rusty::Option<CwCallback> update_callback_init, rusty::sync::atomic::AtomicBool stop_init, rusty::sync::atomic::AtomicBool running_init, CwJoinHandle handle_init) : cm(std::move(cm_init)), local_config(std::move(local_config_init)), poll_interval_ms(std::move(poll_interval_ms_init)), last_version(std::move(last_version_init)), poll_count(std::move(poll_count_init)), update_callback(std::move(update_callback_init)), stop(std::move(stop_init)), running(std::move(running_init)), handle(std::move(handle_init)) {}
    ConfigWatcher(const ConfigWatcher&) = default;
    ConfigWatcher(ConfigWatcher&& other) noexcept : cm(std::move(other.cm)), local_config(std::move(other.local_config)), poll_interval_ms(std::move(other.poll_interval_ms)), last_version(std::move(other.last_version)), poll_count(std::move(other.poll_count)), update_callback(std::move(other.update_callback)), stop(std::move(other.stop)), running(std::move(other.running)), handle(std::move(other.handle)) {
        this->_rusty_forgotten = other._rusty_forgotten;
        other._rusty_forgotten = true;
    }
    ConfigWatcher& operator=(const ConfigWatcher&) = default;
    ConfigWatcher& operator=(ConfigWatcher&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~ConfigWatcher();
        new (this) ConfigWatcher(std::move(other));
        return *this;
    }
    void rusty_mark_forgotten() const noexcept { _rusty_forgotten = true; }


    static ConfigWatcher new_(ConfigManager* cm, ClusterConfig* local_config, uint64_t poll_interval_ms);
    bool Poll();
    void Start();
    void Stop();
    void SetUpdateCallback(CwCallback cb);
    bool IsRunning() const;
    uint64_t GetLastVersion() const;
    uint64_t GetPollCount() const;
    ~ConfigWatcher() noexcept(false);
};


inline ConfigWatcher ConfigWatcher::new_(ConfigManager* cm, ClusterConfig* local_config, uint64_t poll_interval_ms) {
    return ConfigWatcher(cm, local_config, std::move(poll_interval_ms), static_cast<uint64_t>(0), static_cast<uint64_t>(0), rusty::None, rusty::sync::atomic::AtomicBool::new_(false), rusty::sync::atomic::AtomicBool::new_(false), rusty::None);
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
    if (((*this)).update_callback.is_some()) {
        // @unsafe
        {
            (((*this)).update_callback.as_mut().unwrap())((rusty::detail::deref_if_pointer_like(((*this)).local_config)));
        }
    }
    return true;
}

inline void ConfigWatcher::Start() {
    if (((*this)).running.load()) {
        return;
    }
    ((*this)).stop.store(false);
    ((*this)).running.store(true);
    ConfigWatcher* const op = &(*this);
    const uint64_t interval = ((*this)).poll_interval_ms;
    ((*this)).handle = rusty::Some(rusty::thread::spawn([=, interval = std::move(interval), op = std::move(op)]() mutable {
while (!(*op).stop.load()) {
    // @unsafe
    {
        ((*op)).Poll();
    }
    rusty::thread::sleep(std::chrono::milliseconds(std::move(interval)));
}
// @unsafe
{
    (*op).running.store(false);
}
}));
}

inline void ConfigWatcher::Stop() {
    ((*this)).stop.store(true);
    if (((*this)).handle.is_some()) {
        ((*this)).handle.take().unwrap().join();
    }
    ((*this)).running.store(false);
}

inline void ConfigWatcher::SetUpdateCallback(CwCallback cb) {
    ((*this)).update_callback = rusty::Option<CwCallback>(std::move(cb));
}

inline bool ConfigWatcher::IsRunning() const {
    return ((*this)).running.load();
}

inline uint64_t ConfigWatcher::GetLastVersion() const {
    return ((*this)).last_version;
}

inline uint64_t ConfigWatcher::GetPollCount() const {
    return ((*this)).poll_count;
}

inline ConfigWatcher::~ConfigWatcher() noexcept(false) {
    if (_rusty_forgotten) { return; }
    ((*this)).stop.store(true);
    if (((*this)).handle.is_some()) {
        ((*this)).handle.take().unwrap().join();
    }
}
/*RUSTYCPP:GEN-END id=config_watcher.1*/

}  // namespace janus
