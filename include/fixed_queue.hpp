#ifndef CO_MIRA_FIXED_QUEUE_HPP
#define CO_MIRA_FIXED_QUEUE_HPP
#include <array>
#include <bit>
#include <cassert>
#include <concepts>
#include <limits>

namespace mira {

template <typename T, std::unsigned_integral Cap_Type, Cap_Type Cap
          // = (Cap_Type(1) << (sizeof(Cap_Type) * 8 - 1))
          >
struct fixed_queue {
  static_assert(std::has_single_bit(Cap));

  using index_t = Cap_Type;

  static constexpr Cap_Type mask = Cap - 1;

  constexpr index_t head() const noexcept { return head_; }
  constexpr index_t tail() const noexcept { return tail_; }
  constexpr bool empty() const noexcept { return head_ == tail_; }
  constexpr bool full() const noexcept { return next_tail() == head_; }
  constexpr index_t size() const noexcept { return (tail_ - head_ + Cap) & mask; }

  constexpr index_t advance_tail() noexcept {
    index_t cur_tail = tail_;
    tail_ = next_tail();
    return cur_tail;
  }

  constexpr index_t advance_head() noexcept {
    index_t cur_head = head_;
    head_ = next_head();
    return cur_head;
  }

  constexpr index_t next_tail() const noexcept { return (tail_ + 1) & mask; }
  constexpr index_t next_head() const noexcept { return (head_ + 1) & mask; }

  constexpr void push(T &ele) {
    assert(!full() && "fixed_queue is full");
    array_[advance_tail()] = ele;
  }

  constexpr void push(T &&ele) {
    assert(!full() && "fixed_queue is full");
    array_[advance_tail()] = std::move(ele);
  }

  constexpr T pop() {
    assert(!empty() && "fixed_queue is empty");
    return array_[advance_head()];
  }

private:
  index_t head_ = 0;
  index_t tail_ = 0;
  std::array<T, Cap> array_;
};

} // namespace mira

#endif