use std::sync::{Arc, atomic::{AtomicUsize, Ordering}};

#[test]
fn native_threads_run_and_have_distinct_stable_identities() {
    let owner = std::thread::current().id();
    assert_eq!(owner, std::thread::current().id());
    let observed = Arc::new(AtomicUsize::new(0));
    let child_observed = observed.clone();
    srpc::threading::spawn_abort_on_panic(move || {
        assert_ne!(owner, std::thread::current().id());
        assert_eq!(std::thread::current().id(), std::thread::current().id());
        child_observed.store(37, Ordering::Release);
    }).join().unwrap();
    assert_eq!(observed.load(Ordering::Acquire), 37);
}

#[test]
fn thread_panics_do_not_disappear() {
    if std::env::var_os("SRPC_ADAPTER_PANIC_CHILD").is_some() {
        srpc::threading::spawn_abort_on_panic(|| panic!("uncaught native thread panic")).join().unwrap();
        panic!("thread panic was swallowed");
    }
    use std::os::unix::process::ExitStatusExt;
    let output = std::process::Command::new("sh")
        .args(["-c", "ulimit -c 0\nexec \"$1\" --exact thread_panics_do_not_disappear --nocapture", "sh"])
        .arg(std::env::current_exe().unwrap())
        .env("SRPC_ADAPTER_PANIC_CHILD", "1")
        .output().unwrap();
    assert_eq!(output.status.signal(), Some(6), "{output:?}");
    assert!(String::from_utf8_lossy(&output.stderr).contains("uncaught native thread panic"));
}

#[test]
fn poll_worker_can_request_shutdown_without_joining_itself() {
    let worker = srpc::reactor::PollThread::create();
    let weak_worker = Arc::downgrade(&worker);
    let (finished, result) = std::sync::mpsc::channel();
    worker.add(Arc::new(srpc::misc::OneTimeJob::new(Box::new(move || {
        let worker = weak_worker.upgrade().unwrap();
        worker.shutdown();
        finished.send(()).unwrap();
    }))));
    result.recv_timeout(std::time::Duration::from_secs(3)).unwrap();
    let handle = worker.join_handle_.lock().unwrap().take().unwrap();
    handle.join().unwrap();
    assert_ne!(worker.poll_thread_id_bits_.load(Ordering::Acquire), 0);
}
