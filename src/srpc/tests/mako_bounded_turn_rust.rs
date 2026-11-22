// Mako-local: the bounded poll turn of a PollThread's driver (see the
// Mako-local notes in reactor/reactor.rs: Reactor::run_loop,
// Reactor::process_stackless_tasks, reactor_create_run_fiber_at_impl and
// poll_driver_poll).  This file goes upstream with that change.
//
// These tests drive the real crate, as pollthread_lion_rust.rs does: real
// PollThreads, real fibers, a real loopback socket.
//
//   1. Shutdown drain.  A stopping driver has no next turn, so the event
//      chain a job starts in the same driver poll that takes the Shutdown
//      command must still run to its end, as it did when run_loop drained to
//      quiescence.  Without the drain in poll_driver_poll the chain stops
//      after its first hop.
//   2. Bounded turn.  Two fibers passing a baton keep the ready queue
//      non-empty forever.  The driver must still take commands, and must
//      still return to Lion so the TCP writer task sends a frame queued from
//      the poll thread.  With run_loop's unbounded drain the first job after
//      the baton starts never runs.

#![allow(unsafe_code)]

use srpc::channel::{ChannelError, ChannelFrame};
use srpc::misc::{Job, OneTimeJob};
use srpc::reactor::{create_sp_int_event, Fiber, IntEvent, PollThread};
use srpc::tcp_channel::TcpFactory;
use std::cell::RefCell;
use std::io::Read;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex, MutexGuard, PoisonError};
use std::time::Duration;

static SERIAL: Mutex<()> = Mutex::new(());

fn serial() -> MutexGuard<'static, ()> {
    SERIAL.lock().unwrap_or_else(PoisonError::into_inner)
}

const LIMIT: Duration = Duration::from_secs(10);

fn job<F: FnOnce() + Send + 'static>(f: F) -> Arc<dyn Job> {
    let slot = Mutex::new(Some(f));
    Arc::new(OneTimeJob::new(Box::new(move || {
        let taken = slot.lock().unwrap().take();
        if let Some(f) = taken {
            f();
        }
    })))
}

fn run_on<R: Send + 'static, F: FnOnce() -> R + Send + 'static>(pt: &PollThread, f: F) -> R {
    let (tx, rx) = mpsc::channel();
    pt.add(job(move || {
        tx.send(f()).unwrap();
    }));
    rx.recv_timeout(LIMIT).expect("the job never ran on the poll thread")
}

#[test]
fn a_chain_started_in_the_stopping_poll_runs_to_its_end() {
    let _serial = serial();
    const HOPS: usize = 8;
    for round in 0..20 {
        let pt = PollThread::create();
        let reached = Arc::new(AtomicU64::new(0));
        // Hold the driver inside one job while the kick job and the Shutdown
        // command queue up behind it, so its next poll takes both.
        let (hold_tx, hold_rx) = mpsc::channel::<()>();
        let (held_tx, held_rx) = mpsc::channel::<()>();
        let chain_reached = reached.clone();
        pt.add(job(move || {
            // The chain: fiber i waits on event i, then sets event i + 1.
            let events: Rc<RefCell<Vec<Arc<IntEvent>>>> = Rc::new(RefCell::new(Vec::new()));
            for _ in 0..=HOPS {
                events.borrow_mut().push(create_sp_int_event(1));
            }
            for hop in 0..HOPS {
                let (mine, next) = {
                    let all = events.borrow();
                    (all[hop].clone(), all[hop + 1].clone())
                };
                let counter = chain_reached.clone();
                Fiber::create_run(move || {
                    mine.wait();
                    counter.fetch_add(1, Ordering::AcqRel);
                    next.set(1);
                });
            }
            let first = events.borrow()[0].clone();
            // The kick runs as a later job, on this thread.
            KICK.with(|slot| *slot.borrow_mut() = Some(first));
            held_tx.send(()).unwrap();
            hold_rx.recv_timeout(LIMIT).expect("the hold was never released");
        }));
        held_rx.recv_timeout(LIMIT).expect("the holding job never ran");
        pt.add(job(move || {
            let first = KICK.with(|slot| slot.borrow_mut().take()).unwrap();
            first.set(1);
        }));
        let stopper = pt.clone();
        let (stopped_tx, stopped_rx) = mpsc::channel::<()>();
        let joined = std::thread::spawn(move || {
            stopper.shutdown();
            let _ = stopped_tx.send(());
        });
        // Let the Shutdown command reach the channel, then release the driver.
        std::thread::sleep(Duration::from_millis(30));
        hold_tx.send(()).unwrap();
        // Bounded: a stop drain that never reaches quiescence must fail this
        // test, not hang the gate.
        stopped_rx.recv_timeout(LIMIT).expect("round: the stopping driver never finished its drain");
        joined.join().unwrap();
        assert_eq!(
            reached.load(Ordering::Acquire),
            HOPS as u64,
            "round {round}: the stopping driver abandoned the chain"
        );
    }
}

thread_local! {
    static KICK: RefCell<Option<Arc<IntEvent>>> = const { RefCell::new(None) };
}

struct Baton {
    events: [RefCell<Option<Arc<IntEvent>>>; 2],
}

#[test]
fn ready_pressure_does_not_starve_commands_or_the_writer_task() {
    let _serial = serial();
    let pt = PollThread::create();
    let peer = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
    let connected = TcpFactory::new(pt.clone()).connect(&peer.local_addr().unwrap().to_string());
    assert_eq!(connected.error, ChannelError::None);
    let proxy = Arc::new(Mutex::new(connected.connection));
    let (mut stream, _) = peer.accept().unwrap();
    stream.set_read_timeout(Some(LIMIT)).unwrap();

    let stop = Arc::new(AtomicBool::new(false));
    let exchanges = Arc::new(AtomicU64::new(0));
    let (fiber_stop, fiber_exchanges) = (stop.clone(), exchanges.clone());
    run_on(&pt, move || {
        let baton = Rc::new(Baton { events: [RefCell::new(None), RefCell::new(None)] });
        for side in 0..2 {
            *baton.events[side].borrow_mut() = Some(create_sp_int_event(1));
        }
        for side in 0..2usize {
            let (baton, stop, exchanges) = (baton.clone(), fiber_stop.clone(), fiber_exchanges.clone());
            Fiber::create_run(move || loop {
                let current = baton.events[side].borrow().as_ref().unwrap().clone();
                current.wait();
                let other = baton.events[1 - side].borrow().as_ref().unwrap().clone();
                exchanges.fetch_add(1, Ordering::AcqRel);
                if stop.load(Ordering::Acquire) {
                    other.set(1);
                    return;
                }
                *baton.events[side].borrow_mut() = Some(create_sp_int_event(1));
                other.set(1);
            });
        }
        let first = baton.events[0].borrow().as_ref().unwrap().clone();
        first.set(1);
    });
    // The pressure is running: every later command needs a bounded turn.
    let before = exchanges.load(Ordering::Acquire);
    run_on(&pt, || ());
    // A frame queued from the poll thread is written only by the writer task,
    // which runs only when the driver returns to Lion.
    let sender = proxy.clone();
    let payload: Vec<u8> = (0..64u8).collect();
    let sent = payload.clone();
    let result = run_on(&pt, move || {
        let guard = sender.lock().unwrap();
        let frame = ChannelFrame { payload: sent.as_ptr(), size: sent.len() };
        unsafe { guard.as_ref().unwrap().send_frame(&frame) }
    });
    assert_eq!(result, ChannelError::None);
    let mut wire = vec![0u8; 4 + payload.len()];
    stream.read_exact(&mut wire).expect("the writer task never sent the frame under ready pressure");
    assert_eq!(&wire[4..], &payload[..]);
    let after = exchanges.load(Ordering::Acquire);
    assert!(after > before, "the baton stopped moving: {before} -> {after}");
    stop.store(true, Ordering::Release);
    run_on(&pt, || ());
    {
        let guard = proxy.lock().unwrap();
        guard.as_ref().unwrap().close();
    }
    pt.shutdown();
}
