#include "io.hpp"
#include "scheduler.hpp"
#include "task.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
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
    if (!(expression))                                                                                                                               \
      throw test_failure(#expression, __FILE__, __LINE__);                                                                                           \
  } while (false)

template <typename Exception, typename Function> void check_throws(Function &&function) {
  bool caught = false;
  try {
    std::forward<Function>(function)();
  } catch (const Exception &) {
    caught = true;
  }
  CHECK(caught);
}

task<void> remote_nop(std::atomic<unsigned> &completed, std::atomic<unsigned> &errors, std::thread::id &target_thread) {
  const int result = co_await mira::co::io::nop();
  target_thread = std::this_thread::get_id();
  if (result == 0)
    completed.fetch_add(1, std::memory_order_release);
  else
    errors.fetch_add(1, std::memory_order_relaxed);
}

task<void> distribute_work(scheduler &target, unsigned task_count, std::atomic<unsigned> &completed, std::atomic<unsigned> &errors,
                           std::thread::id &source_thread, std::thread::id &target_thread) {
  source_thread = std::this_thread::get_id();

  for (unsigned index = 0; index < task_count; ++index)
    target.co_spawn(remote_nop(completed, errors, target_thread));

  while (completed.load(std::memory_order_acquire) != task_count && errors.load(std::memory_order_relaxed) == 0)
    (void)co_await mira::co::io::sleep_for(100us);

  target.request_stop();
}

void test_msg_ring_cross_scheduler_spawn() {
  constexpr unsigned task_count = 128;
  scheduler source;
  scheduler target;
  std::atomic<unsigned> completed = 0;
  std::atomic<unsigned> errors = 0;
  std::thread::id source_thread;
  std::thread::id target_thread;
  const std::thread::id test_thread = std::this_thread::get_id();

  target.start_remote();
  CHECK(target.status() == scheduler::runtime_state::running);

  check_throws<std::logic_error>([&] { target.co_spawn(remote_nop(completed, errors, target_thread)); });
  check_throws<std::logic_error>([&] { target.request_stop(); });

  source.co_spawn(distribute_work(target, task_count, completed, errors, source_thread, target_thread));
  source.start();
  source.join();
  target.join();

  CHECK(completed.load(std::memory_order_acquire) == task_count);
  CHECK(errors.load(std::memory_order_relaxed) == 0);
  CHECK(source_thread != test_thread);
  CHECK(target_thread != test_thread);
  CHECK(source_thread != target_thread);
  CHECK(source.status() == scheduler::runtime_state::stopped);
  CHECK(target.status() == scheduler::runtime_state::stopped);
}

} // namespace

int main() {
  try {
    test_msg_ring_cross_scheduler_spawn();
    std::cout << "[PASS] MSG_RING cross-scheduler spawn\n";
  } catch (const std::exception &error) {
    std::cerr << "[FAIL] MSG_RING cross-scheduler spawn: " << error.what() << '\n';
    return 1;
  }
}
