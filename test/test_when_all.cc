#include "when_all.hpp"

#include "io.hpp"
#include "scheduler.hpp"
#include "task.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

using namespace std::chrono_literals;
using mira::co::scheduler;
using mira::co::task;

using nop_pair_task = decltype(mira::co::when_all(mira::co::io::nop(), mira::co::io::nop()));
static_assert(std::same_as<nop_pair_task, task<std::tuple<int32_t, int32_t>>>);
using unsafe_nop_pair_task =
    decltype(mira::co::when_all<false>(mira::co::io::nop(), mira::co::io::nop()));
static_assert(std::same_as<unsafe_nop_pair_task, task<std::tuple<int32_t, int32_t>>>);

class test_failure : public std::runtime_error {
public:
  test_failure(const char *expression, const char *file, int line)
      : std::runtime_error(std::string(file) + ':' + std::to_string(line) +
                           ": check failed: " + expression) {}
};

#define CHECK(expression)                                                                          \
  do {                                                                                             \
    if (!(expression))                                                                             \
      throw test_failure(#expression, __FILE__, __LINE__);                                         \
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

void run_scheduler(task<void> item) {
  scheduler sche;
  sche.co_spawn(std::move(item));
  sche.start();
  sche.join();
}

task<std::unique_ptr<int>> make_move_only_value() { co_return std::make_unique<int>(42); }

task<void> finish_void(bool &completed) {
  completed = true;
  co_return;
}

task<int &> return_reference(int &value) { co_return value; }

task<int> fail_immediately() {
  throw std::runtime_error("when_all child failure");
  co_return 0;
}

task<void> await_nops(std::tuple<int32_t, int32_t> &results) {
  results = co_await mira::co::when_all(mira::co::io::nop(), mira::co::io::nop());
}

task<void> await_nops_unsafe(std::tuple<int32_t, int32_t> &results) {
  results = co_await mira::co::when_all<false>(mira::co::io::nop(), mira::co::io::nop());
}

task<void> await_empty(bool &completed) {
  auto results = co_await mira::co::when_all();
  static_assert(std::same_as<decltype(results), std::tuple<>>);
  completed = true;
}

task<void> await_slow_child(std::chrono::steady_clock::duration &elapsed,
                            std::tuple<int32_t, int32_t> &results) {
  const auto start = std::chrono::steady_clock::now();
  results = co_await mira::co::when_all(mira::co::io::nop(), mira::co::io::sleep_for(10ms));
  elapsed = std::chrono::steady_clock::now() - start;
}

task<void> await_child_exception(bool &caught, std::chrono::steady_clock::duration &elapsed) {
  const auto start = std::chrono::steady_clock::now();
  try {
    (void)co_await mira::co::when_all(fail_immediately(), mira::co::io::sleep_for(10ms));
  } catch (const std::runtime_error &error) {
    caught = std::string_view(error.what()) == "when_all child failure";
  }
  elapsed = std::chrono::steady_clock::now() - start;
}

task<void> await_move_only_result(std::unique_ptr<int> &value, int32_t &nop_result) {
  auto results = co_await mira::co::when_all(make_move_only_value(), mira::co::io::nop());
  value = std::move(std::get<0>(results));
  nop_result = std::get<1>(results);
}

task<void> await_void_and_reference(bool &void_completed, int &value, bool &same_address) {
  auto results = co_await mira::co::when_all(finish_void(void_completed), return_reference(value));
  static_assert(
      std::same_as<decltype(results), std::tuple<std::monostate, std::reference_wrapper<int>>>);
  same_address = &std::get<1>(results).get() == &value;
}

task<void> await_negative_errno(int32_t &close_result, int32_t &nop_result) {
  auto results = co_await mira::co::when_all(mira::co::io::close(-1), mira::co::io::nop());
  close_result = std::get<0>(results);
  nop_result = std::get<1>(results);
}

task<void> await_nested(std::tuple<int32_t, int32_t> &inner_results, int32_t &outer_nop_result) {
  auto inner = mira::co::when_all(mira::co::io::nop(), mira::co::io::nop());
  auto results = co_await mira::co::when_all(std::move(inner), mira::co::io::nop());
  inner_results = std::move(std::get<0>(results));
  outer_nop_result = std::get<1>(results);
}

task<int> finish_on(scheduler &target, int value, std::thread::id &thread_id) {
  co_await mira::co::resume_on(target);
  thread_id = std::this_thread::get_id();
  co_return value;
}

task<void> await_cross_scheduler(scheduler &second, scheduler &third,
                                 std::atomic<bool> &completed,
                                 std::atomic<bool> &failed) {
  const std::thread::id origin_thread = std::this_thread::get_id();
  std::thread::id second_thread;
  std::thread::id third_thread;

  auto results = co_await mira::co::when_all(finish_on(second, 11, second_thread),
                                              finish_on(third, 22, third_thread));

  if (std::this_thread::get_id() != origin_thread || second_thread == origin_thread ||
      third_thread == origin_thread || second_thread == third_thread ||
      std::get<0>(results) != 11 || std::get<1>(results) != 22)
    failed.store(true, std::memory_order_release);

  completed.store(true, std::memory_order_release);
}

task<void> stop_when_completed(scheduler &first, scheduler &second, scheduler &third,
                               const std::atomic<bool> &completed,
                               std::atomic<bool> &timed_out) {
  unsigned attempts = 0;
  while (!completed.load(std::memory_order_acquire) && attempts++ != 20'000)
    (void)co_await mira::co::io::sleep_for(100us);

  if (!completed.load(std::memory_order_acquire))
    timed_out.store(true, std::memory_order_release);

  first.request_stop();
  second.request_stop();
  third.request_stop();
}

void test_nops() {
  std::tuple<int32_t, int32_t> results{-1, -1};
  run_scheduler(await_nops(results));
  CHECK(std::get<0>(results) == 0);
  CHECK(std::get<1>(results) == 0);
}

void test_unsafe_nops() {
  std::tuple<int32_t, int32_t> results{-1, -1};
  run_scheduler(await_nops_unsafe(results));
  CHECK(std::get<0>(results) == 0);
  CHECK(std::get<1>(results) == 0);
}

void test_empty() {
  bool completed = false;
  run_scheduler(await_empty(completed));
  CHECK(completed);
}

void test_waits_for_every_child() {
  std::chrono::steady_clock::duration elapsed{};
  std::tuple<int32_t, int32_t> results{};
  run_scheduler(await_slow_child(elapsed, results));
  CHECK(std::get<0>(results) == 0);
  CHECK(std::get<1>(results) == -ETIME);
  CHECK(elapsed >= 5ms);
}

void test_exception_waits_for_every_child() {
  bool caught = false;
  std::chrono::steady_clock::duration elapsed{};
  run_scheduler(await_child_exception(caught, elapsed));
  CHECK(caught);
  CHECK(elapsed >= 5ms);
}

void test_move_only_result() {
  std::unique_ptr<int> value;
  int32_t nop_result = -1;
  run_scheduler(await_move_only_result(value, nop_result));
  CHECK(value);
  CHECK(*value == 42);
  CHECK(nop_result == 0);
}

void test_void_and_reference_results() {
  bool void_completed = false;
  bool same_address = false;
  int value = 7;
  run_scheduler(await_void_and_reference(void_completed, value, same_address));
  CHECK(void_completed);
  CHECK(same_address);
}

void test_negative_errno_is_a_result() {
  int32_t close_result = 0;
  int32_t nop_result = -1;
  run_scheduler(await_negative_errno(close_result, nop_result));
  CHECK(close_result == -EBADF);
  CHECK(nop_result == 0);
}

void test_nested_when_all() {
  std::tuple<int32_t, int32_t> inner_results{-1, -1};
  int32_t outer_nop_result = -1;
  run_scheduler(await_nested(inner_results, outer_nop_result));
  CHECK(std::get<0>(inner_results) == 0);
  CHECK(std::get<1>(inner_results) == 0);
  CHECK(outer_nop_result == 0);
}

void test_cross_scheduler_when_all() {
  scheduler first;
  scheduler second;
  scheduler third;
  scheduler coordinator;
  std::atomic<bool> completed = false;
  std::atomic<bool> failed = false;
  std::atomic<bool> timed_out = false;

  second.start_remote();
  third.start_remote();
  first.co_spawn(await_cross_scheduler(second, third, completed, failed));
  first.start_remote();

  coordinator.co_spawn(stop_when_completed(first, second, third, completed, timed_out));
  coordinator.start();
  coordinator.join();
  first.join();
  second.join();
  third.join();

  CHECK(!timed_out.load(std::memory_order_acquire));
  CHECK(!failed.load(std::memory_order_acquire));
  CHECK(completed.load(std::memory_order_acquire));
}

} // namespace

int main() {
  run_test("two nops return both results", test_nops);
  run_test("unsafe two nops return both results", test_unsafe_nops);
  run_test("empty when_all is immediately ready", test_empty);
  run_test("when_all waits for every child", test_waits_for_every_child);
  run_test("child exception waits then propagates", test_exception_waits_for_every_child);
  run_test("move-only task result", test_move_only_result);
  run_test("void and reference task results", test_void_and_reference_results);
  run_test("negative errno remains a result", test_negative_errno_is_a_result);
  run_test("nested when_all tasks", test_nested_when_all);
  run_test("cross-scheduler when_all returns to origin", test_cross_scheduler_when_all);
  return failed_tests == 0 ? 0 : 1;
}
