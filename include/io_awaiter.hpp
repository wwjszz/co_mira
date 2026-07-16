#ifndef CO_MIRA_IO_AWAITER_HPP
#define CO_MIRA_IO_AWAITER_HPP

#include "scheduler.hpp"
#include "task_info.hpp"
#include "thread_meta.hpp"
#include "user_data.hpp"

#include <liburing.h>
#include <liburing/io_uring.h>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace mira::co::core {

struct [[nodiscard]] io_awaiter;

template <typename Action>
concept awaiter_action = std::derived_from<std::remove_cvref_t<Action>, io_awaiter> &&
                         requires(std::remove_cvref_t<Action> &action, io_uring_sqe *sqe) {
                           { action.prepare(sqe) } noexcept -> std::same_as<void>;
                         };

// TODO: get_free_sqe when await_suspend
struct io_awaiter {
  static constexpr bool await_ready() noexcept { return false; }
  void await_suspend(this awaiter_action auto &self, co_handle<> handle) noexcept {
    auto sqe = this_thread.sche_state->get_free_sqe();
    self.prepare(sqe);
    self.init_sqe(sqe);
    self.init_handle(handle);
  }
  int32_t await_resume() const noexcept { return this->io_info.result; }

  void init_sqe(io_uring_sqe *sqe) noexcept {
    sqe->user_data =
        this->io_info.as_user_data() | static_cast<uint64_t>(user_data::type::task_info_pointer);
  }
  void init_handle(co_handle<> handle) noexcept { this->io_info.handle = handle; }

  static constexpr void set_link(io_uring_sqe *sqe) noexcept {
    sqe->flags |= IOSQE_IO_LINK;
    sqe->user_data |= static_cast<uint64_t>(user_data::type::task_info_linked);
  }

  task_info io_info;

protected:
  io_awaiter(const io_awaiter &) = delete;
  io_awaiter &operator=(const io_awaiter &) = delete;

  io_awaiter(io_awaiter &&) = default;
  io_awaiter &operator=(io_awaiter &&) = default;

  io_awaiter() = default;
  ~io_awaiter() = default;
};

// TODO: submit once for all
template <typename... Awaiters>
  requires((std::is_base_of_v<io_awaiter, Awaiters> && awaiter_action<Awaiters>) && ...)
struct [[nodiscard]] linked_io_awaiter {
  static_assert(sizeof...(Awaiters) >= 2, "");
  static constexpr bool await_ready() noexcept { return false; }
  void await_suspend(co_handle<> handle) noexcept {
    constexpr std::size_t sz = sizeof...(Awaiters);

    auto sche_state = this_thread.sche_state;
    sche_state->ensure_sq_space(sz);

    [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (this->prepare_one<Index>(sche_state, handle), ...);
    }(std::index_sequence_for<Awaiters...>{});
  }
  int32_t await_resume() const noexcept {
    return std::get<sizeof...(Awaiters) - 1>(awaiters).io_info.result;
  }
  std::tuple<Awaiters...> awaiters;

  explicit linked_io_awaiter(Awaiters &&...elements) noexcept(
      std::is_nothrow_constructible_v<std::tuple<Awaiters...>, Awaiters &&...>)
      : awaiters(std::forward<Awaiters>(elements)...) {}

  explicit linked_io_awaiter(std::tuple<Awaiters...> &&other_awaiters) noexcept(
      std::is_nothrow_move_constructible_v<std::tuple<Awaiters...>>)
      : awaiters(std::move(other_awaiters)) {}

  ~linked_io_awaiter() = default;

  linked_io_awaiter(const linked_io_awaiter &) = delete;
  linked_io_awaiter &operator=(const linked_io_awaiter &) = delete;

  linked_io_awaiter(linked_io_awaiter &&) = delete;
  linked_io_awaiter &operator=(linked_io_awaiter &&) = delete;

private:
  template <std::size_t Index>
  void prepare_one(scheduler_state *sche_state, co_handle<> handle) noexcept {
    constexpr bool is_final = Index + 1 == sizeof...(Awaiters);

    auto &awaiter = std::get<Index>(awaiters);
    auto sqe = sche_state->get_free_sqe();
    awaiter.prepare(sqe);
    awaiter.init_sqe(sqe);

    if constexpr (is_final) {
      awaiter.init_handle(handle);
    } else {
      awaiter.set_link(sqe);
    }
  }
};

template <awaiter_action Left, awaiter_action Right>
  requires(!std::is_lvalue_reference_v<Left> && !std::is_lvalue_reference_v<Right>)
inline auto operator&&(Left &&left, Right &&right) {
  using LeftAction = std::remove_cvref_t<Left>;
  using RightAction = std::remove_cvref_t<Right>;

  return linked_io_awaiter<LeftAction, RightAction>{std::forward<Left>(left),
                                                    std::forward<Right>(right)};
}

template <awaiter_action... LeftAction, awaiter_action Right>
  requires(!std::is_lvalue_reference_v<Right>)
inline auto operator&&(linked_io_awaiter<LeftAction...> &&left, Right &&right) {
  using RightAction = std::remove_cvref_t<Right>;

  return linked_io_awaiter<LeftAction..., RightAction>(std::tuple_cat(
      std::move(left.awaiters), std::tuple<RightAction>(std::forward<Right>(right))));
}

template <awaiter_action... LeftAction, awaiter_action... RightAction>
inline auto operator&&(linked_io_awaiter<LeftAction...> &&left,
                       linked_io_awaiter<RightAction...> &&right) {
  return linked_io_awaiter<LeftAction..., RightAction...>(
      std::tuple_cat(std::move(left.awaiters), std::move(right.awaiters)));
}

struct [[nodiscard]] io_accept : io_awaiter {
  io_accept(int fd_, struct sockaddr *addr, socklen_t *addrlen, int flags) noexcept
      : fd_(fd_), addr_(addr), addrlen_(addrlen), flags_(flags) {}
  void prepare(io_uring_sqe *sqe) noexcept {
    io_uring_prep_accept(sqe, fd_, addr_, addrlen_, flags_);
  }

  int fd_;
  struct sockaddr *addr_;
  socklen_t *addrlen_;
  int flags_;
};

struct [[nodiscard]] io_close : io_awaiter {
  io_close(int fd_) noexcept : fd_(fd_) {}
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_close(sqe, fd_); }
  int fd_;
};

struct [[nodiscard]] io_readv : io_awaiter {
  io_readv(int fd, std::span<const iovec> iovecs, uint64_t offset) noexcept
      : fd_(fd), iovecs_(iovecs), offset_(offset) {}
  void prepare(io_uring_sqe *sqe) noexcept {
    io_uring_prep_readv(sqe, fd_, iovecs_.data(), iovecs_.size(), offset_);
  }

  int fd_;
  std::span<const iovec> iovecs_;
  uint64_t offset_;
};

struct [[nodiscard]] io_writev : io_awaiter {
  io_writev(int fd, std::span<const iovec> iovecs, uint64_t offset) noexcept
      : fd_(fd), iovecs_(iovecs), offset_(offset) {}
  void prepare(io_uring_sqe *sqe) noexcept {
    io_uring_prep_writev(sqe, fd_, iovecs_.data(), iovecs_.size(), offset_);
  }

  int fd_;
  std::span<const iovec> iovecs_;
  uint64_t offset_;
};

struct [[nodiscard]] io_recv : io_awaiter {
  io_recv(int fd, std::span<char> buf, int flag) noexcept : fd_(fd), buf_(buf), flag_(flag) {}
  void prepare(io_uring_sqe *sqe) noexcept {
    io_uring_prep_recv(sqe, fd_, buf_.data(), buf_.size(), flag_);
  }

  int fd_;
  std::span<char> buf_;
  int flag_;
};

struct [[nodiscard]] io_send : io_awaiter {
  io_send(int fd, std::span<char> buf, int flag) noexcept : fd_(fd), buf_(buf), flag_(flag) {}
  void prepare(io_uring_sqe *sqe) noexcept {
    io_uring_prep_send(sqe, fd_, buf_.data(), buf_.size(), flag_);
  }

  int fd_;
  std::span<char> buf_;
  int flag_;
};

} // namespace mira::co::core

#endif