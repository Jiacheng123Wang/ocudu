// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/executors/unique_thread.h"
#include "ocudu/adt/scope_exit.h"
#include "ocudu/adt/static_vector.h"
#include "ocudu/support/macos_compat.h"
#include "fmt/std.h"
#include <cstdio>
#include <mutex>
#include <pthread.h>
#include <sys/types.h>
#include <thread>

using namespace ocudu;

/// Sets thread OS scheduling real-time priority.
static bool thread_set_param(::pthread_t t, os_thread_realtime_priority prio)
{
  sched_param param{};

  param.sched_priority = prio.native();
  if (::pthread_setschedparam(t, prio.native_sched_policy(), &param) != 0) {
    fmt::println(stderr,
                 "Warning: Scheduling priority of thread \"{}\" not changed. Cause: Not enough privileges.",
                 this_thread_name());
    return false;
  }

  return true;
}

static bool thread_set_affinity(::pthread_t t, const os_sched_affinity_bitmask& bitmap, const std::string& name)
{
  // Platform mapping lives in the compat layer: pthread_setaffinity_np on
  // Linux, no-op on macOS (the QoS class and Mach affinity tag are the
  // scheduling mechanisms there).
  return compat::set_thread_affinity(t, bitmap, name);
}

static std::string compute_this_thread_name()
{
  // See Posix pthread_setname_np.
  const uint32_t    MAX_THREAD_NAME_LEN       = 16;
  char              name[MAX_THREAD_NAME_LEN] = {};
  const ::pthread_t tid                       = ::pthread_self();
  if (::pthread_getname_np(tid, name, MAX_THREAD_NAME_LEN)) {
    ::perror("Could not get pthread name");
  }

  return name;
}

static void print_thread_priority(::pthread_t t, const char* tname)
{
  if (t == 0) {
    fmt::println("Error: Trying to print priority of invalid thread handle");
    return;
  }

  // Platform mapping lives in the compat layer (CPU set on Linux, notice on
  // macOS).
  compat::print_thread_affinity_info(t);

  int           policy;
  ::sched_param param;
  int sched_err = ::pthread_getschedparam(t, &policy, &param);
  if (sched_err != 0) {
    fmt::println("error pthread_getschedparam: {}", ::strerror(sched_err));
  }

  const char* p;
  switch (policy) {
    case SCHED_FIFO:
      p = "SCHED_FIFO";
      break;
    case SCHED_RR:
      p = "SCHED_RR";
      break;
    default:
      p = "Other";
      break;
  }

  fmt::println("Thread [{}]: Sched policy is \"{}\". Priority is {}.", tname, p, param.sched_priority);
}

namespace {

/// List of observers of thread creation/deletion. This list may only grow in size.
class unique_thread_observer_list
{
public:
  void add(std::unique_ptr<unique_thread::observer> observer)
  {
    std::lock_guard<std::mutex> lock(mutex);
    observers.emplace_back(std::move(observer));
  }

  /// Called on every thread creation.
  void on_thread_creation()
  {
    // Pre-initialize thread_local variable storing the thread name.
    this_thread_name();

    // Note: we use index-based loop because list of observers may increase (never decrease) throughout the loop.
    std::unique_lock<std::mutex> lock(mutex);
    for (unsigned i = 0; i < observers.size(); ++i) {
      unique_thread::observer* observer = observers[i].get();
      lock.unlock();
      // Call observer without holding the mutex.
      observer->on_thread_creation();
      lock.lock();
    }
  }

  /// Called on every thread destruction.
  void on_thread_destruction()
  {
    std::unique_lock<std::mutex> lock(mutex);
    for (unsigned i = 0; i < observers.size(); ++i) {
      unique_thread::observer* observer = observers[i].get();
      lock.unlock();
      observer->on_thread_destruction();
      lock.lock();
    }
  }

private:
  std::mutex                                            mutex;
  std::vector<std::unique_ptr<unique_thread::observer>> observers;
};

/// Class maintaining the unique thread indexes.
class unique_thread_index_manager
{
  /// Maximum number of threads supported by the application.
  static constexpr unsigned MAX_NOF_THREADS = 256;

public:
  unique_thread_index_manager()
  {
    for (unsigned i = 0; i != MAX_NOF_THREADS; ++i) {
      free_list.push_back(i);
    }
  }

  /// Returns maximum number of threads supported by the application.
  static unsigned get_max_nof_supported_threads() { return MAX_NOF_THREADS; }

  /// Get free identifier.
  unsigned get_free_identifier()
  {
    std::unique_lock<std::mutex> lock(mutex);

    report_error_if_not(!free_list.empty(), "Failed to get a free unique thread identifier");
    unsigned idx = free_list.back();
    free_list.pop_back();

    return idx;
  }

  /// Release used identifier.
  void release_identifier(unsigned id)
  {
    std::unique_lock<std::mutex> lock(mutex);

    ocudu_sanity_check(id < MAX_NOF_THREADS, "Invalid unique thread identifier being released");
    free_list.push_back(id);
  }

private:
  std::mutex                               mutex;
  static_vector<unsigned, MAX_NOF_THREADS> free_list;
};

} // namespace

static unique_thread_index_manager& get_thread_index_manager()
{
  static unique_thread_index_manager thread_index_manager;
  return thread_index_manager;
}

/// Unique index associated with each thread.
thread_local unsigned unique_thread_index;

/// Global unique list of thread lifetime observers.
static unique_thread_observer_list thread_observers;

const os_sched_affinity_bitmask& os_sched_affinity_bitmask::available_cpus()
{
  static os_sched_affinity_bitmask available_cpus_mask = []() {
    os_sched_affinity_bitmask bitmask;
    // Platform mapping lives in the compat layer: the process affinity cpuset
    // on Linux, the first N hardware threads on macOS.
    for (size_t cpu_idx : compat::get_available_cpu_ids()) {
      if (cpu_idx < bitmask.size()) {
        bitmask.cpu_bitset.set(cpu_idx);
      }
    }
    return bitmask;
  }();

  return available_cpus_mask;
}

static_vector<size_t, os_sched_affinity_bitmask::MAX_CPUS>
os_sched_affinity_bitmask::subtract(const os_sched_affinity_bitmask& rhs) const
{
  auto invalid_bitmap = (~rhs.cpu_bitset) & cpu_bitset;
  return invalid_bitmap.get_bit_positions();
}

///////////////////////////////////////

namespace {
/// pthread trampoline: runs the unique_function and releases it.
void* unique_thread_trampoline(void* arg)
{
  auto* callable = static_cast<unique_function<void()>*>(arg);
  (*callable)();
  delete callable;
  return nullptr;
}
} // namespace

unique_thread::thread_handle_impl unique_thread::make_thread(const std::string&               name,
                                                             unique_function<void()>          callable,
                                                             os_thread_realtime_priority      prio,
                                                             const os_sched_affinity_bitmask& cpu_mask)
{
  ::pthread_attr_t attr;
  ::pthread_attr_init(&attr);
  // Platform mapping lives in the compat layer: on macOS the default pthread
  // stack (512 KiB) is too small for the gNB's deep call chains, so the compat
  // layer enlarges it to 16 MiB (Linux keeps its 8 MiB default).
  compat::configure_worker_thread_attributes(attr);

  auto* thread_callable = new unique_function<void()>([name, prio, cpu_mask, callable = std::move(callable)]() {
    std::string fixed_name = name;

    // Truncate the thread name if it exceeds the maximum length.
    static constexpr unsigned MAX_THREADNAME_LEN = 15;
    if (fixed_name.size() > MAX_THREADNAME_LEN) {
      fixed_name.erase(MAX_THREADNAME_LEN, std::string::npos);
      fmt::println("Thread [{}]: Thread name '{}' exceeds {} characters, truncating to '{}'",
                   std::this_thread::get_id(),
                   name,
                   MAX_THREADNAME_LEN,
                   fixed_name);
    }

    // Platform mapping lives in the compat layer: the pthread_setname_np
    // signature differs between Linux (handle + name) and macOS (name only).
    if (not compat::set_thread_name(::pthread_self(), fixed_name.c_str())) {
      ::perror("pthread_setname_np");
      fmt::println("Thread [{}]: Error while setting thread name to {}.", std::this_thread::get_id(), name);
    }

    // Platform mapping lives in the compat layer: on macOS this elevates the
    // QoS class (P-core steering), applies the Mach affinity tag and, for the
    // real-time intent, requests the Mach time constraint. On Linux it is a
    // no-op because the POSIX priority and affinity below are the native
    // mechanisms there.
    compat::apply_worker_thread_scheduling(prio, cpu_mask, name);

    // Set thread OS priority and affinity.
    // Note: TSAN seems to have issues with thread attributes when running as normal user, disable them in that case.
#ifndef HAVE_TSAN
    if (prio != os_thread_realtime_priority::no_realtime() && compat::posix_realtime_priority_is_enforceable()) {
      thread_set_param(::pthread_self(), prio);
    }
    if (cpu_mask.any()) {
      thread_set_affinity(::pthread_self(), cpu_mask, name);
    }
#endif

    // Initialize unique thread index.
    unique_thread_index = get_thread_index_manager().get_free_identifier();
    auto on_thread_destruction =
        make_scope_exit([]() { get_thread_index_manager().release_identifier(unique_thread_index); });

    // Trigger observers.
    thread_observers.on_thread_creation();

    // Run task.
    callable();

    // Trigger observers.
    thread_observers.on_thread_destruction();
  });

  thread_handle_impl handle;
  int                ret = ::pthread_create(&handle.tid, &attr, &unique_thread_trampoline, thread_callable);
  ::pthread_attr_destroy(&attr);
  if (ret != 0) {
    delete thread_callable;
    report_fatal_error("Failed to create thread '{}': {}", name, ::strerror(ret));
  }
  handle.running = true;
  return handle;
}

unsigned ocudu::get_thread_index()
{
  return unique_thread_index;
}

const char* ocudu::this_thread_name()
{
  // Storage of current thread name, set via unique_thread.
  thread_local std::string this_thread_name_val = compute_this_thread_name();
  return this_thread_name_val.c_str();
}

void ocudu::print_this_thread_priority()
{
  print_thread_priority(::pthread_self(), this_thread_name());
}

void unique_thread::print_priority()
{
  print_thread_priority(thread_handle.tid, name.c_str());
}

void unique_thread::add_observer(std::unique_ptr<observer> observer)
{
  thread_observers.add(std::move(observer));
}

unsigned unique_thread::get_max_nof_supported_threads()
{
  return unique_thread_index_manager::get_max_nof_supported_threads();
}
