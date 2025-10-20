// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

// Ignore warnings in libev headers
// MSVC is not supported
#if defined(__GUNC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wunused-function"
#include <adapters/libev.h>
#pragma GCC diagnostic pop

#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include <adapters/libev.h>
#pragma clang diagnostic pop

#else
#include <adapters/libev.h>
#endif

#include "common.h"
#include "redis_utils.h"

namespace syslogsrv {

std::string NetClass::getIpAddressBySockAddr(const sockaddr* const saddr) {
    if (!saddr) {
        return "";
    }

    char ip[std::max(INET6_ADDRSTRLEN, INET_ADDRSTRLEN) + 1] = {'\0'};

    switch (saddr->sa_family) {
    case AF_INET: {
        sockaddr_in* addr_in = (sockaddr_in*)saddr;

        inet_ntop(AF_INET, &(addr_in->sin_addr), ip, INET_ADDRSTRLEN);
        break;
    }
    case AF_INET6: {
        sockaddr_in6* addr_in6 = (sockaddr_in6*)saddr;

        inet_ntop(AF_INET6, &(addr_in6->sin6_addr), ip, INET6_ADDRSTRLEN);
        break;
    }
    }

    return ip;
}

int NetClass::getaddrinfo(const char* node, const char* service, const addrinfo* hints, addrinfo** res) {
    return ::getaddrinfo(node, service, hints, res);
}

void NetClass::freeaddrinfo(addrinfo* info) { return ::freeaddrinfo(info); }

redisAsyncContext* NetClass::redisAsyncConnect(const char* ip, int port) { return ::redisAsyncConnect(ip, port); }

int NetClass::redisLibevAttach(struct ev_loop* loop, redisAsyncContext* ac) { return ::redisLibevAttach(loop, ac); }

void NetClass::redisAsyncDisconnect(redisAsyncContext* ac) { return ::redisAsyncDisconnect(ac); }

int NetClass::redisAsyncCommand(redisAsyncContext* ac, redisCallbackFn* fn, void* privdata, const char* format) {
    return ::redisAsyncCommand(ac, fn, privdata, format);
}

void NetClass::redisAsyncFree(redisAsyncContext* ac) {
    // This check isn't necessary from hiredis v1.1.0 onwards but
    // for compatibility with older versions we keep it here for now.
    if (ac != nullptr) {
        ::redisAsyncFree(ac);
    }
}

RedisServerConnection::RedisServerConnection(std::string host_addr, int host_port, std::unique_ptr<NetInterface> net)
    : m_redis_addr(std::move(host_addr)), m_redis_port(host_port), m_redis_net(std::move(net)) {

    m_conn_id = "QoS(" + m_redis_addr + ":" + std::to_string(m_redis_port) + ")";
    m_logger = spdlog::get(SERVER_NAME);
    if (!m_logger) {
        // If no logger with the desired name is configured, fall back to the default logger
        m_logger = spdlog::default_logger();
    }
    if (!m_redis_net) {
        m_redis_net = std::make_unique<NetClass>();
    }
}

RedisServerConnection::~RedisServerConnection() {
    m_redis_net->redisAsyncFree(m_async_context);
    m_async_context = nullptr;
}

void RedisServerConnection::checkIfNeedsReconnect() {
    if (m_connection_status != RedisConnectionState::CONNECTED || m_needs_reconnect) {
        return;
    }

    const std::string redis_port = std::to_string(this->m_redis_port);
    addrinfo* servinfo = nullptr;
    addrinfo hints = {};
    hints.ai_socktype = SOCK_STREAM;

    hints.ai_family = AF_INET;
    const int rv4 = m_redis_net->getaddrinfo(m_redis_addr.c_str(), redis_port.c_str(), &hints, &servinfo);
    if (rv4 != 0) {
        hints.ai_family = AF_INET6;
        const int rv6 = m_redis_net->getaddrinfo(m_redis_addr.c_str(), redis_port.c_str(), &hints, &servinfo);
        if (rv6 != 0) {
            m_logger->error("failed to check connectivity to {}: {}/{}", m_conn_id, gai_strerror(rv4),
                            gai_strerror(rv6));
            return;
        }
    }

    bool ip_changed = true;
    for (auto p = servinfo; p != nullptr; p = p->ai_next) {
        auto saddr = std::make_unique<unsigned char[]>(p->ai_addrlen);

        memcpy(saddr.get(), p->ai_addr, p->ai_addrlen);
        std::string curr_ip = m_redis_net->getIpAddressBySockAddr((sockaddr*)saddr.get());

        if (m_redis_ip == curr_ip) {
            ip_changed = false;
            break;
        }
    }

    if (servinfo) {
        m_redis_net->freeaddrinfo(servinfo);
        servinfo = nullptr;
    }

    m_needs_reconnect = ip_changed;
}

void RedisServerConnection::connect() {
    ++m_total_conns_requested;

    switch (m_connection_status) {
    case RedisConnectionState::CONNECTING:
        m_logger->info("waiting for pending connection attempt to {}", m_conn_id);
        return;
    case RedisConnectionState::CONNECTED:
        m_logger->error("already connected to {}", m_conn_id);
        return;
    case RedisConnectionState::DISCONNECTING:
        m_logger->info("waiting for disconnecting from {}", m_conn_id);
        return;
    case RedisConnectionState::DISCONNECTED:
        assert(m_async_context == nullptr);
        break;
    }

    ++m_total_conns_made;
    m_logger->info("initiating connection attempt to {}", m_conn_id);

    m_async_context = m_redis_net->redisAsyncConnect(m_redis_addr.c_str(), m_redis_port);
    if (m_async_context == nullptr) {
        m_logger->error("Failed to allocate an async connection context for redis");
        return;
    }
    if (m_async_context->err != 0) {
        ++m_total_conns_failed;
        m_logger->error("failed to connect to {}: {}", m_conn_id, m_async_context->errstr);
        m_redis_net->redisAsyncFree(m_async_context);
        m_async_context = nullptr;
        return;
    }

    if (m_loop != nullptr) {
        ev_loop_destroy(m_loop);
    }
    m_loop = ev_loop_new(EVFLAG_AUTO);
    int r = m_redis_net->redisLibevAttach(m_loop, m_async_context);
    if (r != REDIS_OK) {
        ++m_total_conns_failed;
        m_logger->error("failed to attach {} context: {}", m_conn_id, r);
        throw(std::runtime_error("failed to attach " + m_conn_id + " context"));
    }

    m_connection_status = RedisConnectionState::CONNECTING;
    m_async_context->data = this;
    redisAsyncSetConnectCallback(m_async_context, this->connectCallback);
    redisAsyncSetDisconnectCallback(m_async_context, this->disconnectCallback);
}

void RedisServerConnection::reconnectIfNeeded() {
    if (m_needs_reconnect) {
        if (m_connection_status == RedisConnectionState::CONNECTED) {
            m_connection_status = RedisConnectionState::DISCONNECTING;
            ++m_total_reconnects;
            m_redis_net->redisAsyncDisconnect(m_async_context);
        }
        m_needs_reconnect = false;
    }
}

void RedisServerConnection::connectCallback(const redisAsyncContext* c, int status) {
    RedisServerConnection* rsc = static_cast<RedisServerConnection*>(c->data);

    if (status != REDIS_OK) {
        rsc->m_logger->error("{} connect error: {}", rsc->m_conn_id, c->errstr);
        ++rsc->m_total_conns_failed;
        rsc->m_connection_status = RedisConnectionState::DISCONNECTED;
        rsc->m_async_context = nullptr;
        return;
    }

    ++rsc->m_total_conns_success;
    rsc->m_redis_ip = rsc->m_redis_net->getIpAddressBySockAddr((sockaddr*)c->c.saddr);
    rsc->m_connection_status = RedisConnectionState::CONNECTED;

    rsc->m_logger->info("connected to {} with IP addr {}", rsc->m_conn_id, rsc->m_redis_ip);
}

void RedisServerConnection::disconnectCallback(const redisAsyncContext* c, int status) {
    RedisServerConnection* rsc = static_cast<RedisServerConnection*>(c->data);

    rsc->m_connection_status = RedisConnectionState::DISCONNECTED;
    ++rsc->m_total_conn_drops;

    // Inside redisAsyncDisconnect, the async context gets freed automatically, which makes sense
    // when you consider it as the opposite of redisAsyncConnect which created the context.
    // Nulling-out the context pointer communicates that it is no longer valid in any other
    // code that accesses it and in particular prevents double-freeing.
    rsc->m_async_context = nullptr;

    if (status != REDIS_OK) {
        rsc->m_logger->error("{} connection failed: {}", rsc->m_conn_id, c->errstr);
    } else {
        rsc->m_logger->info("{} need to reconnect because of IP change", rsc->m_conn_id);
        rsc->connect();
    }
}

void RedisServerConnection::replyCallback(redisAsyncContext* c, void* r, void* privdata) {
    RedisServerConnection* rsc = static_cast<RedisServerConnection*>(c->data);

    ++rsc->m_total_recv_cnt;

    redisReply* reply = static_cast<redisReply*>(r);
    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        // we should get connection closed callback eventually
        rsc->m_logger->error("{} redis server reply error: {}", rsc->m_conn_id,
                             ((reply == nullptr || reply->str == nullptr) ? "null reply" : reply->str));
        ++rsc->m_total_recv_failure;
    }
}

void RedisServerConnection::addCommand(const std::string& cmd) {
    m_logger->debug("Redis command: {}", cmd);
    ++m_total_sent_cnt;

    int r = m_redis_net->redisAsyncCommand(m_async_context, this->replyCallback,
                                           nullptr, // privdata unused
                                           cmd.c_str());

    if (r != REDIS_OK) {
        // we should get connection closed callback eventually
        m_logger->error("send to {} failed: {}", m_conn_id, r);
        ++m_total_sent_failure;
    }
}

} // namespace syslogsrv
