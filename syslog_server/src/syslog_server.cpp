// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#include <algorithm>
#include <arpa/inet.h>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <netdb.h>
#include <regex>
#include <spdlog/sinks/hourly_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "common.h"
#include "msg_processor.h"
#include "processor_config.h"
#include "syslog_server.h"
#include "time_wrapper.h"

namespace syslogsrv {

size_t getRmemMax(const std::string& rmemm_path) {
    try {
        std::string value;
        std::ifstream inp_file;

        inp_file.open(rmemm_path);
        std::getline(inp_file, value);
        inp_file.close();

        return std::stoul(value);
    } catch (const std::exception& e) {
        auto logger = spdlog::get(SERVER_NAME);

        logger->error("failed to read rmem_max: {}", e.what());
        return MAX_UDP_RECV_BUFFER_SIZE;
    }
}

size_t getDesiredUdpRecvBufSize(size_t rmem_max) {
    // See https://man7.org/linux/man-pages/man7/socket.7.html
    //   SO_RCVBUF: Sets or gets the maximum socket receive buffer in bytes.
    //   The kernel doubles this value (to allow space for bookkeeping
    //   overhead) when it is set using setsockopt(2).
    //
    // If we don't explicitly double below, buffer size remains at rmem_max
    // and it's used for datagrams and bookkeeping. Doubling it below sets the
    // buffer used for the actual datagrams to rmem_max. Note that, if more
    // than doubled, setsockopt floors it back to rmem_max * 2; so, no need to
    // go further than doubling.
    return rmem_max * 2;
}

size_t getUdpRecvBufSize(int s, SystemInterface& sys_call) {
    int optval;
    socklen_t len = sizeof(optval);
    int r = sys_call.getsockopt(s, SOL_SOCKET, SO_RCVBUF, &optval, &len);
    assert(len == sizeof(optval));

    if (r < 0) {
        auto logger = spdlog::get(SERVER_NAME);

        logger->error("failed to get socket recv buf size: {}", strerror(errno));
        exit(r);
    }
    if (optval < 0) {
        auto logger = spdlog::get(SERVER_NAME);
        logger->error("Received invalid UDP receive buffer size value: {}", optval);
        exit(-1);
    }

    return static_cast<size_t>(optval);
}

void setUdpRecvBufSize(int s, size_t size, SystemInterface& sys_call) {
    int r = sys_call.setsockopt(s, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));

    if (r < 0) {
        auto logger = spdlog::get(SERVER_NAME);

        logger->error("setsockopt SO_RCVBUF failed: {}", strerror(errno));
        exit(r);
    }
}

// Returns the new UDP receive buffer size for the given socket
size_t setUdpRecvBufSize(const int s, SystemInterface& sys_call) {
    auto logger = spdlog::get(SERVER_NAME);

    const size_t current_udp_recv_buf_size = getUdpRecvBufSize(s, sys_call);
    const size_t desired_udp_recv_buf_size = getDesiredUdpRecvBufSize(getRmemMax(sys_call.getRmemMaxPath()));
    if (desired_udp_recv_buf_size > current_udp_recv_buf_size) {
        setUdpRecvBufSize(s, desired_udp_recv_buf_size, sys_call);
    }
    const size_t new_udp_recv_buf_size = getUdpRecvBufSize(s, sys_call);

    logger->info("Default UDP recv buf size {} bytes", current_udp_recv_buf_size);
    logger->info("Max UDP recv buf size {} bytes", desired_udp_recv_buf_size);
    logger->info("New UDP recv buf size {} bytes", new_udp_recv_buf_size);
    return new_udp_recv_buf_size;
}

void setUdpPortReuseOption(const int s, SystemInterface& sys_call) {
    int reuse = 1;
    int r = sys_call.setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuse, sizeof(reuse));

    if (r < 0) {
        auto logger = spdlog::get(SERVER_NAME);

        logger->error("setsockopt SO_REUSEPORT failed: {}", strerror(errno));
        exit(r);
    }
}

// Create a socket with port in config
int createSocket(const YAML::Node& config, SystemInterface& sys_call) {
    auto logger = spdlog::get(SERVER_NAME);

    auto s = sys_call.socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == -1) {
        logger->error("Can't create socket");
        return s;
    }
    sockaddr_in local_sock_addr = {};
    auto port = yamlAsOrDefault<int>(logger, CONFIG_PORT, config[CONFIG_PORT], 0);
    local_sock_addr.sin_family = AF_INET;
    local_sock_addr.sin_port = htons(port);
    local_sock_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    setUdpPortReuseOption(s, sys_call);

    if (sys_call.bind(s, (sockaddr*)&local_sock_addr, sizeof(local_sock_addr)) == -1) {
        logger->error("Failed to bind socket.");
        return -1;
    }

    return s;
}

void msgProducerThread(int sock, Processor::FIFOList& queue, std::shared_ptr<spdlog::logger> logger,
                       std::shared_ptr<spdlog::logger> access_logger, int worker_id, SystemInterface& sys_call,
                       TimeWrapper& time) {

    // Allocate a userspace buffer that is as large as the socket's receive buffer so that we can never
    // fail to receive a packet due to the packet being larger than the buffer we passed to `recv()`.
    const size_t buffer_len = setUdpRecvBufSize(sock, sys_call);
    std::unique_ptr<char[]> buffer(new char[buffer_len + 1]());
    size_t total_msgs_processed = 0;
    size_t last_logged_msgs_processed = 0;

    auto last_stats_time = time.now();
    while (true) {
        ssize_t recv_len = sys_call.recvfrom(sock, buffer.get(), buffer_len, 0, nullptr, nullptr);
        if (recv_len < 0) {
            logger->error("Error when receiving data");
            exit(1);
        }

        if (recv_len == 0) {
            continue;
        }

        assert(recv_len <= buffer_len);

        std::string_view buf_view{buffer.get(), (size_t)recv_len};

        // the data might be truncated
        if (recv_len == buffer_len) {
            logger->error("message is too big: {}", buf_view);
            continue;
        }

        // strip trailing "\n"
        while (!buf_view.empty() && buf_view.back() == '\n') {
            buf_view.remove_suffix(1);
        }

        size_t pos = buf_view.find(RawEvents::reqStart());
        if (pos == std::string_view::npos) {
            pos = buf_view.find(RawEvents::reqEnd());
        }
        if (pos == std::string_view::npos) {
            pos = buf_view.find(RawEvents::dataXfer());
        }
        if (pos == std::string_view::npos) {
            pos = buf_view.find(RawEvents::activeReqs());
        }
        if (pos == std::string_view::npos) {
            pos = buf_view.find(RawEvents::reqEnd());
        }
        if (pos != std::string_view::npos) {
            std::string_view data_start = buf_view.substr(pos);
            if (!queue.try_enqueue(std::string(data_start))) {
                logger->error("Queue is full, dropping message: {}", data_start);
            }
            logger->debug("haproxy logged command: {}", buf_view);
        } else if (buf_view[0] == '{') {
            // JSON line from HAProxy
            access_logger->info("{}", buf_view);
        } else {
            // Logs from Lua
            logger->info("haproxy logged message: {}", buf_view);
        }

        ++total_msgs_processed;
        const auto now = time.now();
        if (now - last_stats_time > STATS_LOG_INTERVAL) {
            const size_t new_msgs_processed = total_msgs_processed - last_logged_msgs_processed;
            logger->info("Msg Producer Thread - current queue size={}, msgs processed since last log={}, worker_id={}",
                         queue.size_approx(), new_msgs_processed, worker_id);
            last_logged_msgs_processed = total_msgs_processed;
            last_stats_time = now;
        }
    }
}

void startSyslogServer(YAML::Node config, int worker_id) {
    auto logger = spdlog::get(SERVER_NAME);
    auto access_logger = spdlog::get(ACCESS_LOG);

    logger->info("started the child syslog server {} with pid {}", worker_id, getpid());

    try {
        SysCallClass sys_call;
        auto s = createSocket(config, sys_call); // this is the socket that we listen to.
        if (s == -1) {
            logger->error("Failed to create socket");
        }

        // create shared queue between consumer (Processor object) and producer:
        int msg_queue_size = DEFAULT_MSG_QUEUE_SIZE;
        if (const auto& node = config[CONFIG_MSG_QUEUE_SIZE]) {
            msg_queue_size = yamlAsOrDefault<int>(logger, CONFIG_MSG_QUEUE_SIZE, node, DEFAULT_MSG_QUEUE_SIZE);
        }

        Processor::FIFOList message_queue(msg_queue_size);
        TimeWrapper time;

        // create & start message consumer worker
        auto net = std::make_unique<NetClass>();
        Processor worker(message_queue, config, worker_id, time, std::move(net));
        worker.start();

        // read incoming HAProxy messages forever & dispatch to workers' queues
        msgProducerThread(s, message_queue, logger, access_logger, worker_id, sys_call, time);
    } catch (const std::exception& e) {
        logger->error("Exception in syslog-server {}: {}", worker_id, e.what());
    }
}

} // namespace syslogsrv
