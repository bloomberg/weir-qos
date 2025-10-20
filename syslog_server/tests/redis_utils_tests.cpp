// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#include <arpa/inet.h>
#include <memory>
#include <redis_utils.h>
#include <sys/socket.h>

#include "test_common.h"

namespace syslogsrv {

namespace test {

class MockNetClass : public NetInterface {
  public:
    MOCK_METHOD(int, getaddrinfo, (const char*, const char*, const addrinfo*, addrinfo**), (override));
    MOCK_METHOD(void, freeaddrinfo, (addrinfo*), (override));
    MOCK_METHOD(std::string, getIpAddressBySockAddr, (const sockaddr* const saddr), (override));
    MOCK_METHOD(redisAsyncContext*, redisAsyncConnect, (const char*, int), (override));
    MOCK_METHOD(int, redisLibevAttach, (EV_P_ redisAsyncContext*), (override));
    MOCK_METHOD(void, redisAsyncDisconnect, (redisAsyncContext*), (override));
    MOCK_METHOD(int, redisAsyncCommand, (redisAsyncContext*, redisCallbackFn*, void*, const char*), (override));
    MOCK_METHOD(void, redisAsyncFree, (redisAsyncContext*), (override));
};

TEST(getIpAddressBySockAddr, BothIPv4AndIpV6ResolveToSensibleStrings) {
    RedisServerConnection conn("127.0.0.1", 1234, nullptr);

    {
        sockaddr_in saddr;
        saddr.sin_port = htons(1234);
        saddr.sin_family = AF_INET;
        inet_aton("127.0.0.1", &saddr.sin_addr);

        EXPECT_EQ(conn.m_redis_net->getIpAddressBySockAddr((sockaddr*)&saddr), "127.0.0.1");
    }
    {
        sockaddr_in6 saddr;
        saddr.sin6_port = htons(1234);
        saddr.sin6_family = AF_INET6;
        inet_pton(AF_INET6, "0000:0000:0000:0000:0000:0000:7f00:0001", &saddr.sin6_addr);

        EXPECT_EQ(conn.m_redis_net->getIpAddressBySockAddr((sockaddr*)&saddr), "::127.0.0.1");
    }
}

TEST(RedisServerConnection, constructor) {
    RedisServerConnection conn("127.0.0.1", 1234, nullptr);

    EXPECT_EQ(conn.m_redis_addr, "127.0.0.1");
    EXPECT_EQ(conn.m_redis_port, 1234);
    EXPECT_EQ(conn.m_conn_id, "QoS(127.0.0.1:1234)");
}

TEST(RedisReconnect, DoesntResolveDnsIfNotAlreadyConnected) {
    auto mock_net = std::make_unique<MockNetClass>();
    EXPECT_CALL(*mock_net, getaddrinfo).Times(0);
    EXPECT_CALL(*mock_net, getIpAddressBySockAddr).Times(0);

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));

    {
        conn.m_connection_status = RedisConnectionState::DISCONNECTING;
        conn.m_needs_reconnect = false;
        conn.checkIfNeedsReconnect();
        EXPECT_FALSE(conn.m_needs_reconnect);
    }
    {
        conn.m_connection_status = RedisConnectionState::DISCONNECTED;
        conn.m_needs_reconnect = false;
        conn.checkIfNeedsReconnect();
        EXPECT_FALSE(conn.m_needs_reconnect);
    }
    {
        conn.m_connection_status = RedisConnectionState::CONNECTING;
        conn.m_needs_reconnect = false;
        conn.checkIfNeedsReconnect();
        EXPECT_FALSE(conn.m_needs_reconnect);
    }
}

TEST(RedisReconnect, ReconnectOnlyRequestedIfDnsResolutionSucceeds) {
    auto mock_net = std::make_unique<MockNetClass>();
    MockNetClass* p = mock_net.get();
    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));

    {
        EXPECT_CALL(*p, getaddrinfo).WillOnce(testing::Return(0));

        conn.m_connection_status = RedisConnectionState::CONNECTED;
        conn.m_needs_reconnect = false;
        conn.checkIfNeedsReconnect();

        EXPECT_TRUE(conn.m_needs_reconnect);
    }
    {
        EXPECT_CALL(*p, getaddrinfo).WillOnce(testing::Return(1)).WillOnce(testing::Return(0));

        conn.m_connection_status = RedisConnectionState::CONNECTED;
        conn.m_needs_reconnect = false;
        conn.checkIfNeedsReconnect();

        EXPECT_TRUE(conn.m_needs_reconnect);
    }
    {
        EXPECT_CALL(*p, getaddrinfo).WillOnce(testing::Return(1)).WillOnce(testing::Return(1));

        conn.m_connection_status = RedisConnectionState::CONNECTED;
        conn.m_needs_reconnect = false;
        conn.checkIfNeedsReconnect();

        EXPECT_FALSE(conn.m_needs_reconnect);
    }
}

TEST(RedisReconnect, ReconnectIfIpChanges) {
    auto mock_net = std::make_unique<MockNetClass>();
    sockaddr_in result_addr = {};
    inet_pton(AF_INET, "2.2.2.2", &result_addr.sin_addr);
    addrinfo* result = new addrinfo{
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
        .ai_addrlen = sizeof(result_addr),
        .ai_addr = (sockaddr*)&result_addr,
    };
    EXPECT_CALL(*mock_net, getaddrinfo).WillOnce(testing::DoAll(testing::SetArgPointee<3>(result), testing::Return(0)));
    EXPECT_CALL(*mock_net, getIpAddressBySockAddr).WillOnce(testing::Return("2.2.2.2"));

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));
    conn.m_connection_status = RedisConnectionState::CONNECTED;
    conn.m_needs_reconnect = false;
    conn.m_redis_ip = "1.1.1.1";
    conn.checkIfNeedsReconnect();

    EXPECT_TRUE(conn.m_needs_reconnect);
}

TEST(RedisReconnect, DontReconnectIfIpIsTheSame) {
    auto mock_net = std::make_unique<MockNetClass>();
    sockaddr_in result_addr = {};
    inet_pton(AF_INET, "1.1.1.1", &result_addr.sin_addr);
    addrinfo result = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
        .ai_addrlen = sizeof(result_addr),
        .ai_addr = (sockaddr*)&result_addr,
    };
    EXPECT_CALL(*mock_net, getaddrinfo)
        .WillOnce(testing::DoAll(testing::SetArgPointee<3>(&result), testing::Return(0)));
    EXPECT_CALL(*mock_net, getIpAddressBySockAddr).WillOnce(testing::Return("1.1.1.1"));
    EXPECT_CALL(*mock_net, freeaddrinfo).Times(1);

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));
    conn.m_connection_status = RedisConnectionState::CONNECTED;
    conn.m_needs_reconnect = false;
    conn.m_redis_ip = "1.1.1.1";
    conn.checkIfNeedsReconnect();

    EXPECT_FALSE(conn.m_needs_reconnect);
}

// connect
TEST_F(MockLog, connectStatusCheck) {
    RedisServerConnection conn("127.0.0.1", 1234, nullptr);
    uint64_t total_conns_requested = 3;
    uint64_t total_conns_made = 3;

    auto logger = spdlog::get(SERVER_NAME);
    std::string line;
    std::string pre_line;
    // case CONNECTING
    {
        conn.m_total_conns_requested = total_conns_requested;
        conn.m_total_conns_made = total_conns_made;
        conn.m_connection_status = RedisConnectionState::CONNECTING;
        conn.connect();
        std::ifstream log(MOCK_LOG);
        logger->flush();
        while (std::getline(log, line)) {
            pre_line = line;
        }
        EXPECT_TRUE(pre_line.find("waiting for pending connection attempt to") != std::string::npos)
            << "the last log line is: " << pre_line << std::endl;
        EXPECT_EQ(conn.m_total_conns_requested, total_conns_requested + 1);
        EXPECT_EQ(conn.m_total_conns_made, total_conns_made);
    }
    // case CONNECTED
    {
        conn.m_total_conns_requested = total_conns_requested;
        conn.m_total_conns_made = total_conns_made;
        conn.m_connection_status = RedisConnectionState::CONNECTED;
        conn.connect();
        std::ifstream log(MOCK_LOG);
        logger->flush();
        while (std::getline(log, line)) {
            pre_line = line;
        }
        EXPECT_TRUE(pre_line.find("already connected to") != std::string::npos)
            << "the last log line is: " << pre_line << std::endl;
        EXPECT_EQ(conn.m_total_conns_requested, total_conns_requested + 1);
        EXPECT_EQ(conn.m_total_conns_made, total_conns_made);
    }
    // case DISCONNECTING
    {
        conn.m_total_conns_requested = total_conns_requested;
        conn.m_total_conns_made = total_conns_made;
        conn.m_connection_status = RedisConnectionState::DISCONNECTING;
        conn.connect();
        std::ifstream log(MOCK_LOG);
        logger->flush();
        while (std::getline(log, line)) {
            pre_line = line;
        }
        EXPECT_TRUE(pre_line.find("waiting for disconnecting from") != std::string::npos)
            << "the last log line is: " << pre_line << std::endl;
        EXPECT_EQ(conn.m_total_conns_requested, total_conns_requested + 1);
        EXPECT_EQ(conn.m_total_conns_made, total_conns_made);
    }
}
TEST_F(MockLog, connectAsyncErr) {
    auto logger = spdlog::get(SERVER_NAME);
    std::string line;
    std::string pre_line;

    auto mock_net = std::make_unique<MockNetClass>();

    redisAsyncContext* rac = (redisAsyncContext*)malloc(sizeof(redisAsyncContext)); // Freed by hiredis
    *rac = {};
    rac->err = 3;
    char s[] = "error code 3";
    rac->errstr = s;
    EXPECT_CALL(*mock_net, redisAsyncConnect).WillOnce(testing::Return(rac));

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));

    uint64_t total_conns_requested = 5;
    uint64_t total_conns_made = 3;
    uint64_t total_conns_failed = 1;

    conn.m_total_conns_requested = total_conns_requested;
    conn.m_total_conns_made = total_conns_made;
    conn.m_total_conns_failed = total_conns_failed;
    conn.m_connection_status = RedisConnectionState::DISCONNECTED;
    conn.m_conn_id = "fake_id";

    conn.connect();

    logger->flush();
    std::ifstream log(MOCK_LOG);
    while (std::getline(log, line)) {
        pre_line = line;
    }
    EXPECT_TRUE(pre_line.find("failed to connect to") != std::string::npos)
        << "the last log line is: " << pre_line << std::endl;

    EXPECT_EQ(conn.m_total_conns_requested, total_conns_requested + 1);
    EXPECT_EQ(conn.m_total_conns_made, total_conns_made + 1);
    EXPECT_EQ(conn.m_total_conns_failed, total_conns_failed + 1);
}
TEST_F(MockLog, connectRedisNoOk) {
    auto logger = spdlog::get(SERVER_NAME);
    std::string line;
    std::string pre_line;

    auto mock_net = std::make_unique<MockNetClass>();

    redisAsyncContext rac;
    rac.err = 0;
    EXPECT_CALL(*mock_net, redisAsyncConnect).WillOnce(testing::Return(&rac));
    EXPECT_CALL(*mock_net, redisLibevAttach).WillOnce(testing::Return(REDIS_ERR));

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));

    uint64_t total_conns_requested = 5;
    uint64_t total_conns_made = 3;
    uint64_t total_conns_failed = 1;

    conn.m_total_conns_requested = total_conns_requested;
    conn.m_total_conns_made = total_conns_made;
    conn.m_total_conns_failed = total_conns_failed;
    conn.m_connection_status = RedisConnectionState::DISCONNECTED;
    conn.m_conn_id = "fake_id";

    EXPECT_THROW(conn.connect(), std::runtime_error);

    logger->flush();
    std::ifstream log(MOCK_LOG);
    while (std::getline(log, line)) {
        pre_line = line;
    }
    EXPECT_TRUE(pre_line.find("failed to attach") != std::string::npos)
        << "the last log line is: " << pre_line << std::endl;

    EXPECT_EQ(conn.m_total_conns_requested, total_conns_requested + 1);
    EXPECT_EQ(conn.m_total_conns_made, total_conns_made + 1);
    EXPECT_EQ(conn.m_total_conns_failed, total_conns_failed + 1);
}

TEST_F(MockLog, connectRedisOk) {
    auto mock_net = std::make_unique<MockNetClass>();

    redisAsyncContext rac = {};
    rac.err = 0;
    EXPECT_CALL(*mock_net, redisAsyncConnect).WillOnce(testing::Return(&rac));
    EXPECT_CALL(*mock_net, redisLibevAttach).WillOnce(testing::Return(REDIS_OK));

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));

    uint64_t total_conns_requested = 5;
    uint64_t total_conns_made = 3;
    uint64_t total_conns_failed = 1;

    conn.m_total_conns_requested = total_conns_requested;
    conn.m_total_conns_made = total_conns_made;
    conn.m_total_conns_failed = total_conns_failed;
    conn.m_connection_status = RedisConnectionState::DISCONNECTED;
    conn.m_conn_id = "fake_id";

    conn.connect();

    EXPECT_EQ(conn.m_total_conns_requested, total_conns_requested + 1);
    EXPECT_EQ(conn.m_total_conns_made, total_conns_made + 1);
    EXPECT_EQ(conn.m_total_conns_failed, total_conns_failed);
    EXPECT_EQ(conn.m_connection_status, RedisConnectionState::CONNECTING);
}

// reconnectIfNeeded
TEST(reconnectIfNeededTest, Normal) {
    auto mock_net = std::make_unique<MockNetClass>();
    MockNetClass* p = mock_net.get();

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));
    uint64_t total_reconnects = 3;

    {
        conn.m_total_reconnects = total_reconnects;
        conn.m_needs_reconnect = true;
        conn.m_connection_status = RedisConnectionState::CONNECTED;
        EXPECT_CALL(*p, redisAsyncDisconnect).Times(1);

        conn.reconnectIfNeeded();

        EXPECT_EQ(conn.m_needs_reconnect, false);
        EXPECT_EQ(conn.m_connection_status, RedisConnectionState::DISCONNECTING);
        EXPECT_EQ(conn.m_total_reconnects, total_reconnects + 1);
    }
    {
        conn.m_total_reconnects = total_reconnects;
        conn.m_needs_reconnect = true;
        conn.m_connection_status = RedisConnectionState::CONNECTING;
        EXPECT_CALL(*p, redisAsyncDisconnect).Times(0);

        conn.reconnectIfNeeded();

        EXPECT_EQ(conn.m_needs_reconnect, false);
        EXPECT_EQ(conn.m_connection_status, RedisConnectionState::CONNECTING);
        EXPECT_EQ(conn.m_total_reconnects, total_reconnects);
    }
    {
        conn.m_total_reconnects = total_reconnects;
        conn.m_needs_reconnect = true;
        conn.m_connection_status = RedisConnectionState::DISCONNECTED;
        EXPECT_CALL(*p, redisAsyncDisconnect).Times(0);

        conn.reconnectIfNeeded();

        EXPECT_EQ(conn.m_needs_reconnect, false);
        EXPECT_EQ(conn.m_connection_status, RedisConnectionState::DISCONNECTED);
        EXPECT_EQ(conn.m_total_reconnects, total_reconnects);
    }
    {
        conn.m_total_reconnects = total_reconnects;
        conn.m_needs_reconnect = true;
        conn.m_connection_status = RedisConnectionState::DISCONNECTING;
        EXPECT_CALL(*p, redisAsyncDisconnect).Times(0);

        conn.reconnectIfNeeded();

        EXPECT_EQ(conn.m_needs_reconnect, false);
        EXPECT_EQ(conn.m_connection_status, RedisConnectionState::DISCONNECTING);
        EXPECT_EQ(conn.m_total_reconnects, total_reconnects);
    }

    {
        conn.m_total_reconnects = total_reconnects;
        conn.m_needs_reconnect = false;
        conn.m_connection_status = RedisConnectionState::CONNECTED;
        EXPECT_CALL(*p, redisAsyncDisconnect).Times(0);

        conn.reconnectIfNeeded();

        EXPECT_EQ(conn.m_total_reconnects, total_reconnects);
    }
    {
        conn.m_total_reconnects = total_reconnects;
        conn.m_needs_reconnect = false;
        conn.m_connection_status = RedisConnectionState::CONNECTING;
        EXPECT_CALL(*p, redisAsyncDisconnect).Times(0);

        conn.reconnectIfNeeded();

        EXPECT_EQ(conn.m_total_reconnects, total_reconnects);
    }
    {
        conn.m_total_reconnects = total_reconnects;
        conn.m_needs_reconnect = false;
        conn.m_connection_status = RedisConnectionState::DISCONNECTED;
        EXPECT_CALL(*p, redisAsyncDisconnect).Times(0);

        conn.reconnectIfNeeded();

        EXPECT_EQ(conn.m_total_reconnects, total_reconnects);
    }
    {
        conn.m_total_reconnects = total_reconnects;
        conn.m_needs_reconnect = false;
        conn.m_connection_status = RedisConnectionState::DISCONNECTING;
        EXPECT_CALL(*p, redisAsyncDisconnect).Times(0);

        conn.reconnectIfNeeded();

        EXPECT_EQ(conn.m_total_reconnects, total_reconnects);
    }
}

// connectCallback
TEST_F(MockLog, connectCallbackRedisNoOk) {
    uint64_t total_conns_success = 3;
    uint64_t total_conns_failed = 5;
    auto logger = spdlog::get(SERVER_NAME);
    std::string line;
    std::string pre_line;

    auto mock_net = std::make_unique<MockNetClass>();

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));
    conn.m_total_conns_success = total_conns_success;
    conn.m_total_conns_failed = total_conns_failed;
    conn.m_conn_id = "fake_id";

    redisAsyncContext rac;
    rac.data = static_cast<RedisServerConnection*>(&conn);
    char s[] = "error message";
    rac.errstr = s;

    conn.connectCallback(&rac, REDIS_ERR);

    logger->flush();
    std::ifstream log(MOCK_LOG);
    while (std::getline(log, line)) {
        pre_line = line;
    }
    EXPECT_TRUE(pre_line.find("connect error:") != std::string::npos)
        << "the last log line is: " << pre_line << std::endl;
    EXPECT_EQ(conn.m_total_conns_success, total_conns_success);
    EXPECT_EQ(conn.m_total_conns_failed, total_conns_failed + 1);
    EXPECT_EQ(conn.m_connection_status, RedisConnectionState::DISCONNECTED);
}
TEST_F(MockLog, connectCallbackRedisOk) {
    uint64_t total_conns_success = 3;
    uint64_t total_conns_failed = 5;
    auto logger = spdlog::get(SERVER_NAME);
    std::string line;
    std::string pre_line;

    RedisServerConnection conn("127.0.0.1", 1234, nullptr);
    conn.m_total_conns_success = total_conns_success;
    conn.m_total_conns_failed = total_conns_failed;
    conn.m_conn_id = "fake_id";

    redisAsyncContext rac;
    rac.data = static_cast<RedisServerConnection*>(&conn);
    sockaddr_in saddr;
    saddr.sin_port = htons(1234);
    saddr.sin_family = AF_INET;
    inet_aton("1.1.1.1", &saddr.sin_addr);

    // The cast below *should* just be `(sockaddr*)&saddr`, but at the time of writing we need to support older
    // versions of hiredis that have this bug: https://github.com/redis/hiredis/issues/867
    // Until we can migrate all usages to more recent versions, we just cast to the target type, whatever it is.
    rac.c.saddr = reinterpret_cast<decltype(rac.c.saddr)>(&saddr);

    conn.connectCallback(&rac, REDIS_OK);

    logger->flush();
    std::ifstream log(MOCK_LOG);
    while (std::getline(log, line)) {
        pre_line = line;
    }
    EXPECT_TRUE(pre_line.find("connected to fake_id with IP addr") != std::string::npos)
        << "the last log line is: " << pre_line << std::endl;
    EXPECT_EQ(conn.m_total_conns_success, total_conns_success + 1);
    EXPECT_EQ(conn.m_total_conns_failed, total_conns_failed);
    EXPECT_EQ(conn.m_connection_status, RedisConnectionState::CONNECTED);
    EXPECT_EQ(conn.m_redis_ip, "1.1.1.1");
}

// disconnectCallback
TEST_F(MockLog, disconnectCallbackRedisNoOk) {
    uint64_t total_conn_drops = 3;
    auto logger = spdlog::get(SERVER_NAME);
    std::string line;
    std::string pre_line;

    auto mock_net = std::make_unique<MockNetClass>();

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));
    conn.m_total_conn_drops = total_conn_drops;
    conn.m_conn_id = "fake_id";

    redisAsyncContext rac;
    rac.data = static_cast<RedisServerConnection*>(&conn);
    char s[] = "error message";
    rac.errstr = s;

    conn.disconnectCallback(&rac, REDIS_ERR);

    logger->flush();
    std::ifstream log(MOCK_LOG);
    while (std::getline(log, line)) {
        pre_line = line;
    }
    EXPECT_TRUE(pre_line.find("connection failed:") != std::string::npos)
        << "the last log line is: " << pre_line << std::endl;
    EXPECT_EQ(conn.m_total_conn_drops, total_conn_drops + 1);
    EXPECT_EQ(conn.m_connection_status, RedisConnectionState::DISCONNECTED);
}
TEST_F(MockLog, disconnectCallbackRedisOk) {
    uint64_t total_conn_drops = 3;
    auto logger = spdlog::get(SERVER_NAME);
    std::string line;

    auto mock_net = std::make_unique<MockNetClass>();
    MockNetClass* p = mock_net.get();

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));
    conn.m_total_conn_drops = total_conn_drops;
    conn.m_conn_id = "fake_id";

    redisAsyncContext* rac = (redisAsyncContext*)malloc(sizeof(redisAsyncContext)); // Freed by hiredis
    *rac = {};
    rac->data = static_cast<RedisServerConnection*>(&conn);
    rac->err = 3;
    char s[] = "error code 3";
    rac->errstr = s;
    EXPECT_CALL(*p, redisAsyncConnect).WillOnce(testing::Return(rac));

    conn.disconnectCallback(rac, REDIS_OK);

    logger->flush();
    std::ifstream log(MOCK_LOG);
    std::getline(log, line);
    EXPECT_TRUE(line.find("need to reconnect because of IP change") != std::string::npos)
        << "the last log line is: " << line << std::endl;
    EXPECT_EQ(conn.m_total_conn_drops, total_conn_drops + 1);
    EXPECT_EQ(conn.m_connection_status, RedisConnectionState::DISCONNECTED);
}

// replyCallback
TEST_F(MockLog, replyCallbackNull) {
    uint64_t total_recv_cnt = 3;
    uint64_t total_recv_failure = 5;
    auto logger = spdlog::get(SERVER_NAME);
    std::string line;

    auto mock_net = std::make_unique<MockNetClass>();

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));
    conn.m_total_recv_cnt = total_recv_cnt;
    conn.m_total_recv_failure = total_recv_failure;
    conn.m_conn_id = "fake_id";

    redisAsyncContext rac;
    rac.data = static_cast<RedisServerConnection*>(&conn);

    void* r = nullptr;
    void* pdata = nullptr;
    conn.replyCallback(&rac, r, pdata);

    logger->flush();
    std::ifstream log(MOCK_LOG);
    std::getline(log, line);
    EXPECT_TRUE(line.find("redis server reply error:") != std::string::npos)
        << "the last log line is: " << line << std::endl;
    EXPECT_EQ(conn.m_total_recv_cnt, total_recv_cnt + 1);
    EXPECT_EQ(conn.m_total_recv_failure, total_recv_failure + 1);
}
TEST_F(MockLog, replyCallbackNoNull) {
    uint64_t total_recv_cnt = 3;
    uint64_t total_recv_failure = 5;

    auto mock_net = std::make_unique<MockNetClass>();

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));
    conn.m_total_recv_cnt = total_recv_cnt;
    conn.m_total_recv_failure = total_recv_failure;
    conn.m_conn_id = "fake_id";

    redisAsyncContext rac;
    rac.data = static_cast<RedisServerConnection*>(&conn);

    void* pdata = nullptr;
    redisReply r;
    conn.replyCallback(&rac, &r, pdata);

    EXPECT_EQ(conn.m_total_recv_cnt, total_recv_cnt + 1);
    EXPECT_EQ(conn.m_total_recv_failure, total_recv_failure);
}

// addCommand
TEST_F(MockLog, addCommandNoRedis) {
    uint64_t total_sent_cnt = 3;
    uint64_t total_sent_failure = 5;
    auto logger = spdlog::get(SERVER_NAME);
    std::string line;

    auto mock_net = std::make_unique<MockNetClass>();
    MockNetClass* p = mock_net.get();

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));
    conn.m_total_sent_cnt = total_sent_cnt;
    conn.m_total_sent_failure = total_sent_failure;
    conn.m_conn_id = "fake_id";

    EXPECT_CALL(*p, redisAsyncCommand).WillOnce(testing::Return(REDIS_ERR));

    conn.addCommand("fake_command");

    logger->flush();
    std::ifstream log(MOCK_LOG);
    std::getline(log, line);
    EXPECT_TRUE(line.find("send to fake_id failed:") != std::string::npos)
        << "the last log line is: " << line << std::endl;
    EXPECT_EQ(conn.m_total_sent_cnt, total_sent_cnt + 1);
    EXPECT_EQ(conn.m_total_sent_failure, total_sent_failure + 1);
}
TEST_F(MockLog, addCommandRedis) {
    uint64_t total_sent_cnt = 3;
    uint64_t total_sent_failure = 5;

    auto mock_net = std::make_unique<MockNetClass>();
    MockNetClass* p = mock_net.get();

    RedisServerConnection conn("127.0.0.1", 1234, std::move(mock_net));
    conn.m_total_sent_cnt = total_sent_cnt;
    conn.m_total_sent_failure = total_sent_failure;

    EXPECT_CALL(*p, redisAsyncCommand).WillOnce(testing::Return(REDIS_OK));

    conn.addCommand("fake_command");

    EXPECT_EQ(conn.m_total_sent_cnt, total_sent_cnt + 1);
    EXPECT_EQ(conn.m_total_sent_failure, total_sent_failure);
}

} // namespace test
} // namespace syslogsrv
