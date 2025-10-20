// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#ifndef INCLUDED_TIME_WRAPPER
#define INCLUDED_TIME_WRAPPER

#include <chrono>
#include <functional>

namespace syslogsrv {

// Wrapper class to allow us to inject our own "current time". Used in tests to exercise timing logic without sleeping.
class TimeWrapper {
  public:
    TimeWrapper() = default;
    TimeWrapper(std::function<std::chrono::system_clock::time_point()> time_func) : m_time_func(time_func) {}

    std::chrono::system_clock::time_point now() const {
        if (m_time_func) {
            return m_time_func();
        }
        return std::chrono::system_clock::now();
    }

  private:
    // We specifically use the *system_clock*, which in C++20 is defined to measure the time since
    // midnight on 1970-01-01 *excluding* leap seconds. This is for compatibility with python's time.time()
    // in polygen, which is also defined ("on most unix systems") to exclude leap seconds.
    // While C++20 also exposes a `utc_clock`, this is defined to *include* leap seconds.
    // Using that would cause failures without changes in polygen because the two would disagree on what the
    // current timestamp is (and therefore on what data from redis should be taken into account when determining usage).
    // Similarly, we can't use `steady_clock` because its epoch is not defined and could be something like
    // the time since system startup. Often our need for time is to check the actual current unix timestamp
    // (for interactions with haproxy), so a clock with an undefined epoch is unhelpful.
    std::function<std::chrono::system_clock::time_point()> m_time_func;
};

} // namespace syslogsrv

#endif
