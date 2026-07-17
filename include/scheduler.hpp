#ifndef CO_MIRA_SCHEDULER_HPP
#define CO_MIRA_SCHEDULER_HPP

#include "co_mira.hpp"
#include "fixed_queue.hpp"
#include "task.hpp"
#include "task_info.hpp"
#include "thread_meta.hpp"
#include "tuning.hpp"
#include "user_data.hpp"

#include <cerrno>
#include <coroutine>
#include <exception>
#include <iostream>
#include <liburing.h>
#include <print>
#include <thread>

namespace mira::co {

class scheduler;

namespace core {
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

[[nodiscard]] inline detached_task run_detached(scheduler &, task<void>) noexcept;

struct scheduler_state {
  using inner_queue = fixed_queue<std::coroutine_handle<>,
                                  std::remove_cv_t<decltype(tuning::ready_queue_capacity_)>,
                                  tuning::ready_queue_capacity_>;
  using queue_size_t = inner_queue::index_t;

  friend scheduler;

  scheduler_state() = default;
  ~scheduler_state() {
    this->destroy_all_handle();
    if (this->initialized_) {
      // if (!ready_queue_.empty()) {
      //   std::terminate();
      // }
      this_thread.sche_state = nullptr;
      io_uring_queue_exit(&this->ring_);
      this->initialized_ = false;
    }
  }

  void init(unsigned entries) noexcept;
  [[nodiscard]] queue_size_t size() const noexcept;
  void push_handle(co_handle<> handle) noexcept;
  void co_spawn(co_handle<> handle) noexcept;

  [[nodiscard]] constexpr bool has_task_ready() const noexcept;
  [[nodiscard]] io_uring_sqe *get_free_sqe() noexcept;
  void wait_uring() noexcept;
  [[nodiscard]] bool peek_uring() noexcept;
  void handle_cqe(io_uring_cqe *cqe) noexcept;

  [[nodiscard]] co_handle<> get_handle() noexcept;
  void resume_handle() noexcept;

  void flush_submissions() noexcept;
  [[nodiscard]] uint32_t reap_completions() noexcept;

  [[nodiscard]] unsigned submit_ready_count() noexcept;
  void ensure_sq_space(unsigned required) noexcept;

  void destroy_all_handle() noexcept;

  scheduler_state(const scheduler_state &) = delete;
  scheduler_state &operator=(const scheduler_state &) = delete;

  // TODO: support move
  scheduler_state(scheduler_state &&) = delete;
  scheduler_state &operator=(scheduler_state &&) = delete;

private:
  uint32_t need_to_reap_ = 0; // submit but not complete

  io_uring ring_;
  inner_queue ready_queue_;
  bool initialized_ = false;
};
} // namespace core

class scheduler {
public:
  scheduler() = default;
  ~scheduler() {
    if (this->cur_state_ == STATE::STARTED) {
      this->join();
    }
  }

  void co_spawn(task<void> &&item) noexcept;
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

  void init() noexcept;
  void run();
  void flush_submissions() noexcept;
  void reap_completions() noexcept;
  void reap_completions_slow() noexcept;
  void resume_ready_task() noexcept;

  [[nodiscard]] core::detached_task run_detached(task<void> &&) noexcept;

  bool is_stop = false;
  STATE cur_state_ = STATE::CREATED;
  std::thread host_thread_;
  core::scheduler_state state_;
};

namespace core {
inline void scheduler_state::init(unsigned entries) noexcept {
  int ret = io_uring_queue_init(entries, &this->ring_, 0);
  if (ret != 0)
    std::terminate();
  this->initialized_ = true;
  this_thread.sche_state = this;
}

inline scheduler_state::queue_size_t scheduler_state::size() const noexcept {
  return this->ready_queue_.size();
}

inline void scheduler_state::push_handle(co_handle<> handle) noexcept {
  assert(handle && "push an empty task");
  this->ready_queue_.push(handle);
}

inline void scheduler_state::co_spawn(co_handle<> handle) noexcept { this->push_handle(handle); }

inline constexpr bool scheduler_state::has_task_ready() const noexcept {
  return !this->ready_queue_.empty();
}

inline io_uring_sqe *scheduler_state::get_free_sqe() noexcept {
  io_uring_sqe *sqe = io_uring_get_sqe(&this->ring_);
  if (sqe == nullptr) [[unlikely]] {
    std::terminate();
  }

  ++this->need_to_reap_;
  return sqe;
}

inline void scheduler_state::wait_uring() noexcept {
  io_uring_cqe *_;
  int res = io_uring_wait_cqe(&this->ring_, &_);
  if (res < 0) [[unlikely]] {
    std::terminate();
  }
}

inline bool scheduler_state::peek_uring() noexcept {
  io_uring_cqe *cqe;
  io_uring_peek_cqe(&this->ring_, &cqe);
  return cqe != nullptr;
}

inline void scheduler_state::handle_cqe(io_uring_cqe *cqe) noexcept {
  --this->need_to_reap_;

  const int32_t result = cqe->res;
  user_data data(cqe->user_data);

  io_uring_cqe_seen(&this->ring_, cqe);

  using utype = user_data::type;
  auto [info, type] = data.from_user_data();
  assert(type < utype::unknown);

  switch (type) {
  [[likely]] case utype::task_info_pointer:
    info->result = result;
    this->push_handle(info->handle);
    break;

  case utype::task_info_linked:
    info->result = result;
    break;

  [[unlikely]] case utype::unknown:
    assert(false && "handle_cqe: unknown case");
    break;
  }
}

inline co_handle<> scheduler_state::get_handle() noexcept {
  assert(!this->ready_queue_.empty());
  return this->ready_queue_.pop();
}

inline void scheduler_state::resume_handle() noexcept {
  assert(!this->ready_queue_.empty());
  co_handle<> task = this->ready_queue_.pop();
  task.resume();
}

inline void scheduler_state::flush_submissions() noexcept {
  if (io_uring_sq_ready(&this->ring_)) [[likely]] {
    int res = io_uring_submit_and_wait(&this->ring_, !this->has_task_ready());
    // TODO: handle error when res < 0
    if (res < 0) {
      std::terminate();
    }
  }
}

inline uint32_t scheduler_state::reap_completions() noexcept {
  io_uring_cqe *cqe;
  unsigned head, count = 0;
  io_uring_for_each_cqe(&this->ring_, head, cqe) {
    ++count;
    handle_cqe(cqe);
  };
  // TODO: use advance
  return count;
}

inline unsigned scheduler_state::submit_ready_count() noexcept {
  return io_uring_sq_ready(&this->ring_);
}

inline void scheduler_state::ensure_sq_space(unsigned required) noexcept {
  unsigned sq_entries_ = this->ring_.sq.ring_entries;
  if (sq_entries_ < required) [[unlikely]]
    std::terminate();

  while (io_uring_sq_space_left(&this->ring_) < required) {
    int res = 0;
    do {
      res = io_uring_submit(&this->ring_);
    } while (res == -EINTR);

    if (res < 0) [[unlikely]]
      std::terminate();
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

inline detached_task run_detached(scheduler &sche, task<void> item) noexcept {
  try {
    co_await item;
  } catch (...) {
    sche.report_exception(std::current_exception());
  }
}

} // namespace core

inline void scheduler::init() noexcept {
  core::this_thread.sche = this;
  this->state_.init(tuning::default_io_uring_entries);
}

inline void scheduler::start() {
  if (this->cur_state_ != STATE::CREATED && this->cur_state_ != STATE::SPAWNED) {
    std::println(std::cout, "start");
    std::terminate();
  }

  if (this->cur_state_ == STATE::SPAWNED) {
    this->cur_state_ = STATE::STARTED;
    try {
      host_thread_ = std::thread([this]() {
        this->init();
        this->run();
      });
    } catch (...) {
      this->cur_state_ = STATE::SPAWNED;
      throw;
    }
  } else {
    this->cur_state_ = STATE::STARTED;
  }
}

inline void scheduler::join() {
  if (this->cur_state_ != STATE::STARTED) {
    std::println(std::cout, "join");
    std::terminate();
  }
  if (host_thread_.joinable())
    host_thread_.join();
  this->cur_state_ = STATE::JOINED;
}

inline void scheduler::run() {
  while (!this->is_stop) [[likely]] {
    this->resume_ready_task();

    this->flush_submissions();

    this->reap_completions();
  }
}

// co_spawn must before start
inline void scheduler::co_spawn(task<void> &&item) noexcept {
  if (this->cur_state_ == STATE::CREATED || this->cur_state_ == STATE::SPAWNED) {
    this->cur_state_ = STATE::SPAWNED;
  } else if (this->cur_state_ != STATE::STARTED || core::this_thread.sche != this) {
    std::terminate();
  }
  auto dt = this->run_detached(std::move(item));
  this->state_.push_handle(dt.release());
}

inline void scheduler::flush_submissions() noexcept { this->state_.flush_submissions(); }

inline void scheduler::reap_completions() noexcept {

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

inline void scheduler::reap_completions_slow() noexcept {

  if (!this->state_.peek_uring() && this->state_.need_to_reap_) {
    this->state_.wait_uring();
  }

  uint32_t handle_num = this->state_.reap_completions();

  // TODO: in case of EINTR
  if (!handle_num) [[unlikely]] {
    this->is_stop = true;
  }
}

inline void scheduler::resume_ready_task() noexcept {
  auto num = this->state_.size();
  for (; num > 0; --num) {
    this->state_.resume_handle();
  }
}

inline void scheduler::report_exception(std::exception_ptr except) noexcept {
  try {
    std::rethrow_exception(except);
  } catch (const std::exception &error) {
    // log(error.what());
  } catch (...) {
    // log("unknown detached task exception");
  }
}

inline core::detached_task scheduler::run_detached(task<void> &&item) noexcept {
  return core::run_detached(*this, std::move(item));
}

} // namespace mira::co

#endif