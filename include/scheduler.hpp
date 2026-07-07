#ifndef CO_MIRA_SCHEDULER_HPP
#define CO_MIRA_SCHEDULER_HPP

#include "fixed_queue.hpp"
#include "task.hpp"
#include "task_info.hpp"
#include "thread_meta.hpp"
#include "tuning.hpp"
#include "user_data.hpp"

#include <coroutine>
#include <exception>
#include <liburing.h>

namespace mira::co {

namespace core {
struct scheduler_state {
  using inner_queue = fixed_queue<std::coroutine_handle<>,
                                  std::remove_cv_t<decltype(tuning::ready_queue_capacity_)>,
                                  tuning::ready_queue_capacity_>;
  using queue_size_t = inner_queue::index_t;

  constexpr scheduler_state() = default;
  constexpr ~scheduler_state() {
    if (initialized_) {
      io_uring_queue_exit(&ring_);
      initialized_ = false;
    }
  }

  void init(unsigned entries) noexcept;
  queue_size_t size() const noexcept;
  void push_handle(co_handle<> handle) noexcept;
  void co_spawn(co_handle<> handle) noexcept;

  [[nodiscard]] constexpr bool has_task_ready() const noexcept;
  io_uring_sqe *get_free_sqe() noexcept;
  void wait_uring() noexcept;
  bool peek_uring() noexcept;
  void handle_cqe(io_uring_cqe *cqe) noexcept;

  co_handle<> get_handle() noexcept;
  void resume_handle() noexcept;

  void flush_submissions() noexcept;
  uint32_t reap_completions() noexcept;

  uint32_t need_to_reap_ = 0;   // submit but not complete
  uint32_t need_to_submit_ = 0; // not submit

  scheduler_state(const scheduler_state &) = delete;
  scheduler_state &operator=(const scheduler_state &) = delete;

  // TODO: support move
  scheduler_state(scheduler_state &&) = delete;
  scheduler_state &operator=(scheduler_state &&) = delete;

private:
  io_uring ring_;
  inner_queue ready_queue_;
  bool initialized_ = false;
};
} // namespace core

class scheduler {
public:
  scheduler() { init(); }
  ~scheduler() = default;

  void init() noexcept;
  void run();
  void co_spawn(task<void> &&item) noexcept;
  void flush_submissions() noexcept;
  void reap_completions() noexcept;
  void reap_completions_slow() noexcept;
  void resume_ready_task() noexcept;

  scheduler(const scheduler &) = delete;
  scheduler &operator=(const scheduler &) = delete;

  // TODO: support move
  scheduler(scheduler &&) = delete;
  scheduler &operator=(scheduler &&) = delete;

  // TODO: move to private
  core::scheduler_state state_;

private:
  bool is_stop = false;
};

namespace core {
void scheduler_state::init(unsigned entries) noexcept {
  int ret = io_uring_queue_init(entries, &ring_, 0);
  if (ret != 0)
    std::terminate();
  initialized_ = true;
  this_thread.sche_state = this;
}

scheduler_state::queue_size_t scheduler_state::size() const noexcept { return ready_queue_.size(); }

void scheduler_state::push_handle(co_handle<> handle) noexcept {
  assert(handle && "push an empty task");
  ready_queue_.push(handle);
}

void scheduler_state::co_spawn(co_handle<> handle) noexcept { push_handle(handle); }

constexpr bool scheduler_state::has_task_ready() const noexcept { return !ready_queue_.empty(); }

io_uring_sqe *scheduler_state::get_free_sqe() noexcept {
  io_uring_sqe *sqe = io_uring_get_sqe(&ring_);
  assert(sqe != nullptr);

  ++need_to_submit_;
  ++need_to_reap_;
  return sqe;
}

void scheduler_state::wait_uring() noexcept {
  io_uring_cqe *_;
  io_uring_wait_cqe(&ring_, &_);
}

bool scheduler_state::peek_uring() noexcept {
  io_uring_cqe *cqe;
  io_uring_peek_cqe(&ring_, &cqe);
  return cqe != nullptr;
}

void scheduler_state::handle_cqe(io_uring_cqe *cqe) noexcept {
  --need_to_reap_;

  const int32_t result = cqe->res;
  user_data data(cqe->user_data);

  io_uring_cqe_seen(&ring_, cqe);

  using utype = user_data::type;
  auto [info, type] = data.from_user_data();
  assert(type < utype::unknown);

  switch (type) {
  [[likely]] case utype::task_info_pointer:
    info->result = result;
    push_handle(info->handle);
    break;

  case utype::task_info_linked:
    info->result = result;
    break;

  [[unlikely]] case utype::unknown:
    assert(false && "handle_cqe: unknown case");
    break;
  }
}

co_handle<> scheduler_state::get_handle() noexcept {
  assert(!ready_queue_.empty());
  return ready_queue_.pop();
}

void scheduler_state::resume_handle() noexcept {
  assert(!ready_queue_.empty());
  co_handle<> task = ready_queue_.pop();
  task.resume();
}

void scheduler_state::flush_submissions() noexcept {
  if (need_to_submit_) [[likely]] {
    int res = io_uring_submit_and_wait(&ring_, !has_task_ready());
    // TODO: handle error when res < 0
    assert(res >= 0 && "submit_and_wait error");
    need_to_submit_ = 0;
  }
}

uint32_t scheduler_state::reap_completions() noexcept {
  io_uring_cqe *cqe;
  unsigned head, count = 0;
  io_uring_for_each_cqe(&ring_, head, cqe) {
    ++count;
    handle_cqe(cqe);
  };
  return count;
}

} // namespace core

void scheduler::init() noexcept { state_.init(tuning::default_io_uring_entries); }

void scheduler::run() {
  while (!is_stop) [[likely]] {
    resume_ready_task();

    flush_submissions();

    reap_completions();
  }
}

void scheduler::co_spawn(task<void> &&item) noexcept {
  co_handle<> handle = item.get_handle();
  item.detach();
  state_.push_handle(handle);
}

void scheduler::flush_submissions() noexcept { state_.flush_submissions(); }

void scheduler::reap_completions() noexcept {

  uint32_t handle_num = state_.peek_uring() ? state_.reap_completions() : 0;

  bool should_block = !(
      // quick judgment
      handle_num |
      // can submit
      state_.need_to_submit_ |
      // can resume
      state_.has_task_ready());
  if (!should_block)
    return;

  reap_completions_slow();
}

void scheduler::reap_completions_slow() noexcept {

  if (!state_.peek_uring() && state_.need_to_reap_) {
    state_.wait_uring();
  }

  uint32_t handle_num = state_.reap_completions();

  // TODO: in case of EINTR
  if (!handle_num) [[unlikely]] {
    is_stop = true;
  }
}

void scheduler::resume_ready_task() noexcept {
  auto num = state_.size();
  for (; num > 0; --num) {
    state_.resume_handle();
  }
}

} // namespace mira::co

#endif