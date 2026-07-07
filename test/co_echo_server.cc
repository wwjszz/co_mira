#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <coroutine>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <liburing.h>
#include <netdb.h>
#include <queue>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <utility>

#include "io.hpp"
#include "scheduler.hpp"
#include "task.hpp"

static constexpr int LISTEN_BACK_LOG = 32;
static constexpr int BUF_SIZE = 256;
static constexpr int DEPTH = 32;

#define PRINTLN_ERROR(FORMAT_STRING, ...)                                                          \
  std::println(std::cerr, FORMAT_STRING __VA_OPT__(, ) __VA_ARGS__)

#define PRINTLN_PERROR(FUNC) std::println(std::cerr, #FUNC " {}", strerror(errno))

#define PRINTLN_ERROR_EXIT(FORMAT_STRING, ...)                                                     \
  do {                                                                                             \
    PRINTLN_ERROR(FORMAT_STRING, __VA_ARGS__);                                                     \
    std::exit(EXIT_FAILURE);                                                                       \
  } while (0)

#define PRINTLN_PERROR_EXIT(FUNC)                                                                  \
  do {                                                                                             \
    PRINTLN_PERROR(FUNC);                                                                          \
    std::exit(EXIT_FAILURE);                                                                       \
  } while (0)

void print_sockaddr(struct sockaddr *in_addr) {
  char ipstr[INET6_ADDRSTRLEN];
  void *addr;
  const char *ipnstr;

  if (in_addr->sa_family == AF_INET) {
    addr = &(((struct sockaddr_in *)in_addr)->sin_addr);
    ipnstr = "IPv4";
  } else {
    addr = &(((struct sockaddr_in6 *)in_addr)->sin6_addr);
    ipnstr = "IPv6";
  }

  const char *res = inet_ntop(in_addr->sa_family, addr, ipstr, sizeof(ipstr));
  std::println(std::cout, "  {}: {}\n", ipnstr, res);
}

int get_listen_socket(int i_port) {
  char port[10];
  auto result = std::to_chars(port, port + sizeof(port), i_port);
  if (result.ec != std::errc{}) {
    PRINTLN_ERROR_EXIT("{}", std::make_error_code(result.ec).message());
  }

  *result.ptr = '\0';
  std::println(std::cout, "port: {}", port);

  struct addrinfo hint, *servinfo;
  memset(&hint, 0, sizeof(hint));
  hint.ai_family = AF_UNSPEC;
  hint.ai_socktype = SOCK_STREAM;
  hint.ai_flags = AI_PASSIVE;

  int gai_status = getaddrinfo(NULL, port, &hint, &servinfo);
  if (gai_status != 0) {
    PRINTLN_ERROR_EXIT("getaddrinfo: {}", gai_strerror(gai_status));
  }

  struct addrinfo *p;
  int yes = 1, s_fd;
  for (p = servinfo; p != nullptr; p = p->ai_next) {
    s_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (s_fd == -1) {
      PRINTLN_PERROR("socket");
      continue;
    }

    if (setsockopt(s_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) != 0) {
      close(s_fd);
      PRINTLN_PERROR("setsocket");
      continue;
    }

    if (bind(s_fd, p->ai_addr, p->ai_addrlen) != 0) {
      close(s_fd);
      PRINTLN_PERROR("bind");
      continue;
    }

    break;
  }

  if (p == NULL) {
    return -1;
  }

  freeaddrinfo(servinfo);

  if (listen(s_fd, LISTEN_BACK_LOG) != 0) {
    PRINTLN_PERROR("listen");
    return -1;
  }

  return s_fd;
}

using namespace mira::co;
using scheduler = mira::co::scheduler;
using task_info = mira::co::core::task_info;
using user_data_type = mira::co::core::user_data::type;

task<void> handle_client(scheduler &sche, int s_fd, int c_fd) {
  char buf[BUF_SIZE];
  while (1) {
    int res = co_await io::recv(c_fd, buf);
    if (res < 0)
      break;

    if (res == 0) {
      std::println(std::cout, "{} leave", c_fd);
      break;
    }

    std::print(std::cout, "recv: {}", std::string_view(buf, res));

    res = co_await io::send(c_fd, {buf, static_cast<std::size_t>(res)});
    if (res < 0)
      break;
  }
  // close(c_fd);
  co_await io::close(c_fd);
}

task<void> server(scheduler &sche, int s_fd) {
  sockaddr_storage c_addr;
  auto rc_addr = reinterpret_cast<sockaddr *>(&c_addr);
  socklen_t c_addrlen = sizeof(c_addr);
  while (1) {
    int c_fd = co_await io::accept(s_fd, rc_addr, &c_addrlen);
    std::println(std::cout, "server: new connection");
    print_sockaddr(rc_addr);
    sche.co_spawn(handle_client(sche, s_fd, c_fd));
  }
  close(s_fd);
}

int main(int argc, char *argv[]) {
  if (argc > 2) {
    PRINTLN_ERROR("Usage: {} [port]", argv[0]);
    return EXIT_FAILURE;
  }

  int port = 6130;
  if (argc > 1) {
    size_t pos = -1;
    port = std::stoi(argv[1], &pos);
    if (argv[1][pos] != '\0') {
      PRINTLN_ERROR("port \"{}\" is not a integer", argv[0]);
      return EXIT_FAILURE;
    }
  }

  int s_fd = get_listen_socket(port);
  if (s_fd == -1) {
    PRINTLN_ERROR_EXIT("error getting listening socket");
  }

  scheduler sche;
  sche.co_spawn(server(sche, s_fd));

  sche.run();

  return 0;
}