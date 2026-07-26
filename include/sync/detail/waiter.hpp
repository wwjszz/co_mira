#ifndef CO_MIRA_SYNC_DETAIL_WAITER_HPP
#define CO_MIRA_SYNC_DETAIL_WAITER_HPP

#include "detail/task_info.hpp"
#include "detail/thread_meta.hpp"
#include "scheduler.hpp"

#include <coroutine>
#include <exception>
#include <stdexcept>

namespace mira::co::core {

// A synchronization waiter is embedded in the awaiting coroutine frame. The
// synchronization object must remove it from its queue before scheduling it.
// Cross-scheduler wakeups reuse the MSG_RING resume_on control path, so the
// origin scheduler must have been started with start_remote().
struct sync_waiter {
  void prepare(co_handle<> handle) {
    if (this_thread.sche_state == nullptr) [[unlikely]] {
      log("coroutine synchronization requires a scheduler host thread");
      throw std::logic_error("coroutine synchronization requires a scheduler host thread");
    }

    this->origin_ = this_thread.sche_state;
    this->info_.handle = handle;
  }

  void schedule() noexcept {
    try {
      scheduler_state *const current = this_thread.sche_state;
      if (current == nullptr) [[unlikely]] {
        log("waking a coroutine synchronization waiter requires a scheduler host thread");
        std::terminate();
      }

      if (current == this->origin_)
        this->origin_->push_handle(this->info_.handle);
      else
        this->origin_->resume_on_msg_ring(&this->info_);
    } catch (...) {
      // Losing a mutex/semaphore/CV wakeup cannot be recovered locally: the
      // resource has already been handed to this waiter.
      log("failed to schedule a coroutine synchronization waiter");
      std::terminate();
    }
  }

  void finish() const noexcept {
    if (this->info_.result < 0) [[unlikely]] {
      log("cross-scheduler synchronization wakeup failed with errno {}", -this->info_.result);
      std::terminate();
    }
  }

private:
  task_info info_;
  scheduler_state *origin_ = nullptr;
};

} // namespace mira::co::core

#endif
