/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unknwn.h>

#include "util_error.hpp"

namespace dxmt {

/**
 * \brief Thread priority
 */
enum class ThreadPriority : int32_t {
  Normal,
  Lowest,
};

#ifdef _WIN32
/**
 * \brief Thread that skips DLL_THREAD_ATTACH processing
 *
 * Uses NtCreateThreadEx with THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH
 * to create threads that bypass Wine's loader_section acquisition for
 * DLL notifications. This prevents deadlocks when Metal framework
 * threads contend with Wine's loader lock during DLL initialization.
 */
class unnotified_thread {
public:
  unnotified_thread() {}

  explicit unnotified_thread(std::function<void()> &&func)
      : m_proc(new std::function<void()>(std::move(func))) {
    // NtCreateThreadEx with THREAD_CREATE_FLAGS_SKIP_THREAD_ATTACH (0x2)
    // Declare locally to avoid winternl.h conflicts
    using NtCreateThreadEx_t = LONG(WINAPI *)(
        HANDLE *, ACCESS_MASK, void *, HANDLE,
        DWORD(WINAPI *)(void *), void *,
        ULONG, ULONG_PTR, SIZE_T, SIZE_T, void *);
    static auto pNtCreateThreadEx = (NtCreateThreadEx_t)
        ::GetProcAddress(::GetModuleHandleA("ntdll.dll"), "NtCreateThreadEx");
    LONG status = pNtCreateThreadEx(
        &m_handle, THREAD_ALL_ACCESS, nullptr, GetCurrentProcess(),
        threadProc, m_proc,
        0x2 /* SKIP_THREAD_ATTACH */, 0, 0x100000, 0x100000, nullptr);
    if (status != 0) {
      delete m_proc;
      m_proc = nullptr;
      throw MTLD3DError("Failed to create native thread");
    }
  }

  ~unnotified_thread() {
    if (m_handle)
      std::terminate();
  }

  unnotified_thread(unnotified_thread &&other)
      : m_handle(other.m_handle), m_proc(other.m_proc) {
    other.m_handle = nullptr;
    other.m_proc = nullptr;
  }

  unnotified_thread &operator=(unnotified_thread &&other) {
    if (m_handle)
      std::terminate();
    m_handle = other.m_handle;
    m_proc = other.m_proc;
    other.m_handle = nullptr;
    other.m_proc = nullptr;
    return *this;
  }

  void join() {
    if (!m_handle)
      throw MTLD3DError("Thread not joinable");
    ::WaitForSingleObjectEx(m_handle, INFINITE, FALSE);
    ::CloseHandle(m_handle);
    m_handle = nullptr;
  }

  bool joinable() const { return m_handle != nullptr; }

private:
  HANDLE m_handle = nullptr;
  std::function<void()> *m_proc = nullptr;

  static DWORD WINAPI threadProc(void *arg) {
    auto *func = static_cast<std::function<void()> *>(arg);
    (*func)();
    delete func;
    return 0;
  }
};

namespace this_thread {
inline void yield() { SwitchToThread(); }

inline uint32_t get_id() { return uint32_t(GetCurrentThreadId()); }

bool isInModuleDetachment();
} // namespace this_thread

/**
 * \brief SRW-based mutex implementation
 *
 * Drop-in replacement for \c std::mutex that uses Win32
 * SRW locks, which are implemented with \c futex in wine.
 */
class mutex {

public:
  using native_handle_type = PSRWLOCK;

  mutex() {}

  mutex(const mutex &) = delete;
  mutex &operator=(const mutex &) = delete;

  void lock() { AcquireSRWLockExclusive(&m_lock); }

  void unlock() { ReleaseSRWLockExclusive(&m_lock); }

  bool try_lock() { return TryAcquireSRWLockExclusive(&m_lock); }

  native_handle_type native_handle() { return &m_lock; }

private:
  SRWLOCK m_lock = SRWLOCK_INIT;
};

/**
 * \brief SRW-based shared mutex implementation
 */
class shared_mutex {

public:
  using native_handle_type = PSRWLOCK;

  shared_mutex() {}

  shared_mutex(const shared_mutex &) = delete;
  shared_mutex &operator=(const shared_mutex &) = delete;

  void lock() { AcquireSRWLockExclusive(&m_lock); }

  void lock_shared() { AcquireSRWLockShared(&m_lock); }

  void unlock() { ReleaseSRWLockExclusive(&m_lock); }

  void unlock_shared() { ReleaseSRWLockShared(&m_lock); }

  bool try_lock() { return TryAcquireSRWLockExclusive(&m_lock); }

  bool try_lock_shared() { return TryAcquireSRWLockShared(&m_lock); }

  native_handle_type native_handle() { return &m_lock; }

private:
  SRWLOCK m_lock = SRWLOCK_INIT;
};

/**
 * \brief Recursive mutex implementation
 *
 * Drop-in replacement for \c std::recursive_mutex that
 * uses Win32 critical sections.
 */
class recursive_mutex {

public:
  using native_handle_type = PCRITICAL_SECTION;

  recursive_mutex() { InitializeCriticalSection(&m_lock); }

  ~recursive_mutex() { DeleteCriticalSection(&m_lock); }

  recursive_mutex(const recursive_mutex &) = delete;
  recursive_mutex &operator=(const recursive_mutex &) = delete;

  void lock() { EnterCriticalSection(&m_lock); }

  void unlock() { LeaveCriticalSection(&m_lock); }

  bool try_lock() { return TryEnterCriticalSection(&m_lock); }

  native_handle_type native_handle() { return &m_lock; }

private:
  CRITICAL_SECTION m_lock;
};

/**
 * \brief SRW-based condition variable implementation
 *
 * Drop-in replacement for \c std::condition_variable that
 * uses Win32 condition variables on SRW locks.
 */
class condition_variable {

public:
  using native_handle_type = PCONDITION_VARIABLE;

  condition_variable() { InitializeConditionVariable(&m_cond); }

  condition_variable(condition_variable &) = delete;

  condition_variable &operator=(condition_variable &) = delete;

  void notify_one() { WakeConditionVariable(&m_cond); }

  void notify_all() { WakeAllConditionVariable(&m_cond); }

  void wait(std::unique_lock<dxmt::mutex> &lock) {
    auto srw = lock.mutex()->native_handle();
    SleepConditionVariableSRW(&m_cond, srw, INFINITE, 0);
  }

  template <typename Predicate>
  void wait(std::unique_lock<dxmt::mutex> &lock, Predicate pred) {
    while (!pred())
      wait(lock);
  }

  template <typename Clock, typename Duration>
  std::cv_status
  wait_until(std::unique_lock<dxmt::mutex> &lock,
             const std::chrono::time_point<Clock, Duration> &time) {
    auto now = Clock::now();

    return (now < time) ? wait_for(lock, now - time) : std::cv_status::timeout;
  }

  template <typename Clock, typename Duration, typename Predicate>
  bool wait_until(std::unique_lock<dxmt::mutex> &lock,
                  const std::chrono::time_point<Clock, Duration> &time,
                  Predicate pred) {
    if (pred())
      return true;

    auto now = Clock::now();
    return now < time && wait_for(lock, now - time, pred);
  }

  template <typename Rep, typename Period>
  std::cv_status wait_for(std::unique_lock<dxmt::mutex> &lock,
                          const std::chrono::duration<Rep, Period> &timeout) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout);
    auto srw = lock.mutex()->native_handle();

    return SleepConditionVariableSRW(&m_cond, srw, ms.count(), 0)
               ? std::cv_status::no_timeout
               : std::cv_status::timeout;
  }

  template <typename Rep, typename Period, typename Predicate>
  bool wait_for(std::unique_lock<dxmt::mutex> &lock,
                const std::chrono::duration<Rep, Period> &timeout,
                Predicate pred) {
    bool result = pred();

    if (!result && wait_for(lock, timeout) == std::cv_status::no_timeout)
      result = pred();

    return result;
  }

  native_handle_type native_handle() { return &m_cond; }

private:
  CONDITION_VARIABLE m_cond;
};

#else
class thread : public std::thread {
public:
  using std::thread::thread;

  void set_priority(ThreadPriority priority) {
    ::sched_param param = {};
    int32_t policy;
    switch (priority) {
    default:
    case ThreadPriority::Normal:
      policy = SCHED_OTHER;
      break;
    case ThreadPriority::Lowest:
#ifdef __APPLE__
      policy = SCHED_FIFO; /* No SCHED_IDLE on macOS */
#else
      policy = SCHED_IDLE;
#endif
      break;
    }
    ::pthread_setschedparam(this->native_handle(), policy, &param);
  }
};

using mutex = std::mutex;
using shared_mutex = std::shared_mutex;
using recursive_mutex = std::recursive_mutex;
using condition_variable = std::condition_variable;

namespace this_thread {
inline void yield() { std::this_thread::yield(); }

uint32_t get_id();

inline bool isInModuleDetachment() { return false; }
} // namespace this_thread
#endif

struct null_mutex {
  void lock() {}
  void unlock() noexcept {}
  bool try_lock() { return true; }
};

} // namespace dxmt
