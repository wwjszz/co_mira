#ifndef CO_MIRA_DETAIL_TASK_INFO_HPP
#define CO_MIRA_DETAIL_TASK_INFO_HPP
#include "detail/core.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace mira::co::core {

struct cancel_state {
  enum class phase {
    unbound,
    submitted,
    completed,
  };

  [[nodiscard]] bool submitted() const noexcept { return this->phase_ == phase::submitted; }

  [[nodiscard]] bool completed() const noexcept { return this->phase_ == phase::completed; }

private:
  friend struct cancel_token;
  friend struct io_awaiter;
  friend struct io_cancel;
  friend struct scheduler_state;

  void mark_completed() noexcept { this->phase_ = phase::completed; }

  void mark_submitted(uint64_t user_data) noexcept {
    assert(this->phase_ == phase::unbound);
    this->user_data_ = user_data;
    this->phase_ = phase::submitted;
  }

  uint64_t user_data_ = 0;
  phase phase_ = phase::unbound;
};

struct [[nodiscard]] cancel_token {
  friend struct io_awaiter;
  friend struct io_cancel;

public:
  cancel_token() : state_(std::make_shared<cancel_state>()) {}
  ~cancel_token() = default;

  cancel_token(const cancel_token &) = default;
  cancel_token &operator=(const cancel_token &) = default;

  cancel_token(cancel_token &&) noexcept = default;
  cancel_token &operator=(cancel_token &&) noexcept = default;

  [[nodiscard]] bool submitted() const noexcept { return this->state_ && this->state_->submitted(); }

  [[nodiscard]] bool completed() const noexcept { return this->state_ && this->state_->completed(); }

private:
  friend struct cancel_source;

  explicit cancel_token(std::shared_ptr<cancel_state> state) noexcept : state_(std::move(state)) {}

  std::shared_ptr<cancel_state> state_;
};

struct [[nodiscard]] cancel_source {
  cancel_source() : state_(std::make_shared<cancel_state>()) {}
  ~cancel_source() = default;

  cancel_source(const cancel_source &) = delete;
  cancel_source &operator=(const cancel_source &) = delete;

  cancel_source(cancel_source &&) noexcept = default;
  cancel_source &operator=(cancel_source &&) noexcept = default;

  [[nodiscard]] cancel_token get_token() const {
    if (!this->state_) {
      log("cannot get a token from a moved-from cancel source");
      throw std::logic_error("cannot get a token from a moved-from cancel source");
    }

    return cancel_token{this->state_};
  }

  [[nodiscard]] bool submitted() const noexcept { return this->state_ && this->state_->submitted(); }

  [[nodiscard]] bool completed() const noexcept { return this->state_ && this->state_->completed(); }

private:
  std::shared_ptr<cancel_state> state_;
};

struct linked_completion_state {
  void start(co_handle<> continuation, uint32_t completion_count) noexcept {
    assert(completion_count != 0);
    assert(this->remaining == 0);
    this->continuation = continuation;
    this->remaining = completion_count;
  }

  [[nodiscard]] bool complete_one() noexcept {
    assert(this->remaining != 0);
    return --this->remaining == 0;
  }

  co_handle<> continuation{};
  uint32_t remaining = 0;
};

// The low 3 bits of any task_info address are always 0
struct task_info {
  co_handle<> handle{};
  int32_t result{};
  cancel_state *cancel = nullptr;
  linked_completion_state *completion = nullptr;

  [[nodiscard]] uint64_t as_user_data() const noexcept { return reinterpret_cast<uint64_t>(this); }

  [[nodiscard]] static task_info *from_user_data(uint64_t user_data) noexcept;
};

static_assert(alignof(task_info) == 8);

inline constexpr uint64_t task_info_mask = ~static_cast<uint64_t>(alignof(task_info) - 1);

inline task_info *task_info::from_user_data(uint64_t user_data) noexcept { return reinterpret_cast<task_info *>(user_data & task_info_mask); }

} // namespace mira::co::core

#endif
