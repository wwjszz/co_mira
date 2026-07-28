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

#include <atomic>
#include <cerrno>
#include <coroutine>
#include <exception>
#include <experimental/scope>
#include <liburing.h>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace mira::co {

class scheduler;

struct scheduler_config {
  unsigned io_uring_entries = tuning::default_io_uring_entries;
  unsigned io_uring_setup_flags =
      IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG;
};

namespace core {

struct resume_on_awaiter;

inline constexpr int32_t resume_on_target_result = 1;

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
  [[nodiscard]] handle_type native_handle() const noexcept { return this->handle_; }

private:
  handle_type handle_;
};

[[nodiscard]] inline detached_task run_detached(scheduler &, task<void>);

inline void destroy_detached_handle(co_handle<> handle) noexcept {
  if (!handle)
    return;
#ifdef CO_MIRA_ENABLE_COUNTERS
  ::mira::co::handle_counter.decrement();
#endif
  handle.destroy();
}

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
      io_uring_queue_exit(&this->ring_);
      this->initialized_ = false;
    }
  }

  void init(unsigned entries, unsigned setup_flags);
  void deinit() noexcept;
  [[nodiscard]] queue_size_t size() const noexcept;
  void push_handle(co_handle<> handle);

  void co_spawn_msg_ring(co_handle<> handle);
  void resume_on_msg_ring(task_info *info);
  void request_stop() noexcept;
  void request_stop_msg_ring();
  void begin_shutdown();
  [[nodiscard]] bool stop_requested() const noexcept;

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
  int ring_fd_ = -1;

  inner_queue ready_queue_;
  bool initialized_ = false;
  bool stop_requested_ = false;
  bool shutdown_cancel_submitted_ = false;
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
// co_spawn() is allowed before start(), from the scheduler's own host thread,
// or from another running scheduler host thread when the target was started by
// start_remote(). Ordinary external threads cannot submit after start().
class scheduler {
  friend struct core::resume_on_awaiter;

public:
  enum class runtime_state : uint8_t {
    stopped,
    starting,
    running,
    stopping,
  };

  scheduler() = default;
  explicit scheduler(scheduler_config config) noexcept : config_(config) {}
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
  void start_remote();
  void request_stop();
  void join();

  [[nodiscard]] runtime_state status() const noexcept { return this->runtime_state_.load(std::memory_order_acquire); }

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
  void start_impl(bool accept_remote);
  void run();
  void flush_submissions();
  void resume_ready_task();

  [[nodiscard]] core::detached_task run_detached(task<void> &&);

  STATE cur_state_ = STATE::CREATED;
  bool accept_remote_ = false;
  scheduler_config config_;
  std::atomic<runtime_state> runtime_state_{runtime_state::stopped};
  std::thread host_thread_;
  core::scheduler_state state_;
  std::exception_ptr state_exception_;
#ifdef CO_MIRA_ENABLE_TESTING
  core::scheduler_test_failure test_failure_ = core::scheduler_test_failure::none;
#endif
};

namespace core {
struct [[nodiscard]] resume_on_awaiter {
  explicit resume_on_awaiter(scheduler &target) noexcept : target_(&target) {}

  [[nodiscard]] bool await_ready() const noexcept { return this_thread.sche == this->target_; }

  void await_suspend(co_handle<> handle) {
    if (this_thread.sche_state == nullptr) [[unlikely]] {
      log("resume_on requires a scheduler host thread");
      throw std::logic_error("resume_on requires a scheduler host thread");
    }

    const scheduler::runtime_state state = this->target_->runtime_state_.load(std::memory_order_acquire);
    if (state != scheduler::runtime_state::running || !this->target_->accept_remote_) [[unlikely]] {
      log("resume_on target must be a running start_remote scheduler");
      throw std::logic_error("resume_on target must be a running start_remote scheduler");
    }

    this->info_.handle = handle;
    this->target_->state_.resume_on_msg_ring(&this->info_);
  }

  void await_resume() const {
    if (this->info_.result < 0) [[unlikely]]
      throw_uring_error(-this->info_.result, "IORING_OP_MSG_RING resume_on");
  }

  resume_on_awaiter(const resume_on_awaiter &) = delete;
  resume_on_awaiter &operator=(const resume_on_awaiter &) = delete;
  resume_on_awaiter(resume_on_awaiter &&) = delete;
  resume_on_awaiter &operator=(resume_on_awaiter &&) = delete;

private:
  scheduler *target_;
  task_info info_;
};

inline void scheduler_state::init(unsigned entries, unsigned setup_flags) {
#ifdef CO_MIRA_ENABLE_TESTING
  if (this->test_failure_ == scheduler_test_failure::init) [[unlikely]]
    throw_uring_error(EINVAL, "io_uring_queue_init");
#endif

  int ret = io_uring_queue_init(entries, &this->ring_, setup_flags);
  if (ret != 0) [[unlikely]]
    throw_uring_error(-ret, "io_uring_queue_init");

  this->ring_fd_ = this->ring_.ring_fd;
  this->initialized_ = true;
  this_thread.sche_state = this;
}

inline void scheduler_state::deinit() noexcept {
  this_thread.sche_state = nullptr;
  if (this->initialized_) {
    io_uring_queue_exit(&this->ring_);
    this->ring_fd_ = -1;
    this->initialized_ = false;
  }
}

inline scheduler_state::queue_size_t scheduler_state::size() const noexcept { return this->ready_queue_.size(); }

inline void scheduler_state::push_handle(co_handle<> handle) {
  if (handle == nullptr) [[unlikely]] {
    log("pushing an empty handle");
    throw std::invalid_argument("push an empty handle");
  }
  this->ready_queue_.push(handle);
}

inline void scheduler_state::co_spawn_msg_ring(co_handle<> handle) {
  if (handle == nullptr) [[unlikely]] {
    log("pushing an empty handle");
    throw std::invalid_argument("push an empty handle");
  }
  scheduler_state *const current = this_thread.sche_state;
  if (current == nullptr) [[unlikely]] {
    log("MSG_RING spawn requires a scheduler host thread");
    throw std::logic_error("MSG_RING spawn requires a scheduler host thread");
  }
  io_uring_sqe *sqe = current->get_free_sqe();
  uint64_t data = reinterpret_cast<uint64_t>(handle.address()) | static_cast<uint8_t>(user_data::type::msg_ring);
  io_uring_prep_msg_ring(sqe, this->ring_fd_, 0, data, 0);
  sqe->user_data = reinterpret_cast<uint64_t>(handle.address()) | static_cast<uint8_t>(user_data::type::msg_ring_ack);
}

inline void scheduler_state::resume_on_msg_ring(task_info *info) {
  scheduler_state *const current = this_thread.sche_state;
  if (current == nullptr) [[unlikely]] {
    log("resume_on requires a scheduler host thread");
    throw std::logic_error("resume_on requires a scheduler host thread");
  }

  io_uring_sqe *sqe = current->get_free_sqe();
  const uint64_t data = info->as_user_data() | static_cast<uint8_t>(user_data::type::resume_on);
  io_uring_prep_msg_ring(sqe, this->ring_fd_, resume_on_target_result, data, 0);
  sqe->user_data = data;
}

inline void scheduler_state::request_stop() noexcept { this->stop_requested_ = true; }

inline void scheduler_state::request_stop_msg_ring() {
  scheduler_state *const current = this_thread.sche_state;
  if (current == nullptr) [[unlikely]] {
    log("MSG_RING stop requires a scheduler host thread");
    throw std::logic_error("MSG_RING stop requires a scheduler host thread");
  }

  io_uring_sqe *sqe = current->get_free_sqe();
  const uint64_t target_data = static_cast<uint64_t>(user_data::type::msg_ring_stop);
  io_uring_prep_msg_ring(sqe, this->ring_fd_, 0, target_data, 0);
  sqe->user_data = static_cast<uint64_t>(user_data::type::msg_ring_stop_ack);
}

inline void scheduler_state::begin_shutdown() {
  if (this->shutdown_cancel_submitted_ || this->in_flight_ == 0)
    return;

  io_uring_sqe *sqe = this->get_free_sqe();
  io_uring_prep_cancel64(sqe, 0, IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL);
  sqe->user_data = user_data::shutdown_cancel;
  this->shutdown_cancel_submitted_ = true;
}

inline bool scheduler_state::stop_requested() const noexcept { return this->stop_requested_; }

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

  if (cqe->user_data == user_data::shutdown_cancel) {
    if (result < 0 && result != -ENOENT && result != -EALREADY) [[unlikely]] {
      log("shutdown cancel-all failed with errno {}", -result);
      this_thread.sche->report_exception(
          std::make_exception_ptr(std::system_error(-result, std::generic_category(), "IORING_OP_ASYNC_CANCEL shutdown")));
    }
    return;
  }

  user_data data(cqe->user_data);

  using utype = user_data::type;
  const auto type = data.type_from_user_data();

  if (type == utype::unknown) [[unlikely]] {
    log("handle_cqe: unknown user_data type");
    std::terminate();
  }

  if (type == utype::task_info_pointer || type == utype::task_info_linked) {
    auto *info = data.task_info_from_user_data();

    if (auto *state = std::exchange(info->cancel, nullptr))
      state->mark_completed();

    info->result = result;

    if (auto *completion = std::exchange(info->completion, nullptr)) {
      if (completion->complete_one())
        this->push_handle(completion->continuation);
    } else if (type == utype::task_info_pointer) [[likely]] {
      this->push_handle(info->handle);
    }
  } else if (type == utype::msg_ring) {
    co_handle<> handle = co_handle<>::from_address(data.address_from_user_data());
    try {
      this->push_handle(handle);
    } catch (...) {
      destroy_detached_handle(handle);
      throw;
    }
  } else if (type == utype::msg_ring_ack) {
    if (cqe->res < 0) {
      co_handle<> handle = co_handle<>::from_address(data.address_from_user_data());
      destroy_detached_handle(handle);
      log("IORING_OP_MSG_RING spawn failed with errno {}", -cqe->res);
      this_thread.sche->report_exception(std::make_exception_ptr(std::system_error(-cqe->res, std::generic_category(), "IORING_OP_MSG_RING spawn")));
    }
  } else if (type == utype::msg_ring_stop) {
    this->request_stop();
  } else if (type == utype::msg_ring_stop_ack && cqe->res < 0) {
    log("IORING_OP_MSG_RING stop failed with errno {}", -cqe->res);
    this_thread.sche->report_exception(std::make_exception_ptr(std::system_error(-cqe->res, std::generic_category(), "IORING_OP_MSG_RING stop")));
  } else if (type == utype::resume_on) {
    if (result == 0)
      return;

    task_info *const info = data.task_info_from_user_data();
    if (result == resume_on_target_result) {
      info->result = 0;
      info->handle.resume();
    } else if (result < 0) {
      info->result = result;
      info->handle.resume();
    } else [[unlikely]] {
      log("unexpected MSG_RING resume_on result {}", result);
      this_thread.sche->report_exception(
          std::make_exception_ptr(std::system_error(EPROTO, std::generic_category(), "IORING_OP_MSG_RING resume_on result")));
    }
  }
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
  unsigned head, consumed = 0, completed_local = 0;

  auto guard = std::experimental::scope_exit([&]() noexcept {
    if (consumed != 0) [[likely]] {
      if (completed_local > this->in_flight_) [[unlikely]] {
        log("local CQE count {} exceeds in-flight request count {}", completed_local, this->in_flight_);
        std::terminate();
      }
      io_uring_cq_advance(&this->ring_, consumed);
      this->in_flight_ -= completed_local;
    }
  });

  io_uring_for_each_cqe(&this->ring_, head, cqe) {
    ++consumed;
    const bool shutdown_cancel = cqe->user_data == user_data::shutdown_cancel;
    const user_data::type type = shutdown_cancel ? user_data::type::unknown : user_data(cqe->user_data).type_from_user_data();
    const bool injected = type == user_data::type::msg_ring || type == user_data::type::msg_ring_stop ||
                          (type == user_data::type::resume_on && cqe->res == resume_on_target_result);
    if (!injected)
      ++completed_local;
    handle_cqe(cqe);
  }

  return consumed;
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
    destroy_detached_handle(handle);
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
  this->state_.init(this->config_.io_uring_entries, this->config_.io_uring_setup_flags);
}

inline void scheduler::deinit() noexcept {
  core::this_thread.sche = nullptr;
  this->state_.deinit();
}

inline void scheduler::start() { this->start_impl(false); }

inline void scheduler::start_remote() { this->start_impl(true); }

inline void scheduler::start_impl(bool accept_remote) {
  if (this->cur_state_ != STATE::CREATED && this->cur_state_ != STATE::SPAWNED) {
    log("invalid scheduler state for start");
    throw std::logic_error("invalid scheduler state for start");
  }

  const STATE previous = this->cur_state_;
  const bool launch_worker = accept_remote || previous == STATE::SPAWNED;
  this->accept_remote_ = accept_remote;
  this->cur_state_ = STATE::STARTED;

  if (!launch_worker)
    return;

  this->runtime_state_.store(runtime_state::starting, std::memory_order_release);
  try {
    host_thread_ = std::thread([this]() {
      try {
        this->init();
        this->runtime_state_.store(runtime_state::running, std::memory_order_release);
        this->runtime_state_.notify_all();
        this->run();
      } catch (...) {
        this->state_exception_ = std::current_exception();
      }

      this->deinit();
      this->runtime_state_.store(runtime_state::stopped, std::memory_order_release);
      this->runtime_state_.notify_all();
    });
  } catch (...) {
    this->runtime_state_.store(runtime_state::stopped, std::memory_order_release);
    this->runtime_state_.notify_all();
    this->cur_state_ = previous;
    this->accept_remote_ = false;
    log("failed to start scheduler worker thread");
    throw;
  }

  while (this->runtime_state_.load(std::memory_order_acquire) == runtime_state::starting)
    this->runtime_state_.wait(runtime_state::starting, std::memory_order_acquire);
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
  for (;;) {
    this->resume_ready_task();
    this->flush_submissions();

    if (this->state_.peek_uring())
      (void)this->state_.reap_completions();

    if (this->state_.stop_requested()) {
      this->runtime_state_.store(runtime_state::stopping, std::memory_order_release);
      this->runtime_state_.notify_all();
      this->state_.begin_shutdown();
      this->flush_submissions();
      if (!this->state_.has_task_ready() && this->state_.submit_ready_count() == 0 && this->state_.in_flight_ == 0)
        break;
    }

    if (this->state_.has_task_ready() || this->state_.submit_ready_count() != 0)
      continue;

    if (this->state_.in_flight_ != 0 || this->accept_remote_) {
      this->state_.wait_uring();
      continue;
    }

    break;
  }
}

inline void scheduler::co_spawn(task<void> &&item) {
  const runtime_state runtime = this->runtime_state_.load(std::memory_order_acquire);
  if (runtime == runtime_state::running) {
    auto detached = this->run_detached(std::move(item));

    if (core::this_thread.sche == this) {
      this->state_.push_handle(detached.native_handle());
    } else if (core::this_thread.sche_state != nullptr && this->accept_remote_) {
      this->state_.co_spawn_msg_ring(detached.native_handle());
    } else {
      log("cross-thread co_spawn requires another scheduler host thread and a "
          "start_remote target");
      throw std::logic_error("cross-thread co_spawn requires MSG_RING scheduler threads");
    }

    (void)detached.release();
    return;
  }

  if (this->cur_state_ == STATE::CREATED || this->cur_state_ == STATE::SPAWNED) {
    this->cur_state_ = STATE::SPAWNED;
  } else {
    log("invalid scheduler state for co_spawn");
    throw std::logic_error("invalid scheduler state for co_spawn");
  }

  auto detached = this->run_detached(std::move(item));
  this->state_.push_handle(detached.native_handle());
  (void)detached.release();
}

inline void scheduler::request_stop() {
  if (this->runtime_state_.load(std::memory_order_acquire) != runtime_state::running) {
    log("request_stop requires a running scheduler");
    throw std::logic_error("request_stop requires a running scheduler");
  }

  if (core::this_thread.sche == this) {
    this->state_.request_stop();
    this->runtime_state_.store(runtime_state::stopping, std::memory_order_release);
    this->runtime_state_.notify_all();
  } else if (core::this_thread.sche_state != nullptr && this->accept_remote_) {
    this->state_.request_stop_msg_ring();
  } else {
    log("request_stop requires this scheduler or another scheduler host "
        "thread");
    throw std::logic_error("request_stop requires a scheduler host thread");
  }
}

inline void scheduler::flush_submissions() {
#ifdef CO_MIRA_ENABLE_TESTING
  if (this->test_failure_ == core::scheduler_test_failure::submit_and_wait) [[unlikely]]
    throw_uring_error(EIO, "io_uring_submit_and_wait");
#endif
  this->state_.flush_submissions();
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

[[nodiscard]] inline core::resume_on_awaiter resume_on(scheduler &target) noexcept { return core::resume_on_awaiter(target); }

} // namespace mira::co

#endif
