#ifndef CO_MIRA_IO_OWNED_AWAITER_HPP
#define CO_MIRA_IO_OWNED_AWAITER_HPP

#include "io/awaiter.hpp"
#include "io/unique_fd.hpp"

#include <utility>

namespace mira::co::core {

// Keeps descriptor ownership until the close CQE arrives. If SQE acquisition or
// submission throws, normal stack unwinding closes the descriptor synchronously.
// This wrapper deliberately is not a linkable io_awaiter: linked chains do not
// call await_resume() for every member, so they cannot finalize this ownership
// transfer safely yet.
struct [[nodiscard]] io_close_owned {
  explicit io_close_owned(io::unique_fd fd, const cancel_token *token = nullptr) noexcept : fd_(std::move(fd)), operation_(this->fd_.get(), token) {}

  static constexpr bool await_ready() noexcept { return false; }
  void await_suspend(co_handle<> handle) { this->operation_.await_suspend(handle); }

  int32_t await_resume() noexcept {
    const int32_t result = this->operation_.await_resume();
    if (result >= 0)
      (void)this->fd_.release();
    return result;
  }

private:
  io::unique_fd fd_;
  io_close operation_;
};

} // namespace mira::co::core

#endif
