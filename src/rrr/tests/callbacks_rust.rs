use rrr::callbacks::{CallbackManager, ConnectionCallbacks};
use rrr::errors::RpcError;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Condvar, Mutex};
use std::thread;
use std::time::Duration;

fn assert_send_and_sync<T: Send + Sync>() {}

#[test]
fn public_state_is_thread_safe_and_initially_empty() {
    assert_send_and_sync::<ConnectionCallbacks>();
    assert_send_and_sync::<CallbackManager>();

    let manager = CallbackManager::new();
    assert_eq!(manager.callback_count(), 0);
    assert!(!manager.has_callbacks());
    assert_eq!(manager.on_connected_count(), 0);
    assert_eq!(manager.on_disconnected_count(), 0);
    assert_eq!(manager.on_error_count(), 0);
    assert_eq!(manager.on_reconnecting_count(), 0);
    assert_eq!(manager.on_reconnected_count(), 0);
}

#[test]
fn every_event_preserves_arguments_order_and_counts() {
    let manager = CallbackManager::new();
    let observed = Arc::new(Mutex::new(Vec::<String>::new()));

    let sink = Arc::clone(&observed);
    manager.add_on_connected(Box::new(move || {
        sink.lock().unwrap().push("connected".to_owned());
    }));

    let sink = Arc::clone(&observed);
    manager.add_on_disconnected(Box::new(move || {
        sink.lock().unwrap().push("disconnected".to_owned());
    }));

    let sink = Arc::clone(&observed);
    manager.add_on_error(Box::new(move |error, message| {
        sink.lock()
            .unwrap()
            .push(format!("error:{}:{message}", error as i32));
    }));

    let sink = Arc::clone(&observed);
    manager.add_on_reconnecting(Box::new(move || {
        sink.lock().unwrap().push("reconnecting".to_owned());
    }));

    let sink = Arc::clone(&observed);
    manager.add_on_reconnected(Box::new(move |success| {
        sink.lock().unwrap().push(format!("reconnected:{success}"));
    }));

    assert_eq!(manager.callback_count(), 5);
    manager.invoke_on_connected();
    manager.invoke_on_disconnected();
    manager.invoke_on_error(RpcError::REQUEST_TIMEOUT, &"deadline".to_owned());
    manager.invoke_on_reconnecting();
    manager.invoke_on_reconnected(false);

    assert_eq!(
        *observed.lock().unwrap(),
        [
            "connected",
            "disconnected",
            "error:401:deadline",
            "reconnecting",
            "reconnected:false",
        ]
    );
}

#[test]
fn panic_is_swallowed_and_later_callbacks_still_run() {
    let manager = CallbackManager::new();
    let reached = Arc::new(AtomicBool::new(false));

    manager.add_on_connected(Box::new(|| panic!("expected callback panic")));
    let reached_by_callback = Arc::clone(&reached);
    manager.add_on_connected(Box::new(move || {
        reached_by_callback.store(true, Ordering::SeqCst);
    }));

    manager.invoke_on_connected();
    assert!(reached.load(Ordering::SeqCst));
}

#[test]
fn registration_during_dispatch_only_affects_the_next_snapshot() {
    let manager = Arc::new(CallbackManager::new());
    let installed = Arc::new(AtomicBool::new(false));
    let late_calls = Arc::new(AtomicUsize::new(0));

    let manager_weak = Arc::downgrade(&manager);
    let installed_by_callback = Arc::clone(&installed);
    let late_calls_by_callback = Arc::clone(&late_calls);
    manager.add_on_connected(Box::new(move || {
        if !installed_by_callback.swap(true, Ordering::SeqCst) {
            let late_calls = Arc::clone(&late_calls_by_callback);
            manager_weak
                .upgrade()
                .unwrap()
                .add_on_connected(Box::new(move || {
                    late_calls.fetch_add(1, Ordering::SeqCst);
                }));
        }
    }));

    manager.invoke_on_connected();
    assert_eq!(late_calls.load(Ordering::SeqCst), 0);
    manager.invoke_on_connected();
    assert_eq!(late_calls.load(Ordering::SeqCst), 1);
}

#[test]
fn clear_all_waits_for_a_dispatch_snapshot_to_finish() {
    let manager = Arc::new(CallbackManager::new());
    let release = Arc::new((Mutex::new(false), Condvar::new()));
    let (callback_started_tx, callback_started_rx) = mpsc::channel();

    let release_in_callback = Arc::clone(&release);
    manager.add_on_connected(Box::new(move || {
        callback_started_tx.send(()).unwrap();
        let (lock, condvar) = &*release_in_callback;
        let guard = lock.lock().unwrap();
        let _guard = condvar.wait_while(guard, |released| !*released).unwrap();
    }));

    let dispatch_manager = Arc::clone(&manager);
    let dispatch = thread::spawn(move || dispatch_manager.invoke_on_connected());
    callback_started_rx
        .recv_timeout(Duration::from_secs(2))
        .unwrap();

    let (clear_done_tx, clear_done_rx) = mpsc::channel();
    let clear_manager = Arc::clone(&manager);
    let clear = thread::spawn(move || {
        clear_manager.clear_all();
        clear_done_tx.send(()).unwrap();
    });

    let mut attempts = 0;
    while manager.callback_count() != 0 && attempts < 2_000 {
        thread::sleep(Duration::from_millis(1));
        attempts += 1;
    }
    assert_eq!(manager.callback_count(), 0);
    assert!(matches!(
        clear_done_rx.recv_timeout(Duration::from_millis(20)),
        Err(mpsc::RecvTimeoutError::Timeout)
    ));

    {
        let (lock, condvar) = &*release;
        *lock.lock().unwrap() = true;
        condvar.notify_all();
    }

    clear_done_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    dispatch.join().unwrap();
    clear.join().unwrap();
    assert_eq!(manager.callback_count(), 0);
}

#[test]
fn concurrent_registration_and_invocation_are_lossless() {
    let manager = Arc::new(CallbackManager::new());
    let calls = Arc::new(AtomicUsize::new(0));
    let mut workers = Vec::new();

    for _ in 0..4 {
        let manager = Arc::clone(&manager);
        let calls = Arc::clone(&calls);
        workers.push(thread::spawn(move || {
            for _ in 0..25 {
                let calls = Arc::clone(&calls);
                manager.add_on_connected(Box::new(move || {
                    calls.fetch_add(1, Ordering::SeqCst);
                }));
            }
        }));
    }
    for worker in workers {
        worker.join().unwrap();
    }

    assert_eq!(manager.callback_count(), 100);
    manager.invoke_on_connected();
    assert_eq!(calls.load(Ordering::SeqCst), 100);
}
