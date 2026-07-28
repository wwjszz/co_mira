#include <co_mira.hpp>

#include <cstdint>

namespace {

mira::co::task<void> run_nop(int32_t &result) {
  result = co_await mira::co::io::nop();
}

} // namespace

int main() {
  int32_t result = -1;

  mira::co::scheduler scheduler;
  scheduler.co_spawn(run_nop(result));
  scheduler.start();
  scheduler.join();

  return result == 0 ? 0 : 1;
}
