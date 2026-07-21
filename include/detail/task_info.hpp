#ifndef CO_MIRA_DETAIL_TASK_INFO_HPP
#define CO_MIRA_DETAIL_TASK_INFO_HPP
#include "detail/core.hpp"

#include <cassert>
#include <cstdint>

namespace mira::co::core {

struct [[nodiscard]] cancel_token {
  friend struct io_awaiter;
  friend struct io_cancel;
  friend struct scheduler_state;

public:
  enum class state {
    unbound,
    submitted,
    completed,
  };

  cancel_token() = default;
  ~cancel_token() = default;

  cancel_token(const cancel_token &) = delete;
  cancel_token &operator=(const cancel_token &) = delete;

  cancel_token(cancel_token &&) = delete;
  cancel_token &operator=(cancel_token &&) = delete;

  [[nodiscard]] bool submitted() const noexcept { return this->state_ == state::submitted; }

  [[nodiscard]] bool completed() const noexcept { return this->state_ == state::completed; }

private:
  void mark_completed() noexcept { this->state_ = state::completed; }

  void mark_submitted() noexcept {
    assert(this->state_ == state::unbound);
    this->state_ = state::submitted;
  }

  uint64_t user_data_ = 0;
  state state_ = state::unbound;
};

// The low 3 bits of any task_info address are always 0
struct task_info {
  co_handle<> handle{};
  int32_t result{};
  cancel_token *token = nullptr;

  [[nodiscard]] uint64_t as_user_data() const noexcept { return reinterpret_cast<uint64_t>(this); }

  [[nodiscard]] static task_info *from_user_data(uint64_t user_data) noexcept;
};

static_assert(alignof(task_info) == 8);

inline constexpr uint64_t task_info_mask = ~static_cast<uint64_t>(alignof(task_info) - 1);

inline task_info *task_info::from_user_data(uint64_t user_data) noexcept { return reinterpret_cast<task_info *>(user_data & task_info_mask); }

} // namespace mira::co::core

#endif
