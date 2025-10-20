// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#include <cstring>
#include <memory>
#include <spdlog/sinks/hourly_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <thread>

#include "common.h"
#include "processor_config.h"
#include "syslog_server.h"

namespace {

constexpr auto NUM_LOG_FILES = 4;

std::shared_ptr<spdlog::logger> createLogger(const YAML::Node& path_node, const YAML::Node& level_node,
                                             const std::string& log_name, const std::string& log_format) {
    std::shared_ptr<spdlog::logger> logger = nullptr;
    if (path_node.IsDefined() && path_node.IsScalar()) {
        const std::string& log_file_str = path_node.Scalar();

        // _mt indicates thread safe
        logger =
            spdlog::hourly_logger_mt(log_name,     // identify logger/log-server
                                     log_file_str, // name of the log file
                                     false,        // do not truncate when opening the file
                                     NUM_LOG_FILES // keep the NUM_LOG_FILES files around each for an hour
                                                   // Since spdlog doesn't have built-in compression support, we are
                                                   // going to use a separate/customized solution to compress old
                                                   // files and eventually delete old zipped files on an hourly basis.
            );
    } else {
        logger = spdlog::stdout_color_mt(log_name);
    }

    logger->set_pattern(log_format, spdlog::pattern_time_type::utc);

    spdlog::level::level_enum loglvl = spdlog::level::info;
    if (level_node.IsDefined() && level_node.IsScalar()) {
        const std::string& loglvl_str = level_node.Scalar();
        loglvl = spdlog::level::from_str(loglvl_str);
        if (loglvl == spdlog::level::off) {
            logger->error("Unrecognised log level config value '{}', defaulting to info", loglvl_str);
            loglvl = spdlog::level::info;
        }
    }
    logger->set_level(loglvl);
    logger->flush_on(loglvl);

    return logger;
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "ERROR: Expected a config file path as the only parameter. Terminating...\n");
        exit(-ENOENT);
    }

    YAML::Node config;
    try {
        config = YAML::LoadFile(argv[1]);
    } catch (const YAML::BadFile& badfile) {
        fprintf(stderr, "ERROR: Failed to open config file '%s': %s.\nTerminating...\n", argv[1], badfile.what());
        exit(-ENOENT);
    } catch (const YAML::ParserException& exc) {
        fprintf(stderr, "ERROR: Failed to parse config file '%s': %s.\nTerminating...\n", argv[1], exc.what());
        exit(-EINVAL);
    }

    std::shared_ptr<spdlog::logger> logger = createLogger(config[syslogsrv::CONFIG_LOG_FILE_NAME],
                                                          config[syslogsrv::CONFIG_LOG_LEVEL], syslogsrv::SERVER_NAME,
                                                          // 2020-05-04 23:35:49,156 - INFO - <msg>
                                                          "%Y-%m-%d %H:%M:%S,%e - %l - %v");
    createLogger(config[syslogsrv::CONFIG_ACCESS_LOG_FILE_NAME], YAML::Node{}, syslogsrv::ACCESS_LOG, "%v");

    logger->info("started the main sysloger server using the cfg file {}", argv[1]);

    // leave one syslog server in the main thread for the convenience of debugging
    int num_of_syslog_servers = 1;
    if (const auto& node = config[syslogsrv::CONFIG_NUM_OF_SYSLOG_SERVERS]) {
        num_of_syslog_servers =
            syslogsrv::yamlAsOrDefault<int>(logger, syslogsrv::CONFIG_NUM_OF_SYSLOG_SERVERS, node, 1);
    }

    std::vector<std::jthread> servers;
    for (auto i = 1; i < num_of_syslog_servers; ++i) {
        servers.push_back(std::jthread(syslogsrv::startSyslogServer, config, i));
    }

    syslogsrv::startSyslogServer(config, 0);
    return 0;
}
