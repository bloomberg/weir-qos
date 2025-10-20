/*
 * Weir distributed rate-limiting utility functions
 *
 * Copyright 2024 Bloomberg Finance L.P.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <arpa/inet.h>
#include <haproxy/khash.h>
#include <haproxy/log.h>
#include <haproxy/rate_limit.h>
#include <haproxy/tools.h>
#include <math.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static uint64_t ip_port_from_sockaddr(struct sockaddr_in* addr_in) {
    uint16_t port = 0;
    uint32_t ip = 0;

    if (addr_in == NULL)
        return 0;
    port = ntohs(addr_in->sin_port);
    ip = ntohl(addr_in->sin_addr.s_addr);
    return (uint64_t)ip << 32 | port;
}

typedef enum units { UNIT_MB = 1024 * 1024, UNIT_USECS_IN_SEC = 1000000, UINT_USECS_IN_MILLISEC = 1000 } Units;

typedef struct {
    uint32_t in_seconds;
    uint64_t elapsed_usec_in_the_epoch;
} epoch_t;

static inline epoch_t get_current_epoch() {
    epoch_t current_epoch;
    struct timeval current_time;

    gettimeofday(&current_time, NULL);

    current_epoch.in_seconds = current_time.tv_sec;
    current_epoch.elapsed_usec_in_the_epoch = current_time.tv_usec;

    return current_epoch;
}

static const char* LOG_DELIMITER = "~|~";
static volatile int BASE_JITTER_RANGE_MS = 2;
static const int SPEED_TABLE_CLEANUP_PERIOD_USEC = 60 * UNIT_USECS_IN_SEC;
static const int SPEED_TABLE_STALE_POLICY_AGE_SEC = 120;
// Throttling backoff settings:
static const int BACKOFF_WINDOW_EPOCHS = 6;
static const int MIN_RUN_TIME_USEC = 50 * UINT_USECS_IN_MILLISEC;
static const float DIFF_RATIO_LOW_MARK_TO_JITTER = 1.5;

// equal to c++: using  ip_port_key_hash = unordered_map<uint64_t, kh_cstr_t>
KHASH_MAP_INIT_INT64(ip_port_key_hash, kh_cstr_t)
// equal to c++: using  key_ip_port_count_hash = unordered_map<kh_cstr_t, khint32_t>
KHASH_MAP_INIT_STR(key_ip_port_count_hash, khint32_t)
// equal to  c++: using speed_hash = unordered_map<kh_string_t, unit32_t>
typedef struct {
    uint32_t throttle;                /* 0: no, >0: yes */
    khint32_t num_active_connections; /* user's active conns when the this violation record found */
    uint32_t received_epoch_sec;
    float diff_ratio;
    uint64_t elapsed_usec_in_the_epoch;
    uint64_t allowed_run_time_usec;
    float previous_diff_ratio;
} speed_hash_value_t;
KHASH_MAP_INIT_STR(speed_hash, speed_hash_value_t)

static khash_t(ip_port_key_hash) * ip_port_key_hashmap;
static khash_t(key_ip_port_count_hash) * key_ip_port_count_hashmap;
static pthread_rwlock_t ip_port_key_hashmap_lck;
static khash_t(speed_hash) * key_upload_speed_epoch_hashmap;
static pthread_rwlock_t key_upload_speed_epoch_hashmap_lck;
static khash_t(speed_hash) * key_download_speed_epoch_hashmap;
static pthread_rwlock_t key_download_speed_epoch_hashmap_lck;
static void* remove_old_epochs(void* unused);

static inline int is_valid_violation_policy(speed_hash_value_t* policy, uint32_t curr_sec) {
    uint32_t policy_age_epochs = curr_sec - policy->received_epoch_sec;
    return policy_age_epochs <= BACKOFF_WINDOW_EPOCHS;
}

// init static variables defined above, this function should be called from main()
void init_speed_epoch_hashmaps() {
    pthread_t tid;

    // Only initialise globals once
    if (ip_port_key_hashmap != NULL) {
        return;
    }

    ip_port_key_hashmap = kh_init(ip_port_key_hash);
    key_ip_port_count_hashmap = kh_init(key_ip_port_count_hash);
    key_upload_speed_epoch_hashmap = kh_init(speed_hash);
    key_download_speed_epoch_hashmap = kh_init(speed_hash);
    if (pthread_rwlock_init(&key_upload_speed_epoch_hashmap_lck, NULL) != 0 ||
        pthread_rwlock_init(&key_download_speed_epoch_hashmap_lck, NULL) != 0 ||
        pthread_rwlock_init(&ip_port_key_hashmap_lck, NULL) != 0) {
        send_log(NULL, LOG_ERR, "failed to init ip_port/speed hashmap locks");
        exit(1);
    }
    if (pthread_create(&tid, NULL, remove_old_epochs, NULL) != 0) {
        send_log(NULL, LOG_ERR, "failed to create clean-up thread");
        exit(1);
    }
}

static void debug_print_key_speed_table(khash_t(speed_hash) * hash, pthread_rwlock_t* lck) {
    khint_t k;

    if (hash == NULL || lck == NULL)
        return;

    send_log(NULL, LOG_DEBUG, "start of dumping speed table\n");
    pthread_rwlock_rdlock(lck);
    for (k = kh_begin(hash); k != kh_end(hash); ++k) {
        if (kh_exist(hash, k)) {
            speed_hash_value_t found = kh_value(hash, k);
            send_log(NULL, LOG_DEBUG, "key: %s, epoch: %u, diff_ratio: %f\n", kh_key(hash, k), found.received_epoch_sec,
                     found.diff_ratio);
        }
    }
    pthread_rwlock_unlock(lck);
    send_log(NULL, LOG_DEBUG, "end of dumping speed table\n");
}

static inline khash_t(speed_hash) * get_key_speed_hash(DataDirection data_direction) {
    switch (data_direction) {
    case RL_DOWNLOAD:
        return key_download_speed_epoch_hashmap;
    case RL_UPLOAD:
        return key_upload_speed_epoch_hashmap;
    }
    return NULL;
}

static inline pthread_rwlock_t* get_key_speed_hash_lock(DataDirection data_direction) {
    switch (data_direction) {
    case RL_DOWNLOAD:
        return &key_download_speed_epoch_hashmap_lck;
    case RL_UPLOAD:
        return &key_upload_speed_epoch_hashmap_lck;
    }
    return NULL;
}

void print_out_key_speed_table(DataDirection data_direction) {
    khash_t(speed_hash) * hash;
    pthread_rwlock_t* lck;
    hash = get_key_speed_hash(data_direction);
    lck = get_key_speed_hash_lock(data_direction);
    debug_print_key_speed_table(hash, lck);
}

static inline void incr_num_connections_of_a_key(const char* key) {
    khint_t k = kh_get(key_ip_port_count_hash, key_ip_port_count_hashmap, key);

    if (k == kh_end(key_ip_port_count_hashmap)) {
        const char* key_copy = strdup(key);

        if (key_copy != NULL) {
            int absent;

            k = kh_put(key_ip_port_count_hash, key_ip_port_count_hashmap, key_copy, &absent);
            kh_value(key_ip_port_count_hashmap, k) = 1;
        }
    } else {
        khint32_t v = kh_value(key_ip_port_count_hashmap, k);

        kh_value(key_ip_port_count_hashmap, k) = v + 1;
    }
}

static inline void decr_num_connections_of_a_key(const char* key) {
    khint_t k = kh_get(key_ip_port_count_hash, key_ip_port_count_hashmap, key);

    if (k != kh_end(key_ip_port_count_hashmap)) {
        khint32_t v = kh_value(key_ip_port_count_hashmap, k);

        if (v == 0) {
            send_log(NULL, LOG_ERR, "for %s there seems to be no pending conn", key);
            return;
        }
        if (v == 1) {
            void* acckey = (void*)kh_key(key_ip_port_count_hashmap, k);

            kh_del(key_ip_port_count_hash, key_ip_port_count_hashmap, k);
            free(acckey);
        } else {
            kh_value(key_ip_port_count_hashmap, k) = v - 1;
        }
    }
}

static inline khint32_t get_ip_port_count_from_key(kh_cstr_t access_key) {
    khint_t k;
    khint32_t count = 0;

    pthread_rwlock_rdlock(&ip_port_key_hashmap_lck);
    k = kh_get(key_ip_port_count_hash, key_ip_port_count_hashmap, access_key);
    if (k != kh_end(key_ip_port_count_hashmap)) {
        count = kh_value(key_ip_port_count_hashmap, k);
    }
    pthread_rwlock_unlock(&ip_port_key_hashmap_lck);

    return count;
}

static inline kh_cstr_t get_key_from_ip_port(uint64_t ip_port) {
    khint_t k;
    kh_cstr_t access_key = NULL;

    pthread_rwlock_rdlock(&ip_port_key_hashmap_lck);
    k = kh_get(ip_port_key_hash, ip_port_key_hashmap, ip_port);
    if (k != kh_end(ip_port_key_hashmap)) {
        access_key = kh_value(ip_port_key_hashmap, k);
    }
    pthread_rwlock_unlock(&ip_port_key_hashmap_lck);
    return access_key;
}

static void compute_allowed_run_time(speed_hash_value_t* policy, uint32_t curr_sec) {
    const uint32_t policy_age = curr_sec - policy->received_epoch_sec;
    uint64_t allowed = (uint64_t)((double)policy->elapsed_usec_in_the_epoch / policy->diff_ratio);

    allowed = MAX(MIN_RUN_TIME_USEC, allowed);

    if (policy_age == 0) {
        policy->allowed_run_time_usec = 0;
    } else if (policy_age <= BACKOFF_WINDOW_EPOCHS) {
        allowed = allowed * pow(2, policy_age - 1);
        policy->allowed_run_time_usec = MIN(allowed, UNIT_USECS_IN_SEC);
    } else {
        policy->allowed_run_time_usec = UNIT_USECS_IN_SEC;
    }
}

static speed_hash_value_t get_epoch_sec(uint64_t ip_port, DataDirection data_direction, uint32_t curr_sec,
                                        kh_cstr_t* access_key) {
    khint_t k;
    pthread_rwlock_t* lck;
    khash_t(speed_hash) * hash;
    speed_hash_value_t found = {.throttle = 0};

    if (access_key == NULL) {
        send_log(NULL, LOG_DEBUG, "Access key is null");
        return found;
    }

    *access_key = get_key_from_ip_port(ip_port);
    if (*access_key == NULL || strlen(*access_key) == 0) {
        send_log(NULL, LOG_DEBUG, "Can not get access key from ip_port_key_hashmap");
        return found;
    }

    hash = get_key_speed_hash(data_direction);
    lck = get_key_speed_hash_lock(data_direction);
    if (hash == NULL || lck == NULL) {
        send_log(NULL, LOG_ERR, "Invalid speed hash map or lock.");
        return found;
    }

    pthread_rwlock_rdlock(lck);
    k = kh_get(speed_hash, hash, *access_key);
    if (k != kh_end(hash)) {
        found = kh_value(hash, k);
        found.throttle = 0;
        if (is_valid_violation_policy(&found, curr_sec)) {
            found.throttle = 1;
            found.num_active_connections = get_ip_port_count_from_key(*access_key);
            compute_allowed_run_time(&found, curr_sec);
        }
    }
    pthread_rwlock_unlock(lck);
    return found;
}

static void* remove_old_epochs(void* unused) {
    time_t current_epoch;
    khint_t k, k_next;
    DataDirection dir = RL_DOWNLOAD;
    pthread_rwlock_t* lck = NULL;
    khash_t(speed_hash)* hash = NULL;

    while (1) {
        lck = get_key_speed_hash_lock(dir);
        hash = get_key_speed_hash(dir);
        current_epoch = time(NULL);

        pthread_rwlock_wrlock(lck);
        k = kh_begin(hash);
        while (k != kh_end(hash)) {
            k_next = k + 1;
            if (kh_exist(hash, k)) {
                speed_hash_value_t v = kh_value(hash, k);
                if (current_epoch > v.received_epoch_sec &&
                    current_epoch - v.received_epoch_sec > SPEED_TABLE_STALE_POLICY_AGE_SEC) {
                    kh_del(speed_hash, hash, k);
                }
            }
            k = k_next;
        }
        pthread_rwlock_unlock(lck);

        dir = dir == RL_DOWNLOAD ? RL_UPLOAD : RL_DOWNLOAD;
        usleep(SPEED_TABLE_CLEANUP_PERIOD_USEC);
    }

    return NULL;
}

static inline uint32_t get_jitter_usec(speed_hash_value_t* policy) {
    int jitter = MAX(policy->previous_diff_ratio, policy->diff_ratio) >= DIFF_RATIO_LOW_MARK_TO_JITTER ||
                 policy->diff_ratio - policy->previous_diff_ratio > 0;

    return jitter ? (ha_random32() % BASE_JITTER_RANGE_MS) * UINT_USECS_IN_MILLISEC : 0;
}

int rl_speed_throttle(struct sockaddr_in* addr_in, DataDirection data_direction) {
    uint64_t ip_port;
    kh_cstr_t access_key;
    speed_hash_value_t found;
    epoch_t current_epoch = get_current_epoch();

    if (addr_in == NULL)
        return RL_NO_THROTTLE;

    ip_port = ip_port_from_sockaddr(addr_in);

    found = get_epoch_sec(ip_port, data_direction, current_epoch.in_seconds, &access_key);
    send_log(NULL, LOG_DEBUG,
             "in speed_throttle: throttle=%u key=%s curr_epoch=%d ip=%s port=%d "
             "direction=%s violation_recv_sec=%d elapsed_in_epoch=%lu diff_ratio=%f allowed=%lu\n",
             found.throttle, access_key, current_epoch.in_seconds, inet_ntoa(addr_in->sin_addr),
             ntohs(addr_in->sin_port), (data_direction == RL_DOWNLOAD ? "download" : "upload"),
             found.received_epoch_sec, found.elapsed_usec_in_the_epoch, found.diff_ratio, found.allowed_run_time_usec);

    if (0 == found.throttle)
        return RL_NO_THROTTLE;

    // NOTE: extended sleeping in this thread would affect other connections as well.
    if (current_epoch.elapsed_usec_in_the_epoch < found.allowed_run_time_usec) {
        uint32_t jitter_us = get_jitter_usec(&found);

        if (jitter_us > 0) {
            send_log(NULL, LOG_DEBUG, "Sleeping: jitter=%u\n", jitter_us);
            usleep(jitter_us);
        }
        return RL_NO_THROTTLE;
    }

    send_log(NULL, LOG_DEBUG,
             "Slowing down: key=%s curr_epoch=%d ip=%s port=%d direction=%s "
             "policy_epoch=%u elapsed_in_epoch_us=%lu allowed_run_time_us=%lu diff_ratio=%f "
             "num_conns=%d\n",
             access_key, current_epoch.in_seconds, inet_ntoa(addr_in->sin_addr), ntohs(addr_in->sin_port),
             (data_direction == RL_DOWNLOAD ? "download" : "upload"), found.received_epoch_sec,
             found.elapsed_usec_in_the_epoch, found.allowed_run_time_usec, found.diff_ratio,
             found.num_active_connections);

    // Don't sleep here, as that will be handled by the calling code in the weir filter
    return RL_THROTTLE;
}

void rl_data_transferred(struct sockaddr_in* addr_in, DataDirection data_direction, unsigned int done) {
    uint64_t ip_port;
    kh_cstr_t access_key;

    if (addr_in == NULL)
        return;

    ip_port = ip_port_from_sockaddr(addr_in);

    access_key = get_key_from_ip_port(ip_port);
    if (access_key == NULL || strlen(access_key) == 0) {
        send_log(NULL, LOG_DEBUG, "Can not get access key from ip_port_key_hashmap: conn=%s:%d direction=%s done=%u",
                 inet_ntoa(addr_in->sin_addr), ntohs(addr_in->sin_port),
                 (data_direction == RL_DOWNLOAD ? "download" : "upload"), done);
        return;
    }

    send_log(NULL, LOG_INFO, "data_xfer%s%s:%d%s%s%s%s%s%u", LOG_DELIMITER, inet_ntoa(addr_in->sin_addr),
             ntohs(addr_in->sin_port), LOG_DELIMITER, access_key, LOG_DELIMITER,
             (data_direction == RL_DOWNLOAD ? "dwn" : "up"), LOG_DELIMITER, done);
}

void set_throttle_epoch_us(const char* key, uint64_t epoch_us, DataDirection data_direction, float diff_ratio) {
    khint_t k;
    int absent;
    khash_t(speed_hash) * hash;
    pthread_rwlock_t* lck;
    epoch_t current_epoch = get_current_epoch();
    speed_hash_value_t value = {
        .throttle = current_epoch.in_seconds,
        .received_epoch_sec = current_epoch.in_seconds,
        .diff_ratio = diff_ratio,
        .elapsed_usec_in_the_epoch = epoch_us % UNIT_USECS_IN_SEC,
        .allowed_run_time_usec = 0,
        .previous_diff_ratio = 0,
    };

    if (key == NULL || strlen(key) == 0) {
        send_log(NULL, LOG_WARNING, "Empty key is used to set epoch_sec for speed throttling.");
        return;
    }
    send_log(NULL, LOG_DEBUG,
             "Set throttle epoch: key=%s recv_epoch_us=%lu curr_epoch=%u "
             "elapsed_usec_in_the_epoch=%lu diff_ratio=%f\n",
             key, epoch_us, value.received_epoch_sec, value.elapsed_usec_in_the_epoch, value.diff_ratio);

    hash = get_key_speed_hash(data_direction);
    lck = get_key_speed_hash_lock(data_direction);
    if (hash == NULL || lck == NULL) {
        send_log(NULL, LOG_ERR, "Invalid speed hash map or lock.");
        return;
    }

    pthread_rwlock_wrlock(lck);
    k = kh_get(speed_hash, hash, key);
    if (k != kh_end(hash)) {
        speed_hash_value_t found = kh_value(hash, k);

        value.previous_diff_ratio = found.diff_ratio;
        kh_value(hash, k) = value;
    } else {
        char* t = strdup(key); // we need to own the key in our hash maps.
        if (t != NULL) {
            k = kh_put(speed_hash, hash, t, &absent);
            kh_value(hash, k) = value;
        } else {
            send_log(NULL, LOG_ERR, "Running out of memory");
        }
    }
    pthread_rwlock_unlock(lck);
}

static inline uint64_t get_ip_port(const char* ip_str, const char* port_str) {
    struct in_addr addr;
    uint16_t port;
    uint32_t ip;

    if (inet_pton(AF_INET, ip_str, &addr)) {
        port = (uint16_t)atoi(port_str);
        ip = ntohl(addr.s_addr);
        return (uint64_t)ip << 32 | port;
    } else {
        return 0;
    }
}

void set_jitter_range(uint32_t range) {
    BASE_JITTER_RANGE_MS = range;
    send_log(NULL, LOG_INFO, "Jitter range has been set to %d", BASE_JITTER_RANGE_MS);
}

void set_ip_port_key(const char* ip, const char* port, const char* key) {
    uint64_t ip_port;
    int absent;
    char* key_copy;
    khint_t k;

    if (key == NULL || strlen(key) == 0) {
        send_log(NULL, LOG_WARNING, "Empty access key is used to set speed.");
        return;
    }
    send_log(NULL, LOG_DEBUG, "set_ip_port_key: ip=%s port=%s key=%s\n", ip, port, key);

    // In case of ip_port is reused (http-keep-alive) across multiple keys,
    // simply updating the ip_port -> key map should be enough.
    ip_port = get_ip_port(ip, port);
    if (ip_port != 0) {
        key_copy = strdup(key);
        if (key_copy != NULL) {
            void* p = NULL;
            pthread_rwlock_wrlock(&ip_port_key_hashmap_lck);
            k = kh_get(ip_port_key_hash, ip_port_key_hashmap, ip_port);
            if (k != kh_end(ip_port_key_hashmap)) {
                // not reusing the key buffer is because the keys might have different lengths
                p = (void*)kh_value(ip_port_key_hashmap, k);
            } else {
                k = kh_put(ip_port_key_hash, ip_port_key_hashmap, ip_port, &absent);
            }
            kh_value(ip_port_key_hashmap, k) = key_copy;
            incr_num_connections_of_a_key(key);
            send_log(NULL, LOG_DEBUG, "set_ip_port_key set: ip=%s port=%s key=%s\n", ip, port, key);
            pthread_rwlock_unlock(&ip_port_key_hashmap_lck);
            free(p);
        } else {
            send_log(NULL, LOG_ERR, "Running out of memory");
        }
    } else {
        send_log(NULL, LOG_INFO, "bad ip address %s\n", ip);
    }
}

static void remove_from_ip_port_key_hash(uint64_t ip_port) {
    khint_t k;
    kh_cstr_t access_key = NULL;

    pthread_rwlock_wrlock(&ip_port_key_hashmap_lck);
    k = kh_get(ip_port_key_hash, ip_port_key_hashmap, ip_port);
    if (k != kh_end(ip_port_key_hashmap)) {
        access_key = kh_value(ip_port_key_hashmap, k);
        kh_del(ip_port_key_hash, ip_port_key_hashmap, k);
        decr_num_connections_of_a_key(access_key);
    }
    pthread_rwlock_unlock(&ip_port_key_hashmap_lck);

    free((void*)access_key);
}

static void remove_from_rate_limit_map(struct sockaddr_in* addr_in) {
    uint64_t ip_port = ip_port_from_sockaddr(addr_in);
    remove_from_ip_port_key_hash(ip_port);
}

void rl_request_end(struct sockaddr_in* addr_in) {
    if (addr_in == NULL)
        return;
    remove_from_rate_limit_map(addr_in);
}
