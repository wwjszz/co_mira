#ifndef CO_MIRA_USER_DATA_HPP
#define CO_MIRA_USER_DATA_HPP

#include "task_info.hpp"

#include <cstdint>
#include <utility>

namespace mira::co::core {

struct user_data {
  enum class type : uint8_t { task_info_pointer, task_info_linked, unknown };

  user_data(uint64_t user_data) : user_data_(user_data) {}

  static type type_from_user_data(uint64_t user_data) noexcept {
    return static_cast<type>(user_data & 0b111);
  }
  static task_info *task_info_from_user_data(uint64_t user_data) noexcept {
    return task_info::from_user_data(user_data);
  }
  static std::pair<task_info *, type> from_user_data(uint64_t user_data) noexcept {
    return {task_info_from_user_data(user_data), type_from_user_data(user_data)};
  }

  type type_from_user_data() noexcept { return type_from_user_data(user_data_); }
  task_info *task_info_from_user_data() noexcept { return task_info_from_user_data(user_data_); }
  std::pair<task_info *, type> from_user_data() noexcept {
    return {task_info_from_user_data(user_data_), type_from_user_data(user_data_)};
  }

  uint64_t get_user_data() const noexcept { return user_data_; }

private:
  uint64_t user_data_;
};

} // namespace mira::co::core

#endif