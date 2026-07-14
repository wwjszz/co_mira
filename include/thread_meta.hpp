#ifndef CO_MIRA_THREAD_META_HPP
#define CO_MIRA_THREAD_META_HPP

namespace mira::co {
class scheduler;

namespace core {

struct scheduler_state;

struct thread_meta {
  scheduler *sche;
  scheduler_state *sche_state;
};

inline thread_local thread_meta this_thread{};
} // namespace core
} // namespace mira::co

#endif