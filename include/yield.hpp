#ifndef CO_MIRA_YIELD_HPP
#define CO_MIRA_YIELD_HPP

#include "co_mira.h"
#include "scheduler.hpp"
#include "thread_meta.hpp"

namespace mira::co {
namespace core {
struct yield_awaiter {
  static constexpr bool await_ready() noexcept { return false; }
  static constexpr void await_suspend(co_handle<> handle) {
    this_thread.sche_state->push_handle(handle);
  }
  static constexpr void await_resume() noexcept {}
};
} // namespace core

inline core::yield_awaiter yield() noexcept { return {}; }

} // namespace mira::co

#endif