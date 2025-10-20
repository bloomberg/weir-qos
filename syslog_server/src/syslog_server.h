// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#ifndef INCLUDED_SYSLOG_SERVER
#define INCLUDED_SYSLOG_SERVER

#include <string>
#include <yaml-cpp/yaml.h>

#include "syscall_wrapper.h"

namespace syslogsrv {

constexpr inline size_t MAX_UDP_RECV_BUFFER_SIZE = 64 * 1024 * 1024;

// utility functions
size_t getRmemMax(const std::string& rmemm_path);
size_t getDesiredUdpRecvBufSize(size_t rmem_max);
size_t getUdpRecvBufSize(int s, SystemInterface& sys_call);
void setUdpRecvBufSize(int s, size_t size, SystemInterface& sys_call);
void setUdpPortReuseOption(const int s, SystemInterface& sys_call);
int createSocket(const YAML::Node& config, SystemInterface& sys_call);

// The main entry point for each syslog server thread.
// Handles receipt of messages from HAProxy via socket and either writes
// them to the log if it's a log message or queues it for processing otherwise.
void startSyslogServer(YAML::Node config, int worker_id);

} // namespace syslogsrv

#endif
