// Design rule under test: among time-constrained threads, does a TIGHTER deadline win?
// 12 hog threads (all TC, as the real gNB workers all were in the 2026-09-01 arm) plus one
// measured thread A.  If A's latency stays tiny only when its deadline is tighter than the
// hogs', then UNIFORM parameters are the hazard, not the constraint.
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define ITERS 6000
#define PERIOD_NS 500000
#define TB 24
static const char* LBL[5]={
  "hogs: plain (no TC)                 A: TC 1000/100/200  ",
  "hogs: TC 5000/1000/4000 (loose)     A: TC  500/ 50/100 (TIGHT, differentiated)",
  "hogs: TC  500/ 50/100 (tight)       A: TC  500/ 50/100 (IDENTICAL params, tie)",
  "hogs: TC 5000/1000/4000 (loose)     A: TC 5000/1000/4000 (IDENTICAL params, tie)",
  "hogs: plain (no TC)                 A: TC 5000/1000/4000 (loose)"};
static atomic_int stop=0;
static void tc(unsigned p,unsigned c,unsigned k){ thread_time_constraint_policy_data_t t={0}; t.period=p*TB; t.computation=c*TB; t.constraint=k*TB; t.preemptible=1;
  thread_policy_set(mach_thread_self(),THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&t,THREAD_TIME_CONSTRAINT_POLICY_COUNT); }
/* arm -> (hog params, A params); 0 = no TC */
static unsigned HP[5][3]={{0,0,0},{5000,1000,4000},{500,50,100},{5000,1000,4000},{0,0,0}};
static unsigned AP[5][3]={{1000,100,200},{500,50,100},{500,50,100},{5000,1000,4000},{5000,1000,4000}};
static uint64_t now_ns(void){return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);}
static int cmp(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return x<y?-1:x>y;}
static void* hog(void* a){ int arm=*(int*)a; if(HP[arm][0]) tc(HP[arm][0],HP[arm][1],HP[arm][2]);
  while(!atomic_load(&stop)){ uint64_t s=now_ns(); while(now_ns()-s<4000000ull){} struct timespec ts={1,0}; nanosleep(&ts,NULL);} return NULL; }
static void* measured(void* a){ int arm=*(int*)a; tc(AP[arm][0],AP[arm][1],AP[arm][2]);
  double* d=calloc(ITERS,sizeof(double)); uint64_t next=now_ns()+PERIOD_NS;
  for(int i=0;i<ITERS;i++){ mach_wait_until(next*TB/1000); d[i]=(double)((int64_t)(now_ns()-next))/1000.0; next+=PERIOD_NS; }
  qsort(d,ITERS,sizeof(double),cmp); double sum=0; for(int i=0;i<ITERS;i++) sum+=d[i];
  int o100=0; for(int i=0;i<ITERS;i++) if(d[i]>100) o100++;
  printf("  %-72s p50=%8.1f p99=%9.1f p99.9=%10.1f max=%10.1f | mean=%7.1f >100us %4d\n",
         LBL[arm], d[ITERS/2],d[ITERS*99/100],d[(int)(ITERS*0.999)],d[ITERS-1],sum/ITERS,o100);
  free(d); return NULL; }
int main(void){ printf("=== 12 hog threads + 1 measured thread, all on 14 cores; A wakes every 500 us ===\n");
  for(int arm=0;arm<5;arm++){ static int ai[5]; ai[arm]=arm; pthread_t h[12];
    for(int i=0;i<12;i++) pthread_create(&h[i],NULL,hog,&ai[arm]);
    struct timespec ts={0,200000000}; nanosleep(&ts,NULL);
    pthread_t t; pthread_create(&t,NULL,measured,&ai[arm]); pthread_join(t,NULL);
    atomic_store(&stop,1); for(int i=0;i<12;i++) pthread_join(h[i],NULL); atomic_store(&stop,0); }
  return 0; }
