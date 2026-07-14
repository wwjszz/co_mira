#include "task.hpp"

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using mira::co::co_handle;
using mira::co::task;

static_assert(std::movable<task<int>>);
static_assert(!std::copy_constructible<task<int>>);
static_assert(!std::copyable<task<int>>);

class test_failure : public std::runtime_error {
public:
  test_failure(const char *expression, const char *file, int line)
      : std::runtime_error(make_message(expression, file, line)) {}

private:
  static std::string make_message(const char *expression, const char *file, int line) {
    return std::string(file) + ':' + std::to_string(line) + ": check failed: " + expression;
  }
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

template <typename Exception, typename Function> void check_throws(Function &&function) {
  bool caught = false;
  try {
    std::forward<Function>(function)();
  } catch (const Exception &) {
    caught = true;
  }
  CHECK(caught);
}

class manual_loop {
public:
  struct reschedule_awaiter {
    manual_loop *loop;

    static constexpr bool await_ready() noexcept { return false; }
    void await_suspend(co_handle<> handle) const { this->loop->schedule(handle); }
    static constexpr void await_resume() noexcept {}
  };

  void schedule(co_handle<> handle) {
    CHECK(handle);
    CHECK(!handle.done());
    this->ready_.push_back(handle);
  }

  [[nodiscard]] reschedule_awaiter reschedule() noexcept { return {this}; }

  bool run_one() {
    if (this->ready_.empty())
      return false;

    auto handle = this->ready_.front();
    this->ready_.pop_front();
    CHECK(handle);
    CHECK(!handle.done());
    handle.resume();
    return true;
  }

  void run() {
    while (this->run_one()) {
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return this->ready_.size(); }
  [[nodiscard]] bool empty() const noexcept { return this->ready_.empty(); }
  void clear() noexcept { this->ready_.clear(); }

private:
  std::deque<co_handle<>> ready_;
};

template <typename T> void complete(manual_loop &loop, task<T> &item) {
  CHECK(item.get_handle());
  CHECK(!item.is_ready());
  loop.schedule(item.get_handle());
  loop.run();
  CHECK(item.is_ready());
}

template <typename T> decltype(auto) result_of(task<T> &item) {
  CHECK(item.get_handle());
  CHECK(item.is_ready());
  return item.get_handle().promise().result();
}

struct task_error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct construction_error : std::runtime_error {
  construction_error() : std::runtime_error("result construction failed") {}
};

struct lifetime_probe {
  explicit lifetime_probe(int &alive) noexcept : alive_(&alive) { ++*this->alive_; }

  lifetime_probe(lifetime_probe &&other) noexcept : alive_(std::exchange(other.alive_, nullptr)) {}

  lifetime_probe &operator=(lifetime_probe &&) = delete;
  lifetime_probe(const lifetime_probe &) = delete;
  lifetime_probe &operator=(const lifetime_probe &) = delete;

  ~lifetime_probe() {
    if (this->alive_)
      --*this->alive_;
  }

private:
  int *alive_;
};

struct throws_when_moved {
  throws_when_moved() = default;
  throws_when_moved(const throws_when_moved &) = delete;
  throws_when_moved &operator=(const throws_when_moved &) = delete;
  throws_when_moved(throws_when_moved &&) { throw construction_error{}; }
};

task<int> lazy_integer(bool &entered, int value) {
  entered = true;
  co_return value;
}

task<int> integer_value(int value) { co_return value; }

task<std::string> converted_value() { co_return "converted"; }

task<void> void_value(bool &completed) {
  completed = true;
  co_return;
}

task<int &> mutable_reference(int &value) { co_return value; }

task<const int &> const_reference(const int &value) { co_return value; }

task<std::unique_ptr<int>> move_only_value(int value) { co_return std::make_unique<int>(value); }

task<int> ordered_child(std::vector<int> &trace) {
  trace.push_back(2);
  co_return 7;
}

task<void> ordered_parent(std::vector<int> &trace, int &result) {
  trace.push_back(1);
  result = co_await ordered_child(trace);
  trace.push_back(3);
}

task<void> await_void_and_reference_children(bool &void_completed, int &value,
                                             int *&reference_address) {
  co_await void_value(void_completed);
  int &reference = co_await mutable_reference(value);
  if (std::same_as<int &, decltype(reference)>) {
    std::println(std::cout, "reference is int&!!!");
  }
  reference_address = std::addressof(reference);
  reference = 29;
  // auto &&reference = co_await mutable_reference(value);
  // reference_address = std::addressof(reference);
  // reference = 29;
}

task<int> suspended_value(manual_loop &loop, int &stage) {
  stage = 1;
  co_await loop.reschedule();
  stage = 2;
  co_return 19;
}

task<int> suspended_child(manual_loop &loop, std::vector<int> &trace) {
  trace.push_back(2);
  co_await loop.reschedule();
  trace.push_back(3);
  co_return 23;
}

task<void> parent_of_suspended_child(manual_loop &loop, std::vector<int> &trace, int &result) {
  trace.push_back(1);
  result = co_await suspended_child(loop, trace);
  trace.push_back(4);
}

task<void> consume_move_only_value(int &result) {
  auto value = co_await move_only_value(31);
  CHECK(value != nullptr);
  result = *value;
}

task<void> observe_lvalue_and_const_results(int &observed) {
  auto child = integer_value(5);
  int &mutable_result = co_await child;
  if (std::same_as<int &, decltype(mutable_result)>) {
    std::println(std::cout, "mutable_result is int&!!!");
  }
  mutable_result = 8;

  const auto &const_child = child;
  const int &const_result = co_await const_child;
  observed = const_result;
}

task<int> fail_with_value_exception() {
  throw task_error("task<int> failed");
  co_return 0;
}

task<void> fail_with_void_exception() {
  throw task_error("task<void> failed");
  co_return;
}

task<int &> fail_with_reference_exception(int &value) {
  throw task_error("task<int&> failed");
  co_return value;
}

task<int> fail_with_non_std_exception() {
  throw 73;
  co_return 0;
}

task<int> fail_after_reschedule(manual_loop &loop, bool &reached_suspend) {
  reached_suspend = true;
  co_await loop.reschedule();
  throw task_error("failed after suspension");
  co_return 0;
}

task<throws_when_moved> fail_while_storing_result() { co_return throws_when_moved{}; }

task<void> catch_child_exception(bool &caught, bool &continued) {
  try {
    (void)co_await fail_with_value_exception();
  } catch (const task_error &) {
    caught = true;
  }
  continued = true;
}

task<void> catch_void_child_exception(bool &caught) {
  try {
    co_await fail_with_void_exception();
  } catch (const task_error &) {
    caught = true;
  }
}

task<int> propagate_exception_one_level() { co_return co_await fail_with_value_exception(); }

task<void> propagate_exception_two_levels() { (void)co_await propagate_exception_one_level(); }

task<int> recursive_sum(int depth) {
  if (depth == 0)
    co_return 0;
  co_return depth + co_await recursive_sum(depth - 1);
}

task<int> recursive_failure(int depth) {
  if (depth == 0)
    throw task_error("deep failure");
  co_return co_await recursive_failure(depth - 1);
}

task<void> inspect_successful_when_ready(bool &ready_after_wait, int &result) {
  auto child = integer_value(41);
  co_await child.when_ready();
  ready_after_wait = child.is_ready();
  result = co_await child;
}

task<void> inspect_failing_when_ready(bool &ready_after_wait, bool &threw_on_result) {
  auto child = fail_with_value_exception();
  co_await child.when_ready();
  ready_after_wait = child.is_ready();

  try {
    (void)co_await child;
  } catch (const task_error &) {
    threw_on_result = true;
  }
}

task<void> guarded_suspension(manual_loop &loop, int &alive) {
  lifetime_probe probe{alive};
  co_await loop.reschedule();
}

task<void> own_probe_parameter(std::unique_ptr<lifetime_probe> probe) {
  CHECK(probe != nullptr);
  co_return;
}

void test_lazy_start_and_basic_results() {
  manual_loop loop;
  bool entered = false;
  auto integer = lazy_integer(entered, 17);

  CHECK(!entered);
  CHECK(!integer.is_ready());
  complete(loop, integer);
  CHECK(entered);
  CHECK(result_of(integer) == 17);

  bool completed = false;
  auto empty = void_value(completed);
  CHECK(!completed);
  complete(loop, empty);
  CHECK(completed);
  result_of(empty);

  auto converted = converted_value();
  complete(loop, converted);
  CHECK(result_of(converted) == "converted");
}

void test_reference_results() {
  manual_loop loop;
  int value = 10;
  auto reference = mutable_reference(value);
  complete(loop, reference);

  int &result = result_of(reference);
  CHECK(std::addressof(result) == std::addressof(value));
  result = 22;
  CHECK(value == 22);

  const int const_value = 37;
  auto const_result_task = const_reference(const_value);
  complete(loop, const_result_task);
  const int &const_result = result_of(const_result_task);
  CHECK(std::addressof(const_result) == std::addressof(const_value));
}

void test_parent_child_order_and_symmetric_transfer() {
  manual_loop loop;
  std::vector<int> trace;
  int result = 0;
  auto parent = ordered_parent(trace, result);

  complete(loop, parent);
  result_of(parent);
  CHECK(trace == std::vector<int>({1, 2, 3}));
  CHECK(result == 7);

  bool void_completed = false;
  int value = 3;
  int *reference_address = nullptr;
  auto mixed_parent = await_void_and_reference_children(void_completed, value, reference_address);
  complete(loop, mixed_parent);
  result_of(mixed_parent);
  CHECK(void_completed);
  CHECK(reference_address == std::addressof(value));
  CHECK(value == 29);
}

void test_direct_suspension_and_resumption() {
  manual_loop loop;
  int stage = 0;
  auto item = suspended_value(loop, stage);

  loop.schedule(item.get_handle());
  CHECK(loop.run_one());
  CHECK(stage == 1);
  CHECK(!item.is_ready());
  CHECK(loop.size() == 1);

  CHECK(loop.run_one());
  CHECK(stage == 2);
  CHECK(item.is_ready());
  CHECK(loop.empty());
  CHECK(result_of(item) == 19);
}

void test_suspended_child_resumes_parent() {
  manual_loop loop;
  std::vector<int> trace;
  int result = 0;
  auto parent = parent_of_suspended_child(loop, trace, result);

  loop.schedule(parent.get_handle());
  CHECK(loop.run_one());
  CHECK(trace == std::vector<int>({1, 2}));
  CHECK(!parent.is_ready());
  CHECK(loop.size() == 1);

  CHECK(loop.run_one());
  CHECK(trace == std::vector<int>({1, 2, 3, 4}));
  CHECK(parent.is_ready());
  CHECK(loop.empty());
  CHECK(result == 23);
  result_of(parent);
}

void test_move_only_and_result_categories() {
  manual_loop loop;
  int moved_value = 0;
  auto consumer = consume_move_only_value(moved_value);
  complete(loop, consumer);
  CHECK(moved_value == 31);

  int observed = 0;
  auto observer = observe_lvalue_and_const_results(observed);
  complete(loop, observer);
  CHECK(observed == 8);
}

void test_root_exception_storage() {
  manual_loop loop;

  auto value_failure = fail_with_value_exception();
  complete(loop, value_failure);
  check_throws<task_error>([&] { (void)result_of(value_failure); });

  auto void_failure = fail_with_void_exception();
  complete(loop, void_failure);
  check_throws<task_error>([&] { result_of(void_failure); });

  int value = 0;
  auto reference_failure = fail_with_reference_exception(value);
  complete(loop, reference_failure);
  check_throws<task_error>([&] { (void)result_of(reference_failure); });

  auto non_std_failure = fail_with_non_std_exception();
  complete(loop, non_std_failure);
  bool caught_integer = false;
  try {
    (void)result_of(non_std_failure);
  } catch (int exception_value) {
    caught_integer = exception_value == 73;
  }
  CHECK(caught_integer);
}

void test_exception_after_suspension() {
  manual_loop loop;
  bool reached_suspend = false;
  auto item = fail_after_reschedule(loop, reached_suspend);

  complete(loop, item);
  CHECK(reached_suspend);
  check_throws<task_error>([&] { (void)result_of(item); });
}

void test_nested_exception_propagation() {
  manual_loop loop;
  bool caught = false;
  bool continued = false;
  auto catcher = catch_child_exception(caught, continued);
  complete(loop, catcher);
  result_of(catcher);
  CHECK(caught);
  CHECK(continued);

  bool caught_void = false;
  auto void_catcher = catch_void_child_exception(caught_void);
  complete(loop, void_catcher);
  result_of(void_catcher);
  CHECK(caught_void);

  auto propagated = propagate_exception_two_levels();
  complete(loop, propagated);
  check_throws<task_error>([&] { result_of(propagated); });
}

void test_deep_task_chains() {
  manual_loop loop;
  constexpr int depth = 128;
  auto sum = recursive_sum(depth);
  complete(loop, sum);
  CHECK(result_of(sum) == depth * (depth + 1) / 2);

  auto failure = recursive_failure(depth);
  complete(loop, failure);
  check_throws<task_error>([&] { (void)result_of(failure); });
}

void test_result_construction_exception() {
  manual_loop loop;
  auto item = fail_while_storing_result();
  complete(loop, item);
  check_throws<construction_error>([&] { (void)result_of(item); });
}

void test_when_ready_does_not_consume_result() {
  manual_loop loop;
  bool ready_after_wait = false;
  int result = 0;
  auto success = inspect_successful_when_ready(ready_after_wait, result);
  complete(loop, success);
  result_of(success);
  CHECK(ready_after_wait);
  CHECK(result == 41);

  ready_after_wait = false;
  bool threw_on_result = false;
  auto failure = inspect_failing_when_ready(ready_after_wait, threw_on_result);
  complete(loop, failure);
  result_of(failure);
  CHECK(ready_after_wait);
  CHECK(threw_on_result);
}

void test_move_construction_and_assignment() {
  manual_loop loop;
  auto source = integer_value(9);
  auto moved = std::move(source);
  CHECK(!source.get_handle());
  CHECK(source.is_ready());
  complete(loop, moved);
  CHECK(result_of(moved) == 9);

  auto destination = integer_value(1);
  auto replacement = integer_value(2);
  destination = std::move(replacement);
  complete(loop, destination);
  CHECK(result_of(destination) == 2);

  // Move assignment uses swap semantics, so the moved-from object remains
  // valid and may own the destination's previous coroutine.
  if (replacement.get_handle()) {
    complete(loop, replacement);
    CHECK(result_of(replacement) == 1);
  }
}

void test_destruction_of_unstarted_and_suspended_tasks() {
  int unstarted_alive = 0;
  {
    auto probe = std::make_unique<lifetime_probe>(unstarted_alive);
    auto item = own_probe_parameter(std::move(probe));
    CHECK(!probe);
    CHECK(unstarted_alive == 1);
    CHECK(!item.is_ready());
  }
  CHECK(unstarted_alive == 0);

  int suspended_alive = 0;
  manual_loop loop;
  {
    auto item = guarded_suspension(loop, suspended_alive);
    loop.schedule(item.get_handle());
    CHECK(loop.run_one());
    CHECK(suspended_alive == 1);
    CHECK(!item.is_ready());

    // Remove the synthetic wake-up before destroying the suspended frame.
    loop.clear();
  }
  CHECK(suspended_alive == 0);
}

} // namespace

int main() {
  run_test("lazy start and basic results", test_lazy_start_and_basic_results);
  run_test("reference results", test_reference_results);
  run_test("parent-child order and symmetric transfer",
           test_parent_child_order_and_symmetric_transfer);
  run_test("direct suspension and resumption", test_direct_suspension_and_resumption);
  run_test("suspended child resumes parent", test_suspended_child_resumes_parent);
  run_test("move-only values and result categories", test_move_only_and_result_categories);
  run_test("root exception storage", test_root_exception_storage);
  run_test("exception after suspension", test_exception_after_suspension);
  run_test("nested exception propagation", test_nested_exception_propagation);
  run_test("deep task chains", test_deep_task_chains);
  run_test("result construction exception", test_result_construction_exception);
  run_test("when_ready does not consume result", test_when_ready_does_not_consume_result);
  run_test("move construction and assignment", test_move_construction_and_assignment);
  run_test("destruction of unstarted and suspended tasks",
           test_destruction_of_unstarted_and_suspended_tasks);

  if (failed_tests != 0) {
    std::cerr << failed_tests << " test(s) failed\n";
    return 1;
  }

  std::cout << "All task tests passed\n";
  return 0;
}
