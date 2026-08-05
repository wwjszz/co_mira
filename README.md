# co_mira

`co_mira` is a lightweight coroutine library for writing high-concurrency Linux
applications as straightforward, sequential C++ code. Built on
[`io_uring`](https://kernel.dk/io_uring.pdf), it combines awaitable I/O with
task composition, coroutine-aware synchronization and explicit scheduler
control, avoiding callback-driven state machines without hiding the runtime.

The library is header-only, targets C++23 and works for both single-scheduler
programs and applications that distribute work across multiple schedulers.

## Features

- **Awaitable I/O:** socket, file and timer operations, linked requests,
  operation timeouts and owning cancellation.
- **Coroutine building blocks:** `task`, `shared_task`, detached tasks and
  generators, plus `when_all`, `when_any` and `when_some` for composition.
- **Synchronization:** coroutine-aware mutexes, semaphores and condition
  variables that can coordinate work across schedulers.
- **Multi-scheduler execution:** cross-scheduler `co_spawn`, `resume_on` and
  `IORING_OP_MSG_RING` transport, with orderly shutdown and exception reporting.

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

The complete HTTP server used for performance testing is maintained on the
[`benchmark` branch](https://github.com/wwjszz/co_mira/blob/benchmark/example/http_server/main.cc).

## Build and test

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## HTTP benchmark

The benchmark compares the same 3-byte HTTP response across four execution
models:

- `co_mira` coroutine + io_uring
- Explicit io_uring callback state machine
- Nonblocking epoll event loop
- Blocking fixed thread pool

The coroutine and callback versions use the same
`IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG` ring setup flags.

### Two-CPU Vlab VM

This comparison uses one acceptor and one server worker. Results were collected
on 2026-07-28 on a Vlab VM with two available
Intel Xeon Silver 4314 logical CPUs, Linux 7.0.14 and GCC 14.2. The server used
CPU 9 for its front end and CPU 63 for its worker; loopback wrk 4.1.0 used one
thread on CPU 9. Each point is a median of repeated Release runs. No run
reported socket or HTTP errors.

![HTTP throughput](https://raw.githubusercontent.com/wwjszz/co_mira/benchmark/benchmark/http_server/results-server-2cpu/throughput.svg)

| wrk | co_mira coroutine | io_uring callback | epoll | blocking |
|---|---:|---:|---:|---:|
| `-t1 -c1` | 35,826 | 35,480 | 32,077 | 34,913 |
| `-t1 -c16` | 83,650 | **84,334** | 70,004 | 35,079 |
| `-t1 -c64` | **86,367** | 85,508 | 69,890 | 34,951 |
| `-t1 -c256` | 90,350 | **90,723** | 70,566 | 34,472 |

Values are requests/second. At 256 connections, P99 latency was 2.64 ms for
the coroutine server, 2.61 ms for callback, 3.61 ms for epoll and 724 ms for
the blocking pool. The coroutine and callback results are effectively tied:
coroutine suspension is not the bottleneck in this workload. The blocking
model maintains single-connection throughput but develops severe queueing
latency as concurrency rises.

Because wrk and the servers share a two-CPU VM, these numbers are a local
comparison rather than a hardware-independent capacity claim. Machine-readable
summaries, methodology and additional WSL results are on the
[`benchmark` branch](https://github.com/wwjszz/co_mira/tree/benchmark/benchmark/http_server).

### WSL2 rerun

The current benchmark branch was rerun on 2026-07-29 on an Intel Core
i9-14900KF under WSL2 (`6.18.33.2-microsoft-standard-WSL2`), using GCC 15.2,
liburing 2.14 and a Release build. The servers used two workers
on CPUs `0,2,4`; wrk used CPUs `10,12`. Each point is the median of three
10-second runs after a 3-second warmup, with no socket or HTTP errors.

![WSL2 HTTP throughput](https://raw.githubusercontent.com/wwjszz/co_mira/benchmark/benchmark/http_server/results-wsl-current/throughput-connections.svg)

| wrk | co_mira coroutine | io_uring callback | epoll | blocking |
|---|---:|---:|---:|---:|
| `-t1 -c1` | 32,832 | 33,504 | 32,197 | **33,660** |
| `-t2 -c16` | **515,601** | 485,743 | 405,766 | 69,162 |
| `-t2 -c64` | **631,164** | 613,848 | 526,134 | 72,259 |
| `-t2 -c256` | **671,786** | 656,630 | 540,903 | 71,154 |
| `-t2 -c1024` | **649,172** | 625,048 | 512,489 | 68,517 |

At 256 connections, P99 latency was 0.522 ms for coroutine, 0.488 ms for
callback, 0.710 ms for epoll and 350.59 ms for blocking. Coroutine and callback
remain close after using identical ring flags; coroutine leads by 2.3% at this
point, so coroutine suspension is still not the limiting cost. The async
servers peaked at two workers for this small loopback workload.
