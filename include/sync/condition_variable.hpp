#ifndef CO_MIRA_SYNC_CONDITION_VARIABLE_HPP
#define CO_MIRA_SYNC_CONDITION_VARIABLE_HPP

#include "sync/mutex.hpp"

#include <functional>
#include <list>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mira::co {

class condition_variable {
  class [[nodiscard]] wait_awaiter {
  public:
    wait_awaiter(condition_variable &condition, unique_lock &lock) noexcept : condition_(&condition), lock_(&lock) {}

    static constexpr bool await_ready() noexcept { return false; }

    bool await_suspend(co_handle<> handle) {
      this->waiter_.prepare(handle);

      std::lock_guard state_lock(this->condition_->state_mutex_);
      this->condition_->waiters_.push_back(this);
      // Publishing the waiter before releasing the user mutex closes the
      // predicate-check/unlock/wait lost-wakeup window.
      this->lock_->release_for_wait();
      return true;
    }

    void await_resume() const noexcept { this->waiter_.finish(); }

    wait_awaiter(const wait_awaiter &) = delete;
    wait_awaiter &operator=(const wait_awaiter &) = delete;
    wait_awaiter(wait_awaiter &&) = default;
    wait_awaiter &operator=(wait_awaiter &&) = delete;

  private:
    friend class condition_variable;

    condition_variable *condition_;
    unique_lock *lock_;
    core::sync_waiter waiter_;
  };

public:
  condition_variable() noexcept = default;

  ~condition_variable() noexcept {
    if (!this->waiters_.empty()) [[unlikely]] {
      try {
        log("destroying a condition_variable with suspended waiters");
      } catch (...) {
      }
      std::terminate();
    }
  }

  condition_variable(const condition_variable &) = delete;
  condition_variable &operator=(const condition_variable &) = delete;
  condition_variable(condition_variable &&) = delete;
  condition_variable &operator=(condition_variable &&) = delete;

  task<void> wait(unique_lock &lock) {
    if (!lock.owns_lock() || lock.associated_mutex() == nullptr) {
      log("condition_variable wait requires an owning unique_lock");
      throw std::logic_error("condition_variable wait requires an owning unique_lock");
    }

    mutex *const target = lock.associated_mutex();
    co_await wait_awaiter{*this, lock};
    co_await target->lock();
    lock.adopt_after_wait();
  }

  template <typename Predicate> task<void> wait(unique_lock &lock, Predicate predicate) {
    while (!std::invoke(predicate))
      co_await this->wait(lock);
  }

  void notify_one() noexcept {
    wait_awaiter *waiter = nullptr;
    {
      std::lock_guard lock(this->state_mutex_);
      if (this->waiters_.empty())
        return;
      waiter = this->waiters_.front();
      this->waiters_.pop_front();
    }

    waiter->waiter_.schedule();
  }

  void notify_all() noexcept {
    std::list<wait_awaiter *> wakeups;
    {
      std::lock_guard lock(this->state_mutex_);
      wakeups.swap(this->waiters_);
    }

    for (wait_awaiter *waiter : wakeups)
      waiter->waiter_.schedule();
  }

private:
  std::mutex state_mutex_;
  std::list<wait_awaiter *> waiters_;
};

} // namespace mira::co

#endif
