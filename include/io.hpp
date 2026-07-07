#ifndef CO_MIRA_IO_HPP
#define CO_MIRA_IO_HPP

#include "io_awaiter.hpp"

#include <span>

namespace mira::co::io {

inline core::io_accept accept(int fd, struct sockaddr *addr, socklen_t *addrlen,
                              int flags = 0) noexcept {
  return core::io_accept{fd, addr, addrlen, flags};
}

inline core::io_close close(int fd) noexcept { return core::io_close{fd}; }

inline core::io_readv readv(int fd, std::span<const iovec> iovecs,
                            uint64_t offset = -1ULL) noexcept {
  return core::io_readv{fd, iovecs, offset};
}

inline core::io_writev writev(int fd, std::span<const iovec> iovecs,
                              uint64_t offset = -1ULL) noexcept {
  return core::io_writev{fd, iovecs, offset};
}

inline core::io_recv recv(int fd, std::span<char> buf, int flag = 0) noexcept {
  return core::io_recv{fd, buf, flag};
}

inline core::io_send send(int fd, std::span<char> buf, int flag = 0) noexcept {
  return core::io_send{fd, buf, flag};
}

} // namespace mira::co::io

#endif