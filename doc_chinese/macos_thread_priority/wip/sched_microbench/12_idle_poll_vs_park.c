// What does an IDLE lower-PHY worker cost, and how does it wait?  The 10 kHz poll against a condition-variable park.
//
// WHY THIS EXISTS (dev doc 11.49, the user's point). "每个线程10万次/秒的轮询确实有点恐怖，这一点必须消除" - and a
// claim like that has to be a reading, not a shudder. The lower-PHY workers waited for work with
// `pop_blocking_generic` over a `sleep_wait_policy`: try_pop, then std::this_thread::sleep_for(10 us), forever. Two
// things follow and both are measurable in seconds at IDLE, with no radio and no traffic:
//
//   * the LOOP RATE - how many times per second the thread re-checks an empty queue (the "100k" in the claim), and
//   * the COST of that loop - because sleep_for(10us) is a kernel timer request, not a spin: each turn is a syscall
//     and a timer expiry, charged to the process even when the thread has nothing to do. On a machine whose whole
//     problem is that the host is too busy, a thread that never truly parks is part of the load it suffers from.
//
// The two arms are the two shapes the tree can build, reproduced here as the tree implements them
// (include/ocudu/adt/detail/concurrent_queue_helper.h for the poll, include/ocudu/adt/blocking_queue.h for the park):
//
//   poll  - try_pop + sleep_for(10us)                                 <- every lower-PHY worker today
//   park  - mutex + condition variable, woken by the producer's push   <- OCUDU_PHY_BLOCKING_WAIT=1
//
// The idle half pushes NOTHING: that is the case the poll pays for and the park does not. The second half pushes one
// task per slot (500 us) and reports the hand-off each arm gives - the same reading handoff_probe.h takes on air, and
// the reason the poll's hand-off is quantised by its own period while the park's is a wake-up.
//
// build:  cc -O2 -Wall -o out/12_idle_poll_vs_park 12_idle_poll_vs_park.c -lpthread
// run:    ./out/12_idle_poll_vs_park [idle_seconds] [slots]
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define POLL_US 10  /* worker_manager.cpp: the lower-PHY workers' wait_sleep_time */
#define SLOT_US 500 /* one slot at 30 kHz SCS */
#define MAX_LAT 100000

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static int cmp_d(const void* a, const void* b)
{
  double x = *(const double*)a, y = *(const double*)b;
  return x < y ? -1 : x > y;
}

static atomic_int stop_flag = 0;

/* ------------------------------------------------------------------ poll arm */
static atomic_uint_fast64_t poll_turns    = 0; /* turns of the poll loop, i.e. wake-ups at idle */
static atomic_uint_fast64_t push_pending  = 0; /* 1 = the producer pushed a task */
static atomic_uint_fast64_t push_instant  = 0; /* ... at this instant */
static double               poll_lat[MAX_LAT];
static int                  poll_lat_n = 0;

static void* poll_worker(void* arg)
{
  (void)arg;
  while (!atomic_load(&stop_flag)) {
    if (atomic_load(&push_pending) != 0) {
      const uint64_t pushed = atomic_load(&push_instant);
      atomic_store(&push_pending, 0);
      if (pushed != 0 && poll_lat_n < MAX_LAT) {
        poll_lat[poll_lat_n++] = (double)(now_ns() - pushed) / 1000.0;
      }
      continue;
    }
    atomic_fetch_add(&poll_turns, 1);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = POLL_US * 1000};
    nanosleep(&ts, NULL);
  }
  return NULL;
}

/* ------------------------------------------------------------------ park arm */
static pthread_mutex_t park_m            = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  park_cv           = PTHREAD_COND_INITIALIZER;
static uint64_t        park_pushed_ns    = 0; /* guarded by park_m: non-zero means "a task is pending" */
static atomic_uint_fast64_t park_wakes   = 0;
static double               park_lat[MAX_LAT];
static int                  park_lat_n = 0;

static void* park_worker(void* arg)
{
  (void)arg;
  pthread_mutex_lock(&park_m);
  while (!atomic_load(&stop_flag)) {
    if (park_pushed_ns == 0) {
      pthread_cond_wait(&park_cv, &park_m); /* THE point: off the CPU, no timer, no syscall while idle */
      continue;
    }
    const uint64_t pushed = park_pushed_ns;
    park_pushed_ns        = 0;
    pthread_mutex_unlock(&park_m);
    if (park_lat_n < MAX_LAT) {
      park_lat[park_lat_n++] = (double)(now_ns() - pushed) / 1000.0;
    }
    atomic_fetch_add(&park_wakes, 1);
    pthread_mutex_lock(&park_m);
  }
  pthread_mutex_unlock(&park_m);
  return NULL;
}

static double cpu_now_s(void)
{
  struct timespec c;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c);
  return (double)c.tv_sec + (double)c.tv_nsec / 1e9;
}

static void report_lat(const char* arm, double* lat, int n)
{
  if (n == 0) {
    printf("%-5s hand-off: NO SAMPLE (the consumer never saw a push)\n", arm);
    return;
  }
  qsort(lat, n, sizeof(double), cmp_d);
  printf("%-5s hand-off (push -> consumer starts): n=%d p50=%.1fus p90=%.1fus p99=%.1fus max=%.1fus\n",
         arm, n, lat[n / 2], lat[(int)(n * 0.9)], lat[(int)(n * 0.99)], lat[n - 1]);
}

int main(int argc, char** argv)
{
  const double idle_s = (argc > 1) ? atof(argv[1]) : 5.0;
  const int    slots  = (argc > 2) ? atoi(argv[2]) : 400;
  pthread_t    t;

  printf("idle cost of the two wait shapes (NO work at all), then the hand-off each one gives\n");

  /* --- idle: poll --- */
  double c0 = cpu_now_s(), w0 = (double)now_ns();
  pthread_create(&t, NULL, poll_worker, NULL);
  usleep((useconds_t)(idle_s * 1e6));
  double wall = ((double)now_ns() - w0) / 1e9;
  uint64_t turns = atomic_load(&poll_turns);
  atomic_store(&stop_flag, 1);
  pthread_join(t, NULL);
  double cpu = cpu_now_s() - c0;
  printf("poll  idle: %.0f loop turns/s (%.1f MHz of wake-ups), cpu %.3f s over %.1f s wall = %.1f%% of one core\n",
         (double)turns / wall, (double)turns / wall / 1e6, cpu, wall, 100.0 * cpu / wall);

  /* --- idle: park --- */
  atomic_store(&stop_flag, 0);
  c0 = cpu_now_s();
  w0 = (double)now_ns();
  pthread_create(&t, NULL, park_worker, NULL);
  usleep((useconds_t)(idle_s * 1e6));
  wall = ((double)now_ns() - w0) / 1e9;
  atomic_store(&stop_flag, 1);
  pthread_mutex_lock(&park_m);
  pthread_cond_broadcast(&park_cv);
  pthread_mutex_unlock(&park_m);
  pthread_join(t, NULL);
  cpu = cpu_now_s() - c0;
  printf("park  idle: 0 loop turns/s (the thread is off the CPU), cpu %.3f s over %.1f s wall = %.2f%% of one core\n",
         cpu, wall, 100.0 * cpu / wall);

  /* --- hand-off: poll. The producer's cadence is a sleep, deliberately: the push phase against the consumer's poll
     phase is then arbitrary, which is what a data-driven push looks like in the real pipeline. --- */
  atomic_store(&stop_flag, 0);
  poll_lat_n = 0;
  pthread_create(&t, NULL, poll_worker, NULL);
  for (int i = 0; i < slots; ++i) {
    usleep(SLOT_US);
    atomic_store(&push_instant, now_ns());
    atomic_store(&push_pending, 1);
  }
  usleep(2000);
  atomic_store(&stop_flag, 1);
  pthread_join(t, NULL);
  report_lat("poll", poll_lat, poll_lat_n);

  /* --- hand-off: park --- */
  atomic_store(&stop_flag, 0);
  park_lat_n = 0;
  pthread_create(&t, NULL, park_worker, NULL);
  for (int i = 0; i < slots; ++i) {
    usleep(SLOT_US);
    pthread_mutex_lock(&park_m);
    park_pushed_ns = now_ns();
    pthread_mutex_unlock(&park_m);
    pthread_cond_signal(&park_cv);
  }
  usleep(2000);
  atomic_store(&stop_flag, 1);
  pthread_mutex_lock(&park_m);
  pthread_cond_broadcast(&park_cv);
  pthread_mutex_unlock(&park_m);
  pthread_join(t, NULL);
  report_lat("park", park_lat, park_lat_n);

  return 0;
}
