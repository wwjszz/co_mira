#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t max_header_size = 16 * 1024;
constexpr std::size_t receive_buffer_size = 8 * 1024;
constexpr std::size_t max_requests_per_connection = 100;
constexpr int max_epoll_events = 256;

volatile sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int) { stop_requested = 1; }

[[noreturn]] void throw_system_error(std::string_view operation) {
  throw std::system_error(errno, std::generic_category(),
                          std::string(operation));
}

class unique_fd {
public:
  unique_fd() = default;
  explicit unique_fd(int fd) noexcept : fd_(fd) {}

  unique_fd(const unique_fd &) = delete;
  unique_fd &operator=(const unique_fd &) = delete;

  unique_fd(unique_fd &&other) noexcept
      : fd_(std::exchange(other.fd_, -1)) {}

  unique_fd &operator=(unique_fd &&other) noexcept {
    if (this != &other)
      this->reset(std::exchange(other.fd_, -1));
    return *this;
  }

  ~unique_fd() { this->reset(); }

  [[nodiscard]] int get() const noexcept { return this->fd_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return this->fd_ >= 0;
  }

  void reset(int fd = -1) noexcept {
    if (this->fd_ >= 0)
      ::close(this->fd_);
    this->fd_ = fd;
  }

private:
  int fd_ = -1;
};

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() &&
         (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t'))
    text.remove_suffix(1);
  return text;
}

[[nodiscard]] std::string lower_copy(std::string_view text) {
  std::string result{text};
  for (char &character : result) {
    if (character >= 'A' && character <= 'Z')
      character = static_cast<char>(character - 'A' + 'a');
  }
  return result;
}

struct http_request {
  std::string method;
  std::string target;
  bool keep_alive = true;
  bool head_only = false;
};

[[nodiscard]] std::optional<std::size_t>
parse_decimal(std::string_view text) noexcept {
  std::size_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} ||
      result.ptr != text.data() + text.size())
    return std::nullopt;
  return value;
}

[[nodiscard]] std::optional<http_request>
parse_request(std::string_view header) {
  const std::size_t first_line_end = header.find("\r\n");
  if (first_line_end == std::string_view::npos)
    return std::nullopt;

  const std::string_view request_line = header.substr(0, first_line_end);
  const std::size_t first_space = request_line.find(' ');
  const std::size_t second_space =
      first_space == std::string_view::npos
          ? std::string_view::npos
          : request_line.find(' ', first_space + 1);
  if (first_space == std::string_view::npos ||
      second_space == std::string_view::npos ||
      request_line.find(' ', second_space + 1) !=
          std::string_view::npos)
    return std::nullopt;

  const std::string_view method = request_line.substr(0, first_space);
  std::string_view target = request_line.substr(
      first_space + 1, second_space - first_space - 1);
  const std::string_view version = request_line.substr(second_space + 1);
  if (method.empty() || target.empty() || target.front() != '/' ||
      (version != "HTTP/1.1" && version != "HTTP/1.0"))
    return std::nullopt;

  if (const std::size_t query = target.find('?');
      query != std::string_view::npos)
    target = target.substr(0, query);

  http_request request{
      .method = std::string(method),
      .target = std::string(target),
      .keep_alive = version == "HTTP/1.1",
      .head_only = method == "HEAD",
  };

  std::size_t cursor = first_line_end + 2;
  while (cursor < header.size()) {
    const std::size_t line_end = header.find("\r\n", cursor);
    if (line_end == std::string_view::npos)
      return std::nullopt;
    if (line_end == cursor)
      break;

    const std::string_view line = header.substr(cursor, line_end - cursor);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos)
      return std::nullopt;

    const std::string name = lower_copy(trim(line.substr(0, colon)));
    const std::string value = lower_copy(trim(line.substr(colon + 1)));
    if (name == "connection") {
      if (value.find("close") != std::string::npos)
        request.keep_alive = false;
      else if (value.find("keep-alive") != std::string::npos)
        request.keep_alive = true;
    } else if (name == "content-length") {
      const std::optional<std::size_t> content_length =
          parse_decimal(value);
      if (!content_length || *content_length != 0)
        return std::nullopt;
    } else if (name == "transfer-encoding") {
      return std::nullopt;
    }

    cursor = line_end + 2;
  }

  return request;
}

struct http_response {
  int status = 200;
  std::string reason = "OK";
  std::string content_type = "text/plain; charset=utf-8";
  std::shared_ptr<const std::string> body;
  std::string download_name;
  std::string cache_control = "no-store";
};

[[nodiscard]] std::shared_ptr<const std::string>
make_body(std::string body) {
  return std::make_shared<const std::string>(std::move(body));
}

[[nodiscard]] http_response text_response(int status, std::string reason,
                                          std::string body) {
  return {
      .status = status,
      .reason = std::move(reason),
      .content_type = "text/plain; charset=utf-8",
      .body = make_body(std::move(body)),
      .download_name = {},
      .cache_control = "no-store",
  };
}

[[nodiscard]] http_response route_request(const http_request &request) {
  if (request.method != "GET" && request.method != "HEAD") {
    return text_response(405, "Method Not Allowed",
                         "Only GET and HEAD are supported.\n");
  }

  if (request.target == "/benchmark") {
    static const auto body = make_body("OK\n");
    return {
        .status = 200,
        .reason = "OK",
        .content_type = "text/plain; charset=utf-8",
        .body = body,
        .download_name = {},
        .cache_control = "no-store",
    };
  }

  return {
      .status = 404,
      .reason = "Not Found",
      .content_type = "text/html; charset=utf-8",
      .body = make_body(
          "<!doctype html><html><body><h1>404</h1>"
          "<p>The requested resource does not exist.</p>"
          "<p><a href=\"/\">Return home</a></p></body></html>\n"),
      .download_name = {},
      .cache_control = "no-store",
  };
}

[[nodiscard]] std::string
build_response_header(const http_response &response, bool keep_alive) {
  const std::size_t content_length =
      response.body ? response.body->size() : 0;

  std::string header;
  header.reserve(384);
  header += "HTTP/1.1 ";
  header += std::to_string(response.status);
  header += ' ';
  header += response.reason;
  header += "\r\nServer: co_mira\r\nContent-Type: ";
  header += response.content_type;
  header += "\r\nContent-Length: ";
  header += std::to_string(content_length);
  header += "\r\nConnection: ";
  header += keep_alive ? "keep-alive" : "close";
  header += "\r\nCache-Control: ";
  header += response.cache_control;
  header +=
      "\r\nX-Content-Type-Options: nosniff"
      "\r\nReferrer-Policy: no-referrer"
      "\r\nContent-Security-Policy: default-src 'self'; "
      "script-src 'self'; style-src 'self'; connect-src 'self'";
  if (!response.download_name.empty()) {
    header += "\r\nContent-Disposition: attachment; filename=\"";
    header += response.download_name;
    header += '"';
  }
  if (response.status == 405)
    header += "\r\nAllow: GET, HEAD";
  header += "\r\n\r\n";
  return header;
}

enum class output_phase : uint8_t {
  header,
  body,
};

class connection {
public:
  explicit connection(int fd) noexcept : fd_(fd) {
    this->pending_.reserve(receive_buffer_size);
  }

  [[nodiscard]] int fd() const noexcept { return this->fd_.get(); }
  [[nodiscard]] bool wants_write() const noexcept {
    return this->output_.has_value();
  }

  [[nodiscard]] bool handle(uint32_t events) {
    if ((events & EPOLLERR) != 0)
      return false;

    if ((events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) {
      if (!this->receive_input())
        return false;
    }

    if (!this->output_)
      this->prepare_next_response();

    if (this->output_ && !this->flush_output())
      return false;

    if (this->peer_closed_ && !this->output_)
      return false;
    return true;
  }

private:
  struct output_state {
    http_response response;
    std::string header;
    output_phase phase = output_phase::header;
    std::size_t sent = 0;
    std::size_t consumed = 0;
    bool keep_alive = false;
    bool head_only = false;
  };

  [[nodiscard]] bool receive_input() {
    while (!this->peer_closed_) {
      const ssize_t received =
          ::recv(this->fd_.get(), this->receive_buffer_.data(),
                 this->receive_buffer_.size(), 0);
      if (received > 0) {
        this->pending_.append(this->receive_buffer_.data(),
                              static_cast<std::size_t>(received));
        if (this->pending_.size() >= max_header_size)
          break;
        continue;
      }
      if (received == 0) {
        this->peer_closed_ = true;
        break;
      }
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      return false;
    }
    return true;
  }

  void prepare_next_response() {
    const std::size_t header_end = this->pending_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      if (this->pending_.size() >= max_header_size) {
        this->prepare_response(
            text_response(431, "Request Header Fields Too Large",
                          "Request headers exceed 16 KiB.\n"),
            false, false, 0);
      }
      return;
    }

    const std::size_t consumed = header_end + 4;
    if (consumed > max_header_size) {
      this->prepare_response(
          text_response(431, "Request Header Fields Too Large",
                        "Request headers exceed 16 KiB.\n"),
          false, false, 0);
      return;
    }

    const std::optional<http_request> request = parse_request(
        std::string_view{this->pending_.data(), consumed});
    if (!request) {
      this->prepare_response(
          text_response(400, "Bad Request", "Malformed HTTP request.\n"),
          false, false, 0);
      return;
    }

    ++this->request_count_;
    const bool keep_alive =
        request->keep_alive && !this->peer_closed_ &&
        this->request_count_ < max_requests_per_connection;
    this->prepare_response(route_request(*request), keep_alive,
                           request->head_only, consumed);
  }

  void prepare_response(http_response response, bool keep_alive,
                        bool head_only, std::size_t consumed) {
    std::string header = build_response_header(response, keep_alive);
    this->output_.emplace(output_state{
        .response = std::move(response),
        .header = std::move(header),
        .phase = output_phase::header,
        .sent = 0,
        .consumed = consumed,
        .keep_alive = keep_alive,
        .head_only = head_only,
    });
  }

  [[nodiscard]] bool flush_output() {
    while (this->output_) {
      output_state &output = *this->output_;
      const std::string_view current =
          output.phase == output_phase::header
              ? std::string_view{output.header}
              : std::string_view{*output.response.body};

      while (output.sent < current.size()) {
        const ssize_t sent =
            ::send(this->fd_.get(), current.data() + output.sent,
                   current.size() - output.sent, MSG_NOSIGNAL);
        if (sent > 0) {
          output.sent += static_cast<std::size_t>(sent);
          continue;
        }
        if (sent < 0 && errno == EINTR)
          continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
          return true;
        return false;
      }

      if (output.phase == output_phase::header && !output.head_only &&
          output.response.body && !output.response.body->empty()) {
        output.phase = output_phase::body;
        output.sent = 0;
        continue;
      }

      const bool keep_alive = output.keep_alive;
      const std::size_t consumed = output.consumed;
      this->output_.reset();
      if (consumed != 0)
        this->pending_.erase(0, consumed);
      if (!keep_alive)
        return false;

      this->prepare_next_response();
    }
    return true;
  }

  unique_fd fd_;
  std::array<char, receive_buffer_size> receive_buffer_{};
  std::string pending_;
  std::optional<output_state> output_;
  std::size_t request_count_ = 0;
  bool peer_closed_ = false;
};

class worker {
public:
  worker()
      : epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)),
        wake_fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    if (!this->epoll_fd_ || !this->wake_fd_)
      throw_system_error("epoll worker setup");

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.ptr = nullptr;
    if (::epoll_ctl(this->epoll_fd_.get(), EPOLL_CTL_ADD,
                    this->wake_fd_.get(), &event) != 0)
      throw_system_error("epoll_ctl(eventfd)");

    this->thread_ = std::thread([this] { this->run(); });
  }

  worker(const worker &) = delete;
  worker &operator=(const worker &) = delete;

  ~worker() {
    if (this->thread_.joinable()) {
      this->stop();
      this->thread_.join();
    }
  }

  [[nodiscard]] bool enqueue(int fd) {
    {
      std::lock_guard lock{this->pending_mutex_};
      if (this->stopping_.load(std::memory_order_relaxed))
        return false;
      this->pending_fds_.push_back(fd);
    }
    this->wake();
    return true;
  }

  void stop() noexcept {
    this->stopping_.store(true, std::memory_order_relaxed);
    this->wake();
  }

  void join() {
    this->thread_.join();
    if (this->exception_)
      std::rethrow_exception(this->exception_);
  }

private:
  void wake() noexcept {
    const uint64_t increment = 1;
    ssize_t result;
    do {
      result =
          ::write(this->wake_fd_.get(), &increment, sizeof(increment));
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EAGAIN)
      std::terminate();
  }

  void run() noexcept {
    try {
      std::array<epoll_event, max_epoll_events> events{};
      while (true) {
        int count;
        do {
          count = ::epoll_wait(this->epoll_fd_.get(), events.data(),
                               static_cast<int>(events.size()), -1);
        } while (count < 0 && errno == EINTR);
        if (count < 0)
          throw_system_error("epoll_wait(worker)");

        bool woke = false;
        for (int index = 0; index < count; ++index) {
          if (events[index].data.ptr == nullptr) {
            woke = true;
            continue;
          }

          auto *current =
              static_cast<connection *>(events[index].data.ptr);
          const int fd = current->fd();
          if (!current->handle(events[index].events)) {
            this->remove_connection(fd);
            continue;
          }
          this->update_interest(*current);
        }

        if (woke)
          this->drain_wake_fd();
        this->adopt_pending_connections();
        if (this->stopping_.load(std::memory_order_relaxed))
          break;
      }
    } catch (...) {
      this->exception_ = std::current_exception();
    }

    this->close_pending_connections();
    this->connections_.clear();
  }

  void drain_wake_fd() noexcept {
    uint64_t value = 0;
    while (::read(this->wake_fd_.get(), &value, sizeof(value)) > 0) {
    }
  }

  void adopt_pending_connections() {
    std::deque<int> pending;
    {
      std::lock_guard lock{this->pending_mutex_};
      pending.swap(this->pending_fds_);
    }

    if (this->stopping_.load(std::memory_order_relaxed)) {
      for (const int fd : pending)
        ::close(fd);
      return;
    }

    for (const int fd : pending) {
      auto current = std::make_unique<connection>(fd);
      epoll_event event{};
      event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
      event.data.ptr = current.get();
      if (::epoll_ctl(this->epoll_fd_.get(), EPOLL_CTL_ADD, fd,
                      &event) != 0)
        continue;
      this->connections_.emplace(fd, std::move(current));
    }
  }

  void update_interest(connection &current) {
    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    if (current.wants_write())
      event.events |= EPOLLOUT;
    event.data.ptr = &current;
    if (::epoll_ctl(this->epoll_fd_.get(), EPOLL_CTL_MOD, current.fd(),
                    &event) != 0)
      this->remove_connection(current.fd());
  }

  void remove_connection(int fd) noexcept {
    ::epoll_ctl(this->epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr);
    this->connections_.erase(fd);
  }

  void close_pending_connections() noexcept {
    std::deque<int> pending;
    {
      std::lock_guard lock{this->pending_mutex_};
      pending.swap(this->pending_fds_);
    }
    for (const int fd : pending)
      ::close(fd);
  }

  unique_fd epoll_fd_;
  unique_fd wake_fd_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex pending_mutex_;
  std::deque<int> pending_fds_;
  std::unordered_map<int, std::unique_ptr<connection>> connections_;
  std::exception_ptr exception_;
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
      value == 0 || value > 256)
    return std::nullopt;
  return value;
}

[[nodiscard]] unique_fd create_listener(unsigned short port,
                                        unsigned short &bound_port) {
  unique_fd listener{::socket(AF_INET,
                              SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK,
                              IPPROTO_TCP)};
  if (!listener)
    throw_system_error("socket");

  int enabled = 1;
  if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enabled,
                   sizeof(enabled)) != 0)
    throw_system_error("setsockopt(SO_REUSEADDR)");

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port);
  if (::bind(listener.get(), reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) != 0)
    throw_system_error("bind");
  if (::listen(listener.get(), SOMAXCONN) != 0)
    throw_system_error("listen");

  socklen_t address_size = sizeof(address);
  if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&address),
                    &address_size) != 0)
    throw_system_error("getsockname");
  bound_port = ntohs(address.sin_port);
  return listener;
}

void install_signal_handlers() {
  struct sigaction action {};
  action.sa_handler = request_stop;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  if (::sigaction(SIGINT, &action, nullptr) != 0 ||
      ::sigaction(SIGTERM, &action, nullptr) != 0)
    throw_system_error("sigaction");

  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (::sigaction(SIGPIPE, &ignore, nullptr) != 0)
    throw_system_error("sigaction(SIGPIPE)");
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2 || argc > 3) {
      std::cerr << "usage: epoll_http_server <port> [workers]\n";
      return EXIT_FAILURE;
    }

    const std::optional<unsigned short> port = parse_port(argv[1]);
    const std::optional<std::size_t> worker_count =
        argc == 3 ? parse_worker_count(argv[2])
                  : std::optional<std::size_t>{1};
    if (!port || !worker_count) {
      std::cerr << "invalid port or worker count\n";
      return EXIT_FAILURE;
    }

    install_signal_handlers();
    unsigned short bound_port = 0;
    unique_fd listener = create_listener(*port, bound_port);
    unique_fd front_epoll{::epoll_create1(EPOLL_CLOEXEC)};
    if (!front_epoll)
      throw_system_error("epoll_create1(front)");

    epoll_event listener_event{};
    listener_event.events = EPOLLIN | EPOLLET;
    listener_event.data.fd = listener.get();
    if (::epoll_ctl(front_epoll.get(), EPOLL_CTL_ADD, listener.get(),
                    &listener_event) != 0)
      throw_system_error("epoll_ctl(listener)");

    std::vector<std::unique_ptr<worker>> workers;
    workers.reserve(*worker_count);
    for (std::size_t index = 0; index < *worker_count; ++index)
      workers.push_back(std::make_unique<worker>());

    std::cout << "epoll event-loop HTTP server listening on "
              << "http://0.0.0.0:" << bound_port << " with "
              << workers.size() << " worker(s)\n";
    std::cout.flush();

    std::size_t next_worker = 0;
    while (!stop_requested) {
      epoll_event event{};
      int count;
      do {
        count = ::epoll_wait(front_epoll.get(), &event, 1, 200);
      } while (count < 0 && errno == EINTR && !stop_requested);
      if (count < 0) {
        if (errno == EINTR && stop_requested)
          break;
        throw_system_error("epoll_wait(front)");
      }
      if (count == 0)
        continue;

      while (true) {
        const int accepted =
            ::accept4(listener.get(), nullptr, nullptr,
                      SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (accepted < 0) {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          if (errno == ECONNABORTED)
            continue;
          throw_system_error("accept4");
        }

        const int enabled = 1;
        if (::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &enabled,
                         sizeof(enabled)) != 0 ||
            !workers[next_worker++ % workers.size()]->enqueue(accepted))
          ::close(accepted);
      }
    }

    listener.reset();
    for (auto &current : workers)
      current->stop();

    std::exception_ptr first_exception;
    for (auto &current : workers) {
      try {
        current->join();
      } catch (...) {
        if (!first_exception)
          first_exception = std::current_exception();
      }
    }
    if (first_exception)
      std::rethrow_exception(first_exception);
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "epoll_http_server: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
