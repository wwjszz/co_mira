#ifndef CO_MIRA_NET_SOCKET_HPP
#define CO_MIRA_NET_SOCKET_HPP

#include "io.hpp"
#include "io/unique_fd.hpp"
#include "net/error.hpp"
#include "net/inet_address.hpp"

#include <cerrno>
#include <netinet/tcp.h>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <utility>

namespace mira::co::net {

namespace detail {

struct [[nodiscard]] connect_operation : core::io_awaiter {
  connect_operation(int fd, inet_address address, core::cancel_token *token = nullptr) noexcept
      : core::io_awaiter(token), fd_(fd), address_(std::move(address)) {}

  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_connect(sqe, this->fd_, this->address_.data(), this->address_.length()); }

private:
  int fd_;
  inet_address address_;
};

} // namespace detail

// socket owns its descriptor. Its synchronous destructor closes the descriptor;
// callers must ensure no operation using that descriptor is still pending.
class socket {
public:
  socket() noexcept = default;
  explicit socket(io::unique_fd fd) noexcept : fd_(std::move(fd)) {}

  [[nodiscard]] static socket adopt(int fd) {
    if (fd < 0) {
      co::log("cannot adopt an invalid socket descriptor");
      throw std::invalid_argument("cannot adopt an invalid socket descriptor");
    }
    return socket{io::unique_fd{fd}};
  }

  [[nodiscard]] static socket tcp(sa_family_t family = AF_INET) { return create(family, SOCK_STREAM, IPPROTO_TCP); }
  [[nodiscard]] static socket udp(sa_family_t family = AF_INET) { return create(family, SOCK_DGRAM, IPPROTO_UDP); }

  socket(socket &&) noexcept = default;
  socket &operator=(socket &&) noexcept = default;
  socket(const socket &) = delete;
  socket &operator=(const socket &) = delete;

  [[nodiscard]] int native_handle() const noexcept { return this->fd_.get(); }
  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(this->fd_); }
  [[nodiscard]] int release() noexcept { return this->fd_.release(); }

  socket &bind(const inet_address &address) {
    if (::bind(this->native_handle(), address.data(), address.length()) != 0)
      detail::throw_errno("bind");
    return *this;
  }

  socket &listen(int backlog = SOMAXCONN) {
    if (::listen(this->native_handle(), backlog) != 0)
      detail::throw_errno("listen");
    return *this;
  }

  socket &set_reuse_address(bool enabled = true) {
    const int value = enabled ? 1 : 0;
    if (::setsockopt(this->native_handle(), SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) != 0)
      detail::throw_errno("setsockopt(SO_REUSEADDR)");
    return *this;
  }

  socket &set_tcp_no_delay(bool enabled = true) {
    const int value = enabled ? 1 : 0;
    if (::setsockopt(this->native_handle(), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) != 0)
      detail::throw_errno("setsockopt(TCP_NODELAY)");
    return *this;
  }

  [[nodiscard]] inet_address local_address() const { return this->query_address(false); }
  [[nodiscard]] inet_address peer_address() const { return this->query_address(true); }

  [[nodiscard]] detail::connect_operation connect(inet_address address) const noexcept {
    return detail::connect_operation{this->native_handle(), std::move(address)};
  }

  [[nodiscard]] detail::connect_operation connect(inet_address address, io::cancel_token &token) const noexcept {
    return detail::connect_operation{this->native_handle(), std::move(address), &token};
  }

  [[nodiscard]] core::io_recv recv(std::span<char> buffer, int flags = 0) const noexcept { return io::recv(this->native_handle(), buffer, flags); }

  [[nodiscard]] core::io_recv recv(std::span<char> buffer, io::cancel_token &token, int flags = 0) const noexcept {
    return io::recv(this->native_handle(), buffer, token, flags);
  }

  [[nodiscard]] core::io_send send(std::span<const char> buffer, int flags = 0) const noexcept {
    return io::send(this->native_handle(), buffer, flags);
  }

  [[nodiscard]] core::io_send send(std::span<const char> buffer, io::cancel_token &token, int flags = 0) const noexcept {
    return io::send(this->native_handle(), buffer, token, flags);
  }

  [[nodiscard]] core::io_close_owned async_close() noexcept { return io::close(std::move(this->fd_)); }

private:
  [[nodiscard]] static socket create(sa_family_t family, int type, int protocol) {
    const int fd = ::socket(family, type | SOCK_CLOEXEC, protocol);
    if (fd < 0)
      detail::throw_errno("socket");
    return socket{io::unique_fd{fd}};
  }

  [[nodiscard]] inet_address query_address(bool peer) const {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    const int result = peer ? ::getpeername(this->native_handle(), reinterpret_cast<sockaddr *>(&storage), &length)
                            : ::getsockname(this->native_handle(), reinterpret_cast<sockaddr *>(&storage), &length);
    if (result != 0)
      detail::throw_errno(peer ? "getpeername" : "getsockname");
    return inet_address::from_native(reinterpret_cast<const sockaddr *>(&storage), length);
  }

  io::unique_fd fd_;
};

} // namespace mira::co::net

#endif
