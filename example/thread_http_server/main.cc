#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t max_header_size = 16 * 1024;
constexpr std::size_t max_requests_per_connection = 100;
constexpr std::string_view benchmark_body = "OK\n";
constexpr std::string_view not_found_body = "Not Found\n";

volatile sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int) { stop_requested = 1; }

class unique_fd {
public:
  unique_fd() = default;
  explicit unique_fd(int fd) noexcept : fd_(fd) {}

  unique_fd(const unique_fd &) = delete;
  unique_fd &operator=(const unique_fd &) = delete;

  unique_fd(unique_fd &&other) noexcept
      : fd_(std::exchange(other.fd_, -1)) {}

  unique_fd &operator=(unique_fd &&other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  ~unique_fd() { reset(); }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = fd;
  }

private:
  int fd_ = -1;
};

class connection_queue {
public:
  [[nodiscard]] bool push(int fd) {
    {
      std::lock_guard lock{mutex_};
      if (stopping_)
        return false;
      connections_.push_back(fd);
    }
    ready_.notify_one();
    return true;
  }

  [[nodiscard]] std::optional<int> pop() {
    std::unique_lock lock{mutex_};
    ready_.wait(lock,
                [this] { return stopping_ || !connections_.empty(); });
    if (connections_.empty())
      return std::nullopt;

    const int fd = connections_.front();
    connections_.pop_front();
    return fd;
  }

  void stop() {
    std::deque<int> abandoned;
    {
      std::lock_guard lock{mutex_};
      stopping_ = true;
      abandoned.swap(connections_);
    }
    for (const int fd : abandoned)
      ::close(fd);
    ready_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<int> connections_;
  bool stopping_ = false;
};

struct request {
  std::string_view target;
  bool head_only = false;
  bool keep_alive = true;
};

[[nodiscard]] std::optional<unsigned short>
parse_port(std::string_view text) {
  unsigned value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() ||
      value > std::numeric_limits<unsigned short>::max())
    return std::nullopt;
  return static_cast<unsigned short>(value);
}

[[nodiscard]] std::optional<std::size_t>
parse_worker_count(std::string_view text) {
  std::size_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() ||
      value == 0 || value > 4096)
    return std::nullopt;
  return value;
}

[[nodiscard]] std::string lowercase(std::string_view text) {
  std::string result{text};
  for (char &value : result) {
    if (value >= 'A' && value <= 'Z')
      value = static_cast<char>(value - 'A' + 'a');
  }
  return result;
}

[[nodiscard]] std::optional<request>
parse_request(std::string_view header) {
  const std::size_t first_line_end = header.find("\r\n");
  if (first_line_end == std::string_view::npos)
    return std::nullopt;

  const std::string_view first_line = header.substr(0, first_line_end);
  const std::size_t method_end = first_line.find(' ');
  if (method_end == std::string_view::npos)
    return std::nullopt;
  const std::size_t target_end = first_line.find(' ', method_end + 1);
  if (target_end == std::string_view::npos)
    return std::nullopt;

  const std::string_view method = first_line.substr(0, method_end);
  const std::string_view target =
      first_line.substr(method_end + 1, target_end - method_end - 1);
  const std::string_view version = first_line.substr(target_end + 1);
  if ((method != "GET" && method != "HEAD") || target.empty() ||
      (version != "HTTP/1.1" && version != "HTTP/1.0"))
    return std::nullopt;

  const std::string normalized = lowercase(header);
  bool keep_alive = version == "HTTP/1.1";
  if (normalized.find("\r\nconnection: close") != std::string::npos)
    keep_alive = false;
  else if (normalized.find("\r\nconnection: keep-alive") !=
           std::string::npos)
    keep_alive = true;

  return request{
      .target = target,
      .head_only = method == "HEAD",
      .keep_alive = keep_alive,
  };
}

[[nodiscard]] std::string build_response_header(
    int status, std::string_view reason, std::string_view content_type,
    std::size_t content_length, bool keep_alive) {
  std::string header;
  header.reserve(384);
  header += "HTTP/1.1 ";
  header += std::to_string(status);
  header += ' ';
  header += reason;
  header += "\r\nServer: co_mira\r\nContent-Type: ";
  header += content_type;
  header += "\r\nContent-Length: ";
  header += std::to_string(content_length);
  header += "\r\nConnection: ";
  header += keep_alive ? "keep-alive" : "close";
  header +=
      "\r\nCache-Control: no-store"
      "\r\nX-Content-Type-Options: nosniff"
      "\r\nReferrer-Policy: no-referrer"
      "\r\nContent-Security-Policy: default-src 'self'; "
      "script-src 'self'; style-src 'self'; connect-src 'self'"
      "\r\n\r\n";
  return header;
}

[[nodiscard]] bool send_all(int fd, std::string_view data) {
  while (!data.empty()) {
    const ssize_t sent =
        ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (sent > 0) {
      data.remove_prefix(static_cast<std::size_t>(sent));
      continue;
    }
    if (sent < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

void handle_connection(unique_fd client) {
  std::string pending;
  pending.reserve(4096);
  char receive_buffer[4096];
  std::size_t request_count = 0;

  while (!stop_requested) {
    std::size_t header_end = pending.find("\r\n\r\n");
    while (header_end == std::string::npos) {
      if (pending.size() >= max_header_size)
        return;

      const ssize_t received =
          ::recv(client.get(), receive_buffer, sizeof(receive_buffer), 0);
      if (received > 0) {
        pending.append(receive_buffer,
                       static_cast<std::size_t>(received));
        header_end = pending.find("\r\n\r\n");
        continue;
      }
      if (received < 0 && errno == EINTR)
        continue;
      return;
    }

    const std::size_t consumed = header_end + 4;
    if (consumed > max_header_size)
      return;

    const std::optional<request> parsed =
        parse_request(std::string_view{pending.data(), consumed});
    if (!parsed)
      return;

    ++request_count;
    const bool keep_alive =
        parsed->keep_alive &&
        request_count < max_requests_per_connection;

    const bool benchmark = parsed->target == "/benchmark";
    const int status = benchmark ? 200 : 404;
    const std::string_view reason = benchmark ? "OK" : "Not Found";
    const std::string_view body =
        benchmark ? benchmark_body : not_found_body;
    const std::string header = build_response_header(
        status, reason, "text/plain; charset=utf-8", body.size(),
        keep_alive);

    if (!send_all(client.get(), header))
      return;
    if (!parsed->head_only && !send_all(client.get(), body))
      return;

    pending.erase(0, consumed);
    if (!keep_alive)
      return;
  }
}

void worker_loop(connection_queue &queue) {
  while (const std::optional<int> fd = queue.pop())
    handle_connection(unique_fd{*fd});
}

[[nodiscard]] unique_fd create_listener(unsigned short port,
                                        unsigned short &bound_port) {
  unique_fd listener{
      ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP)};
  if (!listener)
    return {};

  int enabled = 1;
  if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled)) < 0)
    return {};

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (::bind(listener.get(), reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) < 0 ||
      ::listen(listener.get(), SOMAXCONN) < 0)
    return {};

  socklen_t address_size = sizeof(address);
  if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&address),
                    &address_size) < 0)
    return {};
  bound_port = ntohs(address.sin_port);
  return listener;
}

void install_signal_handlers() {
  struct sigaction action {};
  action.sa_handler = request_stop;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  ::sigaction(SIGINT, &action, nullptr);
  ::sigaction(SIGTERM, &action, nullptr);

  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  ::sigaction(SIGPIPE, &ignore, nullptr);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2 || argc > 3) {
    std::cerr << "usage: thread_http_server <port> [threads]\n";
    return EXIT_FAILURE;
  }

  const std::optional<unsigned short> port = parse_port(argv[1]);
  const std::optional<std::size_t> worker_count =
      argc == 3 ? parse_worker_count(argv[2])
                : std::optional<std::size_t>{std::thread::hardware_concurrency()};
  if (!port || !worker_count || *worker_count == 0) {
    std::cerr << "invalid port or thread count\n";
    return EXIT_FAILURE;
  }

  install_signal_handlers();

  unsigned short bound_port = 0;
  unique_fd listener = create_listener(*port, bound_port);
  if (!listener) {
    std::cerr << "failed to create listener: " << std::strerror(errno)
              << '\n';
    return EXIT_FAILURE;
  }

  connection_queue queue;
  std::vector<std::thread> workers;
  workers.reserve(*worker_count);
  for (std::size_t index = 0; index < *worker_count; ++index)
    workers.emplace_back(worker_loop, std::ref(queue));

  std::cout << "blocking thread HTTP server listening on http://0.0.0.0:"
            << bound_port << " with " << *worker_count << " thread(s)\n";
  std::cout.flush();

  while (!stop_requested) {
    pollfd event{
        .fd = listener.get(),
        .events = POLLIN,
        .revents = 0,
    };
    const int poll_result = ::poll(&event, 1, 200);
    if (poll_result < 0) {
      if (errno == EINTR)
        continue;
      std::cerr << "poll failed: " << std::strerror(errno) << '\n';
      break;
    }
    if (poll_result == 0)
      continue;
    if ((event.revents & POLLIN) == 0)
      continue;

    const int accepted =
        ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC);
    if (accepted < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED)
        continue;
      std::cerr << "accept failed: " << std::strerror(errno) << '\n';
      continue;
    }
    int no_delay = 1;
    if (::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                     sizeof(no_delay)) < 0) {
      ::close(accepted);
      continue;
    }
    if (!queue.push(accepted))
      ::close(accepted);
  }

  listener.reset();
  queue.stop();
  for (std::thread &worker : workers)
    worker.join();
}
