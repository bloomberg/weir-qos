// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <mutex>
#include <spdlog/fmt/bundled/format.h>
#include <string>
#include <thread>
#include <unordered_set>

#include "common.h"
#include "msg_processor.h"
#include "processor_config.h"
#include "stringsplit.h"

namespace {
// Define an enum or struct to give names to the tokens
enum class RequestToken {
    Source,
    SourcePort,
    UserKey,
    Verb,
    Direction,
    InstanceId,
    ActiveRequests,
    RequestClass,
    Count // Sentinel value for the number of tokens
};

bool isPrintableASCII(const std::string_view key) {
    return std::all_of(key.begin(), key.end(), [](char c) { return std::isprint(static_cast<unsigned char>(c)); });
}

uint32_t getEpochSecs(const std::chrono::system_clock::time_point& time_point) {
    return std::chrono::duration_cast<std::chrono::seconds>(time_point.time_since_epoch()).count();
}
} // namespace

namespace syslogsrv {

bool Processor::RedisCmdKey::operator==(const RedisCmdKey& other) const {
    // We specifically are interested in commands differing only when they refer to events on different seconds.
    // For this reason we need both the equality and hash functions to indicate that keys are equivalent if they
    // differ only in the timestamp and refer to different points within the same second.
    // We could equivalently use std::chrono::floor instead of getEpochSecs to round off the timestamp but by using
    // getEpochSecs we also make it more clear that equivalent values will produce equivalent hashes.
    return m_user == other.m_user && getEpochSecs(m_timestamp) == getEpochSecs(other.m_timestamp) &&
           m_cat == other.m_cat;
}
std::size_t Processor::RedisCmdKeyHash::operator()(const RedisCmdKey& k) const {
    std::size_t h1 = std::hash<std::string>()(k.m_user);
    // Much like equality, we care only about distinct seconds when hashing
    std::size_t h2 = std::hash<uint32_t>()(getEpochSecs(k.m_timestamp));
    std::size_t h3 = std::hash<std::string>()(k.m_cat);
    return h1 ^ (h2 << 1) ^ (h3 << 2);
}

void Processor::setMetricsBatchingParams(const YAML::Node& config) {
    const YAML::Node& count_node = config[CONFIG_METRICS_BATCH_COUNT];
    if (count_node) {
        m_processor_batch_count =
            yamlAsOrDefault<int>(m_logger, CONFIG_METRICS_BATCH_COUNT, count_node, DEFAULT_METRICS_BATCHING_COUNT);
    }

    const YAML::Node& period_node = config[CONFIG_METRICS_BATCH_PERIOD_MSEC];
    if (period_node) {
        m_processor_batch_flush_period = std::chrono::milliseconds(yamlAsOrDefault<int>(
            m_logger, CONFIG_METRICS_BATCH_PERIOD_MSEC, period_node, DEFAULT_METRICS_BATCHING_MSEC_PERIOD));
    }

    m_logger->info("metrics_batching: count -> {}, period -> {}ms", m_processor_batch_count,
                   m_processor_batch_flush_period.count());
}

Processor::Processor(FIFOList& msg_q, const YAML::Node& config, int worker_id, const TimeWrapper& time,
                     std::unique_ptr<NetInterface> net)
    : m_haprxy_mesg_q(msg_q), m_worker_id(worker_id), m_time(time), m_redis_qos_ttl(DEFAULT_REDIS_QOS_TTL),
      m_redis_qos_conn_ttl(DEFAULT_REDIS_QOS_CONN_TTL), m_check_conn_interval(DEFAULT_CHECK_CONN_INTERVAL_SECS),
      m_qos_not_send_count(0), m_processor_batch_count(DEFAULT_METRICS_BATCHING_COUNT),
      m_processor_batch_flush_period(std::chrono::milliseconds(DEFAULT_METRICS_BATCHING_MSEC_PERIOD)) {

    m_logger = spdlog::get(SERVER_NAME);

    const auto& endpoint = config[CONFIG_ENDPOINT];
    if (endpoint) {
        m_endpoint = yamlAsOrDefault<std::string>(m_logger, CONFIG_ENDPOINT, endpoint, "");
    } else {
        throw(std::runtime_error("No endpoint configured"));
    }

    const auto& ttl = config[CONFIG_REDIS_QOS_TTL];
    if (ttl) {
        m_redis_qos_ttl = yamlAsOrDefault<int>(m_logger, CONFIG_REDIS_QOS_TTL, ttl, DEFAULT_REDIS_QOS_TTL);
    }

    const auto& conn_ttl = config[CONFIG_REDIS_QOS_CONN_TTL];
    if (conn_ttl) {
        m_redis_qos_conn_ttl =
            yamlAsOrDefault<int>(m_logger, CONFIG_REDIS_QOS_CONN_TTL, conn_ttl, DEFAULT_REDIS_QOS_CONN_TTL);
    }

    setMetricsBatchingParams(config);

    const auto& conn_interval = config[CONFIG_REDIS_CHECK_CONN_INTERVAL_SEC];
    if (conn_interval) {
        m_check_conn_interval = std::chrono::seconds(yamlAsOrDefault<int>(
            m_logger, CONFIG_REDIS_CHECK_CONN_INTERVAL_SEC, conn_interval, DEFAULT_CHECK_CONN_INTERVAL_SECS));
    }

    // redis_server (QoS): e.g., 1.2.3.4:6379
    const auto& redis_server = config[CONFIG_REDIS_SERVER];
    if (!redis_server) {
        throw(std::runtime_error("No redis_server configured"));
    }
    const auto redis_server_str = yamlAsOrDefault<std::string>(m_logger, CONFIG_REDIS_SERVER, redis_server, "");

    StringSplit redis_srv_split(redis_server_str, ":");
    const std::string_view redis_host = redis_srv_split.next();
    const std::string_view redis_port_str = redis_srv_split.next();
    int redis_port = 0;
    const auto port_convert_result = std::from_chars(redis_port_str.begin(), redis_port_str.end(), redis_port);
    if (!redis_srv_split.finishedSuccessfully() || (port_convert_result.ec != std::errc{})) {
        throw(std::runtime_error("Can't parse qos redis server address"));
    }
    m_qos_redis_conn = std::make_unique<RedisServerConnection>(std::string(redis_host), redis_port, std::move(net));
}

Processor::~Processor() {
    // Request a stop on the redis-reconnect thread specifically because it's the only one waiting on the condition
    // variable, and we don't want to signal the condition variable, have the other thread wake up and then immediately
    // go back to sleep because a stop hasn't been requested.
    m_redis_reconnect_thread.get_stop_source().request_stop();

    m_redis_cv.notify_one();

    // Member std::jthreads will be auto-joined on destruction anyway, but if we let that happen implicitly
    // then when that happens will depend on the order of member declarations.
    // Since for correctness we need the processing thread to terminate before we destroy any member fields
    // it depends on, doing this implicitly would mean that member variables need to be declared in a
    // particular order to preserve correctness during shutdown. That's fragile and likely to cause surprise,
    // so instead we side-step the issue and explicitly join the processing thread here so that by the time
    // we get to destroying member variables there aren't any internal threads running.
    if (m_msg_consumer_thread.joinable()) {
        m_msg_consumer_thread.join();
    }
    if (m_redis_reconnect_thread.joinable()) {
        m_redis_reconnect_thread.join();
    }
}

void Processor::start() {
    m_msg_consumer_thread = std::jthread([this](std::stop_token stop) { messageConsumerThread(stop); });
    m_redis_reconnect_thread = std::jthread([this](std::stop_token stop) { checkRedisServerConnThread(stop); });
}

/*
 * WARNING: This function must be executed by a single thread only!
 *
 *   (1) FIFOList cannot have multi-threaded consumer or producer. If
 *       `m_haprxy_mesg_q` FIFOList is to be consumed by multiple threads
 *       then we need to use a lock-based queue.
 *   (2) m_qos_redis_conn (which wrap redis contexts) can be
 *       interacted with only one thread. If we want to push data into a redis
 *       server from multiple threads then none of the these threads can share
 *       a redis context; i.e., multiple contexts are needed.
 */
void Processor::messageConsumerThread(std::stop_token stop) {
    constexpr std::chrono::microseconds sleep_time(100);
    auto last_stats_time = m_time.now();

    m_last_redis_connect_time = m_time.now();
    m_qos_redis_conn->connect();

    while (!stop.stop_requested()) {
        std::string buffer;
        if (m_haprxy_mesg_q.wait_dequeue_timed(buffer, sleep_time)) {
            if (buffer.find(RawEvents::reqStart(), 0) == 0) {
                processReq(buffer);
            } else if (buffer.find(RawEvents::dataXfer(), 0) == 0) {
                processDataXfer(buffer);
            } else if (buffer.find(RawEvents::activeReqs(), 0) == 0) {
                processActiveRequests(buffer);
            } else if (buffer.find(RawEvents::reqEnd(), 0) == 0) {
                processReqEnd(buffer);
            } else {
                m_logger->info("Unrecognized message:{}", buffer);
            }
        }
        sendToRedisQos();

        const auto now = m_time.now();
        if (now - last_stats_time > STATS_LOG_INTERVAL) {
            m_logger->info("Msg Consumer Thread - current msg-Q size:{} worker_id:{}", m_haprxy_mesg_q.size_approx(),
                           m_worker_id);
            last_stats_time = now;
        }

        m_qos_redis_conn->drainRedisCmdPipeline();
        m_qos_redis_conn->reconnectIfNeeded();
    }
}

void Processor::checkRedisServerConnThread(std::stop_token stop) {
    while (!stop.stop_requested()) {
        {
            std::unique_lock<std::mutex> lock(m_redis_cv_mutex);
            auto status = m_redis_cv.wait_for(lock, m_check_conn_interval, [&stop] { return stop.stop_requested(); });

            if (status) {
                // exiting
                break;
            }
        }

        m_qos_redis_conn->checkIfNeedsReconnect();
    }
}

void Processor::sendToRedisQos() {
    auto now = m_time.now();
    bool flush_for_time = (now - m_last_redis_flush_time > m_processor_batch_flush_period);
    bool flush_for_msg_count = (m_qos_not_send_count >= m_processor_batch_count);
    if (!flush_for_time && !flush_for_msg_count) {
        return;
    }
    m_last_redis_flush_time = now;
    m_qos_not_send_count = 0;

    if (!m_qos_redis_conn->connected()) {
        const auto mono_now = m_time.now();
        const std::chrono::system_clock::duration since_connect = mono_now - m_last_redis_connect_time;
        if (since_connect > m_check_conn_interval) {
            m_last_redis_connect_time = mono_now;
            m_qos_redis_conn->connect();
        }

        const auto cutoff_timestamp = m_time.now() - std::chrono::seconds(m_redis_qos_ttl);
        const auto is_timestamp_before_cutoff = [cutoff_timestamp](const QosRedisCommandMap::value_type& kvp) {
            return kvp.first.m_timestamp < cutoff_timestamp;
        };
        std::erase_if(m_qos_redis_commands, is_timestamp_before_cutoff);

        m_qos_redis_active_reqs.clear();
        return;
    }

    std::unordered_set<std::string> keys_found;
    for (const auto& [key, val] : m_qos_redis_commands) {
        // Example command:  hincrby
        // verb_1599322430_user_AKIAIOSFODNN7EXAMPLE$dev.dc PUT 1
        //
        // Delimiter between the entity key and the endpoint should be a
        // random charater that is invalid in all ips, access keys and
        // bucket names. We choose "$" here.

        const auto ss_key = fmt::v10::format("verb_{}_{}${}", getEpochSecs(key.m_timestamp), key.m_user, m_endpoint);

        auto ss_cmd = fmt::v10::format("hincrby {} {} {}", ss_key, key.m_cat, val);
        m_qos_redis_conn->addCommand(ss_cmd);

        if (keys_found.insert(ss_key).second) {
            ss_cmd = fmt::v10::format("expire {} {}", ss_key, m_redis_qos_ttl);
            m_qos_redis_conn->addCommand(ss_cmd);
        }
    }
    m_qos_redis_commands.clear();

    for (const auto& [key, active_request_count] : m_qos_redis_active_reqs) {
        // example key: conn_v2_user_up_instance1234_AKIAIOSFODNN7EXAMPLE$dev.dc
        const auto ss_cmd = fmt::v10::format("set {} {} ex {}", key, active_request_count, m_redis_qos_conn_ttl);
        m_qos_redis_conn->addCommand(ss_cmd);
    }
    m_qos_redis_active_reqs.clear();
}

// This function processes the req data from haproxy.
void Processor::processReq(std::string_view raw_input) {
    // req~|~1.2.3.4:58840~|~AKIAIOSFODNN7EXAMPLE~|~PUT~|~up~|~instance1234~|~7~|~LISTBUCKETS
    // Note: the last token (LISTBUCKETS) may be empty
    StringSplit split(raw_input, DELIMITER);
    std::array<std::string_view, static_cast<size_t>(RequestToken::Count)> tokens;
    for (int i = 0; i < static_cast<int>(RequestToken::Count); ++i) {
        tokens[i] = split.next();
    }
    if (!split.finishedSuccessfully()) {
        m_logger->error("Unexpected request format: {}", raw_input);
        return;
    }

    int active_requests;
    const auto areqs_convert_result =
        std::from_chars(tokens[static_cast<size_t>(RequestToken::ActiveRequests)].begin(),
                        tokens[static_cast<size_t>(RequestToken::ActiveRequests)].end(), active_requests);
    if (areqs_convert_result.ec != std::errc{}) {
        m_logger->error("Unexpected active request format: {}", raw_input);
        return;
    }

    const auto user_key = tokens[static_cast<size_t>(RequestToken::UserKey)];
    if (!isPrintableASCII(user_key)) {
        m_logger->error("Invalid access key: {}", user_key);
        return;
    }

    const std::string_view verb = tokens[static_cast<size_t>(RequestToken::Verb)];
    const std::string_view direction = tokens[static_cast<size_t>(RequestToken::Direction)];
    const std::string_view instance_id = tokens[static_cast<size_t>(RequestToken::InstanceId)];
    const std::string_view request_class = tokens[static_cast<size_t>(RequestToken::RequestClass)];

    const auto conn_key = fmt::v10::format("conn_v2_user_{}_{}_{}${}", direction, instance_id, user_key, m_endpoint);
    const auto cmd_key = fmt::v10::format("user_{}", user_key);

    if (request_class.length() > 0) {
        m_qos_redis_commands[RedisCmdKey{cmd_key, m_time.now(), std::string(request_class)}] += 1;
    }
    m_qos_redis_commands[RedisCmdKey{cmd_key, m_time.now(), std::string(verb)}] += 1;
    m_qos_redis_active_reqs[conn_key] = active_requests;

    ++m_qos_not_send_count;
}

void Processor::processDataXfer(std::string_view raw_input) {
    // data_xfer~|~1.2.3.4:55094~|~AKIAIOSFODNN7EXAMPLE~|~dwn~|~4096
    StringSplit split(raw_input, DELIMITER);
    split.next(); // Skip past the 'data_xfer' prefix
    split.next(); // Skip past unused "request key" field
    const std::string_view user = split.next();
    const std::string_view direction = split.next();
    const std::string_view len_str = split.next();
    int len = 0;
    const auto len_convert_result = std::from_chars(len_str.begin(), len_str.end(), len);
    if (!split.finishedSuccessfully() || (len_convert_result.ec != std::errc{})) {
        m_logger->error("Unexpected data_xfer format: {}", raw_input);
        return;
    }
    if (!isPrintableASCII(user)) {
        m_logger->error("Invalid access key: {}", user);
        return;
    }

    if (user.empty()) {
        return;
    }

    const std::string direction_key = fmt::v10::format("bnd_{}", direction);
    std::string cmd_key = fmt::v10::format("user_{}", user);
    m_qos_redis_commands[{std::move(cmd_key), m_time.now(), direction_key}] += len;

    ++m_qos_not_send_count;
}

void Processor::processActiveRequests(std::string_view raw_input) {
    // active_reqs~|~instanceid-1234~|~AKIAIOSFODNN7EXAMPLE~|~up~|~7
    StringSplit split(raw_input, DELIMITER);
    split.next(); // Skip past the 'active_reqs' prefix
    const std::string_view instance_id = split.next();
    const std::string_view user_key = split.next();
    const std::string_view direction = split.next();
    const std::string_view active_reqs_str = split.next();
    int active_requests = 0;
    const auto areqs_convert_result = std::from_chars(active_reqs_str.begin(), active_reqs_str.end(), active_requests);
    if (!split.finishedSuccessfully() || (areqs_convert_result.ec != std::errc{})) {
        m_logger->error("Unexpected active-requests format: {}", raw_input);
        return;
    }

    const auto conn_key = fmt::v10::format("conn_v2_user_{}_{}_{}${}", direction, instance_id, user_key, m_endpoint);
    m_qos_redis_active_reqs[conn_key] = active_requests;

    ++m_qos_not_send_count;
}

void Processor::processReqEnd(std::string_view raw_input) {
    // req_end~|~1.2.3.4:58840~|~AKIAIOSFODNN7EXAMPLE~|~PUT~|~up~|~instance1234~|~7
    StringSplit split(raw_input, DELIMITER);
    split.next(); // Skip past the 'req_end' prefix
    split.next(); // Skip past unused "request key" field
    const std::string_view user_key = split.next();
    split.next(); // Skip past unused "verb" field
    const std::string_view direction = split.next();
    const std::string_view instance_id = split.next();
    const std::string_view active_reqs_str = split.next();
    int active_requests = 0;
    const auto areqs_convert_result = std::from_chars(active_reqs_str.begin(), active_reqs_str.end(), active_requests);
    if (!split.finishedSuccessfully() || (areqs_convert_result.ec != std::errc{})) {
        m_logger->error("Unexpected request-end format: {}", raw_input);
        return;
    }

    const auto conn_key = fmt::v10::format("conn_v2_user_{}_{}_{}${}", direction, instance_id, user_key, m_endpoint);
    m_qos_redis_active_reqs[conn_key] = active_requests;

    ++m_qos_not_send_count;
}

} // namespace syslogsrv
