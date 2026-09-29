// netdelay.c -- LD_PRELOAD shim that injects Jetpack's WAN_DELAY_MS.
//
// Jetpack (src/deptran/communicator.h:15-46, raft/commo.cc:38,112,179) makes
// every outbound Raft request wait WAN_DELAY_MS before it is sent; replies
// are not delayed. This shim reproduces that as a link delay, identically for
// every raft_bench build, without touching their Raft or RPC code:
//
//   * a socket this process connect()s (an outbound, request-carrying
//     connection) is marked;
//   * bytes written to a marked socket (send, write, writev, sendmsg) are
//     accepted at once and handed to one helper thread, which writes them
//     WAN_DELAY_MS later, in order;
//   * accepted sockets (the server side, which carries replies) and reads are
//     untouched.
//
// So a request reaches its peer WAN_DELAY_MS late and its reply comes straight
// back: the injected round trip is WAN_DELAY_MS, as in Jetpack's Raft. The
// sender is never blocked (unlike Jetpack's coroutine Wait), so the delay does
// not serialize one arm's send loop differently from another's.
//
// Build: cc -O2 -shared -fPIC -o libnetdelay.so netdelay.c -ldl -lpthread
// Use:   WAN_DELAY_MS=20 LD_PRELOAD=/path/libnetdelay.so raft_bench ...
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define MAX_FDS 65536

typedef struct chunk {
  struct chunk* next;
  int fd;
  uint32_t gen;      // the fd's generation when queued; a reused fd drops stale bytes
  uint64_t due_ns;
  size_t len, off;
  char data[];
} chunk;

static int (*real_connect)(int, const struct sockaddr*, socklen_t);
static ssize_t (*real_send)(int, const void*, size_t, int);
static ssize_t (*real_write)(int, const void*, size_t);
static ssize_t (*real_writev)(int, const struct iovec*, int);
static ssize_t (*real_sendmsg)(int, const struct msghdr*, int);
static int (*real_close)(int);

static uint64_t g_delay_ns;
static unsigned char g_marked[MAX_FDS];
static uint32_t g_gen[MAX_FDS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv;
static chunk* g_head;
static chunk* g_tail;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static unsigned long long g_queued_chunks, g_sent_bytes;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

static void* pump(void* arg);

static void init_once(void) {
  real_connect = dlsym(RTLD_NEXT, "connect");
  real_send = dlsym(RTLD_NEXT, "send");
  real_write = dlsym(RTLD_NEXT, "write");
  real_writev = dlsym(RTLD_NEXT, "writev");
  real_sendmsg = dlsym(RTLD_NEXT, "sendmsg");
  real_close = dlsym(RTLD_NEXT, "close");
  const char* v = getenv("WAN_DELAY_MS");
  g_delay_ns = v ? (uint64_t) (atof(v) * 1e6) : 0;
  pthread_condattr_t a;
  pthread_condattr_init(&a);
  pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  pthread_cond_init(&g_cv, &a);
  if (g_delay_ns > 0) {
    pthread_t t;
    pthread_create(&t, NULL, pump, NULL);
    pthread_detach(t);
    fprintf(stderr, "[netdelay] WAN_DELAY_MS=%s: outbound (connect()ed) sockets delayed %.3f ms\n",
            v, g_delay_ns / 1e6);
  }
}

static inline void init(void) { pthread_once(&g_once, init_once); }

static inline int is_marked(int fd) {
  return g_delay_ns > 0 && fd >= 0 && fd < MAX_FDS &&
         __atomic_load_n(&g_marked[fd], __ATOMIC_ACQUIRE);
}

static void enqueue(int fd, const struct iovec* iov, int n) {
  size_t len = 0;
  for (int i = 0; i < n; ++i) len += iov[i].iov_len;
  if (len == 0) return;
  chunk* c = malloc(sizeof(chunk) + len);
  if (!c) abort();
  c->next = NULL;
  c->fd = fd;
  c->len = len;
  c->off = 0;
  size_t p = 0;
  for (int i = 0; i < n; ++i) {
    memcpy(c->data + p, iov[i].iov_base, iov[i].iov_len);
    p += iov[i].iov_len;
  }
  c->due_ns = now_ns() + g_delay_ns;
  pthread_mutex_lock(&g_mu);
  c->gen = g_gen[fd];
  if (g_tail) g_tail->next = c; else g_head = c;
  g_tail = c;
  ++g_queued_chunks;
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_mu);
}

// The constant delay keeps the queue in due order, so one FIFO suffices.
static void* pump(void* arg) {
  (void) arg;
  pthread_mutex_lock(&g_mu);
  for (;;) {
    while (!g_head) pthread_cond_wait(&g_cv, &g_mu);
    chunk* c = g_head;
    uint64_t t = now_ns();
    if (c->due_ns > t) {
      struct timespec ts = {(time_t) (c->due_ns / 1000000000ull), (long) (c->due_ns % 1000000000ull)};
      pthread_cond_timedwait(&g_cv, &g_mu, &ts);
      continue;
    }
    int drop = c->gen != g_gen[c->fd] || !g_marked[c->fd];
    while (!drop && c->off < c->len) {
      ssize_t r = real_send(c->fd, c->data + c->off, c->len - c->off, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (r > 0) {
        c->off += (size_t) r;
        g_sent_bytes += (unsigned long long) r;
      } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN || errno == EINTR)) {
        if (now_ns() - c->due_ns > 5000000000ull) { drop = 1; break; }  // never connected
        // Wait for room without holding the lock (close() must not block).
        int fd = c->fd;
        pthread_mutex_unlock(&g_mu);
        struct pollfd pfd = {fd, POLLOUT, 0};
        poll(&pfd, 1, 1);
        pthread_mutex_lock(&g_mu);
        if (c->gen != g_gen[c->fd] || !g_marked[c->fd]) drop = 1;
      } else {
        drop = 1;  // peer gone; the application sees it on its own reads
      }
    }
    g_head = c->next;
    if (!g_head) g_tail = NULL;
    free(c);
  }
  return NULL;
}

int connect(int fd, const struct sockaddr* addr, socklen_t len) {
  init();
  int r = real_connect(fd, addr, len);
  int saved = errno;
  if (g_delay_ns > 0 && fd >= 0 && fd < MAX_FDS && addr && addr->sa_family == AF_INET &&
      (r == 0 || saved == EINPROGRESS)) {
    pthread_mutex_lock(&g_mu);
    ++g_gen[fd];
    __atomic_store_n(&g_marked[fd], 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_mu);
  }
  errno = saved;
  return r;
}

ssize_t send(int fd, const void* buf, size_t len, int flags) {
  init();
  if (is_marked(fd)) {
    struct iovec v = {(void*) buf, len};
    enqueue(fd, &v, 1);
    return (ssize_t) len;
  }
  return real_send(fd, buf, len, flags);
}

ssize_t write(int fd, const void* buf, size_t len) {
  init();
  if (is_marked(fd)) {
    struct iovec v = {(void*) buf, len};
    enqueue(fd, &v, 1);
    return (ssize_t) len;
  }
  return real_write(fd, buf, len);
}

ssize_t writev(int fd, const struct iovec* iov, int n) {
  init();
  if (is_marked(fd)) {
    ssize_t len = 0;
    for (int i = 0; i < n; ++i) len += (ssize_t) iov[i].iov_len;
    enqueue(fd, iov, n);
    return len;
  }
  return real_writev(fd, iov, n);
}

ssize_t sendmsg(int fd, const struct msghdr* m, int flags) {
  init();
  if (is_marked(fd) && m->msg_name == NULL && m->msg_controllen == 0) {
    ssize_t len = 0;
    for (size_t i = 0; i < m->msg_iovlen; ++i) len += (ssize_t) m->msg_iov[i].iov_len;
    enqueue(fd, m->msg_iov, (int) m->msg_iovlen);
    return len;
  }
  return real_sendmsg(fd, m, flags);
}

int close(int fd) {
  init();
  if (g_delay_ns > 0 && fd >= 0 && fd < MAX_FDS) {
    pthread_mutex_lock(&g_mu);
    if (g_marked[fd]) {
      __atomic_store_n(&g_marked[fd], 0, __ATOMIC_RELEASE);
      ++g_gen[fd];  // queued bytes for the old socket are dropped
    }
    pthread_mutex_unlock(&g_mu);
  }
  return real_close(fd);
}

__attribute__((destructor)) static void report(void) {
  if (g_delay_ns > 0 && g_queued_chunks > 0) {
    fprintf(stderr, "[netdelay] %llu chunks queued, %llu bytes sent late\n", g_queued_chunks,
            g_sent_bytes);
  }
}
