/*
 * everarch - the hopefully ever lasting archive
 * Copyright (C) 2021-2022  Markus Peröbner
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <string.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>
#include <threads.h>
#include <libxslt/xslt.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include "basics.h"
#include "claims.h"
#include "errors.h"
#include "logger.h"
#include "evr-glacier-client.h"
#include "signatures.h"
#include "server.h"
#include "attr-index-db.h"
#include "configurations.h"
#include "files.h"
#include "configp.h"

const char *argp_program_version = "evr-attr-index " VERSION;
const char *argp_program_bug_address = PACKAGE_BUGREPORT;

static char doc[] = "evr-attr-index provides an index over a evr-glacier-storage server.";

static char args_doc[] = "";

#define default_host "localhost"

#define arg_host 256
#define arg_storage_host 257
#define arg_storage_port 258

static struct argp_option options[] = {
    {"state-dir-path", 'd', "DIR", 0, "State directory path. This is the place where the index is persisted."},
    {"host", arg_host, "HOST", 0, "The network interface at which the attr index server will listen on. The default is " default_host "."},
    {"port", 'p', "PORT", 0, "The tcp port at which the attr index server will listen. The default port is " to_string(evr_glacier_attr_index_port) "."},
    {"storage-host", arg_storage_host, "HOST", 0, "The hostname of the evr-glacier-storage server to connect to. Default hostname is " evr_glacier_storage_host "."},
    {"storage-port", arg_storage_port, "PORT", 0, "The port of the evr-glalier-storage server to connect to. Default port is " to_string(evr_glacier_storage_port) "."},
    {0},
};

static error_t parse_opt(int key, char *arg, struct argp_state *state, void (*usage)(const struct argp_state *state)){
    struct evr_attr_index_cfg *cfg = (struct evr_attr_index_cfg*)state->input;
    switch(key){
    default:
        return ARGP_ERR_UNKNOWN;
    case 'd':
        evr_replace_str(cfg->state_dir_path, arg);
        break;
    case arg_host:
        evr_replace_str(cfg->host, arg);
        break;
    case 'p':
        evr_replace_str(cfg->port, arg);
        break;
    case arg_storage_host:
        evr_replace_str(cfg->storage_host, arg);
        break;
    case arg_storage_port:
        evr_replace_str(cfg->storage_port, arg);
        break;
    }
    return 0;
}

static error_t parse_opt_adapter(int key, char *arg, struct argp_state *state){
    return parse_opt(key, arg, state, argp_usage);
}

sig_atomic_t running = 1;
mtx_t stop_lock;
cnd_t stop_signal;

struct evr_connection {
    int socket;
};

/**
 * watch_overlap defines the overlap of claim watches in seconds.
 */
#define watch_overlap (10 * 60)
#define apply_watch_overlap(t) (t <= watch_overlap ? 0 : t - watch_overlap)

struct evr_attr_index_cfg *cfg;

struct evr_handover_ctx {
    int occupied;
    mtx_t lock;
    cnd_t on_push_spec;
    cnd_t on_empty_spec;
};

struct evr_attr_spec_handover_ctx {
    struct evr_handover_ctx handover;

    struct evr_attr_spec_claim *claim;
    evr_blob_ref claim_key;
    evr_time created;
};

struct evr_index_handover_ctx {
    struct evr_handover_ctx handover;

    evr_blob_ref index_ref;
};

struct evr_current_index_ctx {
    struct evr_handover_ctx handover;
    evr_blob_ref index_ref;
};

struct evr_current_index_ctx current_index_ctx;

struct evr_search_ctx {
    struct evr_connection *con;
    int parse_res;
};

void evr_load_attr_index_cfg(int argc, char **argv);

void handle_sigterm(int signum);
#define evr_init_attr_spec_handover_ctx(ctx) evr_init_handover_ctx(&(ctx)->handover)
int evr_free_attr_spec_handover_ctx(struct evr_attr_spec_handover_ctx *ctx);
#define evr_init_index_handover_ctx(ctx) evr_init_handover_ctx(&(ctx)->handover)
#define evr_free_index_handover_ctx(ctx) evr_free_handover_ctx(&(ctx)->handover)
#define evr_init_current_index_ctx(ctx) evr_init_handover_ctx(&(ctx)->handover)
#define evr_free_current_index_ctx(ctx) evr_free_handover_ctx(&(ctx)->handover)

int evr_init_handover_ctx(struct evr_handover_ctx *ctx);
int evr_free_handover_ctx(struct evr_handover_ctx *ctx);
int evr_stop_handover(struct evr_handover_ctx *ctx);
int evr_wait_for_handover_available(struct evr_handover_ctx *ctx);
int evr_wait_for_handover_occupied(struct evr_handover_ctx *ctx);
int evr_lock_handover(struct evr_handover_ctx *ctx);
int evr_unlock_handover(struct evr_handover_ctx *ctx);
int evr_occupy_handover(struct evr_handover_ctx *ctx);
int evr_empty_handover(struct evr_handover_ctx *ctx);

int evr_watch_index_claims_worker(void *arg);
int evr_build_index_worker(void *arg);
int evr_index_sync_worker(void *arg);
int evr_bootstrap_db(evr_blob_ref claim_key, struct evr_attr_spec_claim *spec);
int evr_index_claim_set(struct evr_attr_index_db *db, struct evr_attr_spec_claim *spec, xsltStylesheetPtr stylesheet, evr_blob_ref claim_set_ref, evr_time claim_set_last_modified, int *c);
int evr_attr_index_tcp_server();
int evr_connection_worker(void *ctx);
int evr_work_cmd(struct evr_connection *ctx, char *line);
int evr_work_search_cmd(struct evr_connection *ctx, char *query);
int evr_respond_search_status(void *context, int parse_res, char *parse_errer);
int evr_respond_search_result(void *context, const evr_claim_ref ref, struct evr_attr_tuple *attrs, size_t attrs_len);
int evr_list_claims_for_seed(struct evr_connection *ctx, char *seed_ref_str);
int evr_get_current_index_ref(evr_blob_ref index_ref);
int evr_respond_help(struct evr_connection *ctx);
int evr_respond_status(struct evr_connection *ctx, int ok, char *msg);
int evr_respond_message_end(struct evr_connection *ctx);
int evr_write_blob_to_file(void *ctx, char *path, mode_t mode, evr_blob_ref ref);

int main(int argc, char **argv){
    int ret = evr_error;
    evr_load_attr_index_cfg(argc, argv);
    if(mtx_init(&stop_lock, mtx_plain) != thrd_success){
        goto out_with_free_cfg;
    }
    if(cnd_init(&stop_signal) != thrd_success){
        goto out_with_free_stop_lock;
    }
    if(evr_init_current_index_ctx(&current_index_ctx) != evr_ok){
        goto out_with_free_stop_signal;
    }
    {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = handle_sigterm;
        sigaction(SIGINT, &action, NULL);
        signal(SIGPIPE, SIG_IGN);
    }
    if(sqlite3_config(SQLITE_CONFIG_MULTITHREAD) != SQLITE_OK){
        // read https://sqlite.org/threadsafe.html if you run into
        // this error
        log_error("Failed to configure multi-threaded mode for sqlite3");
        goto out_with_free_current_index;
    }
    evr_init_signatures();
    xmlInitParser();
    struct evr_attr_spec_handover_ctx attr_spec_handover_ctx;
    if(evr_init_attr_spec_handover_ctx(&attr_spec_handover_ctx) != evr_ok){
        goto out_with_cleanup_xml_parser;
    }
    struct evr_index_handover_ctx index_handover_ctx;
    if(evr_init_index_handover_ctx(&index_handover_ctx) != evr_ok){
        goto out_with_free_attr_spec_handover_ctx;
    }
    thrd_t watch_index_claims_thrd;
    if(thrd_create(&watch_index_claims_thrd, evr_watch_index_claims_worker, &attr_spec_handover_ctx) != thrd_success){
        goto out_with_free_index_handover_ctx;
    }
    thrd_t build_index_thrd;
    void *build_index_thrd_ctx[] = {
        &attr_spec_handover_ctx,
        &index_handover_ctx,
    };
    if(thrd_create(&build_index_thrd, evr_build_index_worker, &build_index_thrd_ctx) != thrd_success){
        goto out_with_join_watch_index_claims_thrd;
    }
    thrd_t index_sync_thrd;
    if(thrd_create(&index_sync_thrd, evr_index_sync_worker, &index_handover_ctx) != thrd_success){
        goto out_with_join_build_index_thrd;
    }
    thrd_t tcp_server_thrd;
    if(thrd_create(&tcp_server_thrd, evr_attr_index_tcp_server, &index_handover_ctx) != thrd_success){
        goto out_with_join_index_sync_thrd;
    }
    if(mtx_lock(&stop_lock) != thrd_success){
        evr_panic("Failed to lock stop lock");
        goto out_with_join_watch_index_claims_thrd;
    }
    while(running){
        if(cnd_wait(&stop_signal, &stop_lock) != thrd_success){
            evr_panic("Failed to wait for stop signal");
            goto out_with_join_watch_index_claims_thrd;
        }
    }
    if(mtx_unlock(&stop_lock) != thrd_success){
        evr_panic("Failed to unlock stop lock");
        goto out_with_join_watch_index_claims_thrd;
    }
    if(evr_stop_handover(&index_handover_ctx.handover) != evr_ok){
        goto out_with_join_watch_index_claims_thrd;
    }
    if(evr_stop_handover(&attr_spec_handover_ctx.handover) != evr_ok){
        goto out_with_join_watch_index_claims_thrd;
    }
    if(evr_stop_handover(&current_index_ctx.handover) != evr_ok){
        goto out_with_join_watch_index_claims_thrd;
    }
    ret = evr_ok;
    int thrd_res;
 out_with_join_index_sync_thrd:
    if(thrd_join(index_sync_thrd, &thrd_res) != thrd_success){
        evr_panic("Failed to join index sync thread");
        ret = evr_error;
    }
    if(thrd_res != evr_ok){
        ret = evr_error;
    }
 out_with_join_build_index_thrd:
    if(thrd_join(build_index_thrd, &thrd_res) != thrd_success){
        evr_panic("Failed to join build index thread");
        ret = evr_error;
    }
    if(thrd_res != evr_ok){
        ret = evr_error;
    }
 out_with_join_watch_index_claims_thrd:
    if(thrd_join(watch_index_claims_thrd, &thrd_res) != thrd_success){
        evr_panic("Failed to join watch index claims thread");
        ret = evr_error;
    }
    if(thrd_res != evr_ok){
        ret = evr_error;
    }
 out_with_free_index_handover_ctx:
    if(evr_free_index_handover_ctx(&index_handover_ctx) != evr_ok){
        evr_panic("Failed to free index handover context");
        ret = evr_error;
    }
 out_with_free_attr_spec_handover_ctx:
    if(evr_free_attr_spec_handover_ctx(&attr_spec_handover_ctx) != evr_ok){
        evr_panic("Failed to free attr-spec handover context");
        ret = evr_error;
    }
 out_with_cleanup_xml_parser:
    xsltCleanupGlobals();
    xmlCleanupParser();
 out_with_free_current_index:
    evr_free_current_index_ctx(&current_index_ctx);
 out_with_free_stop_signal:
    cnd_destroy(&stop_signal);
 out_with_free_stop_lock:
    mtx_destroy(&stop_lock);
 out_with_free_cfg:
    evr_free_attr_index_cfg(cfg);
    return ret;
}

void evr_load_attr_index_cfg(int argc, char **argv){
    cfg = malloc(sizeof(struct evr_attr_index_cfg));
    if(!cfg){
        evr_panic("Unable to allocate memory for configuration.");
        return;
    }
    cfg->state_dir_path = strdup(EVR_PREFIX "/var/everarch/attr-index");
    cfg->host = strdup(default_host);
    cfg->port = strdup(to_string(evr_glacier_attr_index_port));
    cfg->storage_host = strdup(evr_glacier_storage_host);
    cfg->storage_port = strdup(to_string(evr_glacier_storage_port));
    if(!cfg->state_dir_path || !cfg->host || !cfg->port || !cfg->storage_host || !cfg->storage_port){
        evr_panic("Unable to allocate memory for configuration.");
    }
    struct configp configp = {
        options, parse_opt, args_doc, doc
    };
    char *config_paths[] = {
        "attr-index.conf",
        "~/.config/everarch/attr-index.conf",
        "/etc/everarch/attr-index.conf",
        NULL,
    };
    if(configp_parse(&configp, config_paths, cfg) != 0){
        evr_panic("Unable to parse config files");
        return;
    }
    struct argp argp = { options, parse_opt_adapter, args_doc, doc };
    argp_parse(&argp, argc, argv, 0, 0, cfg);
    evr_single_expand_property(cfg->state_dir_path, panic);
    return;
 panic:
    evr_panic("Unable to expand configuration values");
}

void handle_sigterm(int signum){
    if(mtx_lock(&stop_lock) != thrd_success){
        evr_panic("Failed to lock stop lock");
        return;
    }
    if(running){
        log_info("Shutting down");
        running = 0;
        if(cnd_signal(&stop_signal) != thrd_success){
            evr_panic("Failed to send stop signal");
            return;
        }
    }
    if(mtx_unlock(&stop_lock) != thrd_success){
        evr_panic("Failed to unlock stop lock");
        return;
    }
}

int evr_free_attr_spec_handover_ctx(struct evr_attr_spec_handover_ctx *ctx){
    int ret = evr_error;
    if(ctx->claim){
        free(ctx->claim);
    }
    if(evr_free_handover_ctx(&ctx->handover) != evr_ok){
        goto out;
    }
    ret = evr_ok;
 out:
    return ret;
}

int evr_init_handover_ctx(struct evr_handover_ctx *ctx){
    int ret = evr_error;
    ctx->occupied = 0;
    if(mtx_init(&ctx->lock, mtx_plain) != thrd_success){
        goto out;
    }
    if(cnd_init(&ctx->on_push_spec) != thrd_success){
        goto out_with_free_lock;
    }
    if(cnd_init(&ctx->on_empty_spec) != thrd_success){
        goto out_with_free_on_push_spec;
    }
    ret = evr_ok;
 out:
    return ret;
 out_with_free_on_push_spec:
    cnd_destroy(&ctx->on_push_spec);
 out_with_free_lock:
    mtx_destroy(&ctx->lock);
    return ret;
}

int evr_free_handover_ctx(struct evr_handover_ctx *ctx){
    cnd_destroy(&ctx->on_empty_spec);
    cnd_destroy(&ctx->on_push_spec);
    mtx_destroy(&ctx->lock);
    return evr_ok;
}

int evr_stop_handover(struct evr_handover_ctx *ctx){
    int ret = evr_error;
    if(cnd_signal(&ctx->on_push_spec) != thrd_success){
        evr_panic("Failed to signal on_push on termination");
        goto out;
    }
    if(cnd_signal(&ctx->on_empty_spec) != thrd_success){
        evr_panic("Failed to signal on_empty on termination");
        goto out;
    }
    ret = evr_ok;
 out:
    return ret;
}

int evr_wait_for_handover_available(struct evr_handover_ctx *ctx){
    int ret = evr_error;
    if(evr_lock_handover(ctx) != evr_ok){
        evr_panic("Failed to lock handover lock");
        goto out;
    }
    while(ctx->occupied){
        if(!running){
            if(mtx_unlock(&ctx->lock) != thrd_success){
                evr_panic("Failed to unlock handover lock");
                goto out;
            }
            break;
        }
        if(cnd_wait(&ctx->on_empty_spec, &ctx->lock) != thrd_success){
            evr_panic("Failed to wait for empty handover signal");
            goto out;
        }
    }
    ret = evr_ok;
 out:
    return ret;
}

int evr_wait_for_handover_occupied(struct evr_handover_ctx *ctx){
    int ret = evr_error;
    if(evr_lock_handover(ctx) != evr_ok){
        evr_panic("Failed to lock handover lock");
        goto out;
    }
    while(!ctx->occupied){
        if(!running){
            if(mtx_unlock(&ctx->lock) != thrd_success){
                evr_panic("Failed to unlock handover lock");
                goto out;
            }
            break;
        }
        if(cnd_wait(&ctx->on_push_spec, &ctx->lock) != thrd_success){
            evr_panic("Failed to wait for handover push");
            goto out;
        }
    }
    ret = evr_ok;
 out:
    return ret;
}

int evr_lock_handover(struct evr_handover_ctx *ctx){
    int ret = evr_error;
    if(mtx_lock(&ctx->lock) != thrd_success){
        goto out;
    }
    ret = evr_ok;
 out:
    return ret;
}

int evr_unlock_handover(struct evr_handover_ctx *ctx){
    int ret = evr_error;
    if(mtx_unlock(&ctx->lock) != thrd_success){
        evr_panic("Failed to unlock handover");
        goto out;
    }
    ret = evr_ok;
 out:
    return ret;
}

int evr_occupy_handover(struct evr_handover_ctx *ctx){
    int ret = evr_error;
    ctx->occupied = 1;
    if(cnd_signal(&ctx->on_push_spec) != thrd_success){
        evr_panic("Failed to signal spec pushed on occupy");
        goto out;
    }
    if(evr_unlock_handover(ctx) != evr_ok){
        goto out;
    }
    ret = evr_ok;
 out:
    return ret;
}

int evr_empty_handover(struct evr_handover_ctx *ctx){
    int ret = evr_error;
    ctx->occupied = 0;
    if(cnd_signal(&ctx->on_empty_spec) != thrd_success){
        evr_panic("Failed to signal handover empty");
        goto out;
    }
    if(evr_unlock_handover(ctx) != evr_ok){
        goto out;
    }
    ret = evr_ok;
 out:
    return ret;
}

int evr_watch_index_claims_worker(void *arg){
    int ret = evr_error;
    struct evr_attr_spec_handover_ctx *ctx = arg;
    log_debug("Started watch index claims worker");
    // cw is the connection used for watching for blob changes.
    int cw = evr_connect_to_storage(cfg->storage_host, cfg->storage_port);
    if(cw < 0){
        log_error("Failed to connect to evr-glacier-storage server");
        goto out;
    }
    struct evr_blob_filter filter;
    filter.flags_filter = evr_blob_flag_index_rule_claim;
    filter.last_modified_after = 0;
    if(evr_req_cmd_watch_blobs(cw, &filter) != evr_ok){
        goto out_with_close_cw;
    }
    struct evr_watch_blobs_body body;
    struct evr_attr_spec_claim *latest_spec = NULL;
    evr_blob_ref latest_spec_key;
    evr_time latest_spec_created = 0;
    // cs is the connection used for finding the most recent
    // attr-spec claim
    int cs = -1;
    log_debug("Watching index claims");
    fd_set active_fd_set;
    struct timeval timeout;
    while(running){
        FD_ZERO(&active_fd_set);
        FD_SET(cw, &active_fd_set);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int sret = select(cw + 1, &active_fd_set, NULL, NULL, &timeout);
        if(sret < 0){
            goto out_with_close_cw;
        }
        if(!running){
            ret = evr_ok;
            goto out_with_close_cw;
        }
        if(sret == 0){
            continue;
        }
        if(evr_read_watch_blobs_body(cw, &body) != evr_ok){
            goto out_with_free_latest_spec;
        }
#ifdef EVR_LOG_INFO
        do {
            evr_blob_ref_str fmt_key;
            evr_fmt_blob_ref(fmt_key, body.key);
            log_info("Checking index claim %s for attr-spec", fmt_key);
        } while(0);
#endif
        if(cs == -1){
            cs = evr_connect_to_storage(cfg->storage_host, cfg->storage_port);
            if(cs < 0){
                log_error("Failed to connect to evr-glacier-storage server");
                goto out_with_free_latest_spec;
            }
        }
        xmlDocPtr claim_doc = evr_fetch_signed_xml(cs, body.key);
        if(!claim_doc){
            evr_blob_ref_str fmt_key;
            evr_fmt_blob_ref(fmt_key, body.key);
            log_error("Index claim not fetchable for blob key %s", fmt_key);
            goto out_with_free_latest_spec;
        }
        xmlNode *cs_node = evr_get_root_claim_set(claim_doc);
        if(!cs_node){
            evr_blob_ref_str fmt_key;
            evr_fmt_blob_ref(fmt_key, body.key);
            log_error("Index claim does not contain claim-set element for blob key %s", fmt_key);
            goto out_with_free_claim_doc;
        }
        evr_time created;
        if(evr_parse_created(&created, cs_node) != evr_ok){
            evr_blob_ref_str fmt_key;
            evr_fmt_blob_ref(fmt_key, body.key);
            log_error("Failed to parse created date from claim-set for blob key %s", fmt_key);
            goto out_with_free_claim_doc;
        }
        if(latest_spec == NULL || created > latest_spec_created){
            xmlNode *c_node = evr_find_next_element(evr_first_claim(cs_node), "attr-spec");
            if(c_node){
                if(latest_spec){
                    free(latest_spec);
                }
                latest_spec = evr_parse_attr_spec_claim(c_node);
                if(!latest_spec){
                    goto out_with_free_claim_doc;
                }
                memcpy(latest_spec_key, body.key, evr_blob_ref_size);
                latest_spec_created = created;
            }
        }
        xmlFree(claim_doc);
        if((body.flags & evr_watch_flag_eob) == 0 || !latest_spec){
            continue;
        }
        close(cs);
        cs = -1;
        if(evr_wait_for_handover_available(&ctx->handover) != evr_ok){
            goto out_with_free_latest_spec;
        }
        if(!running){
            break;
        }
        // handover ctx is available
#ifdef EVR_LOG_DEBUG
        {
            evr_blob_ref_str fmt_key;
            evr_fmt_blob_ref(fmt_key, latest_spec_key);
            log_debug("Handover latest attr-spec %s", fmt_key);
        }
#endif
        ctx->claim = latest_spec;
        memcpy(ctx->claim_key, latest_spec_key, evr_blob_ref_size);
        ctx->created = latest_spec_created;
        if(evr_occupy_handover(&ctx->handover) != evr_ok){
            goto out_with_free_latest_spec;
        }
        latest_spec = NULL;
        continue;
    out_with_free_claim_doc:
        xmlFree(claim_doc);
        goto out_with_free_latest_spec;
    }
    ret = evr_ok;
 out_with_free_latest_spec:
    if(latest_spec){
        free(latest_spec);
    }
    if(cs >= 0){
        close(cs);
    }
 out_with_close_cw:
    close(cw);
 out:
    log_debug("Ended watch index claims worker with result %d", ret);
    return ret;
}

int evr_build_index_worker(void *arg){
    int ret = evr_error;
    void **evr_build_index_worker_ctx = arg;
    struct evr_attr_spec_handover_ctx *sctx = evr_build_index_worker_ctx[0];
    struct evr_index_handover_ctx *ictx = evr_build_index_worker_ctx[1];
    log_debug("Started build index worker");
    while(running){
        if(evr_wait_for_handover_occupied(&sctx->handover) != evr_ok){
            goto out;
        }
        if(!running){
            break;
        }
        struct evr_attr_spec_claim *claim = sctx->claim;
        sctx->claim = NULL;
        evr_blob_ref claim_key;
        memcpy(claim_key, sctx->claim_key, evr_blob_ref_size);
        if(evr_empty_handover(&sctx->handover) != evr_ok){
            goto out;
        }
#ifdef EVR_LOG_INFO
        {
            evr_blob_ref_str fmt_key;
            evr_fmt_blob_ref(fmt_key, claim_key);
            log_info("Start building attr index for %s", fmt_key);
        }
#endif
        if(evr_bootstrap_db(claim_key, claim) != evr_ok){
            evr_blob_ref_str fmt_key;
            evr_fmt_blob_ref(fmt_key, claim_key);
            log_error("Failed building attr index for %s", fmt_key);
            goto out;
        }
#ifdef EVR_LOG_INFO
        {
            evr_blob_ref_str fmt_key;
            evr_fmt_blob_ref(fmt_key, claim_key);
            log_info("Finished building attr index for %s", fmt_key);
        }
#endif
        free(claim);
        if(evr_wait_for_handover_available(&ictx->handover) != evr_ok){
            goto out;
        }
        if(!running){
            break;
        }
#ifdef EVR_LOG_DEBUG
        {
            evr_blob_ref_str fmt_key;
            evr_fmt_blob_ref(fmt_key, claim_key);
            log_debug("Handover attr index for %s", fmt_key);
        }
#endif
        memcpy(ictx->index_ref, claim_key, evr_blob_ref_size);
        if(evr_occupy_handover(&ictx->handover) != evr_ok){
            goto out;
        }
    }
    ret = evr_ok;
 out:
    log_debug("Ended build index worker with result %d", ret);
    return ret;
}

int evr_bootstrap_db(evr_blob_ref claim_key, struct evr_attr_spec_claim *spec){
    int ret = evr_error;
    evr_blob_ref_str claim_key_str;
    evr_fmt_blob_ref(claim_key_str, claim_key);
    struct evr_attr_index_db *db = evr_open_attr_index_db(cfg, claim_key_str, evr_write_blob_to_file, NULL);
    if(!db){
        goto out;
    }
    if(evr_setup_attr_index_db(db, spec) != evr_ok){
        goto out_with_free_db;
    }
    if(evr_prepare_attr_index_db(db) != evr_ok){
        goto out_with_free_db;
    }
    sqlite3_int64 stage;
    if(evr_attr_index_get_state(db, evr_state_key_stage, &stage) != evr_ok){
        goto out_with_free_db;
    }
    if(stage >= evr_attr_index_stage_built){
        ret = evr_ok;
        goto out_with_free_db;
    }
    int cw = evr_connect_to_storage(cfg->storage_host, cfg->storage_port);
    if(cw < 0){
        log_error("Failed to connect to evr-glacier-storage server");
        goto out_with_free_db;
    }
    xsltStylesheetPtr style = evr_fetch_stylesheet(cw, spec->transformation_blob_ref);
    if(!style){
        goto out_with_close_cw;
    }
    sqlite3_int64 last_indexed_claim_ts;
    if(evr_attr_index_get_state(db, evr_state_key_last_indexed_claim_ts, &last_indexed_claim_ts) != evr_ok){
        goto out_with_free_style;
    }
    struct evr_blob_filter filter;
    filter.flags_filter = evr_blob_flag_claim;
    filter.last_modified_after = apply_watch_overlap(last_indexed_claim_ts);
    if(evr_req_cmd_watch_blobs(cw, &filter) != evr_ok){
        goto out_with_free_style;
    }
    struct evr_watch_blobs_body wbody;
    fd_set active_fd_set;
    int cs = -1;
    struct timeval timeout;
    while(running){
        FD_ZERO(&active_fd_set);
        FD_SET(cw, &active_fd_set);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int sret = select(cw + 1, &active_fd_set, NULL, NULL, &timeout);
        if(sret < 0){
            goto out_with_close_cs;
        }
        if(!running){
            ret = evr_ok;
            goto out_with_close_cs;
        }
        if(sret == 0){
            continue;
        }
        if(evr_read_watch_blobs_body(cw, &wbody) != evr_ok){
            goto out_with_close_cs;
        }
        if(evr_index_claim_set(db, spec, style, wbody.key, wbody.last_modified, &cs) != evr_ok){
            goto out_with_close_cs;
        }
        if((wbody.flags & evr_watch_flag_eob) == evr_watch_flag_eob){
            break;
        }
    }
    if(evr_attr_index_set_state(db, evr_state_key_stage, evr_attr_index_stage_built) != evr_ok){
        goto out_with_close_cs;
    }
    ret = evr_ok;
 out_with_close_cs:
    if(cs >= 0){
        close(cs);
    }
 out_with_free_style:
    xsltFreeStylesheet(style);
 out_with_close_cw:
    close(cw);
 out_with_free_db:
    if(evr_free_attr_index_db(db) != evr_ok){
        ret = evr_error;
    }
 out:
    return ret;
}

int evr_index_claim_set(struct evr_attr_index_db *db, struct evr_attr_spec_claim *spec, xsltStylesheetPtr style, evr_blob_ref claim_set_ref, evr_time claim_set_last_modified, int *c){
    int ret = evr_error;
#ifdef EVR_LOG_DEBUG
    {
        evr_blob_ref_str ref_str;
        evr_fmt_blob_ref(ref_str, claim_set_ref);
        log_debug("Indexing claim set %s", ref_str);
    }
#endif
    if(*c == -1){
        *c = evr_connect_to_storage(cfg->storage_host, cfg->storage_port);
        if(*c < 0){
            log_error("Failed to connect to evr-glacier-storage server");
            goto out;
        }
    }
    xmlDocPtr claim_set = evr_fetch_signed_xml(*c, claim_set_ref);
    if(!claim_set){
        evr_blob_ref_str ref_str;
        evr_fmt_blob_ref(ref_str, claim_set_ref);
        log_error("Claim set not fetchable for blob key %s", ref_str);
        goto out;
    }
    evr_time t;
    evr_now(&t);
    if(evr_merge_attr_index_claim_set(db, spec, style, t, claim_set_ref, claim_set, 0) != evr_ok){
        goto out_with_free_claim_set;
    }
    if(evr_attr_index_set_state(db, evr_state_key_last_indexed_claim_ts, claim_set_last_modified) != evr_ok){
        goto out_with_free_claim_set;
    }
    ret = evr_ok;
 out_with_free_claim_set:
    xmlFreeDoc(claim_set);
 out:
    return ret;
}

xmlDocPtr get_claim_set_for_reindex(void *ctx, evr_blob_ref claim_set_ref);

int evr_index_sync_worker(void *arg){
    int ret = evr_error;
    struct evr_index_handover_ctx *ctx = arg;
    log_debug("Started index sync worker");
    if(evr_wait_for_handover_occupied(&ctx->handover) != evr_ok){
        goto out;
    }
    evr_blob_ref index_ref;
    memcpy(index_ref, ctx->index_ref, evr_blob_ref_size);
    if(evr_empty_handover(&ctx->handover) != evr_ok){
        goto out;
    }
    int cg = -1; // connection get
    int cw = -1; // connection watch
    struct evr_attr_index_db *db = NULL;
    fd_set active_fd_set;
    struct timeval timeout;
    struct evr_watch_blobs_body wbody;
    struct evr_attr_spec_claim *spec = NULL;
    xsltStylesheetPtr style = NULL;
    evr_time last_reindex = 0;
    while(running){
        if(evr_lock_handover(&ctx->handover) != evr_ok){
            goto out_with_free;
        }
        if(ctx->handover.occupied){
            if(cw != -1){
#ifdef EVR_LOG_DEBUG
                evr_blob_ref_str index_ref_str;
                evr_fmt_blob_ref(index_ref_str, index_ref);
                log_debug("Index sync worker stop index %s", index_ref_str);
#endif
                close(cw);
                cw = -1;
            }
            memcpy(index_ref, ctx->index_ref, evr_blob_ref_size);
            if(evr_empty_handover(&ctx->handover) != evr_ok){
                goto out_with_free;
            }
        } else {
            if(mtx_unlock(&ctx->handover.lock) != thrd_success){
                evr_panic("Failed to unlock evr_index_handover_ctx");
                goto out_with_free;
            }
        }
        if(cw == -1){
            if(style){
                xsltFreeStylesheet(style);
                style = NULL;
            }
            if(spec){
                free(spec);
                spec = NULL;
            }
            if(db){
                if(evr_free_attr_index_db(db) != evr_ok){
                    log_error("Failed to close stopped index db");
                    goto out;
                }
                db = NULL;
            }
            // after this point the former index should be cleaned up
            // with all it's dependant variables
            evr_blob_ref_str index_ref_str;
            evr_fmt_blob_ref(index_ref_str, index_ref);
            log_info("Index sync worker switches to index %s", index_ref_str);
            if(evr_lock_handover(&current_index_ctx.handover) != evr_ok){
                goto out_with_free;
            }
            memcpy(current_index_ctx.index_ref, index_ref, evr_blob_ref_size);
            if(evr_occupy_handover(&current_index_ctx.handover) != evr_ok){
                evr_panic("Failed to occupy current index handover");
                goto out_with_free;
            }
            db = evr_open_attr_index_db(cfg, index_ref_str, evr_write_blob_to_file, NULL);
            if(!db){
                goto out_with_free;
            }
            cw = evr_connect_to_storage(cfg->storage_host, cfg->storage_port);
            if(cw < 0){
                log_error("Failed to connect to evr-glacier-storage server");
                goto out_with_free;
            }
            if(evr_prepare_attr_index_db(db) != evr_ok){
                goto out_with_free;
            }
            xmlDocPtr cs_doc = evr_fetch_signed_xml(cw, index_ref);
            if(!cs_doc){
                evr_blob_ref_str fmt_key;
                evr_fmt_blob_ref(fmt_key, index_ref);
                log_error("Index claim not fetchable for blob key %s", fmt_key);
                goto out_with_free;
            }
            xmlNode *cs_node = evr_get_root_claim_set(cs_doc);
            if(!cs_node){
                goto out_with_free_cs_doc;
            }
            xmlNode *c_node = evr_find_next_element(evr_first_claim(cs_node), "attr-spec");
            if(!c_node){
                goto out_with_free_cs_doc;
            }
            spec = evr_parse_attr_spec_claim(c_node);
            xmlFree(cs_doc);
            if(!spec){
                goto out_with_free;
            }
            style = evr_fetch_stylesheet(cw, spec->transformation_blob_ref);
            if(!style){
                goto out_with_free;
            }
            sqlite3_int64 last_indexed_claim_ts;
            if(evr_attr_index_get_state(db, evr_state_key_last_indexed_claim_ts, &last_indexed_claim_ts) != evr_ok){
                goto out_with_free;
            }
            struct evr_blob_filter filter;
            filter.flags_filter = evr_blob_flag_claim;
            filter.last_modified_after = apply_watch_overlap(last_indexed_claim_ts);
            if(evr_req_cmd_watch_blobs(cw, &filter) != evr_ok){
                goto out_with_free;
            }
            goto end_init_style;
        out_with_free_cs_doc:
            xmlFree(cs_doc);
            goto out_with_free;
        end_init_style:
            log_debug("Index sync worker switch done");
            do{} while(0);
        }
        FD_ZERO(&active_fd_set);
        FD_SET(cw, &active_fd_set);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int sret = select(cw + 1, &active_fd_set, NULL, NULL, &timeout);
        if(sret < 0){
            goto out_with_free;
        }
        if(!running){
            break;
        }
        if(sret == 0){
            evr_time now;
            evr_now(&now);
            // TODO we should use a time source which does not jump on ntpd actions
            if(now - last_reindex >= evr_reindex_interval) {
                last_reindex = now;
                if(evr_reindex_failed_claim_sets(db, spec, style, now, get_claim_set_for_reindex, &cg) != evr_ok){
                    log_error("Error while reindexing failed claim-sets");
                    goto out_with_free;
                }
            }
            // TODO close cg after n timeouts in a row and set to -1
            continue;
        }
        if(evr_read_watch_blobs_body(cw, &wbody) != evr_ok){
            goto out_with_free;
        }
        if(evr_index_claim_set(db, spec, style, wbody.key, wbody.last_modified, &cg) != evr_ok){
            goto out_with_free;
        }
    }
    ret = evr_ok;
 out_with_free:
    if(cg >= 0){
        close(cg);
    }
    if(cw >= 0){
        close(cw);
    }
    if(style){
        xsltFreeStylesheet(style);
    }
    if(spec){
        free(spec);
    }
    if(db){
        if(evr_free_attr_index_db(db) != evr_ok){
            ret = evr_error;
        }
    }
 out:
    log_debug("Ended index sync worker with result %d", ret);
    return ret;
}

xmlDocPtr get_claim_set_for_reindex(void *ctx, evr_blob_ref claim_set_ref){
    int *c = ctx;
    if(*c == -1){
        *c = evr_connect_to_storage(cfg->storage_host, cfg->storage_port);
        if(*c < 0){
            log_error("Failed to connect to evr-glacier-storage server");
            return NULL;
        }
    }
    return evr_fetch_signed_xml(*c, claim_set_ref);
}

int evr_attr_index_tcp_server(){
    int ret = evr_error;
    int s = evr_make_tcp_socket(cfg->host, cfg->port);
    if(s < 0){
        goto out;
    }
    if(listen(s, 7) < 0){
        log_error("Failed to listen on %s:%s", cfg->host, cfg->port);
        goto out_with_close_s;
    }
    log_info("Listening on %s:%s", cfg->host, cfg->port);
    fd_set active_fd_set;
    struct timeval timeout;
    struct sockaddr_in client_addr;
    while(running){
        FD_ZERO(&active_fd_set);
        FD_SET(s, &active_fd_set);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int sret = select(s + 1, &active_fd_set, NULL, NULL, &timeout);
        if(sret < 0){
            goto out_with_close_s;
        }
        if(!running){
            break;
        }
        if(sret == 0){
            continue;
        }
        for(int i = 0; i < FD_SETSIZE; ++i){
            if(FD_ISSET(i, &active_fd_set)){
                if(i == s){
                    socklen_t size = sizeof(client_addr);
                    int c = accept(s, (struct sockaddr*)&client_addr, &size);
                    if(c < 0){
                        goto out_with_close_s;
                    }
                    log_debug("Connection from %s:%d accepted (will be worker %d)", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port), c);
                    struct evr_connection *ctx = malloc(sizeof(struct evr_connection));
                    if(!ctx){
                        goto out_with_close_c;
                    }
                    ctx->socket = c;
                    thrd_t t;
                    if(thrd_create(&t, evr_connection_worker, ctx) != thrd_success){
                        goto out_with_free_ctx;
                    }
                    if(thrd_detach(t) != thrd_success){
                        evr_panic("Failed to detach connection worker thread for worker %d", c);
                        goto out_with_close_s;
                    }
                    goto loop;
                out_with_free_ctx:
                    free(ctx);
                out_with_close_c:
                    close(c);
                    log_error("Failed to startup connection from %s:%d", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                loop:
                    continue;
                }
            }
        }
    }
    ret = evr_ok;
 out_with_close_s:
    close(s);
 out:
    return ret;
}

int evr_connection_worker(void *context) {
    int ret = evr_error;
    struct evr_connection ctx = *(struct evr_connection*)context;
    free(context);
    log_debug("Started connection worker %d", ctx.socket);
    char query_str[8*1024];
    char *query_scanned = query_str;
    char *query_end = &query_str[sizeof(query_str)];
    while(running){
        size_t max_read = query_end - query_scanned;
        if(max_read == 0){
            log_debug("Connection worker %d retrieved too big query", ctx.socket);
            goto out_with_close_socket;
        }
        ssize_t bytes_read = read(ctx.socket, query_scanned, max_read);
        if(bytes_read == 0){
            ret = evr_ok;
            goto out_with_close_socket;
        }
        if(bytes_read < 0){
            goto out_with_close_socket;
        }
        char *read_end = &query_scanned[bytes_read];
        while(query_scanned != read_end){
            if(*query_scanned != '\n'){
                ++query_scanned;
                continue;
            }
            *query_scanned = '\0';
            int cmd_res = evr_work_cmd(&ctx, query_str);
            if(cmd_res == evr_end){
                ret = evr_ok;
                goto out_with_close_socket;
            }
            if(cmd_res != evr_ok){
                goto out_with_close_socket;
            }
            size_t l = read_end - (query_scanned + 1);
            if(l > 0){
                memmove(query_str, query_scanned + 1, l);
            }
            read_end -= (query_scanned + 1) - query_str;
            query_scanned = query_str;
        }
    }
 out_with_close_socket:
    close(ctx.socket);
    log_debug("Ended connection worker %d with result %d", ctx.socket, ret);
    return ret;
}

int evr_work_cmd(struct evr_connection *ctx, char *line){
    log_debug("Connection worker %d retrieved cmd: %s", ctx->socket, line);
    char *cmd = line;
    char *args = index(line, ' ');
    if(args){
        *args = '\0';
        ++args;
    }
    if(strcmp(cmd, "s") == 0){
        return evr_work_search_cmd(ctx, args);
    }
    if(strcmp(cmd, "c") == 0){
        return evr_list_claims_for_seed(ctx, args);
    }
    if(strcmp(cmd, "exit") == 0){
        return evr_end;
    }
    if(strcmp(cmd, "?") == 0 || strcmp(cmd, "help") == 0){
        return evr_respond_help(ctx);
    }
    if(evr_respond_status(ctx, 0, "No such command.") != evr_ok){
        return evr_error;
    }
    return evr_respond_message_end(ctx);
}

int evr_work_search_cmd(struct evr_connection *ctx, char *query){
    int ret = evr_error;
    if(query == NULL){
        query = "";
    }
    log_debug("Connection worker %d retrieved query: %s", ctx->socket, query);
    evr_blob_ref index_ref;
    int res = evr_get_current_index_ref(index_ref);
    if(res == evr_end){
        ret = evr_end;
        goto out;
    }
    if(res != evr_ok){
        goto out;
    }
    evr_blob_ref_str index_ref_str;
    evr_fmt_blob_ref(index_ref_str, index_ref);
    log_debug("Connection worker %d is using index %s for query", ctx->socket, index_ref_str);
    struct evr_attr_index_db *db = evr_open_attr_index_db(cfg, index_ref_str, evr_write_blob_to_file, NULL);
    if(!db){
        goto out;
    }
    struct evr_search_ctx sctx;
    sctx.con = ctx;
    if(evr_attr_query_claims(db, query, evr_respond_search_status, evr_respond_search_result, &sctx) != evr_ok){
        goto out_with_free_db;
    }
    if(evr_respond_message_end(ctx) != evr_ok){
        goto out_with_free_db;
    }
    ret = evr_ok;
 out_with_free_db:
    if(evr_free_attr_index_db(db) != evr_ok){
        ret = evr_error;
    }
 out:
    return ret;
}

int evr_respond_search_status(void *context, int parse_res, char *parse_error){
    struct evr_search_ctx *ctx = context;
    ctx->parse_res = parse_res;
    if(parse_res != evr_ok){
        return evr_respond_status(ctx->con, 0, parse_error);
    }
    return evr_respond_status(ctx->con, 1, NULL);
}

int evr_respond_search_result(void *context, const evr_claim_ref ref, struct evr_attr_tuple *attrs, size_t attrs_len){
    int ret = evr_error;
    size_t attrs_size = 0;
    if(attrs){
        struct evr_attr_tuple *end = &attrs[attrs_len];
        for(struct evr_attr_tuple *a = attrs; a != end; ++a){
            attrs_size += 1 + strlen(a->key) + 1 + strlen(a->value) + 1;
        }
    }
    struct evr_search_ctx *ctx = context;
    char buf[evr_claim_ref_str_size + attrs_size];
    struct evr_buf_pos bp;
    evr_init_buf_pos(&bp, buf);
    evr_fmt_claim_ref(bp.pos, ref);
    evr_inc_buf_pos(&bp, evr_claim_ref_str_size - 1);
    evr_push_concat(&bp, "\n");
    if(attrs){
        struct evr_attr_tuple *end = &attrs[attrs_len];
        for(struct evr_attr_tuple *a = attrs; a != end; ++a){
            evr_push_concat(&bp, "\t");
            evr_push_concat(&bp, a->key);
            evr_push_concat(&bp, "=");
            evr_push_concat(&bp, a->value);
            evr_push_concat(&bp, "\n");
        }
    }
    if(write_n(ctx->con->socket, bp.buf, bp.pos - bp.buf) != evr_ok){
        goto out;
    }
    ret = evr_ok;
 out:
    return ret;
}

int evr_respond_claims_for_seed_result(void *ctx, const evr_claim_ref claim);

int evr_list_claims_for_seed(struct evr_connection *ctx, char *seed_ref_str){
    if(seed_ref_str == NULL){
        seed_ref_str = "";
    }
    log_debug("Connection worker %d retrieved list claims for seed %s", ctx->socket, seed_ref_str);
    int ret = evr_error;
    evr_claim_ref seed_ref;
    if(evr_parse_claim_ref(seed_ref, seed_ref_str) != evr_ok){
        log_error("Failed to parse seed_ref %s", seed_ref_str);
        goto out;
    }
    evr_blob_ref index_ref;
    int res = evr_get_current_index_ref(index_ref);
    if(res == evr_end){
        ret = evr_end;
        goto out;
    }
    if(res != evr_ok){
        goto out;
    }
    evr_blob_ref_str index_ref_str;
    evr_fmt_blob_ref(index_ref_str, index_ref);
    log_debug("Connection worker %d is using index %s for list claims for seed", ctx->socket, index_ref_str);
    struct evr_attr_index_db *db = evr_open_attr_index_db(cfg, index_ref_str, evr_write_blob_to_file, NULL);
    if(!db){
        goto out;
    }
    if(evr_prepare_attr_index_db(db) != evr_ok){
        goto out;
    }
    if(evr_attr_visit_claims_for_seed(db, seed_ref, evr_respond_claims_for_seed_result, ctx) != evr_ok){
        goto out_with_free_db;
    }
    if(evr_respond_message_end(ctx) != evr_ok){
        goto out_with_free_db;
    }
    ret = evr_ok;
 out_with_free_db:
    if(evr_free_attr_index_db(db) != evr_ok){
        ret = evr_error;
    }
 out:
    return ret;
}

int evr_respond_claims_for_seed_result(void *context, const evr_claim_ref claim){
    struct evr_connection *ctx = context;
    evr_claim_ref_str claim_str;
    evr_fmt_claim_ref(claim_str, claim);
    claim_str[evr_claim_ref_str_size - 1] = '\n';
    return write_n(ctx->socket, claim_str, evr_claim_ref_str_size);
}

int evr_get_current_index_ref(evr_blob_ref index_ref){
    if(evr_wait_for_handover_occupied(&current_index_ctx.handover) != evr_ok){
        return evr_error;
    }
    memcpy(index_ref, current_index_ctx.index_ref, evr_blob_ref_size);
    if(evr_unlock_handover(&current_index_ctx.handover) != evr_ok){
        evr_panic("Failed to unlock current index handover");
        return evr_error;
    }
    if(!running){
        return evr_end;
    }
    return evr_ok;
}

int evr_respond_help(struct evr_connection *ctx){
    int ret = evr_error;
    if(evr_respond_status(ctx, 1, NULL) != evr_ok){
        goto out;
    }
    const char help[] = PACKAGE_STRING "\n"
        "These commands are defined.\n"
        "exit - closes the conneciton\n"
        "help - shows this help message\n"
        "s QUERY - searches for claims matching the given query.\n"
        "c REF - lists all claims referencing the given seed claim.\n"
        ;
    if(write_n(ctx->socket, help, sizeof(help)) != evr_ok){
        goto out;
    }
    if(evr_respond_message_end(ctx) != evr_ok){
        goto out;
    }
    ret = evr_ok;
 out:
    return ret;
}

int evr_respond_status(struct evr_connection *ctx, int ok, char *msg){
    size_t msg_len = msg ? 1 + strlen(msg) : 0;
    char buf[5 + msg_len + 1 + 1];
    struct evr_buf_pos bp;
    evr_init_buf_pos(&bp, buf);
    evr_push_concat(&bp, ok ? "OK" : "ERROR");
    if(msg){
        evr_push_concat(&bp, " ");
        evr_push_concat(&bp, msg);
    }
    evr_push_concat(&bp, "\n");
    return write_n(ctx->socket, buf, bp.pos - bp.buf);
}

int evr_respond_message_end(struct evr_connection *ctx){
    return write_n(ctx->socket, "\n", 1);
}

int evr_write_blob_to_file(void *ctx, char *path, mode_t mode, evr_blob_ref ref){
    int ret = evr_error;
    int f = creat(path, mode);
    if(f < 0){
        goto out;
    }
    int c = evr_connect_to_storage(cfg->storage_host, cfg->storage_port);
    if(c < 0){
        log_error("Failed to connect to evr-glacier-storage server");
        goto out_with_close_f;
    }
    struct evr_resp_header resp;
    if(evr_req_cmd_get_blob(c, ref, &resp) != evr_ok){
        goto out_with_close_c;
        return evr_error;
    }
    if(resp.status_code != evr_status_code_ok){
        evr_blob_ref_str ref_str;
        evr_fmt_blob_ref(ref_str, ref);
        log_error("Failed to read blob %s from server. Responded status code was 0x%02x", resp.status_code);
        goto out;
    }
    if(resp.body_size > evr_max_blob_data_size){
        log_error("Server indicated huge blob size of %ul bytes", resp.body_size);
        goto out_with_close_c;
    }
    // ignore one byte containing the flags
    char buf[1];
    if(read_n(c, buf, sizeof(buf)) != evr_ok){
        goto out_with_close_c;
    }
    if(pipe_n(f, c, resp.body_size - sizeof(buf)) != evr_ok){
        goto out_with_close_c;
    }
    ret = evr_ok;
 out_with_close_c:
    if(close(c)){
        ret = evr_error;
    }
 out_with_close_f:
    if(close(f)){
        ret = evr_error;
    }
 out:
    return ret;
}
