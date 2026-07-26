#ifndef CO_MIRA_SYNC_SEMAPHORE_HPP
#define CO_MIRA_SYNC_SEMAPHORE_HPP

#include "sync/detail/waiter.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <list>
#include <mutex>
#include <stdexcept>

namespace mira::co {

// A FIFO coroutine semaphore. Internal std::mutex sections only protect the
// counter and waiter list; scheduler threads are never blocked while waiting
// for a permit.
class counting_semaphore {
public:
  class [[nodiscard]] acquire_awaiter {
  public:
    explicit acquire_awaiter(counting_semaphore &semaphore) noexcept : semaphore_(&semaphore) {}

    static constexpr bool await_ready() noexcept { return false; }

    bool await_suspend(co_handle<> handle) {
      this->waiter_.prepare(handle);

      std::lock_guard lock(this->semaphore_->state_mutex_);
      if (this->semaphore_->count_ != 0) {
        --this->semaphore_->count_;
        return false;
      }

      this->semaphore_->waiters_.push_back(this);
      return true;
    }

    void await_resume() noexcept {
      this->waiter_.finish();
      this->semaphore_->synchronize_handoff();
    }

    acquire_awaiter(const acquire_awaiter &) = delete;
    acquire_awaiter &operator=(const acquire_awaiter &) = delete;
    acquire_awaiter(acquire_awaiter &&) = default;
    acquire_awaiter &operator=(acquire_awaiter &&) = delete;

  private:
    friend class counting_semaphore;

    counting_semaphore *semaphore_;
    core::sync_waiter waiter_;
  };

  explicit counting_semaphore(std::ptrdiff_t desired) {
    if (desired < 0) {
      log("semaphore initial count cannot be negative");
      throw std::invalid_argument("semaphore initial count cannot be negative");
    }
    this->count_ = desired;
  }

  ~counting_semaphore() noexcept {
    if (!this->waiters_.empty()) [[unlikely]] {
      try {
        log("destroying a semaphore with suspended waiters");
      } catch (...) {
      }
      std::terminate();
    }
  }

  counting_semaphore(const counting_semaphore &) = delete;
  counting_semaphore &operator=(const counting_semaphore &) = delete;
  counting_semaphore(counting_semaphore &&) = delete;
  counting_semaphore &operator=(counting_semaphore &&) = delete;

  [[nodiscard]] acquire_awaiter acquire() noexcept { return acquire_awaiter{*this}; }

  [[nodiscard]] bool try_acquire() {
    std::lock_guard lock(this->state_mutex_);
    if (this->count_ == 0)
      return false;
    --this->count_;
    return true;
  }

  void release(std::ptrdiff_t update = 1) {
    if (update <= 0) {
      log("semaphore release count must be positive");
      throw std::invalid_argument("semaphore release count must be positive");
    }

    std::list<acquire_awaiter *> wakeups;
    {
      std::lock_guard lock(this->state_mutex_);

      const std::size_t wake_count =
          std::min<std::size_t>(static_cast<std::size_t>(update), this->waiters_.size());
      const std::ptrdiff_t remaining = update - static_cast<std::ptrdiff_t>(wake_count);

      if (remaining > std::numeric_limits<std::ptrdiff_t>::max() - this->count_) {
        log("semaphore count overflow");
        throw std::overflow_error("semaphore count overflow");
      }

      for (std::size_t index = 0; index < wake_count; ++index)
        wakeups.splice(wakeups.end(), this->waiters_, this->waiters_.begin());

      this->count_ += remaining;
    }

    for (acquire_awaiter *waiter : wakeups)
      waiter->waiter_.schedule();
  }

private:
  void synchronize_handoff() noexcept {
    // Acquire the release performed by release() after it removed this waiter.
    std::lock_guard lock(this->state_mutex_);
  }

  std::mutex state_mutex_;
  std::ptrdiff_t count_ = 0;
  std::list<acquire_awaiter *> waiters_;
};

using semaphore = counting_semaphore;

} // namespace mira::co

#endif
