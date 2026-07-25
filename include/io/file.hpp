#ifndef CO_MIRA_IO_FILE_HPP
#define CO_MIRA_IO_FILE_HPP

#include "io/file_awaiter.hpp"

#include <cstdint>
#include <span>
#include <sys/stat.h>
#include <sys/types.h>

namespace mira::co::io {

// File paths, buffers, and result structures are non-owning and must remain
// alive until the operation's CQE has been consumed.

inline core::io_read read(int fd, std::span<char> buffer, uint64_t offset = -1ULL) noexcept { return core::io_read{fd, buffer, offset}; }

inline core::io_read read(int fd, std::span<char> buffer, const core::cancel_token &token, uint64_t offset = -1ULL) noexcept {
  return core::io_read{fd, buffer, offset, &token};
}

inline core::io_write write(int fd, std::span<const char> buffer, uint64_t offset = -1ULL) noexcept { return core::io_write{fd, buffer, offset}; }

inline core::io_write write(int fd, std::span<const char> buffer, const core::cancel_token &token, uint64_t offset = -1ULL) noexcept {
  return core::io_write{fd, buffer, offset, &token};
}

inline core::io_openat openat(int directory_fd, const char *path, int flags, mode_t mode = 0) noexcept {
  return core::io_openat{directory_fd, path, flags, mode};
}

inline core::io_openat openat(int directory_fd, const char *path, int flags, const core::cancel_token &token, mode_t mode = 0) noexcept {
  return core::io_openat{directory_fd, path, flags, mode, &token};
}

inline core::io_fsync fsync(int fd, unsigned flags = 0) noexcept { return core::io_fsync{fd, flags}; }

inline core::io_fsync fsync(int fd, const core::cancel_token &token, unsigned flags = 0) noexcept { return core::io_fsync{fd, flags, &token}; }

inline core::io_statx statx(int directory_fd, const char *path, int flags, unsigned mask, struct statx *result) noexcept {
  return core::io_statx{directory_fd, path, flags, mask, result};
}

inline core::io_statx statx(int directory_fd, const char *path, int flags, unsigned mask, struct statx *result, const core::cancel_token &token) noexcept {
  return core::io_statx{directory_fd, path, flags, mask, result, &token};
}

inline core::io_fallocate fallocate(int fd, int mode, uint64_t offset, uint64_t length) noexcept {
  return core::io_fallocate{fd, mode, offset, length};
}

inline core::io_fallocate fallocate(int fd, int mode, uint64_t offset, uint64_t length, const core::cancel_token &token) noexcept {
  return core::io_fallocate{fd, mode, offset, length, &token};
}

inline core::io_unlinkat unlinkat(int directory_fd, const char *path, int flags = 0) noexcept { return core::io_unlinkat{directory_fd, path, flags}; }

inline core::io_unlinkat unlinkat(int directory_fd, const char *path, const core::cancel_token &token, int flags = 0) noexcept {
  return core::io_unlinkat{directory_fd, path, flags, &token};
}

inline core::io_renameat renameat(int old_directory_fd, const char *old_path, int new_directory_fd, const char *new_path,
                                  unsigned flags = 0) noexcept {
  return core::io_renameat{old_directory_fd, old_path, new_directory_fd, new_path, flags};
}

inline core::io_renameat renameat(int old_directory_fd, const char *old_path, int new_directory_fd, const char *new_path, const core::cancel_token &token,
                                  unsigned flags = 0) noexcept {
  return core::io_renameat{old_directory_fd, old_path, new_directory_fd, new_path, flags, &token};
}

inline core::io_mkdirat mkdirat(int directory_fd, const char *path, mode_t mode) noexcept { return core::io_mkdirat{directory_fd, path, mode}; }

inline core::io_mkdirat mkdirat(int directory_fd, const char *path, mode_t mode, const core::cancel_token &token) noexcept {
  return core::io_mkdirat{directory_fd, path, mode, &token};
}

} // namespace mira::co::io

#endif
