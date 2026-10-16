// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/signal_handling.h"
#include <gtest/gtest.h>
#include <csignal>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

using namespace ocudu;

namespace {

/// Reports one event to the parent from a signal handler. write(2) is async-signal-safe, so a byte on a pipe is the
/// only thing a handler may use here -- and it is enough: the process is killed before it could say anything else.
void note(int fd)
{
  const char            byte    = 'x';
  [[maybe_unused]] auto written = ::write(fd, &byte, 1);
}

/// Where the cleanup handler reports, as a plain function pointer cannot capture the pipe: the child (and only the
/// child) sets it before registering.
int cleanup_fd = -1;

void note_cleanup(int)
{
  note(cleanup_fd);
}

} // namespace

/// The first signal stops gracefully; every later one must FORCE the exit.
///
/// Measured 2026-10-16, on the operator's side: a gNB whose Ctrl+C did not stop it could not be stopped by pressing
/// Ctrl+C again either, because the graceful handler had already been exchanged away and the later signals returned
/// without doing anything. The only ways out were the five-second timer or `kill -9` from another terminal -- and the
/// operator's report was "只能 kill". A stop that hangs is exactly the situation in which someone presses again, so
/// the second press is the one that must not be a no-op.
TEST(signal_handling_test, a_duplicate_interrupt_is_ignored_and_a_deliberate_one_forces_the_exit)
{
  // Two opposite failures, measured on 2026-10-16, and this test is the contract that keeps both closed:
  //
  //  * a second press did NOTHING, so an operator whose stop hung had only the timer or `kill -9`;
  //  * and once later signals forced the exit, an ORDINARY Ctrl+C started killing working shutdowns: the runner
  //    starts the gNB in the same process group, so the terminal and the runner's trap both send SIGINT -- two
  //    signals, microseconds apart, from one key press. The leg aillr-256qam-ai_1016_1804 died that way and lost
  //    its report block ("THIS LEG HAS NO COMPLETE REPORT").
  //
  // The child below stands in for a gNB whose graceful stop is still running: its handler does not return.
  int started[2];
  int cleaned[2];
  ASSERT_EQ(::pipe(started), 0);
  ASSERT_EQ(::pipe(cleaned), 0);

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0) << "fork failed";
  if (pid == 0) {
    cleanup_fd = cleaned[1];
    // NOT a single sleep(): the duplicate signal interrupts it (nanosleep returns EINTR however the handler was
    // installed), the handler then returns, and the child exits normally -- which the parent's waitpid reads as
    // "the duplicate forced the exit". The stand-in has to keep the graceful handler inside for as long as the
    // test needs, exactly like a stop that is still working.
    ocudu::register_interrupt_signal_handler([](int) {
      for (;;) {
        ::sleep(1);
      }
    });
    ocudu::register_cleanup_signal_handler(note_cleanup);
    note(started[1]);
    std::raise(SIGINT); // enters the handler above and does not return
    ::_exit(0);         // not reached while the handler sleeps
  }

  ::close(started[1]);
  ::close(cleaned[1]);
  char byte = 0;
  ASSERT_EQ(::read(started[0], &byte, 1), 1) << "the child did not install its handlers";

  auto alive = [&]() { return ::kill(pid, 0) == 0; };
  auto wait_for_exit = [&](unsigned tenths) {
    int   status = 0;
    pid_t waited = 0;
    for (unsigned i = 0; i != tenths; ++i) {
      waited = ::waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        return std::pair<pid_t, int>{waited, status};
      }
      ::usleep(100000);
    }
    return std::pair<pid_t, int>{waited, status};
  };

  // 1. A DUPLICATE, inside the grace window: it must be ignored, and the child must still be alive.
  ::usleep(200000);
  ASSERT_EQ(::kill(pid, SIGINT), 0);
  auto [waited_early, status_early] = wait_for_exit(5);
  if (waited_early == pid) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status_early, 0);
    FAIL() << "a duplicate interrupt forced the exit: one Ctrl-C would kill a working shutdown again";
  }
  EXPECT_TRUE(alive()) << "the child must still be stopping gracefully";

  // 2. A DELIBERATE one, after the grace window: it must force, and the cleanup handler must run first.
  ::sleep(2);
  ASSERT_EQ(::kill(pid, SIGINT), 0);
  auto [waited, status] = wait_for_exit(50);
  if (waited != pid) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    FAIL() << "the deliberate second signal did not force the exit; the test had to kill the child itself";
  }
  EXPECT_TRUE(WIFSIGNALED(status)) << "the forced path must not return normally";
  EXPECT_EQ(WTERMSIG(status), SIGKILL) << "the forced path raises SIGKILL";
  EXPECT_EQ(::read(cleaned[0], &byte, 1), 1) << "the cleanup handler did not run on the forced exit";
  ::close(started[0]);
  ::close(cleaned[0]);
}
