// Lost-wakeup hunt for nfs4::RpcServer's job queue (cvmfs/nfs4/rpc.cc), without
// cvmfs. Readers push under queue_lock and notify_one after unlocking; workers
// do a predicate wait on the same mutex. Optional ingredients stress the window
// between a waiter's user-space sequence bump and its kernel block (prepost):
// oversubscription, low worker QoS, fork/posix_spawn, signals, suspension.
//
// A stall is reported when the oldest queued job is older than --stall-ms while
// at least one worker is idle in the wait. The raw pthread_cond_t words are
// printed, then a broadcast is tried and the words are printed again.

#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace {

typedef std::chrono::steady_clock Clock;

struct Options {
  int workers = 16;
  int readers = 3;
  int burst_max = 8;
  int pause_us_max = 2000;   // reader pause between bursts
  int work_us_max = 200;     // worker spin per job
  int hogs = 0;              // busy threads competing for CPUs
  int fork_ms = 0;           // fork()+_exit period, 0 = off
  int spawn_ms = 0;          // posix_spawn(/usr/bin/true) period, 0 = off
  int signal_us = 0;         // SIGUSR1 to a random worker every N us, 0 = off
  int suspend_ms = 0;        // thread_suspend/resume a random worker, 0 = off
  qos_class_t worker_qos = QOS_CLASS_UNSPECIFIED;
  qos_class_t reader_qos = QOS_CLASS_UNSPECIFIED;
  int max_depth = 64;        // readers back off above this, like a client
                             // with a bounded number of outstanding calls
  int stall_ms = 3000;
  int duration_s = 0;        // 0 = forever
  int report_s = 60;
};

Options g_opt;
std::mutex queue_lock;
std::condition_variable queue_cond;
std::deque<Clock::time_point> queue;
std::atomic<bool> stopping(false);
std::atomic<int> idle_workers(0);
std::atomic<uint64_t> n_push(0), n_pop(0), n_wait(0), n_sig(0), n_fork(0);
std::vector<pthread_t> worker_threads;
std::vector<mach_port_t> worker_ports;
std::mutex worker_threads_lock;

void DumpCond(const char *label) {
  const unsigned char *p =
      reinterpret_cast<const unsigned char *>(queue_cond.native_handle());
  uint32_t w[12];
  memcpy(w, p, sizeof(w));
  fprintf(stderr, "%s cv words:", label);
  for (int i = 0; i < 12; ++i) fprintf(stderr, " %08x", w[i]);
  fprintf(stderr, "\n");
}

void Spin(int us) {
  const Clock::time_point end = Clock::now() + std::chrono::microseconds(us);
  while (Clock::now() < end) {
  }
}

void SetQos(qos_class_t q) {
  if (q != QOS_CLASS_UNSPECIFIED) pthread_set_qos_class_self_np(q, 0);
}

void Worker(int id) {
  SetQos(g_opt.worker_qos);
  {
    std::lock_guard<std::mutex> l(worker_threads_lock);
    worker_threads[id] = pthread_self();
    worker_ports[id] = pthread_mach_thread_np(pthread_self());
  }
  std::mt19937 rng(id * 7919 + 1);
  while (true) {
    {
      std::unique_lock<std::mutex> lock(queue_lock);
      idle_workers++;
      queue_cond.wait(lock, [] {
        n_wait++;
        return stopping || !queue.empty();
      });
      idle_workers--;
      if (queue.empty()) return;
      queue.pop_front();
    }
    n_pop++;
    if (g_opt.work_us_max > 0) Spin(rng() % g_opt.work_us_max);
  }
}

void Reader(int id) {
  SetQos(g_opt.reader_qos);
  std::mt19937 rng(id * 104729 + 3);
  while (!stopping) {
    const int burst = 1 + rng() % g_opt.burst_max;
    for (int i = 0; i < burst; ++i) {
      {
        std::lock_guard<std::mutex> lock(queue_lock);
        if (queue.size() >= static_cast<size_t>(g_opt.max_depth)) break;
        queue.push_back(Clock::now());
      }
      queue_cond.notify_one();
      n_push++;
    }
    if (g_opt.pause_us_max > 0)
      std::this_thread::sleep_for(
          std::chrono::microseconds(rng() % g_opt.pause_us_max));
  }
}

void Hog() {
  volatile uint64_t x = 0;
  while (!stopping) x++;
}

void Forker() {
  while (!stopping) {
    const pid_t pid = fork();
    if (pid == 0) _exit(0);
    if (pid > 0) waitpid(pid, NULL, 0);
    n_fork++;
    std::this_thread::sleep_for(std::chrono::milliseconds(g_opt.fork_ms));
  }
}

void Spawner() {
  char arg0[] = "/usr/bin/true";
  char *argv[] = {arg0, NULL};
  while (!stopping) {
    pid_t pid;
    if (posix_spawn(&pid, arg0, NULL, NULL, argv, environ) == 0)
      waitpid(pid, NULL, 0);
    n_fork++;
    std::this_thread::sleep_for(std::chrono::milliseconds(g_opt.spawn_ms));
  }
}

void OnSignal(int) {}

void Signaller() {
  std::mt19937 rng(17);
  while (!stopping) {
    pthread_t t;
    {
      std::lock_guard<std::mutex> l(worker_threads_lock);
      t = worker_threads[rng() % worker_threads.size()];
    }
    if (t) {
      pthread_kill(t, SIGUSR1);
      n_sig++;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(g_opt.signal_us));
  }
}

void Suspender() {
  std::mt19937 rng(23);
  while (!stopping) {
    mach_port_t port;
    {
      std::lock_guard<std::mutex> l(worker_threads_lock);
      port = worker_ports[rng() % worker_ports.size()];
    }
    if (port && thread_suspend(port) == KERN_SUCCESS) {
      std::this_thread::sleep_for(std::chrono::microseconds(rng() % 500));
      thread_resume(port);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(g_opt.suspend_ms));
  }
}

qos_class_t ParseQos(const char *s) {
  if (!strcmp(s, "utility")) return QOS_CLASS_UTILITY;
  if (!strcmp(s, "background")) return QOS_CLASS_BACKGROUND;
  if (!strcmp(s, "user-initiated")) return QOS_CLASS_USER_INITIATED;
  if (!strcmp(s, "user-interactive")) return QOS_CLASS_USER_INTERACTIVE;
  return QOS_CLASS_UNSPECIFIED;
}

void Usage() {
  fprintf(stderr,
          "cvrepro [--workers N] [--readers N] [--burst-max N] [--pause-us N]\n"
          "        [--work-us N] [--hogs N] [--fork-ms N] [--spawn-ms N]\n"
          "        [--signal-us N] [--suspend-ms N] [--worker-qos Q]\n"
          "        [--reader-qos Q] [--max-depth N] [--stall-ms N] [--duration-s N] [--report-s N]\n");
  exit(2);
}

}  // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (i + 1 >= argc) Usage();
    const char *v = argv[++i];
    if (a == "--workers") g_opt.workers = atoi(v);
    else if (a == "--readers") g_opt.readers = atoi(v);
    else if (a == "--burst-max") g_opt.burst_max = atoi(v);
    else if (a == "--pause-us") g_opt.pause_us_max = atoi(v);
    else if (a == "--work-us") g_opt.work_us_max = atoi(v);
    else if (a == "--hogs") g_opt.hogs = atoi(v);
    else if (a == "--fork-ms") g_opt.fork_ms = atoi(v);
    else if (a == "--spawn-ms") g_opt.spawn_ms = atoi(v);
    else if (a == "--signal-us") g_opt.signal_us = atoi(v);
    else if (a == "--suspend-ms") g_opt.suspend_ms = atoi(v);
    else if (a == "--worker-qos") g_opt.worker_qos = ParseQos(v);
    else if (a == "--reader-qos") g_opt.reader_qos = ParseQos(v);
    else if (a == "--max-depth") g_opt.max_depth = atoi(v);
    else if (a == "--stall-ms") g_opt.stall_ms = atoi(v);
    else if (a == "--duration-s") g_opt.duration_s = atoi(v);
    else if (a == "--report-s") g_opt.report_s = atoi(v);
    else Usage();
  }

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = OnSignal;  // no SA_RESTART: interrupted waits see EINTR
  sigaction(SIGUSR1, &sa, NULL);

  worker_threads.assign(g_opt.workers, pthread_t());
  worker_ports.assign(g_opt.workers, MACH_PORT_NULL);
  std::vector<std::thread> threads;
  for (int i = 0; i < g_opt.workers; ++i) threads.emplace_back(Worker, i);
  for (int i = 0; i < g_opt.readers; ++i) threads.emplace_back(Reader, i);
  for (int i = 0; i < g_opt.hogs; ++i) threads.emplace_back(Hog);
  if (g_opt.fork_ms > 0) threads.emplace_back(Forker);
  if (g_opt.spawn_ms > 0) threads.emplace_back(Spawner);
  if (g_opt.signal_us > 0) threads.emplace_back(Signaller);
  if (g_opt.suspend_ms > 0) threads.emplace_back(Suspender);

  const Clock::time_point start = Clock::now();
  Clock::time_point last_report = start;
  int rc = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const Clock::time_point now = Clock::now();
    size_t depth = 0;
    long age_ms = 0;
    {
      std::lock_guard<std::mutex> lock(queue_lock);
      depth = queue.size();
      if (depth > 0)
        age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - queue.front()).count();
    }
    if (depth > 0 && age_ms >= g_opt.stall_ms && idle_workers > 0) {
      fprintf(stderr,
              "STALL after %.0f s: depth %zu, oldest %ld ms, idle workers %d, "
              "push %llu pop %llu wait %llu sig %llu fork %llu\n",
              std::chrono::duration<double>(now - start).count(), depth, age_ms,
              idle_workers.load(), (unsigned long long)n_push.load(),
              (unsigned long long)n_pop.load(), (unsigned long long)n_wait.load(),
              (unsigned long long)n_sig.load(), (unsigned long long)n_fork.load());
      DumpCond("stalled");
      std::this_thread::sleep_for(std::chrono::seconds(2));
      DumpCond("stalled+2s");
      const uint64_t pop0 = n_pop;
      queue_cond.notify_all();
      std::this_thread::sleep_for(std::chrono::seconds(1));
      DumpCond("after broadcast");
      fprintf(stderr, "broadcast %s (pop +%llu)\n",
              n_pop > pop0 ? "recovered" : "did NOT recover",
              (unsigned long long)(n_pop - pop0));
      rc = 3;
      break;
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_report)
            .count() >= g_opt.report_s) {
      fprintf(stderr, "%.0f s: push %llu pop %llu wait %llu sig %llu fork %llu depth %zu\n",
              std::chrono::duration<double>(now - start).count(),
              (unsigned long long)n_push.load(), (unsigned long long)n_pop.load(),
              (unsigned long long)n_wait.load(), (unsigned long long)n_sig.load(),
              (unsigned long long)n_fork.load(), depth);
      last_report = now;
    }
    if (g_opt.duration_s > 0 &&
        std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >=
            g_opt.duration_s)
      break;
  }
  // Leave stalled threads alone; exit without joining.
  fflush(stderr);
  _exit(rc);
}
