#include <assert.h>
#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#include <sys/types.h>

using std::chrono::duration_cast;
using std::chrono::microseconds;

size_t get_udp_error_count(bool print_error_counts) {
    // We read all the stats from /proc/net/snmp and add together a few of the relevant values.
    FILE* f = fopen("/proc/net/snmp", "r");
    if (f == nullptr) {
        return 0;
    }
    char buffer[4096] = {};
    fread(buffer, sizeof(buffer), 1, f);
    fclose(f);

    const char* udp_header_line = strstr(buffer, "Udp: ");
    assert(udp_header_line != nullptr);
    const char* udp_stat_line = strstr(udp_header_line + 1, "Udp: ");
    assert(udp_stat_line != nullptr);

    size_t in_datagrams;
    size_t no_ports;
    size_t in_errors;
    size_t out_datagrams;
    size_t recvbuf_errors;
    size_t sndbuf_errors;
    size_t in_csum_errors;
    size_t ignored_multi;
    size_t mem_errors;
    const int match_count =
        sscanf(udp_stat_line, "Udp: %zu %zu %zu %zu %zu %zu %zu %zu %zu\n", &in_datagrams, &no_ports, &in_errors,
               &out_datagrams, &recvbuf_errors, &sndbuf_errors, &in_csum_errors, &ignored_multi, &mem_errors);
    if (match_count <= 0) {
        printf("Failed to find recvbuf error count\n");
        return 0;
    }

    if (print_error_counts) {
        printf("UDP errors: %zu %zu %zu %zu %zu %zu %zu\n", no_ports, in_errors, recvbuf_errors, sndbuf_errors,
               in_csum_errors, ignored_multi, mem_errors);
    }
    return no_ports + in_errors + recvbuf_errors + sndbuf_errors;
}

int main(int argc, const char** argv) {
    int msgs_per_second = 100'000;
    int destination_port = 9003;
    bool verbose = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            printf("syslog-bench: A very simple workload generator for the Weir syslog server\n");
            printf("Usage: syslog-bench [--msgs <N>] [--port <N>]\n");
            printf("\n");
            printf("--msgs <N>: The number of messages to send per second, defaults to 100,000.\n");
            printf("--port <N>: The port to which the UDP messages should be sent, defaults to 9003.\n");
            printf("--verbose: Enable debugging output\n");
            printf("\n");
            printf("This tool will send control messages to the Weir syslog-server at a defined rate\n");
            printf("and periodically report the number of UDP errors reported by kernel.\n");
            printf("If the reported error count is not zero when sending many messages, it suggests that\n");
            printf("the syslog server is unable to keep up with that workload on the current hardware, and\n");
            printf("would need to either be reconfigured or optimised to be faster.\n");
            printf("\n");
            printf("In conjunction with the output of this benchmark, one should check the output of the\n");
            printf("syslog server itself while running the test, because in addition to dropping packets\n");
            printf("in the kernel, there is an internal fixed-size queue, which could fill up in extreme\n");
            printf("circumstances, causing it to also drop messages.\n");
            return 0;

        } else if (strcmp(argv[i], "--msgs") == 0) {
            if (i + 1 < argc) {
                int new_count = atoi(argv[i + 1]);
                if (new_count > 0) {
                    msgs_per_second = new_count;
                } else {
                    printf("Invalid value given for %s\n", argv[i]);
                    return 1;
                }
                i++;
            } else {
                printf("No value given for %s\n", argv[i]);
                return 1;
            }

        } else if (strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                int new_port = atoi(argv[i + 1]);
                if (new_port > 0) {
                    destination_port = new_port;
                } else {
                    printf("Invalid value given for %s\n", argv[i]);
                    return 1;
                }
                i++;
            } else {
                printf("No value given for %s\n", argv[i]);
                return 1;
            }

        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        }
    }

    const char* test_msgs[] = {
        "req~|~127.0.0.1:8080~|~AKIAIOSFODNN7EXAMPLE~|~PUT~|~up~|~instance1234~|~7\r\n",
        "data_xfer~|~127.0.0.1:8080~|~AKIAIOSFODNN7EXAMPLE~|~dwn~|~4096\r\n",
    };
    const size_t test_msg_count = sizeof(test_msgs) / sizeof(test_msgs[0]);
    size_t test_msg_index = 0;

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const int send_flags = 0;
    sockaddr_in dest_addr = {};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(destination_port);
    dest_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const int batch_size = (msgs_per_second > 1000) ? 10 : 1;
    const int batches_per_second = msgs_per_second / batch_size;
    const microseconds batch_interval = duration_cast<microseconds>(std::chrono::seconds(1)) / batches_per_second;
    auto next_msg_time = std::chrono::high_resolution_clock::now();
    auto last_packet_drop_log = std::chrono::high_resolution_clock::now();

    printf("Sending commands to port %d at a rate of %d/s...\n", destination_port, msgs_per_second);
    const bool running = true;
    size_t previous_udp_errors = get_udp_error_count(verbose);
    while (running) {
        const auto now = std::chrono::high_resolution_clock::now();
        if (now - last_packet_drop_log >= std::chrono::seconds(10)) {
            const size_t new_udp_errors = get_udp_error_count(verbose);
            const size_t delta_udp_errors = new_udp_errors - previous_udp_errors;
            previous_udp_errors = new_udp_errors;
            printf("OS reports %zu new UDP errors\n", delta_udp_errors);
            last_packet_drop_log = now;
        }

        const char* test_msg = test_msgs[test_msg_index];
        test_msg_index = (test_msg_index + 1) % test_msg_count;

        // Send messages in small batches to avoid running into timing issues when trying to divide
        // a second into too many single-message timeslices.
        for (int i = 0; i < batch_size; i++) {
            int send_result =
                sendto(sock, test_msg, strlen(test_msg), send_flags, (sockaddr*)&dest_addr, sizeof(dest_addr));
            assert(send_result > 0);
        }

        next_msg_time += batch_interval;
        const int64_t us_till_next_msg = duration_cast<microseconds>(next_msg_time - now).count();
        if (us_till_next_msg >= 100) {
            usleep(us_till_next_msg);
        }
    }

    return 0;
}
