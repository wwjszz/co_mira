#ifndef CO_MIRA_SHARED_TASK_HPP
#define CO_MIRA_SHARED_TASK_HPP

#include "sync/detail/waiter.hpp"
#include "task.hpp"

#include <atomic>
#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mira::co {

template <typename T> class shared_task;

namespace core {

template <typename T> struct shared_task_promise;

struct shared_task_waiter {
  sync_waiter scheduler_waiter;
};

class shared_task_promise_base {
  enum class execution_state : uint8_t {
    not_started,
    running,
    completed,
  };

  struct final_awaiter {
    static constexpr bool await_ready() noexcept { return false; }

    template <std::derived_from<shared_task_promise_base> Promise> void await_suspend(co_handle<Promise> handle) noexcept {
      handle.promise().complete(handle);
    }

    static constexpr void await_resume() noexcept {}
  };

public:
  shared_task_promise_base() noexcept = default;

  static constexpr std::suspend_always initial_suspend() noexcept { return {}; }
  static constexpr final_awaiter final_suspend() noexcept { return {}; }

  void unhandled_exception() noexcept { this->exception_ = std::current_exception(); }

  [[nodiscard]] bool is_ready() const noexcept { return this->ready_.load(std::memory_order_acquire); }

  void add_reference() noexcept {
    const uint32_t previous = this->reference_count_.fetch_add(1, std::memory_order_relaxed);
    if (previous == std::numeric_limits<uint32_t>::max()) [[unlikely]] {
      log("shared_task reference count overflow");
      std::terminate();
    }
  }

  void release_reference(co_handle<> handle) noexcept {
    const uint32_t previous = this->reference_count_.fetch_sub(1, std::memory_order_acq_rel);
    assert(previous != 0);
    if (previous != 1)
      return;

#ifdef CO_MIRA_ENABLE_COUNTERS
    ::mira::co::handle_counter.decrement();
#endif
    handle.destroy();
  }

  co_handle<> suspend(shared_task_waiter &waiter, co_handle<> continuation, co_handle<> task_handle) {
    waiter.scheduler_waiter.prepare(continuation);

    std::lock_guard lock(this->state_mutex_);
    if (this->state_ == execution_state::completed)
      return continuation;

    this->waiters_.push_back(std::addressof(waiter));
    if (this->state_ == execution_state::not_started) {
      // Keep the frame alive even if every public shared_task is released
      // while its coroutine is still executing.
      this->add_reference();
      this->state_ = execution_state::running;
      return task_handle;
    }

    return std::noop_coroutine();
  }

  void finish_await(const shared_task_waiter &waiter) const noexcept {
    waiter.scheduler_waiter.finish();
    if (!this->ready_.load(std::memory_order_acquire)) [[unlikely]] {
      log("shared_task waiter resumed before the result was published");
      std::terminate();
    }
  }

protected:
  void rethrow_if_exception() const {
    if (this->exception_) [[unlikely]] {
      log("shared_task rethrowing stored exception");
      std::rethrow_exception(this->exception_);
    }
  }

private:
  void complete(co_handle<> handle) noexcept {
    std::list<shared_task_waiter *> ready_waiters;
    {
      std::lock_guard lock(this->state_mutex_);
      if (this->state_ != execution_state::running) [[unlikely]] {
        log("shared_task completed from an invalid execution state");
        std::terminate();
      }

      this->state_ = execution_state::completed;
      this->ready_.store(true, std::memory_order_release);
      ready_waiters.splice(ready_waiters.end(), this->waiters_);
    }

    for (shared_task_waiter *waiter : ready_waiters)
      waiter->scheduler_waiter.schedule();

    // Drop the execution reference acquired by the first waiter.
    this->release_reference(handle);
  }

  std::atomic<uint32_t> reference_count_{1};
  std::atomic<bool> ready_{false};
  mutable std::mutex state_mutex_;
  std::list<shared_task_waiter *> waiters_;
  execution_state state_ = execution_state::not_started;
  std::exception_ptr exception_;
};

template <typename T> struct [[nodiscard]] shared_task_promise : shared_task_promise_base {
  static_assert(!std::is_reference_v<T>);

  shared_task<T> get_return_object() noexcept;

  template <typename Value>
    requires std::constructible_from<T, Value &&>
  void return_value(Value &&value) noexcept(std::is_nothrow_constructible_v<T, Value &&>) {
    this->value_.emplace(std::forward<Value>(value));
  }

  [[nodiscard]] T &result() {
    this->rethrow_if_exception();
    if (!this->value_) [[unlikely]] {
      log("shared_task completed without a value");
      std::terminate();
    }
    return *this->value_;
  }

private:
  std::optional<T> value_;
};

template <> struct [[nodiscard]] shared_task_promise<void> : shared_task_promise_base {
  shared_task<void> get_return_object() noexcept;
  static constexpr void return_void() noexcept {}
  void result() const { this->rethrow_if_exception(); }
};

template <typename T> struct [[nodiscard]] shared_task_promise<T &> : shared_task_promise_base {
  shared_task<T &> get_return_object() noexcept;
  void return_value(T &value) noexcept { this->value_ = std::addressof(value); }

  [[nodiscard]] T &result() {
    this->rethrow_if_exception();
    if (this->value_ == nullptr) [[unlikely]] {
      log("shared_task<T&> completed without a reference");
      std::terminate();
    }
    return *this->value_;
  }

private:
  T *value_ = nullptr;
};

} // namespace core

// A lazy, copyable, multi-consumer coroutine result. Execution starts exactly
// once, when the first consumer awaits it. The completed value or exception is
// retained in the coroutine frame until the last shared_task/awaiter reference
// is released. Awaiting shared_task<T> returns T&, so consumers share one result
// object and must synchronize concurrent mutation themselves. A non-ready task
// must be awaited from a scheduler host thread. If consumers can wait on
// different schedulers, every origin scheduler must remain alive and be started
// with start_remote() so completion can return through MSG_RING.
template <typename T = void> class [[nodiscard]] shared_task {
public:
  static_assert(!std::is_rvalue_reference_v<T>, "shared_task does not support rvalue-reference result types");

  using promise_type = core::shared_task_promise<T>;
  using handle_type = co_handle<promise_type>;
  using value_type = T;

private:
  class awaiter_base {
  public:
    explicit awaiter_base(handle_type handle) noexcept : handle_(handle) {
      if (this->handle_)
        this->handle_.promise().add_reference();
    }

    ~awaiter_base() {
      if (this->handle_)
        this->handle_.promise().release_reference(this->handle_);
    }

    [[nodiscard]] bool await_ready() const noexcept { return !this->handle_ || this->handle_.promise().is_ready(); }

    co_handle<> await_suspend(co_handle<> continuation) { return this->handle_.promise().suspend(this->waiter_, continuation, this->handle_); }

    void finish() const noexcept {
      if (this->handle_)
        this->handle_.promise().finish_await(this->waiter_);
    }

    awaiter_base(const awaiter_base &) = delete;
    awaiter_base &operator=(const awaiter_base &) = delete;
    awaiter_base(awaiter_base &&) = delete;
    awaiter_base &operator=(awaiter_base &&) = delete;

  protected:
    handle_type handle_;

  private:
    core::shared_task_waiter waiter_;
  };

public:
  shared_task() noexcept = default;

  shared_task(const shared_task &other) noexcept : handle_(other.handle_) {
    if (this->handle_)
      this->handle_.promise().add_reference();
  }

  shared_task(shared_task &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  ~shared_task() { this->reset(); }

  shared_task &operator=(const shared_task &other) noexcept {
    if (this->handle_ == other.handle_)
      return *this;

    handle_type next = other.handle_;
    if (next)
      next.promise().add_reference();
    this->reset();
    this->handle_ = next;
    return *this;
  }

  shared_task &operator=(shared_task &&other) noexcept {
    if (this != std::addressof(other)) {
      this->reset();
      this->handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  void reset() noexcept {
    if (this->handle_) {
      handle_type handle = std::exchange(this->handle_, {});
      handle.promise().release_reference(handle);
    }
  }

  void swap(shared_task &other) noexcept { std::swap(this->handle_, other.handle_); }

  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(this->handle_); }

  [[nodiscard]] bool is_ready() const noexcept { return !this->handle_ || this->handle_.promise().is_ready(); }

  [[nodiscard]] auto operator co_await() const noexcept {
    struct result_awaiter final : awaiter_base {
      using awaiter_base::awaiter_base;

      decltype(auto) await_resume() {
        this->finish();
        if (!this->handle_) [[unlikely]] {
          log("awaiting an empty shared_task");
          throw std::logic_error("awaiting an empty shared_task");
        }
        return this->handle_.promise().result();
      }
    };

    return result_awaiter{this->handle_};
  }

  [[nodiscard]] auto when_ready() const noexcept {
    struct ready_awaiter final : awaiter_base {
      using awaiter_base::awaiter_base;
      void await_resume() const noexcept { this->finish(); }
    };

    return ready_awaiter{this->handle_};
  }

  friend bool operator==(const shared_task &, const shared_task &) noexcept = default;

private:
  friend promise_type;

  explicit shared_task(handle_type handle) noexcept : handle_(handle) {}

  handle_type handle_{};
};

template <typename T> inline shared_task<T> core::shared_task_promise<T>::get_return_object() noexcept {
#ifdef CO_MIRA_ENABLE_COUNTERS
  ::mira::co::handle_counter.increment();
#endif
  return shared_task<T>{co_handle<shared_task_promise>::from_promise(*this)};
}

inline shared_task<void> core::shared_task_promise<void>::get_return_object() noexcept {
#ifdef CO_MIRA_ENABLE_COUNTERS
  ::mira::co::handle_counter.increment();
#endif
  return shared_task<void>{co_handle<shared_task_promise>::from_promise(*this)};
}

template <typename T> inline shared_task<T &> core::shared_task_promise<T &>::get_return_object() noexcept {
#ifdef CO_MIRA_ENABLE_COUNTERS
  ::mira::co::handle_counter.increment();
#endif
  return shared_task<T &>{co_handle<shared_task_promise>::from_promise(*this)};
}

template <typename T> shared_task<T> make_shared_task(task<T> item) {
  if constexpr (std::is_void_v<T>) {
    co_await std::move(item);
    co_return;
  } else {
    decltype(auto) result = co_await std::move(item);
    co_return std::forward<decltype(result)>(result);
  }
}

template <typename T> void swap(shared_task<T> &left, shared_task<T> &right) noexcept { left.swap(right); }

} // namespace mira::co

#endif
