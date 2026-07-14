#ifndef CO_MIRA_TASK_HPP
#define CO_MIRA_TASK_HPP
#include "co_mira.h"

#include <cassert>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <utility>
#include <variant>

namespace mira::co {

struct task_promise_base;
template <typename T> struct task_promise;
template <typename T> struct task;

struct final_awaiter {
  static constexpr bool await_ready() noexcept { return false; }
  template <std::derived_from<task_promise_base> Promise>
  co_handle<> await_suspend(co_handle<Promise> cont) noexcept {
    auto &parent = cont.promise().parent_coro_;
    return parent ? parent : std::noop_coroutine();
  }
  // never resume
  static constexpr void await_resume() noexcept {}
};

struct task_promise_base {
  task_promise_base() noexcept = default;
  friend struct final_awaiter;

  static constexpr std::suspend_always initial_suspend() noexcept { return {}; }
  final_awaiter final_suspend() noexcept { return {}; }

  void set_parent(co_handle<> cont) noexcept {
    assert(!this->has_parent());
    this->parent_coro_ = cont;
  }
  bool has_parent() noexcept { return static_cast<bool>(this->parent_coro_); }

  task_promise_base(const task_promise_base &) noexcept = delete;
  task_promise_base &operator=(const task_promise_base &) noexcept = delete;
  task_promise_base(task_promise_base &&) noexcept = delete;
  task_promise_base &operator=(task_promise_base &&) noexcept = delete;

private:
  co_handle<> parent_coro_{};
};

template <typename T> struct task_promise : task_promise_base {
  task<T> get_return_object();
  void unhandled_exception() noexcept {
    this->result_.template emplace<exception_index>(std::current_exception());
  }
  template <typename Value>
  void return_value(Value &&value) noexcept(std::is_nothrow_constructible_v<T, Value &&>) {
    this->result_.template emplace<value_index>(std::forward<Value>(value));
  }

  auto &&result(this auto &&self) {
    if (auto vptr = std::get_if<value_index>(&self.result_)) [[likely]] {
      return std::forward_like<decltype(self)>(*vptr);
    }

    if (auto eptr = std::get_if<exception_index>(&self.result_)) [[unlikely]] {
      std::rethrow_exception(*eptr);
    }

    // mono
    std::terminate();
  }

private:
  static constexpr uint8_t empty_index = 0;
  static constexpr uint8_t value_index = 1;
  static constexpr uint8_t exception_index = 2;

  std::variant<std::monostate, T, std::exception_ptr> result_;
};

template <> struct task_promise<void> : task_promise_base {
  task<void> get_return_object();
  void unhandled_exception() noexcept { this->except_ = std::current_exception(); }
  void return_void() const {}
  void result() const {
    if (auto &e = this->except_) {
      std::rethrow_exception(e);
    }
  }

private:
  std::exception_ptr except_;
};

template <typename T> struct task_promise<T &> : task_promise_base {
  task<T &> get_return_object();
  void unhandled_exception() noexcept {
    this->result_.template emplace<exception_index>(std::current_exception());
  }

  void return_value(T &value) noexcept { this->result_ = std::addressof(value); }

  T &result() const {
    if (auto eptr = std::get_if<exception_index>(&this->result_)) [[unlikely]] {
      std::rethrow_exception(*eptr);
    }
    return *std::get<pointer_index>(this->result_);
  }

private:
  static constexpr uint8_t pointer_index = 0;
  static constexpr uint8_t exception_index = 1;

  std::variant<T *, std::exception_ptr> result_;
};

template <typename T = void> struct [[nodiscard]] task {
  using promise_type = task_promise<T>;
  using handle_type = co_handle<promise_type>;

  friend promise_type;

  struct task_awaiter_base {
    bool await_ready() noexcept { return !this->handle_ || this->handle_.done(); }
    co_handle<> await_suspend(co_handle<> cont) {
      this->handle_.promise().set_parent(cont);
      return this->handle_;
    }
    static constexpr void await_resume() noexcept {}

    co_handle<promise_type> handle_;
  };

  ~task() {
    if (auto &h = this->handle_)
      h.destroy();
  }

  task(task &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  task &operator=(task &&other) noexcept {
    if (this != &other) {
      std::swap(this->handle_, other.handle_);
    }
    return *this;
  }

  task(const task &) = delete;
  task &operator=(const task &) = delete;

  co_handle<promise_type> get_handle() noexcept { return this->handle_; }

  [[nodiscard]] bool is_ready() noexcept { return !this->handle_ || this->handle_.done(); }
  auto when_ready() { return task_awaiter_base{this->handle_}; }

  auto operator co_await(this auto &&self) {
    assert(self.handle_);
    struct task_awaiter : task_awaiter_base {
      decltype(auto) await_resume() {
        return std::forward_like<decltype(self)>(this->handle_.promise()).result();
      }
    };
    return task_awaiter{self.handle_};
  }

private:
  explicit task(co_handle<promise_type> handle) noexcept : handle_(handle) {}
  co_handle<promise_type> handle_;
};

template <typename T> task<T> inline task_promise<T>::get_return_object() {
  return task<T>{co_handle<task_promise>::from_promise(*this)};
}

task<void> inline task_promise<void>::get_return_object() {
  return task<void>{co_handle<task_promise>::from_promise(*this)};
}

template <typename T> task<T &> inline task_promise<T &>::get_return_object() {
  return task<T &>{co_handle<task_promise>::from_promise(*this)};
}

} // namespace mira::co

#endif