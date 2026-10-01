// FIFO vs time-constraint vs QoS on the SAME metric (wakeup latency under 2x oversubscription).
// FIFO is what the historical arm effectively had; TC is what the 2026-09-01 arm had.
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
#define ITERS 8000
#define PERIOD_NS 500000
#define TB 24
static const char* ARMS[6]={"baseline (DEFAULT qos)","QoS USER_INTERACTIVE","SCHED_FIFO prio 46 (historical arm)","SCHED_FIFO 46 then TC 1000/100/200","TC 1000/100/200 (2026-09-01 arm)","TC 1000/100/200 then SCHED_FIFO 46"};
static atomic_int stop=0;
static void* hog(void*a){(void)a; volatile double x=1.0; while(!atomic_load(&stop)) x=x*1.0000001+0.1; return NULL;}
static uint64_t now_ns(void){return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);}
static int cmp(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return x<y?-1:x>y;}
static void apply_tc(void){ thread_time_constraint_policy_data_t tc={0}; tc.period=1000*TB; tc.computation=100*TB; tc.constraint=200*TB; tc.preemptible=1;
  thread_policy_set(mach_thread_self(),THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&tc,THREAD_TIME_CONSTRAINT_POLICY_COUNT); }
static int apply_fifo(void){ struct sched_param sp; memset(&sp,0,sizeof sp); sp.sched_priority=46; return pthread_setschedparam(pthread_self(),SCHED_FIFO,&sp); }
static const char* rb(void){ static char b[160]; qos_class_t q=0; int r=-1; pthread_get_qos_class_np(pthread_self(),&q,&r);
  thread_time_constraint_policy_data_t g={0}; mach_msg_type_number_t n=THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t d=FALSE;
  thread_policy_get(mach_thread_self(),THREAD_TIME_CONSTRAINT_POLICY,(thread_policy_t)&g,&n,&d);
  int pol=-1; struct sched_param sp={0}; pthread_getschedparam(pthread_self(),&pol,&sp);
  snprintf(b,sizeof b,"qos=%2u tc=%-5s posix=%s/%d",(unsigned)q, d?"unset":"SET", pol==4?"FIFO":pol==2?"RR":pol==1?"OTHER":"?",sp.sched_priority); return b; }
static void* worker(void* arg){ int m=(int)(long)arg; int rc=0;
  if(m==1) pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
  if(m==2) rc=apply_fifo();
  if(m==3){ rc=apply_fifo(); apply_tc(); }
  if(m==4) apply_tc();
  if(m==5){ apply_tc(); rc=apply_fifo(); }
  double* d=calloc(ITERS,sizeof(double)); uint64_t next=now_ns()+PERIOD_NS;
  for(int i=0;i<ITERS;i++){ mach_wait_until(next*TB/1000); d[i]=(double)((int64_t)(now_ns()-next))/1000.0; next+=PERIOD_NS; }
  qsort(d,ITERS,sizeof(double),cmp); double sum=0; for(int i=0;i<ITERS;i++) sum+=d[i];
  int o100=0; for(int i=0;i<ITERS;i++) if(d[i]>100) o100++;
  printf("  %-38s [%s]%s p50=%7.1f p90=%7.1f p99=%8.1f p99.9=%9.1f max=%9.1f | mean=%6.1f >100us %4d\n",
         ARMS[m], rb(), rc?"":"", d[ITERS/2],d[ITERS*9/10],d[ITERS*99/100],d[(int)(ITERS*0.999)],d[ITERS-1],sum/ITERS,o100);
  free(d); return NULL; }
int main(void){ pthread_t sp[32]; printf("=== HOSTILE: 16 spinners / 14 cores, %d wakeups at 500 us ===\n", ITERS);
  for(int i=0;i<16;i++) pthread_create(&sp[i],NULL,hog,NULL); struct timespec ts={1,0}; nanosleep(&ts,NULL);
  for(int m=0;m<6;m++){ pthread_t t; pthread_create(&t,NULL,worker,(void*)(long)m); pthread_join(t,NULL); }
  atomic_store(&stop,1); for(int i=0;i<16;i++) pthread_join(sp[i],NULL); return 0; }
