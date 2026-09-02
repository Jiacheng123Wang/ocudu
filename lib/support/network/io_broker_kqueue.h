// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/blocking_queue.h"
#include "ocudu/support/io/io_broker.h"
#include "ocudu/support/io/unique_fd.h"
#include <atomic>
#include <future>
#include <memory>
#include <unordered_map>
#include <utility>

namespace ocudu {
class task_executor;

/// \brief Implementation of an IO broker using kqueue (macOS equivalent of io_broker_epoll).
class io_broker_kqueue final : public io_broker
{
public:
  explicit io_broker_kqueue(const io_broker_config& config);
  ~io_broker_kqueue() override;

  [[nodiscard]] subscriber
  register_fd(unique_fd fd, task_executor& executor, recv_callback_t handler, error_callback_t err_handler) override;

private:
  /// Event enqueued to be handled in the io_broker thread.
  struct control_event {
    enum class event_type { close_io_broker, register_fd, deregister_fd } type;
    unique_fd           fd;
    int                 raw_fd;
    task_executor*      executor = nullptr;
    recv_callback_t     handler;
    error_callback_t    err_handler;
    std::promise<bool>* completed = nullptr;
  };

  /// Event handler for a file descriptor.
  struct fd_handler {
    fd_handler(task_executor*   executor_,
               recv_callback_t  read_callback_,
               error_callback_t error_callback_,
               unique_fd        fd_) :
      executor(executor_),
      read_callback(std::move(read_callback_)),
      error_callback(std::move(error_callback_)),
      fd(std::move(fd_))
    {
    }

    task_executor*        executor;
    recv_callback_t       read_callback;
    error_callback_t      error_callback;
    std::atomic<unsigned> job_count                  = 0;
    std::atomic<bool>     is_executing_recv_callback = false;
    unique_fd             fd;

    // Determines whether the io_broker has deregistered the event handler from the kqueue.
    bool registered_in_kqueue() const { return static_cast<bool>(read_callback); }
  };

  [[nodiscard]] bool unregister_fd(int fd, std::promise<bool>* complete_notifier) override;

  void thread_loop();

  // Enqueues event to be asynchronously processed by the kqueue thread.
  bool enqueue_event(control_event&& event);

  // Handle events stored in the ctrl event queue.
  void handle_enqueued_events();

  // Handle the registration of a new file descriptor.
  bool handle_fd_registration(unique_fd               fd,
                              const recv_callback_t&  callback,
                              const error_callback_t& err_handler,
                              task_executor*          executor,
                              std::promise<bool>*     complete_notifier);

  // Handle the deregistration of an existing file descriptor.
  bool handle_fd_removal(int                       fd,
                         bool                      io_broker_deregistration_required,
                         std::optional<error_code> kqueue_error,
                         std::promise<bool>*       complete_notifier);

  void stop_impl();

  void rearm_fd(int fd);

  ocudulog::basic_logger& logger;

  // Main kqueue file descriptor.
  unique_fd kqueue_fd;
  // Control pipe used to interrupt the blocking kevent() call when a stop, fd registration, or fd deregistration is
  // requested: the read end is watched by the kqueue, the write end is used to signal it.
  unique_fd ctrl_event_read_fd;
  unique_fd ctrl_event_write_fd;
  int       ctrl_event_raw_fd = -1;

  // Lookup table mapping file descriptors to handlers. Nodes are shared_ptr-owned: the deferred receive callback
  // holds its own reference, so a handler erased while a callback is in flight (e.g. the broker-thread synchronous
  // deregistration fast path) outlives the callback epilogue instead of being written to after free (UAF-001).
  std::unordered_map<int, std::shared_ptr<fd_handler>> event_handler;

  // Shared lifetime flag: the deferred receive callback holds a copy and stops touching the broker (rearm_fd) once
  // this is cleared in the destructor, so a task stranded on an executor that outlives the broker cannot
  // use-after-free the broker object itself (UAF-001 family).
  std::shared_ptr<std::atomic<bool>> lifetime{std::make_shared<std::atomic<bool>>(true)};

  // Queue used to communicate commands to the kqueue broker.
  blocking_queue<control_event> event_queue;

  std::atomic<bool> running{true};
  unique_thread     thread;

  // File descriptors pending to be removed.
  std::vector<std::pair<int, std::promise<bool>*>> pending_fds_to_remove;
};

} // namespace ocudu
