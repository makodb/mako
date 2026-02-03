#include "epoll_wrapper.h"

namespace rrr {

void Epoll::Wait() {
	const int max_nev = 100;
	#ifdef USE_KQUEUE
	struct kevent evlist[max_nev];
	struct timespec timeout;
	timeout.tv_sec = 0;
	timeout.tv_nsec = 50 * 1000 * 1000; // 0.05 sec

	int nev = kevent(poll_fd_, nullptr, 0, evlist, max_nev, &timeout);

	for (int i = 0; i < nev; i++) {
	Pollable* poll = (Pollable*) evlist[i].udata;
	verify(poll != nullptr);

	if (evlist[i].filter == EVFILT_READ) {
	poll->handle_read();
	}
	if (evlist[i].filter == EVFILT_WRITE) {
	poll->handle_write();
	}

	// handle error after handle IO, so that we can at least process something
	if (evlist[i].flags & EV_EOF) {
	poll->handle_error();
	}
}

#else
	struct epoll_event evlist[max_nev];
	int timeout = 1; // milli, 0.001 sec
	// int timeout = 0; // busy loop
	struct timespec begin_wait, end_wait, begin_wait_cpu, end_wait_cpu;
		clock_gettime(CLOCK_MONOTONIC_RAW, &begin_wait);
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &begin_wait_cpu);
	int nev = epoll_wait(poll_fd_, evlist, max_nev, timeout);
		
		clock_gettime(CLOCK_MONOTONIC_RAW, &end_wait);
		clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &end_wait_cpu);

	long wait_time = (end_wait.tv_sec - begin_wait.tv_sec)*1000000 + (end_wait.tv_nsec - begin_wait.tv_nsec)/1000;

	if (nev > 0 ) {
		have_count++;
		total_have_time += wait_time;
	} else {
		no_count++;
		total_no_time += wait_time;
	}

	// if ((have_count + no_count) % 1000 == 0)
	//   Log_info("have/no: %lld/%lld, avg wait: %f, have wait: %f, no wait: %f",
	//     have_count, no_count,
	//     (double)(total_have_time + total_no_time) / (have_count + no_count),
	//     (double)total_have_time / have_count,
	//     (double)total_no_time / no_count);

	if(nev != 0){
		/*Log_info("stuck here: %d", nev);
		std::vector<size_t> lengths(nev);
		for (int i = 0; i < nev; i++) {
		Pollable* poll = (Pollable *) evlist[i].data.ptr;
		lengths[i] = poll->content_size();
		}

		std::vector<size_t> args(nev);
		std::iota(args.begin(), args.end(), 0);
		std::sort(args.begin(), args.end(), [&lengths](int i1, int i2) { return lengths[i1] < lengths[i2];} );
		Log_info("not stuck here");*/
	}


	for (int i = 0; i < nev; i++) {
		Pollable* poll = (Pollable *) evlist[i].data.ptr;
		verify(poll != nullptr);

		if (evlist[i].events & EPOLLIN) {
			poll->handle_read();
		}
		if (evlist[i].events & EPOLLOUT) {
			poll->handle_write();
		}

		// handle error after handle IO, so that we can at least process something
		if (evlist[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
			poll->handle_error();
		}
	}
#endif
}

} // namespace rrr
