#ifndef CO_MIRA_IO_AWAITER_HPP
#define CO_MIRA_IO_AWAITER_HPP

#include "scheduler.hpp"
#include "task_info.hpp"
#include "thread_meta.hpp"
#include "user_data.hpp"

#include <liburing.h>
#include <span>

namespace mira::co::core {

struct io_awaiter {
  io_awaiter() noexcept : sqe(this_thread.sche_state->get_free_sqe()) {
    sqe->user_data =
        io_info.as_user_data() | static_cast<uint64_t>(user_data::type::task_info_pointer);
  }
  ~io_awaiter() = default;

  static constexpr bool await_ready() noexcept { return false; }
  void await_suspend(co_handle<> handle) noexcept { io_info.handle = handle; }
  uint32_t await_resume() const noexcept { return io_info.result; }

  void set_link() noexcept { set_link(sqe); }

  static constexpr void set_link(io_uring_sqe *sqe) noexcept {
    sqe->flags |= IOSQE_IO_LINK;
    sqe->user_data |= static_cast<uint64_t>(user_data::type::task_info_linked);
  }

  io_uring_sqe *sqe;
  task_info io_info;
};

struct linked_io_awaiter {
  static constexpr bool await_ready() noexcept { return false; }
  void await_suspend(co_handle<> handle) const noexcept {
    last_io_awaiter->io_info.handle = handle;
  }
  int32_t await_resume() const noexcept { return last_io_awaiter->io_info.result; }

  void set_link() noexcept { last_io_awaiter->set_link(); }

  io_awaiter *last_io_awaiter;
};

inline linked_io_awaiter operator&&(io_awaiter &&lhs, io_awaiter &&rhs) noexcept {
  lhs.set_link();
  return linked_io_awaiter{&rhs};
}

inline linked_io_awaiter &&operator&&(linked_io_awaiter &&lhs, io_awaiter &&rhs) noexcept {
  lhs.set_link();
  lhs.last_io_awaiter = &rhs;
  return std::move(lhs);
}

inline linked_io_awaiter &&operator&&(linked_io_awaiter &&lhs, linked_io_awaiter &&rhs) noexcept {
  lhs.set_link();
  return std::move(rhs);
}

struct io_accept : io_awaiter {
  io_accept(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags) noexcept {
    io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
  }
};

struct io_close : io_awaiter {
  io_close(int fd) noexcept { io_uring_prep_close(sqe, fd); }
};

struct io_readv : io_awaiter {
  io_readv(int fd, std::span<const iovec> iovecs, uint64_t offset) noexcept {
    io_uring_prep_readv(sqe, fd, iovecs.data(), iovecs.size(), offset);
  }
};

struct io_writev : io_awaiter {
  io_writev(int fd, std::span<const iovec> iovecs, uint64_t offset) noexcept {
    io_uring_prep_writev(sqe, fd, iovecs.data(), iovecs.size(), offset);
  }
};

struct io_recv : io_awaiter {
  io_recv(int fd, std::span<char> buf, int flag) noexcept {
    io_uring_prep_recv(sqe, fd, buf.data(), buf.size(), flag);
  }
};

struct io_send : io_awaiter {
  io_send(int fd, std::span<char> buf, int flag) noexcept {
    io_uring_prep_send(sqe, fd, buf.data(), buf.size(), flag);
  }
};

} // namespace mira::co::core

#endif