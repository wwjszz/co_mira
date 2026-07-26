#ifndef CO_MIRA_SYNC_MUTEX_HPP
#define CO_MIRA_SYNC_MUTEX_HPP

#include "sync/detail/waiter.hpp"
#include "task.hpp"

#include <list>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mira::co {

class unique_lock;
class condition_variable;

class mutex {
public:
  class [[nodiscard]] lock_awaiter {
  public:
    explicit lock_awaiter(mutex &target) noexcept : mutex_(&target) {}

    static constexpr bool await_ready() noexcept { return false; }

    bool await_suspend(co_handle<> handle) {
      this->waiter_.prepare(handle);

      std::lock_guard lock(this->mutex_->state_mutex_);
      if (!this->mutex_->locked_) {
        this->mutex_->locked_ = true;
        return false;
      }

      this->mutex_->waiters_.push_back(this);
      return true;
    }

    void await_resume() noexcept {
      this->waiter_.finish();
      this->mutex_->synchronize_handoff();
    }

    lock_awaiter(const lock_awaiter &) = delete;
    lock_awaiter &operator=(const lock_awaiter &) = delete;
    lock_awaiter(lock_awaiter &&) = default;
    lock_awaiter &operator=(lock_awaiter &&) = delete;

  private:
    friend class mutex;

    mutex *mutex_;
    core::sync_waiter waiter_;
  };

  class [[nodiscard]] lock_guard_awaiter {
  public:
    explicit lock_guard_awaiter(mutex &target) noexcept : mutex_(&target), operation_(target) {}

    bool await_ready() noexcept { return this->operation_.await_ready(); }

    bool await_suspend(co_handle<> handle) { return this->operation_.await_suspend(handle); }

    unique_lock await_resume() noexcept;

    lock_guard_awaiter(const lock_guard_awaiter &) = delete;
    lock_guard_awaiter &operator=(const lock_guard_awaiter &) = delete;
    lock_guard_awaiter(lock_guard_awaiter &&) = default;
    lock_guard_awaiter &operator=(lock_guard_awaiter &&) = delete;

  private:
    mutex *mutex_;
    lock_awaiter operation_;
  };

  mutex() noexcept = default;

  ~mutex() noexcept {
    if (this->locked_ || !this->waiters_.empty()) [[unlikely]] {
      try {
        log("destroying a locked coroutine mutex or one with suspended waiters");
      } catch (...) {
      }
      std::terminate();
    }
  }

  mutex(const mutex &) = delete;
  mutex &operator=(const mutex &) = delete;
  mutex(mutex &&) = delete;
  mutex &operator=(mutex &&) = delete;

  [[nodiscard]] lock_awaiter lock() noexcept { return lock_awaiter{*this}; }

  [[nodiscard]] lock_guard_awaiter lock_guard() noexcept { return lock_guard_awaiter{*this}; }

  [[nodiscard]] lock_guard_awaiter scoped_lock() noexcept { return lock_guard_awaiter{*this}; }

  [[nodiscard]] bool try_lock() {
    std::lock_guard lock(this->state_mutex_);
    if (this->locked_)
      return false;
    this->locked_ = true;
    return true;
  }

  void unlock() noexcept {
    lock_awaiter *waiter = nullptr;
    {
      std::lock_guard lock(this->state_mutex_);
      if (!this->locked_) [[unlikely]] {
        try {
          log("unlocking an unlocked coroutine mutex");
        } catch (...) {
        }
        std::terminate();
      }

      if (this->waiters_.empty()) {
        this->locked_ = false;
        return;
      }

      waiter = this->waiters_.front();
      this->waiters_.pop_front();
      // Direct handoff: locked_ stays true so a new contender cannot barge in.
    }

    waiter->waiter_.schedule();
  }

private:
  friend class unique_lock;

  void synchronize_handoff() noexcept {
    // Acquire the release performed by unlock() after it selected this waiter.
    std::lock_guard lock(this->state_mutex_);
  }

  std::mutex state_mutex_;
  bool locked_ = false;
  std::list<lock_awaiter *> waiters_;
};

// A movable RAII owner used by condition_variable. Unlike a simple lock_guard,
// it can temporarily unlock and later reacquire the same coroutine mutex.
class [[nodiscard]] unique_lock {
public:
  unique_lock() noexcept = default;

  ~unique_lock() noexcept {
    if (this->owns_)
      this->mutex_->unlock();
  }

  unique_lock(unique_lock &&other) noexcept
      : mutex_(std::exchange(other.mutex_, nullptr)), owns_(std::exchange(other.owns_, false)) {}

  unique_lock &operator=(unique_lock &&other) noexcept {
    if (this != &other) {
      if (this->owns_)
        this->mutex_->unlock();
      this->mutex_ = std::exchange(other.mutex_, nullptr);
      this->owns_ = std::exchange(other.owns_, false);
    }
    return *this;
  }

  unique_lock(const unique_lock &) = delete;
  unique_lock &operator=(const unique_lock &) = delete;

  [[nodiscard]] bool owns_lock() const noexcept { return this->owns_; }
  [[nodiscard]] explicit operator bool() const noexcept { return this->owns_; }
  [[nodiscard]] mutex *associated_mutex() const noexcept { return this->mutex_; }

  void unlock() {
    if (!this->owns_) {
      log("unlocking a unique_lock that does not own its mutex");
      throw std::logic_error("unique_lock does not own its mutex");
    }

    this->owns_ = false;
    this->mutex_->unlock();
  }

  task<void> lock() {
    if (this->mutex_ == nullptr) {
      log("locking a unique_lock without an associated mutex");
      throw std::logic_error("unique_lock has no associated mutex");
    }
    if (this->owns_) {
      log("locking a unique_lock that already owns its mutex");
      throw std::logic_error("unique_lock already owns its mutex");
    }

    co_await this->mutex_->lock();
    this->owns_ = true;
  }

private:
  friend class mutex;
  friend class condition_variable;

  explicit unique_lock(mutex &target) noexcept : mutex_(&target), owns_(true) {}

  void release_for_wait() noexcept {
    if (!this->owns_) [[unlikely]] {
      try {
        log("condition_variable wait requires an owning unique_lock");
      } catch (...) {
      }
      std::terminate();
    }

    this->owns_ = false;
    this->mutex_->unlock();
  }

  void adopt_after_wait() noexcept { this->owns_ = true; }

  mutex *mutex_ = nullptr;
  bool owns_ = false;
};

inline unique_lock mutex::lock_guard_awaiter::await_resume() noexcept {
  this->operation_.await_resume();
  return unique_lock{*this->mutex_};
}

} // namespace mira::co

#endif
