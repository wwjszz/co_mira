#include "io.hpp"
#include "scheduler.hpp"
#include "task.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

using mira::co::scheduler;
using mira::co::task;
using namespace std::chrono_literals;

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

task<void> wait_then_stop(scheduler &first, scheduler *second, const std::atomic<unsigned> &completed, unsigned expected,
                          std::atomic<bool> &timed_out) {
  constexpr unsigned max_attempts = 20'000;
  unsigned attempts = 0;
  while (completed.load(std::memory_order_acquire) != expected && attempts++ != max_attempts)
    (void)co_await mira::co::io::sleep_for(100us);

  if (completed.load(std::memory_order_acquire) != expected)
    timed_out.store(true, std::memory_order_relaxed);

  first.request_stop();
  if (second != nullptr)
    second->request_stop();
}

task<void> migrate_between(scheduler &first, scheduler &second, unsigned rounds, std::atomic<unsigned> &completed, std::atomic<unsigned> &errors,
                           std::thread::id &first_thread, std::thread::id &second_thread) {
  first_thread = std::this_thread::get_id();

  co_await mira::co::resume_on(first);
  if (std::this_thread::get_id() != first_thread)
    errors.fetch_add(1, std::memory_order_relaxed);

  for (unsigned round = 0; round < rounds; ++round) {
    co_await mira::co::resume_on(second);
    if (round == 0)
      second_thread = std::this_thread::get_id();
    if (std::this_thread::get_id() != second_thread || second_thread == first_thread)
      errors.fetch_add(1, std::memory_order_relaxed);

    co_await mira::co::resume_on(first);
    if (std::this_thread::get_id() != first_thread)
      errors.fetch_add(1, std::memory_order_relaxed);
  }

  completed.store(1, std::memory_order_release);
}

void test_resume_on_migrates_current_coroutine() {
  constexpr unsigned migration_rounds = 1'000;
  scheduler first;
  scheduler second;
  scheduler coordinator;
  std::atomic<unsigned> completed = 0;
  std::atomic<unsigned> errors = 0;
  std::atomic<bool> timed_out = false;
  std::thread::id first_thread;
  std::thread::id second_thread;

  second.start_remote();
  first.co_spawn(migrate_between(first, second, migration_rounds, completed, errors, first_thread, second_thread));
  first.start_remote();

  coordinator.co_spawn(wait_then_stop(first, &second, completed, 1, timed_out));
  coordinator.start();
  coordinator.join();
  first.join();
  second.join();

  CHECK(!timed_out.load(std::memory_order_relaxed));
  CHECK(completed.load(std::memory_order_acquire) == 1);
  CHECK(errors.load(std::memory_order_relaxed) == 0);
  CHECK(first_thread != std::thread::id{});
  CHECK(second_thread != std::thread::id{});
  CHECK(first_thread != second_thread);
}

task<void> expect_resume_on_rejection(scheduler &target, bool &caught) {
  try {
    co_await mira::co::resume_on(target);
  } catch (const std::logic_error &) {
    caught = true;
  }
}

void test_resume_on_rejects_inactive_target() {
  scheduler source;
  scheduler inactive_target;
  bool caught = false;
  source.co_spawn(expect_resume_on_rejection(inactive_target, caught));
  source.start();
  source.join();
  CHECK(caught);
}

task<void> pending_recv(int fd, std::atomic<unsigned> &entered, int32_t &result, std::atomic<unsigned> &exited) {
  std::array<char, 8> buffer{};
  entered.fetch_add(1, std::memory_order_release);
  result = co_await mira::co::io::recv(fd, buffer);
  exited.fetch_add(1, std::memory_order_release);
}

task<void> pending_timeout(std::atomic<unsigned> &entered, int32_t &result, std::atomic<unsigned> &exited) {
  entered.fetch_add(1, std::memory_order_release);
  result = co_await mira::co::io::sleep_for(std::chrono::hours(1));
  exited.fetch_add(1, std::memory_order_release);
}

void test_shutdown_cancels_pending_io() {
  int sockets[2]{-1, -1};
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

  scheduler target;
  scheduler coordinator;
  std::atomic<unsigned> entered = 0;
  std::atomic<unsigned> exited = 0;
  std::atomic<bool> timed_out = false;
  int32_t recv_result = 0;
  int32_t timeout_result = 0;

  target.co_spawn(pending_recv(sockets[0], entered, recv_result, exited));
  target.co_spawn(pending_timeout(entered, timeout_result, exited));
  target.start_remote();

  coordinator.co_spawn(wait_then_stop(target, nullptr, entered, 2, timed_out));
  coordinator.start();
  coordinator.join();
  target.join();

  CHECK(::close(sockets[0]) == 0);
  CHECK(::close(sockets[1]) == 0);
  CHECK(!timed_out.load(std::memory_order_relaxed));
  CHECK(exited.load(std::memory_order_acquire) == 2);
  CHECK(recv_result == -ECANCELED);
  CHECK(timeout_result == -ECANCELED);
  CHECK(target.status() == scheduler::runtime_state::stopped);
}

task<void> remote_nop(std::atomic<unsigned> &completed, std::atomic<unsigned> &errors) {
  const int result = co_await mira::co::io::nop();
  if (result != 0)
    errors.fetch_add(1, std::memory_order_relaxed);
  completed.fetch_add(1, std::memory_order_release);
}

task<void> submit_batch(scheduler &target, const std::atomic<bool> &start, unsigned count, std::atomic<unsigned> &completed,
                        std::atomic<unsigned> &errors) {
  while (!start.load(std::memory_order_acquire))
    (void)co_await mira::co::io::sleep_for(100us);

  for (unsigned index = 0; index < count; ++index)
    target.co_spawn(remote_nop(completed, errors));
}

void test_multisource_lifecycle_stress() {
  constexpr unsigned rounds = 25;
  constexpr unsigned source_count = 2;
  constexpr unsigned tasks_per_source = 64;
  constexpr unsigned tasks_per_round = source_count * tasks_per_source;

  for (unsigned round = 0; round < rounds; ++round) {
    scheduler target;
    std::array<scheduler, source_count> sources;
    scheduler coordinator;
    std::atomic<bool> start = false;
    std::atomic<unsigned> completed = 0;
    std::atomic<unsigned> errors = 0;
    std::atomic<bool> timed_out = false;

    target.start_remote();
    for (scheduler &source : sources) {
      source.co_spawn(submit_batch(target, start, tasks_per_source, completed, errors));
      source.start();
    }

    start.store(true, std::memory_order_release);
    for (scheduler &source : sources)
      source.join();

    coordinator.co_spawn(wait_then_stop(target, nullptr, completed, tasks_per_round, timed_out));
    coordinator.start();
    coordinator.join();
    target.join();

    CHECK(!timed_out.load(std::memory_order_relaxed));
    CHECK(completed.load(std::memory_order_acquire) == tasks_per_round);
    CHECK(errors.load(std::memory_order_relaxed) == 0);
    CHECK(target.status() == scheduler::runtime_state::stopped);
  }
}

} // namespace

int main() {
  run_test("resume_on migrates current coroutine", test_resume_on_migrates_current_coroutine);
  run_test("resume_on rejects inactive target", test_resume_on_rejects_inactive_target);
  run_test("shutdown cancels pending IO", test_shutdown_cancels_pending_io);
  run_test("multi-source lifecycle stress", test_multisource_lifecycle_stress);

#ifdef CO_MIRA_ENABLE_COUNTERS
  if (mira::co::handle_counter.counter.load(std::memory_order_relaxed) != 0) {
    ++failed_tests;
    std::cerr << "[FAIL] coroutine handle counter did not return to zero\n";
  }
#endif

  if (failed_tests != 0) {
    std::cerr << failed_tests << " test(s) failed\n";
    return 1;
  }

  std::cout << "All multithread tests passed\n";
  return 0;
}
