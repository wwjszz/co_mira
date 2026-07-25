#include "when_any.hpp"
#include "when_some.hpp"

#include "io.hpp"
#include "scheduler.hpp"
#include "task.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
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

task<int> value_after(std::chrono::milliseconds delay, int value) {
  if (delay != 0ms)
    (void)co_await mira::co::io::sleep_for(delay);
  co_return value;
}

task<std::unique_ptr<int>> move_only_value() {
  co_return std::make_unique<int>(42);
}

task<void> finish_void(bool &completed) {
  completed = true;
  co_return;
}

task<int &> return_reference(int &value) { co_return value; }

task<int> fail_immediately() {
  throw std::runtime_error("selected child failure");
  co_return 0;
}

task<void> await_any(std::size_t &index, int &value) {
  auto result =
      co_await mira::co::when_any(value_after(0ms, 11), value_after(10ms, 22));
  index = result.index;
  value = std::get<1>(result.value);
}

task<void> await_any_unsafe(std::size_t &index, int &value) {
  auto result = co_await mira::co::when_any<false>(
      value_after(0ms, 31), value_after(10ms, 32));
  index = result.index;
  value = std::get<1>(result.value);
}

task<void> await_any_move_only(std::unique_ptr<int> &value) {
  auto result =
      co_await mira::co::when_any(move_only_value(), value_after(10ms, 7));
  CHECK(result.index == 0);
  value = std::move(std::get<1>(result.value));
}

task<void> await_any_exception(bool &caught) {
  try {
    (void)co_await mira::co::when_any(fail_immediately(),
                                      value_after(10ms, 7));
  } catch (const std::runtime_error &error) {
    caught = std::string_view(error.what()) == "selected child failure";
  }
}

int selected_int(const auto &result) {
  switch (result.index) {
  case 0:
    return std::get<1>(result.value);
  case 1:
    return std::get<2>(result.value);
  case 2:
    return std::get<3>(result.value);
  default:
    throw std::logic_error("unexpected when_some result index");
  }
}

task<void> await_some(std::vector<std::size_t> &indices,
                      std::vector<int> &values) {
  auto results = co_await mira::co::when_some(
      2, value_after(0ms, 11), value_after(5ms, 22),
      value_after(20ms, 33));

  for (const auto &result : results) {
    indices.push_back(result.index);
    values.push_back(selected_int(result));
  }
}

task<void> await_some_unsafe(std::vector<std::size_t> &indices) {
  auto results = co_await mira::co::when_some<false>(
      2, value_after(0ms, 1), value_after(5ms, 2),
      value_after(20ms, 3));
  for (const auto &result : results)
    indices.push_back(result.index);
}

task<void> await_some_exception(bool &caught) {
  try {
    (void)co_await mira::co::when_some(
        2, fail_immediately(), value_after(0ms, 2),
        value_after(20ms, 3));
  } catch (const std::runtime_error &error) {
    caught = std::string_view(error.what()) == "selected child failure";
  }
}

task<void> await_some_void_and_reference(bool &void_completed, int &value,
                                         bool &same_address) {
  auto results = co_await mira::co::when_some(
      2, finish_void(void_completed), return_reference(value),
      value_after(20ms, 3));

  CHECK(results.size() == 2);
  CHECK(results[0].index == 0);
  CHECK(results[1].index == 1);
  (void)std::get<1>(results[0].value);
  same_address = &std::get<2>(results[1].value).get() == &value;
}

task<void> await_invalid_some(bool &zero_caught, bool &large_caught) {
  try {
    (void)co_await mira::co::when_some(
        0, value_after(0ms, 1), value_after(0ms, 2));
  } catch (const std::invalid_argument &) {
    zero_caught = true;
  }

  try {
    (void)co_await mira::co::when_some(
        3, value_after(0ms, 1), value_after(0ms, 2));
  } catch (const std::invalid_argument &) {
    large_caught = true;
  }
}

task<int> value_on(scheduler &target, std::chrono::milliseconds delay,
                   int value, std::atomic<bool> &finished) {
  co_await mira::co::resume_on(target);
  if (delay != 0ms)
    (void)co_await mira::co::io::sleep_for(delay);
  finished.store(true, std::memory_order_release);
  co_return value;
}

task<void> await_cross_any(scheduler &fast_target, scheduler &slow_target,
                           std::atomic<bool> &fast_finished,
                           std::atomic<bool> &slow_finished,
                           std::atomic<bool> &parent_finished,
                           std::atomic<bool> &failed) {
  const std::thread::id origin_thread = std::this_thread::get_id();
  auto result = co_await mira::co::when_any(
      value_on(fast_target, 0ms, 41, fast_finished),
      value_on(slow_target, 20ms, 42, slow_finished));

  if (std::this_thread::get_id() != origin_thread || result.index != 0 ||
      std::get<1>(result.value) != 41 ||
      slow_finished.load(std::memory_order_acquire))
    failed.store(true, std::memory_order_release);

  parent_finished.store(true, std::memory_order_release);
}

task<void> await_cross_some(scheduler &first_target, scheduler &second_target,
                            std::atomic<bool> &first_finished,
                            std::atomic<bool> &second_finished,
                            std::atomic<bool> &slow_finished,
                            std::atomic<bool> &parent_finished,
                            std::atomic<bool> &failed) {
  const std::thread::id origin_thread = std::this_thread::get_id();
  auto results = co_await mira::co::when_some(
      2, value_on(first_target, 0ms, 51, first_finished),
      value_on(second_target, 2ms, 52, second_finished),
      value_on(second_target, 20ms, 53, slow_finished));

  if (std::this_thread::get_id() != origin_thread || results.size() != 2 ||
      results[0].index != 0 || selected_int(results[0]) != 51 ||
      results[1].index != 1 || selected_int(results[1]) != 52 ||
      slow_finished.load(std::memory_order_acquire))
    failed.store(true, std::memory_order_release);

  parent_finished.store(true, std::memory_order_release);
}

task<void> stop_after_loser(scheduler &origin, scheduler &first_target,
                            scheduler &second_target,
                            const std::atomic<bool> &parent_finished,
                            const std::atomic<bool> &loser_finished,
                            std::atomic<bool> &timed_out) {
  unsigned attempts = 0;
  while ((!parent_finished.load(std::memory_order_acquire) ||
          !loser_finished.load(std::memory_order_acquire)) &&
         attempts++ != 20'000)
    (void)co_await mira::co::io::sleep_for(100us);

  if (!parent_finished.load(std::memory_order_acquire) ||
      !loser_finished.load(std::memory_order_acquire))
    timed_out.store(true, std::memory_order_release);

  origin.request_stop();
  first_target.request_stop();
  second_target.request_stop();
}

void test_any_first_result() {
  std::size_t index = static_cast<std::size_t>(-1);
  int value = 0;
  run_scheduler(await_any(index, value));
  CHECK(index == 0);
  CHECK(value == 11);
}

void test_any_unsafe() {
  std::size_t index = static_cast<std::size_t>(-1);
  int value = 0;
  run_scheduler(await_any_unsafe(index, value));
  CHECK(index == 0);
  CHECK(value == 31);
}

void test_any_move_only() {
  std::unique_ptr<int> value;
  run_scheduler(await_any_move_only(value));
  CHECK(value);
  CHECK(*value == 42);
}

void test_any_exception() {
  bool caught = false;
  run_scheduler(await_any_exception(caught));
  CHECK(caught);
}

void test_some_first_results() {
  std::vector<std::size_t> indices;
  std::vector<int> values;
  run_scheduler(await_some(indices, values));
  CHECK(indices == std::vector<std::size_t>({0, 1}));
  CHECK(values == std::vector<int>({11, 22}));
}

void test_some_unsafe() {
  std::vector<std::size_t> indices;
  run_scheduler(await_some_unsafe(indices));
  CHECK(indices == std::vector<std::size_t>({0, 1}));
}

void test_some_exception() {
  bool caught = false;
  run_scheduler(await_some_exception(caught));
  CHECK(caught);
}

void test_some_void_and_reference() {
  bool void_completed = false;
  bool same_address = false;
  int value = 7;
  run_scheduler(
      await_some_void_and_reference(void_completed, value, same_address));
  CHECK(void_completed);
  CHECK(same_address);
}

void test_some_rejects_invalid_count() {
  bool zero_caught = false;
  bool large_caught = false;
  run_scheduler(await_invalid_some(zero_caught, large_caught));
  CHECK(zero_caught);
  CHECK(large_caught);
}

void test_cross_scheduler_any() {
  scheduler origin;
  scheduler fast_target;
  scheduler slow_target;
  scheduler coordinator;
  std::atomic<bool> fast_finished = false;
  std::atomic<bool> slow_finished = false;
  std::atomic<bool> parent_finished = false;
  std::atomic<bool> failed = false;
  std::atomic<bool> timed_out = false;

  fast_target.start_remote();
  slow_target.start_remote();
  origin.co_spawn(await_cross_any(fast_target, slow_target, fast_finished,
                                  slow_finished, parent_finished, failed));
  origin.start_remote();

  coordinator.co_spawn(stop_after_loser(
      origin, fast_target, slow_target, parent_finished, slow_finished,
      timed_out));
  coordinator.start();
  coordinator.join();
  origin.join();
  fast_target.join();
  slow_target.join();

  CHECK(fast_finished.load(std::memory_order_acquire));
  CHECK(slow_finished.load(std::memory_order_acquire));
  CHECK(parent_finished.load(std::memory_order_acquire));
  CHECK(!failed.load(std::memory_order_acquire));
  CHECK(!timed_out.load(std::memory_order_acquire));
}

void test_cross_scheduler_some() {
  scheduler origin;
  scheduler first_target;
  scheduler second_target;
  scheduler coordinator;
  std::atomic<bool> first_finished = false;
  std::atomic<bool> second_finished = false;
  std::atomic<bool> slow_finished = false;
  std::atomic<bool> parent_finished = false;
  std::atomic<bool> failed = false;
  std::atomic<bool> timed_out = false;

  first_target.start_remote();
  second_target.start_remote();
  origin.co_spawn(await_cross_some(
      first_target, second_target, first_finished, second_finished,
      slow_finished, parent_finished, failed));
  origin.start_remote();

  coordinator.co_spawn(stop_after_loser(
      origin, first_target, second_target, parent_finished, slow_finished,
      timed_out));
  coordinator.start();
  coordinator.join();
  origin.join();
  first_target.join();
  second_target.join();

  CHECK(first_finished.load(std::memory_order_acquire));
  CHECK(second_finished.load(std::memory_order_acquire));
  CHECK(slow_finished.load(std::memory_order_acquire));
  CHECK(parent_finished.load(std::memory_order_acquire));
  CHECK(!failed.load(std::memory_order_acquire));
  CHECK(!timed_out.load(std::memory_order_acquire));
}

} // namespace

int main() {
  run_test("when_any returns first result", test_any_first_result);
  run_test("when_any unsafe mode", test_any_unsafe);
  run_test("when_any supports move-only result", test_any_move_only);
  run_test("when_any propagates selected exception", test_any_exception);
  run_test("when_some returns first results", test_some_first_results);
  run_test("when_some unsafe mode", test_some_unsafe);
  run_test("when_some propagates selected exception", test_some_exception);
  run_test("when_some supports void and reference results",
           test_some_void_and_reference);
  run_test("when_some rejects invalid count", test_some_rejects_invalid_count);
  run_test("cross-scheduler when_any keeps loser alive",
           test_cross_scheduler_any);
  run_test("cross-scheduler when_some keeps loser alive",
           test_cross_scheduler_some);
  return failed_tests == 0 ? 0 : 1;
}
