#ifndef CO_MIRA_DETAIL_FIXED_QUEUE_HPP
#define CO_MIRA_DETAIL_FIXED_QUEUE_HPP
#include "log.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <concepts>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mira {

// A power-of-two ring buffer that reserves one slot to distinguish full from
// empty. The maximum number of stored elements is therefore Cap - 1.
template <typename T, std::unsigned_integral Cap_Type, Cap_Type Cap> struct fixed_queue {
  static_assert(std::has_single_bit(Cap));
  static_assert(std::is_nothrow_move_constructible_v<T>, "fixed_queue::pop requires a nothrow move-constructible value type");

  using index_t = Cap_Type;

  [[nodiscard]] constexpr bool empty() const noexcept { return this->head_ == this->tail_; }
  [[nodiscard]] constexpr bool full() const noexcept { return next_tail() == this->head_; }
  [[nodiscard]] constexpr index_t size() const noexcept { return (this->tail_ - this->head_ + Cap) & this->mask; }

  template <typename Value> [[nodiscard]] constexpr bool try_push(Value &&val) noexcept(std::is_nothrow_assignable_v<T &, Value &&>) {
    if (full()) [[unlikely]]
      return false;

    auto index = this->advance_tail();
    if constexpr (std::is_nothrow_assignable_v<T &, Value &&>) {
      this->array_[index] = std::forward<Value>(val);
    } else {
      try {
        this->array_[index] = std::forward<Value>(val);
      } catch (...) {
        this->decline_tail();
        co::log("fixed_queue assignment failed; rolling back push");
        throw;
      }
    }
    return true;
  }

  template <typename Value> constexpr void push(Value &&val) {
    if (!this->try_push(std::forward<Value>(val))) [[unlikely]] {
      co::log("fixed_queue is full");
      throw std::length_error("fixed_queue is full");
    }
  }

  [[nodiscard]] constexpr T pop() {
    if (this->empty()) [[unlikely]] {
      co::log("fixed_queue is empty");
      throw std::out_of_range("fixed_queue is empty");
    }
    return std::move(this->array_[this->advance_head()]);
  }

private:
  static constexpr Cap_Type mask = Cap - 1;

  [[nodiscard]] constexpr index_t head() const noexcept { return this->head_; }
  [[nodiscard]] constexpr index_t tail() const noexcept { return this->tail_; }

  [[nodiscard]] constexpr index_t next_tail() const noexcept { return (this->tail_ + 1) & this->mask; }

  [[nodiscard]] constexpr index_t next_head() const noexcept { return (this->head_ + 1) & this->mask; }

  [[nodiscard]] constexpr index_t advance_tail() noexcept {
    index_t cur_tail = this->tail_;
    this->tail_ = next_tail();
    return cur_tail;
  }

  [[nodiscard]] constexpr index_t advance_head() noexcept {
    index_t cur_head = this->head_;
    this->head_ = next_head();
    return cur_head;
  }

  constexpr void decline_tail() noexcept { this->tail_ = (this->tail_ - 1) & this->mask; }

  index_t head_ = 0;
  index_t tail_ = 0;
  std::array<T, Cap> array_;
};

} // namespace mira

#endif
