/*
 * include/haproxy/rate_limit.h
 * Weir distributed rate-limiting utility function prototypes
 *
 * Copyright 2024 Bloomberg Finance L.P.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _HAPROXY_RATE_LIMIT_H
#define _HAPROXY_RATE_LIMIT_H
#include <stdlib.h>
typedef enum { RL_UPLOAD, RL_DOWNLOAD } DataDirection;
typedef enum { RL_THROTTLE, RL_NO_THROTTLE } ThrottleFlag;
void rl_request_end(struct sockaddr_in* addr_in);
int rl_speed_throttle(struct sockaddr_in* addr_in, DataDirection data_direction);
void rl_data_transferred(struct sockaddr_in* addr_in, DataDirection data_direction, unsigned int done);
void set_jitter_range(uint32_t range);
void set_throttle_epoch_us(const char* key, uint64_t epoch_us, DataDirection data_direction, float diff_ratio);
void set_ip_port_key(const char* ip, const char* port, const char* key);
void init_speed_epoch_hashmaps();
void print_out_key_speed_table(DataDirection data_direction);
#endif
