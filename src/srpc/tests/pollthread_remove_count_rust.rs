use srpc::reactor::PollThread;
use std::os::fd::AsRawFd;
use std::os::unix::net::UnixStream;

#[test]
fn removal_count_tracks_accepted_commands_across_threads_and_shutdown() {
    let worker = PollThread::create();
    let (socket, _peer) = UnixStream::pair().unwrap();
    let fd = socket.as_raw_fd();
    assert_eq!(worker.get_remove_count(), 0);
    let threads: Vec<_> = (0..4)
        .map(|_| {
            let worker = worker.clone();
            std::thread::spawn(move || {
                for _ in 0..8 {
                    worker.remove_fd(fd);
                }
            })
        })
        .collect();
    for thread in threads {
        thread.join().unwrap();
    }
    assert_eq!(worker.get_remove_count(), 32);
    worker.shutdown();
    worker.remove_fd(fd);
    assert_eq!(worker.get_remove_count(), 32, "closed queue rejects removal");
}
