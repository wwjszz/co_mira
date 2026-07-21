#include "io.hpp"
#include "scheduler.hpp"
#include "task.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <linux/stat.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace {

using mira::co::scheduler;
using mira::co::task;

static_assert(!mira::co::core::linkable_action<mira::co::core::io_close_owned>);

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

void run_scheduler(task<void> item) {
  scheduler sche;
  sche.co_spawn(std::move(item));
  sche.start();
  sche.join();
}

struct file_results {
  int32_t mkdir_result = 1;
  int32_t open_result = 1;
  int32_t write_result = 1;
  int32_t fsync_result = 1;
  int32_t fallocate_result = 1;
  int32_t statx_result = 1;
  int32_t read_result = 1;
  int32_t close_result = 1;
  int32_t rename_result = 1;
  int32_t unlink_result = 1;
  int32_t rmdir_result = 1;
  struct statx stat_buffer{};
  std::array<char, 32> read_buffer{};
};

task<void> exercise_file_api(std::string directory, file_results &results) {
  const std::string original = directory + "/original.txt";
  const std::string renamed = directory + "/renamed.txt";

  results.mkdir_result = co_await mira::co::io::mkdirat(AT_FDCWD, directory.c_str(), 0700);
  if (results.mkdir_result < 0)
    co_return;

  results.open_result = co_await mira::co::io::openat(AT_FDCWD, original.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
  if (results.open_result < 0)
    co_return;

  mira::co::io::unique_fd file{results.open_result};
  const int fd = file.get();
  const std::array<char, 10> payload{'c', 'o', '_', 'm', 'i', 'r', 'a', '_', 'i', 'o'};
  results.write_result = co_await mira::co::io::write(fd, payload, 0);
  results.fsync_result = co_await mira::co::io::fsync(fd);
  results.fallocate_result = co_await mira::co::io::fallocate(fd, 0, 0, 4096);
  results.statx_result = co_await mira::co::io::statx(AT_FDCWD, original.c_str(), 0, STATX_SIZE, &results.stat_buffer);
  results.read_result = co_await mira::co::io::read(fd, results.read_buffer, 0);
  results.close_result = co_await mira::co::io::close(std::move(file));
  results.rename_result = co_await mira::co::io::renameat(AT_FDCWD, original.c_str(), AT_FDCWD, renamed.c_str());
  results.unlink_result = co_await mira::co::io::unlinkat(AT_FDCWD, renamed.c_str());
  results.rmdir_result = co_await mira::co::io::unlinkat(AT_FDCWD, directory.c_str(), AT_REMOVEDIR);
}

task<void> open_missing_file(std::string path, int32_t &result) {
  result = co_await mira::co::io::openat(AT_FDCWD, path.c_str(), O_RDONLY | O_CLOEXEC);
}

task<void> read_invalid_fd_with_token(mira::co::io::cancel_token &token, int32_t &result) {
  std::array<char, 8> buffer{};
  result = co_await mira::co::io::read(-1, buffer, token);
}

task<void> close_owned_fd(mira::co::io::unique_fd fd) { (void)co_await mira::co::io::close(std::move(fd)); }

void cleanup(const std::string &directory) noexcept {
  const std::string original = directory + "/original.txt";
  const std::string renamed = directory + "/renamed.txt";
  (void)::unlink(original.c_str());
  (void)::unlink(renamed.c_str());
  (void)::rmdir(directory.c_str());
}

void test_file_operations() {
  const std::string directory = "/tmp/co_mira_file_io_" + std::to_string(::getpid());
  cleanup(directory);

  file_results results{};
  run_scheduler(exercise_file_api(directory, results));

  CHECK(results.mkdir_result == 0);
  CHECK(results.open_result >= 0);
  CHECK(results.write_result == 10);
  CHECK(results.fsync_result == 0);
  CHECK(results.fallocate_result == 0);
  CHECK(results.statx_result == 0);
  CHECK(results.stat_buffer.stx_size >= 4096);
  CHECK(results.read_result == static_cast<int32_t>(results.read_buffer.size()));
  CHECK(std::string_view(results.read_buffer.data(), 10) == "co_mira_io");
  CHECK(results.close_result == 0);
  CHECK(results.rename_result == 0);
  CHECK(results.unlink_result == 0);
  CHECK(results.rmdir_result == 0);

  cleanup(directory);
}

void test_file_errors_and_cancel_overload() {
  const std::string missing = "/tmp/co_mira_missing_" + std::to_string(::getpid());
  (void)::unlink(missing.c_str());

  int32_t open_result = 0;
  run_scheduler(open_missing_file(missing, open_result));
  CHECK(open_result == -ENOENT);

  mira::co::io::cancel_token token;
  int32_t read_result = 0;
  run_scheduler(read_invalid_fd_with_token(token, read_result));
  CHECK(read_result == -EBADF);
  CHECK(token.completed());
}

void test_owned_close_rolls_back_on_sqe_failure() {
  int descriptors[2]{-1, -1};
  CHECK(::pipe(descriptors) == 0);

  scheduler sche{mira::co::core::scheduler_test_failure::io_awaiter};
  sche.co_spawn(close_owned_fd(mira::co::io::unique_fd{descriptors[0]}));
  sche.start();

  bool threw = false;
  try {
    sche.join();
  } catch (const std::system_error &error) {
    threw = true;
    CHECK(error.code().value() == ENOSPC);
  }

  CHECK(threw);
  CHECK(::fcntl(descriptors[0], F_GETFD) == -1);
  CHECK(errno == EBADF);
  CHECK(::close(descriptors[1]) == 0);
}

} // namespace

int main() {
  try {
    test_file_operations();
    test_file_errors_and_cancel_overload();
    test_owned_close_rolls_back_on_sqe_failure();
  } catch (const std::exception &error) {
    std::cerr << "[FAIL] file IO: " << error.what() << '\n';
    return 1;
  }

  std::cout << "All file IO tests passed\n";
  return 0;
}
