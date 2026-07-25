#ifndef CO_MIRA_WHEN_ANY_HPP
#define CO_MIRA_WHEN_ANY_HPP

#include "detail/core.hpp"
#include "detail/thread_meta.hpp"
#include "log.hpp"
#include "scheduler.hpp"
#include "task.hpp"
#include "when_all.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mira::co {

template <typename Variant> struct [[nodiscard]] when_any_result {
  std::size_t index;
  Variant value;
};

} // namespace mira::co

namespace mira::co::core {

template <typename... Awaitables>
using when_variant_t = std::variant<std::monostate, when_all_stored_result_t<when_all_await_result_t<Awaitables &&>>...>;

template <typename... Awaitables> using when_any_result_t = when_any_result<when_variant_t<Awaitables...>>;

template <typename Variant> class when_completion_entry {
public:
  template <std::size_t Index, typename Result, typename Value> void set_value(Value &&value) {
    if constexpr (std::is_lvalue_reference_v<Result>) {
      this->result_.emplace(when_any_result<Variant>{Index, Variant{std::in_place_index<Index + 1>, std::ref(value)}});
    } else {
      this->result_.emplace(when_any_result<Variant>{Index, Variant{std::in_place_index<Index + 1>, std::forward<Value>(value)}});
    }
  }

  template <std::size_t Index> void set_value() {
    this->result_.emplace(when_any_result<Variant>{Index, Variant{std::in_place_index<Index + 1>, std::monostate{}}});
  }

  void set_exception(std::exception_ptr exception) noexcept { this->exception_ = std::move(exception); }

  void rethrow_if_exception() const {
    if (this->exception_) [[unlikely]] {
      log("when combinator rethrowing selected child exception");
      std::rethrow_exception(this->exception_);
    }
  }

  when_any_result<Variant> take_result() {
    assert(this->result_.has_value());
    return std::move(*this->result_);
  }

private:
  std::optional<when_any_result<Variant>> result_;
  std::exception_ptr exception_;
};

template <bool ThreadSafe, typename Variant> class when_any_state {
public:
  using result_type = when_any_result<Variant>;
  enum class phase {
    open,
    claimed,
    published,
  };
  using phase_type = std::conditional_t<ThreadSafe, std::atomic<phase>, phase>;

  explicit when_any_state(scheduler_state &origin) noexcept : phase_(phase::open), origin_(&origin) {}

  when_any_state(const when_any_state &) = delete;
  when_any_state &operator=(const when_any_state &) = delete;
  when_any_state(when_any_state &&) = delete;
  when_any_state &operator=(when_any_state &&) = delete;

  [[nodiscard]] bool completed() const noexcept {
    if constexpr (ThreadSafe)
      return this->phase_.load(std::memory_order_acquire) == phase::published;
    else
      return this->phase_ == phase::published;
  }

  [[nodiscard]] bool selection_complete() const noexcept {
    if constexpr (ThreadSafe)
      return this->phase_.load(std::memory_order_relaxed) != phase::open;
    else
      return this->phase_ != phase::open;
  }

  [[nodiscard]] bool try_select() noexcept {
    if constexpr (ThreadSafe) {
      phase expected = phase::open;
      return this->phase_.compare_exchange_strong(expected, phase::claimed, std::memory_order_relaxed, std::memory_order_relaxed);
    } else {
      if (this->phase_ != phase::open)
        return false;
      this->phase_ = phase::claimed;
      return true;
    }
  }

  void publish() noexcept {
    if constexpr (ThreadSafe)
      this->phase_.store(phase::published, std::memory_order_release);
    else
      this->phase_ = phase::published;
  }

  void set_continuation(co_handle<> continuation) noexcept { this->continuation_ = continuation; }

  when_completion_entry<Variant> &entry() noexcept { return this->entry_; }

  void resume_parent() noexcept {
    // Ready-queue exhaustion is currently a scheduler hard failure.
    this->origin_->push_handle(this->continuation_);
  }

  void rethrow_if_exception() const { this->entry_.rethrow_if_exception(); }

  result_type take_result() { return this->entry_.take_result(); }

private:
  phase_type phase_;
  co_handle<> continuation_;
  scheduler_state *origin_;
  when_completion_entry<Variant> entry_;
};

template <typename State> struct when_shared_waiter {
  std::shared_ptr<State> state;

  [[nodiscard]] bool await_ready() const noexcept { return this->state->completed(); }

  void await_suspend(co_handle<> continuation) noexcept { this->state->set_continuation(continuation); }

  void await_resume() const noexcept {
    // The result slots are consumed only after acquiring their publication
    // state. This also covers the await_ready() fast path.
    if (!this->state->completed()) [[unlikely]] {
      log("when combinator parent resumed before results were published");
      std::terminate();
    }
  }
};

template <bool ThreadSafe, std::size_t Index, typename State, typename Awaitable>
task<void> when_any_evaluate(std::shared_ptr<State> state, Awaitable awaitable, scheduler &origin) noexcept {
  using result_type = when_all_await_result_t<Awaitable &&>;

  if (state->selection_complete())
    co_return;

  bool selected = false;
  try {
    if constexpr (std::is_void_v<result_type>) {
      co_await std::move(awaitable);
      selected = state->try_select();
      if (!selected)
        co_return;
      state->entry().template set_value<Index>();
    } else {
      decltype(auto) result = co_await std::move(awaitable);
      selected = state->try_select();
      if (!selected)
        co_return;
      state->entry().template set_value<Index, result_type>(std::forward<decltype(result)>(result));
    }
  } catch (...) {
    if (!selected && !state->try_select())
      co_return;
    state->entry().set_exception(std::current_exception());
  }

  state->publish();

  if constexpr (!ThreadSafe)
    assert(this_thread.sche == &origin && "when_any<false> child must complete on the origin scheduler");

  if constexpr (ThreadSafe) {
    // TODO: handle resume_on(origin) failure transactionally. The selected
    // child owns the only parent wakeup, so a transport failure otherwise
    // leaves the parent suspended forever.
    co_await ::mira::co::resume_on(origin);
  }

  state->resume_parent();
}

template <bool ThreadSafe, std::size_t Index, typename State, typename Awaitable>
void spawn_when_any_child(scheduler &origin, const std::shared_ptr<State> &state, Awaitable &&awaitable) noexcept {
  // TODO: make child creation/enqueue transactional. A hard failure currently
  // terminates instead of leaving a partially spawned combinator.
  auto child = when_any_evaluate<ThreadSafe, Index>(state, std::forward<Awaitable>(awaitable), origin);
  origin.co_spawn(std::move(child));
}

template <bool ThreadSafe, typename... Awaitables> using when_any_state_t = when_any_state<ThreadSafe, when_variant_t<Awaitables...>>;

} // namespace mira::co::core

namespace mira::co {

// The first completion wins, including an exceptional completion. Awaitables
// that have not started may observe the selected state and exit early; already
// suspended awaitables continue naturally. No cancellation request is issued.
template <bool ThreadSafe = true, typename... Awaitables> task<core::when_any_result_t<Awaitables...>> when_any(Awaitables... awaitables) {
  static_assert(sizeof...(Awaitables) != 0, "when_any requires at least one awaitable");

  if (core::this_thread.sche == nullptr || core::this_thread.sche_state == nullptr) [[unlikely]] {
    log("when_any requires a scheduler host thread");
    throw std::logic_error("when_any requires a scheduler host thread");
  }

  using state_type = core::when_any_state_t<ThreadSafe, Awaitables...>;
  auto state = std::make_shared<state_type>(*core::this_thread.sche_state);
  auto inputs = std::tuple<Awaitables...>{std::move(awaitables)...};
  scheduler &origin = *core::this_thread.sche;

  [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    (core::spawn_when_any_child<ThreadSafe, Index>(origin, state, std::move(std::get<Index>(inputs))), ...);
  }(std::index_sequence_for<Awaitables...>{});

  co_await core::when_shared_waiter<state_type>{state};
  state->rethrow_if_exception();
  co_return state->take_result();
}

} // namespace mira::co

#endif
