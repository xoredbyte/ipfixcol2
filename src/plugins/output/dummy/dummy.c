/**
 * \file src/plugins/output/dummy/dummy.c
 * \author Lukas Hutak <lukas.hutak@cesnet.cz> 
 * \brief Example output plugin for IPFIXcol 2 with per-ODID metrics
 * \date 2018-2025
 */

#include <ipfixcol2.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <pthread.h>
#include <microhttpd.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdint.h>

#include "config.h"

#define IANA_PEN     0
#define IANA_PEN_REV 29305
#define IE_ID_BYTES  1
#define IE_ID_PKTS   2

#define PROMETHEUS_PORT 9101
#define METRICS_BUF_LEN 4096

#define IE_ID_FLOW_END   153  // flowEndMilliseconds
#define LATENCY_TIMEOUT_MS 30000

IPX_API struct ipx_plugin_info ipx_plugin_info = {
    .type = IPX_PT_OUTPUT,
    .name = "dummy",
    .dsc = "High-performance dummy output plugin (atomic counters + prometheus + per-ODID).",
    .flags = 0,
    .version = "2.3.0-perf",
    .ipx_min = "2.0.0"
};

/* Structure for per-ODID statistics */
struct odid_stats {
    uint32_t odid;
    atomic_uint_fast64_t cnt_flows_data;
    atomic_uint_fast64_t cnt_flows_opts;
    atomic_uint_fast64_t cnt_bytes;
    atomic_uint_fast64_t cnt_pkts;
    atomic_uint_fast64_t total_latency_ms;
    atomic_uint_fast64_t latency_count;
    atomic_uint_fast64_t last_latency_update_ms;
    struct odid_stats *next;
};

/* Global plugin instance data */
struct instance_data {
    struct instance_config *config;

    atomic_uint_fast64_t cnt_flows_data;
    atomic_uint_fast64_t cnt_flows_opts;
    atomic_uint_fast64_t cnt_bytes;
    atomic_uint_fast64_t cnt_pkts;

    atomic_uint_fast64_t rps;
    uint64_t rps_history[10];
    uint64_t rps_history_index;
    uint64_t rps_history_sum;
    uint64_t rps_history_count;
    uint64_t last_snapshot;

    atomic_int stop_threads;

    struct odid_stats *odid_head;
    pthread_mutex_t odid_lock; // protect odid linked list
};

/* Helper to increment atomics */
static inline void atomic_add_u64(atomic_uint_fast64_t *a, uint64_t v) {
    atomic_fetch_add_explicit(a, v, memory_order_relaxed);
}

/* Find or create per-ODID stats */
static struct odid_stats *get_odid_stats(struct instance_data *inst, uint32_t odid) {
    pthread_mutex_lock(&inst->odid_lock);
    struct odid_stats *cur = inst->odid_head;
    while (cur) {
        if (cur->odid == odid) {
            pthread_mutex_unlock(&inst->odid_lock);
            return cur;
        }
        cur = cur->next;
    }

    /* Not found, create new */
    struct odid_stats *new_stat = calloc(1, sizeof(*new_stat));
    if (!new_stat) {
        pthread_mutex_unlock(&inst->odid_lock);
        return NULL;
    }
    new_stat->odid = odid;
    atomic_init(&new_stat->cnt_flows_data, 0);
    atomic_init(&new_stat->cnt_flows_opts, 0);
    atomic_init(&new_stat->cnt_bytes, 0);
    atomic_init(&new_stat->cnt_pkts, 0);
    atomic_init(&new_stat->total_latency_ms, 0);
    atomic_init(&new_stat->latency_count, 0);
    atomic_init(&new_stat->last_latency_update_ms, 0);

    new_stat->next = inst->odid_head;
    inst->odid_head = new_stat;

    pthread_mutex_unlock(&inst->odid_lock);
    return new_stat;
}

/* Update stats for each IPFIX message */
static void stats_update(struct instance_data *inst, ipx_msg_ipfix_t *msg) {
    uint32_t rec_cnt = ipx_msg_ipfix_get_drec_cnt(msg);
    const struct ipx_msg_ctx *ipfix_ctx = ipx_msg_ipfix_get_ctx(msg);
    struct odid_stats *odid_stat = get_odid_stats(inst, ipfix_ctx->odid);
    if (!odid_stat) return;

    for (uint32_t i = 0; i < rec_cnt; ++i) {
        struct ipx_ipfix_record *rec_ptr = ipx_msg_ipfix_get_drec(msg, i);
        const struct fds_template *tmplt = rec_ptr->rec.tmplt;
        const enum fds_template_type ttype = tmplt->type;
        struct fds_drec_field field;
        uint64_t value;

        if (ttype == FDS_TYPE_TEMPLATE) {
            atomic_add_u64(&inst->cnt_flows_data, 1);
            atomic_add_u64(&odid_stat->cnt_flows_data, 1);
        } else if (ttype == FDS_TYPE_TEMPLATE_OPTS) {
            atomic_add_u64(&inst->cnt_flows_opts, 1);
            atomic_add_u64(&odid_stat->cnt_flows_opts, 1);
            continue;
        }

        if (fds_drec_find(&rec_ptr->rec, IANA_PEN, IE_ID_BYTES, &field) != FDS_EOC &&
            fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
            atomic_add_u64(&inst->cnt_bytes, value);
            atomic_add_u64(&odid_stat->cnt_bytes, value);
        }

        if (fds_drec_find(&rec_ptr->rec, IANA_PEN, IE_ID_PKTS, &field) != FDS_EOC &&
            fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
            atomic_add_u64(&inst->cnt_pkts, value);
            atomic_add_u64(&odid_stat->cnt_pkts, value);
        }

        if (fds_drec_find(&rec_ptr->rec, IANA_PEN, IE_ID_FLOW_END, &field) != FDS_EOC &&
            fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            uint64_t now_ms = ((uint64_t)now.tv_sec * 1000) + (now.tv_nsec / 1000000);
            if (now_ms >= value) {
                uint64_t latency = now_ms - value;
                atomic_add_u64(&odid_stat->total_latency_ms, latency);
                atomic_add_u64(&odid_stat->latency_count, 1);
                atomic_store_explicit(&odid_stat->last_latency_update_ms, now_ms, memory_order_relaxed);
            }
        }

        if ((tmplt->flags & FDS_TEMPLATE_BIFLOW) != 0) {
            if (fds_drec_find(&rec_ptr->rec, IANA_PEN_REV, IE_ID_BYTES, &field) != FDS_EOC &&
                fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
                atomic_add_u64(&inst->cnt_bytes, value);
                atomic_add_u64(&odid_stat->cnt_bytes, value);
            }

            if (fds_drec_find(&rec_ptr->rec, IANA_PEN_REV, IE_ID_PKTS, &field) != FDS_EOC &&
                fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
                atomic_add_u64(&inst->cnt_pkts, value);
                atomic_add_u64(&odid_stat->cnt_pkts, value);
            }
        }
    }
}

/* Metrics handler: global + per-ODID */
static int metrics_handler(void *cls, struct MHD_Connection *connection,
                           const char *url, const char *method,
                           const char *version, const char *upload_data,
                           size_t *upload_data_size, void **con_cls)
{
    (void)url; (void)method; (void)version;
    (void)upload_data; (void)upload_data_size; (void)con_cls;

    struct instance_data *inst = (struct instance_data *)cls;

    uint64_t cnt_data = atomic_load_explicit(&inst->cnt_flows_data, memory_order_relaxed);
    uint64_t cnt_opts = atomic_load_explicit(&inst->cnt_flows_opts, memory_order_relaxed);
    uint64_t cnt_bytes = atomic_load_explicit(&inst->cnt_bytes, memory_order_relaxed);
    uint64_t cnt_pkts = atomic_load_explicit(&inst->cnt_pkts, memory_order_relaxed);
    uint64_t rps = atomic_load_explicit(&inst->rps, memory_order_relaxed);

    char buf[METRICS_BUF_LEN];
    int len = snprintf(buf, sizeof(buf),
        "# HELP dummy_data_records_total Total data records processed (global)\n"
        "# TYPE dummy_data_records_total counter\n"
        "dummy_data_records_total %" PRIu64 "\n"
        "# HELP dummy_options_records_total Total options records processed (global)\n"
        "# TYPE dummy_options_records_total counter\n"
        "dummy_options_records_total %" PRIu64 "\n"
        "# HELP dummy_bytes_total Total bytes processed (global)\n"
        "# TYPE dummy_bytes_total counter\n"
        "dummy_bytes_total %" PRIu64 "\n"
        "# HELP dummy_packets_total Total packets processed (global)\n"
        "# TYPE dummy_packets_total counter\n"
        "dummy_packets_total %" PRIu64 "\n"
        "# HELP dummy_records_per_second Rolling average records per second (global)\n"
        "# TYPE dummy_records_per_second gauge\n"
        "dummy_records_per_second %" PRIu64 "\n",
        cnt_data, cnt_opts, cnt_bytes, cnt_pkts, rps);

    if (len < 0 || len >= (int)sizeof(buf)) return MHD_NO;

    /* Add per-ODID metrics */
    pthread_mutex_lock(&inst->odid_lock);
    struct odid_stats *cur = inst->odid_head;
    while (cur) {
        uint64_t od_cnt_data = atomic_load_explicit(&cur->cnt_flows_data, memory_order_relaxed);
        uint64_t od_cnt_opts = atomic_load_explicit(&cur->cnt_flows_opts, memory_order_relaxed);
        uint64_t od_cnt_bytes = atomic_load_explicit(&cur->cnt_bytes, memory_order_relaxed);
        uint64_t od_cnt_pkts = atomic_load_explicit(&cur->cnt_pkts, memory_order_relaxed);
        uint64_t od_latency_total = atomic_load_explicit(&cur->total_latency_ms, memory_order_relaxed);
        uint64_t od_latency_count = atomic_load_explicit(&cur->latency_count, memory_order_relaxed);
        uint64_t od_last_update = atomic_load_explicit(&cur->last_latency_update_ms, memory_order_relaxed);

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        uint64_t now_ms = ((uint64_t)now.tv_sec * 1000) + (now.tv_nsec / 1000000);

        double avg_latency = 0.0;
        if (od_latency_count > 0 && (now_ms - od_last_update) <= LATENCY_TIMEOUT_MS) {
            avg_latency = (double)od_latency_total / od_latency_count;
        }

        int n = snprintf(buf + len, sizeof(buf) - len,
            "# HELP dummy_data_records_total{odid=\"%" PRIu32 "\"} Data records for ODID\n"
            "# TYPE dummy_data_records_total counter\n"
            "dummy_data_records_total{odid=\"%" PRIu32 "\"} %" PRIu64 "\n"
            "# HELP dummy_options_records_total{odid=\"%" PRIu32 "\"} Options records for ODID\n"
            "# TYPE dummy_options_records_total counter\n"
            "dummy_options_records_total{odid=\"%" PRIu32 "\"} %" PRIu64 "\n"
            "# HELP dummy_bytes_total{odid=\"%" PRIu32 "\"} Bytes processed for ODID\n"
            "# TYPE dummy_bytes_total counter\n"
            "dummy_bytes_total{odid=\"%" PRIu32 "\"} %" PRIu64 "\n"
            "# HELP dummy_packets_total{odid=\"%" PRIu32 "\"} Packets processed for ODID\n"
            "# TYPE dummy_packets_total counter\n"
            "dummy_packets_total{odid=\"%" PRIu32 "\"} %" PRIu64 "\n"
            "# HELP dummy_ingest_latency_ms{odid=\"%" PRIu32 "\"} Avg latency for ODID\n"
            "# TYPE dummy_ingest_latency_ms gauge\n"
            "dummy_ingest_latency_ms{odid=\"%" PRIu32 "\"} %.3f\n",
            cur->odid, cur->odid, od_cnt_data,
            cur->odid, cur->odid, od_cnt_opts,
            cur->odid, cur->odid, od_cnt_bytes,
            cur->odid, cur->odid, od_cnt_pkts,
            cur->odid, cur->odid, avg_latency);

        if (n > 0) len += n;
        cur = cur->next;
    }
    pthread_mutex_unlock(&inst->odid_lock);

    struct MHD_Response *resp = MHD_create_response_from_buffer((size_t)len, buf, MHD_RESPMEM_MUST_COPY);
    if (!resp) return MHD_NO;
    MHD_add_response_header(resp, "Content-Type", "text/plain; version=0.0.4");
    int rc = MHD_queue_response(connection, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return rc;
}

/* Metrics thread same as original, only uses global counters */
static void *metrics_thread(void *arg) {
    struct instance_data *inst = (struct instance_data *)arg;

    struct MHD_Daemon *daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY,
                                                 PROMETHEUS_PORT,
                                                 NULL, NULL,
                                                 &metrics_handler, inst,
                                                 MHD_OPTION_END);
    if (!daemon) {
        fprintf(stderr, "dummy: failed to start prometheus http daemon on port %d\n", PROMETHEUS_PORT);
        return NULL;
    }

    while (!atomic_load_explicit(&inst->stop_threads, memory_order_relaxed)) {
        sleep(1);
        uint64_t now = atomic_load_explicit(&inst->cnt_flows_data, memory_order_relaxed);
        uint64_t last = inst->last_snapshot;
        uint64_t delta = (now >= last) ? (now - last) : 0;
        inst->last_snapshot = now;

        if (inst->rps_history_count >= 10) inst->rps_history_sum -= inst->rps_history[inst->rps_history_index];
        else inst->rps_history_count++;
        inst->rps_history[inst->rps_history_index] = delta;
        inst->rps_history_sum += delta;
        inst->rps_history_index = (inst->rps_history_index + 1) % 10;
        atomic_store_explicit(&inst->rps, inst->rps_history_sum / inst->rps_history_count, memory_order_relaxed);
    }

    MHD_stop_daemon(daemon);
    return NULL;
}

/* Plugin lifecycle functions */
int ipx_plugin_init(ipx_ctx_t *ctx, const char *params) {
    struct instance_data *data = calloc(1, sizeof(*data));
    if (!data) return IPX_ERR_DENIED;

    atomic_init(&data->cnt_flows_data, 0);
    atomic_init(&data->cnt_flows_opts, 0);
    atomic_init(&data->cnt_bytes, 0);
    atomic_init(&data->cnt_pkts, 0);
    atomic_init(&data->rps, 0);
    atomic_init(&data->stop_threads, 0);

    pthread_mutex_init(&data->odid_lock, NULL);

    if ((data->config = config_parse(ctx, params)) == NULL) {
        free(data);
        return IPX_ERR_DENIED;
    }

    ipx_ctx_private_set(ctx, data);

    uint16_t new_mask = IPX_MSG_IPFIX | IPX_MSG_SESSION | IPX_MSG_PERIODIC;
    ipx_ctx_subscribe(ctx, &new_mask, NULL);

    pthread_t tid;
    if (pthread_create(&tid, NULL, metrics_thread, data) != 0) {
        config_destroy(data->config);
        free(data);
        return IPX_ERR_DENIED;
    }
    pthread_detach(tid);

    return IPX_OK;
}

void ipx_plugin_destroy(ipx_ctx_t *ctx, void *cfg) {
    (void)ctx;
    struct instance_data *data = (struct instance_data *)cfg;

    atomic_store_explicit(&data->stop_threads, 1, memory_order_relaxed);
    sleep(1);

    if (data->config && data->config->en_stats) {
        uint64_t cnt_data = atomic_load_explicit(&data->cnt_flows_data, memory_order_relaxed);
        uint64_t cnt_opts = atomic_load_explicit(&data->cnt_flows_opts, memory_order_relaxed);
        uint64_t cnt_bytes = atomic_load_explicit(&data->cnt_bytes, memory_order_relaxed);
        uint64_t cnt_pkts = atomic_load_explicit(&data->cnt_pkts, memory_order_relaxed);

        fprintf(stdout, "Final Global Stats:\n");
        fprintf(stdout, "- data records:    %" PRIu64 "\n", cnt_data);
        fprintf(stdout, "- options records: %" PRIu64 "\n", cnt_opts);
        fprintf(stdout, "- total bytes:     %" PRIu64 "\n", cnt_bytes);
        fprintf(stdout, "- total packets:   %" PRIu64 "\n", cnt_pkts);
        fflush(stdout);
    }

    /* free ODID stats */
    pthread_mutex_lock(&data->odid_lock);
    struct odid_stats *cur = data->odid_head;
    while (cur) {
        struct odid_stats *tmp = cur;
        cur = cur->next;
        free(tmp);
    }
    pthread_mutex_unlock(&data->odid_lock);

    config_destroy(data->config);
    free(data);
}

int ipx_plugin_process(ipx_ctx_t *ctx, void *cfg, ipx_msg_t *msg) {
    struct instance_data *data = (struct instance_data *)cfg;

    int type = ipx_msg_get_type(msg);

    if (type == IPX_MSG_PERIODIC) {
        ipx_msg_periodic_t *periodic_message = ipx_msg_base2periodic(msg);
        IPX_CTX_INFO(ctx, "Periodic message %lu: %lu ms",
            ipx_msg_periodic_get_seq_num(periodic_message),
            (unsigned long)((ipx_msg_periodic_get_last_processed(periodic_message).tv_sec
                             - ipx_msg_periodic_get_created(periodic_message).tv_sec) * 1000));
    }

    if (type == IPX_MSG_IPFIX) {
        ipx_msg_ipfix_t *ipfix_msg = ipx_msg_base2ipfix(msg);
        const struct ipx_msg_ctx *ipfix_ctx = ipx_msg_ipfix_get_ctx(ipfix_msg);
        IPX_CTX_INFO(ctx, "[ODID: %" PRIu32 "] Received an IPFIX message", ipfix_ctx->odid);

        if (data->config && data->config->en_stats) stats_update(data, ipfix_msg);
    }

    if (type == IPX_MSG_SESSION) {
        ipx_msg_session_t *session_msg = ipx_msg_base2session(msg);
        enum ipx_msg_session_event event = ipx_msg_session_get_event(session_msg);
        const struct ipx_session *session = ipx_msg_session_get_session(session_msg);
        const char *status_msg = (event == IPX_MSG_SESSION_OPEN) ? "opened" : "closed";
        IPX_CTX_INFO(ctx, "Transport Session '%s' %s", session->ident, status_msg);
    }

    const struct timespec *delay = &data->config->sleep_time;
    if (delay->tv_sec != 0 || delay->tv_nsec != 0) nanosleep(delay, NULL);

    return IPX_OK;
}
