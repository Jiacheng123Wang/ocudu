// Two questions that fix the P4 parameter rule:
//  (1) if a thread under-declares `computation`, does the kernel throttle it?
//  (2) does declaring period==computation (100% duty) actually steal CPU from other work?
// The spinner counter is the control: it measures what the constraint takes from everyone else.
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ITERS 4000
#define PERIOD_NS 500000
#define BUSY_NS 150000   /* each period really needs 150 us of CPU */
#define TB 24
static const char* ARMS[4] = {
    "no TC           (duty 0%)",
    "TC comp=200us   (duty 40%)",
    "TC comp= 20us   (duty  4%, under-declared 7.5x)",
    "TC comp=500us   (duty 100%, the 2026-09-01 shape)"};
static unsigned COMP[4] = {0, 200, 20, 500};

static atomic_int spin_stop = 0;
static atomic_ulong spin_work = 0;
static void* spinner(void* a) { (void)a; volatile double x = 1.0; unsigned long n = 0;
  while (!atomic_load(&spin_stop)) { x = x * 1.0000001 + 0.1; if ((++n & 0xFFFF) == 0) atomic_fetch_add(&spin_work, 0x10000); }
  atomic_fetch_add(&spin_work, n & 0xFFFF); return NULL; }
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int cmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }

static void* worker(void* arg) {
  int mode = (int)(long)arg;
  if (COMP[mode]) {
    thread_time_constraint_policy_data_t tc = {0};
    tc.period = PERIOD_NS/1000*TB; tc.computation = COMP[mode]*TB; tc.constraint = PERIOD_NS/2/1000*TB; tc.preemptible = 1;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
  }
  double* busy = calloc(ITERS, sizeof(double));
  double* over = calloc(ITERS, sizeof(double));
  uint64_t t0 = now_ns(), next = t0 + PERIOD_NS;
  for (int i = 0; i < ITERS; i++) {
    uint64_t s = now_ns();
    while (now_ns() - s < BUSY_NS) { }              /* the real per-period work */
    busy[i] = (double)(now_ns() - s) / 1000.0;
    mach_wait_until(next * TB / 1000);
    over[i] = (double)((int64_t)(now_ns() - next)) / 1000.0;
    next += PERIOD_NS;
  }
  double wall_ms = (double)(now_ns() - t0) / 1e6;
  qsort(busy, ITERS, sizeof(double), cmp); qsort(over, ITERS, sizeof(double), cmp);
  int starved = 0; for (int i = 0; i < ITERS; i++) if (busy[i] > 2 * BUSY_NS/1000.0) starved++;
  printf("  %-46s busy p50=%7.1f p99=%8.1f max=%9.1f | wait p50=%6.1f p99=%8.1f max=%8.1f | inflated %3d | wall=%7.1fms\n",
         ARMS[mode], busy[ITERS/2], busy[ITERS*99/100], busy[ITERS-1], over[ITERS/2], over[ITERS*99/100], over[ITERS-1], starved, wall_ms);
  free(busy); free(over); return NULL;
}
int main(void) {
  pthread_t sp[32]; int nsp = 16;
  printf("=== 16 spinners on 14 cores; each arm: %d periods of 500us with %d us of real work ===\n", ITERS, BUSY_NS/1000);
  for (int i = 0; i < nsp; i++) pthread_create(&sp[i], NULL, spinner, NULL);
  struct timespec ts = {1, 0}; nanosleep(&ts, NULL);
  for (int m = 0; m < 4; m++) {
    atomic_store(&spin_work, 0); uint64_t w0 = now_ns();
    pthread_t t; pthread_create(&t, NULL, worker, (void*)(long)m); pthread_join(t, NULL);
    double secs = (double)(now_ns() - w0) / 1e9;
    printf("      -> other work done during this arm: %.3f G-iter/s (spinner throughput)\n", atomic_load(&spin_work) / secs / 1e9);
  }
  atomic_store(&spin_stop, 1);
  for (int i = 0; i < nsp; i++) pthread_join(sp[i], NULL);
  return 0;
}
