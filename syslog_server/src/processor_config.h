// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#ifndef INCLUDED_PROCESSOR_CONFIG
#define INCLUDED_PROCESSOR_CONFIG

namespace syslogsrv {

// default values
constexpr inline int DEFAULT_METRICS_BATCHING_COUNT = 250000;
constexpr inline int DEFAULT_METRICS_BATCHING_MSEC_PERIOD = 31;
constexpr inline int DEFAULT_REDIS_QOS_TTL = 2;
constexpr inline int DEFAULT_REDIS_QOS_CONN_TTL = 60;
constexpr inline int DEFAULT_CHECK_CONN_INTERVAL_SECS = 5;
constexpr inline int DEFAULT_MSG_QUEUE_SIZE = 1024;

// message processor configuration options
constexpr inline char CONFIG_ACCESS_LOG_FILE_NAME[] = "access_log_file_name";
constexpr inline char CONFIG_ENDPOINT[] = "endpoint";
constexpr inline char CONFIG_LOG_FILE_NAME[] = "log_file_name";
constexpr inline char CONFIG_LOG_LEVEL[] = "log_level";
constexpr inline char CONFIG_MSG_QUEUE_SIZE[] = "msg_queue_size";
constexpr inline char CONFIG_METRICS_BATCH_COUNT[] = "metrics_batch_count";
constexpr inline char CONFIG_METRICS_BATCH_PERIOD_MSEC[] = "metrics_batch_period_msec";
constexpr inline char CONFIG_NUM_OF_SYSLOG_SERVERS[] = "num_of_syslog_servers";
constexpr inline char CONFIG_PORT[] = "port";
constexpr inline char CONFIG_REDIS_QOS_TTL[] = "redis_qos_ttl";
constexpr inline char CONFIG_REDIS_QOS_CONN_TTL[] = "redis_qos_conn_ttl";
constexpr inline char CONFIG_REDIS_CHECK_CONN_INTERVAL_SEC[] = "redis_check_conn_interval_sec";
constexpr inline char CONFIG_REDIS_SERVER[] = "redis_server";

} // namespace syslogsrv

#endif
