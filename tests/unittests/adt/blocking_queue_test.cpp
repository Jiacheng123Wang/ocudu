// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "tests/test_doubles/utils/test_rng.h"
#include "ocudu/adt/blocking_queue.h"
#include "ocudu/support/test_utils.h"
#include <chrono>
#include <gtest/gtest.h>

using namespace ocudu;

TEST(blocking_queue_test, blocking_push_from_main_thread_and_pop_from_another_thread)
{
  unsigned            qsize = test_rng::uniform_int<unsigned>(1, 10000);
  blocking_queue<int> queue(qsize);

  std::atomic<int> count{0};
  std::thread      t([&queue, &count]() {
    while (true) {
      int val = queue.pop_blocking();
      if (queue.is_stopped()) {
        break;
      }
      ASSERT_EQ(val, count);
      count++;
    }
  });

  unsigned nof_objs = test_rng::uniform_int<unsigned>(1, 100000);
  for (unsigned i = 0; i < nof_objs; ++i) {
    queue.push_blocking(i);
  }

  while ((unsigned)count != nof_objs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  queue.stop();
  t.join();
}

TEST(blocking_queue_test, blocking_push_from_another_thread_and_pop_in_main_thread)
{
  std::thread t;

  unsigned            qsize = test_rng::uniform_int<unsigned>(1, 10000);
  blocking_queue<int> queue(qsize);

  unsigned nof_objs = test_rng::uniform_int<unsigned>(1, 100000);
  t                 = std::thread([&queue, nof_objs]() {
    int count = 0;
    while ((unsigned)count != nof_objs and queue.push_blocking(count++)) {
    }
  });

  for (unsigned i = 0; i < nof_objs; ++i) {
    ASSERT_EQ(queue.pop_blocking(), (int)i);
  }

  ASSERT_TRUE(queue.empty());

  queue.stop();
  t.join();
}

TEST(blocking_queue_test, blocking_push_and_pop_in_batches_in_separate_threads)
{
  std::thread t;

  std::vector<int> vec(test_rng::uniform_int<unsigned>(1, 10000));
  for (unsigned i = 0; i != vec.size(); ++i) {
    vec[i] = i;
  }

  blocking_queue<int> queue(test_rng::uniform_int<unsigned>(100, 1000));
  t = std::thread([&queue, &vec]() {
    for (unsigned i = 0; i < vec.size();) {
      unsigned batch_size = test_rng::uniform_int<unsigned>(1, vec.size() - i);
      unsigned n          = queue.push_blocking(span<int>(vec).subspan(i, batch_size));
      EXPECT_LE(n, batch_size);
      i += n;
    }
  });

  std::vector<int> vec2(vec.size());
  for (unsigned i = 0; i < vec.size();) {
    unsigned batch_size = test_rng::uniform_int<unsigned>(1, vec.size() - i);
    unsigned n          = queue.pop_blocking(vec2.begin() + i, vec2.begin() + i + batch_size);
    i += n;
  }

  ASSERT_EQ(vec2, vec);

  queue.stop();
  t.join();
}

// pop_wait_for() is the bounded wait the LOWER PHY's receive path uses when the pool is dry (Q9-A, dev doc
// 6.13): it waits at most the given duration, and it must report the three outcomes apart - SUCCESS when an
// element arrived, TIMEOUT when the duration passed with none, and FAILED when the queue was stopped - because
// the caller's loop reaps and asks again on TIMEOUT and must give up (returning the same null buffer
// pop_blocking() would) on FAILED. Nothing else in the tree used the call before that loop, so this is where
// its contract is pinned.
TEST(blocking_queue_test, pop_wait_for_reports_success_timeout_and_stop)
{
  blocking_queue<int> queue(4);

  // SUCCESS: an element is already there, and the wait costs nothing.
  ASSERT_TRUE(queue.try_push(7));
  int          value = 0;
  const auto   deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  const auto   success  = queue.pop_wait_for(value, std::chrono::milliseconds(50));
  ASSERT_EQ(success, decltype(success)::success);
  ASSERT_EQ(value, 7);
  ASSERT_LT(std::chrono::steady_clock::now(), deadline);

  // TIMEOUT: empty, and the call returns after its own bound rather than blocking forever.
  const auto t0      = std::chrono::steady_clock::now();
  const auto timeout = queue.pop_wait_for(value, std::chrono::milliseconds(20));
  const auto waited  = std::chrono::steady_clock::now() - t0;
  ASSERT_EQ(timeout, decltype(timeout)::timeout);
  ASSERT_GE(waited, std::chrono::milliseconds(20));

  // FAILED: a stopped queue releases the waiter with "failed" (what the receive path turns into the null
  // buffer), and it does so well before the bound.
  queue.stop();
  const auto stopped = queue.pop_wait_for(value, std::chrono::seconds(10));
  ASSERT_EQ(stopped, decltype(stopped)::failed);
  ASSERT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));
}
