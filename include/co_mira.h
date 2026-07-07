#ifndef CO_MIRA_H
#define CO_MIRA_H
#include <coroutine>

namespace mira::co {
template <typename T = void> using co_handle = std::coroutine_handle<T>;
} // namespace mira::co
#endif