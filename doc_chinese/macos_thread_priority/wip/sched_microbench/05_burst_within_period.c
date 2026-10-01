// The real question: a burst that EXCEEDS the declared computation but FITS inside the period.
// If the kernel enforces the declared computation, the burst is stretched or the next deadline slips.
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ITERS 3000
#define PERIOD_NS 5000000      /* 5 ms period so a 3 ms burst still fits */
#define NORMAL_NS 150000
#define BURST_NS 3000000
#define BURST_EVERY 100
#define TB 24
/*                       label                          period  comp  cons  burst? */
static const char* LBL[4] = {
  "no TC, burst 3ms/5ms            ",
  "TC 5000/1000/2000, NO burst     ",
  "TC 5000/1000/2000, burst 3ms/5ms (burst >> declared comp)",
  "TC 5000/4000/5000, burst 3ms/5ms (budget covers burst)   "};
static unsigned P[4]={0,5000,5000,5000}, C[4]={0,1000,1000,4000}, K[4]={0,2000,2000,5000};
static int BRST[4]={1,0,1,1};

static atomic_int stop=0;
static void* hog(void* a){(void)a; volatile double x=1.0; while(!atomic_load(&stop)) x=x*1.0000001+0.1; return NULL;}
static uint64_t now_ns(void){ return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int cmp(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return x<y?-1:x>y;}
static void* worker(void* arg){
  int m=(int)(long)arg; const char* rb="unset";
  if(P[m]){ thread_time_constraint_policy_data_t tc={0};
    tc.period=P[m]*TB; tc.computation=C[m]*TB; tc.constraint=K[m]*TB; tc.preemptible=1;
    int kr=(int)thread_policy_set(mach_thread_self(),THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&tc,THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    thread_time_constraint_policy_data_t g={0}; mach_msg_type_number_t n=THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t d=FALSE;
    thread_policy_get(mach_thread_self(),THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&g,&n,&d);
    rb=(kr==0&&!d)?"APPLIED":"REJECTED"; }
  double* w=calloc(ITERS,sizeof(double)); double* bb=calloc(ITERS,sizeof(double)); int nb=0; double bstretch_max=0, bstretch_p50=0;
  double* bs=calloc(ITERS,sizeof(double));
  uint64_t next=now_ns()+PERIOD_NS;
  for(int i=0;i<ITERS;i++){
    int burst = BRST[m] && (i%BURST_EVERY==BURST_EVERY-1);
    uint64_t s=now_ns(); uint64_t need = burst?BURST_NS:NORMAL_NS;
    while(now_ns()-s<need){}
    double spent=(double)(now_ns()-s)/1000.0; bb[i]=spent;
    if(burst) bs[nb++]=spent;
    mach_wait_until(next*TB/1000);
    w[i]=(double)((int64_t)(now_ns()-next))/1000.0;
    next+=PERIOD_NS;
  }
  qsort(w,ITERS,sizeof(double),cmp); qsort(bb,ITERS,sizeof(double),cmp); if(nb) qsort(bs,nb,sizeof(double),cmp);
  if(nb){ bstretch_p50=bs[nb/2]; bstretch_max=bs[nb-1]; }
  int miss=0; for(int i=0;i<ITERS;i++) if(w[i]>100.0) miss++;
  printf("  %-58s [%s] wait p50=%7.1f p99=%8.1f max=%9.1f | deadlines missed >100us: %3d | burst stretched: p50=%7.1f max=%7.1f us\n",
         LBL[m], rb, w[ITERS/2], w[ITERS*99/100], w[ITERS-1], miss, bstretch_p50, bstretch_max);
  free(w); free(bb); free(bs); return NULL; }
int main(void){ pthread_t sp[32];
  printf("=== 16 spinners/14 cores; 5 ms period, 150 us normal work, 3 ms burst every %d periods ===\n", BURST_EVERY);
  for(int i=0;i<16;i++) pthread_create(&sp[i],NULL,hog,NULL); struct timespec ts={1,0}; nanosleep(&ts,NULL);
  for(int m=0;m<4;m++){ pthread_t t; pthread_create(&t,NULL,worker,(void*)(long)m); pthread_join(t,NULL); }
  atomic_store(&stop,1); for(int i=0;i<16;i++) pthread_join(sp[i],NULL); return 0; }
