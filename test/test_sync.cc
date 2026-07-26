#include "co_mira.hpp"

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using mira::co::condition_variable;
using mira::co::counting_semaphore;
using mira::co::mutex;
using mira::co::scheduler;
using mira::co::task;

static_assert(!std::copy_constructible<mira::co::mutex>);
static_assert(!std::move_constructible<mira::co::mutex>);
static_assert(!std::copy_constructible<mira::co::counting_semaphore>);
static_assert(!std::copy_constructible<mira::co::condition_variable>);
static_assert(!std::copy_constructible<mira::co::unique_lock>);
static_assert(std::move_constructible<mira::co::unique_lock>);

class test_failure : public std::runtime_error {
public:
  test_failure(const char *expression, const char *file, int line)
      : std::runtime_error(std::string(file) + ':' + std::to_string(line) + ": check failed: " + expression) {}
};

#define CHECK(expression)                                                                                                                            \
  do {                                                                                                                                               \
    if (!(expression))                                                                                                                               \
      throw test_failure(#expression, __FILE__, __LINE__);                                                                                           \
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

task<void> check_immediate_semaphore(counting_semaphore &semaphore, bool &completed) {
  co_await semaphore.acquire();
  CHECK(semaphore.try_acquire());
  CHECK(!semaphore.try_acquire());
  semaphore.release();
  CHECK(semaphore.try_acquire());
  completed = true;
}

void test_semaphore_immediate_and_try_acquire() {
  counting_semaphore semaphore{2};
  bool completed = false;
  run_scheduler(check_immediate_semaphore(semaphore, completed));
  CHECK(completed);
}

task<void> wait_for_permit(counting_semaphore &semaphore, int id, std::vector<int> &order) {
  co_await semaphore.acquire();
  order.push_back(id);
}

task<void> release_permits(counting_semaphore &semaphore, std::ptrdiff_t count) {
  co_await mira::co::yield();
  semaphore.release(count);
}

void test_semaphore_fifo_batch_release() {
  counting_semaphore semaphore{0};
  std::vector<int> order;
  scheduler sche;
  for (int id = 0; id < 8; ++id)
    sche.co_spawn(wait_for_permit(semaphore, id, order));
  sche.co_spawn(release_permits(semaphore, 8));
  sche.start();
  sche.join();

  CHECK(order.size() == 8);
  for (int id = 0; id < 8; ++id)
    CHECK(order[static_cast<std::size_t>(id)] == id);
}

task<void> increment_under_mutex(mutex &target, int &value, int iterations) {
  for (int iteration = 0; iteration < iterations; ++iteration) {
    auto lock = co_await target.lock_guard();
    CHECK(lock.owns_lock());
    CHECK(!target.try_lock());
    const int snapshot = value;
    co_await mira::co::yield();
    value = snapshot + 1;
  }
}

void test_mutex_mutual_exclusion_and_raii() {
  constexpr int task_count = 16;
  constexpr int iterations = 100;

  mutex target;
  int value = 0;
  scheduler sche;
  for (int index = 0; index < task_count; ++index)
    sche.co_spawn(increment_under_mutex(target, value, iterations));
  sche.start();
  sche.join();

  CHECK(value == task_count * iterations);
  CHECK(target.try_lock());
  target.unlock();
}

task<void> exercise_unique_lock(mutex &target, bool &completed) {
  auto lock = co_await target.lock_guard();
  CHECK(lock.owns_lock());
  CHECK(lock.associated_mutex() == &target);

  lock.unlock();
  CHECK(!lock.owns_lock());
  CHECK(target.try_lock());
  target.unlock();

  co_await lock.lock();
  CHECK(lock.owns_lock());
  completed = true;
}

void test_unique_lock_unlock_and_relock() {
  mutex target;
  bool completed = false;
  run_scheduler(exercise_unique_lock(target, completed));
  CHECK(completed);
  CHECK(target.try_lock());
  target.unlock();
}

task<void> wait_for_condition(condition_variable &condition, mutex &target, bool &ready, int &value, int &observed, int &awakened) {
  auto lock = co_await target.lock_guard();
  co_await condition.wait(lock, [&] { return ready; });
  CHECK(lock.owns_lock());
  CHECK(!target.try_lock());
  observed += value;
  ++awakened;
}

task<void> publish_condition(condition_variable &condition, mutex &target, bool &ready, int &value) {
  co_await mira::co::yield();
  {
    auto lock = co_await target.lock_guard();
    value = 7;
    ready = true;
  }
  condition.notify_all();
}

void test_condition_variable_predicate_and_notify_all() {
  constexpr int consumer_count = 6;

  mutex target;
  condition_variable condition;
  bool ready = false;
  int value = 0;
  int observed = 0;
  int awakened = 0;
  scheduler sche;

  for (int index = 0; index < consumer_count; ++index)
    sche.co_spawn(wait_for_condition(condition, target, ready, value, observed, awakened));
  sche.co_spawn(publish_condition(condition, target, ready, value));
  sche.start();
  sche.join();

  CHECK(awakened == consumer_count);
  CHECK(observed == consumer_count * value);
}

task<void> cross_semaphore_waiter(counting_semaphore &semaphore, scheduler &target, std::atomic<bool> &entered,
                                  std::atomic<bool> &completed) {
  entered.store(true, std::memory_order_release);
  co_await semaphore.acquire();
  completed.store(true, std::memory_order_release);
  target.request_stop();
}

task<void> cross_semaphore_releaser(counting_semaphore &semaphore, const std::atomic<bool> &entered) {
  while (!entered.load(std::memory_order_acquire))
    (void)co_await mira::co::io::sleep_for(100us);
  semaphore.release();
}

void test_cross_scheduler_semaphore_wakeup() {
  counting_semaphore semaphore{0};
  scheduler target;
  scheduler source;
  std::atomic<bool> entered = false;
  std::atomic<bool> completed = false;

  target.co_spawn(cross_semaphore_waiter(semaphore, target, entered, completed));
  target.start_remote();
  source.co_spawn(cross_semaphore_releaser(semaphore, entered));
  source.start();
  source.join();
  target.join();

  CHECK(completed.load(std::memory_order_acquire));
}

task<void> cross_mutex_holder(mutex &target, std::atomic<bool> &locked, std::atomic<unsigned> &completed) {
  {
    auto lock = co_await target.lock_guard();
    locked.store(true, std::memory_order_release);
    (void)co_await mira::co::io::sleep_for(5ms);
  }
  completed.fetch_add(1, std::memory_order_release);
}

task<void> cross_mutex_contender(mutex &target, const std::atomic<bool> &locked, std::atomic<bool> &acquired,
                                 std::atomic<unsigned> &completed) {
  while (!locked.load(std::memory_order_acquire))
    (void)co_await mira::co::io::sleep_for(100us);
  auto lock = co_await target.lock_guard();
  acquired.store(true, std::memory_order_release);
  completed.fetch_add(1, std::memory_order_release);
}

task<void> stop_two_when_complete(scheduler &first, scheduler &second, const std::atomic<unsigned> &completed, std::atomic<bool> &timed_out) {
  unsigned attempts = 0;
  while (completed.load(std::memory_order_acquire) != 2 && attempts++ != 20'000)
    (void)co_await mira::co::io::sleep_for(100us);

  if (completed.load(std::memory_order_acquire) != 2)
    timed_out.store(true, std::memory_order_release);

  first.request_stop();
  second.request_stop();
}

void test_cross_scheduler_mutex_handoff() {
  mutex target;
  scheduler first;
  scheduler second;
  scheduler coordinator;
  std::atomic<bool> locked = false;
  std::atomic<bool> acquired = false;
  std::atomic<unsigned> completed = 0;
  std::atomic<bool> timed_out = false;

  first.co_spawn(cross_mutex_holder(target, locked, completed));
  first.start_remote();
  second.co_spawn(cross_mutex_contender(target, locked, acquired, completed));
  second.start_remote();
  coordinator.co_spawn(stop_two_when_complete(first, second, completed, timed_out));
  coordinator.start();
  coordinator.join();
  first.join();
  second.join();

  CHECK(!timed_out.load(std::memory_order_acquire));
  CHECK(acquired.load(std::memory_order_acquire));
}

task<void> cross_cv_consumer(condition_variable &condition, mutex &target, bool &ready, int &value, std::atomic<bool> &entered,
                             std::atomic<bool> &completed, scheduler &scheduler) {
  auto lock = co_await target.lock_guard();
  entered.store(true, std::memory_order_release);
  co_await condition.wait(lock, [&] { return ready; });
  CHECK(value == 42);
  completed.store(true, std::memory_order_release);
  lock.unlock();
  scheduler.request_stop();
}

task<void> cross_cv_producer(condition_variable &condition, mutex &target, bool &ready, int &value, const std::atomic<bool> &entered) {
  while (!entered.load(std::memory_order_acquire))
    (void)co_await mira::co::io::sleep_for(100us);

  {
    auto lock = co_await target.lock_guard();
    value = 42;
    ready = true;
  }
  condition.notify_one();
}

void test_cross_scheduler_condition_variable() {
  mutex target_mutex;
  condition_variable condition;
  scheduler target;
  scheduler source;
  bool ready = false;
  int value = 0;
  std::atomic<bool> entered = false;
  std::atomic<bool> completed = false;

  target.co_spawn(cross_cv_consumer(condition, target_mutex, ready, value, entered, completed, target));
  target.start_remote();
  source.co_spawn(cross_cv_producer(condition, target_mutex, ready, value, entered));
  source.start();
  source.join();
  target.join();

  CHECK(completed.load(std::memory_order_acquire));
}

} // namespace

int main() {
  run_test("semaphore immediate and try_acquire", test_semaphore_immediate_and_try_acquire);
  run_test("semaphore FIFO batch release", test_semaphore_fifo_batch_release);
  run_test("mutex mutual exclusion and RAII", test_mutex_mutual_exclusion_and_raii);
  run_test("unique_lock unlock and relock", test_unique_lock_unlock_and_relock);
  run_test("condition variable predicate and notify_all", test_condition_variable_predicate_and_notify_all);
  run_test("cross-scheduler semaphore wakeup", test_cross_scheduler_semaphore_wakeup);
  run_test("cross-scheduler mutex handoff", test_cross_scheduler_mutex_handoff);
  run_test("cross-scheduler condition variable", test_cross_scheduler_condition_variable);

  if (failed_tests != 0) {
    std::cerr << failed_tests << " test(s) failed\n";
    return 1;
  }

  std::cout << "All synchronization tests passed\n";
  return 0;
}
