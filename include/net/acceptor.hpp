#ifndef CO_MIRA_NET_ACCEPTOR_HPP
#define CO_MIRA_NET_ACCEPTOR_HPP

#include "net/socket.hpp"

#include <sys/socket.h>

namespace mira::co::net {

class acceptor {
public:
  explicit acceptor(const inet_address &listen_address, int backlog = SOMAXCONN) : socket_(socket::tcp(listen_address.family())) {
    this->socket_.set_reuse_address().bind(listen_address).listen(backlog);
  }

  acceptor(acceptor &&) noexcept = default;
  acceptor &operator=(acceptor &&) noexcept = default;
  acceptor(const acceptor &) = delete;
  acceptor &operator=(const acceptor &) = delete;

  [[nodiscard]] int native_handle() const noexcept { return this->socket_.native_handle(); }
  [[nodiscard]] inet_address local_address() const { return this->socket_.local_address(); }

  [[nodiscard]] core::io_accept accept(int flags = SOCK_CLOEXEC) const noexcept { return io::accept(this->native_handle(), nullptr, nullptr, flags); }

  [[nodiscard]] core::io_accept accept(const io::cancel_token &token, int flags = SOCK_CLOEXEC) const noexcept {
    return io::accept(this->native_handle(), nullptr, nullptr, token, flags);
  }

private:
  socket socket_;
};

} // namespace mira::co::net

#endif
