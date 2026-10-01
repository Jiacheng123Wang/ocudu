// ★ ARTIFACT — KEPT ON PURPOSE, DO NOT CITE ITS NUMBERS AS EVIDENCE.  See README §E.
//
// The 2026-09-01 hypothesis, tested: a BURSTY thread that declares a 1 ms budget but
// occasionally needs 3 ms.  Does the kernel punish the burst and delay the next critical wakeup?
//
// The answer this probe appears to give ("yes, 2.02 ms") is ARITHMETIC, not the kernel: the 3 ms
// burst overruns the 1 ms PERIOD, so the deadline passed to mach_wait_until() is already in the
// past and (now - next) is simply the overrun.  The corrected experiment keeps the burst INSIDE
// the period (05_burst_within_period.c) and measures ZERO penalty.  Kept because the artifact's
// direction matched the hypothesis we wanted to believe, and only moving the burst inside the
// period exposed it.
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ITERS 2000
#define PERIOD_NS 1000000
#define NORMAL_NS 150000
#define BURST_NS 3000000
#define BURST_EVERY 200
#define TB 24
static const char* LBL[4] = {
  "no TC                              ",
  "TC 1000/1000/1000 (historical, bounded work)",
  "TC 1000/1000/1000 + 3ms burst every 200 periods (HISTORICAL SHAPE, BURSTY)",
  "TC 1000/4000/4000 (budget >= worst-case burst, 400% declared duty)"};
static unsigned P[4]={0,1000,1000,1000}, C[4]={0,1000,1000,4000}, K[4]={0,1000,1000,4000};

static atomic_int stop=0;
static void* hog(void* a){(void)a; volatile double x=1.0; while(!atomic_load(&stop)) x=x*1.0000001+0.1; return NULL;}
static uint64_t now_ns(void){ return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int cmp(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return x<y?-1:x>y;}
static void* worker(void* arg){
  int m=(int)(long)arg;
  const char* rb="unset";
  if(P[m]){ thread_time_constraint_policy_data_t tc={0};
    tc.period=P[m]*TB; tc.computation=C[m]*TB; tc.constraint=K[m]*TB; tc.preemptible=1;
    int kr=(int)thread_policy_set(mach_thread_self(),THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&tc,THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    thread_time_constraint_policy_data_t g={0}; mach_msg_type_number_t n=THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t d=FALSE;
    thread_policy_get(mach_thread_self(),THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&g,&n,&d);
    rb = (kr==0 && !d) ? "APPLIED" : "REJECTED"; }
  double* w=calloc(ITERS,sizeof(double)); double* after=calloc(ITERS,sizeof(double)); int na=0, burst_max=0;
  uint64_t next=now_ns()+PERIOD_NS;
  for(int i=0;i<ITERS;i++){
    int burst = (m>=2) && (i%BURST_EVERY==BURST_EVERY-1);
    uint64_t s=now_ns(); uint64_t need = burst?BURST_NS:NORMAL_NS;
    while(now_ns()-s<need){} uint64_t spent=(uint64_t)(now_ns()-s);
    if(burst && spent>burst_max) burst_max=(int)spent;
    mach_wait_until(next*TB/1000);
    double dly=(double)((int64_t)(now_ns()-next))/1000.0; w[i]=dly;
    if(burst) after[na++]=dly;
    next+=PERIOD_NS;
  }
  qsort(w,ITERS,sizeof(double),cmp); if(na) qsort(after,na,sizeof(double),cmp);
  printf("  %-62s [%s] wait p50=%9.1f p99=%10.1f max=%10.1f | after-burst p50=%9.1f max=%10.1f | burst stretched to %d us\n",
         LBL[m], rb, w[ITERS/2], w[ITERS*99/100], w[ITERS-1],
         na?after[na/2]:0.0, na?after[na-1]:0.0, burst_max);
  free(w); free(after); return NULL; }
int main(void){ pthread_t sp[32];
  printf("=== 16 spinners / 14 cores; 1 ms period, 150 us work, 3 ms burst every %d periods ===\n", BURST_EVERY);
  for(int i=0;i<16;i++) pthread_create(&sp[i],NULL,hog,NULL); struct timespec ts={1,0}; nanosleep(&ts,NULL);
  for(int m=0;m<4;m++){ pthread_t t; pthread_create(&t,NULL,worker,(void*)(long)m); pthread_join(t,NULL); }
  atomic_store(&stop,1); for(int i=0;i<16;i++) pthread_join(sp[i],NULL); return 0; }
