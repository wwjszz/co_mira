# co_mira

`co_mira` is a header-only C++23 coroutine runtime for Linux, built on
[`io_uring`](https://kernel.dk/io_uring.pdf) and liburing. It provides a small
async I/O runtime without virtual dispatch or a separate code-generation step.

## Features

- Single-thread and multi-scheduler runtimes with `IORING_OP_MSG_RING`
- `task`, `shared_task`, detached tasks and generators
- Socket, file and timer operations with owning cancellation
- Coroutine mutex, semaphore and condition variable
- `when_all`, `when_any` and `when_some`
- Cross-scheduler `co_spawn`, `resume_on` and orderly shutdown

The current minimum target is Linux 5.19. A C++23 compiler, CMake 3.20+ and
liburing are required.

## Quick start

```cpp
#include "co_mira.hpp"

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

mira::co::task<void> hello() {
  co_await mira::co::io::sleep_for(10ms);
  std::cout << "hello from co_mira\n";
}

int main() {
  mira::co::scheduler scheduler;
  scheduler.co_spawn(hello());
  scheduler.start();
  scheduler.join();
}
```

## Build and test

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```
