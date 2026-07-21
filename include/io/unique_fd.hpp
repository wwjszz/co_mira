#ifndef CO_MIRA_IO_UNIQUE_FD_HPP
#define CO_MIRA_IO_UNIQUE_FD_HPP

#include "log.hpp"

#include <cerrno>
#include <unistd.h>
#include <utility>

namespace mira::co::io {

class unique_fd {
public:
  unique_fd() noexcept = default;
  explicit unique_fd(int fd) noexcept : fd_(fd) {}

  ~unique_fd() noexcept { this->reset(); }

  unique_fd(unique_fd &&other) noexcept : fd_(other.release()) {}

  unique_fd &operator=(unique_fd &&other) noexcept {
    if (this != &other)
      this->reset(other.release());
    return *this;
  }

  unique_fd(const unique_fd &) = delete;
  unique_fd &operator=(const unique_fd &) = delete;

  [[nodiscard]] int get() const noexcept { return this->fd_; }
  [[nodiscard]] explicit operator bool() const noexcept { return this->fd_ >= 0; }

  [[nodiscard]] int release() noexcept { return std::exchange(this->fd_, -1); }

  void reset(int replacement = -1) noexcept {
    if (replacement == this->fd_)
      return;

    const int old = std::exchange(this->fd_, replacement);
    if (old < 0)
      return;

    if (::close(old) != 0)
      co::log("close({}) failed with errno {}", old, errno);
  }

  void swap(unique_fd &other) noexcept { std::swap(this->fd_, other.fd_); }

private:
  int fd_ = -1;
};

inline void swap(unique_fd &left, unique_fd &right) noexcept { left.swap(right); }

} // namespace mira::co::io

#endif
