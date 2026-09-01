// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "io_broker_kqueue.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

using namespace ocudu;

// TODO: parameterize
static constexpr unsigned event_queue_size = 32;

/// Tracks the FD that is currently being read in the receive callback.
thread_local int fd_read_in_callback = -1;

/// Sentinel value used by \c fd_read_in_callback to signal that the FD should not be rearmed.
static constexpr int AVOID_FD_REARMING = -2;

io_broker_kqueue::io_broker_kqueue(const io_broker_config& config) :
  logger(ocudulog::fetch_basic_logger("IO-EPOLL")), event_queue(event_queue_size)
{
  pending_fds_to_remove.reserve(16);

  // Init kqueue.
  kqueue_fd = unique_fd{::kqueue()};
  if (not kqueue_fd.is_open()) {
    report_fatal_error("IO broker: failed to create kqueue. error={}", ::strerror(errno));
  }

  // Create the control pipe: the read end is watched by the kqueue, the write end is used to interrupt the blocking
  // kevent() call when a stop, fd registration, or fd deregistration is requested.
  int ctrl_pipe_fds[2];
  if (::pipe(ctrl_pipe_fds) != 0) {
    report_fatal_error("IO broker: failed to create control pipe. error={}", ::strerror(errno));
  }
  for (int fd : {ctrl_pipe_fds[0], ctrl_pipe_fds[1]}) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1 or ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
      report_fatal_error("IO broker: failed to set control pipe to non-blocking. error={}", ::strerror(errno));
    }
  }
  ctrl_event_read_fd  = unique_fd{ctrl_pipe_fds[0]};
  ctrl_event_write_fd = unique_fd{ctrl_pipe_fds[1]};

  auto data_handler  = [this]() { handle_enqueued_events(); };
  auto error_handler = [this](error_code code) {
    logger.error("Error on control pipe. Error code: {}", (int)code);
  };
  ctrl_event_raw_fd = ctrl_event_read_fd.value();
  if (not handle_fd_registration(std::move(ctrl_event_read_fd), data_handler, error_handler, nullptr, nullptr)) {
    report_fatal_error("IO broker: failed to register control pipe. ctrl_event_fd={}", ctrl_event_raw_fd);
  }

  // start thread to handle kqueue events.
  std::promise<void> p;
  std::future<void>  fut = p.get_future();
  thread                 = unique_thread(config.thread_name, config.thread_prio, [this, &p]() {
    running = true;
    p.set_value();
    thread_loop();
  });

  // Wait for the thread to start before returning.
  fut.wait();
}

io_broker_kqueue::~io_broker_kqueue()
{
  // Wait for completion.
  if (thread.running()) {
    enqueue_event(control_event{control_event::event_type::close_io_broker, {}, -1, nullptr, {}, {}, nullptr});
    thread.join();
  }

  stop_impl();

  // Close the kqueue and the control pipe.
  if (not kqueue_fd.close()) {
    logger.error("Failed to close io kqueue: {}", ::strerror(errno));
  }
  if (ctrl_event_write_fd.is_open() and not ctrl_event_write_fd.close()) {
    logger.error("Failed to close io broker control pipe write end: {}", ::strerror(errno));
  }

  logger.info("Closed io_broker");
}

void io_broker_kqueue::rearm_fd(int fd)
{
  struct kevent ev;
  // EV_DISPATCH mirrors the EPOLLONESHOT semantics: the event is delivered once and the filter gets disabled.
  EV_SET(&ev, fd, EVFILT_READ, EV_ENABLE | EV_DISPATCH, 0, 0, nullptr);

  if (::kevent(kqueue_fd.value(), &ev, 1, nullptr, 0, nullptr) == -1) {
    logger.error("Failed to rearm file descriptor: {} (errno={})", fd, ::strerror(errno));
  }
}

// Function is executed in a loop until the destructor is called.
void io_broker_kqueue::thread_loop()
{
  logger.debug("io_broker thread started...");

  while (true) {
    // Process any pending file descriptor removals.
    for (auto it = pending_fds_to_remove.begin(); it != pending_fds_to_remove.end();) {
      if (auto event_it = event_handler.find(it->first); event_it != event_handler.end()) {
        if (event_it->second.job_count.load(std::memory_order_acquire) == 0) {
          handle_fd_removal(it->first, true, std::nullopt, it->second);
          it = pending_fds_to_remove.erase(it);
          continue;
        }
      }
      ++it;
    }

    // Wait for events (null timeout: block indefinitely).
    const uint32_t MAX_EVENTS         = 8;
    struct kevent  events[MAX_EVENTS] = {};
    int            nof_events         = ::kevent(kqueue_fd.value(), nullptr, 0, events, MAX_EVENTS, nullptr);

    // handle event
    if (nof_events == -1) {
      // Note: "Interrupted system call" can happen while debugging.
      if (errno != EINTR) {
        logger.error("kevent(): {}", ::strerror(errno));
      }
      continue;
    }

    for (int i = 0; i < nof_events; ++i) {
      int      fd    = static_cast<int>(events[i].ident);
      uint16_t flags = events[i].flags;
      int64_t  data  = events[i].data;

      if (flags & EV_ERROR) {
        logger.error("fd={}: Error on file descriptor. Error code: {}", fd, data);
        handle_fd_removal(fd, false, io_broker::error_code::error, nullptr);
        continue;
      }

      if ((flags & EV_EOF) and data == 0) {
        // Hang up (e.g., peer closed the connection). Note: some container environments hang up stdin (fd=0) in
        // case of non-interactive sessions.
        logger.warning("fd={}: Hang up on file descriptor.", fd);
        handle_fd_removal(fd, false, io_broker::error_code::hang_up, nullptr);
        continue;
      }

      const auto it = event_handler.find(fd);
      if (it == event_handler.end() or not it->second.registered_in_kqueue()) {
        logger.info("fd={}: Ignoring event. Cause: File descriptor handler not found", fd);
        continue;
      }

      if (fd == ctrl_event_raw_fd) {
        it->second.read_callback();
        // No rearming needed when the stop command has been executed by the callback.
        if (running.load(std::memory_order_relaxed)) {
          rearm_fd(fd);
        } else {
          logger.debug("io_broker thread stopped");
          return;
        }
        continue;
      }

      // Avoid enqueuing the callback if this FD is enqueued for deletion. It will be removed in the next loop
      // iteration.
      if (auto pending_it = std::find_if(pending_fds_to_remove.begin(),
                                         pending_fds_to_remove.end(),
                                         [fd](const auto& elem) { return fd == elem.first; });
          pending_it != pending_fds_to_remove.end()) {
        continue;
      }

      // Make sure that the socket was not re-armed while the callback is still running.
      bool in_callback = false;
      it->second.is_executing_recv_callback.compare_exchange_strong(
          in_callback, true, std::memory_order_acq_rel, std::memory_order_relaxed);
      if (in_callback) {
        logger.error("Trying to defer callback execution, but previous callback for this socket is not finished");
        continue;
      }

      // Increment fd_handler job count before deferring the task.
      it->second.job_count.fetch_add(1, std::memory_order_release);
      if (not it->second.executor->defer([this,
                                          fd,
                                          callback       = &it->second.read_callback,
                                          job_count      = &it->second.job_count,
                                          is_in_callback = &it->second.is_executing_recv_callback]() {
            // Track the current FD that is being read by this thread.
            fd_read_in_callback = fd;
            (*callback)();
            is_in_callback->store(false, std::memory_order_release);
            // Avoid rearming this FD if the callback unregistered it.
            if (fd_read_in_callback != AVOID_FD_REARMING) {
              rearm_fd(fd);
            }
            fd_read_in_callback = -1;
            // Decrement fd_handler job count after deferred task finished.
            job_count->fetch_sub(1, std::memory_order_release);
          })) {
        rearm_fd(fd);
        // Reset is_executing_recv_callback flag and decrement fd_handler job count after task deferring failed.
        it->second.is_executing_recv_callback.store(false, std::memory_order_release);
        it->second.job_count.fetch_sub(1, std::memory_order_release);
        logger.error("Could not enqueue task for processing file descriptor: {}", fd);
      }
    }
  }
}

bool io_broker_kqueue::enqueue_event(control_event&& event)
{
  // Push of an event
  event_queue.push_blocking(std::move(event));

  // Trigger a kqueue event to interrupt the possible kevent() call.
  uint8_t tmp = 1;
  ssize_t ret = ::write(ctrl_event_write_fd.value(), &tmp, sizeof(tmp));
  if (ret == -1) {
    logger.error("Error notifying IO control pipe (errno={})", ::strerror(errno));
  }
  return ret >= 0;
}

void io_broker_kqueue::handle_enqueued_events()
{
  // Drain the control pipe to avoid re-triggering the kqueue.
  uint8_t ignore_buf[64];
  while (::read(ctrl_event_raw_fd, ignore_buf, sizeof(ignore_buf)) > 0) {
  }

  // Keep popping from the event queue.
  control_event ev;
  while (event_queue.try_pop(ev)) {
    // Handle event dequeued.
    switch (ev.type) {
      case control_event::event_type::register_fd:
        // Register new fd and event handler.
        handle_fd_registration(std::move(ev.fd), ev.handler, ev.err_handler, ev.executor, ev.completed);
        break;
      case control_event::event_type::deregister_fd:
        if (auto it = event_handler.find(ev.raw_fd); it != event_handler.end()) {
          // It is safe to directly deregister the FD if there are no tasks reading from it.
          if (it->second.job_count.load(std::memory_order_acquire) == 0) {
            handle_fd_removal(ev.raw_fd, false, std::nullopt, ev.completed);
            break;
          }
          // Enqueue fd deregistration.
          pending_fds_to_remove.emplace_back(ev.raw_fd, ev.completed);
          break;
        }
        // FD may have been already deregistered.
        ev.completed->set_value(false);
        break;
      case control_event::event_type::close_io_broker:
        // Set flag to stop thread loop.
        running.store(false, std::memory_order_release);
        return;
      default:
        report_fatal_error("Unknown event type {}", (int)ev.type);
    }
  }
}

bool io_broker_kqueue::handle_fd_registration(unique_fd               fd,
                                              const recv_callback_t&  handler,
                                              const error_callback_t& err_handler,
                                              task_executor*          executor,
                                              std::promise<bool>*     complete_notifier)
{
  if (event_handler.count(fd.value()) > 0) {
    logger.error("fd={}: Failed to register file descriptor. Cause: File descriptor already registered", fd.value());
    if (complete_notifier != nullptr) {
      complete_notifier->set_value(false);
    }
    return false;
  }

  int raw_fd = fd.value();

  // Add fd to the kqueue. EV_DISPATCH mirrors the EPOLLONESHOT semantics: the event is delivered once and the
  // filter is disabled until rearm_fd() re-enables it.
  struct kevent ev;
  EV_SET(&ev, raw_fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_DISPATCH, 0, 0, nullptr);
  if (::kevent(kqueue_fd.value(), &ev, 1, nullptr, 0, nullptr) == -1) {
    logger.error(
        "fd={}: Failed to register file descriptor. Cause: kevent() failed with \"{}\"", raw_fd, ::strerror(errno));
    if (complete_notifier != nullptr) {
      complete_notifier->set_value(false);
    }
    return false;
  }

  // Register the handler of the fd.
  event_handler.emplace(std::piecewise_construct,
                        std::forward_as_tuple(raw_fd),
                        std::forward_as_tuple(executor, handler, err_handler, std::move(fd)));

  if (complete_notifier != nullptr) {
    complete_notifier->set_value(true);
  }
  return true;
}

bool io_broker_kqueue::handle_fd_removal(int                       fd,
                                         bool                      io_broker_deregistration_required,
                                         std::optional<error_code> kqueue_error,
                                         std::promise<bool>*       complete_notifier)
{
  // The file descriptor must be already registered.
  auto ev_it = event_handler.find(fd);
  if (ev_it == event_handler.end()) {
    // File descriptor not found.
    // Note: It could have been automatically deregistered by the io broker and the subscriber could trigger the
    // removal on its destruction.
    logger.error("fd={}: Failed to deregister file descriptor. Cause: File descriptor not found", fd);
    if (complete_notifier != nullptr) {
      complete_notifier->set_value(false);
    }
    return false;
  }

  // In case the cause for the FD removal was a kqueue error, forward the error to the event handler.
  // Note: We avoid calling the error handling callback in case the FD removal was due to the subscriber
  // close/destruction or due to the io_broker being destroyed.
  if (kqueue_error.has_value()) {
    ev_it->second.error_callback(*kqueue_error);
  }

  // Remove FD from the kqueue. Note: if the file descriptor was already closed, the kernel removed the filter
  // automatically and EV_DELETE is a harmless no-op.
  struct kevent ev;
  EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  ::kevent(kqueue_fd.value(), &ev, 1, nullptr, 0, nullptr);

  logger.debug("fd={}: File descriptor deregistered from kqueue interest list", fd);
  event_handler.erase(ev_it);

  // Notify completion of asynchronous task.
  if (complete_notifier != nullptr) {
    complete_notifier->set_value(true);
  }
  return true;
}

/// Adds a new file descriptor to the kqueue handler. The call is thread-safe and new file descriptors can be added
/// while the kevent() is blocking.
io_broker::subscriber io_broker_kqueue::register_fd(unique_fd        fd,
                                                    task_executor&   executor,
                                                    recv_callback_t  handler,
                                                    error_callback_t err_handler)
{
  if (not fd.is_open()) {
    logger.error("File descriptor registration failed. Cause: Invalid file descriptor value");
    return subscriber{};
  }

  if (not running.load(std::memory_order_acquire)) {
    logger.warning("fd={}: Registration failed. Cause: io_broker is not running", fd.value());
    return subscriber{};
  }

  int raw_fd = fd.value();

  if (thread.is_this_thread()) {
    // Registration from within the kqueue thread.
    if (handle_fd_registration(std::move(fd), handler, err_handler, nullptr, nullptr)) {
      return subscriber{*this, raw_fd};
    }
    return subscriber{};
  }

  report_error_if_not(fd_read_in_callback < 0, "Cannot register a new file descriptor inside the read callback");

  std::promise<bool> p;
  std::future<bool>  fut = p.get_future();

  enqueue_event(control_event{
      control_event::event_type::register_fd, std::move(fd), raw_fd, &executor, handler, err_handler, &p});

  // Wait for the registration to complete.
  if (fut.get()) {
    logger.info("fd={}: Registered file descriptor successfully", raw_fd);
    return subscriber{*this, raw_fd};
  }
  return subscriber{};
}

/// \brief Remove fd from the kqueue handler.
bool io_broker_kqueue::unregister_fd(int fd, std::promise<bool>* complete_notifier)
{
  if (fd < 0) {
    logger.error("fd={}: File descriptor deregistration failed. Cause: Invalid file descriptor value", fd);
    if (complete_notifier) {
      complete_notifier->set_value(false);
    }
    return false;
  }
  if (not running.load(std::memory_order_acquire)) {
    logger.warning("fd={}: Deregistration failed. Cause: io_broker is not running", fd);
    if (complete_notifier) {
      complete_notifier->set_value(false);
    }
    return false;
  }
  if (thread.is_this_thread()) {
    // Deregistration from within the kqueue thread.
    handle_fd_removal(fd, false, std::nullopt, complete_notifier);
    return true;
  }
  // Handle the case of calling unregister from the read callback.
  if (fd_read_in_callback == fd) {
    // No rearming is needed as the FD is going to be being removed and no more callbacks should be called for it.
    fd_read_in_callback = AVOID_FD_REARMING;
    if (complete_notifier) {
      complete_notifier->set_value(true);
      complete_notifier = nullptr;
    }
  }

  if (not enqueue_event(
          control_event{control_event::event_type::deregister_fd, {}, fd, nullptr, {}, {}, complete_notifier})) {
    if (complete_notifier) {
      complete_notifier->set_value(false);
    }
    return false;
  }

  return true;
}

void io_broker_kqueue::stop_impl()
{
  // NOTE: stop_impl() runs after the kqueue thread has been joined, so no further events can arrive. The
  // per-fd job_count may still be non-zero at this point when a deferred callback sits in an executor queue that
  // will never run it again (e.g. a manual_task_worker whose owner stopped popping, or a task worker that already
  // shut down). Waiting for the count to drop - as the Linux epoll broker does - can therefore spin forever and
  // hang the application shutdown; instead, remove every remaining entry unconditionally. Any callback epilogue
  // running on a still-live executor thread after the erase performs the documented latent write to freed memory
  // (see UAF-001 in docs/macos_compat_refactor/phase3_report.md); the shutdown itself must not depend on it.

  // Process any pending file descriptor removals.
  for (auto it = pending_fds_to_remove.begin(); it != pending_fds_to_remove.end();) {
    if (auto event_it = event_handler.find(it->first); event_it != event_handler.end()) {
      handle_fd_removal(it->first, true, std::nullopt, it->second);
      it = pending_fds_to_remove.erase(it);
      continue;
    }
    ++it;
  }

  // Check if there are any alive file descriptors apart from the control event file descriptor.
  if (event_handler.size() > 1) {
    logger.warning("File descriptors are still registered during io broker shutdown.");
  }

  // Deregistering all existing file descriptors.
  for (auto it = event_handler.begin(); it != event_handler.end();) {
    struct kevent ev;
    EV_SET(&ev, it->first, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    ::kevent(kqueue_fd.value(), &ev, 1, nullptr, 0, nullptr);
    it = event_handler.erase(it);
  }

  // Clear event queue.
  event_queue.clear();
  // Clear event handler map.
  event_handler.clear();
}
