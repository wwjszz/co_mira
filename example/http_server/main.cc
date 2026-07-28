#include "co_mira.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using mira::co::scheduler;
using mira::co::task;
using mira::co::net::acceptor;
using mira::co::net::inet_address;
using mira::co::net::socket;

constexpr std::size_t max_header_size = 16 * 1024;
constexpr std::size_t receive_buffer_size = 8 * 1024;
constexpr std::size_t max_requests_per_connection = 100;

struct server_metrics {
  std::atomic<uint64_t> accepted_connections{0};
  std::atomic<uint64_t> active_connections{0};
  std::atomic<uint64_t> requests{0};
};

class active_connection_guard {
public:
  explicit active_connection_guard(server_metrics &metrics) noexcept
      : metrics_(&metrics) {
    this->metrics_->active_connections.fetch_add(1,
                                                  std::memory_order_relaxed);
  }

  ~active_connection_guard() {
    this->metrics_->active_connections.fetch_sub(1,
                                                  std::memory_order_relaxed);
  }

  active_connection_guard(const active_connection_guard &) = delete;
  active_connection_guard &operator=(const active_connection_guard &) = delete;

private:
  server_metrics *metrics_;
};

class scheduler_pool {
public:
  explicit scheduler_pool(std::size_t worker_count) {
    if (worker_count == 0) {
      mira::co::log("HTTP scheduler pool requires at least one worker");
      throw std::invalid_argument(
          "HTTP scheduler pool requires at least one worker");
    }

    this->workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index)
      this->workers_.push_back(std::make_unique<scheduler>());
  }

  void start() {
    for (auto &worker : this->workers_)
      worker->start_remote();
  }

  void dispatch(task<void> item) {
    const std::size_t index =
        this->next_.fetch_add(1, std::memory_order_relaxed) %
        this->workers_.size();
    this->workers_[index]->co_spawn(std::move(item));
  }

  void request_stop() noexcept {
    for (auto &worker : this->workers_) {
      try {
        if (worker->status() == scheduler::runtime_state::running)
          worker->request_stop();
      } catch (...) {
        mira::co::log("failed to stop an HTTP worker scheduler");
      }
    }
  }

  void join() {
    std::exception_ptr first_exception;
    for (auto &worker : this->workers_) {
      try {
        worker->join();
      } catch (...) {
        if (!first_exception)
          first_exception = std::current_exception();
      }
    }

    if (first_exception) {
      mira::co::log("rethrowing HTTP worker scheduler failure");
      std::rethrow_exception(first_exception);
    }
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return this->workers_.size();
  }

private:
  std::vector<std::unique_ptr<scheduler>> workers_;
  std::atomic<std::size_t> next_{0};
};

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    mira::co::log("failed to open HTTP asset: {}", path.string());
    throw std::runtime_error("failed to open HTTP asset: " + path.string());
  }

  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

struct static_resource {
  std::string content_type;
  std::shared_ptr<const std::string> body;
  std::string download_name;
  std::string cache_control;
};

class static_site {
public:
  explicit static_site(const std::filesystem::path &root) {
    this->add("/", root / "index.html", "text/html; charset=utf-8");
    this->add("/assets/style.css", root / "style.css",
              "text/css; charset=utf-8");
    this->add("/assets/app.js", root / "app.js",
              "text/javascript; charset=utf-8");
    this->add("/download/co-mira-starter.txt",
              root / "downloads" / "co-mira-starter.txt",
              "text/plain; charset=utf-8", "co-mira-starter.txt", "no-store");
    this->add("/download/sample-data.json",
              root / "downloads" / "sample-data.json",
              "application/json; charset=utf-8", "co-mira-sample-data.json",
              "no-store");
  }

  [[nodiscard]] const static_resource *
  find(std::string_view target) const noexcept {
    const auto found = this->resources_.find(std::string(target));
    return found == this->resources_.end() ? nullptr
                                           : std::addressof(found->second);
  }

private:
  void add(std::string path, const std::filesystem::path &file,
           std::string content_type, std::string download_name = {},
           std::string cache_control = "public, max-age=300") {
    static_resource resource{
        .content_type = std::move(content_type),
        .body = std::make_shared<const std::string>(read_file(file)),
        .download_name = std::move(download_name),
        .cache_control = std::move(cache_control),
    };
    this->resources_.emplace(std::move(path), std::move(resource));
  }

  std::unordered_map<std::string, static_resource> resources_;
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
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
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
      request_line.find(' ', second_space + 1) != std::string_view::npos)
    return std::nullopt;

  const std::string_view method = request_line.substr(0, first_space);
  std::string_view target =
      request_line.substr(first_space + 1, second_space - first_space - 1);
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
      const std::optional<std::size_t> content_length = parse_decimal(value);
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

[[nodiscard]] http_response route_request(const http_request &request,
                                          const static_site &site,
                                          const server_metrics &metrics,
                                          std::size_t worker_count) {
  if (request.method != "GET" && request.method != "HEAD") {
    http_response response =
        text_response(405, "Method Not Allowed",
                      "Only GET and HEAD are supported.\n");
    return response;
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

  if (request.target == "/api/status") {
    std::string json;
    json.reserve(192);
    json += "{\n  \"status\": \"ok\",\n  \"workers\": ";
    json += std::to_string(worker_count);
    json += ",\n  \"accepted_connections\": ";
    json += std::to_string(
        metrics.accepted_connections.load(std::memory_order_relaxed));
    json += ",\n  \"active_connections\": ";
    json += std::to_string(
        metrics.active_connections.load(std::memory_order_relaxed));
    json += ",\n  \"requests\": ";
    json +=
        std::to_string(metrics.requests.load(std::memory_order_relaxed));
    json += "\n}\n";
    return {
        .status = 200,
        .reason = "OK",
        .content_type = "application/json; charset=utf-8",
        .body = make_body(std::move(json)),
        .download_name = {},
        .cache_control = "no-store",
    };
  }

  if (const static_resource *resource = site.find(request.target)) {
    return {
        .status = 200,
        .reason = "OK",
        .content_type = resource->content_type,
        .body = resource->body,
        .download_name = resource->download_name,
        .cache_control = resource->cache_control,
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

[[nodiscard]] std::string build_response_header(const http_response &response,
                                                bool keep_alive) {
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

task<bool> send_response(const socket &client, std::string_view header,
                         std::string_view body = {}) {
  std::size_t header_sent = 0;
  std::size_t body_sent = 0;
  while (header_sent != header.size() || body_sent != body.size()) {
    std::array<iovec, 2> iovecs{};
    std::size_t count = 0;
    if (header_sent != header.size()) {
      iovecs[count++] = {
          .iov_base = const_cast<char *>(header.data() + header_sent),
          .iov_len = header.size() - header_sent,
      };
    }
    if (body_sent != body.size()) {
      iovecs[count++] = {
          .iov_base = const_cast<char *>(body.data() + body_sent),
          .iov_len = body.size() - body_sent,
      };
    }

    msghdr message{};
    message.msg_iov = iovecs.data();
    message.msg_iovlen = count;
    const int result = co_await mira::co::io::sendmsg(
        client.native_handle(), &message, MSG_NOSIGNAL);
    if (result <= 0)
      co_return false;

    std::size_t completed = static_cast<std::size_t>(result);
    const std::size_t header_remaining = header.size() - header_sent;
    const std::size_t header_completed =
        std::min(completed, header_remaining);
    header_sent += header_completed;
    completed -= header_completed;

    const std::size_t body_remaining = body.size() - body_sent;
    const std::size_t body_completed =
        std::min(completed, body_remaining);
    body_sent += body_completed;
    completed -= body_completed;
    if (completed != 0)
      co_return false;
  }
  co_return true;
}

task<void> serve_connection(socket client, const static_site &site,
                            server_metrics &metrics,
                            std::size_t worker_count) {
  active_connection_guard guard(metrics);
  std::array<char, receive_buffer_size> receive_buffer{};
  std::string pending;
  pending.reserve(receive_buffer_size);
  std::size_t request_count = 0;

  for (;;) {
    std::size_t header_end = pending.find("\r\n\r\n");
    while (header_end == std::string::npos) {
      if (pending.size() >= max_header_size) {
        const http_response response =
            text_response(431, "Request Header Fields Too Large",
                          "Request headers exceed 16 KiB.\n");
        const std::string header = build_response_header(response, false);
        (void)co_await send_response(client, header, *response.body);
        co_return;
      }

      const int received = co_await client.recv(receive_buffer);
      if (received <= 0)
        co_return;
      pending.append(receive_buffer.data(),
                     static_cast<std::size_t>(received));
      header_end = pending.find("\r\n\r\n");
    }

    const std::size_t consumed = header_end + 4;
    if (consumed > max_header_size) {
      const http_response response =
          text_response(431, "Request Header Fields Too Large",
                        "Request headers exceed 16 KiB.\n");
      const std::string header = build_response_header(response, false);
      (void)co_await send_response(client, header, *response.body);
      co_return;
    }

    const std::optional<http_request> request =
        parse_request(std::string_view{pending.data(), consumed});
    if (!request) {
      const http_response response =
          text_response(400, "Bad Request", "Malformed HTTP request.\n");
      const std::string header = build_response_header(response, false);
      (void)co_await send_response(client, header, *response.body);
      co_return;
    }

    ++request_count;
    metrics.requests.fetch_add(1, std::memory_order_relaxed);
    const bool keep_alive =
        request->keep_alive &&
        request_count < max_requests_per_connection;
    const http_response response =
        route_request(*request, site, metrics, worker_count);
    const std::string header =
        build_response_header(response, keep_alive);
    const std::string_view body =
        !request->head_only && response.body
            ? std::string_view{*response.body}
            : std::string_view{};
    if (!(co_await send_response(client, header, body)))
      co_return;

    pending.erase(0, consumed);
    if (!keep_alive)
      co_return;
  }
}

task<void> accept_loop(acceptor listener, scheduler_pool &workers,
                       const static_site &site, server_metrics &metrics) {
  for (;;) {
    const int accepted = co_await listener.accept();
    if (accepted == -ECANCELED)
      co_return;
    if (accepted < 0) {
      mira::co::log("HTTP accept failed with errno {}", -accepted);
      const int wait_result =
          co_await mira::co::io::sleep_for(std::chrono::milliseconds{25});
      if (wait_result == -ECANCELED)
        co_return;
      continue;
    }

    metrics.accepted_connections.fetch_add(1, std::memory_order_relaxed);
    try {
      socket client = socket::adopt(accepted);
      client.set_tcp_no_delay();
      workers.dispatch(serve_connection(std::move(client), site, metrics,
                                        workers.size()));
    } catch (...) {
      mira::co::log("failed to dispatch an accepted HTTP connection");
    }
  }
}

task<void> shutdown_on_signal(mira::co::io::unique_fd signal_fd,
                              scheduler &front, scheduler_pool &workers) {
  signalfd_siginfo signal_info{};
  std::span<char> buffer{reinterpret_cast<char *>(&signal_info),
                         sizeof(signal_info)};
  const int result = co_await mira::co::io::read(signal_fd.get(), buffer);
  if (result < 0)
    mira::co::log("signalfd read failed with errno {}", -result);
  else
    mira::co::log("received signal {}, stopping HTTP server",
                  signal_info.ssi_signo);

  workers.request_stop();
  front.request_stop();
}

[[nodiscard]] uint16_t parse_port(std::string_view text) {
  const std::optional<std::size_t> value = parse_decimal(text);
  if (!value || *value > 65535) {
    mira::co::log("invalid HTTP port: {}", text);
    throw std::invalid_argument("port must be in the range 0..65535");
  }
  return static_cast<uint16_t>(*value);
}

[[nodiscard]] std::size_t parse_worker_count(std::string_view text) {
  const std::optional<std::size_t> value = parse_decimal(text);
  if (!value || *value == 0 || *value > 256) {
    mira::co::log("invalid HTTP worker count: {}", text);
    throw std::invalid_argument("worker count must be in the range 1..256");
  }
  return *value;
}

[[nodiscard]] std::size_t default_worker_count() noexcept {
  const unsigned hardware = std::thread::hardware_concurrency();
  return std::clamp<std::size_t>(hardware == 0 ? 2 : hardware, 1, 8);
}

[[nodiscard]] mira::co::io::unique_fd create_signal_fd() {
  sigset_t mask;
  ::sigemptyset(&mask);
  ::sigaddset(&mask, SIGINT);
  ::sigaddset(&mask, SIGTERM);
  if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
    mira::co::log("sigprocmask failed with errno {}", errno);
    throw std::system_error(errno, std::generic_category(), "sigprocmask");
  }

  const int descriptor = ::signalfd(-1, &mask, SFD_CLOEXEC);
  if (descriptor < 0) {
    mira::co::log("signalfd failed with errno {}", errno);
    throw std::system_error(errno, std::generic_category(), "signalfd");
  }
  return mira::co::io::unique_fd{descriptor};
}

void print_usage(const char *program) {
  std::cout << "Usage: " << program << " [port] [workers] [asset-root]\n"
            << "  port        0..65535; 0 selects a free port (default: 8080)\n"
            << "  workers     1..256 (default: up to 8 hardware threads)\n"
            << "  asset-root  directory containing index.html, style.css, "
               "app.js and downloads/\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc > 1 &&
        (std::string_view{argv[1]} == "--help" ||
         std::string_view{argv[1]} == "-h")) {
      print_usage(argv[0]);
      return 0;
    }
    if (argc > 4) {
      print_usage(argv[0]);
      return 2;
    }

    const uint16_t port = argc > 1 ? parse_port(argv[1]) : 8080;
    const std::size_t worker_count =
        argc > 2 ? parse_worker_count(argv[2]) : default_worker_count();
    const std::filesystem::path asset_root =
        argc > 3 ? std::filesystem::path{argv[3]}
                 : std::filesystem::path{CO_MIRA_HTTP_ASSET_ROOT};

    static_site site(asset_root);
    server_metrics metrics;
    scheduler_pool workers(worker_count);
    scheduler front;
    acceptor listener(inet_address{port});
    const inet_address address = listener.local_address();
    mira::co::io::unique_fd signal_fd = create_signal_fd();

    workers.start();
    front.co_spawn(
        accept_loop(std::move(listener), workers, site, metrics));
    front.co_spawn(
        shutdown_on_signal(std::move(signal_fd), front, workers));

    std::cout << "co_mira HTTP server listening on http://"
              << address.ip() << ':' << address.port() << " with "
              << worker_count << " worker(s)\n"
              << "Downloads:\n"
              << "  http://127.0.0.1:" << address.port()
              << "/download/co-mira-starter.txt\n"
              << "  http://127.0.0.1:" << address.port()
              << "/download/sample-data.json\n"
              << "Press Ctrl-C to stop.\n"
              << std::flush;

    front.start();
    front.join();
    workers.join();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "co_http_server: " << error.what() << '\n';
    return 1;
  }
}
