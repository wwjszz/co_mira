#ifndef CO_MIRA_LOG_HPP
#define CO_MIRA_LOG_HPP

#include <iostream>
#include <print>

namespace mira::co {
// TODO: improve log
template <typename... Args> inline void log(std::format_string<Args...> format, Args &&...args) {
  std::println(std::cout, format, std::forward<Args>(args)...);
}
} // namespace mira::co

#endif