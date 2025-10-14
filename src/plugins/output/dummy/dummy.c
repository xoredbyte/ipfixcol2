/**
 * \file src/plugins/output/dummy/dummy.c
 * \author Lukas Hutak <lukas.hutak@cesnet.cz>
 * \brief Example output plugin for IPFIXcol 2
 * \date 2018-2020
 * 
 * Modified to support per-ODID statistics tracking
 */

/* Copyright (C) 2018-2020 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is'', and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
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
#define PROMETHEUS_PORT 9101
/* Max buffer for /metrics output (increased to handle multiple ODIDs) */
#define METRICS_BUF_LEN 65536

#define IE_ID_FLOW_END   153  // flowEndMilliseconds

/* Latency data timeout - if no new latency measurements for this many ms, return 0 */
#define LATENCY_TIMEOUT_MS 30000

IPX_API struct ipx_plugin_info ipx_plugin_info = {
    .type = IPX_PT_OUTPUT,
    .name = "dummy",
    .dsc = "High-performance dummy output plugin (atomic counters + prometheus) with per-ODID statistics.",
    .flags = 0,
    .version = "2.3.0-perf-odid",
    .ipx_min = "2.0.0"
};

/* Per-ODID statistics structure */
struct odid_stats {
    uint32_t odid;
    
    atomic_uint_fast64_t cnt_flows_data;
    atomic_uint_fast64_t cnt_flows_opts;
    atomic_uint_fast64_t cnt_bytes;
    atomic_uint_fast64_t cnt_pkts;
    atomic_uint_fast64_t rps;
    
    /* Rolling average RPS calculation - 10 second window */
    uint64_t rps_history[10];
    uint64_t rps_history_index;
    uint64_t rps_history_sum;
    uint64_t rps_history_count;
    uint64_t last_snapshot;
    
    atomic_uint_fast64_t total_latency_ms;
    atomic_uint_fast64_t latency_count;
    atomic_uint_fast64_t last_latency_update_ms;
    
    struct odid_stats *next;  /* Linked list for hash collision */
};

/* Hash table for per-ODID statistics */
#define ODID_HASH_SIZE 256
struct odid_hash_table {
    struct odid_stats *buckets[ODID_HASH_SIZE];
    pthread_mutex_t lock;
};

/* Instance data - atomics for global counts + per-ODID hash table */
struct instance_data {
    struct instance_config *config;

    /* Global statistics */
    atomic_uint_fast64_t cnt_flows_data;
    atomic_uint_fast64_t cnt_flows_opts;
    atomic_uint_fast64_t cnt_bytes;
    atomic_uint_fast64_t cnt_pkts;
    atomic_uint_fast64_t rps;
    
    /* Rolling average RPS calculation - 10 second window */
    uint64_t rps_history[10];
    uint64_t rps_history_index;
    uint64_t rps_history_sum;
    uint64_t rps_history_count;
    uint64_t last_snapshot;

    atomic_uint_fast64_t total_latency_ms;
    atomic_uint_fast64_t latency_count;
    atomic_uint_fast64_t last_latency_update_ms;

    /* Per-ODID statistics */
    struct odid_hash_table odid_table;
    
    /* thread control */
    atomic_int stop_threads;
};

/* Hash function for ODID */
static inline uint32_t
odid_hash(uint32_t odid)
{
    return odid % ODID_HASH_SIZE;
}

/* Get or create per-ODID statistics (thread-safe) */
static struct odid_stats *
get_or_create_odid_stats(struct instance_data *inst, uint32_t odid)
{
    uint32_t hash = odid_hash(odid);
    struct odid_stats *stats;
    
    pthread_mutex_lock(&inst->odid_table.lock);
    
    /* Search for existing ODID stats */
    for (stats = inst->odid_table.buckets[hash]; stats != NULL; stats = stats->next) {
        if (stats->odid == odid) {
            pthread_mutex_unlock(&inst->odid_table.lock);
            return stats;
        }
    }
    
    /* Create new ODID stats */
    stats = calloc(1, sizeof(*stats));
    if (!stats) {
        pthread_mutex_unlock(&inst->odid_table.lock);
        return NULL;
    }
    
    stats->odid = odid;
    atomic_init(&stats->cnt_flows_data, 0);
    atomic_init(&stats->cnt_flows_opts, 0);
    atomic_init(&stats->cnt_bytes, 0);
    atomic_init(&stats->cnt_pkts, 0);
    atomic_init(&stats->rps, 0);
    atomic_init(&stats->total_latency_ms, 0);
    atomic_init(&stats->latency_count, 0);
    atomic_init(&stats->last_latency_update_ms, 0);
    
    memset(stats->rps_history, 0, sizeof(stats->rps_history));
    stats->rps_history_index = 0;
    stats->rps_history_sum = 0;
    stats->rps_history_count = 0;
    stats->last_snapshot = 0;
    
    /* Insert at head of bucket */
    stats->next = inst->odid_table.buckets[hash];
    inst->odid_table.buckets[hash] = stats;
    
    pthread_mutex_unlock(&inst->odid_table.lock);
    return stats;
}

/* Inline helper to increment counters (relaxed ordering for max perf) */
static inline void
atomic_add_u64(atomic_uint_fast64_t *a, uint64_t v)
{
    atomic_fetch_add_explicit(a, (uint_fast64_t)v, memory_order_relaxed);
}

/* Update statistics about flow records (hot path) */
static void
stats_update(struct instance_data *inst, ipx_msg_ipfix_t *msg, uint32_t odid)
{
    uint32_t rec_cnt = ipx_msg_ipfix_get_drec_cnt(msg);
    struct odid_stats *odid_stats = get_or_create_odid_stats(inst, odid);

    for (uint32_t i = 0; i < rec_cnt; ++i) {
        struct ipx_ipfix_record *rec_ptr = ipx_msg_ipfix_get_drec(msg, i);
        const struct fds_template *tmplt = rec_ptr->rec.tmplt;
        const enum fds_template_type ttype = tmplt->type;

        struct fds_drec_field field;
        uint64_t value;

        /* Update both global and per-ODID counters */
        if (ttype == FDS_TYPE_TEMPLATE) {
            atomic_add_u64(&inst->cnt_flows_data, 1);
            if (odid_stats) atomic_add_u64(&odid_stats->cnt_flows_data, 1);
        } else if (ttype == FDS_TYPE_TEMPLATE_OPTS) {
            atomic_add_u64(&inst->cnt_flows_opts, 1);
            if (odid_stats) atomic_add_u64(&odid_stats->cnt_flows_opts, 1);
            continue; /* options records don't have bytes/packets counters */
        }

        /* octetDeltaCount */
        if (fds_drec_find(&rec_ptr->rec, IANA_PEN, IE_ID_BYTES, &field) != FDS_EOC
            && fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
            atomic_add_u64(&inst->cnt_bytes, value);
            if (odid_stats) atomic_add_u64(&odid_stats->cnt_bytes, value);
        }

        /* packetDeltaCount */
        if (fds_drec_find(&rec_ptr->rec, IANA_PEN, IE_ID_PKTS, &field) != FDS_EOC
            && fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
            atomic_add_u64(&inst->cnt_pkts, value);
            if (odid_stats) atomic_add_u64(&odid_stats->cnt_pkts, value);
        }

        /* Latency calculation */
        if (fds_drec_find(&rec_ptr->rec, IANA_PEN, IE_ID_FLOW_END, &field) != FDS_EOC &&
            fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {

            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            uint64_t now_ms;
            if (now.tv_sec > UINT64_MAX / 1000) {
                now_ms = UINT64_MAX;
            } else {
                now_ms = ((uint64_t)now.tv_sec * 1000) + (now.tv_nsec / 1000000);
            }

            if (now_ms >= value) {
                uint64_t latency = now_ms - value;
                atomic_add_u64(&inst->total_latency_ms, latency);
                atomic_add_u64(&inst->latency_count, 1);
                atomic_store_explicit(&inst->last_latency_update_ms, now_ms, memory_order_relaxed);
                
                if (odid_stats) {
                    atomic_add_u64(&odid_stats->total_latency_ms, latency);
                    atomic_add_u64(&odid_stats->latency_count, 1);
                    atomic_store_explicit(&odid_stats->last_latency_update_ms, now_ms, memory_order_relaxed);
                }
            }
        }

        /* Biflow (reverse) */
        if ((tmplt->flags & FDS_TEMPLATE_BIFLOW) != 0) {
            if (fds_drec_find(&rec_ptr->rec, IANA_PEN_REV, IE_ID_BYTES, &field) != FDS_EOC
                && fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
                atomic_add_u64(&inst->cnt_bytes, value);
                if (odid_stats) atomic_add_u64(&odid_stats->cnt_bytes, value);
            }

            if (fds_drec_find(&rec_ptr->rec, IANA_PEN_REV, IE_ID_PKTS, &field) != FDS_EOC
                && fds_get_uint_be(field.data, field.size, &value) == FDS_OK) {
                atomic_add_u64(&inst->cnt_pkts, value);
                if (odid_stats) atomic_add_u64(&odid_stats->cnt_pkts, value);
            }
        }
    }
}

/* Helper function to calculate average latency with timeout check */
static double
get_avg_latency(uint64_t total_latency, uint64_t latency_count, uint64_t last_update)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    uint64_t now_ms;
    if (now.tv_sec > UINT64_MAX / 1000) {
        now_ms = UINT64_MAX;
    } else {
        now_ms = ((uint64_t)now.tv_sec * 1000) + (now.tv_nsec / 1000000);
    }
    
    if (latency_count > 0 && (now_ms - last_update) <= LATENCY_TIMEOUT_MS) {
        return (double)total_latency / latency_count;
    }
    return 0.0;
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

    /* Load global atomics */
    uint64_t cnt_data = (uint64_t)atomic_load_explicit(&inst->cnt_flows_data, memory_order_relaxed);
    uint64_t cnt_opts = (uint64_t)atomic_load_explicit(&inst->cnt_flows_opts, memory_order_relaxed);
    uint64_t cnt_bytes = (uint64_t)atomic_load_explicit(&inst->cnt_bytes, memory_order_relaxed);
    uint64_t cnt_pkts = (uint64_t)atomic_load_explicit(&inst->cnt_pkts, memory_order_relaxed);
    uint64_t rps = (uint64_t)atomic_load_explicit(&inst->rps, memory_order_relaxed);
    uint64_t total_latency = atomic_load_explicit(&inst->total_latency_ms, memory_order_relaxed);
    uint64_t latency_count = atomic_load_explicit(&inst->latency_count, memory_order_relaxed);
    uint64_t last_update = atomic_load_explicit(&inst->last_latency_update_ms, memory_order_relaxed);
    
    double avg_latency = get_avg_latency(total_latency, latency_count, last_update);

    char *buf = malloc(METRICS_BUF_LEN);
    if (!buf) {
        return MHD_NO;
    }

    int len = snprintf(buf, METRICS_BUF_LEN,
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
        "# HELP dummy_records_per_second Rolling average records per second (global, 10-second window)\n"
        "# TYPE dummy_records_per_second gauge\n"
        "dummy_records_per_second %" PRIu64 "\n"
        "# HELP dummy_ingest_latency_ms Average latency from flow end to processing (global, ms)\n"
        "# TYPE dummy_ingest_latency_ms gauge\n"
        "dummy_ingest_latency_ms %.3f\n",
        cnt_data, cnt_opts, cnt_bytes, cnt_pkts, rps, avg_latency);

    if (len < 0 || len >= METRICS_BUF_LEN) {
        free(buf);
        return MHD_NO;
    }

    /* Add per-ODID metrics */
    pthread_mutex_lock(&inst->odid_table.lock);
    
    int offset = len;
    offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
        "# HELP dummy_data_records_total_by_odid Data records per ODID\n"
        "# TYPE dummy_data_records_total_by_odid counter\n");
    
    for (int i = 0; i < ODID_HASH_SIZE && offset < METRICS_BUF_LEN - 1024; i++) {
        for (struct odid_stats *stats = inst->odid_table.buckets[i]; 
             stats != NULL && offset < METRICS_BUF_LEN - 1024; 
             stats = stats->next) {
            
            uint64_t odid_data = atomic_load_explicit(&stats->cnt_flows_data, memory_order_relaxed);
            offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
                "dummy_data_records_total_by_odid{odid=\"%" PRIu32 "\"} %" PRIu64 "\n",
                stats->odid, odid_data);
        }
    }
    
    offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
        "# HELP dummy_options_records_total_by_odid Options records per ODID\n"
        "# TYPE dummy_options_records_total_by_odid counter\n");
    
    for (int i = 0; i < ODID_HASH_SIZE && offset < METRICS_BUF_LEN - 1024; i++) {
        for (struct odid_stats *stats = inst->odid_table.buckets[i]; 
             stats != NULL && offset < METRICS_BUF_LEN - 1024; 
             stats = stats->next) {
            
            uint64_t odid_opts = atomic_load_explicit(&stats->cnt_flows_opts, memory_order_relaxed);
            offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
                "dummy_options_records_total_by_odid{odid=\"%" PRIu32 "\"} %" PRIu64 "\n",
                stats->odid, odid_opts);
        }
    }
    
    offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
        "# HELP dummy_bytes_total_by_odid Bytes per ODID\n"
        "# TYPE dummy_bytes_total_by_odid counter\n");
    
    for (int i = 0; i < ODID_HASH_SIZE && offset < METRICS_BUF_LEN - 1024; i++) {
        for (struct odid_stats *stats = inst->odid_table.buckets[i]; 
             stats != NULL && offset < METRICS_BUF_LEN - 1024; 
             stats = stats->next) {
            
            uint64_t odid_bytes = atomic_load_explicit(&stats->cnt_bytes, memory_order_relaxed);
            offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
                "dummy_bytes_total_by_odid{odid=\"%" PRIu32 "\"} %" PRIu64 "\n",
                stats->odid, odid_bytes);
        }
    }
    
    offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
        "# HELP dummy_packets_total_by_odid Packets per ODID\n"
        "# TYPE dummy_packets_total_by_odid counter\n");
    
    for (int i = 0; i < ODID_HASH_SIZE && offset < METRICS_BUF_LEN - 1024; i++) {
        for (struct odid_stats *stats = inst->odid_table.buckets[i]; 
             stats != NULL && offset < METRICS_BUF_LEN - 1024; 
             stats = stats->next) {
            
            uint64_t odid_pkts = atomic_load_explicit(&stats->cnt_pkts, memory_order_relaxed);
            offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
                "dummy_packets_total_by_odid{odid=\"%" PRIu32 "\"} %" PRIu64 "\n",
                stats->odid, odid_pkts);
        }
    }
    
    offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
        "# HELP dummy_records_per_second_by_odid RPS per ODID (10-second window)\n"
        "# TYPE dummy_records_per_second_by_odid gauge\n");
    
    for (int i = 0; i < ODID_HASH_SIZE && offset < METRICS_BUF_LEN - 1024; i++) {
        for (struct odid_stats *stats = inst->odid_table.buckets[i]; 
             stats != NULL && offset < METRICS_BUF_LEN - 1024; 
             stats = stats->next) {
            
            uint64_t odid_rps = atomic_load_explicit(&stats->rps, memory_order_relaxed);
            offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
                "dummy_records_per_second_by_odid{odid=\"%" PRIu32 "\"} %" PRIu64 "\n",
                stats->odid, odid_rps);
        }
    }
    
    offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
        "# HELP dummy_ingest_latency_ms_by_odid Latency per ODID (ms)\n"
        "# TYPE dummy_ingest_latency_ms_by_odid gauge\n");
    
    for (int i = 0; i < ODID_HASH_SIZE && offset < METRICS_BUF_LEN - 1024; i++) {
        for (struct odid_stats *stats = inst->odid_table.buckets[i]; 
             stats != NULL && offset < METRICS_BUF_LEN - 1024; 
             stats = stats->next) {
            
            uint64_t odid_total_latency = atomic_load_explicit(&stats->total_latency_ms, memory_order_relaxed);
            uint64_t odid_latency_count = atomic_load_explicit(&stats->latency_count, memory_order_relaxed);
            uint64_t odid_last_update = atomic_load_explicit(&stats->last_latency_update_ms, memory_order_relaxed);
            
            double odid_avg_latency = get_avg_latency(odid_total_latency, odid_latency_count, odid_last_update);
            offset += snprintf(buf + offset, METRICS_BUF_LEN - offset,
                "dummy_ingest_latency_ms_by_odid{odid=\"%" PRIu32 "\"} %.3f\n",
                stats->odid, odid_avg_latency);
        }
    }
    
    pthread_mutex_unlock(&inst->odid_table.lock);

    struct MHD_Response *resp = MHD_create_response_from_buffer((size_t)offset, (void *)buf, MHD_RESPMEM_MUST_FREE);
    if (!resp) {
        free(buf);
        return MHD_NO;
    }
    MHD_add_response_header(resp, "Content-Type", "text/plain; version=0.0.4");
    int rc = MHD_queue_response(connection, MHD_HTTP_OK, resp);
    MHD_destroy_response(resp);
    return rc;
}

/* Update RPS for a specific ODID stats structure */
static void
update_odid_rps(struct odid_stats *stats)
{
    uint64_t now = (uint64_t)atomic_load_explicit(&stats->cnt_flows_data, memory_order_relaxed);
    uint64_t last = stats->last_snapshot;
    
    uint64_t delta = 0;
    if (now >= last) {
        delta = now - last;
    }
    
    stats->last_snapshot = now;
    
    if (stats->rps_history_count >= 10) {
        stats->rps_history_sum -= stats->rps_history[stats->rps_history_index];
    } else {
        stats->rps_history_count++;
    }
    
    stats->rps_history[stats->rps_history_index] = delta;
    stats->rps_history_sum += delta;
    stats->rps_history_index = (stats->rps_history_index + 1) % 10;
    
    uint64_t avg_rps = stats->rps_history_count > 0 ? stats->rps_history_sum / stats->rps_history_count : 0;
    atomic_store_explicit(&stats->rps, (uint_fast64_t)avg_rps, memory_order_relaxed);
}

/* Metrics thread: computes rps each second */
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
        
        /* Update global RPS */
        uint64_t now = (uint64_t)atomic_load_explicit(&inst->cnt_flows_data, memory_order_relaxed);
        uint64_t last = inst->last_snapshot;
        
        uint64_t delta = 0;
        if (now >= last) {
            delta = now - last;
        }
        
        inst->last_snapshot = now;
        
        if (inst->rps_history_count >= 10) {
            inst->rps_history_sum -= inst->rps_history[inst->rps_history_index];
        } else {
            inst->rps_history_count++;
        }
        
        inst->rps_history[inst->rps_history_index] = delta;
        inst->rps_history_sum += delta;
        inst->rps_history_index = (inst->rps_history_index + 1) % 10;
        
        uint64_t avg_rps = inst->rps_history_count > 0 ? inst->rps_history_sum / inst->rps_history_count : 0;
        atomic_store_explicit(&inst->rps, (uint_fast64_t)avg_rps, memory_order_relaxed);
        
        /* Update per-ODID RPS */
        pthread_mutex_lock(&inst->odid_table.lock);
        for (int i = 0; i < ODID_HASH_SIZE; i++) {
            for (struct odid_stats *stats = inst->odid_table.buckets[i]; stats != NULL; stats = stats->next) {
                update_odid_rps(stats);
            }
        }
        pthread_mutex_unlock(&inst->odid_table.lock);
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

    /* initialize global atomics */
    atomic_init(&data->cnt_flows_data, 0);
    atomic_init(&data->cnt_flows_opts, 0);
    atomic_init(&data->cnt_bytes, 0);
    atomic_init(&data->cnt_pkts, 0);
    atomic_init(&data->rps, 0);
    atomic_init(&data->stop_threads, 0);
    atomic_init(&data->total_latency_ms, 0);
    atomic_init(&data->latency_count, 0);
    atomic_init(&data->last_latency_update_ms, 0);
    
    /* initialize rolling average RPS fields */
    memset(data->rps_history, 0, sizeof(data->rps_history));
    data->rps_history_index = 0;
    data->rps_history_sum = 0;
    data->rps_history_count = 0;
    data->last_snapshot = 0;
    
    /* initialize per-ODID hash table */
    memset(&data->odid_table, 0, sizeof(data->odid_table));
    pthread_mutex_init(&data->odid_table.lock, NULL);

    if ((data->config = config_parse(ctx, params)) == NULL) {
        pthread_mutex_destroy(&data->odid_table.lock);
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
        pthread_mutex_destroy(&data->odid_table.lock);
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
    sleep(1);

    /* Print final stats if enabled */
    if (data->config && data->config->en_stats) {
        uint64_t cnt_data = atomic_load_explicit(&data->cnt_flows_data, memory_order_relaxed);
        uint64_t cnt_opts = atomic_load_explicit(&data->cnt_flows_opts, memory_order_relaxed);
        uint64_t cnt_bytes = atomic_load_explicit(&data->cnt_bytes, memory_order_relaxed);
        uint64_t cnt_pkts = atomic_load_explicit(&data->cnt_pkts, memory_order_relaxed);
        uint64_t total_latency = atomic_load_explicit(&data->total_latency_ms, memory_order_relaxed);
        uint64_t latency_count = atomic_load_explicit(&data->latency_count, memory_order_relaxed);
        uint64_t last_update = atomic_load_explicit(&data->last_latency_update_ms, memory_order_relaxed);
        
        fprintf(stdout, "\n=== Global Final Stats ===\n");
        fprintf(stdout, "- data records:    %" PRIu64 "\n", cnt_data);
        fprintf(stdout, "- options records: %" PRIu64 "\n", cnt_opts);
        fprintf(stdout, "- total bytes:     %" PRIu64 "\n", cnt_bytes);
        fprintf(stdout, "- total packets:   %" PRIu64 "\n", cnt_pkts);
        
        double avg_latency = get_avg_latency(total_latency, latency_count, last_update);
        if (avg_latency > 0) {
            fprintf(stdout, "- avg ingest latency: %.3f ms\n", avg_latency);
        }
        
        /* Print per-ODID stats */
        fprintf(stdout, "\n=== Per-ODID Final Stats ===\n");
        pthread_mutex_lock(&data->odid_table.lock);
        for (int i = 0; i < ODID_HASH_SIZE; i++) {
            for (struct odid_stats *stats = data->odid_table.buckets[i]; stats != NULL; stats = stats->next) {
                uint64_t odid_data = atomic_load_explicit(&stats->cnt_flows_data, memory_order_relaxed);
                uint64_t odid_opts = atomic_load_explicit(&stats->cnt_flows_opts, memory_order_relaxed);
                uint64_t odid_bytes = atomic_load_explicit(&stats->cnt_bytes, memory_order_relaxed);
                uint64_t odid_pkts = atomic_load_explicit(&stats->cnt_pkts, memory_order_relaxed);
                uint64_t odid_total_latency = atomic_load_explicit(&stats->total_latency_ms, memory_order_relaxed);
                uint64_t odid_latency_count = atomic_load_explicit(&stats->latency_count, memory_order_relaxed);
                uint64_t odid_last_update = atomic_load_explicit(&stats->last_latency_update_ms, memory_order_relaxed);
                
                fprintf(stdout, "\nODID %" PRIu32 ":\n", stats->odid);
                fprintf(stdout, "  - data records:    %" PRIu64 "\n", odid_data);
                fprintf(stdout, "  - options records: %" PRIu64 "\n", odid_opts);
                fprintf(stdout, "  - total bytes:     %" PRIu64 "\n", odid_bytes);
                fprintf(stdout, "  - total packets:   %" PRIu64 "\n", odid_pkts);
                
                double odid_avg_latency = get_avg_latency(odid_total_latency, odid_latency_count, odid_last_update);
                if (odid_avg_latency > 0) {
                    fprintf(stdout, "  - avg ingest latency: %.3f ms\n", odid_avg_latency);
                }
            }
        }
        pthread_mutex_unlock(&data->odid_table.lock);
        
        fflush(stdout);
    }

    /* Free per-ODID stats */
    for (int i = 0; i < ODID_HASH_SIZE; i++) {
        struct odid_stats *stats = data->odid_table.buckets[i];
        while (stats != NULL) {
            struct odid_stats *next = stats->next;
            free(stats);
            stats = next;
        }
    }
    pthread_mutex_destroy(&data->odid_table.lock);

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
        uint32_t odid = ipfix_ctx->odid;
        
        IPX_CTX_INFO(ctx, "[ODID: %" PRIu32 "] Received an IPFIX message", odid);

        if (data->config && data->config->en_stats) {
            stats_update(data, ipfix_msg, odid);
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
