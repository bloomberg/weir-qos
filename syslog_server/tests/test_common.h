// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#ifndef INCLUDED_TEST_COMMON
#define INCLUDED_TEST_COMMON

#include <filesystem>
#include <fstream>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include "common.h"

namespace syslogsrv {
namespace test {

constexpr inline char MOCK_LOG[] = "/tmp/tmp.log";

class MockLog : public ::testing::Test {
  protected:
    void SetUp() override { auto logger = spdlog::basic_logger_mt(syslogsrv::SERVER_NAME, MOCK_LOG); }
    void TearDown() override {
        std::filesystem::remove(MOCK_LOG);
        spdlog::drop(syslogsrv::SERVER_NAME);
    }
};

} // namespace test
} // namespace syslogsrv

#endif
