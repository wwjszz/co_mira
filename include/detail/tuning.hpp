#ifndef CO_MIRA_DETAIL_TUNING_HPP
#define CO_MIRA_DETAIL_TUNING_HPP
#include <cstdint>

namespace mira::co::tuning {

// TODO: let user define
// inline constexpr uint32_t ready_queue_capacity_ = 16384;
inline constexpr uint32_t ready_queue_capacity_ = 1024;
inline constexpr uint32_t default_io_uring_entries = ready_queue_capacity_ << 1;

} // namespace mira::co::tuning

#endif
