// Pure user-space CPU burner for the P4 disturbance experiment (dev doc macos_thread_priority 10.44).
//
// WHY THIS AND NOT `yes > /dev/null`. The first disturbance used `yes`, and the machine reported 82.55% of
// its CPU in SYSTEM time with `yes` running: `yes` calls write() once per line, so ten of them are a
// syscall hammer rather than a CPU load. That starves the kernel/USB path the radio depends on, and on
// 2026-10-02 it broke the link outright - 942 "Real-time failure in RF" lines in the last 2 MB of the leg,
// CPU idle at 3.56%, and iperf3's uplink traffic at zero, because a UE that cannot decode its grants does
// not transmit.
//
// That is not the regime this experiment is about. A Mach time constraint defends a thread against CPU
// COMPETITION; it cannot defend the radio against a saturated kernel. So the disturbance has to burn CPU
// and nothing else: this loop makes no syscall, touches no memory the kernel tracks, and its whole effect
// is to occupy a runnable slot.
//
// usage: burn <seconds> [duty_percent]
//   duty_percent (default 100) makes the burner alternate between spinning and sleeping, which produces
//   BURSTY competition - closer to what the system daemons do - without changing the average.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char** argv)
{
  const double seconds = (argc > 1) ? atof(argv[1]) : 60.0;
  const int    duty    = (argc > 2) ? atoi(argv[2]) : 100;

  const struct timespec t0 = {0};
  struct timespec       start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  (void)t0;

  // A volatile accumulator keeps the loop from being optimised away without adding any work of its own.
  volatile double sink = 1.0;
  double          x    = 1.0000001;

  struct timespec now;
  for (;;) {
    // One duty cycle: `duty` percent of a 10 ms window spinning, the rest asleep. duty==100 never sleeps.
    if (duty < 100) {
      const int busy_us  = duty * 100;      // 10 ms * duty%
      const int sleep_us = (100 - duty) * 100;
      struct timespec b = {busy_us / 1000000, (busy_us % 1000000) * 1000};
      struct timespec s = {sleep_us / 1000000, (sleep_us % 1000000) * 1000};
      const struct timespec spin_until = start;
      (void)spin_until;
      // spin for busy_us, then sleep
      struct timespec bs;
      clock_gettime(CLOCK_MONOTONIC, &bs);
      const long long spin_end_ns =
          (long long)bs.tv_sec * 1000000000LL + bs.tv_nsec + (long long)busy_us * 1000LL;
      do {
        for (int i = 0; i != 200; ++i) {
          x = x * 1.0000001 + 0.1;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
      } while (((long long)now.tv_sec * 1000000000LL + now.tv_nsec) < spin_end_ns);
      nanosleep(&s, NULL);
    }
    else {
      for (int i = 0; i != 2000; ++i) {
        x = x * 1.0000001 + 0.1;
      }
      clock_gettime(CLOCK_MONOTONIC, &now);
    }
    sink += x;
    if (((long long)now.tv_sec - (long long)start.tv_sec) >= (long long)seconds) {
      break;
    }
  }
  // The accumulator is printed so the compiler cannot prove the loop useless; the value itself is noise.
  if (sink == 12345.6789) {
    fprintf(stderr, "burn: impossible\n");
  }
  return 0;
}
