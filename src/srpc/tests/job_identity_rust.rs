use srpc::misc::{Job, OneTimeJob};
use srpc::reactor::{PollCommand, PollThread};
use std::sync::{atomic::{AtomicBool, AtomicUsize, Ordering}, mpsc, Arc};
use std::time::Duration;

struct GatedJob {
    ready: Arc<AtomicBool>,
    calls: Arc<AtomicUsize>,
    completed: mpsc::Sender<()>,
}

// The worker is the only caller of Job methods. Shared state uses atomics/channels.
#[allow(unsafe_code)]
unsafe impl Job for GatedJob {
    fn Ready(&mut self) -> bool { self.ready.load(Ordering::Acquire) }
    fn Work(&mut self) {
        self.calls.fetch_add(1, Ordering::Relaxed);
        self.completed.send(()).unwrap();
    }
    fn Done(&mut self) -> bool { false }
}

#[test]
fn queued_jobs_deduplicate_and_remove_by_arc_identity() {
    let worker = PollThread::create();
    let ready = Arc::new(AtomicBool::new(false));
    let calls = Arc::new(AtomicUsize::new(0));
    let removed_calls = Arc::new(AtomicUsize::new(0));
    let (completed, completion) = mpsc::channel();
    let job: Arc<dyn Job> = Arc::new(GatedJob {
        ready: ready.clone(), calls: calls.clone(), completed: completed.clone(),
    });
    let removed: Arc<dyn Job> = Arc::new(GatedJob {
        ready: ready.clone(), calls: removed_calls.clone(), completed,
    });
    worker.add(job.clone());
    worker.add(job);
    worker.add(removed.clone());
    worker.sender_.send(PollCommand::RemoveJob { job: removed }).unwrap();
    let (processed, commands_processed) = mpsc::channel();
    worker.add(Arc::new(OneTimeJob::new(Box::new(move || {
        processed.send(()).unwrap();
    }))));
    commands_processed.recv_timeout(Duration::from_secs(3)).unwrap();
    ready.store(true, Ordering::Release);
    completion.recv_timeout(Duration::from_secs(3)).unwrap();
    worker.shutdown();
    assert_eq!(calls.load(Ordering::Relaxed), 1);
    assert_eq!(removed_calls.load(Ordering::Relaxed), 0);
}
