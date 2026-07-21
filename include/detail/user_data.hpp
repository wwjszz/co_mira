#ifndef CO_MIRA_DETAIL_USER_DATA_HPP
#define CO_MIRA_DETAIL_USER_DATA_HPP

#include "detail/task_info.hpp"

#include <cstdint>
#include <utility>

namespace mira::co::core {

struct user_data {
  enum class type : uint8_t { task_info_pointer, task_info_linked, unknown };

  user_data(uint64_t user_data) : user_data_(user_data) {}

  [[nodiscard]] static type type_from_user_data(uint64_t user_data) noexcept {
    switch (user_data & 0b111) {
    case static_cast<uint64_t>(type::task_info_pointer):
      return type::task_info_pointer;
    case static_cast<uint64_t>(type::task_info_linked):
      return type::task_info_linked;
    default:
      return type::unknown;
    }
  }
  [[nodiscard]] static task_info *task_info_from_user_data(uint64_t user_data) noexcept { return task_info::from_user_data(user_data); }
  [[nodiscard]] static std::pair<task_info *, type> from_user_data(uint64_t user_data) noexcept {
    return {task_info_from_user_data(user_data), type_from_user_data(user_data)};
  }

  [[nodiscard]] type type_from_user_data() noexcept { return type_from_user_data(this->user_data_); }

  [[nodiscard]] task_info *task_info_from_user_data() noexcept { return task_info_from_user_data(this->user_data_); }

  [[nodiscard]] std::pair<task_info *, type> from_user_data() noexcept {
    return {task_info_from_user_data(this->user_data_), type_from_user_data(this->user_data_)};
  }

  [[nodiscard]] uint64_t get_user_data() const noexcept { return this->user_data_; }

private:
  uint64_t user_data_;
};

} // namespace mira::co::core

#endif
