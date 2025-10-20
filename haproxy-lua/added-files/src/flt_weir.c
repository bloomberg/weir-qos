/*
 * Weir distributed rate-limiting filter
 *
 * Copyright 2024 Bloomberg Finance L.P.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */
#include <ctype.h>
#include <stdbool.h>

#include <haproxy/api.h>
#include <haproxy/bug.h>
#include <haproxy/channel-t.h>
#include <haproxy/cli.h>
#include <haproxy/filters.h>
#include <haproxy/global.h>
#include <haproxy/http-t.h>
#include <haproxy/http_ana-t.h>
#include <haproxy/http_rules.h>
#include <haproxy/log.h>
#include <haproxy/proxy.h>
#include <haproxy/sample.h>
#include <haproxy/stream.h>
#include <haproxy/tcp_rules.h>
#include <haproxy/thread.h>
#include <haproxy/time.h>
#include <haproxy/tools.h>

#include <haproxy/freq_ctr-t.h>
#include <haproxy/khash.h>
#include <haproxy/rate_limit.h>

const char* weir_flt_id = "weir bandwidth limitation filter";
const int USERMAP_CLEANUP_INTERVAL_MS = 30000;
const int USERMAP_CLEANUP_MIN_MS_SINCE_DISCONNECT = 5000;
const unsigned int DEFAULT_REFRESH_INTERVAL_MS = 10000;
const unsigned int DEFAULT_UNKNOWN_USER_LIMIT =
    10 * 1024 * 1024; // Default to a 10Mbps limit when we've not received a limit for a user
const unsigned int DEFAULT_MINIMUM_BANDWIDTH_LIMIT = 16 * 1024;

// These need to be `#define`s rather than `const int`s to be allowed to use the result as an array length
#define MAX_PORT_STRING_LENGTH 5 // The largest valid port number is ~65k, or 5 decimal characters
#define INSTANCE_ID_EXTRA_BUFFER_CAPACITY                                                                              \
    2 // Allow for a separator char (between hostname and port), plus a null-terminator
#define MAX_INSTANCE_ID_LENGTH (MAX_HOSTNAME_LEN + MAX_PORT_STRING_LENGTH + INSTANCE_ID_EXTRA_BUFFER_CAPACITY)

// Simple wrapper around BUG_ON that also logs the failure to the log file.
// This is useful to get debug info in easily accessible place like the log file
#define WEIR_BUG_ON(cond)                                                                                              \
    do {                                                                                                               \
        if (cond) {                                                                                                    \
            send_log(NULL, LOG_EMERG, "[BUG] Fatal: %s at %s:%d (%s)", #cond, __FILE__, __LINE__, __func__);           \
            BUG_ON(cond);                                                                                              \
        }                                                                                                              \
    } while (0)

struct user_direction_limit {
    bool limit_received;
    uint64_t limit_timestamp;
    uint bytes_per_second;

    struct freq_ctr counter;

    int active_requests;

    // The next tick at which we're allowed to emit a log about the user exceeding their limit
    unsigned int next_throttle_log_tick;
};

struct user_limit {
    struct user_direction_limit upload;
    struct user_direction_limit download;
    unsigned int last_request_end_tick;
};
KHASH_MAP_INIT_STR(user_limit_hashtable_type, struct user_limit*)

struct weir_filter_config {
    khash_t(user_limit_hashtable_type) * user_limit_state;
    HA_RWLOCK_T state_lock;
    int next_cleanup_tick;

    struct task* refresh_task;
    int refresh_interval_ms;

    unsigned int unknown_user_limit;
    unsigned int minimum_limit;
    char instance_id[MAX_INSTANCE_ID_LENGTH];
};

struct weir_lim_state {
    struct sockaddr_in* remote_addr;

    // To avoid having to look up the relevant hashtable entry every time, we store a pointer here instead.
    // This works because the hashtable itself stores pointers (rather than values), so we don't have to worry
    // about the memory moving around when the hashtable grows.
    // We also know that this pointer will be valid for the entire lifetime of this filter state because
    // the user info only gets removed from the hashtable (and freed) some time *after* the last request
    // for that user has ended (at which point all filters for that user are gone).
    struct user_limit* limit;

    char* limit_key;
    char* request_class;
    char* bandwidth_limit_direction;
    unsigned int next_allowed_send_tick;
    bool enabled;
    bool headers_processed;
};

/* Pools used to allocate limit state structs */
DECLARE_STATIC_POOL(pool_head_weir_lim_state, "weir_lim_state", sizeof(struct weir_lim_state));
DECLARE_STATIC_POOL(pool_head_weir_user_limit, "user_limit", sizeof(struct user_limit));

struct weir_filter_config* g_filter = NULL;

int weir_ingest_limit_share_update(uint64_t timestamp, const char* user_key, const char* instance_id,
                                   const char* direction, uint64_t new_limit_share) {
    khint_t iter;
    struct user_limit* user_limit = NULL;

    if (g_filter == NULL) {
        return 0;
    }
    if (strcmp(g_filter->instance_id, instance_id) != 0) {
        return 0;
    }
    send_log(NULL, LOG_DEBUG, "Received a weir limit-share update for user %s/%s: %lubps = %lumbps", user_key,
             direction, new_limit_share, new_limit_share / (1024 * 1024));

    HA_RWLOCK_WRLOCK(OTHER_LOCK, &g_filter->state_lock);
    iter = kh_get(user_limit_hashtable_type, g_filter->user_limit_state, user_key);
    if (iter == kh_end(g_filter->user_limit_state)) {
        int insert_result;
        char* key_duplicate = strdup(user_key); // Freed when the entry is removed from the hashtable

        user_limit = pool_zalloc(pool_head_weir_user_limit);
        WEIR_BUG_ON(user_limit == NULL);
        iter = kh_put(user_limit_hashtable_type, g_filter->user_limit_state, key_duplicate, &insert_result);
        kh_value(g_filter->user_limit_state, iter) = user_limit;
    } else {
        user_limit = kh_value(g_filter->user_limit_state, iter);
    }

    // The freq_ctr that we use, which accurately handles all of the abstract rate-limiting logic
    // for us in an efficient thread-safe fashion, operates on `uint`s. In return for not having
    // to re-implement all of this logic ourselves, we are bound to values that fit into 32-bits
    // (unsigned).
    // In practice this is unlikely to be a problem for us because that limit applies only on a
    // per-instance basis, so even if a client uses a QoS policy that provides >4GB/s bandwidth,
    // they will be forcibly snapped down to 4GB/s, but it'll be 4GB/s *per haproxy instance*.
    // If they want their full throughput, they need only spread that load across several
    // instances and as long as the system balances it out sufficiently-evenly, they will still
    // get their full allocated throughput.
    if (new_limit_share > UINT_MAX) {
        send_log(NULL, LOG_WARNING,
                 "Received a weir limit-share user %s/%s that exceeds the 4GB/s limit. Clamping from %lubps to %ubps.",
                 user_key, direction, new_limit_share, UINT_MAX);
        new_limit_share = UINT_MAX;
    }

    if (strcmp(direction, "up") == 0) {
        user_limit->upload.limit_received = true;
        if (timestamp >= user_limit->upload.limit_timestamp) {
            user_limit->upload.limit_timestamp = timestamp;
            user_limit->upload.bytes_per_second = new_limit_share;
        }
    } else if (strcmp(direction, "dwn") == 0) {
        user_limit->download.limit_received = true;
        if (timestamp >= user_limit->download.limit_timestamp) {
            user_limit->download.limit_timestamp = timestamp;
            user_limit->download.bytes_per_second = new_limit_share;
        }
    } else {
        send_log(NULL, LOG_WARNING, "Received a weir limit-share update with unrecognised direction '%s'\n", direction);
    }
    HA_RWLOCK_WRUNLOCK(OTHER_LOCK, &g_filter->state_lock);

    return 1;
}

/***************************************************************************
 * Hooks that manage the filter lifecycle (init/check/deinit)
 **************************************************************************/
/* Initialize the filter. Returns -1 on error, else 0. */
static int weir_init(struct proxy* px, struct flt_conf* fconf) {
    init_speed_epoch_hashmaps();
    fconf->flags |= FLT_CFG_FL_HTX;
    return 0;
}

static void weir_deinit(struct proxy* px, struct flt_conf* fconf) {
    struct weir_filter_config* conf = fconf->conf;
    if (conf != NULL) {
        kh_destroy(user_limit_hashtable_type, conf->user_limit_state);
        HA_RWLOCK_DESTROY(&conf->state_lock);
        ha_free(&fconf->conf);
    }
}

/**************************************************************************
 * Hooks to handle start/stop of requests
 *************************************************************************/
static const char* method_name(enum http_meth_t method) {
    switch (method) {
    case HTTP_METH_OPTIONS:
        return "OPTIONS";
    case HTTP_METH_GET:
        return "GET";
    case HTTP_METH_HEAD:
        return "HEAD";
    case HTTP_METH_POST:
        return "POST";
    case HTTP_METH_PUT:
        return "PUT";
    case HTTP_METH_DELETE:
        return "DELETE";
    case HTTP_METH_TRACE:
        return "TRACE";
    case HTTP_METH_CONNECT:
        return "CONNECT";

    case HTTP_METH_OTHER:
    default:
        return "OTHER";
    }
}

static DataDirection verb_direction(enum http_meth_t method) {
    if ((method == HTTP_METH_PUT) || (method == HTTP_METH_POST)) {
        return RL_UPLOAD;
    } else {
        return RL_DOWNLOAD;
    }
}

static const char* direction_name(DataDirection direction) {
    switch (direction) {
    case RL_UPLOAD:
        return "up";
    case RL_DOWNLOAD:
        return "dwn";
    }
    send_log(NULL, LOG_WARNING, "Attempt to retrieve the name of unrecognised direction: %d", (int)direction);
    return "unknown";
}

/* Called when a filter instance is created and attached to a stream */
static int weir_attach(struct stream* s, struct filter* filter) {
    struct weir_lim_state* st = NULL;
    struct connection* conn = NULL;

    st = pool_zalloc(pool_head_weir_lim_state);
    if (!st)
        return -1;
    filter->ctx = st;

    // Weir uses the remote IP of the connection to identify it internally.
    // If the stream does not have a connection, that connection doesn't have a source
    // address, or the source address is not an instance of `sockaddr_in`, then weir
    // can't limit the stream.
    conn = sc_conn(s->scf);
    if ((conn != NULL) && (conn->src != NULL) && is_inet_addr(conn->src)) {
        st->remote_addr = (struct sockaddr_in*)conn->src;
    }

    return 1;
}

/* Called when a filter instance is detached from a stream, just before its
 * destruction */
static void weir_detach(struct stream* s, struct filter* filter) {
    struct weir_filter_config* conf = FLT_CONF(filter);
    struct weir_lim_state* st = filter->ctx;
    int active_requests = 0;

    if (!st)
        return;

    WEIR_BUG_ON(conf == NULL);
    CHECK_IF(s->txn == NULL);

    if (st->enabled && st->headers_processed && (s->txn != NULL) && (st->remote_addr != NULL)) {
        // If the lua script provides an empty limit string then the `headers` callback never runs, so we don't
        // issue the request command and we shouldn't issue the request-end command either.

        WEIR_BUG_ON(st->limit == NULL); // We should definitely have a limit if we've been enabled on this stream
        WEIR_BUG_ON(st->limit_key == NULL);
        WEIR_BUG_ON(st->bandwidth_limit_direction == NULL);

        HA_RWLOCK_WRLOCK(OTHER_LOCK, &conf->state_lock);
        st->limit->last_request_end_tick = now_ms;
        if (verb_direction(s->txn->meth) == RL_DOWNLOAD) {
            st->limit->download.active_requests -= 1;
            active_requests = st->limit->download.active_requests;
        } else {
            st->limit->upload.active_requests -= 1;
            active_requests = st->limit->upload.active_requests;
        }
        HA_RWLOCK_WRUNLOCK(OTHER_LOCK, &conf->state_lock);

        WARN_ON(active_requests < 0);
        send_log(NULL, LOG_INFO, "req_end~|~%s:%d~|~%s~|~%s~|~%s~|~%s~|~%d", inet_ntoa(st->remote_addr->sin_addr),
                 ntohs(st->remote_addr->sin_port), st->limit_key, method_name(s->txn->meth),
                 st->bandwidth_limit_direction, conf->instance_id, active_requests);

        rl_request_end(st->remote_addr);
    }

    /* release filter context allocated on attach */
    ha_free(&st->limit_key);     // This could be null if we rejected the request before attaching to it
    ha_free(&st->request_class); // This could be null
    ha_free(&st->bandwidth_limit_direction);
    pool_free(pool_head_weir_lim_state, st);
    filter->ctx = NULL;
}

/**************************************************************************
 * Hooks to filter HTTP messages
 *************************************************************************/
static int weir_http_headers(struct stream* s, struct filter* filter, struct http_msg* msg) {
    struct weir_filter_config* conf = FLT_CONF(filter);
    struct weir_lim_state* st = filter->ctx;
    const int is_request = (msg->chn == &s->req); // This function is called for both the request and the response.

    WEIR_BUG_ON(conf == NULL);
    WEIR_BUG_ON(st == NULL);
    WEIR_BUG_ON(s == NULL);
    CHECK_IF(s->txn == NULL);
    if (st->enabled && is_request && (s->txn != NULL) && (st->remote_addr != NULL)) {
        int active_requests = 0;

        // We need to flag that we've actually processed a request because this callback always runs after all of
        // the frontend lua/config processing is complete, but won't run if the request has been rejected.
        // This accounts for the case where the filter gets attached but then the request is rejected by another check
        // in config.
        st->headers_processed = true;

        WEIR_BUG_ON(st->limit == NULL); // We should definitely have a limit if we've been enabled on this stream
        HA_RWLOCK_RDLOCK(OTHER_LOCK, &conf->state_lock);
        if (verb_direction(s->txn->meth) == RL_DOWNLOAD) {
            active_requests = st->limit->download.active_requests;
        } else {
            active_requests = st->limit->upload.active_requests;
        }
        HA_RWLOCK_RDUNLOCK(OTHER_LOCK, &conf->state_lock);

        WEIR_BUG_ON(st->limit_key == NULL);
        WEIR_BUG_ON(st->bandwidth_limit_direction == NULL);
        // request_class is an optional argument, we should not assume it is always set
        const char* request_class = "";
        if (st->request_class != NULL) {
            request_class = st->request_class;
        }
        send_log(NULL, LOG_INFO, "req~|~%s:%d~|~%s~|~%s~|~%s~|~%s~|~%d~|~%s", inet_ntoa(st->remote_addr->sin_addr),
                 ntohs(st->remote_addr->sin_port), st->limit_key, method_name(s->txn->meth),
                 st->bandwidth_limit_direction, conf->instance_id, active_requests, request_class);
    }

    msg->chn->analyse_exp = TICK_ETERNITY;
    return 1;
}

struct apply_limit_result {
    int wait_ms;
    int bytes_to_forward;
};

/* Ensures that no more than <limit> bytes are transmitted per second, split across <requests> concurrent requests.
 * <len> is the maximum amount of data that the filter can forward right now.
 * This function applies the limitation and returns what the stream is authorized to forward immediately, along
 * with the amount of time it should wait before attempting to forward any more data.
 *
 * This function is adapted from the code for the `bwlim` bandwidth-limit filter.
 * This function is safe to call concurrently from multiple threads for the same `freq_ctr` value, since it
 * contains only one mutating call to the freq_ctr and all freq_ctr functions are individually safe to call
 * from multiple threads concurrently.
 */
static struct apply_limit_result apply_bandwidth_limit(struct freq_ctr* counter, uint limit, int requests,
                                                       unsigned int bytes_available) {
    unsigned int quota_bytes_remaining = 0;
    int overshoot_bytes = 0;
    const uint64_t period_ms = 1000; // All our limits are defined per-second, so the counting period is 1000ms
    const uint64_t max_wait_ms =
        2 * period_ms; // We operate on a sliding window of 2 periods, so never wait for longer than that
    struct apply_limit_result result = {.wait_ms = 0, .bytes_to_forward = bytes_available};

    /* Be sure the current rate does not exceed the limit over the current
     * period. In this case, nothing is forwarded and the waiting time is
     * computed to be sure to not retry too early.
     *
     * The test is used to avoid the initial burst. Otherwise, requests will
     * consume the limit as fast as possible and will then be paused for
     * long time.
     */
    overshoot_bytes = freq_ctr_overshoot_period(counter, period_ms, limit);
    if (overshoot_bytes > 0) {
        struct apply_limit_result result = {.wait_ms = max_wait_ms, .bytes_to_forward = 0};

        // Only compute a proportional wait time if we have a positive limit.
        // If we've overshot because the limit is zero, then always wait for the max time.
        // This prevents us from attempting a division by zero.
        if (limit > 0) {
            result.wait_ms = MIN(max_wait_ms, div64_32((uint64_t)(overshoot_bytes)*period_ms * requests, limit));
        }

        return result;
    }

    /* Get the allowed quota per user. */
    quota_bytes_remaining = freq_ctr_remain_period(counter, period_ms, limit, 0);

    /* Divide the remaining quota evenly between all local active concurrent requests on the same limit */
    quota_bytes_remaining = div64_32((uint64_t)(quota_bytes_remaining + requests - 1), requests);
    result.bytes_to_forward = MIN(result.bytes_to_forward, quota_bytes_remaining);

    /* At the end, update the freq-counter and compute the waiting time if
     * the stream is limited
     */
    update_freq_ctr_period(counter, period_ms, result.bytes_to_forward);
    if (result.bytes_to_forward < bytes_available) {
        result.wait_ms = MIN(max_wait_ms, next_event_delay_period(counter, period_ms, limit, 0));
    }

    return result;
}

static int weir_http_payload(struct stream* s, struct filter* filter, struct http_msg* msg, unsigned int offset,
                             unsigned int len) {
    struct weir_lim_state* st = filter->ctx;
    const DataDirection direction = (msg->chn == &s->req) ? RL_UPLOAD : RL_DOWNLOAD;
    int bytes_to_forward = 0;

    WEIR_BUG_ON(!st->enabled); // We should only be registering the data callback when enabling the filter
    if (st->remote_addr == NULL) {
        bytes_to_forward = len;
    } else if ((len > 0) &&
               (!tick_isset(st->next_allowed_send_tick) || tick_is_expired(st->next_allowed_send_tick, now_ms))) {
        st->next_allowed_send_tick = TICK_ETERNITY;

        WEIR_BUG_ON(st->limit == NULL);
        WEIR_BUG_ON(st->bandwidth_limit_direction == NULL);

        // do not proceed with transferring data if we are throttling this connection
        if (rl_speed_throttle(st->remote_addr, direction) == RL_THROTTLE) {
            unsigned int* next_tick_ptr = (direction == RL_DOWNLOAD) ? &st->limit->download.next_throttle_log_tick
                                                                     : &st->limit->upload.next_throttle_log_tick;
            unsigned int next_throttle_log_tick = HA_ATOMIC_LOAD(next_tick_ptr);

            send_log(NULL, LOG_DEBUG, "Throttling %s connection to %s:%u", st->bandwidth_limit_direction,
                     inet_ntoa(st->remote_addr->sin_addr), ntohs(st->remote_addr->sin_port));

            st->next_allowed_send_tick = tick_add(now_ms, MS_TO_TICKS(1));

            if (!tick_isset(next_throttle_log_tick) || tick_is_expired(next_throttle_log_tick, now_ms)) {
                unsigned int new_log_tick = tick_add(now_ms, MS_TO_TICKS(1000));
                const bool exchange_success = HA_ATOMIC_CAS(next_tick_ptr, &next_throttle_log_tick, new_log_tick);

                // We only want to log once each second for each user but there could be many different threads
                // processing requests for this user, so we do an atomic compare-and-swap (CAS) on the tick at which
                // we're next allowed to swap. If the CAS goes through successfully then we're the thread that changed
                // it, so we can log. If it failed then another thread got in before us and they would have logged, so
                // we can just skip that here.
                if (exchange_success) {
                    struct timespec t = {};
                    const int result = clock_gettime(CLOCK_REALTIME, &t);
                    const long long timestamp_usec = (t.tv_sec * 1000000) + (t.tv_nsec / 1000);
                    WARN_ON(result != 0);

                    send_log(NULL, LOG_INFO, "weir-throttle~|~%lld~|~user_bnd_%s~|~%s", timestamp_usec,
                             st->bandwidth_limit_direction, st->limit_key);
                }
            }
        } else {
            bytes_to_forward = len;
            rl_data_transferred(st->remote_addr, direction, len);
        }
    }

    // Honestly, I don't understand exactly why this is required to make it work.
    // This is what flt_bwlim does and if we don't set this correctly then either
    // HAProxy stops processing the stream (if we return 0 bytes to forward without
    // setting `analyse_exp` appropriately on the channel) or it hits a watchdog
    // timer and asserts (if we set return 0 bytes to forward and set `analyse_exp`
    // to something too small).
    msg->chn->analyse_exp =
        tick_first((tick_is_expired(msg->chn->analyse_exp, now_ms) ? TICK_ETERNITY : msg->chn->analyse_exp),
                   st->next_allowed_send_tick);
    return bytes_to_forward;
}

/********************************************************************
 * Functions that manage the filter initialization
 ********************************************************************/
static struct flt_ops weir_lim_ops = {
    /* Manage weir filter, called for each filter declaration */
    .init = weir_init,
    .deinit = weir_deinit,

    /* Handle start/stop of requests */
    .attach = weir_attach,
    .detach = weir_detach,

    /* Filter HTTP requests and responses */
    .http_headers = weir_http_headers,
    .http_payload = weir_http_payload,
};

/* Enable the filter on a stream. It always returns ACT_RET_CONT. On error, the rule is ignored.
 */
static enum act_return weir_enable_filter(struct act_rule* rule, struct proxy* px, struct session* sess,
                                          struct stream* s, int flags) {
    struct filter* filter;
    struct weir_filter_config* conf = NULL;
    struct weir_lim_state* st = NULL;
    struct sample* smp;
    int sample_options;
    khint_t iter;

    list_for_each_entry(filter, &s->strm_flt.filters, list) {
        if (FLT_ID(filter) == weir_flt_id) {
            st = filter->ctx;
            conf = FLT_CONF(filter);
            break;
        }
    }

    if (!st) {
        return ACT_RET_CONT;
    }

    switch (rule->from) {
    case ACT_F_HTTP_REQ:
        sample_options = SMP_OPT_DIR_REQ | SMP_OPT_FINAL;
        break;
    case ACT_F_HTTP_RES:
        sample_options = SMP_OPT_DIR_RES | SMP_OPT_FINAL;
        break;
    default:
        return ACT_RET_CONT;
    }

    // If one included multiple 'activate-weir' declarations in their config then the filter would
    // get enabled twice and since we count active-requests by the number of filter activations (regardless
    // of the number of requests started) but decrement the count with the number of times the filter detaches,
    // if you enabled the filter multiple times for a single request then haproxy would permanently think
    // that there was +1 active request forever.
    // Even if many activate-weir calls did not result in incorrect request counts, this would still be
    // undesirable because each of those calls could in theory pass a different key, which would be confusing at best.
    if (st->enabled) {
        send_log(NULL, LOG_WARNING,
                 "WARNING: Attempt to activate weir twice on the same request, "
                 "check if there are two 'activate-weir' lines in your config. "
                 "Activations beyond the first will be ignored.");
        return ACT_RET_CONT;
    }

    // The header, attach, and detach callbacks will all always run, regardless of whether the config requests
    // any filter actions. To allow the user to enable or disable limiting on a per-request basis using ACLs,
    // we specifically check the `enabled` flag and avoid any processing if it isn't set.
    st->enabled = true;

    // --- Parse and store: user-key ---
    if (rule->arg.act.p[0]) {
        smp = sample_fetch_as_type(px, sess, s, sample_options, rule->arg.act.p[0], SMP_T_STR);
        if (smp && smp->data.u.str.area) {
            ha_free(&st->limit_key);
            st->limit_key = strdup(smp->data.u.str.area);
        }
    }

    // --- Parse and store: operation-class ---
    if (rule->arg.act.p[1]) {
        smp = sample_fetch_as_type(px, sess, s, sample_options, rule->arg.act.p[1], SMP_T_STR);
        if (smp && smp->data.u.str.area) {
            ha_free(&st->request_class);
            st->request_class = strdup(smp->data.u.str.area);
        }
    }

    // --- Parse and store: operation-direction ---
    if (rule->arg.act.p[2]) {
        smp = sample_fetch_as_type(px, sess, s, sample_options, rule->arg.act.p[2], SMP_T_STR);
        if (smp && smp->data.u.str.area) {
            ha_free(&st->bandwidth_limit_direction);
            st->bandwidth_limit_direction = strdup(smp->data.u.str.area);
            if (strcmp(st->bandwidth_limit_direction, "up") != 0 && strcmp(st->bandwidth_limit_direction, "dwn") != 0) {
                send_log(NULL, LOG_WARNING, "WARNING: Unexpected bandwidth_limit_direction:%s",
                         st->bandwidth_limit_direction);
                return ACT_RET_CONT;
            }
        }
    }

    // Apply the filter to data transferred both for the request and response
    register_data_filter(s, &s->req, filter);
    register_data_filter(s, &s->res, filter);

    // Update the user-limit table with this filter
    WEIR_BUG_ON(conf == NULL);
    WEIR_BUG_ON(conf->user_limit_state == NULL);
    WEIR_BUG_ON(st->limit_key == NULL);

    HA_RWLOCK_WRLOCK(OTHER_LOCK, &conf->state_lock);
    iter = kh_get(user_limit_hashtable_type, conf->user_limit_state, st->limit_key);
    if (iter == kh_end(conf->user_limit_state)) {
        int insert_result;
        char* key_duplicate = strdup(st->limit_key); // Freed when the entry is removed from the hashtable

        st->limit = pool_zalloc(pool_head_weir_user_limit);
        WEIR_BUG_ON(st->limit == NULL);
        iter = kh_put(user_limit_hashtable_type, conf->user_limit_state, key_duplicate, &insert_result);
        kh_value(conf->user_limit_state, iter) = st->limit;
    } else {
        st->limit = kh_value(conf->user_limit_state, iter);
        WEIR_BUG_ON(st->limit == NULL);
    }
    if (verb_direction(s->txn->meth) == RL_DOWNLOAD) {
        st->limit->download.active_requests += 1;
    } else {
        st->limit->upload.active_requests += 1;
    }

    // Clean old entries out of the user-limit table
    if (tick_is_expired(conf->next_cleanup_tick, now_ms)) {
        for (khint_t iter = kh_begin(conf->user_limit_state); iter != kh_end(conf->user_limit_state); iter++) {
            struct user_limit* user_limits;
            if (!kh_exist(conf->user_limit_state, iter)) {
                continue;
            }

            user_limits = kh_value(conf->user_limit_state, iter);
            WARN_ON(user_limits->download.active_requests < 0);
            WARN_ON(user_limits->upload.active_requests < 0);
            if ((user_limits->download.active_requests <= 0) && (user_limits->upload.active_requests <= 0)) {
                // Even if the user has no active requests, make sure we've waited a few seconds since the last one
                // ended before cleaning up their data. This ensures that if they quick make another request (e.g if
                // they're doing many requests in serial), their bandwidth usage from previous requests is taken into
                // account for the new requests.
                const unsigned int user_expire_tick =
                    tick_add(user_limits->last_request_end_tick, USERMAP_CLEANUP_MIN_MS_SINCE_DISCONNECT);
                if (tick_is_expired(user_expire_tick, now_ms)) {
                    pool_free(pool_head_weir_user_limit, user_limits);
                    ha_free((void**)&kh_key(conf->user_limit_state, iter));
                    kh_del(user_limit_hashtable_type, conf->user_limit_state, iter);
                }
            }
        }

        conf->next_cleanup_tick = tick_add(now_ms, MS_TO_TICKS(USERMAP_CLEANUP_INTERVAL_MS));
    }
    HA_RWLOCK_WRUNLOCK(OTHER_LOCK, &conf->state_lock);

    return ACT_RET_CONT;
}

static void release_weir_action(struct act_rule* rule) {
    if (rule->arg.act.p[0]) {
        release_sample_expr(rule->arg.act.p[0]);
        rule->arg.act.p[0] = NULL;
    }
    if (rule->arg.act.p[1]) {
        release_sample_expr(rule->arg.act.p[1]);
        rule->arg.act.p[1] = NULL;
    }
    if (rule->arg.act.p[2]) {
        release_sample_expr(rule->arg.act.p[2]);
        rule->arg.act.p[2] = NULL;
    }
}

/* Parse "activate-weir" action with named arguments.
 * Supported keys: user-key, operation-class, operation-direction
 * Returns ACT_RET_PRS_OK on success, ACT_RET_PRS_ERR on error.
 */
static enum act_parse_ret activate_weir_limit(const char** args, int* orig_arg, struct proxy* px, struct act_rule* rule,
                                              char** err) {
    int cur_arg = *orig_arg;
    struct flt_conf* fc = NULL;
    bool weir_filter_found = false;

    // Prevent declaration of an 'activate-weir' action without a weir filter defined.
    // If you don't define a weir filter a lot of the hooks from haproxy won't run, so no bandwidth limiting will be
    // done. This error ensures that doesn't happen silently and instead fails hard on startup.
    list_for_each_entry(fc, &px->filter_configs, list) {
        if (fc->id == weir_flt_id) {
            weir_filter_found = true;
            break;
        }
    }

    if (!weir_filter_found) {
        memprintf(err,
                  "No weir filter declared but activate-weir called. "
                  "Ensure a weir filter is declared for proxy '%s'",
                  px->id);
        release_weir_action(rule);
        return ACT_RET_PRS_ERR;
    }
    if (!*args[cur_arg]) {
        memprintf(err, "missing key-extraction expression");
        release_weir_action(rule);
        return ACT_RET_PRS_ERR;
    }
    // Initialize expression slots to NULL
    rule->arg.act.p[0] = NULL; // user-key
    rule->arg.act.p[1] = NULL; // operation-class
    rule->arg.act.p[2] = NULL; // operation-direction

    while (*args[cur_arg]) {
        const char* arg_name = args[cur_arg];
        if ((strcmp(arg_name, "user-key") != 0) && (strcmp(arg_name, "operation-class") != 0) &&
            (strcmp(arg_name, "operation-direction") != 0)) {
            // We've parsed passed all the expected tokens, stop here so that we don't interfere with the rest of the
            // expression (namely for adding a condition to this config line).
            break;
        }
        cur_arg++;

        if (!*args[cur_arg]) {
            memprintf(err, "Missing value for argument '%s'", arg_name);
            release_weir_action(rule);
            return ACT_RET_PRS_ERR;
        }

        struct sample_expr* expr = sample_parse_expr((char**)args, &cur_arg, px->conf.args.file, px->conf.args.line,
                                                     NULL, &px->conf.args, NULL);
        if (!expr) {
            memprintf(err, "Invalid sample expression for argument '%s'", arg_name);
            release_weir_action(rule);
            return ACT_RET_PRS_ERR;
        }

        if (strcmp(arg_name, "user-key") == 0) {
            rule->arg.act.p[0] = expr;
        } else if (strcmp(arg_name, "operation-class") == 0) {
            rule->arg.act.p[1] = expr;
        } else if (strcmp(arg_name, "operation-direction") == 0) {
            rule->arg.act.p[2] = expr;
        } else {
            memprintf(err, "Unrecognized argument name: '%s'", arg_name);
            release_sample_expr(expr);
            release_weir_action(rule);
            return ACT_RET_PRS_ERR;
        }
    }

    rule->action_ptr = weir_enable_filter;
    rule->release_ptr = release_weir_action;
    *orig_arg = cur_arg;
    return ACT_RET_PRS_OK;
}

static struct action_kw_list http_req_actions = {.kw = {{"activate-weir", activate_weir_limit, 0}, {NULL, NULL}}};

static struct action_kw_list http_res_actions = {.kw = {{"activate-weir", activate_weir_limit, 0}, {NULL, NULL}}};

INITCALL1(STG_REGISTER, http_req_keywords_register, &http_req_actions);
INITCALL1(STG_REGISTER, http_res_keywords_register, &http_res_actions);

struct task* emit_active_request_refresh(struct task* t, void* ctx, unsigned int state) {
    struct weir_filter_config* conf = (struct weir_filter_config*)ctx;
    WEIR_BUG_ON(conf == NULL);

    HA_RWLOCK_RDLOCK(OTHER_LOCK, &conf->state_lock);
    for (khint_t iter = kh_begin(conf->user_limit_state); iter != kh_end(conf->user_limit_state); iter++) {
        struct user_limit* user_limits = NULL;
        const char* user_key = NULL;
        if (!kh_exist(conf->user_limit_state, iter)) {
            continue;
        }

        user_key = kh_key(conf->user_limit_state, iter);
        user_limits = kh_value(conf->user_limit_state, iter);
        if (user_limits->download.active_requests > 0) {
            send_log(NULL, LOG_INFO, "active_reqs~|~%s~|~%s~|~%s~|~%d", conf->instance_id, user_key,
                     direction_name(RL_DOWNLOAD), user_limits->download.active_requests);
        }
        if (user_limits->upload.active_requests > 0) {
            send_log(NULL, LOG_INFO, "active_reqs~|~%s~|~%s~|~%s~|~%d", conf->instance_id, user_key,
                     direction_name(RL_UPLOAD), user_limits->upload.active_requests);
        }
    }
    HA_RWLOCK_RDUNLOCK(OTHER_LOCK, &conf->state_lock);

    t->expire = tick_add(now_ms, MS_TO_TICKS(conf->refresh_interval_ms));
    return t;
}

/* Generic function to parse filter configuration.
 * Returns -1 on error and 0 on success.
 */
static int parse_weir_flt(char** args, int* cur_arg, struct proxy* px, struct flt_conf* fconf, char** err,
                          void* private) {
    struct flt_conf* fc = NULL;
    struct weir_filter_config* conf = NULL;
    khash_t(user_limit_hashtable_type)* user_limit_state = NULL;
    struct listener* listener = NULL;
    struct task* refresh_task = NULL;
    int pos = *cur_arg + 1;
    unsigned int refresh_interval_ms = DEFAULT_REFRESH_INTERVAL_MS;
    unsigned int unknown_user_limit =
        DEFAULT_UNKNOWN_USER_LIMIT; // Default to a 1Mbps limit when we've not received a limit for a user
    unsigned int minimum_limit = DEFAULT_MINIMUM_BANDWIDTH_LIMIT;

    // Prevent declaration of multiple weir filters on the same frontend
    list_for_each_entry(fc, &px->filter_configs, list) {
        if (fc->id == weir_flt_id) {
            memprintf(err, "weir filter already declared for proxy '%s'", px->id);
            return -1;
        }
    }

    // Get the first listener attached to this proxy, so we can use the listening port to identify the instance
    if (!LIST_ISEMPTY(&px->conf.listeners)) {
        listener = LIST_ELEM((&px->conf.listeners)->n, typeof(listener), by_fe);
    }
    if (listener == NULL) {
        memprintf(err,
                  "%s: no listener found for weir filter in proxy %s. Make sure you declare it in a frontend with a "
                  "'bind' directive.",
                  args[*cur_arg], px->id);
        return -1;
    }

    // Parse config
    while (*args[pos]) {
        if (strcmp(args[pos], "active-requests-refresh-interval") == 0) {
            const char* res = NULL;
            if (!*args[pos + 1]) {
                memprintf(err, "'%s': the value is missing for filter option '%s'", args[*cur_arg], args[pos]);
                return -1;
            }
            res = parse_time_err(args[pos + 1], &refresh_interval_ms, TIME_UNIT_MS);
            if (res != NULL) {
                memprintf(err, "'%s' : invalid time value for option '%s' (unexpected character '%c')", args[*cur_arg],
                          args[pos], *res);
                return -1;
            }
            pos += 2;

        } else if (strcmp(args[pos], "unknown-user-limit") == 0) {
            const char* res;
            if (!*args[pos + 1]) {
                memprintf(err, "'%s': the value is missing for filter option '%s'", args[*cur_arg], args[pos]);
                return -1;
            }
            res = parse_size_err(args[pos + 1], &unknown_user_limit);
            if (res) {
                memprintf(err, "'%s' : invalid data-size value for option '%s' (unexpected character '%c')",
                          args[*cur_arg], args[pos], *res);
                return -1;
            }
            pos += 2;

        } else if (strcmp(args[pos], "minimum-limit") == 0) {
            const char* res;
            if (!*args[pos + 1]) {
                memprintf(err, "'%s': the value is missing for filter option '%s'", args[*cur_arg], args[pos]);
                return -1;
            }
            res = parse_size_err(args[pos + 1], &minimum_limit);
            if (res) {
                memprintf(err, "'%s' : invalid data-size value for option '%s' (unexpected character '%c')",
                          args[*cur_arg], args[pos], *res);
                return -1;
            }
            pos += 2;
        } else
            break;
    }

    // Setup the new filter config
    conf = calloc(1, sizeof(*conf));
    user_limit_state = kh_init(user_limit_hashtable_type);
    refresh_task = task_new_anywhere();
    if ((conf == NULL) || (user_limit_state == NULL) || (refresh_task == NULL)) {
        memprintf(err, "%s: out of memory", args[*cur_arg]);
        ha_free(&conf);
        kh_destroy(user_limit_hashtable_type, user_limit_state);
        task_destroy(refresh_task);
        return -1;
    }

    conf->user_limit_state = user_limit_state;
    conf->next_cleanup_tick = tick_add(now_ms, MS_TO_TICKS(USERMAP_CLEANUP_INTERVAL_MS));
    conf->refresh_interval_ms = refresh_interval_ms;
    conf->unknown_user_limit = unknown_user_limit;
    conf->minimum_limit = minimum_limit;
    HA_RWLOCK_INIT(&conf->state_lock);
    snprintf(conf->instance_id, sizeof(conf->instance_id), "%s-%d", localpeer, get_host_port(&listener->rx.addr));
    // We use underscore as the separator between sections of the key in redis, so we need to make sure we don't clash
    // with that here. Technically this means we could create a clash if two host/peer names were identical except for
    // one having a dash and the other having an underscore, but that can be worked around by explicitly specifying
    // a peer name and seems the lesser evil.
    for (int i = 0; i < sizeof(conf->instance_id); i++) {
        if (conf->instance_id[i] == '_') {
            conf->instance_id[i] = '-';
        }
    }

    // Setup a task to periodically emit updated active-request counts, so that we can rely on TTL expiry to remove
    // old request-count data from redis when a user stops making requests or an haproxy instance crashes or shuts
    // down (in which case we don't get any notifications to suggest this and unless we reliably wait for all requests
    // to terminate, we'll leave some non-zero counts in redis).
    conf->refresh_task = refresh_task;
    conf->refresh_task->process = emit_active_request_refresh;
    conf->refresh_task->context = conf;
    task_schedule(conf->refresh_task, tick_add(now_ms, MS_TO_TICKS(conf->refresh_interval_ms)));

    g_filter = conf;
    *cur_arg = pos;
    fconf->conf = conf;
    fconf->id = weir_flt_id;
    fconf->ops = &weir_lim_ops;
    return 0;
}

/* Declare the filter parser for the "weir" keyword */
static struct flt_kw_list flt_kws = {"WEIR",
                                     {},
                                     {
                                         {"weir", parse_weir_flt, NULL},
                                         {NULL, NULL, NULL},
                                     }};

INITCALL1(STG_REGISTER, flt_register_keywords, &flt_kws);

struct show_lim_ctx {
    unsigned int skip;
};

static void chunk_append_limits(struct buffer* out, struct user_direction_limit* limit) {
    chunk_appendf(out, "%d,%u,%lu,%d", limit->limit_received, limit->bytes_per_second, limit->limit_timestamp,
                  limit->active_requests);
}

// This will be called repeatedly until we return 1. If we can't fit all the output into the applet output buffer
// then we return 0 and bump the number of records to skip so that we try again, continuing until we've emitted
// all the required data.
static int cli_show_weir_limits(struct appctx* appctx) {
    struct show_lim_ctx* ctx = applet_reserve_svcctx(appctx, sizeof(*ctx)); // Zeroed before the first call
    unsigned int skip = ctx->skip;
    int ret = 1;

    if (ctx->skip == 0) {
        chunk_reset(&trash);
        chunk_appendf(&trash, "Local limit shares @ tick %u:\n", now_ms);
        chunk_strcat(&trash, "User key,Last request-end tick,");
        chunk_strcat(&trash, "Down limit received,Down limit,Down limit timestamp,Down active requests,");
        chunk_strcat(&trash, "Up limit received,Up limit,Up limit timestamp,Up active requests\n");
        applet_putchk(appctx, &trash);
    }

    WEIR_BUG_ON(g_filter == NULL);
    HA_RWLOCK_RDLOCK(OTHER_LOCK, &g_filter->state_lock);
    for (khint_t iter = kh_begin(g_filter->user_limit_state) + ctx->skip; iter != kh_end(g_filter->user_limit_state);
         iter++) {
        struct user_limit* user_limits = NULL;
        const char* user_key = NULL;

        if (!kh_exist(g_filter->user_limit_state, iter)) {
            continue;
        }

        user_key = kh_key(g_filter->user_limit_state, iter);
        user_limits = kh_value(g_filter->user_limit_state, iter);

        chunk_reset(&trash);
        chunk_appendf(&trash, "%s,%d,", user_key, user_limits->last_request_end_tick);
        chunk_append_limits(&trash, &user_limits->upload);
        chunk_strcat(&trash, ",");
        chunk_append_limits(&trash, &user_limits->download);
        chunk_strcat(&trash, "\n");

        if (applet_putchk(appctx, &trash) == -1) {
            ctx->skip = skip;
            ret = 0;
            break;
        }
        skip++;
    }
    HA_RWLOCK_RDUNLOCK(OTHER_LOCK, &g_filter->state_lock);
    return ret;
}

/* register cli keywords */
static struct cli_kw_list cli_kws = {{},
                                     {{{"show", "weir", "limits", NULL},
                                       "show weir limits                        : Dump the current state of limits and "
                                       "limit-shares enforced locally by the Weir filter",
                                       NULL,
                                       cli_show_weir_limits,
                                       NULL},
                                      {
                                          {},
                                      }}};

INITCALL1(STG_REGISTER, cli_register_kw, &cli_kws);
