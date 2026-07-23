#ifndef CO_MIRA_WHEN_ALL_HPP
#define CO_MIRA_WHEN_ALL_HPP

#include "detail/core.hpp"
#include "detail/thread_meta.hpp"
#include "log.hpp"
#include "scheduler.hpp"
#include "task.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace mira::co::core {

template <typename Awaitable> decltype(auto) when_all_get_awaiter(Awaitable &&awaitable) {
  if constexpr (requires { std::forward<Awaitable>(awaitable).operator co_await(); }) {
    return std::forward<Awaitable>(awaitable).operator co_await();
  } else if constexpr (requires { operator co_await(std::forward<Awaitable>(awaitable)); }) {
    return operator co_await(std::forward<Awaitable>(awaitable));
  } else {
    return std::forward<Awaitable>(awaitable);
  }
}

template <typename Awaitable> using when_all_await_result_t = decltype(when_all_get_awaiter(std::declval<Awaitable>()).await_resume());

template <typename Result> struct when_all_stored_result {
  using type = std::remove_cvref_t<Result>;
};

template <typename Result> struct when_all_stored_result<Result &> {
  using type = std::reference_wrapper<Result>;
};

template <> struct when_all_stored_result<void> {
  using type = std::monostate;
};

template <typename Result> using when_all_stored_result_t = typename when_all_stored_result<Result>::type;

template <typename Result> struct when_all_result_slot {
  using value_type = when_all_stored_result_t<Result>;

  template <typename Value> void set_value(Value &&value) {
    if constexpr (std::is_lvalue_reference_v<Result>)
      this->value_.emplace(std::ref(value));
    else
      this->value_.emplace(std::forward<Value>(value));
  }

  void set_exception(std::exception_ptr exception) noexcept { this->exception_ = std::move(exception); }

  void rethrow_if_exception() const {
    if (this->exception_) [[unlikely]] {
      log("when_all rethrowing child exception");
      std::rethrow_exception(this->exception_);
    }
  }

  value_type take_value() {
    assert(this->value_.has_value());
    return std::move(*this->value_);
  }

private:
  std::optional<value_type> value_;
  std::exception_ptr exception_;
};

template <> struct when_all_result_slot<void> {
  using value_type = std::monostate;

  void set_value() noexcept { this->completed_ = true; }

  void set_exception(std::exception_ptr exception) noexcept { this->exception_ = std::move(exception); }

  void rethrow_if_exception() const {
    if (this->exception_) [[unlikely]] {
      log("when_all rethrowing child exception");
      std::rethrow_exception(this->exception_);
    }
  }

  value_type take_value() const noexcept {
    assert(this->completed_);
    return {};
  }

private:
  bool completed_ = false;
  std::exception_ptr exception_;
};

template <bool ThreadSafe, typename... Results> class when_all_meta {
public:
  using result_type = std::tuple<when_all_stored_result_t<Results>...>;
  using counter_type = std::conditional_t<ThreadSafe, std::atomic<std::size_t>, std::size_t>;

  explicit when_all_meta(std::size_t count, scheduler_state &origin) noexcept : remaining_(count), origin_(&origin) {}

  when_all_meta(const when_all_meta &) = delete;
  when_all_meta &operator=(const when_all_meta &) = delete;
  when_all_meta(when_all_meta &&) = delete;
  when_all_meta &operator=(when_all_meta &&) = delete;

  [[nodiscard]] bool completed() const noexcept {
    if constexpr (ThreadSafe)
      return this->remaining_.load(std::memory_order_acquire) == 0;
    else
      return this->remaining_ == 0;
  }

  void set_continuation(co_handle<> continuation) noexcept { this->continuation_ = continuation; }

  template <std::size_t Index> auto &slot() noexcept { return std::get<Index>(this->results_); }

  [[nodiscard]] bool count_down() noexcept {
    if constexpr (ThreadSafe) {
      const std::size_t previous = this->remaining_.fetch_sub(1, std::memory_order_acq_rel);
      assert(previous != 0);
      return previous == 1;
    } else {
      assert(this->remaining_ != 0);
      return --this->remaining_ == 0;
    }
  }

  void resume_parent() noexcept {
    // Ready-queue exhaustion is currently a scheduler hard failure.
    this->origin_->push_handle(this->continuation_);
  }

  void rethrow_exceptions() const { this->rethrow_exceptions_impl(std::index_sequence_for<Results...>{}); }

  result_type take_results() { return this->take_results_impl(std::index_sequence_for<Results...>{}); }

private:
  template <std::size_t... Index> void rethrow_exceptions_impl(std::index_sequence<Index...>) const {
    (std::get<Index>(this->results_).rethrow_if_exception(), ...);
  }

  template <std::size_t... Index> result_type take_results_impl(std::index_sequence<Index...>) {
    return result_type{std::get<Index>(this->results_).take_value()...};
  }

  std::tuple<when_all_result_slot<Results>...> results_;
  counter_type remaining_;
  co_handle<> continuation_;
  scheduler_state *origin_;
};

template <typename Meta> struct when_all_waiter {
  Meta &meta;

  [[nodiscard]] bool await_ready() const noexcept { return this->meta.completed(); }

  void await_suspend(co_handle<> continuation) noexcept { this->meta.set_continuation(continuation); }

  static constexpr void await_resume() noexcept {}
};

template <bool ThreadSafe, std::size_t Index, typename Meta, typename Awaitable>
task<void> when_all_evaluate(Meta &meta, Awaitable awaitable, scheduler &sche) noexcept {
  using result_type = when_all_await_result_t<Awaitable &&>;
  auto &slot = meta.template slot<Index>();

  try {
    if constexpr (std::is_void_v<result_type>) {
      co_await std::move(awaitable);
      slot.set_value();
    } else {
      decltype(auto) result = co_await std::move(awaitable);
      slot.set_value(std::forward<decltype(result)>(result));
    }
  } catch (...) {
    slot.set_exception(std::current_exception());
  }

  if constexpr (!ThreadSafe)
    assert(this_thread.sche == &sche &&
           "when_all<false> child must complete on the origin scheduler");

  if (!meta.count_down())
    co_return;

  if constexpr (ThreadSafe) {
    // TODO: handle resume_on(origin) failure transactionally. If it throws,
    // the last child cannot resume the parent when_all coroutine. Record the
    // transport exception in the completion state and arrange another wakeup,
    // or make this an explicit runtime-fatal control-plane failure.
    co_await ::mira::co::resume_on(sche);
  }

  meta.resume_parent();
}

template <bool ThreadSafe, std::size_t Index, typename Meta, typename Awaitable>
void spawn_when_all_child(scheduler &sche, Meta &meta, Awaitable &&awaitable) noexcept {
  // TODO: make child creation/enqueue transactional. noexcept currently
  // converts a partial co_spawn() failure into termination, matching the
  // scheduler's current hard-failure policy without leaving children that
  // reference a destroyed when_all coroutine frame.
  auto child =
      when_all_evaluate<ThreadSafe, Index>(meta, std::forward<Awaitable>(awaitable), sche);
  sche.co_spawn(std::move(child));
}

template <bool ThreadSafe, typename... Awaitables>
using when_all_meta_t =
    when_all_meta<ThreadSafe, when_all_await_result_t<Awaitables &&>...>;

template <typename... Awaitables>
using when_all_result_t =
    typename when_all_meta<false, when_all_await_result_t<Awaitables &&>...>::result_type;

} // namespace mira::co::core

namespace mira::co {

// Awaitables are taken by value so the lazy when_all coroutine owns them from
// the moment it is created. The default thread-safe mode permits children to
// complete on different schedulers: every child performs an atomic count-down,
// and only the last child returns to the origin to resume the parent. If a
// child can leave, the origin must be started with start_remote(). The
// when_all<false>() fast path uses a plain counter and requires every child to
// complete on the origin scheduler.
template <bool ThreadSafe = true, typename... Awaitables>
task<core::when_all_result_t<Awaitables...>> when_all(Awaitables... awaitables) {
  if (core::this_thread.sche == nullptr || core::this_thread.sche_state == nullptr) [[unlikely]] {
    log("when_all requires a scheduler host thread");
    throw std::logic_error("when_all requires a scheduler host thread");
  }

  using meta_type = core::when_all_meta_t<ThreadSafe, Awaitables...>;
  meta_type meta{sizeof...(Awaitables), *core::this_thread.sche_state};
  auto inputs = std::tuple<Awaitables...>{std::move(awaitables)...};
  scheduler &sche = *core::this_thread.sche;

  [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    (core::spawn_when_all_child<ThreadSafe, Index>(
         sche, meta, std::move(std::get<Index>(inputs))),
     ...);
  }(std::index_sequence_for<Awaitables...>{});

  co_await core::when_all_waiter<meta_type>{meta};
  meta.rethrow_exceptions();
  co_return meta.take_results();
}

} // namespace mira::co

#endif
