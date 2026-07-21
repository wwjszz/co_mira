#ifndef CO_MIRA_IO_HPP
#define CO_MIRA_IO_HPP

#include "detail/task_info.hpp"
#include "io/awaiter.hpp"
#include "io/file.hpp"
#include "io/owned_awaiter.hpp"
#include "io/unique_fd.hpp"

#include <chrono>
#include <span>
#include <utility>

namespace mira::co::io {

using cancel_token = core::cancel_token;

// Lifetime contract for every operation below:
// the fd and all referenced storage must remain valid until the operation's CQE
// has been consumed and the awaiting coroutine resumes. This includes the span
// elements, every iovec and its iov_base storage, and accept's sockaddr and
// socklen_t objects. The awaiter stores non-owning views; it does not copy buffer
// contents or address structures.
//
// Completion errors are returned as negative errno values. Infrastructure
// failures in the scheduler/io_uring control path are reported as exceptions.
// A cancel_token is single-use, cannot be moved, and must outlive the target
// operation's CQE. Bind it to the target operation, then pass the same token to
// io::cancel() from another coroutine. Bind tokens to actions before wrapping
// them in with_timeout(); with_timeout() itself does not represent one
// independently cancellable operation.

inline core::io_nop nop() noexcept { return core::io_nop{}; }

inline core::io_nop nop(cancel_token &token) noexcept { return core::io_nop{&token}; }

inline core::io_accept accept(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags = 0) noexcept {
  return core::io_accept{fd, addr, addrlen, flags};
}

inline core::io_accept accept(int fd, struct sockaddr *addr, socklen_t *addrlen, cancel_token &token, int flags = 0) noexcept {
  return core::io_accept{fd, addr, addrlen, flags, &token};
}

inline core::io_connect connect(int fd, const struct sockaddr *addr, socklen_t addrlen) noexcept { return core::io_connect{fd, addr, addrlen}; }

inline core::io_connect connect(int fd, const struct sockaddr *addr, socklen_t addrlen, cancel_token &token) noexcept {
  return core::io_connect{fd, addr, addrlen, &token};
}

inline core::io_close close(int fd) noexcept { return core::io_close{fd}; }

inline core::io_close close(int fd, cancel_token &token) noexcept { return core::io_close{fd, &token}; }

inline core::io_close_owned close(unique_fd &&fd) noexcept { return core::io_close_owned{std::move(fd)}; }

inline core::io_close_owned close(unique_fd &&fd, cancel_token &token) noexcept { return core::io_close_owned{std::move(fd), &token}; }

inline core::io_readv readv(int fd, std::span<const iovec> iovecs, uint64_t offset = -1ULL) noexcept { return core::io_readv{fd, iovecs, offset}; }

inline core::io_readv readv(int fd, std::span<const iovec> iovecs, cancel_token &token, uint64_t offset = -1ULL) noexcept {
  return core::io_readv{fd, iovecs, offset, &token};
}

inline core::io_writev writev(int fd, std::span<const iovec> iovecs, uint64_t offset = -1ULL) noexcept { return core::io_writev{fd, iovecs, offset}; }

inline core::io_writev writev(int fd, std::span<const iovec> iovecs, cancel_token &token, uint64_t offset = -1ULL) noexcept {
  return core::io_writev{fd, iovecs, offset, &token};
}

inline core::io_recv recv(int fd, std::span<char> buf, int flags = 0) noexcept { return core::io_recv{fd, buf, flags}; }

inline core::io_recv recv(int fd, std::span<char> buf, cancel_token &token, int flags = 0) noexcept { return core::io_recv{fd, buf, flags, &token}; }

inline core::io_send send(int fd, std::span<const char> buf, int flags = 0) noexcept { return core::io_send{fd, buf, flags}; }

inline core::io_send send(int fd, std::span<const char> buf, cancel_token &token, int flags = 0) noexcept {
  return core::io_send{fd, buf, flags, &token};
}

inline core::io_recvmsg recvmsg(int fd, msghdr *msg, unsigned int flags = 0) noexcept { return core::io_recvmsg{fd, msg, flags}; }

inline core::io_recvmsg recvmsg(int fd, msghdr *msg, cancel_token &token, unsigned int flags = 0) noexcept {
  return core::io_recvmsg{fd, msg, flags, &token};
}

inline core::io_sendmsg sendmsg(int fd, msghdr *msg, unsigned int flags = 0) noexcept { return core::io_sendmsg{fd, msg, flags}; }

inline core::io_sendmsg sendmsg(int fd, msghdr *msg, cancel_token &token, unsigned int flags = 0) noexcept {
  return core::io_sendmsg{fd, msg, flags, &token};
}

inline core::io_cancel cancel(cancel_token &token) { return core::io_cancel{token}; }

inline core::io_timeout sleep_for(__kernel_timespec ts) noexcept { return core::io_timeout{ts, 0, 0}; }

inline core::io_timeout sleep_for(__kernel_timespec ts, cancel_token &token) noexcept { return core::io_timeout{ts, 0, 0, &token}; }

template <typename Rep, typename Period> inline core::io_timeout sleep_for(std::chrono::duration<Rep, Period> duration) {
  return core::io_timeout{duration, 0, 0};
}

template <typename Rep, typename Period> inline core::io_timeout sleep_for(std::chrono::duration<Rep, Period> duration, cancel_token &token) {
  return core::io_timeout{duration, 0, 0, &token};
}

template <typename Awaiter>
inline auto with_timeout(Awaiter &&awaiter,
                         __kernel_timespec ts) noexcept(noexcept(core::io_with_timeout(std::forward<Awaiter>(awaiter), ts, 0, 0))) {
  return core::io_with_timeout(std::forward<Awaiter>(awaiter), ts, 0, 0);
}

template <typename Awaiter, typename Rep, typename Period>
inline auto
with_timeout(Awaiter &&awaiter,
             std::chrono::duration<Rep, Period> duration) noexcept(noexcept(core::io_with_timeout(std::forward<Awaiter>(awaiter), duration, 0, 0))) {
  return core::io_with_timeout(std::forward<Awaiter>(awaiter), duration, 0, 0);
}

} // namespace mira::co::io

#endif
