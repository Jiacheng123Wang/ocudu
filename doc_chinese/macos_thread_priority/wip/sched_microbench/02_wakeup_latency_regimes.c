// Does a Mach time constraint actually protect a periodic thread from CPU interference,
// and what budget does it need?  Reverse arms (baseline/QoS) are built in: if TC == QoS == baseline,
// the mechanism does nothing measurable on this machine and P4 is dead.
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ITERS 8000
#define PERIOD_NS 500000  /* 500 us = one slot at 30 kHz SCS */
#define TB 24             /* mach ticks per microsecond at a 24 MHz timebase */

static const char* ARMS[4] = {"baseline(nothing)", "QoS USER_INTERACTIVE", "TC 1000/100/200us", "TC  500/ 50/100us"};
static atomic_int spin_stop = 0;

static void* spinner(void* a) { volatile double x = 1.0; (void)a; while (!atomic_load(&spin_stop)) x = x * 1.0000001 + 0.1; return NULL; }
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static void apply_tc(unsigned period_us, unsigned comp_us, unsigned cons_us) {
  thread_time_constraint_policy_data_t tc = {0};
  tc.period = period_us * TB; tc.computation = comp_us * TB; tc.constraint = cons_us * TB; tc.preemptible = 1;
  thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}
static const char* regime_readback(void) {
  static char buf[128];
  qos_class_t q = QOS_CLASS_UNSPECIFIED; int rel = -1; pthread_get_qos_class_np(pthread_self(), &q, &rel);
  thread_time_constraint_policy_data_t tc = {0}; mach_msg_type_number_t c = THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t d = FALSE;
  thread_policy_get(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, &c, &d);
  snprintf(buf, sizeof buf, "qos=%u tc=%s(%u/%u/%u)", (unsigned)q, d ? "unset" : "SET", tc.period, tc.computation, tc.constraint);
  return buf;
}
static int cmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }

static void* worker(void* arg) {
  int mode = (int)(long)arg;
  if (mode == 1) pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  if (mode == 2) apply_tc(1000, 100, 200);
  if (mode == 3) apply_tc(500, 50, 100);
  char rb[128]; snprintf(rb, sizeof rb, "%s", regime_readback());

  double* d = calloc(ITERS, sizeof(double));
  uint64_t next = now_ns() + PERIOD_NS;
  for (int i = 0; i < ITERS; i++) {
    mach_wait_until(next * TB / 1000);
    uint64_t got = now_ns();
    d[i] = (double)((int64_t)(got - next)) / 1000.0;
    next += PERIOD_NS;
  }
  qsort(d, ITERS, sizeof(double), cmp);
  double sum = 0; for (int i = 0; i < ITERS; i++) sum += d[i];
  int o100 = 0, o500 = 0, o1000 = 0;
  for (int i = 0; i < ITERS; i++) { if (d[i] > 100) o100++; if (d[i] > 500) o500++; if (d[i] > 1000) o1000++; }
  printf("  %-22s [%-31s] p50=%7.1f p90=%7.1f p99=%8.1f p99.9=%9.1f max=%9.1f us | mean=%6.1f | >100us %4d >500us %4d >1ms %3d\n",
         ARMS[mode], rb, d[ITERS/2], d[ITERS*9/10], d[ITERS*99/100], d[(int)(ITERS*0.999)], d[ITERS-1], sum/ITERS, o100, o500, o1000);
  free(d); return NULL;
}
int main(int argc, char** argv) {
  int hostile = (argc > 1 && !strcmp(argv[1], "hostile"));
  pthread_t sp[32]; int nsp = hostile ? 16 : 0;
  printf("%s (%d spinners on 14 cores, %d periods of %d us)\n", hostile ? "=== HOSTILE ===" : "=== IDLE ===", nsp, ITERS, PERIOD_NS/1000);
  for (int i = 0; i < nsp; i++) pthread_create(&sp[i], NULL, spinner, NULL);
  if (hostile) { struct timespec ts = {1, 0}; nanosleep(&ts, NULL); }
  for (int m = 0; m < 4; m++) { pthread_t t; pthread_create(&t, NULL, worker, (void*)(long)m); pthread_join(t, NULL); }
  atomic_store(&spin_stop, 1);
  for (int i = 0; i < nsp; i++) pthread_join(sp[i], NULL);
  return 0;
}
