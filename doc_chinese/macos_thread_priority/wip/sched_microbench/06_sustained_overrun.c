// Does a SUSTAINED violation get punished?  Declared computation = 1000 us; actual per-period
// work varies.  This is the 2026-09-01 candidate mechanism: every worker declaring 1 ms while
// FLOW_PROBES=ON pushed real per-slot CPU above it.
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
#define PERIOD_NS 5000000
#define TB 24
static const char* LBL[6]={"TC 5000/1000/2000, work  900us ( 90% of declared)","TC 5000/1000/2000, work 1500us (150% of declared, misses constraint)","TC 5000/1000/2000, work 3000us (300% of declared)","TC 5000/1000/5000, work 3000us (constraint covers it)","no TC,            work 3000us","TC 5000/1000/2000, work 4600us (92% of period)"};
static unsigned C[6]={1000,1000,1000,1000,0,1000}, K[6]={2000,2000,2000,5000,0,2000};
static unsigned WORK[6]={900,1500,3000,3000,3000,4600};
static atomic_int stop=0;
static void* hog(void*a){(void)a; volatile double x=1.0; while(!atomic_load(&stop)) x=x*1.0000001+0.1; return NULL;}
static uint64_t now_ns(void){return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);}
static int cmp(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return x<y?-1:x>y;}
static void* worker(void* arg){ int m=(int)(long)arg;
  if(C[m]){ thread_time_constraint_policy_data_t tc={0}; tc.period=PERIOD_NS/1000*TB; tc.computation=C[m]*TB; tc.constraint=K[m]*TB; tc.preemptible=1;
    thread_policy_set(mach_thread_self(),THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&tc,THREAD_TIME_CONSTRAINT_POLICY_COUNT); }
  double* w=calloc(ITERS,sizeof(double)); double* b=calloc(ITERS,sizeof(double));
  uint64_t next=now_ns()+PERIOD_NS;
  for(int i=0;i<ITERS;i++){ uint64_t s=now_ns(); while(now_ns()-s<WORK[m]*1000ull){} b[i]=(double)(now_ns()-s)/1000.0;
    mach_wait_until(next*TB/1000); w[i]=(double)((int64_t)(now_ns()-next))/1000.0; next+=PERIOD_NS; }
  qsort(w,ITERS,sizeof(double),cmp); qsort(b,ITERS,sizeof(double),cmp);
  int miss=0; for(int i=0;i<ITERS;i++) if(w[i]>100.0) miss++;
  printf("  %-62s wait p50=%8.1f p99=%9.1f max=%9.1f | busy p50=%7.1f p99=%7.1f | late>100us %4d\n",
         LBL[m], w[ITERS/2], w[ITERS*99/100], w[ITERS-1], b[ITERS/2], b[ITERS*99/100], miss);
  free(w); free(b); return NULL; }
int main(void){ pthread_t sp[32]; printf("=== 16 spinners/14 cores; 5 ms period, declared computation 1000 us, SUSTAINED work ===\n");
  for(int i=0;i<16;i++) pthread_create(&sp[i],NULL,hog,NULL); struct timespec ts={1,0}; nanosleep(&ts,NULL);
  for(int m=0;m<6;m++){ pthread_t t; pthread_create(&t,NULL,worker,(void*)(long)m); pthread_join(t,NULL); }
  atomic_store(&stop,1); for(int i=0;i<16;i++) pthread_join(sp[i],NULL); return 0; }
