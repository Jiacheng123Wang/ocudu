// Is a Metal ENCODE+COMMIT stable, and is its tail work or preemption?
//
// WHY THIS EXISTS (2026-10-03, user's question). The on-air fused lane reports
//   [ul_gpu_lane] host: stage entry -> lane commit   median 101.5us  p99 225.5us  max 862.9us
// and the user's objection is the right one: "from reason, the commit time should be very stable".
// That series is WALL, so it cannot tell the two candidates apart:
//
//   * WORK     : a rare path really did more (first hop, allocation, a large TB);
//   * PREEMPTION: the thread was taken off the CPU in the middle of the commit.
//
// The distinction decides a declaration, not just a curiosity: a Mach time constraint's `computation`
// may only cover the WORK. If the 862.9us is mostly preemption then `computation = 300us` is honest and
// the preemption is exactly what the reservation removes; if it is work, 300us UNDER-declares and the
// number has to be ~900us.
//
// WHAT IT DOES, and it is deliberately the same shape as the pipeline's commit thread: at a 500us
// cadence one thread encodes TWO command buffers (the fused hop's two: the estimator weights and the
// merged hop) and commits them, on buffers of the real IQ size (one slot = 11520 samples at 23.04 Msps,
// 2 x float per sample). For each iteration it records
//
//   wall  = start of encode -> commit returned          (what [ul_gpu_lane] measures today)
//   cpu   = thread CPU over the same span               (what the new "host cpu:" series measures)
//   stall = wall - cpu                                  (the preemption inside the activation)
//
// ARMS: nothing / QoS USER_INTERACTIVE / Mach time constraint 500/300/400us (preemptible=1) / SCHED_FIFO.
// The reverse arm is the first one: if the arms do not differ, the reservation buys nothing here and the
// 862.9us is work.
//
// REGIMES: idle, and hostile (16 spinners on 14 cores) - the regime in which the microbenchmarks already
// showed a plain thread's wake-up at 103us p50 / 2567us max against 3.2us for a time constraint.
//
// Build (Objective-C++, no external files):
//   clang++ -O2 -std=c++17 -ObjC++ -fobjc-arc -framework Metal -framework Foundation -o out/10_metal_commit_stability 10_metal_commit_stability.mm
// Run:
//   out/10_metal_commit_stability [idle|hostile]
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#import <Metal/Metal.h>

#define ITERS 3000
#define PERIOD_NS 500000 /* one slot at 30 kHz SCS */
#define TB 24            /* mach ticks per microsecond at a 24 MHz timebase */
#define IQ_SAMPLES 11520 /* 23.04 Msps x 500 us: one slot of IQ, the real buffer size */
#define WAIT_DEPTH 8     /* command buffers in flight before one is drained (the ring's depth) */

static const char* ARMS[4] = {"baseline (nothing)", "QoS USER_INTERACTIVE", "TC 500/300/400us", "SCHED_FIFO 46"};

static atomic_int spin_stop = 0;
static void*      spinner(void* a)
{
  volatile double x = 1.0;
  (void)a;
  while (!atomic_load(&spin_stop)) {
    x = x * 1.0000001 + 0.1;
  }
  return NULL;
}

static uint64_t now_ns(void)
{
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

/// The same reading the pipeline's accounting uses (see this_thread_cpu_ns in thread_sched_snapshot.cpp).
static int64_t thread_cpu_ns(void)
{
  thread_basic_info_data_t basic{};
  mach_msg_type_number_t   count = THREAD_BASIC_INFO_COUNT;
  if (::thread_info(mach_thread_self(), THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&basic), &count) !=
      KERN_SUCCESS) {
    return -1;
  }
  return (static_cast<int64_t>(basic.user_time.seconds) + static_cast<int64_t>(basic.system_time.seconds)) *
             1000000000LL +
         (static_cast<int64_t>(basic.user_time.microseconds) +
          static_cast<int64_t>(basic.system_time.microseconds)) *
             1000LL;
}

static void apply_tc(unsigned period_us, unsigned comp_us, unsigned cons_us)
{
  thread_time_constraint_policy_data_t tc = {0};
  tc.period      = period_us * TB;
  tc.computation = comp_us * TB;
  tc.constraint  = cons_us * TB;
  tc.preemptible = 1;
  thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc,
                    THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

static void apply_fifo(int prio)
{
  struct sched_param p;
  memset(&p, 0, sizeof(p));
  p.sched_priority = prio;
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
}

static const char* regime_readback(void)
{
  static char buf[160];
  qos_class_t q   = QOS_CLASS_UNSPECIFIED;
  int         rel = -1;
  pthread_get_qos_class_np(pthread_self(), &q, &rel);
  thread_time_constraint_policy_data_t tc = {0};
  mach_msg_type_number_t               c  = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
  boolean_t                            d  = FALSE;
  thread_policy_get(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, &c, &d);
  int policy = -1;
  struct sched_param sp;
  pthread_getschedparam(pthread_self(), &policy, &sp);
  snprintf(buf, sizeof buf, "qos=%u tc=%s(%u/%u/%u) policy=%d prio=%d", (unsigned)q, d ? "unset" : "SET", tc.period,
           tc.computation, tc.constraint, policy, sp.sched_priority);
  return buf;
}

static int cmp_d(const void* a, const void* b)
{
  double x = *(const double*)a, y = *(const double*)b;
  return x < y ? -1 : x > y;
}

struct series {
  double* v;
  int     n;
};

static void push(struct series* s, double x)
{
  if (s->n < ITERS) {
    s->v[s->n++] = x;
  }
}

static double pct(const struct series* s, double p)
{
  static double tmp[ITERS];
  memcpy(tmp, s->v, sizeof(double) * s->n);
  qsort(tmp, s->n, sizeof(double), cmp_d);
  int i = (int)((s->n - 1) * p);
  return tmp[i];
}

static double mean_of(const struct series* s)
{
  double sum = 0;
  for (int i = 0; i < s->n; ++i) {
    sum += s->v[i];
  }
  return s->n ? sum / s->n : 0.0;
}

/// One commit thread. `arg` is the arm index.
static void* worker(void* arg)
{
  const int mode = (int)(long)arg;

  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  if (dev == nil) {
    printf("  %-22s NO METAL DEVICE\n", ARMS[mode]);
    return NULL;
  }
  id<MTLCommandQueue> queue = [dev newCommandQueue];

  // A kernel that touches the whole IQ buffer, so the encode is not the only cost and the buffer sizes are
  // the real ones. The SOURCE is inline: the probe must not depend on a built .metallib.
  NSString* src = @"#include <metal_stdlib>\n"
                   "using namespace metal;\n"
                   "kernel void touch(device float2* io [[buffer(0)]], constant uint& n [[buffer(1)]],\n"
                   "                  uint gid [[thread_position_in_grid]]) {\n"
                   "  if (gid < n) { io[gid] = float2(io[gid].x + 1.0f, io[gid].y); }\n"
                   "}\n";
  NSError*  err  = nil;
  id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
  if (lib == nil) {
    printf("  %-22s METAL COMPILE FAILED: %s\n", ARMS[mode], err ? [[err localizedDescription] UTF8String] : "?");
    return NULL;
  }
  id<MTLFunction>             fn   = [lib newFunctionWithName:@"touch"];
  id<MTLComputePipelineState> pipe = [dev newComputePipelineStateWithFunction:fn error:&err];
  if (pipe == nil) {
    printf("  %-22s PIPELINE FAILED\n", ARMS[mode]);
    return NULL;
  }
  const NSUInteger bytes = (NSUInteger)IQ_SAMPLES * sizeof(float) * 2;
  id<MTLBuffer>    buf   = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
  uint32_t         n     = IQ_SAMPLES;

  if (mode == 1) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  } else if (mode == 2) {
    apply_tc(500, 300, 400);
  } else if (mode == 3) {
    apply_fifo(46);
  }
  char rb[160];
  snprintf(rb, sizeof rb, "%s", regime_readback());

  struct series wall  = {(double*)calloc(ITERS, sizeof(double)), 0};
  struct series cpu   = {(double*)calloc(ITERS, sizeof(double)), 0};
  struct series stall = {(double*)calloc(ITERS, sizeof(double)), 0};

  id<MTLCommandBuffer> inflight[WAIT_DEPTH];
  memset(inflight, 0, sizeof inflight);

  uint64_t next = now_ns() + PERIOD_NS;
  for (int i = 0; i < ITERS; ++i) {
    // The activation happens at the slot boundary, not at an arrival: that is the whole point of the plan.
    uint64_t target_tick = next * TB / 1000;
    mach_wait_until(target_tick);
    next += PERIOD_NS;

    // ORDER MATTERS: the two CPU readings must sit INSIDE the wall window, so that `stall = wall - cpu` is
    // the preemption (plus the clock reads) and cannot go negative. The first version read cpu0 BEFORE w0 and
    // cpu1 AFTER w1, which brackets the wall interval with the CPU one and biases every stall reading down by
    // the cost of two clock reads (~5-7us, measured) - enough to hide exactly the small preemptions this probe
    // exists to find.
    const uint64_t w0   = now_ns();
    const int64_t cpu0  = thread_cpu_ns();

    // TWO command buffers per activation, like the fused hop: the estimator's weights and the merged hop.
    for (int cb = 0; cb < 2; ++cb) {
      id<MTLCommandBuffer>         c = [queue commandBuffer];
      id<MTLComputeCommandEncoder> e = [c computeCommandEncoder];
      [e setComputePipelineState:pipe];
      [e setBuffer:buf offset:0 atIndex:0];
      [e setBytes:&n length:sizeof(n) atIndex:1];
      [e dispatchThreads:MTLSizeMake(IQ_SAMPLES, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      [e endEncoding];
      [c commit];
      inflight[cb] = c;
    }

    const int64_t  cpu1 = thread_cpu_ns();
    const uint64_t w1  = now_ns();

    push(&wall, (double)(w1 - w0) / 1e3);
    push(&cpu, (double)(cpu1 - cpu0) / 1e3);
    push(&stall, (double)((int64_t)(w1 - w0) - (cpu1 - cpu0)) / 1e3);

    // Drain, but only to bound how many command buffers are in flight (the ring's depth), and NEVER inside
    // the window above: a wait would put the device's time into the measurement we are trying to isolate.
    if ((i & 7) == 7) {
      for (int k = 0; k < 2; ++k) {
        if (inflight[k] != nil) {
          [inflight[k] waitUntilCompleted];
          inflight[k] = nil;
        }
      }
    }
  }
  for (int k = 0; k < 2; ++k) {
    if (inflight[k] != nil) {
      [inflight[k] waitUntilCompleted];
    }
  }

  int over50 = 0, over100 = 0;
  for (int i = 0; i < stall.n; ++i) {
    if (stall.v[i] > 50.0) {
      ++over50;
    }
    if (stall.v[i] > 100.0) {
      ++over100;
    }
  }
  printf("  %-22s [%-46s]\n", ARMS[mode], rb);
  printf("      wall  us: mean=%7.1f p50=%7.1f p95=%7.1f p99=%7.1f max=%9.1f\n", mean_of(&wall), pct(&wall, 0.50),
         pct(&wall, 0.95), pct(&wall, 0.99), pct(&wall, 1.0));
  printf("      cpu   us: mean=%7.1f p50=%7.1f p95=%7.1f p99=%7.1f max=%9.1f\n", mean_of(&cpu), pct(&cpu, 0.50),
         pct(&cpu, 0.95), pct(&cpu, 0.99), pct(&cpu, 1.0));
  printf("      stall us: mean=%7.1f p50=%7.1f p95=%7.1f p99=%7.1f max=%9.1f | >50us %d >100us %d of %d\n",
         mean_of(&stall), pct(&stall, 0.50), pct(&stall, 0.95), pct(&stall, 0.99), pct(&stall, 1.0), over50, over100,
         stall.n);

  free(wall.v);
  free(cpu.v);
  free(stall.v);
  return NULL;
}

int main(int argc, char** argv)
{
  const int hostile = (argc > 1 && !strcmp(argv[1], "hostile"));
  pthread_t sp[32];
  const int nsp = hostile ? 16 : 0;
  printf("=== %s === (%d spinners on 14 cores, %d activations of 500 us, 2 command buffers each, %d samples)\n",
         hostile ? "HOSTILE" : "IDLE", nsp, ITERS, IQ_SAMPLES);
  if (hostile) {
    for (int i = 0; i < nsp; ++i) {
      pthread_create(&sp[i], NULL, spinner, NULL);
    }
    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
  }
  // ONE thread: this microbenchmark is about one commit thread, not about a pool.
  pthread_t t;
  pthread_create(&t, NULL, worker, (void*)0L);
  pthread_join(t, NULL);
  for (int m = 1; m < 4; ++m) {
    pthread_create(&t, NULL, worker, (void*)(long)m);
    pthread_join(t, NULL);
  }
  atomic_store(&spin_stop, 1);
  for (int i = 0; i < nsp; ++i) {
    pthread_join(sp[i], NULL);
  }
  return 0;
}
