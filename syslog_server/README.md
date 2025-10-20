# syslog_server

A custom syslog server that reads the logs streamed out by the HAProxy lua plugin and uses that data to update the stats in Redis. More detailed information can be found in [Architectural Overview of the Weir QoS Framework](/docs/architecture_overview.md#syslog-server).

## Building

The build is managed with CMake and requires a C++20-compatible compiler. There are two options for dependencies:

The simplest and most reliable option is to let CMake fetch appropriate versions of all dependencies itself at configure time, and then build them along with the syslog server:

```console
mkdir build
cd build
cmake .. -DWEIR_FETCH_DEPENDENCIES=ON
make
```

If your package manager has all the dependencies available at appropriate versions, then you can also install all the dependencies yourself via your package manager and omit the `WEIR_FETCH_DEPENDENCIES` option.

The required dependencies are:

- [libev](https://github.com/enki/libev/tree/93823e6ca699df195a6c7b8bfa6006ec40ee0003) (v4.22)
- [spdlog](https://github.com/gabime/spdlog.git) (v1.11.0)
- [hiredis](https://github.com/redis/hiredis.git) (v1.1.0)
- [yaml-cpp](https://github.com/jbeder/yaml-cpp.git) (v0.7.0)
- [GTest](https://github.com/google/googletest.git) (v1.13.0, for unit tests)

## Unit tests

A unit test framework using Google Test and Gmock is provided, it can be run after completion of the Building step. From the build directory, run:

`ctest -V`

An extra build target called `lcov` is used to generate gcov report. With lcov installed and from build directory, run:

`make lcov`

## Performance

The job of our custom syslog server is to receive and process events from haproxy both for logging and for powering the rate-limiting mechanisms.
Since those events from haproxy are received via UDP, if the syslog server can't keep up with the rate at which they're being sent, some of those messages will be dropped.
While the system is intended to be stable in the face of a few dropped messages, it's not ideal and if the dropped message is a log line then that would simply be lost.

As a result, it is important that the syslog server is fast enough to keep up with the messages coming in from haproxy.
To evaluate whether this is the case in any given environment, we've built [a simple benchmarking tool](../integration-tests/syslog-bench.cpp) that will send messages to the local syslog server at a defined rate and at the same time check if the kernel reports any dropped packets.

Of course the exact throughput you need will depend on the size of your workload on each server, the hardware of your servers and your configuration for haproxy and syslog server.
On high-end hardware it is expected that the syslog server is able to process on the order of 100,000 to 200,000 messages per second with a single concurrent processor (configured as `num_of_syslog_servers`).
