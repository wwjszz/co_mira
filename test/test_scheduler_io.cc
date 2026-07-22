#include "io.hpp"
#include "scheduler.hpp"
#include "task.hpp"
#include "yield.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using mira::co::scheduler;
using mira::co::task;

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

struct test_nop : mira::co::core::io_awaiter {
  test_nop() = default;
  void prepare(io_uring_sqe *sqe) noexcept { io_uring_prep_nop(sqe); }
};

static_assert(mira::co::core::linkable_action<test_nop>);
static_assert(std::move_constructible<test_nop>);
static_assert(!std::is_move_assignable_v<test_nop>);
static_assert(!noexcept(std::declval<test_nop &>().await_suspend(std::declval<mira::co::co_handle<>>())));
static_assert(!noexcept(std::declval<scheduler &>().co_spawn(std::declval<task<void> &&>())));

void run_scheduler(task<void> item) {
  scheduler sche;
  sche.co_spawn(std::move(item));
  sche.start();
  sche.join();
}

task<void> mark_entered(bool &entered) {
  entered = true;
  co_return;
}

task<void> record_value(std::vector<int> &trace, int value) {
  trace.push_back(value);
  co_return;
}

task<void> spawn_from_host(scheduler &sche, std::vector<int> &trace) {
  trace.push_back(1);
  sche.co_spawn(record_value(trace, 3));
  trace.push_back(2);
  co_await mira::co::yield();
  trace.push_back(4);
}

task<void> yielding_task(std::vector<int> &trace, int id) {
  trace.push_back(id * 10 + 1);
  co_await mira::co::yield();
  trace.push_back(id * 10 + 2);
}

task<void> throw_from_detached_task() {
  mira::co::log("throwing detached task failure");
  throw std::runtime_error("detached failure");
  co_return;
}

task<void> await_temporary_nop(int32_t &result, bool &resumed) {
  result = co_await test_nop{};
  resumed = true;
}

task<void> await_stored_nop(test_nop operation, int32_t &result, bool &resumed) {
  result = co_await operation;
  resumed = true;
}

task<void> await_stored_recv(mira::co::core::io_recv operation, int32_t &result) { result = co_await operation; }

task<void> nop_once(int &completed, int &bad_results) {
  int32_t result = co_await test_nop{};
  if (result != 0)
    ++bad_results;
  ++completed;
}

task<void> invalid_fd_operations(int32_t &recv_result, int32_t &close_result) {
  std::array<char, 8> buffer{};
  recv_result = co_await mira::co::io::recv(-1, buffer);
  close_result = co_await mira::co::io::close(-1);
}

struct socket_results {
  int32_t sent = -1;
  int32_t received = -1;
  int32_t close_left = -1;
  int32_t close_right = -1;
  std::array<char, 32> buffer{};
};

task<void> socket_round_trip(int left, int right, socket_results &results) {
  std::array<char, 12> message{'h', 'e', 'l', 'l', 'o', ' ', 'm', 'i', 'r', 'a', '!', '\n'};

  results.sent = co_await mira::co::io::send(left, message);
  results.received = co_await mira::co::io::recv(right, results.buffer);
  results.close_left = co_await mira::co::io::close(left);
  results.close_right = co_await mira::co::io::close(right);
}

struct vector_io_results {
  int32_t written = -1;
  int32_t read = -1;
  int32_t close_read = -1;
  int32_t close_write = -1;
  std::array<char, 3> first{};
  std::array<char, 4> second{};
};

task<void> vector_io_round_trip(int read_fd, int write_fd, vector_io_results &results) {
  std::array<char, 2> first_out{'a', 'b'};
  std::array<char, 5> second_out{'c', 'd', 'e', 'f', 'g'};
  std::array<iovec, 2> write_vectors{
      iovec{.iov_base = first_out.data(), .iov_len = first_out.size()},
      iovec{.iov_base = second_out.data(), .iov_len = second_out.size()},
  };
  std::array<iovec, 2> read_vectors{
      iovec{.iov_base = results.first.data(), .iov_len = results.first.size()},
      iovec{.iov_base = results.second.data(), .iov_len = results.second.size()},
  };

  results.written = co_await mira::co::io::writev(write_fd, write_vectors);
  results.read = co_await mira::co::io::readv(read_fd, read_vectors);
  results.close_read = co_await mira::co::io::close(read_fd);
  results.close_write = co_await mira::co::io::close(write_fd);
}

struct accept_results {
  int32_t accepted_fd = -1;
  int32_t close_accepted = -1;
  int32_t close_listener = -1;
  sa_family_t peer_family = AF_UNSPEC;
};

task<void> accept_one(int listener, accept_results &results) {
  sockaddr_storage peer{};
  socklen_t peer_length = sizeof(peer);

  results.accepted_fd = co_await mira::co::io::accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_length);
  if (results.accepted_fd >= 0) {
    results.peer_family = peer.ss_family;
    results.close_accepted = co_await mira::co::io::close(results.accepted_fd);
  }
  results.close_listener = co_await mira::co::io::close(listener);
}

struct listener_pair {
  int listener = -1;
  int client = -1;
};

listener_pair make_connected_listener() {
  listener_pair result;
  result.listener = ::socket(AF_INET, SOCK_STREAM, 0);
  CHECK(result.listener >= 0);

  int reuse = 1;
  CHECK(::setsockopt(result.listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  CHECK(::bind(result.listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
  CHECK(::listen(result.listener, 8) == 0);

  socklen_t address_length = sizeof(address);
  CHECK(::getsockname(result.listener, reinterpret_cast<sockaddr *>(&address), &address_length) == 0);

  result.client = ::socket(AF_INET, SOCK_STREAM, 0);
  CHECK(result.client >= 0);
  CHECK(::connect(result.client, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
  return result;
}

void test_lazy_start_and_join() {
  bool entered = false;
  scheduler sche;
  sche.co_spawn(mark_entered(entered));
  CHECK(!entered);

  sche.start();
  sche.join();
  CHECK(entered);
}

void test_empty_scheduler_lifecycle() {
  scheduler sche;
  sche.start();
  sche.join();
}

void test_user_data_round_trip() {
  mira::co::core::task_info info{};
  using type = mira::co::core::user_data::type;

  for (type expected : {type::task_info_pointer, type::task_info_linked}) {
    uint64_t encoded = info.as_user_data() | static_cast<uint64_t>(expected);
    auto [decoded_info, decoded_type] = mira::co::core::user_data(encoded).from_user_data();
    CHECK(decoded_info == std::addressof(info));
    CHECK(decoded_type == expected);
  }

  for (type expected : {type::msg_ring, type::msg_ring_ack, type::resume_on}) {
    uint64_t encoded = info.as_user_data() | static_cast<uint64_t>(expected);
    mira::co::core::user_data decoded(encoded);
    CHECK(decoded.address_from_user_data() == std::addressof(info));
    CHECK(decoded.type_from_user_data() == expected);
  }

  for (type expected : {type::msg_ring_stop, type::msg_ring_stop_ack}) {
    uint64_t encoded = static_cast<uint64_t>(expected);
    CHECK(mira::co::core::user_data(encoded).type_from_user_data() == expected);
  }

  uint64_t encoded = info.as_user_data() | 7;
  CHECK(mira::co::core::user_data(encoded).type_from_user_data() == type::unknown);
  CHECK(mira::co::core::user_data(mira::co::core::user_data::shutdown_cancel).type_from_user_data() == type::unknown);
}

void test_host_spawn_and_yield_order() {
  std::vector<int> trace;
  scheduler sche;
  sche.co_spawn(spawn_from_host(sche, trace));
  sche.start();
  sche.join();
  CHECK(trace == std::vector<int>({1, 2, 3, 4}));

  trace.clear();
  scheduler fair_scheduler;
  fair_scheduler.co_spawn(yielding_task(trace, 1));
  fair_scheduler.co_spawn(yielding_task(trace, 2));
  fair_scheduler.start();
  fair_scheduler.join();
  CHECK(trace == std::vector<int>({11, 21, 12, 22}));
}

void test_detached_exception_does_not_stop_other_tasks() {
  bool completed = false;
  scheduler sche;
  sche.co_spawn(throw_from_detached_task());
  sche.co_spawn(mark_entered(completed));
  sche.start();
  bool caught = false;
  try {
    sche.join();
  } catch (const std::runtime_error &error) {
    caught = std::string_view(error.what()) == "detached failure";
  }
  CHECK(caught);
  CHECK(completed);
}

void test_unstarted_scheduler_does_not_execute_tasks_in_destructor() {
  bool entered = false;
  {
    scheduler sche;
    sche.co_spawn(mark_entered(entered));
  }
  CHECK(!entered);
}

void test_temporary_and_preconstructed_awaiters() {
  int32_t temporary_result = -1;
  bool temporary_resumed = false;
  run_scheduler(await_temporary_nop(temporary_result, temporary_resumed));
  CHECK(temporary_resumed);
  CHECK(temporary_result == 0);

  // Construction outside a scheduler must not reserve or submit an SQE.
  test_nop operation;
  int32_t stored_result = -1;
  bool stored_resumed = false;
  run_scheduler(await_stored_nop(std::move(operation), stored_result, stored_resumed));
  CHECK(stored_resumed);
  CHECK(stored_result == 0);

  std::array<char, 8> buffer{};
  auto recv_operation = mira::co::io::recv(-1, buffer);
  int32_t recv_result = 0;
  run_scheduler(await_stored_recv(std::move(recv_operation), recv_result));
  CHECK(recv_result == -EBADF);
}

void test_many_nop_completions() {
  constexpr int operation_count = 512;
  int completed = 0;
  int bad_results = 0;

  scheduler sche;
  for (int i = 0; i < operation_count; ++i)
    sche.co_spawn(nop_once(completed, bad_results));

  sche.start();
  sche.join();
  CHECK(completed == operation_count);
  CHECK(bad_results == 0);
}

void test_negative_io_results() {
  int32_t recv_result = 0;
  int32_t close_result = 0;
  run_scheduler(invalid_fd_operations(recv_result, close_result));
  CHECK(recv_result == -EBADF);
  CHECK(close_result == -EBADF);
}

void test_send_recv_and_async_close() {
  int sockets[2]{-1, -1};
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

  socket_results results;
  run_scheduler(socket_round_trip(sockets[0], sockets[1], results));

  constexpr std::string_view expected = "hello mira!\n";
  CHECK(results.sent == static_cast<int32_t>(expected.size()));
  CHECK(results.received == static_cast<int32_t>(expected.size()));
  CHECK(std::string_view(results.buffer.data(), expected.size()) == expected);
  CHECK(results.close_left == 0);
  CHECK(results.close_right == 0);

  errno = 0;
  CHECK(::close(sockets[0]) == -1);
  CHECK(errno == EBADF);
  errno = 0;
  CHECK(::close(sockets[1]) == -1);
  CHECK(errno == EBADF);
}

void test_readv_writev() {
  int pipe_fds[2]{-1, -1};
  CHECK(::pipe(pipe_fds) == 0);

  vector_io_results results;
  run_scheduler(vector_io_round_trip(pipe_fds[0], pipe_fds[1], results));

  CHECK(results.written == 7);
  CHECK(results.read == 7);
  CHECK(std::string_view(results.first.data(), results.first.size()) == "abc");
  CHECK(std::string_view(results.second.data(), results.second.size()) == "defg");
  CHECK(results.close_read == 0);
  CHECK(results.close_write == 0);
}

void test_accept_and_close() {
  listener_pair sockets = make_connected_listener();
  accept_results results;
  run_scheduler(accept_one(sockets.listener, results));

  CHECK(results.accepted_fd >= 0);
  CHECK(results.peer_family == AF_INET);
  CHECK(results.close_accepted == 0);
  CHECK(results.close_listener == 0);
  CHECK(::close(sockets.client) == 0);
}

} // namespace

int main() {
  run_test("lazy start and join", test_lazy_start_and_join);
  run_test("empty scheduler lifecycle", test_empty_scheduler_lifecycle);
  run_test("user data round trip", test_user_data_round_trip);
  run_test("host spawn and yield order", test_host_spawn_and_yield_order);
  run_test("detached exception isolation", test_detached_exception_does_not_stop_other_tasks);
  run_test("unstarted scheduler destruction", test_unstarted_scheduler_does_not_execute_tasks_in_destructor);
  run_test("temporary and preconstructed awaiters", test_temporary_and_preconstructed_awaiters);
  run_test("many nop completions", test_many_nop_completions);
  run_test("negative io results", test_negative_io_results);
  run_test("send recv and async close", test_send_recv_and_async_close);
  run_test("readv writev", test_readv_writev);
  run_test("accept and close", test_accept_and_close);

  if (failed_tests != 0) {
    std::cerr << failed_tests << " test(s) failed\n";
    return 1;
  }

  std::cout << "All scheduler/io tests passed\n";
  return 0;
}
