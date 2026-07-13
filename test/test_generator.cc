#include "generator.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <iterator>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

template <typename Ref, typename Val = void> using generator = mira::co::generator<Ref, Val>;

using mira::co::elements_of;

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

template <std::ranges::input_range Range>
auto collect(Range &&range) -> std::vector<std::ranges::range_value_t<Range>> {
  std::vector<std::ranges::range_value_t<Range>> result;
  for (auto &&value : range)
    result.emplace_back(static_cast<decltype(value) &&>(value));
  return result;
}

using value_generator = generator<int>;
using value_iterator = decltype(std::declval<value_generator &>().begin());
using reference_generator = generator<int &>;
using const_reference_generator = generator<const int &>;
using converted_reference_generator = generator<std::string_view, std::string>;

static_assert(std::same_as<mira::co::reference_t<int, void>, int &&>);
static_assert(std::same_as<mira::co::reference_t<int &, void>, int &>);
static_assert(std::same_as<mira::co::reference_t<int &&, void>, int &&>);
static_assert(std::same_as<mira::co::reference_t<std::string_view, std::string>, std::string_view>);
static_assert(std::same_as<mira::co::yield_t<int &&>, int &&>);
static_assert(std::same_as<mira::co::yield_t<int &>, int &>);
static_assert(std::same_as<mira::co::yield_t<std::string_view>, const std::string_view &>);

static_assert(std::ranges::view<value_generator>);
static_assert(std::ranges::input_range<value_generator>);
static_assert(!std::ranges::forward_range<value_generator>);
static_assert(std::input_iterator<value_iterator>);
static_assert(std::movable<value_generator>);
static_assert(!std::copy_constructible<value_generator>);
static_assert(std::same_as<std::ranges::range_value_t<value_generator>, int>);
static_assert(std::same_as<std::ranges::range_reference_t<value_generator>, int &&>);
static_assert(std::same_as<std::ranges::range_reference_t<reference_generator>, int &>);
static_assert(std::same_as<std::ranges::range_reference_t<const_reference_generator>, const int &>);
static_assert(std::same_as<std::ranges::range_value_t<converted_reference_generator>, std::string>);
static_assert(
    std::same_as<std::ranges::range_reference_t<converted_reference_generator>, std::string_view>);

using lvalue_elements = decltype(elements_of{std::declval<std::vector<int> &>()});
using rvalue_elements = decltype(elements_of{std::declval<std::vector<int>>()});
static_assert(std::same_as<decltype(std::declval<lvalue_elements>().range), std::vector<int> &>);
static_assert(std::same_as<decltype(std::declval<rvalue_elements>().range), std::vector<int> &&>);

generator<int> empty_sequence(int &entered) {
  ++entered;
  co_return;
}

generator<int> integer_sequence(int &entered) {
  ++entered;
  co_yield 1;
  co_yield 2;
  co_yield 3;
}

generator<std::string> copy_lvalue(std::string &source) { co_yield source; }

generator<std::string> string_sequence() {
  co_yield std::string{"alpha"};
  co_yield std::string{"beta"};
}

generator<int &> mutable_references(std::vector<int> &values) {
  for (int &value : values)
    co_yield value;
}

generator<const int &> const_references(const int &value) {
  co_yield value;
  co_yield 99;
}

converted_reference_generator converted_references() {
  co_yield std::string_view{};
  co_yield std::string_view{"alpha"};
  co_yield std::string_view{"beta"};
}

generator<std::unique_ptr<int>> move_only_values() {
  co_yield std::make_unique<int>(4);
  co_yield std::make_unique<int>(5);
}

generator<int> number_range(int first, int last) {
  while (first != last)
    co_yield first++;
}

generator<int> nested_sequence() {
  co_yield 0;
  co_yield elements_of{number_range(1, 4)};
  co_yield elements_of{number_range(4, 6)};
  co_yield 6;
}

generator<int> recursive_depth(int depth) {
  co_yield depth;
  if (depth != 0)
    co_yield elements_of{recursive_depth(depth - 1)};
  co_yield -depth;
}

generator<int> iota_elements() { co_yield elements_of{std::views::iota(3, 7)}; }

generator<int &> vector_elements(std::vector<int> &values) { co_yield elements_of{values}; }

generator<const int &> const_vector_elements(const std::vector<int> &values) {
  co_yield elements_of{values};
}

generator<int &> temporary_vector_elements() { co_yield elements_of{std::vector<int>{7, 8, 9}}; }

class generator_error : public std::runtime_error {
public:
  explicit generator_error(const char *message) : std::runtime_error(message) {}
};

generator<int> throw_before_first_yield() {
  throw generator_error("before first yield");
  co_return;
}

generator<int> throw_after_first_yield() {
  co_yield 1;
  throw generator_error("after first yield");
}

generator<int> failing_child() {
  co_yield 7;
  throw generator_error("nested child");
}

generator<int> failing_parent(bool &continued) {
  co_yield 6;
  co_yield elements_of{failing_child()};
  continued = true;
  co_yield 8;
}

class range_error : public std::runtime_error {
public:
  explicit range_error(const char *message) : std::runtime_error(message) {}
};

struct throwing_range {
  struct iterator {
    using value_type = int;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::input_iterator_tag;

    throwing_range *owner = nullptr;
    std::size_t index = 0;

    int &operator*() const noexcept { return owner->values[index]; }

    iterator &operator++() {
      if (owner->throw_on_increment)
        throw range_error("range increment");
      ++index;
      return *this;
    }

    void operator++(int) { ++*this; }

    friend bool operator==(const iterator &it, std::default_sentinel_t) noexcept {
      return it.index == 2;
    }
  };

  iterator begin() {
    if (throw_on_begin)
      throw range_error("range begin");
    return {this, 0};
  }

  std::default_sentinel_t end() const noexcept { return {}; }

  int values[2]{10, 20};
  bool throw_on_begin = false;
  bool throw_on_increment = false;
};

static_assert(std::ranges::input_range<throwing_range &>);

generator<int &> throwing_range_elements(throwing_range &range) { co_yield elements_of{range}; }

struct lifetime_probe {
  explicit lifetime_probe(int &alive) : alive_(&alive) { ++*alive_; }
  lifetime_probe(const lifetime_probe &) = delete;
  lifetime_probe &operator=(const lifetime_probe &) = delete;
  ~lifetime_probe() { --*alive_; }

private:
  int *alive_;
};

generator<int> guarded_sequence(int &alive) {
  lifetime_probe probe{alive};
  co_yield 1;
  co_yield 2;
}

generator<int> guarded_child(int &alive) {
  lifetime_probe probe{alive};
  co_yield 11;
  co_yield 12;
}

generator<int> guarded_parent(int &parent_alive, int &child_alive) {
  lifetime_probe probe{parent_alive};
  co_yield elements_of{guarded_child(child_alive)};
}

generator<int> singleton(int value) { co_yield value; }

void test_lazy_start_and_empty_sequence() {
  int entered = 0;
  auto values = integer_sequence(entered);
  CHECK(entered == 0);

  auto it = values.begin();
  CHECK(entered == 1);
  CHECK(it != values.end());
  CHECK(*it == 1);

  int empty_entered = 0;
  auto empty = empty_sequence(empty_entered);
  CHECK(empty_entered == 0);
  auto empty_it = empty.begin();
  CHECK(empty_entered == 1);
  CHECK(empty_it == empty.end());
}

void test_value_iteration_and_iterator_operations() {
  int entered = 0;
  auto values = integer_sequence(entered);
  auto it = values.begin();
  CHECK(*it == 1);
  it++;
  CHECK(*it == 2);
  ++it;
  CHECK(*it == 3);
  ++it;
  CHECK(it == values.end());

  entered = 0;
  CHECK(collect(integer_sequence(entered)) == std::vector<int>({1, 2, 3}));
}

void test_copy_awaiter_keeps_snapshot() {
  std::string source = "before";
  auto values = copy_lvalue(source);
  auto it = values.begin();

  auto &&yielded = *it;

  CHECK(std::addressof(yielded) != std::addressof(source));
  CHECK(yielded == "before");
  source = "after";
  CHECK(yielded == "before");

  ++it;
  CHECK(it == values.end());
  CHECK(collect(string_sequence()) == std::vector<std::string>({"alpha", "beta"}));
}

void test_mutable_reference_semantics() {
  std::vector<int> source{1, 2, 3};
  auto values = mutable_references(source);
  auto it = values.begin();
  CHECK(std::addressof(*it) == std::addressof(source[0]));
  *it = 10;
  ++it;
  CHECK(std::addressof(*it) == std::addressof(source[1]));
  *it = 20;
  ++it;
  *it = 30;
  ++it;
  CHECK(it == values.end());
  CHECK(source == std::vector<int>({10, 20, 30}));
}

void test_const_and_converted_references() {
  int source = 42;
  auto values = const_references(source);
  auto it = values.begin();
  CHECK(std::addressof(*it) == std::addressof(source));
  CHECK(*it == 42);
  ++it;
  CHECK(*it == 99);
  ++it;
  CHECK(it == values.end());

  CHECK(collect(converted_references()) == std::vector<std::string>({"", "alpha", "beta"}));
}

void test_move_only_values() {
  auto pointers = collect(move_only_values());
  CHECK(pointers.size() == 2);
  CHECK(pointers[0] != nullptr);
  CHECK(pointers[1] != nullptr);
  CHECK(*pointers[0] == 4);
  CHECK(*pointers[1] == 5);
}

void test_recursive_generator_order() {
  CHECK(collect(nested_sequence()) == std::vector<int>({0, 1, 2, 3, 4, 5, 6}));
}

void test_deep_recursion_and_unwind() {
  constexpr int depth = 64;
  auto values = collect(recursive_depth(depth));
  CHECK(values.size() == static_cast<std::size_t>(2 * (depth + 1)));

  for (int i = 0; i <= depth; ++i)
    CHECK(values[static_cast<std::size_t>(i)] == depth - i);
  for (int i = 0; i <= depth; ++i)
    CHECK(values[static_cast<std::size_t>(depth + 1 + i)] == -i);
}

void test_generic_range_elements() {
  CHECK(collect(iota_elements()) == std::vector<int>({3, 4, 5, 6}));

  std::vector<int> mutable_values{1, 2, 3};
  for (int &value : vector_elements(mutable_values))
    value *= 2;
  CHECK(mutable_values == std::vector<int>({2, 4, 6}));

  const std::vector<int> const_values{4, 5, 6};
  CHECK(collect(const_vector_elements(const_values)) == const_values);
  CHECK(collect(temporary_vector_elements()) == std::vector<int>({7, 8, 9}));
}

void test_root_exception_propagation() {
  auto before = throw_before_first_yield();
  check_throws<generator_error>([&] { (void)before.begin(); });

  auto after = throw_after_first_yield();
  auto it = after.begin();
  CHECK(*it == 1);
  check_throws<generator_error>([&] { ++it; });
}

void test_nested_exception_propagation() {
  bool continued = false;
  auto values = failing_parent(continued);
  auto it = values.begin();
  CHECK(*it == 6);
  ++it;
  CHECK(*it == 7);
  check_throws<generator_error>([&] { ++it; });
  CHECK(!continued);
}

void test_generic_range_exception_propagation() {
  throwing_range begin_failure;
  begin_failure.throw_on_begin = true;
  auto begin_values = throwing_range_elements(begin_failure);
  check_throws<range_error>([&] { (void)begin_values.begin(); });

  throwing_range increment_failure;
  increment_failure.throw_on_increment = true;
  auto increment_values = throwing_range_elements(increment_failure);
  auto it = increment_values.begin();
  CHECK(*it == 10);
  check_throws<range_error>([&] { ++it; });
}

void test_early_generator_destruction() {
  int alive = 0;
  {
    auto values = guarded_sequence(alive);
    CHECK(alive == 0);
    auto it = values.begin();
    CHECK(*it == 1);
    CHECK(alive == 1);
  }
  CHECK(alive == 0);

  {
    auto values = guarded_sequence(alive);
    CHECK(collect(values) == std::vector<int>({1, 2}));
    CHECK(alive == 0);
  }
  CHECK(alive == 0);
}

void test_nested_generator_destruction() {
  int parent_alive = 0;
  int child_alive = 0;
  {
    auto values = guarded_parent(parent_alive, child_alive);
    auto it = values.begin();
    CHECK(*it == 11);
    CHECK(parent_alive == 1);
    CHECK(child_alive == 1);
  }
  CHECK(parent_alive == 0);
  CHECK(child_alive == 0);
}

void test_generator_move_operations() {
  auto source = singleton(42);
  auto moved = std::move(source);
  CHECK(collect(moved) == std::vector<int>({42}));

  int old_frame_alive = 0;
  {
    auto destination = guarded_sequence(old_frame_alive);
    auto old_iterator = destination.begin();
    CHECK(*old_iterator == 1);
    CHECK(old_frame_alive == 1);

    auto replacement = singleton(9);
    destination = std::move(replacement);
    CHECK(collect(destination) == std::vector<int>({9}));
  }
  CHECK(old_frame_alive == 0);
}

} // namespace

int main() {
  run_test("lazy start and empty sequence", test_lazy_start_and_empty_sequence);
  run_test("value iteration and iterator operations", test_value_iteration_and_iterator_operations);
  run_test("copy awaiter snapshot", test_copy_awaiter_keeps_snapshot);
  run_test("mutable reference semantics", test_mutable_reference_semantics);
  run_test("const and converted references", test_const_and_converted_references);
  run_test("move-only values", test_move_only_values);
  run_test("recursive generator order", test_recursive_generator_order);
  run_test("deep recursion and unwind", test_deep_recursion_and_unwind);
  run_test("generic range elements", test_generic_range_elements);
  run_test("root exception propagation", test_root_exception_propagation);
  run_test("nested exception propagation", test_nested_exception_propagation);
  run_test("generic range exception propagation", test_generic_range_exception_propagation);
  run_test("early generator destruction", test_early_generator_destruction);
  run_test("nested generator destruction", test_nested_generator_destruction);
  run_test("generator move operations", test_generator_move_operations);

  if (failed_tests != 0) {
    std::cerr << failed_tests << " test(s) failed\n";
    return 1;
  }

  std::cout << "All generator tests passed\n";
  return 0;
}
