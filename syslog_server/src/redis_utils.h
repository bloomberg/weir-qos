// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#ifndef INCLUDED_REDIS_UTILS
#define INCLUDED_REDIS_UTILS

#include <arpa/inet.h>
#include <async.h>
#include <condition_variable>
#include <ev.h>
#include <functional>
#include <hiredis.h>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <spdlog/spdlog.h>
#include <string>

#include "common.h"

namespace syslogsrv {

namespace test {
FORWARD_DECLARE_TEST(getIpAddressBySockAddr, BothIPv4AndIpV6ResolveToSensibleStrings);
FORWARD_DECLARE_TEST(RedisServerConnection, constructor);
FORWARD_DECLARE_TEST(RedisReconnect, DoesntResolveDnsIfNotAlreadyConnected);
FORWARD_DECLARE_TEST(RedisReconnect, ReconnectOnlyRequestedIfDnsResolutionSucceeds);
FORWARD_DECLARE_TEST(RedisReconnect, ReconnectIfIpChanges);
FORWARD_DECLARE_TEST(RedisReconnect, DontReconnectIfIpIsTheSame);
FORWARD_DECLARE_TEST(reconnectIfNeededTest, Normal);
FORWARD_DECLARE_TEST(MockLog, checkRedisServerConnThreadConnected);
FORWARD_DECLARE_TEST(MockLog, connectStatusCheck);
FORWARD_DECLARE_TEST(MockLog, connectAsyncErr);
FORWARD_DECLARE_TEST(MockLog, connectRedisNoOk);
FORWARD_DECLARE_TEST(MockLog, connectRedisOk);
FORWARD_DECLARE_TEST(MockLog, connectCallbackRedisNoOk);
FORWARD_DECLARE_TEST(MockLog, connectCallbackRedisOk);
FORWARD_DECLARE_TEST(MockLog, disconnectCallbackRedisNoOk);
FORWARD_DECLARE_TEST(MockLog, disconnectCallbackRedisOk);
FORWARD_DECLARE_TEST(MockLog, replyCallbackNull);
FORWARD_DECLARE_TEST(MockLog, replyCallbackNoNull);
FORWARD_DECLARE_TEST(MockLog, addCommandNoRedis);
FORWARD_DECLARE_TEST(MockLog, addCommandRedis);
} // namespace test

enum class RedisConnectionState {
    DISCONNECTING,
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
};

class NetInterface;

// Wraps the connection to the Redis server.
// Handles asynchronous submission of commands to the server, as well as the
// reconnect logic necessary to handle the active server going down.
//
// WARNING: A RedisServerConnection object can be used only by 1 thread.
//          "A redis[Async]Context is not thread-safe.":
//          see https://github.com/redis/hiredis.
class RedisServerConnection {
  private:
    FRIEND_TEST(test::getIpAddressBySockAddr, BothIPv4AndIpV6ResolveToSensibleStrings);
    FRIEND_TEST(test::RedisServerConnection, constructor);
    FRIEND_TEST(test::RedisReconnect, DoesntResolveDnsIfNotAlreadyConnected);
    FRIEND_TEST(test::RedisReconnect, ReconnectOnlyRequestedIfDnsResolutionSucceeds);
    FRIEND_TEST(test::RedisReconnect, ReconnectIfIpChanges);
    FRIEND_TEST(test::RedisReconnect, DontReconnectIfIpIsTheSame);
    FRIEND_TEST(test::reconnectIfNeededTest, Normal);
    FRIEND_TEST(test::MockLog, checkRedisServerConnThreadConnected);
    FRIEND_TEST(test::MockLog, connectStatusCheck);
    FRIEND_TEST(test::MockLog, connectAsyncErr);
    FRIEND_TEST(test::MockLog, connectRedisNoOk);
    FRIEND_TEST(test::MockLog, connectRedisOk);
    FRIEND_TEST(test::MockLog, connectCallbackRedisNoOk);
    FRIEND_TEST(test::MockLog, connectCallbackRedisOk);
    FRIEND_TEST(test::MockLog, disconnectCallbackRedisNoOk);
    FRIEND_TEST(test::MockLog, disconnectCallbackRedisOk);
    FRIEND_TEST(test::MockLog, replyCallbackNull);
    FRIEND_TEST(test::MockLog, replyCallbackNoNull);
    FRIEND_TEST(test::MockLog, addCommandNoRedis);
    FRIEND_TEST(test::MockLog, addCommandRedis);

    // logging
    std::shared_ptr<spdlog::logger> m_logger;

    // connection details
    redisAsyncContext* m_async_context = nullptr;
    std::string m_conn_id;
    std::string m_redis_addr;
    std::string m_redis_ip;
    int m_redis_port;
    RedisConnectionState m_connection_status = RedisConnectionState::DISCONNECTED;
    bool m_needs_reconnect = false;
    struct ev_loop* m_loop = nullptr;

    // stats
    uint64_t m_total_sent_cnt = 0;
    uint64_t m_total_sent_failure = 0;
    uint64_t m_total_recv_cnt = 0;
    uint64_t m_total_recv_failure = 0;
    uint64_t m_total_conns_requested = 0;
    uint64_t m_total_conns_made = 0;
    uint64_t m_total_conns_failed = 0;
    uint64_t m_total_conns_success = 0;
    uint64_t m_total_conn_drops = 0;
    uint64_t m_total_reconnects = 0;

    // async callback functions
    // these callbacks are required by hiredis APIs and follow the hiredis format
    static void replyCallback(redisAsyncContext* c, void* r, void* privdata);
    static void connectCallback(const redisAsyncContext* c, int status);
    static void disconnectCallback(const redisAsyncContext* c, int status);

    std::unique_ptr<NetInterface> m_redis_net;

  public:
    RedisServerConnection(std::string host_addr, int host_port, std::unique_ptr<NetInterface> net);
    ~RedisServerConnection();

    RedisServerConnection(const RedisServerConnection&) = delete;
    RedisServerConnection& operator=(const RedisServerConnection&) = delete;

    // connect to the Redis server at redis_addr::redis_port
    void connect();
    bool connected() const { return m_connection_status == RedisConnectionState::CONNECTED; }

    // Do the required DNS lookups to determine if a reconnect is necessary.
    // Updates an internal flag accordingly, which is used by `reconnectIfNeeded()`
    // to enact the reconnect at an appropriate time.
    void checkIfNeedsReconnect();

    // If reconnect is needed, disconnect to initiate re-connect.
    void reconnectIfNeeded();

    // add a Redis command to the async pipeline
    void addCommand(const std::string& cmd);

    // drain the async pipeline. Note that replies will be delivered
    // asynchronously via the callback functions listed above.
    void drainRedisCmdPipeline() {
        if (m_loop != nullptr) {
            ev_run(m_loop, EVRUN_NOWAIT);
        }
    }
};

// Wrapper interface for the redis library. Allows us to mock redis interactions for testing.
class NetInterface {
  public:
    virtual ~NetInterface() = default;
    virtual int getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res) = 0;
    virtual void freeaddrinfo(addrinfo* info) = 0;
    virtual std::string getIpAddressBySockAddr(const sockaddr* const saddr) = 0;
    virtual redisAsyncContext* redisAsyncConnect(const char* ip, int port) = 0;
    virtual int redisLibevAttach(EV_P_ redisAsyncContext* ac) = 0;
    virtual void redisAsyncDisconnect(redisAsyncContext* ac) = 0;
    virtual int redisAsyncCommand(redisAsyncContext* ac, redisCallbackFn* fn, void* privdata, const char* format) = 0;
    virtual void redisAsyncFree(redisAsyncContext* ac) = 0;
};

// NetInterface implementation using hiredis library
class NetClass : public NetInterface {
  public:
    int getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res) override;
    void freeaddrinfo(addrinfo* info) override;
    std::string getIpAddressBySockAddr(const sockaddr* const saddr) override;
    redisAsyncContext* redisAsyncConnect(const char* ip, int port) override;
    int redisLibevAttach(struct ev_loop* loop, redisAsyncContext* ac) override;
    void redisAsyncDisconnect(redisAsyncContext* ac) override;
    int redisAsyncCommand(redisAsyncContext* ac, redisCallbackFn* fn, void* privdata, const char* format) override;
    void redisAsyncFree(redisAsyncContext* ac) override;
};

} // namespace syslogsrv

#endif
