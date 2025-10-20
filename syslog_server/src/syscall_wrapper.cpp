// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#include "syscall_wrapper.h"

namespace syslogsrv {

int SysCallClass::socket(int domain, int type, int protocol) { return ::socket(domain, type, protocol); }

int SysCallClass::getsockopt(int sockfd, int level, int optname, void* __restrict optval,
                             socklen_t* __restrict optlen) {
    return ::getsockopt(sockfd, level, optname, optval, optlen);
}

int SysCallClass::setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
    return ::setsockopt(sockfd, level, optname, optval, optlen);
}

int SysCallClass::bind(int sockfd, const sockaddr* addr, socklen_t addrlen) { return ::bind(sockfd, addr, addrlen); }

ssize_t SysCallClass::recvfrom(int sockfd, void* buf, size_t len, int flags, sockaddr* src_addr, socklen_t* addrlen) {
    return ::recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
}

std::string SysCallClass::getRmemMaxPath() { return "/proc/sys/net/core/rmem_max"; }

} // namespace syslogsrv
