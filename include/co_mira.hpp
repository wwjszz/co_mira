#ifndef CO_MIRA_H
#define CO_MIRA_H

#include "log.hpp"

#include <coroutine>
#include <string_view>

#ifdef CO_MIRA_ENABLE_COUNTERS
#include <atomic>
#endif

namespace mira::co {
template <typename T = void> using co_handle = std::coroutine_handle<T>;

#ifdef CO_MIRA_ENABLE_COUNTERS

struct count_helper {
  explicit count_helper(std::string_view value) noexcept : name(value), counter(0) {
    log("set counter at>{}", value);
  }

  count_helper(const count_helper &) = delete;
  count_helper &operator=(const count_helper &) = delete;

  ~count_helper() noexcept {
    const int value = counter.load(std::memory_order_relaxed);

    try {
      if (value == 0) {
        log("name `{}` status<:"
            "\x1b[32m[SUCCESS]\x1b[0m",
            name);
      } else {
        log("name `{}` status<:"
            "\x1b[31m[FAILED]\x1b[0m "
            "counter: {}",
            name, value);
      }
    } catch (...) {
      // Destructors must not propagate logging failures.
    }
  }

  void increment() noexcept { counter.fetch_add(1, std::memory_order_relaxed); }

  void decrement() noexcept { counter.fetch_sub(1, std::memory_order_relaxed); }

  std::string_view name;
  std::atomic<int> counter;
};

inline count_helper handle_counter{"coroutine_handle"};

#endif

[[noreturn]]
inline void throw_uring_error(int result, std::string_view operation) {
  log("{} failed with errno {}", operation, result);
  throw std::system_error(result, std::generic_category(), std::string(operation));
}

} // namespace mira::co
#endif
