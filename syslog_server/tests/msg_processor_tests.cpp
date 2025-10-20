// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#include <memory>
#include <spdlog/sinks/stdout_sinks.h>

#include "msg_processor.h"
#include "test_common.h"
#include "time_wrapper.h"

namespace syslogsrv {
namespace test {

class MockNetInterface : public NetInterface {
  public:
    MOCK_METHOD(int, getaddrinfo, (const char*, const char*, const struct addrinfo*, struct addrinfo**), (override));
    MOCK_METHOD(void, freeaddrinfo, (addrinfo*), (override));
    MOCK_METHOD(std::string, getIpAddressBySockAddr, (const sockaddr* const saddr), (override));
    MOCK_METHOD(redisAsyncContext*, redisAsyncConnect, (const char*, int), (override));
    MOCK_METHOD(int, redisLibevAttach, (EV_P_ redisAsyncContext*), (override));
    MOCK_METHOD(void, redisAsyncDisconnect, (redisAsyncContext*), (override));
    MOCK_METHOD(int, redisAsyncCommand, (redisAsyncContext*, redisCallbackFn*, void*, const char*), (override));
    MOCK_METHOD(void, redisAsyncFree, (redisAsyncContext*), (override));
};

struct TestLogger {
    TestLogger() { spdlog::stdout_logger_mt(SERVER_NAME); }
    ~TestLogger() { spdlog::drop(SERVER_NAME); }
};

TEST(msg_processor, connects_to_redis_on_flush_if_enough_time_has_passed_since_last_connect) {
    TestLogger testlog;
    auto net = std::make_unique<testing::StrictMock<MockNetInterface>>();
    EXPECT_CALL(*net, redisAsyncFree).WillRepeatedly(testing::Return());
    EXPECT_CALL(*net, redisAsyncConnect).Times(testing::Exactly(1));

    const int check_conn_interval_sec = 30;
    const std::string config_yaml = std::string("{ redis_check_conn_interval_sec: ") +
                                    std::to_string(check_conn_interval_sec) +
                                    ", endpoint: localdev.dockerdc, redis_server: localhost:9004, redis_qos_ttl: 2, "
                                    "redis_qos_conn_ttl: 60, reqs_queue_drop_percentage_when_full: 20 }";
    Processor::FIFOList mq(1);

    std::chrono::seconds now_seconds(100);
    TimeWrapper time([&now_seconds]() { return std::chrono::system_clock::time_point(now_seconds); });

    const YAML::Node& config = YAML::Load(config_yaml);
    Processor proc(mq, config, 0, time, std::move(net));
    proc.m_redis_qos_ttl = 2;

    proc.m_qos_redis_commands[{"user_AKIAIOSFODNN7EXAMPL1", time.now(), "GET"}] = 10;
    proc.m_last_redis_connect_time = time.now();

    now_seconds += std::chrono::seconds(check_conn_interval_sec + 10);
    proc.sendToRedisQos();

    EXPECT_EQ(proc.m_qos_redis_commands.size(), 0);
}

TEST(msg_processor, doesnt_connect_to_redis_on_flush_if_there_was_a_recent_connect_attempt) {
    TestLogger testlog;
    auto net = std::make_unique<testing::StrictMock<MockNetInterface>>();
    EXPECT_CALL(*net, redisAsyncFree).WillRepeatedly(testing::Return());
    EXPECT_CALL(*net, redisAsyncConnect).Times(testing::Exactly(0));

    const int check_conn_interval_sec = 30;
    const std::string config_yaml = std::string("{ redis_check_conn_interval_sec: ") +
                                    std::to_string(check_conn_interval_sec) +
                                    ", endpoint: localdev.dockerdc, redis_server: localhost:9004, redis_qos_ttl: 2, "
                                    "redis_qos_conn_ttl: 60, reqs_queue_drop_percentage_when_full: 20 }";
    Processor::FIFOList mq(1);

    std::chrono::seconds now_seconds(100);
    TimeWrapper time([&now_seconds]() { return std::chrono::system_clock::time_point(now_seconds); });

    const YAML::Node& config = YAML::Load(config_yaml);
    Processor proc(mq, config, 0, time, std::move(net));
    proc.m_last_redis_connect_time = time.now();

    now_seconds += std::chrono::seconds(check_conn_interval_sec - 10);
    proc.sendToRedisQos();
}

TEST(redis_cmd_key, different_users_produce_different_hashes) {
    auto time_now = TimeWrapper().now();
    Processor::RedisCmdKey key1 = {"user_AKIAIOSFODNN7EXAMPL1", time_now, "GET"};
    Processor::RedisCmdKey key2 = {"user_AKIAIOSFODNN7EXAMPL2", time_now, "GET"};

    Processor::RedisCmdKeyHash hash;
    EXPECT_NE(key1, key2);
    EXPECT_NE(hash(key1), hash(key2));
}

TEST(redis_cmd_key, different_timestamps_produce_different_hashes) {
    auto time_now = TimeWrapper().now();
    Processor::RedisCmdKey key1 = {"user_AKIAIOSFODNN7EXAMPL1", time_now, "GET"};
    Processor::RedisCmdKey key2 = {"user_AKIAIOSFODNN7EXAMPL1", time_now + std::chrono::seconds(3), "GET"};

    Processor::RedisCmdKeyHash hash;
    EXPECT_NE(key1, key2);
    EXPECT_NE(hash(key1), hash(key2));
}

TEST(redis_cmd_key, different_categories_produce_different_hashes) {
    auto time_now = TimeWrapper().now();
    Processor::RedisCmdKey key1 = {"user_AKIAIOSFODNN7EXAMPL1", time_now, "GET"};
    Processor::RedisCmdKey key2 = {"user_AKIAIOSFODNN7EXAMPL1", time_now, "PUT"};

    Processor::RedisCmdKeyHash hash;
    EXPECT_NE(key1, key2);
    EXPECT_NE(hash(key1), hash(key2));
}

TEST(redis_cmd_key, keys_are_equivalent_when_timestamps_differ_slightly_within_a_second) {
    // As of C++20, system_clock is defined to count time since the unix epoch (midnight on 01/01/1970)
    // so it's guaranteed to count from the start of a second, meaning that 997ms is the same second as 987ms.
    std::chrono::system_clock::time_point time1(std::chrono::milliseconds(987));
    std::chrono::system_clock::time_point time2(std::chrono::milliseconds(997));
    Processor::RedisCmdKey key1 = {"user_AKIAIOSFODNN7EXAMPL1", time1, "GET"};
    Processor::RedisCmdKey key2 = {"user_AKIAIOSFODNN7EXAMPL1", time2, "GET"};

    Processor::RedisCmdKeyHash hash;
    EXPECT_EQ(key1, key2);
    EXPECT_EQ(hash(key1), hash(key2));
}

TEST(redis_cmd_key, keys_are_not_equivalent_when_timestamps_differ_slightly_across_seconds) {
    std::chrono::system_clock::time_point time1(std::chrono::milliseconds(997));
    std::chrono::system_clock::time_point time2(std::chrono::milliseconds(1007));
    Processor::RedisCmdKey key1 = {"user_AKIAIOSFODNN7EXAMPL1", time1, "GET"};
    Processor::RedisCmdKey key2 = {"user_AKIAIOSFODNN7EXAMPL1", time2, "GET"};

    Processor::RedisCmdKeyHash hash;
    EXPECT_NE(key1, key2);
    EXPECT_NE(hash(key1), hash(key2));
}

} // namespace test
} // namespace syslogsrv
