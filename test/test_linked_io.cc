#include "io.hpp"
#include "scheduler.hpp"
#include "task.hpp"

#include <array>
#include <cerrno>
#include <concepts>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {

using mira::co::core::operator&&;
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

struct test_nop : mira::co::core::io_awaiter {
  test_nop() = default;
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_nop(sqe); }
};

using two_nop_chain = decltype(test_nop{} && test_nop{});
static_assert(!std::is_reference_v<two_nop_chain>,
              "operator&& must return an owning linked awaiter by value");
static_assert(std::destructible<two_nop_chain>, "a linked awaiter must be destructible");

void run_scheduler(task<void> item) {
  scheduler sche;
  sche.co_spawn(std::move(item));
  sche.start();
  sche.join();
}

task<void> await_two_nops(int32_t &result, bool &resumed) {
  result = co_await (test_nop{} && test_nop{});
  resumed = true;
}

task<void> await_three_nops(int32_t &result, bool &resumed) {
  result = co_await (test_nop{} && test_nop{} && test_nop{});
  resumed = true;
}

task<void> await_joined_chains(int32_t &result, bool &resumed) {
  result = co_await ((test_nop{} && test_nop{}) && (test_nop{} && test_nop{}));
  resumed = true;
}

task<void> linked_failure(int32_t &result, bool &resumed) {
  result = co_await (mira::co::io::close(-1) && test_nop{});
  resumed = true;
}

struct linked_socket_results {
  int32_t result = -1;
  std::array<char, 32> received{};
};

task<void> linked_socket_round_trip(int left, int right, linked_socket_results &results) {
  std::array<char, 11> message{'l', 'i', 'n', 'k', 'e', 'd', ' ', 'i', 'o', '!', '\n'};
  results.result =
      co_await (mira::co::io::send(left, message) && mira::co::io::recv(right, results.received));
}

task<void> linked_nop_once(int &completed, int &bad_results) {
  int32_t result = co_await (test_nop{} && test_nop{} && test_nop{});
  if (result != 0)
    ++bad_results;
  ++completed;
}

void test_two_element_chain() {
  int32_t result = -1;
  bool resumed = false;
  run_scheduler(await_two_nops(result, resumed));
  CHECK(resumed);
  CHECK(result == 0);
}

void test_append_to_chain() {
  int32_t result = -1;
  bool resumed = false;
  run_scheduler(await_three_nops(result, resumed));
  CHECK(resumed);
  CHECK(result == 0);
}

void test_join_two_chains() {
  int32_t result = -1;
  bool resumed = false;
  run_scheduler(await_joined_chains(result, resumed));
  CHECK(resumed);
  CHECK(result == 0);
}

void test_failed_link_cancels_remaining_operations() {
  int32_t result = 0;
  bool resumed = false;
  run_scheduler(linked_failure(result, resumed));
  CHECK(resumed);
  CHECK(result == -ECANCELED);
}

void test_linked_socket_operations() {
  int sockets[2]{-1, -1};
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

  linked_socket_results results;
  run_scheduler(linked_socket_round_trip(sockets[0], sockets[1], results));

  constexpr std::string_view expected = "linked io!\n";
  CHECK(results.result == static_cast<int32_t>(expected.size()));
  CHECK(std::string_view(results.received.data(), expected.size()) == expected);
  CHECK(::close(sockets[0]) == 0);
  CHECK(::close(sockets[1]) == 0);
}

void test_many_linked_completions() {
  constexpr int chain_count = 256;
  int completed = 0;
  int bad_results = 0;

  scheduler sche;
  for (int i = 0; i < chain_count; ++i)
    sche.co_spawn(linked_nop_once(completed, bad_results));

  sche.start();
  sche.join();
  CHECK(completed == chain_count);
  CHECK(bad_results == 0);
}

} // namespace

int main() {
  run_test("two-element linked chain", test_two_element_chain);
  run_test("append operation to linked chain", test_append_to_chain);
  run_test("join two linked chains", test_join_two_chains);
  run_test("failed link cancellation", test_failed_link_cancels_remaining_operations);
  run_test("linked socket operations", test_linked_socket_operations);
  run_test("many linked completions", test_many_linked_completions);

  if (failed_tests != 0) {
    std::cerr << failed_tests << " test(s) failed\n";
    return 1;
  }

  std::cout << "All linked IO tests passed\n";
  return 0;
}
