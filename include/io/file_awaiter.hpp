#ifndef CO_MIRA_IO_FILE_AWAITER_HPP
#define CO_MIRA_IO_FILE_AWAITER_HPP

#include "io/awaiter.hpp"

#include <cstdint>
#include <linux/stat.h>
#include <span>
#include <sys/stat.h>
#include <sys/types.h>

namespace mira::co::core {

struct [[nodiscard]] io_read : io_awaiter {
  io_read(int fd, std::span<char> buffer, uint64_t offset, cancel_token *token = nullptr) noexcept
      : io_awaiter(token), fd_(fd), buffer_(buffer), offset_(offset) {}

  void prepare(io_uring_sqe *sqe) noexcept {
    io_uring_prep_read(sqe, this->fd_, this->buffer_.data(), static_cast<unsigned>(this->buffer_.size()), this->offset_);
  }

private:
  int fd_;
  std::span<char> buffer_;
  uint64_t offset_;
};

struct [[nodiscard]] io_write : io_awaiter {
  io_write(int fd, std::span<const char> buffer, uint64_t offset, cancel_token *token = nullptr) noexcept
      : io_awaiter(token), fd_(fd), buffer_(buffer), offset_(offset) {}

  void prepare(io_uring_sqe *sqe) noexcept {
    io_uring_prep_write(sqe, this->fd_, this->buffer_.data(), static_cast<unsigned>(this->buffer_.size()), this->offset_);
  }

private:
  int fd_;
  std::span<const char> buffer_;
  uint64_t offset_;
};

struct [[nodiscard]] io_openat : io_awaiter {
  io_openat(int directory_fd, const char *path, int flags, mode_t mode, cancel_token *token = nullptr) noexcept
      : io_awaiter(token), directory_fd_(directory_fd), path_(path), flags_(flags), mode_(mode) {}

  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_openat(sqe, this->directory_fd_, this->path_, this->flags_, this->mode_); }

private:
  int directory_fd_;
  const char *path_;
  int flags_;
  mode_t mode_;
};

struct [[nodiscard]] io_fsync : io_awaiter {
  io_fsync(int fd, unsigned flags, cancel_token *token = nullptr) noexcept : io_awaiter(token), fd_(fd), flags_(flags) {}

  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_fsync(sqe, this->fd_, this->flags_); }

private:
  int fd_;
  unsigned flags_;
};

struct [[nodiscard]] io_statx : io_awaiter {
  io_statx(int directory_fd, const char *path, int flags, unsigned mask, struct statx *result, cancel_token *token = nullptr) noexcept
      : io_awaiter(token), directory_fd_(directory_fd), path_(path), flags_(flags), mask_(mask), result_(result) {}

  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_statx(sqe, this->directory_fd_, this->path_, this->flags_, this->mask_, this->result_); }

private:
  int directory_fd_;
  const char *path_;
  int flags_;
  unsigned mask_;
  struct statx *result_;
};

struct [[nodiscard]] io_fallocate : io_awaiter {
  io_fallocate(int fd, int mode, uint64_t offset, uint64_t length, cancel_token *token = nullptr) noexcept
      : io_awaiter(token), fd_(fd), mode_(mode), offset_(offset), length_(length) {}

  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_fallocate(sqe, this->fd_, this->mode_, this->offset_, this->length_); }

private:
  int fd_;
  int mode_;
  uint64_t offset_;
  uint64_t length_;
};

struct [[nodiscard]] io_unlinkat : io_awaiter {
  io_unlinkat(int directory_fd, const char *path, int flags, cancel_token *token = nullptr) noexcept
      : io_awaiter(token), directory_fd_(directory_fd), path_(path), flags_(flags) {}

  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_unlinkat(sqe, this->directory_fd_, this->path_, this->flags_); }

private:
  int directory_fd_;
  const char *path_;
  int flags_;
};

struct [[nodiscard]] io_renameat : io_awaiter {
  io_renameat(int old_directory_fd, const char *old_path, int new_directory_fd, const char *new_path, unsigned flags,
              cancel_token *token = nullptr) noexcept
      : io_awaiter(token), old_directory_fd_(old_directory_fd), old_path_(old_path), new_directory_fd_(new_directory_fd), new_path_(new_path),
        flags_(flags) {}

  void prepare(io_uring_sqe *sqe) noexcept {
    io_uring_prep_renameat(sqe, this->old_directory_fd_, this->old_path_, this->new_directory_fd_, this->new_path_, this->flags_);
  }

private:
  int old_directory_fd_;
  const char *old_path_;
  int new_directory_fd_;
  const char *new_path_;
  unsigned flags_;
};

struct [[nodiscard]] io_mkdirat : io_awaiter {
  io_mkdirat(int directory_fd, const char *path, mode_t mode, cancel_token *token = nullptr) noexcept
      : io_awaiter(token), directory_fd_(directory_fd), path_(path), mode_(mode) {}

  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_mkdirat(sqe, this->directory_fd_, this->path_, this->mode_); }

private:
  int directory_fd_;
  const char *path_;
  mode_t mode_;
};

} // namespace mira::co::core

#endif
