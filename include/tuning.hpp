#ifndef CO_MIRA_TUNING_HPP
#define CO_MIRA_TUNING_HPP
#include <cstdint>

namespace mira::co::tuning {

inline constexpr uint32_t ready_queue_capacity_ = 16384;
inline constexpr uint32_t default_io_uring_entries = ready_queue_capacity_ << 1;

} // namespace mira::co::tuning

#endif