#ifndef CO_MIRA_SCHEDULER_HPP
#define CO_MIRA_SCHEDULER_HPP

#include "detail/core.hpp"
#include "detail/fixed_queue.hpp"
#include "detail/task_info.hpp"
#include "detail/thread_meta.hpp"
#include "detail/tuning.hpp"
#include "detail/user_data.hpp"
#include "log.hpp"
#include "task.hpp"

#include <cerrno>
#include <coroutine>
#include <exception>
#include <experimental/scope>
#include <liburing.h>
#include <stdexcept>
#include <thread>

namespace mira::co {

class scheduler;

namespace core {

#define CO_MIRA_URING_CALL(res, uring_call, ...)                                                                                                     \
  do {                                                                                                                                               \
    res = 0;                                                                                                                                         \
                                                                                                                                                     \
    for (;;) {                                                                                                                                       \
      res = uring_call(__VA_ARGS__);                                                                                                                 \
                                                                                                                                                     \
      if (res == -EINTR) [[unlikely]]                                                                                                                \
        continue;                                                                                                                                    \
                                                                                                                                                     \
      break;                                                                                                                                         \
    }                                                                                                                                                \
                                                                                                                                                     \
    if (res < 0) [[unlikely]]                                                                                                                        \
      throw_uring_error(-res, #uring_call);                                                                                                          \
  } while (0)

#ifdef CO_MIRA_ENABLE_TESTING
enum class scheduler_test_failure : uint8_t {
  none,
  init,
  io_awaiter,
  submit_and_wait,
};
#endif

struct [[nodiscard]] detached_task {
  struct promise_type {
    detached_task get_return_object() {
#ifdef CO_MIRA_ENABLE_COUNTERS
      ::mira::co::handle_counter.increment();
#endif
      return {co_handle<promise_type>::from_promise(*this)};
    }
    static constexpr std::suspend_always initial_suspend() noexcept { return {}; }
    static constexpr std::suspend_never final_suspend() noexcept { return {}; }
    static constexpr void return_void() noexcept {}
    static constexpr void unhandled_exception() noexcept { std::terminate(); }
  };

  using handle_type = co_handle<promise_type>;
  detached_task(handle_type handle) noexcept : handle_(handle) {}

  ~detached_task() {
    if (auto &handle = this->handle_) {
#ifdef CO_MIRA_ENABLE_COUNTERS
      ::mira::co::handle_counter.decrement();
#endif
      handle.destroy();
    }
  }

  detached_task(detached_task &&other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  detached_task &operator=(detached_task &&other) noexcept {
    if (&other != this) {
      std::swap(this->handle_, other.handle_);
    }
    return *this;
  }

  detached_task(const detached_task &) = delete;
  detached_task &operator=(const detached_task &) = delete;

  [[nodiscard]] handle_type release() noexcept { return std::exchange(this->handle_, nullptr); }

private:
  handle_type handle_;
};

[[nodiscard]] inline detached_task run_detached(scheduler &, task<void>);

struct scheduler_state {
  // The ready queue contains heterogeneous, non-owning resume handles while
  // running. Before start(), entries are owning detached roots released by
  // scheduler::co_spawn(); destroy_all_handle() is only valid in that unstarted
  // state. Queue exhaustion is currently a hard runtime error: push throws
  // std::length_error rather than dropping a wakeup.
  using inner_queue = fixed_queue<std::coroutine_handle<>, std::remove_cv_t<decltype(tuning::ready_queue_capacity_)>, tuning::ready_queue_capacity_>;
  using queue_size_t = inner_queue::index_t;

  friend scheduler;

  scheduler_state() = default;
  ~scheduler_state() noexcept {
    // TODO: handle exception
    if (in_flight_ != 0) [[unlikely]] {
      co::log("scheduler_state in_flight_ is not zero, count: {}", in_flight_);
      std::terminate();
    }

    if (this->initialized_) [[likely]] {
      this_thread.sche_state = nullptr;
      this_thread.sche = nullptr;
      io_uring_queue_exit(&this->ring_);
      this->initialized_ = false;
    }
  }

  void init(unsigned entries);
  void deinit() noexcept;
  [[nodiscard]] queue_size_t size() const noexcept;
  void push_handle(co_handle<> handle);

  [[nodiscard]] constexpr bool has_task_ready() const noexcept;
  [[nodiscard]] io_uring_sqe *get_free_sqe();
  void wait_uring();
  [[nodiscard]] bool peek_uring();
  void handle_cqe(io_uring_cqe *cqe);

  void resume_handle();

  void flush_submissions();
  [[nodiscard]] uint32_t reap_completions();

  [[nodiscard]] unsigned submit_ready_count() noexcept;
  void ensure_sq_space(unsigned required);

  void destroy_all_handle() noexcept;

  scheduler_state(const scheduler_state &) = delete;
  scheduler_state &operator=(const scheduler_state &) = delete;

  // TODO: support move
  scheduler_state(scheduler_state &&) = delete;
  scheduler_state &operator=(scheduler_state &&) = delete;

private:
  // Number of successfully submitted requests for which no CQE has been
  // consumed. Acquiring/preparing an SQE does not increment this counter;
  // successful submit increments by its return value, and CQ advance decrements
  // by the number consumed.
  uint32_t in_flight_ = 0;

  io_uring ring_;
  inner_queue ready_queue_;
  bool initialized_ = false;
#ifdef CO_MIRA_ENABLE_TESTING
  scheduler_test_failure test_failure_ = scheduler_test_failure::none;
#endif
};
} // namespace core

// Single-host-thread scheduler lifecycle:
//
//   CREATED --co_spawn()--> SPAWNED --start()--> STARTED --join()--> JOINED
//      |                         ^
//      +---------start()---------+
//
// co_spawn() is allowed from the owner thread before start(). Once started it is
// allowed only from the scheduler's host thread. start() and join() are one-shot;
// invalid transitions throw std::logic_error. join() waits for the host thread
// and rethrows its stored exception. External request_stop(), RUNNING/STOPPING/
// STOPPED observation, and pending-IO cancellation are not implemented yet.
// The type is not generally thread-safe.
class scheduler {
public:
  scheduler() = default;
#ifdef CO_MIRA_ENABLE_TESTING
  explicit scheduler(core::scheduler_test_failure failure) noexcept : test_failure_(failure) {}
#endif
  ~scheduler() noexcept {
    if (this->cur_state_ == STATE::STARTED) {
      try {
        this->join();
      } catch (...) {
        // TODO: handle exception
        log("join failed");
      }
    } else if (this->state_.has_task_ready()) {
      this->state_.destroy_all_handle();
    }
  }

  void co_spawn(task<void> &&item);
  void start();
  void join();

  void report_exception(std::exception_ptr) noexcept;

  scheduler(const scheduler &) = delete;
  scheduler &operator=(const scheduler &) = delete;

  // TODO: support move
  scheduler(scheduler &&) = delete;
  scheduler &operator=(scheduler &&) = delete;

private:
  enum class STATE : uint8_t {
    CREATED,
    SPAWNED,
    STARTED,
    JOINED,
  };

  void init();
  void deinit() noexcept;
  void run();
  void flush_submissions();
  void reap_completions();
  void reap_completions_slow();
  void resume_ready_task();

  [[nodiscard]] core::detached_task run_detached(task<void> &&);

  bool is_stop = false;
  STATE cur_state_ = STATE::CREATED;
  std::thread host_thread_;
  core::scheduler_state state_;
  std::exception_ptr state_exception_;
#ifdef CO_MIRA_ENABLE_TESTING
  core::scheduler_test_failure test_failure_ = core::scheduler_test_failure::none;
#endif
};

namespace core {
inline void scheduler_state::init(unsigned entries) {
#ifdef CO_MIRA_ENABLE_TESTING
  if (this->test_failure_ == scheduler_test_failure::init) [[unlikely]]
    throw_uring_error(EINVAL, "io_uring_queue_init");
#endif

  int ret = io_uring_queue_init(entries, &this->ring_, 0);
  if (ret != 0) [[unlikely]]
    throw_uring_error(-ret, "io_uring_queue_init");
  this->initialized_ = true;
  this_thread.sche_state = this;
}

inline void scheduler_state::deinit() noexcept { this_thread.sche_state = nullptr; }

inline scheduler_state::queue_size_t scheduler_state::size() const noexcept { return this->ready_queue_.size(); }

inline void scheduler_state::push_handle(co_handle<> handle) {
  if (handle == nullptr) [[unlikely]] {
    log("pushing an empty handle");
    throw std::invalid_argument("push an empty handle");
  }
  this->ready_queue_.push(handle);
}

inline constexpr bool scheduler_state::has_task_ready() const noexcept { return !this->ready_queue_.empty(); }

inline io_uring_sqe *scheduler_state::get_free_sqe() {
#ifdef CO_MIRA_ENABLE_TESTING
  if (this->test_failure_ == scheduler_test_failure::io_awaiter) [[unlikely]]
    throw_uring_error(ENOSPC, "io_uring_get_sqe");
#endif

  io_uring_sqe *sqe = io_uring_get_sqe(&this->ring_);
  if (sqe == nullptr) [[unlikely]] {
    // TODO: ensure more space when crowed
    this->ensure_sq_space(1);
    sqe = io_uring_get_sqe(&this->ring_);
    if (sqe == nullptr) [[unlikely]] {
      log("failed to acquire SQE");
      throw std::runtime_error("failed to acquire SQE");
    }
  }

  return sqe;
}

inline void scheduler_state::wait_uring() {
  io_uring_cqe *_;
  int res;
  CO_MIRA_URING_CALL(res, io_uring_wait_cqe, &this->ring_, &_);
}

inline bool scheduler_state::peek_uring() {
  io_uring_cqe *cqe = nullptr;
  int res = 0;

  for (;;) {
    res = io_uring_peek_cqe(&this->ring_, &cqe);

    if (res == -EINTR) [[unlikely]]
      continue;

    if (res == -EAGAIN)
      return false;

    break;
  }

  if (res < 0) [[unlikely]]
    throw_uring_error(-res, "io_uring_peek_cqe");
  return cqe != nullptr;
}

inline void scheduler_state::handle_cqe(io_uring_cqe *cqe) {
  const int32_t result = cqe->res;
  user_data data(cqe->user_data);

  using utype = user_data::type;
  const auto type = data.type_from_user_data();

  if (type == utype::unknown) [[unlikely]] {
    log("handle_cqe: unknown user_data type");
    std::terminate();
  }

  auto *info = data.task_info_from_user_data();

  if (auto *token = std::exchange(info->token, nullptr))
    token->mark_completed();

  info->result = result;
  if (type == utype::task_info_pointer) [[likely]]
    this->push_handle(info->handle);
}

inline void scheduler_state::resume_handle() {
  co_handle<> task = this->ready_queue_.pop();
  task.resume();
}

inline void scheduler_state::flush_submissions() {
  if (io_uring_sq_ready(&this->ring_)) [[likely]] {
    int res;
    CO_MIRA_URING_CALL(res, io_uring_submit_and_wait, &this->ring_, !this->has_task_ready());
    this->in_flight_ += static_cast<uint32_t>(res);
  }
}

inline uint32_t scheduler_state::reap_completions() {
  io_uring_cqe *cqe = nullptr;
  unsigned head, count = 0;

  auto guard = std::experimental::scope_exit([&]() noexcept {
    if (count != 0) [[likely]] {
      if (count > this->in_flight_) [[unlikely]] {
        log("CQE count {} exceeds in-flight request count {}", count, this->in_flight_);
        std::terminate();
      }
      io_uring_cq_advance(&this->ring_, count);
      this->in_flight_ -= count;
    }
  });

  io_uring_for_each_cqe(&this->ring_, head, cqe) {
    ++count;
    handle_cqe(cqe);
  }

  return count;
}

inline unsigned scheduler_state::submit_ready_count() noexcept { return io_uring_sq_ready(&this->ring_); }

inline void scheduler_state::ensure_sq_space(unsigned required) {
  unsigned sq_entries_ = this->ring_.sq.ring_entries;
  if (sq_entries_ < required) [[unlikely]] {
    log("required {} SQ entries exceeds capacity {}", required, sq_entries_);
    throw std::length_error("required sq entries exceeds SQ capacity");
  }

  // TODO: more space optimization
  while (io_uring_sq_space_left(&this->ring_) < required) [[likely]] {
    const unsigned space_before = io_uring_sq_space_left(&this->ring_);
    int res;
    CO_MIRA_URING_CALL(res, io_uring_submit, &this->ring_);
    this->in_flight_ += static_cast<uint32_t>(res);

    const unsigned space_after = io_uring_sq_space_left(&this->ring_);
    if (res == 0 && space_after <= space_before) [[unlikely]] {
      log("io_uring_submit made no SQ progress: required {}, available {}", required, space_after);
      throw std::runtime_error("io_uring_submit returned 0 without freeing SQ space");
    }
  }
}

inline void scheduler_state::destroy_all_handle() noexcept {
  auto &r = this->ready_queue_;
  for (unsigned sz = r.size(); sz; --sz) {
    co_handle<> handle = r.pop();
    if (handle) {
#ifdef CO_MIRA_ENABLE_COUNTERS
      ::mira::co::handle_counter.decrement();
#endif
      handle.destroy();
    }
  }
}

inline detached_task run_detached(scheduler &sche, task<void> item) {
  try {
    co_await item;
  } catch (...) {
    sche.report_exception(std::current_exception());
  }

#ifdef CO_MIRA_ENABLE_COUNTERS
  ::mira::co::handle_counter.decrement();
#endif
}

} // namespace core

inline void scheduler::init() {
  core::this_thread.sche = this;
#ifdef CO_MIRA_ENABLE_TESTING
  this->state_.test_failure_ = this->test_failure_;
#endif
  this->state_.init(tuning::default_io_uring_entries);
}

inline void scheduler::deinit() noexcept {
  core::this_thread.sche = nullptr;
  this->state_.deinit();
}

inline void scheduler::start() {
  if (this->cur_state_ != STATE::CREATED && this->cur_state_ != STATE::SPAWNED) {
    log("invalid scheduler state for start");
    throw std::logic_error("invalid scheduler state for start");
  }

  if (this->cur_state_ == STATE::SPAWNED) {
    this->cur_state_ = STATE::STARTED;
    try {
      host_thread_ = std::thread([this]() {
        try {
          this->init();
          this->run();
        } catch (...) {
          state_exception_ = std::current_exception();
        }
      });
    } catch (...) {
      this->cur_state_ = STATE::SPAWNED;
      log("failed to start scheduler worker thread");
      throw;
    }
  } else {
    this->cur_state_ = STATE::STARTED;
  }
}

inline void scheduler::join() {
  if (this->cur_state_ != STATE::STARTED) [[unlikely]] {
    log("invalid scheduler state for join");
    throw std::logic_error("invalid scheduler state for join");
  }

  if (auto &ht = this->host_thread_; ht.joinable()) [[likely]]
    ht.join();
  this->cur_state_ = STATE::JOINED;

  if (auto &se = this->state_exception_; se) [[unlikely]] {
    log("rethrowing scheduler worker exception from join");
    std::rethrow_exception(se);
  }
}

inline void scheduler::run() {
  while (!this->is_stop) [[likely]] {
    this->resume_ready_task();
    this->flush_submissions();
    this->reap_completions();
  }
}

// co_spawn must before start
inline void scheduler::co_spawn(task<void> &&item) {
  if (this->cur_state_ == STATE::CREATED || this->cur_state_ == STATE::SPAWNED) {
    this->cur_state_ = STATE::SPAWNED;
  } else if (this->cur_state_ != STATE::STARTED || core::this_thread.sche != this) {
    log("invalid scheduler state for co_spawn");
    throw std::logic_error("invalid scheduler state for co_spawn");
  }
  auto dt = this->run_detached(std::move(item));
  this->state_.push_handle(dt.release());
}

inline void scheduler::flush_submissions() {
#ifdef CO_MIRA_ENABLE_TESTING
  if (this->test_failure_ == core::scheduler_test_failure::submit_and_wait) [[unlikely]]
    throw_uring_error(EIO, "io_uring_submit_and_wait");
#endif
  this->state_.flush_submissions();
}

inline void scheduler::reap_completions() {

  uint32_t handle_num = this->state_.peek_uring() ? this->state_.reap_completions() : 0;

  bool should_block = !(
      // quick judgment
      handle_num |
      // can submit
      this->state_.submit_ready_count() |
      // can resume
      this->state_.has_task_ready());
  if (!should_block)
    return;

  this->reap_completions_slow();
}

inline void scheduler::reap_completions_slow() {

  if (!this->state_.peek_uring() && this->state_.in_flight_) {
    this->state_.wait_uring();
  }

  uint32_t handle_num = this->state_.reap_completions();

  // TODO: in case of EINTR
  if (!handle_num) [[unlikely]] {
    this->is_stop = true;
  }
}

inline void scheduler::resume_ready_task() {
  auto num = this->state_.size();
  for (; num > 0; --num) {
    this->state_.resume_handle();
  }
}

inline void scheduler::report_exception(std::exception_ptr except) noexcept {
  if (!this->state_exception_) [[unlikely]] {
    co::log("detached task failed; saving exception for join");
    this->state_exception_ = std::move(except);
  }
}

inline core::detached_task scheduler::run_detached(task<void> &&item) { return core::run_detached(*this, std::move(item)); }

} // namespace mira::co

#endif
