#include "net.hpp"
#include "scheduler.hpp"
#include "task.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

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

template <typename Exception, typename Function> void check_throws(Function &&function) {
  try {
    std::forward<Function>(function)();
  } catch (const Exception &) {
    return;
  }
  mira::co::log("expected exception was not thrown");
  throw test_failure("expected exception", __FILE__, __LINE__);
}

task<void> accept_once(const mira::co::net::acceptor &acceptor, int32_t &result) { result = co_await acceptor.accept(); }

task<void> connect_once(const mira::co::net::socket &client, mira::co::net::inet_address address, int32_t &result) {
  result = co_await client.connect(std::move(address));
}

struct exchange_results {
  int32_t sent = 0;
  int32_t received = 0;
  std::array<char, 16> buffer{};
};

task<void> exchange(mira::co::net::socket &sender, mira::co::net::socket &receiver, exchange_results &results) {
  const std::array<char, 8> payload{'c', 'o', '_', 'm', 'i', 'r', 'a', '!'};
  results.sent = co_await sender.send(payload);
  results.received = co_await receiver.recv(results.buffer);
}

task<void> close_sockets(mira::co::net::socket &first, mira::co::net::socket &second, int32_t &first_result, int32_t &second_result) {
  first_result = co_await first.async_close();
  second_result = co_await second.async_close();
}

void test_unique_fd_move_assignment_closes_old_descriptor() {
  int first_pipe[2]{-1, -1};
  int second_pipe[2]{-1, -1};
  CHECK(::pipe(first_pipe) == 0);
  CHECK(::pipe(second_pipe) == 0);

  const int old_descriptor = first_pipe[0];
  {
    mira::co::io::unique_fd first{first_pipe[0]};
    mira::co::io::unique_fd second{second_pipe[0]};
    first = std::move(second);
    CHECK(::fcntl(old_descriptor, F_GETFD) == -1);
    CHECK(errno == EBADF);
    CHECK(first.get() == second_pipe[0]);
    CHECK(!second);
  }

  CHECK(::fcntl(second_pipe[0], F_GETFD) == -1);
  CHECK(errno == EBADF);
  CHECK(::close(first_pipe[1]) == 0);
  CHECK(::close(second_pipe[1]) == 0);
}

void test_inet_address() {
  mira::co::net::inet_address loopback{"127.0.0.1", 43210};
  CHECK(loopback.family() == AF_INET);
  CHECK(loopback.port() == 43210);
  CHECK(loopback.ip() == "127.0.0.1");

  check_throws<std::invalid_argument>([] { (void)mira::co::net::inet_address{"not-an-address", 1}; });
  check_throws<std::invalid_argument>([] { (void)mira::co::net::inet_address{1, false, AF_UNIX}; });
  check_throws<std::invalid_argument>([] { (void)mira::co::net::socket::adopt(-1); });
}

void test_accept_connect_and_socket_io() {
  int listening_fd = -1;
  {
    mira::co::net::acceptor acceptor{mira::co::net::inet_address{0, true}};
    listening_fd = acceptor.native_handle();
    auto address = acceptor.local_address();
    CHECK(address.ip() == "127.0.0.1");
    CHECK(address.port() != 0);

    auto client = mira::co::net::socket::tcp();
    int32_t accepted_fd = -1;
    int32_t connect_result = -1;
    scheduler connect_scheduler;
    connect_scheduler.co_spawn(accept_once(acceptor, accepted_fd));
    connect_scheduler.co_spawn(connect_once(client, address, connect_result));
    connect_scheduler.start();
    connect_scheduler.join();

    CHECK(connect_result == 0);
    CHECK(accepted_fd >= 0);
    CHECK((::fcntl(accepted_fd, F_GETFD) & FD_CLOEXEC) != 0);

    auto server = mira::co::net::socket::adopt(accepted_fd);
    CHECK(client.peer_address().port() == address.port());
    CHECK(server.local_address().port() == address.port());

    exchange_results results{};
    scheduler exchange_scheduler;
    exchange_scheduler.co_spawn(exchange(client, server, results));
    exchange_scheduler.start();
    exchange_scheduler.join();
    CHECK(results.sent == 8);
    CHECK(results.received == 8);
    CHECK(std::string_view(results.buffer.data(), 8) == "co_mira!");

    int32_t client_close = -1;
    int32_t server_close = -1;
    scheduler close_scheduler;
    close_scheduler.co_spawn(close_sockets(client, server, client_close, server_close));
    close_scheduler.start();
    close_scheduler.join();
    CHECK(client_close == 0);
    CHECK(server_close == 0);
    CHECK(!client);
    CHECK(!server);
  }

  CHECK(::fcntl(listening_fd, F_GETFD) == -1);
  CHECK(errno == EBADF);
}

} // namespace

int main() {
  try {
    test_unique_fd_move_assignment_closes_old_descriptor();
    test_inet_address();
    test_accept_connect_and_socket_io();
  } catch (const std::exception &error) {
    std::cerr << "[FAIL] network wrappers: " << error.what() << '\n';
    return 1;
  }

  std::cout << "All network wrapper tests passed\n";
  return 0;
}
