// After the commit: is the KERNEL disturbed too, or only the host around it?
//
// WHY THIS EXISTS (2026-10-04, the user's question). The paced commit gate moves `[cb commit]` onto a thread
// that wakes on the slot grid with a realtime intent, so the ENCODE+COMMIT is out of reach of a daemon storm.
// The user's follow-up is the sharp one: "then what about the Metal kernel in the GPU - is IT disturbed by
// system processes too?" The question decides where the next lever is, so it has to be answered by a reading
// rather than by reasoning about GPU scheduling.
//
// The commit is not one instant but three phases, and they fail for different reasons:
//
//   queue  = GPUStartTime  - host commit        <- how long the buffer waited before the GPU picked it up
//   device = GPUEndTime    - GPUStartTime       <- the kernel's OWN execution, the number a daemon "cannot"
//                                                  touch (no CPU timeslice exists on the device)
//   deliver= host wake     - GPUEndTime         <- completion delivery + the waiting thread's wake-up, which
//                                                  IS host scheduling again (the waiter is a pool worker)
//
// and wall = the three of them. Only `device` is execution; `queue` and `deliver` are scheduling, one on each
// side of it. The probe records all three per activation, so:
//
//   * if `device` is flat across regimes while `queue`/`deliver` grow, the kernel is NOT disturbed - the GPU
//     scheduler and the host completion path are, and the fix belongs there (the waiter's declaration, the
//     queue's depth), not in the kernel;
//   * if `device` itself grows, the GPU is being shared with other work and no CPU-side declaration can help.
//
// REGIMES, and what each one is a proxy for:
//   idle   - nothing else running;
//   cpu    - 16 spinners on 14 cores: the regime the other microbenchmarks use (a plain thread's wake-up went
//            to 2567 us max there against 3.2 us with a time constraint);
//   gpu    - 4 concurrent threads of this process committing a LONG kernel (200x the IQ size) in a loop.
//            What it stands for: ANY other party using the device. It is the same process because that is what
//            can be produced deterministically on a bench machine - the scheduler serializes command buffers
//            from different processes on the same device no differently than it serializes these, since the
//            contention is on the GPU and not on our queue. A real system daemon's GPU work shows up as the
//            same kind of busy device.
//
// Build (Objective-C++, no external files):
//   clang++ -O2 -std=c++17 -ObjC++ -fobjc-arc -framework Metal -framework Foundation \
//     -o out/11_metal_device_under_load 11_metal_device_under_load.mm
// Run:
//   out/11_metal_device_under_load [idle|cpu|gpu] [iters]
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <Metal/Metal.h>

#define PERIOD_NS 500000 /* one slot at 30 kHz SCS */
#define IQ_SAMPLES 11520 /* 23.04 Msps x 500 us: one slot of IQ, the buffer the hop really uses */
#define LONG_SAMPLES (IQ_SAMPLES * 200)

static const char* KERNEL_SRC =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void touch(device float2* io [[buffer(0)]], constant uint& n [[buffer(1)]],\n"
    "                  uint i [[thread_position_in_grid]])\n"
    "{\n"
    "  if (i >= n) { return; }\n"
    "  float2 v = io[i];\n"
    "  v.x = v.x * 1.000001f + v.y * 0.000001f;\n"
    "  v.y = v.y * 1.000001f - v.x * 0.000001f;\n"
    "  io[i] = v;\n"
    "}\n";

struct series {
  double* v;
  int     n;
};

static void push(struct series* s, double x)
{
  s->v[s->n++] = x;
}

static int cmp_double(const void* a, const void* b)
{
  const double x = *(const double*)a;
  const double y = *(const double*)b;
  return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

static double pct(struct series* s, double p)
{
  if (s->n == 0) {
    return -1.0;
  }
  qsort(s->v, s->n, sizeof(double), cmp_double);
  int i = (int)(p * (double)(s->n - 1));
  return s->v[i];
}

static double mean_of(struct series* s)
{
  double t = 0;
  for (int i = 0; i < s->n; ++i) {
    t += s->v[i];
  }
  return (s->n != 0) ? t / (double)s->n : -1.0;
}

static void timebase(mach_timebase_info_data_t* tb)
{
  if (tb->denom == 0) {
    mach_timebase_info(tb);
  }
}

static uint64_t mach_ns()
{
  mach_timebase_info_data_t tb = {0, 0};
  timebase(&tb);
  return mach_absolute_time() * tb.numer / tb.denom;
}

/// mach_wait_until() takes a deadline in TIME BASE UNITS, not nanoseconds. The first version handed it
/// nanoseconds and every slot became a 41x wait on this machine's 125/3 timebase - a probe that silently
/// measures the wrong period is worse than one that does not compile.
static uint64_t ns_to_ticks(uint64_t ns)
{
  mach_timebase_info_data_t tb = {0, 0};
  timebase(&tb);
  return ns * tb.denom / tb.numer;
}

/// The GPU timestamps are host times in SECONDS, ALREADY on the mach time base (measured: multiplying by
/// denom/numer again put the two clocks 41.7x apart on this machine, which is the timebase factor itself).
/// The clock-check line in the header is what keeps that honest: the two readings must agree.
static double gpu_seconds_to_ns(double s)
{
  return s * 1e9;
}

static atomic_int spin_stop = 0;
static void*      spinner(void* a)
{
  (void)a;
  while (!atomic_load(&spin_stop)) {
  }
  return NULL;
}

/// The GPU-load arm: a long kernel committed in a loop, so the device is busy with work that is not the
/// measured hop. It does not share the measured command buffers - contention on the device is the point.
struct gpu_load_arg {
  id<MTLCommandQueue>  queue;
  id<MTLComputePipelineState> pipe;
  id<MTLBuffer>        buf;
  unsigned             n;
};

static void* gpu_loader(void* a)
{
  struct gpu_load_arg* g = (struct gpu_load_arg*)a;
  while (!atomic_load(&spin_stop)) {
    @autoreleasepool {
      id<MTLCommandBuffer>         c = [g->queue commandBuffer];
      id<MTLComputeCommandEncoder> e = [c computeCommandEncoder];
      [e setComputePipelineState:g->pipe];
      [e setBuffer:g->buf offset:0 atIndex:0];
      [e setBytes:&g->n length:sizeof(g->n) atIndex:1];
      [e dispatchThreads:MTLSizeMake(g->n, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      [e endEncoding];
      [c commit];
      [c waitUntilCompleted];
    }
  }
  return NULL;
}

int main(int argc, char** argv)
{
  const char* regime = (argc > 1) ? argv[1] : "idle";
  const int   iters  = (argc > 2) ? atoi(argv[2]) : 2000;
  const bool  load_cpu = (strcmp(regime, "cpu") == 0);
  const bool  load_gpu = (strcmp(regime, "gpu") == 0);

  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  if (dev == nil) {
    printf("  NO METAL DEVICE\n");
    return 2;
  }
  NSError* err = nil;
  id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:KERNEL_SRC] options:nil error:&err];
  if (lib == nil) {
    printf("  METAL COMPILE FAILED: %s\n", err ? [[err localizedDescription] UTF8String] : "?");
    return 2;
  }
  id<MTLFunction>              fn   = [lib newFunctionWithName:@"touch"];
  id<MTLComputePipelineState>  pipe = [dev newComputePipelineStateWithFunction:fn error:&err];
  id<MTLCommandQueue>          queue = [dev newCommandQueue];
  if (pipe == nil || queue == nil) {
    printf("  PIPELINE/QUEUE FAILED\n");
    return 2;
  }

  const unsigned nsamp = IQ_SAMPLES;
  id<MTLBuffer>  buf   = [dev newBufferWithLength:(NSUInteger)nsamp * sizeof(float) * 2
                                          options:MTLResourceStorageModeShared];
  // The load arm gets its OWN buffer, sized for what it dispatches. The first version handed the long kernel
  // the measured buffer - 200x more threads than it has elements - and the GPU took the whole device down:
  // every measured command buffer came back `Discarded (victim of GPU error/recovery)`, 0 of 400 samples
  // survived. A load generator that faults the device does not measure contention, it measures the fault.
  id<MTLBuffer> long_buf = [dev newBufferWithLength:(NSUInteger)LONG_SAMPLES * sizeof(float) * 2
                                            options:MTLResourceStorageModeShared];

  pthread_t sp[16];
  int       nsp = 0;
  struct gpu_load_arg gargs[4];
  pthread_t           gth[4];
  int                 ngpu = 0;

  printf("=== regime=%s === (%d activations of 500us, 1 command buffer of %u IQ samples each, %d samples)\n",
         regime,
         iters,
         nsamp,
         iters);
  {
    mach_timebase_info_data_t tb = {0, 0};
    timebase(&tb);
    printf("    timebase: numer=%u denom=%u (ns per tick = %.4f); clock check: mach=%.1f ns vs gpu-derived=%.1f ns\n",
           tb.numer,
           tb.denom,
           (double)tb.numer / (double)tb.denom,
           (double)mach_ns(),
           gpu_seconds_to_ns((double)mach_ns() / 1e9));
  }

  if (load_cpu) {
    for (int i = 0; i < 16; ++i) {
      pthread_create(&sp[nsp++], NULL, spinner, NULL);
    }
  }
  if (load_gpu) {
    for (int i = 0; i < 4; ++i) {
      gargs[i].queue = [dev newCommandQueue];
      gargs[i].pipe  = pipe;
      gargs[i].buf   = long_buf;
      gargs[i].n     = LONG_SAMPLES;
      pthread_create(&gth[ngpu++], NULL, gpu_loader, &gargs[i]);
    }
  }

  struct series wall = {(double*)calloc(iters, sizeof(double)), 0}; // host clock, the cross-check on the split
  struct series q = {(double*)calloc(iters, sizeof(double)), 0};
  struct series d = {(double*)calloc(iters, sizeof(double)), 0};
  struct series w = {(double*)calloc(iters, sizeof(double)), 0};

  setvbuf(stdout, NULL, _IOLBF, 0); // a probe killed by a timeout must still show what it printed
  uint64_t next = mach_absolute_time() + ns_to_ticks(PERIOD_NS);
  for (int i = 0; i < iters; ++i) {
    // The activation is the paced gate's shape: wake on the slot grid, commit ONE buffer, wait for it.
    // The wait is INSIDE the loop here (unlike probe 10, which measures the encode only) because `deliver`
    // is one of the three numbers this probe exists for.
    // RE-SYNC, exactly as the paced executor does: a tick that has already gone by is never one to run. The
    // first version accumulated the period unconditionally, so a late iteration left the phase behind for
    // good and the loop free-ran at ~250us - which made the queue phase (and therefore queue/deliver)
    // incomparable across regimes.
    mach_wait_until(next);
    next += ns_to_ticks(PERIOD_NS);
    if (next <= mach_absolute_time()) {
      next = mach_absolute_time() + ns_to_ticks(PERIOD_NS);
    }

    @autoreleasepool {
      id<MTLCommandBuffer>         c = [queue commandBuffer];
      id<MTLComputeCommandEncoder> e = [c computeCommandEncoder];
      [e setComputePipelineState:pipe];
      [e setBuffer:buf offset:0 atIndex:0];
      [e setBytes:&nsamp length:sizeof(nsamp) atIndex:1];
      [e dispatchThreads:MTLSizeMake(nsamp, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      [e endEncoding];

      // THE HANDLER IS WHAT MAKES THE TIMESTAMPS EXIST. Without one, GPUStartTime/GPUEndTime still READ as
      // numbers and are meaningless: the first version of this probe printed a 176us host wall next to a
      // 172ms GPU-derived total. Metal requires the handler before commit() - the same assertion the engine's
      // arm_gpu_time() documents - so it is installed here, empty, purely to arm them.
      [c addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
      }];
      const uint64_t commit_ns = mach_ns();
      [c commit];
      [c waitUntilCompleted];
      const uint64_t wake_ns = mach_ns();

      if (c.status != MTLCommandBufferStatusCompleted || c.GPUStartTime == 0 || c.GPUEndTime == 0) {
        // Say WHY once. Under the GPU-load arm the first version collected 1 sample out of 800 and the reason
        // was invisible - a skipped sample and a measured zero look the same in a percentile.
        static int explained = 0;
        if (explained < 3) {
          ++explained;
          printf("    [skip] status=%d err=%s start=%.6f end=%.6f\n",
                 (int)c.status,
                 c.error ? [[c.error localizedDescription] UTF8String] : "none",
                 c.GPUStartTime,
                 c.GPUEndTime);
        }
        continue;
      }
      const double start_ns = gpu_seconds_to_ns(c.GPUStartTime);
      const double end_ns   = gpu_seconds_to_ns(c.GPUEndTime);
      if (i < 3) {
        // The UNIT question, answered instead of assumed: consecutive activations are one period apart, so the
        // raw delta between two GPUStartTime readings IS the period expressed in whatever unit Metal uses.
        printf("    [dbg %d] raw_start=%.6f raw_end=%.6f | commit_ns=%llu wake_ns=%llu | host delta=%lld ns\n",
               i,
               c.GPUStartTime,
               c.GPUEndTime,
               (unsigned long long)commit_ns,
               (unsigned long long)wake_ns,
               (long long)(wake_ns - commit_ns));
      }
      // The three phases must ADD UP to the host's own wall reading. When they did not, the GPU timestamps
      // were on another epoch and every phase number was fiction - so the cross-check is printed, not assumed.
      // ALL FOUR IN MICROSECONDS. The first version pushed the three GPU-derived ones in ns and printed them
      // as us, so a real 88 us queue showed up as "76375 us" next to a host wall of 171 us - a 1000x labelling
      // error that made every phase look like fiction. The host wall is the cross-check that catches it.
      push(&wall, (double)(wake_ns - commit_ns) / 1e3);
      push(&q, (start_ns - (double)commit_ns) / 1e3);
      push(&d, (end_ns - start_ns) / 1e3);
      push(&w, ((double)wake_ns - end_ns) / 1e3);
    }
  }

  printf("  wall   us (host commit->wake)  : mean=%7.1f p50=%7.1f p95=%7.1f p99=%7.1f max=%9.1f\n",
         mean_of(&wall), pct(&wall, 0.50), pct(&wall, 0.95), pct(&wall, 0.99), pct(&wall, 1.0));
  printf("  queue  us (commit -> GPU start): mean=%7.1f p50=%7.1f p95=%7.1f p99=%7.1f max=%9.1f\n",
         mean_of(&q), pct(&q, 0.50), pct(&q, 0.95), pct(&q, 0.99), pct(&q, 1.0));
  printf("  device us (GPU start -> end)   : mean=%7.1f p50=%7.1f p95=%7.1f p99=%7.1f max=%9.1f\n",
         mean_of(&d), pct(&d, 0.50), pct(&d, 0.95), pct(&d, 0.99), pct(&d, 1.0));
  printf("  deliver us (GPU end -> wake)   : mean=%7.1f p50=%7.1f p95=%7.1f p99=%7.1f max=%9.1f\n",
         mean_of(&w), pct(&w, 0.50), pct(&w, 0.95), pct(&w, 0.99), pct(&w, 1.0));
  printf("  total  us (commit -> wake)     : mean=%7.1f p50=%7.1f p95=%7.1f p99=%7.1f max=%9.1f\n",
         mean_of(&q) + mean_of(&d) + mean_of(&w),
         pct(&q, 0.50) + pct(&d, 0.50) + pct(&w, 0.50),
         pct(&q, 0.95) + pct(&d, 0.95) + pct(&w, 0.95),
         pct(&q, 0.99) + pct(&d, 0.99) + pct(&w, 0.99),
         pct(&q, 1.0) + pct(&d, 1.0) + pct(&w, 1.0));

  atomic_store(&spin_stop, 1);
  for (int i = 0; i < nsp; ++i) {
    pthread_join(sp[i], NULL);
  }
  for (int i = 0; i < ngpu; ++i) {
    pthread_join(gth[i], NULL);
  }
  return 0;
}
