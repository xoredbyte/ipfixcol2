/*
 * src/plugins/output/dummy/dummy.c
 * Improved dummy output plugin (lock-free counters + prometheus exporter)
 * - Uses C11 atomics on the hot path to avoid mutex contention
 * - Exposes Prometheus metrics on PROMETHEUS_PORT and a records/sec gauge
 * - Uses libmicrohttpd for a lightweight metrics endpoint
 *
 * Author: adapted for performance by ChatGPT (based on original CESNET dummy plugin)
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

#include "config.h"

#define IANA_PEN     0
#define IANA_PEN_REV 29305
#define IE_ID_BYTES  1
#define IE_ID_PKTS   2

/* Prometheus port (can be changed) */
#define PROMETHEUS_PORT 9100
/* Max buffer for /metrics output (should be enough for 64-bit counters) */
#define METRICS_BUF_LEN 1024

IPX_API struct ipx_plugin_info ipx_plugin_info = {
    .type = IPX_PT_OUTPUT,
    .name = "dummy",
    .dsc = "High-performance dummy output plugin (atomic counters + prometheus).",
    .flags = 0,
    .version = "2.2.0-perf",
    .ipx_min = "2.0.0"
};

/* Instance data - atomics for counts to avoid locking on hot path */
struct instance_data {
    struct instance_config *config;

    atomic_uint_fast64_t cnt_flows_data;
    atomic_uint_fast64_t cnt_flows_opts;
    atomic_uint_fast64_t cnt_bytes;
    atomic_uint_fast64_t cnt_pkts;

    /* rps is published as an atomic gauge so Prometheus scrape does not need locking */
    atomic_uint_fast64_t rps;

    /* last snapshot used by metrics thread (not used in hot path) */
    uint64_t last_snapshot;

    /* thread control */
    atomic_int stop_threads;
};

/* Inline helper to increment counters (relaxed ordering for max perf) */
static inline void
atomic_add_u64(atomic_uint_fast64_t *a, uint64_t v)
{
    atomic_fetch_add_explicit(a, (uint_fast64_t)v, memory_order_relaxed);
}

/* Update statistics about flow records (hot path) */
static void
stats_update(struct instance_data *inst, ipx_msg_ipfix_t *msg)
{
    uint32_t rec_cnt = ipx_msg_ipfix_get_drec_cnt(msg);

    for (uint32_t i = 0; i < rec_cnt; ++i) {
        struct ipx_ipfix_record *rec_ptr = ipx_msg_ipfix_get_drec(msg, i);
        const struct fds_template *tmplt = rec_ptr->rec.tmplt;
        const enum fds_template_type ttype = tmplt->type;

        struct fds_drec_field field;
        uint64_t value;

        /* Only atomic increments on hot path */
        if (ttype == FDS_TYPE_TEMPLATE) {
            atomic_add_u64(&inst->cnt_flows_data, 1);
        } else if (ttype == FDS_TYPE_TEMPLATE_OPTS) {
            atomic_add_u64(&inst->cnt_flows_opts, 1);
            continue; /* options records don't have bytes/packets counters */
        }

        /* octetDeltaCount */
        if (fds_drec_find(&rec_ptr->rec, IANA_PEN, IE_ID_BYTES, &field) != FDS_EOC
            && fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
            atomic_add_u64(&inst->cnt_bytes, value);
        }

        /* packetDeltaCount */
        if (fds_drec_find(&rec_ptr->rec, IANA_PEN, IE_ID_PKTS, &field) != FDS_EOC
            && fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
            atomic_add_u64(&inst->cnt_pkts, value);
        }

        /* Biflow (reverse) */
        if ((tmplt->flags & FDS_TEMPLATE_BIFLOW) != 0) {
            if (fds_drec_find(&rec_ptr->rec, IANA_PEN_REV, IE_ID_BYTES, &field) != FDS_EOC
                && fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
                atomic_add_u64(&inst->cnt_bytes, value);
            }

            if (fds_drec_find(&rec_ptr->rec, IANA_PEN_REV, IE_ID_PKTS, &field) != FDS_EOC
                && fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
                atomic_add_u64(&inst->cnt_pkts, value);
            }
        }
    }
}

/* Prometheus metrics handler: read atomics (relaxed) and return text */
static int
metrics_handler(void *cls, struct MHD_Connection *connection,
                const char *url, const char *method,
                const char *version, const char *upload_data,
                size_t *upload_data_size, void **con_cls)
{
    (void) url; (void) method; (void) version;
    (void) upload_data; (void) upload_data_size; (void) con_cls;

    struct instance_data *inst = (struct instance_data *)cls;

    /* Load atomics (relaxed is ok; counters monotonically increase) */
    uint64_t cnt_data = (uint64_t)atomic_load_explicit(&inst->cnt_flows_data, memory_order_relaxed);
    uint64_t cnt_opts = (uint64_t)atomic_load_explicit(&inst->cnt_flows_opts, memory_order_relaxed);
    uint64_t cnt_bytes = (uint64_t)atomic_load_explicit(&inst->cnt_bytes, memory_order_relaxed);
    uint64_t cnt_pkts = (uint64_t)atomic_load_explicit(&inst->cnt_pkts, memory_order_relaxed);
    uint64_t rps = (uint64_t)atomic_load_explicit(&inst->rps, memory_order_relaxed);

    char buf[METRICS_BUF_LEN];
    int len = snprintf(buf, sizeof(buf),
        "# HELP dummy_data_records_total Total data records processed\n"
        "# TYPE dummy_data_records_total counter\n"
        "dummy_data_records_total %" PRIu64 "\n"
        "# HELP dummy_options_records_total Total options records processed\n"
        "# TYPE dummy_options_records_total counter\n"
        "dummy_options_records_total %" PRIu64 "\n"
        "# HELP dummy_bytes_total Total bytes processed\n"
        "# TYPE dummy_bytes_total counter\n"
        "dummy_bytes_total %" PRIu64 "\n"
        "# HELP dummy_packets_total Total packets processed\n"
        "# TYPE dummy_packets_total counter\n"
        "dummy_packets_total %" PRIu64 "\n"
        "# HELP dummy_records_per_second Live records per second rate\n"
        "# TYPE dummy_records_per_second gauge\n"
        "dummy_records_per_second %" PRIu64 "\n",
        cnt_data, cnt_opts, cnt_bytes, cnt_pkts, rps);

    if (len < 0) {
        return MHD_NO;
    }
    if (len >= (int)sizeof(buf)) {
        /* truncated; still send up to buffer size */
        len = sizeof(buf) - 1;
    }

    struct MHD_Response *resp = MHD_create_response_from_buffer((size_t)len, (void *)buf, MHD_RESPMEM_MUST_COPY);
    if (!resp) {
        return MHD_NO;
    }
    MHD_add_response_header(resp, "Content-Type", "text/plain; version=0.0.4");
    int rc = MHD_queue_response(connection, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return rc;
}

/* Metrics thread: computes rps each second and optionally prints a short line */
static void *
metrics_thread(void *arg)
{
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
        uint64_t now = (uint64_t)atomic_load_explicit(&inst->cnt_flows_data, memory_order_relaxed);
        uint64_t last = inst->last_snapshot;
        uint64_t delta = now - last;
        inst->last_snapshot = now;
        /* publish gauge */
        atomic_store_explicit(&inst->rps, (uint_fast64_t)delta, memory_order_relaxed);

        /* minimal, infrequent printf - avoid doing this if you need absolute max throughput 
        if (inst->config && inst->config->en_stats) {
            Use a single, small formatted output
            (void)fprintf(stdout, "[dummy] rps=%" PRIu64 " total=%" PRIu64 "\n", delta, now);
            fflush(stdout);
        } */
    }

    MHD_stop_daemon(daemon);
    return NULL;
}

/* Plugin lifecycle functions */

int
ipx_plugin_init(ipx_ctx_t *ctx, const char *params)
{
    struct instance_data *data = calloc(1, sizeof(*data));
    if (!data) {
        return IPX_ERR_DENIED;
    }

    /* initialize atomics */
    atomic_init(&data->cnt_flows_data, 0);
    atomic_init(&data->cnt_flows_opts, 0);
    atomic_init(&data->cnt_bytes, 0);
    atomic_init(&data->cnt_pkts, 0);
    atomic_init(&data->rps, 0);
    atomic_init(&data->stop_threads, 0);
    data->last_snapshot = 0;

    if ((data->config = config_parse(ctx, params)) == NULL) {
        free(data);
        return IPX_ERR_DENIED;
    }

    ipx_ctx_private_set(ctx, data);

    /* subscribe to messages */
    uint16_t new_mask = IPX_MSG_IPFIX | IPX_MSG_SESSION | IPX_MSG_PERIODIC;
    ipx_ctx_subscribe(ctx, &new_mask, NULL);

    /* spawn metrics thread */
    pthread_t tid;
    if (pthread_create(&tid, NULL, metrics_thread, data) != 0) {
        config_destroy(data->config);
        free(data);
        return IPX_ERR_DENIED;
    }
    pthread_detach(tid);

    return IPX_OK;
}

void
ipx_plugin_destroy(ipx_ctx_t *ctx, void *cfg)
{
    (void) ctx;
    struct instance_data *data = (struct instance_data *) cfg;

    /* signal thread to stop */
    atomic_store_explicit(&data->stop_threads, 1, memory_order_relaxed);
    /* give thread a second to exit and stop the daemon */
    sleep(1);

    /* Optionally print final totals */
    if (data->config && data->config->en_stats) {
        uint64_t cnt_data = (uint64_t)atomic_load_explicit(&data->cnt_flows_data, memory_order_relaxed);
        uint64_t cnt_opts = (uint64_t)atomic_load_explicit(&data->cnt_flows_opts, memory_order_relaxed);
        uint64_t cnt_bytes = (uint64_t)atomic_load_explicit(&data->cnt_bytes, memory_order_relaxed);
        uint64_t cnt_pkts = (uint64_t)atomic_load_explicit(&data->cnt_pkts, memory_order_relaxed);
        fprintf(stdout, "Final Stats:\n");
        fprintf(stdout, "- data records:    %" PRIu64 "\n", cnt_data);
        fprintf(stdout, "- options records: %" PRIu64 "\n", cnt_opts);
        fprintf(stdout, "- total bytes:     %" PRIu64 "\n", cnt_bytes);
        fprintf(stdout, "- total packets:   %" PRIu64 "\n", cnt_pkts);
        fflush(stdout);
    }

    config_destroy(data->config);
    free(data);
}

int
ipx_plugin_process(ipx_ctx_t *ctx, void *cfg, ipx_msg_t *msg)
{
    struct instance_data *data = (struct instance_data *) cfg;

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

        if (data->config && data->config->en_stats) {
            stats_update(data, ipfix_msg);
        }
    }

    if (type == IPX_MSG_SESSION) {
        ipx_msg_session_t *session_msg = ipx_msg_base2session(msg);
        enum ipx_msg_session_event event = ipx_msg_session_get_event(session_msg);
        const struct ipx_session *session = ipx_msg_session_get_session(session_msg);
        const char *status_msg = (event == IPX_MSG_SESSION_OPEN) ? "opened" : "closed";
        IPX_CTX_INFO(ctx, "Transport Session '%s' %s", session->ident, status_msg);
    }

    /* respect configured per-message delay if present */
    const struct timespec *delay = &data->config->sleep_time;
    if (delay->tv_sec != 0 || delay->tv_nsec != 0) {
        nanosleep(delay, NULL);
    }

    return IPX_OK;
}

