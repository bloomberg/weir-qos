// Copyright 2024 Bloomberg Finance L.P.
// Distributed under the terms of the Apache 2.0 license.

#ifndef INCLUDED_SYSCALL_WRAPPER
#define INCLUDED_SYSCALL_WRAPPER

#include <string>
#include <sys/socket.h>

namespace syslogsrv {

// Wrapper interface for relevant OS socket functions. Allows us to mock these for testing.
class SystemInterface {
  public:
    virtual ~SystemInterface() = default;

    // name the functions after the corresponding functions in <sys/socket.h> in POSIX standard library
    virtual int socket(int domain, int type, int protocol) = 0;
    virtual int getsockopt(int sockfd, int level, int optname, void* __restrict optval,
                           socklen_t* __restrict optlen) = 0;
    virtual int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) = 0;
    virtual int bind(int sockfd, const sockaddr* addr, socklen_t addrlen) = 0;
    virtual ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, sockaddr* src_addr, socklen_t* addrlen) = 0;
    virtual std::string getRmemMaxPath() = 0;
};

// SystemInterface implementation using the corresponding functions in <sys/socket.h> in POSIX standard library
class SysCallClass : public SystemInterface {
  public:
    int socket(int domain, int type, int protocol) override;
    int getsockopt(int sockfd, int level, int optname, void* __restrict optval, socklen_t* __restrict optlen) override;
    int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) override;
    int bind(int sockfd, const sockaddr* addr, socklen_t addrlen) override;
    ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, sockaddr* src_addr, socklen_t* addrlen) override;
    std::string getRmemMaxPath() override;
};

} // namespace syslogsrv

#endif
