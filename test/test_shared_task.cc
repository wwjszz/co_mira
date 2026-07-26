#include "co_mira.hpp"

#include <atomic>
#include <chrono>
#include <concepts>
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
#include <vector>

namespace {

using namespace std::chrono_literals;
using mira::co::scheduler;
using mira::co::shared_task;
using mira::co::task;

static_assert(std::copy_constructible<shared_task<int>>);
static_assert(std::copyable<shared_task<int>>);
static_assert(std::movable<shared_task<int>>);

class test_failure : public std::runtime_error {
public:
  test_failure(const char *expression, const char *file, int line)
      : std::runtime_error(std::string(file) + ':' + std::to_string(line) +
                           ": check failed: " + expression) {}
};

#define CHECK(expression)                                                       \
  do {                                                                          \
    if (!(expression))                                                          \
      throw test_failure(#expression, __FILE__, __LINE__);                      \
  } while (false)

int failed_tests = 0;

template <typename Function>
void run_test(std::string_view name, Function &&function) {
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

shared_task<int> lazy_value(int &runs) {
  ++runs;
  co_await mira::co::yield();
  co_return 42;
}

task<void> observe_value(shared_task<int> value,
                         std::vector<const int *> &addresses,
                         std::vector<int> &results) {
  int &result = co_await value;
  addresses.push_back(std::addressof(result));
  results.push_back(result);
}

void test_lazy_single_execution_and_shared_result() {
  int runs = 0;
  shared_task<int> value = lazy_value(runs);
  CHECK(value);
  CHECK(!value.is_ready());
  CHECK(runs == 0);

  std::vector<const int *> addresses;
  std::vector<int> results;
  scheduler sche;
  for (int index = 0; index < 16; ++index)
    sche.co_spawn(observe_value(value, addresses, results));
  sche.start();
  sche.join();

  CHECK(runs == 1);
  CHECK(value.is_ready());
  CHECK(addresses.size() == 16);
  CHECK(results.size() == 16);
  for (std::size_t index = 0; index < addresses.size(); ++index) {
    CHECK(addresses[index] == addresses.front());
    CHECK(results[index] == 42);
  }
}

task<void> observe_ready_twice(shared_task<int> value, int &sum) {
  int &first = co_await value;
  int &second = co_await value;
  CHECK(std::addressof(first) == std::addressof(second));
  sum = first + second;
}

void test_repeated_await_after_completion() {
  int runs = 0;
  shared_task<int> value = lazy_value(runs);
  int first_sum = 0;
  run_scheduler(observe_ready_twice(value, first_sum));
  CHECK(first_sum == 84);
  CHECK(runs == 1);

  int second_sum = 0;
  run_scheduler(observe_ready_twice(value, second_sum));
  CHECK(second_sum == 84);
  CHECK(runs == 1);
}

struct shared_error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

shared_task<int> failing_value(int &runs) {
  ++runs;
  co_await mira::co::yield();
  throw shared_error("shared failure");
  co_return 0;
}

task<void> observe_failure(shared_task<int> value, int &caught) {
  try {
    (void)co_await value;
  } catch (const shared_error &) {
    ++caught;
  }
}

task<void> observe_failure_ready(shared_task<int> value, bool &ready,
                                 bool &caught) {
  co_await value.when_ready();
  ready = true;
  try {
    (void)co_await value;
  } catch (const shared_error &) {
    caught = true;
  }
}

void test_exception_is_cached_for_all_consumers() {
  int runs = 0;
  int caught = 0;
  bool ready = false;
  bool caught_after_ready = false;
  shared_task<int> value = failing_value(runs);

  scheduler sche;
  for (int index = 0; index < 8; ++index)
    sche.co_spawn(observe_failure(value, caught));
  sche.co_spawn(observe_failure_ready(value, ready, caught_after_ready));
  sche.start();
  sche.join();

  CHECK(runs == 1);
  CHECK(caught == 8);
  CHECK(ready);
  CHECK(caught_after_ready);
}

shared_task<void> shared_void(int &runs) {
  ++runs;
  co_await mira::co::yield();
}

shared_task<int &> shared_reference(int &value) { co_return value; }

task<void> observe_void_and_reference(shared_task<void> ready,
                                      shared_task<int &> reference,
                                      int &completed) {
  co_await ready;
  int &value = co_await reference;
  ++value;
  ++completed;
}

void test_void_and_reference_results() {
  int runs = 0;
  int value = 10;
  int completed = 0;
  shared_task<void> ready = shared_void(runs);
  shared_task<int &> reference = shared_reference(value);

  scheduler sche;
  for (int index = 0; index < 5; ++index)
    sche.co_spawn(observe_void_and_reference(ready, reference, completed));
  sche.start();
  sche.join();

  CHECK(runs == 1);
  CHECK(completed == 5);
  CHECK(value == 15);
}

shared_task<std::unique_ptr<int>> shared_move_only() {
  co_return std::make_unique<int>(73);
}

task<void> observe_move_only(shared_task<std::unique_ptr<int>> value,
                             const std::unique_ptr<int> *&address,
                             int &result) {
  std::unique_ptr<int> &item = co_await value;
  address = std::addressof(item);
  CHECK(item != nullptr);
  result = *item;
}

void test_move_only_result_is_shared_by_reference() {
  shared_task<std::unique_ptr<int>> value = shared_move_only();
  const std::unique_ptr<int> *first_address = nullptr;
  const std::unique_ptr<int> *second_address = nullptr;
  int first = 0;
  int second = 0;

  scheduler sche;
  sche.co_spawn(observe_move_only(value, first_address, first));
  sche.co_spawn(observe_move_only(value, second_address, second));
  sche.start();
  sche.join();

  CHECK(first_address == second_address);
  CHECK(first == 73);
  CHECK(second == 73);
}

struct lifetime_value {
  explicit lifetime_value(int &alive) noexcept : alive_(&alive) { ++*alive_; }
  lifetime_value(lifetime_value &&other) noexcept
      : alive_(std::exchange(other.alive_, nullptr)) {}
  lifetime_value(const lifetime_value &) = delete;
  lifetime_value &operator=(const lifetime_value &) = delete;
  lifetime_value &operator=(lifetime_value &&) = delete;
  ~lifetime_value() {
    if (this->alive_)
      --*this->alive_;
  }

private:
  int *alive_;
};

shared_task<lifetime_value> make_lifetime_value(int &alive) {
  co_return lifetime_value{alive};
}

task<void> wait_lifetime_value(shared_task<lifetime_value> value) {
  (void)co_await value;
}

void test_frame_lifetime_follows_shared_owners() {
  int alive = 0;
  shared_task<lifetime_value> value = make_lifetime_value(alive);
  shared_task<lifetime_value> copy = value;
  CHECK(alive == 0);

  run_scheduler(wait_lifetime_value(copy));
  CHECK(alive == 1);

  copy.reset();
  CHECK(alive == 1);
  value.reset();
  CHECK(alive == 0);
}

shared_task<void> hold_frame_parameter(lifetime_value value) {
  (void)value;
  co_return;
}

void test_unstarted_frame_is_released_by_last_owner() {
  int alive = 0;
  shared_task<void> value = hold_frame_parameter(lifetime_value{alive});
  CHECK(alive == 1);
  value.reset();
  CHECK(alive == 0);
}

shared_task<lifetime_value> delayed_lifetime_value(int &alive) {
  co_await mira::co::yield();
  co_return lifetime_value{alive};
}

task<void> consume_without_public_owner(shared_task<lifetime_value> value,
                                        bool &completed) {
  auto operation = value.operator co_await();
  value.reset();
  (void)co_await operation;
  completed = true;
}

void test_awaiter_and_execution_keep_frame_alive() {
  int alive = 0;
  bool completed = false;
  shared_task<lifetime_value> value = delayed_lifetime_value(alive);
  task<void> consumer = consume_without_public_owner(value, completed);
  value.reset();

  run_scheduler(std::move(consumer));
  CHECK(completed);
  CHECK(alive == 0);
}

task<int> ordinary_value(int &runs) {
  ++runs;
  co_await mira::co::yield();
  co_return 91;
}

void test_make_shared_task_from_task() {
  int runs = 0;
  shared_task<int> value =
      mira::co::make_shared_task(ordinary_value(runs));
  std::vector<const int *> addresses;
  std::vector<int> results;

  scheduler sche;
  sche.co_spawn(observe_value(value, addresses, results));
  sche.co_spawn(observe_value(value, addresses, results));
  sche.start();
  sche.join();

  CHECK(runs == 1);
  CHECK(results.size() == 2);
  CHECK(results[0] == 91);
  CHECK(results[1] == 91);
}

task<void> await_empty(bool &caught) {
  shared_task<int> empty;
  CHECK(empty.is_ready());
  CHECK(!empty);
  try {
    (void)co_await empty;
  } catch (const std::logic_error &) {
    caught = true;
  }
}

void test_empty_shared_task_reports_error() {
  bool caught = false;
  run_scheduler(await_empty(caught));
  CHECK(caught);
}

shared_task<int> delayed_cross_scheduler_value(std::atomic<int> &runs) {
  runs.fetch_add(1, std::memory_order_relaxed);
  (void)co_await mira::co::io::sleep_for(20ms);
  co_return 117;
}

task<void> cross_scheduler_observer(shared_task<int> value, scheduler &own,
                                    scheduler &other,
                                    std::atomic<unsigned> &completed,
                                    std::atomic<bool> &bad_result) {
  int &result = co_await value;
  if (result != 117)
    bad_result.store(true, std::memory_order_relaxed);

  if (completed.fetch_add(1, std::memory_order_acq_rel) == 1) {
    own.request_stop();
    other.request_stop();
  }
}

void test_cross_scheduler_waiters() {
  std::atomic<int> runs = 0;
  std::atomic<unsigned> completed = 0;
  std::atomic<bool> bad_result = false;
  shared_task<int> value = delayed_cross_scheduler_value(runs);
  scheduler first;
  scheduler second;

  first.co_spawn(
      cross_scheduler_observer(value, first, second, completed, bad_result));
  second.co_spawn(
      cross_scheduler_observer(value, second, first, completed, bad_result));
  first.start_remote();
  second.start_remote();
  first.join();
  second.join();

  CHECK(runs.load(std::memory_order_relaxed) == 1);
  CHECK(completed.load(std::memory_order_acquire) == 2);
  CHECK(!bad_result.load(std::memory_order_relaxed));
}

} // namespace

int main() {
  run_test("lazy single execution and shared result",
           test_lazy_single_execution_and_shared_result);
  run_test("repeated await after completion",
           test_repeated_await_after_completion);
  run_test("exception is cached for all consumers",
           test_exception_is_cached_for_all_consumers);
  run_test("void and reference results", test_void_and_reference_results);
  run_test("move-only result is shared by reference",
           test_move_only_result_is_shared_by_reference);
  run_test("frame lifetime follows shared owners",
           test_frame_lifetime_follows_shared_owners);
  run_test("unstarted frame is released by last owner",
           test_unstarted_frame_is_released_by_last_owner);
  run_test("awaiter and execution keep frame alive",
           test_awaiter_and_execution_keep_frame_alive);
  run_test("make_shared_task from task", test_make_shared_task_from_task);
  run_test("empty shared_task reports error",
           test_empty_shared_task_reports_error);
  run_test("cross-scheduler waiters", test_cross_scheduler_waiters);

  if (failed_tests != 0) {
    std::cerr << failed_tests << " test(s) failed\n";
    return 1;
  }

  std::cout << "All shared_task tests passed\n";
  return 0;
}
