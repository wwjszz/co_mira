#include "io.hpp"
#include "scheduler.hpp"
#include "task.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {

using namespace std::chrono_literals;
using mira::co::core::operator&&;
using mira::co::scheduler;
using mira::co::task;

using timeout_result = mira::co::core::io_with_timeout_result;
using nop_action = mira::co::core::io_nop;
using nop_chain = decltype(nop_action{} && nop_action{});

struct throwing_move_action : mira::co::core::io_awaiter {
  throwing_move_action() = default;
  throwing_move_action(throwing_move_action &&) noexcept(false) {}
  throwing_move_action &operator=(throwing_move_action &&) = delete;

  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_nop(sqe); }
};

static_assert(!noexcept(mira::co::io::with_timeout(std::declval<nop_action &&>(), std::declval<std::chrono::milliseconds>())));
static_assert(!noexcept(mira::co::io::with_timeout(std::declval<nop_chain &&>(), std::declval<std::chrono::milliseconds>())));
static_assert(noexcept(mira::co::io::with_timeout(std::declval<nop_action &&>(), std::declval<__kernel_timespec>())));
static_assert(noexcept(mira::co::io::with_timeout(std::declval<nop_chain &&>(), std::declval<__kernel_timespec>())));
static_assert(!noexcept(mira::co::io::with_timeout(std::declval<throwing_move_action &&>(), std::declval<std::chrono::milliseconds>())));
static_assert(std::is_move_constructible_v<nop_action>);
static_assert(!std::is_move_assignable_v<nop_action>);
static_assert(!std::copy_constructible<mira::co::io::cancel_token>);
static_assert(!std::move_constructible<mira::co::io::cancel_token>);

class test_failure : public std::runtime_error {
public:
  test_failure(const char *expression, const char *file, int line)
      : std::runtime_error(std::string(file) + ':' + std::to_string(line) + ": check failed: " + expression) {}
};

#define CHECK(expression)                                                                                                                            \
  do {                                                                                                                                               \
    if (!(expression)) {                                                                                                                             \
      mira::co::log("check failed: {}", #expression);                                                                                                \
      throw test_failure(#expression, __FILE__, __LINE__);                                                                                           \
    }                                                                                                                                                \
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

void run_scheduler(task<void> item) {
  scheduler sche;
  sche.co_spawn(std::move(item));
  sche.start();
  sche.join();
}

task<void> await_nop_with_timeout(timeout_result &result) { result = co_await mira::co::io::with_timeout(mira::co::io::nop(), 100ms); }

task<void> await_nop(int32_t &result) { result = co_await mira::co::io::nop(); }

task<void> await_cancellable_nop(mira::co::io::cancel_token &token, int32_t &result) { result = co_await mira::co::io::nop(token); }

task<void> await_cancellable_recv(int fd, mira::co::io::cancel_token &token, int32_t &result) {
  std::array<char, 8> buffer{};
  result = co_await mira::co::io::recv(fd, buffer, token);
}

task<void> await_cancellable_linked_recv(int fd, mira::co::io::cancel_token &token, int32_t &result) {
  std::array<char, 8> buffer{};
  auto chain = mira::co::io::recv(fd, buffer, token) && mira::co::io::nop();
  result = co_await std::move(chain);
}

task<void> await_cancellable_sleep(mira::co::io::cancel_token &token, int32_t &result) { result = co_await mira::co::io::sleep_for(200ms, token); }

task<void> await_connect(int fd, const sockaddr_in &address, int32_t &result) {
  result = co_await mira::co::io::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
}

task<void> cancel_after_delay(mira::co::io::cancel_token &token, int32_t &result, std::chrono::milliseconds delay) {
  (void)co_await mira::co::io::sleep_for(delay);
  result = co_await mira::co::io::cancel(token);
}

task<void> await_sleep(std::chrono::nanoseconds duration, int32_t &result, std::chrono::steady_clock::duration &elapsed) {
  auto start = std::chrono::steady_clock::now();
  result = co_await mira::co::io::sleep_for(duration);
  elapsed = std::chrono::steady_clock::now() - start;
}

task<void> await_invalid_timeout(int32_t &result) {
  __kernel_timespec invalid{.tv_sec = 0, .tv_nsec = -1};
  result = co_await mira::co::io::sleep_for(invalid);
}

task<void> await_failed_linked_chain_with_timeout(timeout_result &result) {
  auto chain = mira::co::io::close(-1) && mira::co::io::nop();
  result = co_await mira::co::io::with_timeout(std::move(chain), 100ms);
}

struct message_results {
  int32_t sent = 0;
  int32_t received = 0;
  std::array<char, 16> buffer{};
};

task<void> send_and_receive_message(int sender, int receiver, message_results &results) {
  std::array<char, 6> payload{'m', 'i', 'r', 'a', '!', '\0'};
  iovec send_iov{.iov_base = payload.data(), .iov_len = payload.size()};
  msghdr send_message{};
  send_message.msg_iov = &send_iov;
  send_message.msg_iovlen = 1;

  results.sent = co_await mira::co::io::sendmsg(sender, &send_message);

  iovec receive_iov{.iov_base = results.buffer.data(), .iov_len = results.buffer.size()};
  msghdr receive_message{};
  receive_message.msg_iov = &receive_iov;
  receive_message.msg_iovlen = 1;

  results.received = co_await mira::co::io::recvmsg(receiver, &receive_message);
}

task<void> await_short_sleep(int &completed, int &errors) {
  auto result = co_await mira::co::io::sleep_for(1ms);
  ++completed;
  if (result != -ETIME)
    ++errors;
}

task<void> await_failed_action_with_timeout(timeout_result &result) { result = co_await mira::co::io::with_timeout(mira::co::io::close(-1), 100ms); }

task<void> await_recv_timeout(int fd, timeout_result &result) {
  std::array<char, 8> buffer{};
  result = co_await mira::co::io::with_timeout(mira::co::io::recv(fd, buffer), 10ms);
}

task<void> await_linked_nops_with_timeout(timeout_result &result) {
  auto chain = mira::co::io::nop() && mira::co::io::nop();
  result = co_await mira::co::io::with_timeout(std::move(chain), 100ms);
}

void test_public_nop() {
  int32_t result = -1;
  run_scheduler(await_nop(result));
  CHECK(result == 0);
}

void test_sleep_for_returns_timeout_and_waits() {
  int32_t result = 0;
  std::chrono::steady_clock::duration elapsed{};
  run_scheduler(await_sleep(10ms, result, elapsed));

  CHECK(result == -ETIME);
  CHECK(elapsed >= 5ms);
}

void test_timeout_duration_boundaries() {
  int32_t zero_result = 0;
  std::chrono::steady_clock::duration elapsed{};
  run_scheduler(await_sleep(0ns, zero_result, elapsed));
  CHECK(zero_result == -ETIME);

  int32_t negative_result = 0;
  run_scheduler(await_sleep(-1ns, negative_result, elapsed));
  CHECK(negative_result == -EINVAL);

  int32_t invalid_result = 0;
  run_scheduler(await_invalid_timeout(invalid_result));
  CHECK(invalid_result == -EINVAL);
}

void test_timeout_conversion_errors() {
  auto invalid = check_throws<std::invalid_argument>(
      [] { (void)mira::co::io::sleep_for(std::chrono::duration<long double>{std::numeric_limits<long double>::infinity()}); });
  CHECK(std::string_view(invalid.what()) == "timeout duration is not finite");

  auto overflow = check_throws<std::overflow_error>(
      [] { (void)mira::co::io::sleep_for(std::chrono::duration<long double>{std::numeric_limits<long double>::max()}); });
  CHECK(std::string_view(overflow.what()) == "timeout duration exceeds __kernel_timespec range");

  auto invalid_with_timeout = check_throws<std::invalid_argument>([] {
    (void)mira::co::io::with_timeout(mira::co::io::nop(), std::chrono::duration<long double>{std::numeric_limits<long double>::infinity()});
  });
  CHECK(std::string_view(invalid_with_timeout.what()) == "timeout duration is not finite");

  auto overflow_with_timeout = check_throws<std::overflow_error>(
      [] { (void)mira::co::io::with_timeout(mira::co::io::nop(), std::chrono::duration<long double>{std::numeric_limits<long double>::max()}); });
  CHECK(std::string_view(overflow_with_timeout.what()) == "timeout duration exceeds __kernel_timespec range");
}

void test_cancel_pending_recv() {
  int sockets[2]{-1, -1};
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

  mira::co::io::cancel_token token;
  int32_t recv_result = 0;
  int32_t cancel_result = -1;
  scheduler sche;
  sche.co_spawn(await_cancellable_recv(sockets[0], token, recv_result));
  sche.co_spawn(cancel_after_delay(token, cancel_result, 1ms));
  sche.start();
  sche.join();

  CHECK(cancel_result == 0);
  CHECK(recv_result == -ECANCELED || recv_result == -EINTR);
  CHECK(token.completed());
  CHECK(::close(sockets[0]) == 0);
  CHECK(::close(sockets[1]) == 0);
}

void test_cancel_pending_timeout() {
  mira::co::io::cancel_token token;
  int32_t timeout_result = 0;
  int32_t cancel_result = -1;
  scheduler sche;
  sche.co_spawn(await_cancellable_sleep(token, timeout_result));
  sche.co_spawn(cancel_after_delay(token, cancel_result, 1ms));
  sche.start();
  sche.join();

  CHECK(cancel_result == 0);
  CHECK(timeout_result == -ECANCELED);
  CHECK(token.completed());
}

void test_duplicate_cancel_requests() {
  int sockets[2]{-1, -1};
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

  mira::co::io::cancel_token token;
  int32_t recv_result = 0;
  int32_t first_cancel_result = 1;
  int32_t second_cancel_result = 1;
  scheduler sche;
  sche.co_spawn(await_cancellable_recv(sockets[0], token, recv_result));
  sche.co_spawn(cancel_after_delay(token, first_cancel_result, 1ms));
  sche.co_spawn(cancel_after_delay(token, second_cancel_result, 1ms));
  sche.start();
  sche.join();

  auto is_valid_cancel_result = [](int32_t result) { return result == 0 || result == -ENOENT || result == -EALREADY; };
  CHECK(is_valid_cancel_result(first_cancel_result));
  CHECK(is_valid_cancel_result(second_cancel_result));
  CHECK(first_cancel_result == 0 || second_cancel_result == 0);
  CHECK(recv_result == -ECANCELED || recv_result == -EINTR);
  CHECK(token.completed());
  CHECK(::close(sockets[0]) == 0);
  CHECK(::close(sockets[1]) == 0);
}

void test_cancel_completed_operation() {
  mira::co::io::cancel_token token;
  int32_t nop_result = -1;
  int32_t cancel_result = 0;
  scheduler sche;
  sche.co_spawn(await_cancellable_nop(token, nop_result));
  sche.co_spawn(cancel_after_delay(token, cancel_result, 5ms));
  sche.start();
  sche.join();

  CHECK(nop_result == 0);
  CHECK(cancel_result == -ENOENT);
  CHECK(token.completed());
}

void test_cancel_linked_operation() {
  int sockets[2]{-1, -1};
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

  mira::co::io::cancel_token token;
  int32_t chain_result = 0;
  int32_t cancel_result = -1;
  scheduler sche;
  sche.co_spawn(await_cancellable_linked_recv(sockets[0], token, chain_result));
  sche.co_spawn(cancel_after_delay(token, cancel_result, 1ms));
  sche.start();
  sche.join();

  CHECK(cancel_result == 0);
  CHECK(chain_result == -ECANCELED);
  CHECK(token.completed());
  CHECK(::close(sockets[0]) == 0);
  CHECK(::close(sockets[1]) == 0);
}

void test_cancel_rejects_unbound_and_reused_tokens() {
  mira::co::io::cancel_token unbound;
  auto unbound_error = check_throws<std::logic_error>([&] { (void)mira::co::io::cancel(unbound); });
  CHECK(std::string_view(unbound_error.what()) == "cannot cancel an operation before submission");

  mira::co::io::cancel_token completed;
  int32_t result = -1;
  run_scheduler(await_cancellable_nop(completed, result));
  CHECK(result == 0);

  auto reuse_error = check_throws<std::logic_error>([&] { run_scheduler(await_cancellable_nop(completed, result)); });
  CHECK(std::string_view(reuse_error.what()) == "cancel token is already bound to an operation");
}

void test_action_completes_before_timeout() {
  timeout_result result{};
  run_scheduler(await_nop_with_timeout(result));

  CHECK(result.action_result == 0);
  CHECK(result.timeout_result == -ECANCELED);
  CHECK(!result.timed_out());
}

void test_action_failure_cancels_timeout() {
  timeout_result result{};
  run_scheduler(await_failed_action_with_timeout(result));

  CHECK(result.action_result == -EBADF);
  CHECK(result.timeout_result == -ECANCELED);
  CHECK(!result.timed_out());
}

void test_timeout_cancels_pending_recv() {
  int sockets[2]{-1, -1};
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

  timeout_result result{};
  run_scheduler(await_recv_timeout(sockets[0], result));

  CHECK(result.timed_out());
  CHECK(result.timeout_result == -ETIME);
  CHECK(result.action_result == -ECANCELED || result.action_result == -EINTR);
  CHECK(::close(sockets[0]) == 0);
  CHECK(::close(sockets[1]) == 0);
}

void test_linked_chain_completes_before_timeout() {
  timeout_result result{};
  run_scheduler(await_linked_nops_with_timeout(result));

  CHECK(result.action_result == 0);
  CHECK(result.timeout_result == -ECANCELED);
  CHECK(!result.timed_out());
}

void test_failed_linked_chain_cancels_remaining_operations() {
  timeout_result result{};
  run_scheduler(await_failed_linked_chain_with_timeout(result));

  CHECK(result.action_result == -ECANCELED);
  CHECK(result.timeout_result == -ECANCELED);
  CHECK(!result.timed_out());
}

void test_recvmsg_sendmsg_round_trip() {
  int sockets[2]{-1, -1};
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

  message_results results{};
  run_scheduler(send_and_receive_message(sockets[0], sockets[1], results));

  CHECK(results.sent == 6);
  CHECK(results.received == 6);
  CHECK(std::string_view(results.buffer.data(), 6) == std::string_view("mira!\0", 6));
  CHECK(::close(sockets[0]) == 0);
  CHECK(::close(sockets[1]) == 0);
}

void test_connect_success_and_failure() {
  int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  CHECK(listener >= 0);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  CHECK(::bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
  CHECK(::listen(listener, 1) == 0);

  socklen_t address_length = sizeof(address);
  CHECK(::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_length) == 0);

  int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  CHECK(client >= 0);
  int32_t connect_result = -1;
  run_scheduler(await_connect(client, address, connect_result));
  CHECK(connect_result == 0);

  int accepted = ::accept(listener, nullptr, nullptr);
  CHECK(accepted >= 0);
  CHECK(::close(accepted) == 0);
  CHECK(::close(client) == 0);
  CHECK(::close(listener) == 0);

  sockaddr_in invalid_address{};
  invalid_address.sin_family = AF_INET;
  int32_t failure_result = 0;
  run_scheduler(await_connect(-1, invalid_address, failure_result));
  CHECK(failure_result == -EBADF);
}

void test_concurrent_timeout_batch() {
  constexpr int operation_count = 32;
  int completed = 0;
  int errors = 0;
  scheduler sche;
  for (int i = 0; i < operation_count; ++i)
    sche.co_spawn(await_short_sleep(completed, errors));

  sche.start();
  sche.join();

  CHECK(completed == operation_count);
  CHECK(errors == 0);
}

} // namespace

int main() {
  run_test("public nop", test_public_nop);
  run_test("sleep_for timeout and elapsed time", test_sleep_for_returns_timeout_and_waits);
  run_test("timeout duration boundaries", test_timeout_duration_boundaries);
  run_test("timeout conversion errors", test_timeout_conversion_errors);
  run_test("cancel pending recv", test_cancel_pending_recv);
  run_test("cancel pending timeout", test_cancel_pending_timeout);
  run_test("duplicate cancel requests", test_duplicate_cancel_requests);
  run_test("cancel completed operation", test_cancel_completed_operation);
  run_test("cancel linked operation", test_cancel_linked_operation);
  run_test("cancel token validation", test_cancel_rejects_unbound_and_reused_tokens);
  run_test("action completes before timeout", test_action_completes_before_timeout);
  run_test("action failure cancels timeout", test_action_failure_cancels_timeout);
  run_test("timeout cancels pending recv", test_timeout_cancels_pending_recv);
  run_test("linked chain completes before timeout", test_linked_chain_completes_before_timeout);
  run_test("failed linked chain", test_failed_linked_chain_cancels_remaining_operations);
  run_test("recvmsg/sendmsg round trip", test_recvmsg_sendmsg_round_trip);
  run_test("connect success and failure", test_connect_success_and_failure);
  run_test("concurrent timeout batch", test_concurrent_timeout_batch);

  if (failed_tests != 0) {
    std::cerr << failed_tests << " test(s) failed\n";
    return 1;
  }

  std::cout << "All P2 tests passed\n";
  return 0;
}
