#ifndef CO_MIRA_NET_ERROR_HPP
#define CO_MIRA_NET_ERROR_HPP

#include "log.hpp"

#include <cerrno>
#include <system_error>

namespace mira::co::net::detail {

[[noreturn]] inline void throw_errno(const char *operation) {
  const int error = errno;
  co::log("{} failed with errno {}", operation, error);
  throw std::system_error(error, std::generic_category(), operation);
}

} // namespace mira::co::net::detail

#endif
