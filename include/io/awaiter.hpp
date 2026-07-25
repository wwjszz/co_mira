#ifndef CO_MIRA_IO_AWAITER_HPP
#define CO_MIRA_IO_AWAITER_HPP

#include "detail/core.hpp"
#include "detail/task_info.hpp"
#include "detail/thread_meta.hpp"
#include "detail/user_data.hpp"
#include "scheduler.hpp"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <concepts>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <limits>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <tuple>
#include <type_traits>
#include <utility>

namespace mira::co::core {

struct [[nodiscard]] io_awaiter;
template <typename Awaiter> struct [[nodiscard]] io_with_timeout;

template <typename Action>
concept awaiter_action = requires(std::remove_cvref_t<Action> &action, io_uring_sqe *sqe) {
  { action.prepare(sqe) } noexcept -> std::same_as<void>;
};

template <typename Action>
concept linkable_action = std::derived_from<std::remove_cvref_t<Action>, io_awaiter> && awaiter_action<Action>;

// Low-level public IO result policy:
//   * cqe->res >= 0 is returned unchanged.
//   * cqe->res < 0 is returned as a negative errno; it is not converted to an
//     exception.
// Ring/control-plane failures such as submit, wait, and queue initialization
// still throw std::system_error because the scheduler cannot represent them as
// the result of one particular operation.
struct io_awaiter {
  template <typename... Awaiters>
    requires((std::is_base_of_v<io_awaiter, Awaiters> && linkable_action<Awaiters>) && ...)
  friend struct linked_io_awaiter;

  template <typename Awaiter> friend struct io_with_timeout;

  static constexpr bool await_ready() noexcept { return false; }
  void await_suspend(this awaiter_action auto &self, co_handle<> handle) {
    self.validate_cancel_token();
    auto sqe = this_thread.sche_state->get_free_sqe();
    self.prep_sqe(sqe, handle);
  }

  int32_t await_resume() const noexcept { return this->io_info.result; }

protected:
  void validate_cancel_token() const {
    if (this->cancel_state_ && this->cancel_state_->phase_ != cancel_state::phase::unbound) [[unlikely]] {
      log("cancel token is already bound to an operation");
      throw std::logic_error("cancel token is already bound to an operation");
    }
  }

  void prep_sqe(this awaiter_action auto &self, io_uring_sqe *sqe, co_handle<> handle) noexcept {
    self.prepare(sqe);
    self.init_sqe(sqe);
    self.init_handle(handle);
    self.bind_cancel_token(sqe);
  }

  void prep_sqe_link(this awaiter_action auto &self, io_uring_sqe *sqe) noexcept {
    self.prepare(sqe);
    self.init_sqe(sqe);
    self.set_link(sqe);
    self.bind_cancel_token(sqe);
  }

  void bind_cancel_token(io_uring_sqe *sqe) noexcept {
    if (auto &state = this->cancel_state_) {
      state->mark_submitted(sqe->user_data);
      this->io_info.cancel = state.get();
    }
  }

  int32_t result() const noexcept { return this->io_info.result; }
  /*  */
  void init_sqe(io_uring_sqe *sqe) noexcept {
    sqe->user_data = this->io_info.as_user_data() | static_cast<uint64_t>(user_data::type::task_info_pointer);
  }
  void init_handle(co_handle<> handle) noexcept { this->io_info.handle = handle; }

  static constexpr void set_link(io_uring_sqe *sqe) noexcept {
    sqe->flags |= IOSQE_IO_LINK;
    sqe->user_data |= static_cast<uint64_t>(user_data::type::task_info_linked);
  }

  io_awaiter(const io_awaiter &) = delete;
  io_awaiter &operator=(const io_awaiter &) = delete;

  io_awaiter(io_awaiter &&) = default;
  io_awaiter &operator=(io_awaiter &&) = delete;

  io_awaiter(const cancel_token *token = nullptr) noexcept : cancel_state_(token ? token->state_ : nullptr) {}
  ~io_awaiter() = default;

private:
  task_info io_info;
  std::shared_ptr<cancel_state> cancel_state_;
};

// An owning soft-link chain. operator&& maps to IOSQE_IO_LINK: an earlier
// failure cancels the remaining requests, normally producing -ECANCELED for
// them. await_resume() currently returns only the final request's raw cqe->res,
// so it may hide the original failure. IOSQE_IO_HARDLINK and link timeout are
// intentionally not exposed until named APIs and a complete result view exist.
template <typename... Awaiters>
  requires((std::is_base_of_v<io_awaiter, Awaiters> && linkable_action<Awaiters>) && ...)
struct [[nodiscard]] linked_io_awaiter {

  template <linkable_action Left, linkable_action Right>
    requires(!std::is_lvalue_reference_v<Left> && !std::is_lvalue_reference_v<Right>)
  friend inline auto operator&&(Left &&, Right &&);

  template <linkable_action... LeftAction, linkable_action Right>
    requires(!std::is_lvalue_reference_v<Right>)
  friend inline auto operator&&(linked_io_awaiter<LeftAction...> &&, Right &&);

  template <linkable_action... LeftAction, linkable_action... RightAction>
  friend inline auto operator&&(linked_io_awaiter<LeftAction...> &&, linked_io_awaiter<RightAction...> &&);

  static_assert(sizeof...(Awaiters) >= 2, "");

  static constexpr bool await_ready() noexcept { return false; }
  void await_suspend(co_handle<> handle) { this->prepare_all(handle); }

  int32_t await_resume() const noexcept { return this->result<sizeof...(Awaiters) - 1>(); }

  template <std::size_t Index> int32_t result() const noexcept {
    static_assert(Index < sizeof...(Awaiters), "");
    return std::get<Index>(awaiters).result();
  }

  explicit linked_io_awaiter(Awaiters &&...elements) noexcept(std::is_nothrow_constructible_v<std::tuple<Awaiters...>, Awaiters &&...>)
      : awaiters(std::forward<Awaiters>(elements)...) {}

  explicit linked_io_awaiter(std::tuple<Awaiters...> &&other_awaiters) noexcept(std::is_nothrow_move_constructible_v<std::tuple<Awaiters...>>)
      : awaiters(std::move(other_awaiters)) {}

  ~linked_io_awaiter() = default;

  linked_io_awaiter(const linked_io_awaiter &) = delete;
  linked_io_awaiter &operator=(const linked_io_awaiter &) = delete;

  linked_io_awaiter(linked_io_awaiter &&) = default;
  linked_io_awaiter &operator=(linked_io_awaiter &&) = delete;

private:
  std::tuple<Awaiters...> awaiters;

  void prepare_all(co_handle<> handle) {
    constexpr std::size_t sz = sizeof...(Awaiters);

    auto sche_state = this_thread.sche_state;
    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (std::get<Index>(awaiters).validate_cancel_token(), ...);
    }(std::index_sequence_for<Awaiters...>{});
    sche_state->ensure_sq_space(sz);

    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (this->prepare_one<Index>(sche_state, handle), ...);
    }(std::index_sequence_for<Awaiters...>{});
  }

  template <std::size_t Index> void prepare_one(scheduler_state *sche_state, co_handle<> handle) {
    constexpr bool is_final = Index + 1 == sizeof...(Awaiters);

    auto &awaiter = std::get<Index>(awaiters);
    auto sqe = sche_state->get_free_sqe();

    if constexpr (is_final) {
      awaiter.prep_sqe(sqe, handle);
    } else {
      awaiter.prep_sqe_link(sqe);
    }
  }
};

template <linkable_action Left, linkable_action Right>
  requires(!std::is_lvalue_reference_v<Left> && !std::is_lvalue_reference_v<Right>)
inline auto operator&&(Left &&left, Right &&right) {
  using LeftAction = std::remove_cvref_t<Left>;
  using RightAction = std::remove_cvref_t<Right>;

  return linked_io_awaiter<LeftAction, RightAction>{std::forward<Left>(left), std::forward<Right>(right)};
}

template <linkable_action... LeftAction, linkable_action Right>
  requires(!std::is_lvalue_reference_v<Right>)
inline auto operator&&(linked_io_awaiter<LeftAction...> &&left, Right &&right) {
  using RightAction = std::remove_cvref_t<Right>;

  return linked_io_awaiter<LeftAction..., RightAction>(std::tuple_cat(std::move(left.awaiters), std::tuple<RightAction>(std::forward<Right>(right))));
}

template <linkable_action... LeftAction, linkable_action... RightAction>
inline auto operator&&(linked_io_awaiter<LeftAction...> &&left, linked_io_awaiter<RightAction...> &&right) {
  return linked_io_awaiter<LeftAction..., RightAction...>(std::tuple_cat(std::move(left.awaiters), std::move(right.awaiters)));
}

struct [[nodiscard]] io_nop : io_awaiter {
  io_nop(const cancel_token *token = nullptr) noexcept : io_awaiter(token) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_nop(sqe); }
};

struct [[nodiscard]] io_accept : io_awaiter {
  io_accept(int fd_, struct sockaddr *addr, socklen_t *addrlen, int flags, const cancel_token *token = nullptr) noexcept
      : io_awaiter(token), fd_(fd_), addr_(addr), addrlen_(addrlen), flags_(flags) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_accept(sqe, this->fd_, this->addr_, this->addrlen_, this->flags_); }

private:
  int fd_;
  struct sockaddr *addr_;
  socklen_t *addrlen_;
  int flags_;
};

struct [[nodiscard]] io_connect : io_awaiter {
  io_connect(int fd, const struct sockaddr *addr, socklen_t addrlen, const cancel_token *token = nullptr) noexcept
      : io_awaiter(token), fd_(fd), addr_(addr), addrlen_(addrlen) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_connect(sqe, this->fd_, this->addr_, this->addrlen_); }

private:
  int fd_;
  const struct sockaddr *addr_;
  socklen_t addrlen_;
};

struct [[nodiscard]] io_close : io_awaiter {
  explicit io_close(int fd_, const cancel_token *token = nullptr) noexcept : io_awaiter(token), fd_(fd_) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_close(sqe, this->fd_); }

private:
  int fd_;
};

struct [[nodiscard]] io_readv : io_awaiter {
  io_readv(int fd, std::span<const iovec> iovecs, uint64_t offset, const cancel_token *token = nullptr) noexcept
      : io_awaiter(token), fd_(fd), iovecs_(iovecs), offset_(offset) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_readv(sqe, this->fd_, this->iovecs_.data(), this->iovecs_.size(), this->offset_); }

private:
  int fd_;
  std::span<const iovec> iovecs_;
  uint64_t offset_;
};

struct [[nodiscard]] io_writev : io_awaiter {
  io_writev(int fd, std::span<const iovec> iovecs, uint64_t offset, const cancel_token *token = nullptr) noexcept
      : io_awaiter(token), fd_(fd), iovecs_(iovecs), offset_(offset) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_writev(sqe, this->fd_, this->iovecs_.data(), this->iovecs_.size(), this->offset_); }

private:
  int fd_;
  std::span<const iovec> iovecs_;
  uint64_t offset_;
};

struct [[nodiscard]] io_recv : io_awaiter {
  io_recv(int fd, std::span<char> buf, int flags, const cancel_token *token = nullptr) noexcept : io_awaiter(token), fd_(fd), buf_(buf), flags_(flags) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_recv(sqe, this->fd_, this->buf_.data(), this->buf_.size(), this->flags_); }

private:
  int fd_;
  std::span<char> buf_;
  int flags_;
};

struct [[nodiscard]] io_send : io_awaiter {
  io_send(int fd, std::span<const char> buf, int flags, const cancel_token *token = nullptr) noexcept
      : io_awaiter(token), fd_(fd), buf_(buf), flags_(flags) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_send(sqe, this->fd_, this->buf_.data(), this->buf_.size(), this->flags_); }

private:
  int fd_;
  std::span<const char> buf_;
  int flags_;
};

struct [[nodiscard]] io_recvmsg : io_awaiter {
  io_recvmsg(int fd, msghdr *msg, unsigned int flags, const cancel_token *token = nullptr) noexcept
      : io_awaiter(token), fd_(fd), msg_(msg), flags_(flags) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_recvmsg(sqe, fd_, msg_, flags_); }

private:
  int fd_;
  msghdr *msg_;
  unsigned int flags_;
};

struct [[nodiscard]] io_sendmsg : io_awaiter {
  io_sendmsg(int fd, msghdr *msg, unsigned int flags, const cancel_token *token = nullptr) noexcept
      : io_awaiter(token), fd_(fd), msg_(msg), flags_(flags) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_sendmsg(sqe, fd_, msg_, flags_); }

private:
  int fd_;
  msghdr *msg_;
  unsigned int flags_;
};

struct [[nodiscard]] io_cancel : io_awaiter {
  explicit io_cancel(const cancel_token &token) : target_state_(token.state_) {
    if (!this->target_state_) {
      log("cannot cancel with a moved-from cancel token");
      throw std::logic_error("cannot cancel with a moved-from cancel token");
    }

    if (this->target_state_->phase_ == cancel_state::phase::unbound) {
      log("cannot cancel an operation before submission");
      throw std::logic_error("cannot cancel an operation before submission");
    }
  }

  bool await_ready() noexcept {
    this->completed_before_submit_ = this->target_state_->completed();

    return this->completed_before_submit_;
  }

  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_cancel64(sqe, this->target_state_->user_data_, 0); }

  int32_t await_resume() const noexcept {
    if (this->completed_before_submit_)
      return -ENOENT;

    return io_awaiter::await_resume();
  }

private:
  std::shared_ptr<cancel_state> target_state_;
  bool completed_before_submit_ = false;
};

template <typename Rep, typename Period> __kernel_timespec to_kernel_timespec(std::chrono::duration<Rep, Period> duration) {
  using seconds_ld = std::chrono::duration<long double>;

  const long double total_seconds = seconds_ld(duration).count();

  if (!std::isfinite(total_seconds)) {
    log("timeout duration is not finite");
    throw std::invalid_argument("timeout duration is not finite");
  }

  long double whole_seconds = 0;
  const long double fractional_seconds = std::modf(total_seconds, &whole_seconds);

  using sec_type = decltype(__kernel_timespec::tv_sec);
  constexpr auto min_seconds = static_cast<long double>(std::numeric_limits<sec_type>::min());
  constexpr auto max_seconds = static_cast<long double>(std::numeric_limits<sec_type>::max());

  if (whole_seconds < min_seconds || whole_seconds > max_seconds) {
    log("timeout duration exceeds __kernel_timespec range");
    throw std::overflow_error("timeout duration exceeds __kernel_timespec range");
  }

  constexpr long double nanoseconds_per_second = 1'000'000'000.0L;

  return {
      .tv_sec = static_cast<sec_type>(whole_seconds),
      .tv_nsec = static_cast<decltype(__kernel_timespec::tv_nsec)>(fractional_seconds * nanoseconds_per_second),
  };
}

struct [[nodiscard]] io_timeout_base : io_awaiter {
protected:
  io_timeout_base(__kernel_timespec ts, unsigned int count, unsigned int flags, const cancel_token *token = nullptr) noexcept
      : io_awaiter(token), ts_(ts), count_(count), flags_(flags) {}

  template <typename Rep, typename Period>
  io_timeout_base(std::chrono::duration<Rep, Period> duration, unsigned int count, unsigned int flags, const cancel_token *token = nullptr)
      : io_awaiter(token), ts_(to_kernel_timespec(duration)), count_(count), flags_(flags) {}

  __kernel_timespec ts_;
  unsigned int count_;
  unsigned int flags_;
};

struct [[nodiscard]] io_timeout : io_timeout_base {
  io_timeout(__kernel_timespec ts, unsigned int count, unsigned int flags, const cancel_token *token = nullptr) noexcept
      : io_timeout_base(ts, count, flags, token) {}

  template <typename Rep, typename Period>
  io_timeout(std::chrono::duration<Rep, Period> duration, unsigned int count, unsigned int flags, const cancel_token *token = nullptr)
      : io_timeout_base(duration, count, flags, token) {}

  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_timeout(sqe, &this->ts_, this->count_, this->flags_); }
};

struct [[nodiscard]] io_link_timeout : io_timeout_base {
  io_link_timeout(__kernel_timespec ts, unsigned int count, unsigned int flags, const cancel_token *token = nullptr) noexcept
      : io_timeout_base(ts, count, flags, token) {}

  template <typename Rep, typename Period>
  io_link_timeout(std::chrono::duration<Rep, Period> duration, unsigned int count, unsigned int flags, const cancel_token *token = nullptr)
      : io_timeout_base(duration, count, flags, token) {}

  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_link_timeout(sqe, &this->ts_, this->flags_); }
};

struct [[nodiscard]] io_with_timeout_result {
  int32_t action_result;
  int32_t timeout_result;

  bool timed_out() const noexcept { return timeout_result == -ETIME; }
};

template <typename Awaiter> struct io_with_timeout {
  static_assert(linkable_action<Awaiter>, "Awaiter must be a linkable IO action");

  io_with_timeout(Awaiter &&awaiter, __kernel_timespec ts, unsigned int count, unsigned int flags,
                  const cancel_token *token = nullptr) noexcept(std::is_nothrow_move_constructible_v<Awaiter>)
      : awaiter_(std::move(awaiter)), timeout_(ts, count, flags, token) {}

  template <typename Rep, typename Period>
  io_with_timeout(Awaiter &&awaiter, std::chrono::duration<Rep, Period> duration, unsigned int count, unsigned int flags,
                  const cancel_token *token = nullptr)
      : awaiter_(std::move(awaiter)), timeout_(duration, count, flags, token) {}

  static constexpr bool await_ready() noexcept { return false; }

  void await_suspend(co_handle<> handle) {
    auto &sche_state = this_thread.sche_state;
    this->awaiter_.validate_cancel_token();
    this->timeout_.validate_cancel_token();
    sche_state->ensure_sq_space(2);

    auto sqe1 = sche_state->get_free_sqe();
    this->awaiter_.prep_sqe_link(sqe1);

    auto sqe2 = sche_state->get_free_sqe();
    this->timeout_.prep_sqe(sqe2, handle);
  }
  /*  */
  io_with_timeout_result await_resume() const noexcept {
    return io_with_timeout_result{.action_result = awaiter_.io_info.result, .timeout_result = timeout_.io_info.result};
  }

  io_with_timeout(const io_with_timeout &) = delete;
  io_with_timeout &operator=(const io_with_timeout &) = delete;

  io_with_timeout(io_with_timeout &&) noexcept(std::is_nothrow_move_constructible_v<Awaiter>) = default;
  io_with_timeout &operator=(io_with_timeout &&) = delete;

private:
  Awaiter awaiter_;
  io_link_timeout timeout_;
};

template <typename... Awaiters> struct [[nodiscard]] io_with_timeout<linked_io_awaiter<Awaiters...>> {
  using InputAwaiter = linked_io_awaiter<Awaiters...>;

  using TimedAwaiter = linked_io_awaiter<Awaiters..., io_link_timeout>;

  io_with_timeout(InputAwaiter &&awaiter, __kernel_timespec ts, unsigned int count, unsigned int flags,
                  const cancel_token *token = nullptr) noexcept(std::is_nothrow_move_constructible_v<TimedAwaiter>)
      : awaiter_(std::move(awaiter) && io_link_timeout(ts, count, flags, token)) {}

  template <typename Rep, typename Period>
  io_with_timeout(InputAwaiter &&awaiter, std::chrono::duration<Rep, Period> duration, unsigned int count, unsigned int flags,
                  const cancel_token *token = nullptr)
      : awaiter_(std::move(awaiter) && io_link_timeout(duration, count, flags, token)) {}

  static constexpr bool await_ready() noexcept { return false; }

  void await_suspend(co_handle<> handle) { this->awaiter_.await_suspend(handle); }

  io_with_timeout_result await_resume() const noexcept {
    return io_with_timeout_result{
        .action_result = this->awaiter_.template result<sizeof...(Awaiters) - 1>(),
        .timeout_result = this->awaiter_.template result<sizeof...(Awaiters)>(),
    };
  }

  io_with_timeout(const io_with_timeout &) = delete;
  io_with_timeout &operator=(const io_with_timeout &) = delete;

  io_with_timeout(io_with_timeout &&) noexcept(std::is_nothrow_move_constructible_v<TimedAwaiter>) = default;
  io_with_timeout &operator=(io_with_timeout &&) = delete;

private:
  TimedAwaiter awaiter_;
};

// TODO: support multishot operation
#if 0
struct [[nodiscard]] io_multishot_accept : io_awaiter {
  io_multishot_accept(int fd_, struct sockaddr *addr, socklen_t *addrlen, int flags) noexcept
      : fd_(fd_), addr_(addr), addrlen_(addrlen), flags_(flags) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_multishot_accept(sqe, this->fd_, this->addr_, this->addrlen_, this->flags_); }

private:
  int fd_;
  struct sockaddr *addr_;
  socklen_t *addrlen_;
  int flags_;
};

struct [[nodiscard]] io_multishot_recv : io_awaiter {
  io_multishot_recv(int fd, std::span<char> buf, int flags) noexcept : fd_(fd), buf_(buf), flags_(flags) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_recv_multishot(sqe, this->fd_, this->buf_.data(), this->buf_.size(), this->flags_); }

private:
  int fd_;
  std::span<char> buf_;
  int flags_;
};

struct [[nodiscard]] io_multishot_recvmsg : io_awaiter {
  io_multishot_recvmsg(int fd, msghdr *msg, unsigned int flags) noexcept : fd_(fd), msg_(msg), flags_(flags) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_recvmsg_multishot(sqe, fd_, msg_, flags_); }

private:
  int fd_;
  msghdr *msg_;
  unsigned int flags_;
};
#endif

} // namespace mira::co::core

#endif
