// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#include <iostream>
#include <string>
#include <syslog_server.h>

#include "test_common.h"

namespace syslogsrv {
namespace test {

class MockSysCallClass : public SystemInterface {
  public:
    MOCK_METHOD(int, socket, (int, int, int), (override));
    MOCK_METHOD(int, getsockopt, (int, int, int, void* __restrict, socklen_t* __restrict), (override));
    MOCK_METHOD(int, setsockopt, (int, int, int, const void*, socklen_t), (override));
    MOCK_METHOD(int, bind, (int, const struct sockaddr*, socklen_t), (override));
    MOCK_METHOD(ssize_t, recvfrom, (int, void*, size_t, int, struct sockaddr*, socklen_t*), (override));
    MOCK_METHOD(std::string, getRmemMaxPath, (), (override));
};

// getRmemMax
TEST(getRmemMaxTest, SuccessfulRead) {
    std::string rmemm_path = "/tmp/rmem_max_test";
    std::ofstream fake_file(rmemm_path);
    fake_file << "8866";
    fake_file.close();

    EXPECT_EQ(getRmemMax(rmemm_path), 8866);
    std::filesystem::remove(rmemm_path);
}
// test exception while reading rmem_max
TEST_F(MockLog, getRmemMaxExceptionCatch) {
    std::string rmemm_path = "file_not_exist";

    // Return default value
    EXPECT_EQ(getRmemMax(rmemm_path), MAX_UDP_RECV_BUFFER_SIZE);
    // Varify error message was logged
    auto logger = spdlog::get(SERVER_NAME);
    logger->flush();
    std::ifstream log(MOCK_LOG);
    std::string line;
    std::getline(log, line);
    EXPECT_TRUE(line.find("failed to read rmem_max") != std::string::npos);
}

// getDesiredUdpRecvBufSize
TEST(getDesiredUdpRecvBufSizeTest, HandlesNonNagtive) {
    EXPECT_EQ(getDesiredUdpRecvBufSize(0), 0);
    EXPECT_EQ(getDesiredUdpRecvBufSize(1), 2);
    EXPECT_EQ(getDesiredUdpRecvBufSize(0x7FFFFFFF), 0xFFFFFFFE);
}

// getUdpRecvBufSize
ACTION_P(SetArg3ToInt, value) {
    *static_cast<int*>(arg3) = value;
    return value;
}
TEST(getUdpRecvBufSizeTest, Normal) {
    MockSysCallClass mock_sys_call;
    EXPECT_CALL(mock_sys_call, getsockopt).WillOnce(SetArg3ToInt(53));
    EXPECT_EQ(getUdpRecvBufSize(1, mock_sys_call), 53);
}
TEST_F(MockLog, getUdpRecvBufSizeDeathTest) {
    MockSysCallClass mock_sys_call;
    testing::Mock::AllowLeak(&mock_sys_call);
    ON_CALL(mock_sys_call, getsockopt).WillByDefault(testing::Return(-23));

    EXPECT_EXIT(getUdpRecvBufSize(1, mock_sys_call), ::testing::ExitedWithCode(-23 & 0xFF), "");

    auto logger = spdlog::get(SERVER_NAME);
    logger->flush();
    std::ifstream log(MOCK_LOG);
    std::string line;
    std::getline(log, line);

    EXPECT_TRUE(line.find("failed to get socket recv buf size") != std::string::npos);
}

// setUdpRecvBufSize
TEST_F(MockLog, setUdpRecvBufSizeDeathTest) {
    MockSysCallClass mock_sys_call;
    testing::Mock::AllowLeak(&mock_sys_call);
    ON_CALL(mock_sys_call, setsockopt).WillByDefault(testing::Return(-15));

    EXPECT_EXIT(setUdpRecvBufSize(1, 1, mock_sys_call), ::testing::ExitedWithCode(-15 & 0xFF), "");

    auto logger = spdlog::get(SERVER_NAME);
    logger->flush();
    std::ifstream log(MOCK_LOG);
    std::string line;
    std::getline(log, line);

    EXPECT_TRUE(line.find("setsockopt SO_RCVBUF failed") != std::string::npos);
}

// setUdpPortReuseOption
TEST_F(MockLog, setUdpPortReuseOptionDeathTest) {
    MockSysCallClass mock_sys_call;
    testing::Mock::AllowLeak(&mock_sys_call);
    ON_CALL(mock_sys_call, setsockopt).WillByDefault(testing::Return(-12));

    EXPECT_EXIT(setUdpPortReuseOption(1, mock_sys_call), ::testing::ExitedWithCode(-12 & 0xFF), "");

    auto logger = spdlog::get(SERVER_NAME);
    logger->flush();
    std::ifstream log(MOCK_LOG);
    std::string line;
    std::getline(log, line);

    EXPECT_TRUE(line.find("setsockopt SO_REUSEPORT failed") != std::string::npos);
}

// createSocket
TEST_F(MockLog, createSocketSucceed) {
    const YAML::Node& config = YAML::Load("{port: 8888, other: something}");
    MockSysCallClass mock_sys_call;
    EXPECT_CALL(mock_sys_call, socket)
        .WillOnce(testing::Return(0))
        .WillOnce(testing::Return(-2))
        .WillRepeatedly(testing::Return(1));
    EXPECT_CALL(mock_sys_call, bind)
        .WillOnce(testing::Return(0))
        .WillOnce(testing::Return(0))
        .WillOnce(testing::Return(-2))
        .WillRepeatedly(testing::Return(1));

    EXPECT_EQ(createSocket(config, mock_sys_call), 0);
    EXPECT_EQ(createSocket(config, mock_sys_call), -2);
    EXPECT_EQ(createSocket(config, mock_sys_call), 1);
    EXPECT_EQ(createSocket(config, mock_sys_call), 1);
}
TEST_F(MockLog, createSocketFailCreation) {
    const YAML::Node& config = YAML::Load("{port: 8888, other: something}");
    MockSysCallClass mock_sys_call;
    EXPECT_CALL(mock_sys_call, socket).WillOnce(testing::Return(-1));

    EXPECT_EQ(createSocket(config, mock_sys_call), -1);

    auto logger = spdlog::get(SERVER_NAME);
    logger->flush();
    std::ifstream log(MOCK_LOG);
    std::string line;
    std::getline(log, line);

    EXPECT_TRUE(line.find("Can't create socket") != std::string::npos);
}
TEST_F(MockLog, createSocketFailBind) {
    const YAML::Node& config = YAML::Load("{port: 8888, other: something}");
    MockSysCallClass mock_sys_call;
    EXPECT_CALL(mock_sys_call, bind).WillOnce(testing::Return(-1));

    EXPECT_EQ(createSocket(config, mock_sys_call), -1);

    auto logger = spdlog::get(SERVER_NAME);
    logger->flush();
    std::ifstream log(MOCK_LOG);
    std::string line;
    std::string pre_line;
    while (std::getline(log, line)) {
        pre_line = line;
    }

    EXPECT_TRUE(pre_line.find("Failed to bind socket.") != std::string::npos)
        << "the last log line is: " << line << std::endl;
}

} // namespace test
} // namespace syslogsrv
