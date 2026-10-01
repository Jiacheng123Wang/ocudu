// Authoritative readback: thread_policy_get with get_default=FALSE on input returns the REAL policy.
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <stdio.h>
static void show(const char* w) {
  qos_class_t q = QOS_CLASS_UNSPECIFIED; int rel = -1;
  pthread_get_qos_class_np(pthread_self(), &q, &rel);
  thread_time_constraint_policy_data_t tc = {0};
  mach_msg_type_number_t cnt = THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t dflt = FALSE;
  kern_return_t kr = thread_policy_get(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, &cnt, &dflt);
  int pol = -1; struct sched_param sp = {0}; pthread_getschedparam(pthread_self(), &pol, &sp);
  printf("  %-24s qos=%-3u %-19s | TC: kr=%d %s (%u/%u/%u) | posix=%d/%d\n", w, (unsigned)q,
         q==QOS_CLASS_USER_INTERACTIVE?"USER_INTERACTIVE":q==QOS_CLASS_USER_INITIATED?"USER_INITIATED":"(other)",
         (int)kr, dflt ? "DEFAULT/not-set" : "EXPLICIT", tc.period, tc.computation, tc.constraint, pol, sp.sched_priority);
}
static void set_tc(unsigned p, unsigned c, unsigned k) {
  thread_time_constraint_policy_data_t tc = {0}; tc.period=p; tc.computation=c; tc.constraint=k; tc.preemptible=1;
  printf("  thread_policy_set(TC %u/%u/%u) -> %d\n", p, c, k, (int)thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc, THREAD_TIME_CONSTRAINT_POLICY_COUNT));
}
static void* a(void*) { printf("[A] TC only, then read back:\n"); show("fresh"); set_tc(500000,100000,200000); show("after TC"); return NULL; }
static void* b(void*) { printf("[B] QoS first, then TC:\n"); pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0); show("after set_qos"); set_tc(500000,100000,200000); show("after TC"); return NULL; }
static void* c(void*) { printf("[C] constraint with 1ms/1ms/1ms (the 2026-09-01 shape):\n"); set_tc(24000,24000,24000); show("after TC 1ms/1ms/1ms"); return NULL; }
int main(void){ pthread_t t; pthread_create(&t,NULL,a,NULL); pthread_join(t,NULL); pthread_create(&t,NULL,b,NULL); pthread_join(t,NULL); pthread_create(&t,NULL,c,NULL); pthread_join(t,NULL); return 0; }
