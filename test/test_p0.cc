#include "fixed_queue.hpp"
#include "io_awaiter.hpp"
#include "scheduler.hpp"
#include "task.hpp"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

using mira::co::core::scheduler_test_failure;
using mira::co::scheduler;
using mira::co::task;

class test_failure : public std::runtime_error {
public:
  test_failure(const char *expression, const char *file, int line)
      : std::runtime_error(std::string(file) + ':' + std::to_string(line) +
                           ": check failed: " + expression) {}
};

#define CHECK(expression)                                                                          \
  do {                                                                                             \
    if (!(expression)) {                                                                           \
      mira::co::log("check failed: {}", #expression);                                               \
      throw test_failure(#expression, __FILE__, __LINE__);                                         \
    }                                                                                              \
  } while (false)

int failed_tests = 0;

template <typename Function> void run_test(std::string_view name, Function &&function) {
  try {
    std::forward<Function>(function)();
    std::cout << "[PASS] " << name << '\n';
  } catch (const std::exception &error) {
    ++failed_tests;
    std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
  } catch (...) {
    ++failed_tests;
    std::cerr << "[FAIL] " << name << ": unknown exception\n";
  }
}

template <typename Exception, typename Function> Exception check_throws(Function &&function) {
  try {
    std::forward<Function>(function)();
  } catch (const Exception &error) {
    return error;
  }
  mira::co::log("expected exception was not thrown");
  throw test_failure("expected exception", __FILE__, __LINE__);
}

struct test_nop : mira::co::core::io_awaiter {
  test_nop() = default;
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_nop(sqe); }
};

task<void> await_nop() { (void)co_await test_nop{}; }

task<void> complete_normally(bool &completed) {
  completed = true;
  co_return;
}

task<void> no_op() { co_return; }

void test_throw_uring_error_uses_positive_errno() {
  auto error = check_throws<std::system_error>(
      [] { mira::co::throw_uring_error(EACCES, "p0_test_operation"); });
  CHECK(error.code().value() == EACCES);
  CHECK(error.code().category() == std::generic_category());
  CHECK(std::string_view(error.what()).find("p0_test_operation") != std::string_view::npos);
}

void test_fixed_queue_full_and_empty() {
  mira::fixed_queue<int, uint8_t, 4> queue;
  auto empty_error = check_throws<std::out_of_range>([&] { (void)queue.pop(); });
  CHECK(std::string_view(empty_error.what()) == "fixed_queue is empty");

  queue.push(1);
  queue.push(2);
  queue.push(3);
  auto full_error = check_throws<std::length_error>([&] { queue.push(4); });
  CHECK(std::string_view(full_error.what()) == "fixed_queue is full");
  CHECK(queue.pop() == 1);
  CHECK(queue.pop() == 2);
  CHECK(queue.pop() == 3);
}

void test_invalid_scheduler_states() {
  scheduler never_started;
  (void)check_throws<std::logic_error>([&] { never_started.join(); });

  scheduler started_twice;
  started_twice.co_spawn(no_op());
  started_twice.start();
  (void)check_throws<std::logic_error>([&] { started_twice.start(); });
  started_twice.join();
  (void)check_throws<std::logic_error>([&] { started_twice.start(); });
  (void)check_throws<std::logic_error>([&] { started_twice.co_spawn(no_op()); });
  (void)check_throws<std::logic_error>([&] { started_twice.join(); });
}

void test_worker_init_failure_reaches_join() {
  scheduler sche(scheduler_test_failure::init);
  sche.co_spawn(no_op());
  sche.start();
  auto error = check_throws<std::system_error>([&] { sche.join(); });
  CHECK(error.code().value() == EINVAL);
  CHECK(std::string_view(error.what()).find("io_uring_queue_init") != std::string_view::npos);
}

void test_worker_submit_failure_reaches_join() {
  bool completed = false;
  scheduler sche(scheduler_test_failure::submit_and_wait);
  sche.co_spawn(complete_normally(completed));
  sche.start();
  auto error = check_throws<std::system_error>([&] { sche.join(); });
  CHECK(completed);
  CHECK(error.code().value() == EIO);
  CHECK(std::string_view(error.what()).find("io_uring_submit_and_wait") !=
        std::string_view::npos);
}

void test_io_awaiter_failure_reaches_join() {
  scheduler sche(scheduler_test_failure::io_awaiter);
  sche.co_spawn(await_nop());
  sche.start();
  auto error = check_throws<std::system_error>([&] { sche.join(); });
  CHECK(error.code().value() == ENOSPC);
  CHECK(std::string_view(error.what()).find("io_uring_get_sqe") != std::string_view::npos);
}

} // namespace

int main() {
  run_test("positive errno system_error", test_throw_uring_error_uses_positive_errno);
  run_test("fixed queue full and empty", test_fixed_queue_full_and_empty);
  run_test("invalid scheduler states", test_invalid_scheduler_states);
  run_test("worker init failure reaches join", test_worker_init_failure_reaches_join);
  run_test("worker submit failure reaches join", test_worker_submit_failure_reaches_join);
  run_test("IO awaiter failure reaches join", test_io_awaiter_failure_reaches_join);

  if (failed_tests != 0) {
    std::cerr << failed_tests << " test(s) failed\n";
    return 1;
  }

  std::cout << "All P0 tests passed\n";
  return 0;
}
