#ifndef CO_MIRA_WHEN_SOME_HPP
#define CO_MIRA_WHEN_SOME_HPP

#include "when_any.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace mira::co::core {

template <bool ThreadSafe, typename Variant> class when_some_state {
public:
  using result_type = std::vector<when_any_result<Variant>>;
  using counter_type = std::conditional_t<ThreadSafe, std::atomic<std::size_t>, std::size_t>;

  when_some_state(std::size_t required, scheduler_state &origin)
      : required_(required), claimed_(0), published_(0), results_(required), origin_(&origin) {}

  when_some_state(const when_some_state &) = delete;
  when_some_state &operator=(const when_some_state &) = delete;
  when_some_state(when_some_state &&) = delete;
  when_some_state &operator=(when_some_state &&) = delete;

  [[nodiscard]] bool completed() const noexcept {
    if constexpr (ThreadSafe)
      return this->published_.load(std::memory_order_acquire) >= this->required_;
    else
      return this->published_ >= this->required_;
  }

  [[nodiscard]] bool selection_complete() const noexcept {
    if constexpr (ThreadSafe)
      return this->claimed_.load(std::memory_order_relaxed) >= this->required_;
    else
      return this->claimed_ >= this->required_;
  }

  [[nodiscard]] std::optional<std::size_t> try_claim() noexcept {
    std::size_t rank;
    if constexpr (ThreadSafe)
      rank = this->claimed_.fetch_add(1, std::memory_order_relaxed);
    else
      rank = this->claimed_++;

    if (rank >= this->required_)
      return std::nullopt;
    return rank;
  }

  [[nodiscard]] bool publish() noexcept {
    std::size_t previous;
    if constexpr (ThreadSafe) {
      previous = this->published_.fetch_add(1, std::memory_order_release);
      assert(previous < this->required_);
      return previous + 1 == this->required_;
    } else {
      previous = this->published_++;
      assert(previous < this->required_);
      return previous + 1 == this->required_;
    }
  }

  void set_continuation(co_handle<> continuation) noexcept { this->continuation_ = continuation; }

  when_completion_entry<Variant> &entry(std::size_t rank) noexcept {
    assert(rank < this->results_.size());
    return this->results_[rank];
  }

  void resume_parent() noexcept {
    // Ready-queue exhaustion is currently a scheduler hard failure.
    this->origin_->push_handle(this->continuation_);
  }

  void rethrow_exceptions() const {
    for (const auto &entry : this->results_)
      entry.rethrow_if_exception();
  }

  result_type take_results() {
    result_type output;
    output.reserve(this->results_.size());
    for (auto &entry : this->results_)
      output.push_back(entry.take_result());
    return output;
  }

private:
  std::size_t required_;
  counter_type claimed_;
  counter_type published_;
  std::vector<when_completion_entry<Variant>> results_;
  co_handle<> continuation_;
  scheduler_state *origin_;
};

template <bool ThreadSafe, std::size_t Index, typename State, typename Awaitable>
task<void> when_some_evaluate(std::shared_ptr<State> state, Awaitable awaitable, scheduler &origin) noexcept {
  using result_type = when_all_await_result_t<Awaitable &&>;

  if (state->selection_complete())
    co_return;

  std::exception_ptr exception;
  std::optional<std::size_t> rank;

  if constexpr (std::is_void_v<result_type>) {
    try {
      co_await std::move(awaitable);
    } catch (...) {
      exception = std::current_exception();
    }

    rank = state->try_claim();
    if (!rank)
      co_return;

    if (exception)
      state->entry(*rank).set_exception(std::move(exception));
    else
      state->entry(*rank).template set_value<Index>();
  } else {
    try {
      decltype(auto) result = co_await std::move(awaitable);
      rank = state->try_claim();
      if (!rank)
        co_return;

      try {
        state->entry(*rank).template set_value<Index, result_type>(std::forward<decltype(result)>(result));
      } catch (...) {
        state->entry(*rank).set_exception(std::current_exception());
      }
    } catch (...) {
      rank = state->try_claim();
      if (!rank)
        co_return;
      state->entry(*rank).set_exception(std::current_exception());
    }
  }

  if constexpr (!ThreadSafe)
    assert(this_thread.sche == &origin && "when_some<false> child must complete on the origin scheduler");

  if (!state->publish())
    co_return;

  if constexpr (ThreadSafe) {
    // TODO: handle resume_on(origin) failure transactionally. The last
    // publisher owns the only parent wakeup.
    co_await ::mira::co::resume_on(origin);
  }

  state->resume_parent();
}

template <bool ThreadSafe, std::size_t Index, typename State, typename Awaitable>
void spawn_when_some_child(scheduler &origin, const std::shared_ptr<State> &state, Awaitable &&awaitable) noexcept {
  // TODO: make child creation/enqueue transactional. A hard failure currently
  // terminates instead of leaving a partially spawned combinator.
  auto child = when_some_evaluate<ThreadSafe, Index>(state, std::forward<Awaitable>(awaitable), origin);
  origin.co_spawn(std::move(child));
}

template <bool ThreadSafe, typename... Awaitables> using when_some_state_t = when_some_state<ThreadSafe, when_variant_t<Awaitables...>>;

template <typename... Awaitables> using when_some_result_t = std::vector<when_any_result<when_variant_t<Awaitables...>>>;

} // namespace mira::co::core

namespace mira::co {

// Returns the first required completions in completion order. Exceptions count
// as completions and are rethrown after the required set has been published.
// Awaitables outside that set are not actively cancelled.
template <bool ThreadSafe = true, typename... Awaitables>
task<core::when_some_result_t<Awaitables...>> when_some(std::size_t required, Awaitables... awaitables) {
  static_assert(sizeof...(Awaitables) != 0, "when_some requires at least one awaitable");

  if (required == 0 || required > sizeof...(Awaitables)) [[unlikely]] {
    log("when_some required completion count is out of range");
    throw std::invalid_argument("when_some required completion count is out of range");
  }

  if (core::this_thread.sche == nullptr || core::this_thread.sche_state == nullptr) [[unlikely]] {
    log("when_some requires a scheduler host thread");
    throw std::logic_error("when_some requires a scheduler host thread");
  }

  using state_type = core::when_some_state_t<ThreadSafe, Awaitables...>;
  auto state = std::make_shared<state_type>(required, *core::this_thread.sche_state);
  auto inputs = std::tuple<Awaitables...>{std::move(awaitables)...};
  scheduler &origin = *core::this_thread.sche;

  [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    (core::spawn_when_some_child<ThreadSafe, Index>(origin, state, std::move(std::get<Index>(inputs))), ...);
  }(std::index_sequence_for<Awaitables...>{});

  co_await core::when_shared_waiter<state_type>{state};
  state->rethrow_exceptions();
  co_return state->take_results();
}

} // namespace mira::co

#endif
