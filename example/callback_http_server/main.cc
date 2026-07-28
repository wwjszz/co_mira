#include <liburing.h>

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr unsigned ring_entries = 256;
constexpr std::size_t max_header_size = 16 * 1024;
constexpr std::size_t receive_buffer_size = 8 * 1024;
constexpr std::size_t max_requests_per_connection = 100;

constexpr uint64_t front_accept_tag = std::numeric_limits<uint64_t>::max();
constexpr uint64_t front_signal_tag = front_accept_tag - 1;
constexpr uint64_t front_stop_ack_tag = front_accept_tag - 2;
constexpr uint64_t dispatch_ack_prefix = 0xd150000000000000ULL;
constexpr uint64_t dispatch_ack_mask = 0xffff000000000000ULL;

constexpr uint64_t worker_new_connection_tag =
    std::numeric_limits<uint64_t>::max();
constexpr uint64_t worker_stop_tag = worker_new_connection_tag - 1;

struct server_metrics {
  std::atomic<uint64_t> accepted_connections{0};
  std::atomic<uint64_t> active_connections{0};
  std::atomic<uint64_t> requests{0};
};

[[noreturn]] void throw_system_error(int error, std::string_view operation) {
  throw std::system_error(error, std::generic_category(),
                          std::string(operation));
}

void check_uring(int result, std::string_view operation) {
  if (result < 0)
    throw_system_error(-result, operation);
}

void submit_pending(io_uring &ring) {
  while (io_uring_sq_ready(&ring) != 0) {
    int result;
    do {
      result = io_uring_submit(&ring);
    } while (result == -EINTR);

    check_uring(result, "io_uring_submit");
    if (result == 0)
      throw std::runtime_error(
          "io_uring_submit returned 0 with pending SQEs");
  }
}

void wait_for_completion(io_uring &ring, std::string_view operation) {
  int result;
  if (io_uring_sq_ready(&ring) != 0) {
    do {
      result = io_uring_submit_and_wait(&ring, 1);
    } while (result == -EINTR);
  } else {
    io_uring_cqe *cqe = nullptr;
    do {
      result = io_uring_wait_cqe(&ring, &cqe);
    } while (result == -EINTR);
  }
  check_uring(result, operation);
}

[[nodiscard]] io_uring_sqe *get_sqe(io_uring &ring) {
  if (io_uring_sqe *sqe = io_uring_get_sqe(&ring))
    return sqe;

  submit_pending(ring);
  if (io_uring_sqe *sqe = io_uring_get_sqe(&ring))
    return sqe;

  throw std::runtime_error("io_uring SQ is exhausted");
}

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
  std::string result;
  result.reserve(text.size());
  for (const unsigned char character : text) {
    if (character >= 'A' && character <= 'Z')
      result.push_back(static_cast<char>(character - 'A' + 'a'));
    else
      result.push_back(static_cast<char>(character));
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

// Kept byte-for-byte equivalent to the coroutine HTTP server parser.
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
    const std::string value =
        lower_copy(trim(line.substr(colon + 1)));
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

// Kept byte-for-byte equivalent to the coroutine HTTP response builder.
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

class worker;

enum class connection_operation : uintptr_t {
  receive = 1,
  send = 2,
};

constexpr uintptr_t connection_operation_mask = 0x7;

struct alignas(8) connection_state {
  connection_state(worker &owner, server_metrics &metrics, int fd);
  ~connection_state();

  connection_state(const connection_state &) = delete;
  connection_state &operator=(const connection_state &) = delete;

  void start();
  void complete(connection_operation operation, int32_t result);

private:
  void prepare_receive();
  void process_pending();
  void prepare_error(http_response response);
  void prepare_response(http_response response, bool keep_alive,
                        bool head_only, std::size_t consumed);
  void prepare_send();
  void complete_receive(int32_t result);
  void complete_send(int32_t result);
  void finish_response();
  void close();

  worker *owner_;
  server_metrics *metrics_;
  int fd_;
  std::array<char, receive_buffer_size> receive_buffer_{};
  std::string pending_;
  std::size_t request_count_ = 0;

  http_response response_;
  std::string response_header_;
  std::array<iovec, 2> send_iovecs_{};
  msghdr send_message_{};
  std::size_t header_sent_ = 0;
  std::size_t body_sent_ = 0;
  std::size_t consumed_ = 0;
  bool keep_alive_ = false;
  bool head_only_ = false;
};

static_assert(alignof(connection_state) >= 8);

[[nodiscard]] uint64_t
connection_user_data(connection_state *connection,
                     connection_operation operation) noexcept {
  return reinterpret_cast<uintptr_t>(connection) |
         static_cast<uintptr_t>(operation);
}

[[nodiscard]] connection_state *
connection_from_user_data(uint64_t data) noexcept {
  return reinterpret_cast<connection_state *>(
      data & ~connection_operation_mask);
}

[[nodiscard]] connection_operation
operation_from_user_data(uint64_t data) noexcept {
  return static_cast<connection_operation>(
      data & connection_operation_mask);
}

class worker {
public:
  explicit worker(server_metrics &metrics) : metrics_(&metrics) {
    check_uring(io_uring_queue_init(ring_entries, &this->ring_, 0),
                "io_uring_queue_init(worker)");
    this->initialized_ = true;

    try {
      this->thread_ = std::thread([this] { this->run(); });
    } catch (...) {
      io_uring_queue_exit(&this->ring_);
      this->initialized_ = false;
      throw;
    }
  }

  ~worker() {
    if (this->thread_.joinable())
      std::terminate();
    if (this->initialized_)
      io_uring_queue_exit(&this->ring_);
  }

  worker(const worker &) = delete;
  worker &operator=(const worker &) = delete;

  [[nodiscard]] int ring_fd() const noexcept {
    return this->ring_.ring_fd;
  }

  void join() {
    this->thread_.join();
    if (this->exception_)
      std::rethrow_exception(this->exception_);
  }

private:
  friend struct connection_state;

  [[nodiscard]] io_uring_sqe *get_sqe() {
    return ::get_sqe(this->ring_);
  }

  void run() noexcept {
    try {
      bool stopping = false;
      while (!stopping) {
        wait_for_completion(this->ring_, "worker completion wait");

        unsigned head = 0;
        unsigned consumed = 0;
        io_uring_cqe *cqe = nullptr;
        io_uring_for_each_cqe(&this->ring_, head, cqe) {
          ++consumed;
          const uint64_t data = cqe->user_data;

          if (data == worker_stop_tag) {
            stopping = true;
            continue;
          }

          if (data == worker_new_connection_tag) {
            if (cqe->res < 0)
              continue;
            if (stopping) {
              ::close(cqe->res);
              continue;
            }
            this->create_connection(cqe->res);
            continue;
          }

          if (stopping)
            continue;

          const connection_operation operation =
              operation_from_user_data(data);
          if (operation != connection_operation::receive &&
              operation != connection_operation::send)
            throw std::runtime_error(
                "unknown callback connection operation");

          connection_state *connection =
              connection_from_user_data(data);
          assert(this->connections_.contains(connection));
          connection->complete(operation, cqe->res);
        }

        io_uring_cq_advance(&this->ring_, consumed);
      }
    } catch (...) {
      this->exception_ = std::current_exception();
    }

    this->close_all_connections();
  }

  void create_connection(int fd) {
    auto connection =
        std::make_unique<connection_state>(*this, *this->metrics_, fd);
    connection_state *pointer = connection.get();
    this->connections_.insert(pointer);
    (void)connection.release();

    try {
      pointer->start();
    } catch (...) {
      this->connections_.erase(pointer);
      delete pointer;
      throw;
    }
  }

  void destroy_connection(connection_state *connection) noexcept {
    const std::size_t erased = this->connections_.erase(connection);
    if (erased != 1)
      std::terminate();
    delete connection;
  }

  void close_all_connections() noexcept {
    for (connection_state *connection : this->connections_)
      delete connection;
    this->connections_.clear();
  }

  io_uring ring_{};
  bool initialized_ = false;
  server_metrics *metrics_;
  std::thread thread_;
  std::unordered_set<connection_state *> connections_;
  std::exception_ptr exception_;
};

connection_state::connection_state(worker &owner, server_metrics &metrics,
                                   int fd)
    : owner_(&owner), metrics_(&metrics), fd_(fd) {
  this->pending_.reserve(receive_buffer_size);
  this->metrics_->active_connections.fetch_add(
      1, std::memory_order_relaxed);
}

connection_state::~connection_state() {
  if (this->fd_ >= 0)
    ::close(this->fd_);
  this->metrics_->active_connections.fetch_sub(
      1, std::memory_order_relaxed);
}

void connection_state::start() { this->prepare_receive(); }

void connection_state::complete(connection_operation operation,
                                int32_t result) {
  if (operation == connection_operation::receive)
    this->complete_receive(result);
  else
    this->complete_send(result);
}

void connection_state::prepare_receive() {
  io_uring_sqe *sqe = this->owner_->get_sqe();
  io_uring_prep_recv(sqe, this->fd_, this->receive_buffer_.data(),
                     this->receive_buffer_.size(), 0);
  sqe->user_data =
      connection_user_data(this, connection_operation::receive);
}

void connection_state::process_pending() {
  const std::size_t header_end = this->pending_.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    if (this->pending_.size() >= max_header_size) {
      this->prepare_error(text_response(
          431, "Request Header Fields Too Large",
          "Request headers exceed 16 KiB.\n"));
      return;
    }

    this->prepare_receive();
    return;
  }

  const std::size_t consumed = header_end + 4;
  if (consumed > max_header_size) {
    this->prepare_error(text_response(
        431, "Request Header Fields Too Large",
        "Request headers exceed 16 KiB.\n"));
    return;
  }

  const std::optional<http_request> request = parse_request(
      std::string_view{this->pending_.data(), consumed});
  if (!request) {
    this->prepare_error(
        text_response(400, "Bad Request", "Malformed HTTP request.\n"));
    return;
  }

  ++this->request_count_;
  this->metrics_->requests.fetch_add(1, std::memory_order_relaxed);
  const bool keep_alive =
      request->keep_alive &&
      this->request_count_ < max_requests_per_connection;
  this->prepare_response(route_request(*request), keep_alive,
                         request->head_only, consumed);
}

void connection_state::prepare_error(http_response response) {
  this->prepare_response(std::move(response), false, false, 0);
}

void connection_state::prepare_response(http_response response,
                                        bool keep_alive, bool head_only,
                                        std::size_t consumed) {
  this->response_ = std::move(response);
  this->response_header_ =
      build_response_header(this->response_, keep_alive);
  this->header_sent_ = 0;
  this->body_sent_ = 0;
  this->consumed_ = consumed;
  this->keep_alive_ = keep_alive;
  this->head_only_ = head_only;
  this->prepare_send();
}

void connection_state::prepare_send() {
  std::size_t count = 0;
  if (this->header_sent_ != this->response_header_.size()) {
    this->send_iovecs_[count++] = {
        .iov_base = const_cast<char *>(this->response_header_.data() +
                                      this->header_sent_),
        .iov_len = this->response_header_.size() - this->header_sent_,
    };
  }

  const std::size_t body_size =
      !this->head_only_ && this->response_.body
          ? this->response_.body->size()
          : 0;
  if (this->body_sent_ != body_size) {
    this->send_iovecs_[count++] = {
        .iov_base = const_cast<char *>(this->response_.body->data() +
                                      this->body_sent_),
        .iov_len = body_size - this->body_sent_,
    };
  }
  assert(count != 0);

  this->send_message_ = {};
  this->send_message_.msg_iov = this->send_iovecs_.data();
  this->send_message_.msg_iovlen = count;
  io_uring_sqe *sqe = this->owner_->get_sqe();
  io_uring_prep_sendmsg(sqe, this->fd_, &this->send_message_,
                        MSG_NOSIGNAL);
  sqe->user_data =
      connection_user_data(this, connection_operation::send);
}

void connection_state::complete_receive(int32_t result) {
  if (result <= 0) {
    this->close();
    return;
  }

  this->pending_.append(this->receive_buffer_.data(),
                        static_cast<std::size_t>(result));
  this->process_pending();
}

void connection_state::complete_send(int32_t result) {
  if (result <= 0) {
    this->close();
    return;
  }

  std::size_t completed = static_cast<std::size_t>(result);
  const std::size_t header_remaining =
      this->response_header_.size() - this->header_sent_;
  const std::size_t header_completed =
      std::min(completed, header_remaining);
  this->header_sent_ += header_completed;
  completed -= header_completed;

  const std::size_t body_size =
      !this->head_only_ && this->response_.body
          ? this->response_.body->size()
          : 0;
  const std::size_t body_remaining = body_size - this->body_sent_;
  const std::size_t body_completed =
      std::min(completed, body_remaining);
  this->body_sent_ += body_completed;
  completed -= body_completed;
  if (completed != 0)
    throw std::runtime_error("sendmsg completed too many bytes");

  if (this->header_sent_ != this->response_header_.size() ||
      this->body_sent_ != body_size) {
    this->prepare_send();
    return;
  }

  this->finish_response();
}

void connection_state::finish_response() {
  if (this->consumed_ != 0)
    this->pending_.erase(0, this->consumed_);

  if (!this->keep_alive_) {
    this->close();
    return;
  }

  this->response_ = {};
  this->response_header_.clear();
  this->consumed_ = 0;
  this->process_pending();
}

void connection_state::close() {
  this->owner_->destroy_connection(this);
}

[[nodiscard]] uint64_t dispatch_ack_data(int fd) noexcept {
  return dispatch_ack_prefix | static_cast<uint32_t>(fd);
}

[[nodiscard]] bool is_dispatch_ack(uint64_t data) noexcept {
  return (data & dispatch_ack_mask) == dispatch_ack_prefix;
}

[[nodiscard]] int dispatch_fd(uint64_t data) noexcept {
  return static_cast<int>(static_cast<uint32_t>(data));
}

class callback_server {
public:
  callback_server(uint16_t port, std::size_t worker_count,
                  int signal_fd, server_metrics &metrics)
      : signal_fd_(signal_fd), metrics_(&metrics) {
    check_uring(io_uring_queue_init(ring_entries, &this->ring_, 0),
                "io_uring_queue_init(front)");
    this->initialized_ = true;

    this->listener_ = this->create_listener(port, this->bound_port_);

    this->workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index)
      this->workers_.push_back(std::make_unique<worker>(metrics));
  }

  ~callback_server() {
    if (this->listener_ >= 0)
      ::close(this->listener_);
    if (this->signal_fd_ >= 0)
      ::close(this->signal_fd_);
    if (this->initialized_)
      io_uring_queue_exit(&this->ring_);
  }

  callback_server(const callback_server &) = delete;
  callback_server &operator=(const callback_server &) = delete;

  [[nodiscard]] uint16_t bound_port() const noexcept {
    return this->bound_port_;
  }

  [[nodiscard]] std::size_t worker_count() const noexcept {
    return this->workers_.size();
  }

  void run() {
    this->prepare_accept();
    this->prepare_signal_read();

    bool stopping = false;
    while (!stopping) {
      wait_for_completion(this->ring_, "front completion wait");

      unsigned head = 0;
      unsigned consumed = 0;
      io_uring_cqe *cqe = nullptr;
      io_uring_for_each_cqe(&this->ring_, head, cqe) {
        ++consumed;
        const uint64_t data = cqe->user_data;

        if (data == front_accept_tag) {
          if (cqe->res >= 0)
            this->dispatch(cqe->res);
          else if (cqe->res != -ECANCELED)
            throw_system_error(-cqe->res, "IORING_OP_ACCEPT");

          if (!stopping)
            this->prepare_accept();
          continue;
        }

        if (data == front_signal_tag) {
          if (cqe->res < 0)
            throw_system_error(-cqe->res, "signalfd read");
          stopping = true;
          continue;
        }

        if (data == front_stop_ack_tag)
          continue;

        if (is_dispatch_ack(data)) {
          if (cqe->res < 0)
            ::close(dispatch_fd(data));
          continue;
        }

        throw std::runtime_error("unknown front-ring completion");
      }

      io_uring_cq_advance(&this->ring_, consumed);
    }

    if (this->listener_ >= 0)
      ::close(std::exchange(this->listener_, -1));

    for (const auto &worker : this->workers_) {
      io_uring_sqe *sqe = get_sqe(this->ring_);
      io_uring_prep_msg_ring(sqe, worker->ring_fd(), 0,
                             worker_stop_tag, 0);
      sqe->user_data = front_stop_ack_tag;
    }
    submit_pending(this->ring_);

    std::exception_ptr first_exception;
    for (auto &worker : this->workers_) {
      try {
        worker->join();
      } catch (...) {
        if (!first_exception)
          first_exception = std::current_exception();
      }
    }
    if (first_exception)
      std::rethrow_exception(first_exception);
  }

private:
  [[nodiscard]] static int create_listener(uint16_t port,
                                           uint16_t &bound_port) {
    const int fd =
        ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0)
      throw_system_error(errno, "socket");

    try {
      const int enabled = 1;
      if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                       sizeof(enabled)) != 0)
        throw_system_error(errno, "setsockopt(SO_REUSEADDR)");

      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_ANY);
      address.sin_port = htons(port);
      if (::bind(fd, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) != 0)
        throw_system_error(errno, "bind");
      if (::listen(fd, SOMAXCONN) != 0)
        throw_system_error(errno, "listen");

      socklen_t length = sizeof(address);
      if (::getsockname(fd, reinterpret_cast<sockaddr *>(&address),
                        &length) != 0)
        throw_system_error(errno, "getsockname");
      bound_port = ntohs(address.sin_port);
      return fd;
    } catch (...) {
      ::close(fd);
      throw;
    }
  }

  void prepare_accept() {
    io_uring_sqe *sqe = get_sqe(this->ring_);
    io_uring_prep_accept(sqe, this->listener_, nullptr, nullptr,
                         SOCK_CLOEXEC);
    sqe->user_data = front_accept_tag;
  }

  void prepare_signal_read() {
    io_uring_sqe *sqe = get_sqe(this->ring_);
    io_uring_prep_read(sqe, this->signal_fd_, &this->signal_info_,
                       sizeof(this->signal_info_), 0);
    sqe->user_data = front_signal_tag;
  }

  void dispatch(int fd) {
    const int enabled = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                     sizeof(enabled)) != 0) {
      ::close(fd);
      return;
    }

    this->metrics_->accepted_connections.fetch_add(
        1, std::memory_order_relaxed);
    worker &target =
        *this->workers_[this->next_worker_++ % this->workers_.size()];
    io_uring_sqe *sqe = get_sqe(this->ring_);
    io_uring_prep_msg_ring(sqe, target.ring_fd(), fd,
                           worker_new_connection_tag, 0);
    sqe->user_data = dispatch_ack_data(fd);
  }

  io_uring ring_{};
  bool initialized_ = false;
  int listener_ = -1;
  int signal_fd_ = -1;
  uint16_t bound_port_ = 0;
  signalfd_siginfo signal_info_{};
  server_metrics *metrics_;
  std::vector<std::unique_ptr<worker>> workers_;
  std::size_t next_worker_ = 0;
};

[[nodiscard]] uint16_t parse_port(std::string_view text) {
  const std::optional<std::size_t> value = parse_decimal(text);
  if (!value || *value > 65535)
    throw std::invalid_argument("port must be in the range 0..65535");
  return static_cast<uint16_t>(*value);
}

[[nodiscard]] std::size_t parse_worker_count(std::string_view text) {
  const std::optional<std::size_t> value = parse_decimal(text);
  if (!value || *value == 0 || *value > 256)
    throw std::invalid_argument(
        "worker count must be in the range 1..256");
  return *value;
}

[[nodiscard]] int create_signal_fd() {
  sigset_t mask;
  ::sigemptyset(&mask);
  ::sigaddset(&mask, SIGINT);
  ::sigaddset(&mask, SIGTERM);
  if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0)
    throw_system_error(errno, "sigprocmask");

  const int fd = ::signalfd(-1, &mask, SFD_CLOEXEC);
  if (fd < 0)
    throw_system_error(errno, "signalfd");
  return fd;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2 || argc > 3) {
      std::cerr << "usage: callback_http_server <port> [workers]\n";
      return EXIT_FAILURE;
    }

    const uint16_t port = parse_port(argv[1]);
    const std::size_t worker_count =
        argc == 3 ? parse_worker_count(argv[2]) : 1;
    server_metrics metrics;
    callback_server server(port, worker_count, create_signal_fd(),
                           metrics);

    std::cout << "raw io_uring callback HTTP server listening on "
              << "http://0.0.0.0:" << server.bound_port() << " with "
              << server.worker_count() << " worker(s)\n";
    std::cout.flush();

    server.run();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "callback_http_server: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}