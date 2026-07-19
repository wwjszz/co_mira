#ifndef CO_MIRA_LOG_HPP
#define CO_MIRA_LOG_HPP

#include <format>
#include <iostream>
#include <print>
#include <source_location>
#include <utility>

namespace mira::co {

template <typename... Args> struct format_with_location {
  template <typename String>
  consteval format_with_location(String &&string,
                                 std::source_location loc = std::source_location::current())
      : format(std::forward<String>(string)), location(loc) {}

  std::format_string<Args...> format;
  std::source_location location;
};

template <typename... Args>
void log(format_with_location<std::type_identity_t<Args>...> info, Args &&...args) {
  auto message = std::format(info.format, std::forward<Args>(args)...);

  std::println(std::clog, "file: {}({}:{}) `{}` {}", info.location.file_name(),
               info.location.line(), info.location.column(), info.location.function_name(),
               message);
}

} // namespace mira::co

#endif