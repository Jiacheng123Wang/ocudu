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
TEST(signal_handling_test, the_second_signal_forces_an_exit_that_the_first_one_did_not_finish)
{
  int started[2];
  int cleaned[2];
  ASSERT_EQ(::pipe(started), 0);
  ASSERT_EQ(::pipe(cleaned), 0);

  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0) << "fork failed";
  if (pid == 0) {
    // The child stands in for a gNB whose graceful stop hangs: the handler below is entered by the first signal and
    // does not return.
    cleanup_fd = cleaned[1];
    ocudu::register_interrupt_signal_handler([](int) { ::sleep(60); });
    ocudu::register_cleanup_signal_handler(note_cleanup);
    note(started[1]);
    std::raise(SIGINT); // enters the handler above; returns only if the handler was not installed
    ::_exit(0);         // not reached while the handler sleeps
  }

  ::close(started[1]);
  ::close(cleaned[1]);
  char byte = 0;
  ASSERT_EQ(::read(started[0], &byte, 1), 1) << "the child did not install its handlers";
  ::usleep(200000); // let it enter the handler and consume the first signal
  ASSERT_EQ(::kill(pid, SIGINT), 0) << "the second signal could not be delivered";

  int   status = 0;
  pid_t waited = 0;
  for (unsigned i = 0; i != 100; ++i) {
    waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      break;
    }
    ::usleep(100000);
  }
  if (waited != pid) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    FAIL() << "the second signal did not force the exit; the test had to kill the child itself";
  }

  EXPECT_TRUE(WIFSIGNALED(status)) << "the forced path must not return normally";
  EXPECT_EQ(WTERMSIG(status), SIGKILL) << "the forced path raises SIGKILL";
  // The cleanup handler runs BEFORE the kill: it is what flushes the last readings of the run being stopped.
  EXPECT_EQ(::read(cleaned[0], &byte, 1), 1) << "the cleanup handler did not run on the forced exit";
  ::close(started[0]);
  ::close(cleaned[0]);
}
