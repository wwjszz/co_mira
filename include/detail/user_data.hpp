#ifndef CO_MIRA_DETAIL_USER_DATA_HPP
#define CO_MIRA_DETAIL_USER_DATA_HPP

#include "detail/task_info.hpp"

#include <cstdint>
#include <limits>
#include <utility>

namespace mira::co::core {

struct user_data {
  enum class type : uint8_t {
    task_info_pointer = 0,
    task_info_linked = 1,
    msg_ring = 2,
    msg_ring_ack = 3,
    msg_ring_stop = 4,
    msg_ring_stop_ack = 5,
    resume_on = 6,
    unknown = 0xff,
  };

  // Reserved control-plane completion that cannot alias a valid tagged
  // pointer. Low tag 7 remains invalid for ordinary encoded user_data.
  static constexpr uint64_t shutdown_cancel = std::numeric_limits<uint64_t>::max();

  user_data(uint64_t user_data) : user_data_(user_data) {}

  [[nodiscard]] static type type_from_user_data(uint64_t user_data) noexcept {
    switch (user_data & 0b111) {
    case static_cast<uint64_t>(type::task_info_pointer):
      return type::task_info_pointer;
    case static_cast<uint64_t>(type::task_info_linked):
      return type::task_info_linked;
    case static_cast<uint64_t>(type::msg_ring):
      return type::msg_ring;
    case static_cast<uint64_t>(type::msg_ring_ack):
      return type::msg_ring_ack;
    case static_cast<uint64_t>(type::msg_ring_stop):
      return type::msg_ring_stop;
    case static_cast<uint64_t>(type::msg_ring_stop_ack):
      return type::msg_ring_stop_ack;
    case static_cast<uint64_t>(type::resume_on):
      return type::resume_on;
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
  [[nodiscard]] void *address_from_user_data() const noexcept { return reinterpret_cast<void *>(this->user_data_ & task_info_mask); }

  [[nodiscard]] std::pair<task_info *, type> from_user_data() noexcept {
    return {task_info_from_user_data(this->user_data_), type_from_user_data(this->user_data_)};
  }

  [[nodiscard]] uint64_t get_user_data() const noexcept { return this->user_data_; }

private:
  uint64_t user_data_;
};

} // namespace mira::co::core

#endif
