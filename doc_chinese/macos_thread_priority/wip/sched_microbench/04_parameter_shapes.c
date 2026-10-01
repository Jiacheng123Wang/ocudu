// Which parameter SHAPE is safe?  All arms do 150 us of real work per period.
// The historical shape (period==computation==constraint) is the suspect.
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
#define BUSY_NS 150000
#define TB 24
/*                       label                       period  comp  cons  preemptible */
static const char* LBL[6] = {
  "no TC",
  "TC 1000/200/400  (calibrated, slack)",
  "TC 1000/1000/1000 (HISTORICAL 2026-09-01 shape)",
  "TC 1000/1000/400  (100% duty, constraint<comp)",
  "TC  500/200/400  (calibrated at slot period)",
  "TC  500/500/500  (historical shape at 500us)"};
static unsigned P[6] = {0,1000,1000,1000,500,500}, C[6] = {0,200,1000,1000,200,500}, K[6] = {0,400,1000,400,400,500};

static atomic_int stop = 0;
static void* hog(void* a) { (void)a; volatile double x = 1.0; while (!atomic_load(&stop)) x = x*1.0000001+0.1; return NULL; }
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int cmp(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x<y?-1:x>y; }
static void* worker(void* arg) {
  int m = (int)(long)arg;
  if (P[m]) { thread_time_constraint_policy_data_t tc={0};
    tc.period=P[m]*TB; tc.computation=C[m]*TB; tc.constraint=K[m]*TB; tc.preemptible=1;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, THREAD_TIME_CONSTRAINT_POLICY_COUNT); }
  double* w = calloc(ITERS,sizeof(double)); double* b = calloc(ITERS,sizeof(double));
  uint64_t t0=now_ns(), next=t0+P[m]*1000ull;
  for (int i=0;i<ITERS;i++){ uint64_t s=now_ns(); while(now_ns()-s<BUSY_NS){} b[i]=(double)(now_ns()-s)/1000.0;
    mach_wait_until(next*TB/1000); w[i]=(double)((int64_t)(now_ns()-next))/1000.0; next+=P[m]*1000ull; }
  double wall=(double)(now_ns()-t0)/1e6; qsort(w,ITERS,sizeof(double),cmp); qsort(b,ITERS,sizeof(double),cmp);
  printf("  %-44s wait p50=%9.1f p99=%10.1f max=%10.1f | busy p99=%7.1f max=%8.1f | wall=%8.1fms (%d periods)\n",
         LBL[m], w[ITERS/2], w[ITERS*99/100], w[ITERS-1], b[ITERS*99/100], b[ITERS-1], wall, ITERS);
  free(w); free(b); return NULL; }
int main(void){ pthread_t sp[32]; printf("=== 16 spinners / 14 cores; 150 us of real work per period ===\n");
  for(int i=0;i<16;i++) pthread_create(&sp[i],NULL,hog,NULL); struct timespec ts={1,0}; nanosleep(&ts,NULL);
  for(int m=0;m<6;m++){ pthread_t t; pthread_create(&t,NULL,worker,(void*)(long)m); pthread_join(t,NULL); }
  atomic_store(&stop,1); for(int i=0;i<16;i++) pthread_join(sp[i],NULL); return 0; }
