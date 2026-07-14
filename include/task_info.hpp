#ifndef CO_MIRA_TASK_INFO_HPP
#define CO_MIRA_TASK_INFO_HPP
#include "co_mira.h"

#include <cstdint>

namespace mira::co::core {

// The low 3 bits of any task_info address are always 0
struct task_info {
  co_handle<> handle;
  int32_t result;

  [[nodiscard]] uint64_t as_user_data() const noexcept { return reinterpret_cast<uint64_t>(this); }

  static task_info *from_user_data(uint64_t user_data) noexcept;
};

static_assert(alignof(task_info) == 8);

inline constexpr uint64_t task_info_mask = ~static_cast<uint64_t>(alignof(task_info) - 1);

inline task_info *task_info::from_user_data(uint64_t user_data) noexcept {
  return reinterpret_cast<task_info *>(user_data & task_info_mask);
}

} // namespace mira::co::core

#endif