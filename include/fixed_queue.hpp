#ifndef CO_MIRA_FIXED_QUEUE_HPP
#define CO_MIRA_FIXED_QUEUE_HPP
#include <array>
#include <bit>
#include <cassert>
#include <concepts>
#include <exception>
#include <limits>
#include <type_traits>
#include <utility>

namespace mira {

template <typename T, std::unsigned_integral Cap_Type, Cap_Type Cap
          // = (Cap_Type(1) << (sizeof(Cap_Type) * 8 - 1))
          >
struct fixed_queue {
  static_assert(std::has_single_bit(Cap));

  using index_t = Cap_Type;

  static constexpr Cap_Type mask = Cap - 1;

  [[nodiscard]] constexpr index_t head() const noexcept { return this->head_; }
  [[nodiscard]] constexpr index_t tail() const noexcept { return this->tail_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return this->head_ == this->tail_; }
  [[nodiscard]] constexpr bool full() const noexcept { return next_tail() == this->head_; }
  [[nodiscard]] constexpr index_t size() const noexcept {
    return (this->tail_ - this->head_ + Cap) & this->mask;
  }

  [[nodiscard]] constexpr index_t next_tail() const noexcept {
    return (this->tail_ + 1) & this->mask;
  }
  [[nodiscard]] constexpr index_t next_head() const noexcept {
    return (this->head_ + 1) & this->mask;
  }

  template <typename Value>
  [[nodiscard]] constexpr bool
  try_push(Value &&val) noexcept(std::is_nothrow_assignable_v<T &, Value &&>) {
    if (full()) [[unlikely]]
      return false;

    this->array_[this->advance_tail()] = std::forward<Value>(val);
    return true;
  }

  template <typename Value>
  constexpr void push(Value &&val) noexcept(noexcept(this->try_push(std::forward<Value>(val)))) {
    if (!this->try_push(std::forward<Value>(val)))
      std::terminate();
  }

  [[nodiscard]] constexpr T pop() {
    assert(!this->empty() && "fixed_queue is empty");
    return this->array_[this->advance_head()];
  }

private:
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

  index_t head_ = 0;
  index_t tail_ = 0;
  std::array<T, Cap> array_;
};

} // namespace mira

#endif