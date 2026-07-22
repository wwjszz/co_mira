#ifndef CO_MIRA_DETAIL_TUNING_HPP
#define CO_MIRA_DETAIL_TUNING_HPP
#include <cstdint>

namespace mira::co::tuning {

inline constexpr uint32_t ready_queue_capacity_ = 1024;
// A scheduler owns one ring, so keep the default small enough for several
// schedulers under a normal RLIMIT_MEMLOCK. SQ pressure is handled by
// scheduler_state::ensure_sq_space().
inline constexpr uint32_t default_io_uring_entries = 256;

} // namespace mira::co::tuning

#endif
