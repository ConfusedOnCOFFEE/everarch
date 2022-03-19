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

#include "basics.h"
#include "claims.h"
#include "errors.h"
#include "logger.h"
#include "evr-glacier-client.h"
#include "signatures.h"
#include "server.h"

sig_atomic_t running = 1;
mtx_t stop_lock;
cnd_t stop_signal;

struct evr_attr_spec_handover_ctx {
    mtx_t lock;
    cnd_t on_push_spec;
    cnd_t on_empty_spec;

    /**
     * claim stores the handed over attr-spec claim. NULL indicates
     * that no claim is handed over right now.
     */
    struct evr_attr_spec_claim *claim;
    evr_blob_key_t claim_key;
    time_t created;
    xmlDocPtr stylesheet;
};

void handle_sigterm(int signum);
int evr_init_attr_spec_handover_ctx(struct evr_attr_spec_handover_ctx *ctx);
int evr_free_attr_spec_handover_ctx(struct evr_attr_spec_handover_ctx *ctx);
int evr_watch_index_claims_worker(void *arg);
int evr_build_index_worker(void *arg);
int evr_attr_index_tcp_server();

int main(){
    int ret = evr_error;
    if(mtx_init(&stop_lock, mtx_plain) != thrd_success){
        goto out;
    }
    if(cnd_init(&stop_signal) != thrd_success){
        goto out_with_free_stop_lock;
    }
    {
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = handle_sigterm;
        sigaction(SIGINT, &action, NULL);
        signal(SIGPIPE, SIG_IGN);
    }
    evr_init_signatures();
    xmlInitParser();
    struct evr_attr_spec_handover_ctx attr_spec_handover_ctx;
    if(evr_init_attr_spec_handover_ctx(&attr_spec_handover_ctx) != evr_ok){
        goto out_with_cleanup_xml_parser;
    }
    thrd_t watch_index_claims_thrd;
    if(thrd_create(&watch_index_claims_thrd, evr_watch_index_claims_worker, &attr_spec_handover_ctx) != thrd_success){
        goto out_with_free_attr_spec_handover_ctx;
    }
    thrd_t build_index_thrd;
    if(thrd_create(&build_index_thrd, evr_build_index_worker, &attr_spec_handover_ctx) != thrd_success){
        goto out_with_join_watch_index_claims_thrd;
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
    if(cnd_signal(&attr_spec_handover_ctx.on_push_spec) != thrd_success){
        evr_panic("Failed to signal on_push_spec on termination");
        goto out_with_join_watch_index_claims_thrd;
    }
    if(cnd_signal(&attr_spec_handover_ctx.on_empty_spec) != thrd_success){
        evr_panic("Failed to signal on_empty_spec on termination");
        goto out_with_join_watch_index_claims_thrd;
    }
    // TODO
    /*if(evr_attr_index_tcp_server() != evr_ok){
        goto out_with_join_build_index_thrd;
        }*/
    ret = evr_ok;
    int thrd_res;
    // TODO out_with_join_build_index_thrd:
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
 out_with_free_attr_spec_handover_ctx:
    if(evr_free_attr_spec_handover_ctx(&attr_spec_handover_ctx) != evr_ok){
        evr_panic("Failed to free attr-spec handover context");
        ret = evr_error;
    }
 out_with_cleanup_xml_parser:
    xmlCleanupParser();
    cnd_destroy(&stop_signal);
 out_with_free_stop_lock:
    mtx_destroy(&stop_lock);
 out:
    return ret;
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

int evr_init_attr_spec_handover_ctx(struct evr_attr_spec_handover_ctx *ctx){
    int ret = evr_error;
    if(mtx_init(&ctx->lock, mtx_plain) != thrd_success){
        goto out;
    }
    if(cnd_init(&ctx->on_push_spec) != thrd_success){
        goto out_with_free_lock;
    }
    if(cnd_init(&ctx->on_empty_spec) != thrd_success){
        goto out_with_free_on_push_spec;
    }
    ctx->claim = NULL;
    ret = evr_ok;
 out:
    return ret;
 out_with_free_on_push_spec:
    cnd_destroy(&ctx->on_push_spec);
 out_with_free_lock:
    mtx_destroy(&ctx->lock);
    return ret;
}

int evr_free_attr_spec_handover_ctx(struct evr_attr_spec_handover_ctx *ctx){
    if(ctx->claim){
        free(ctx->claim);
        xmlFree(ctx->stylesheet);
    }
    cnd_destroy(&ctx->on_empty_spec);
    cnd_destroy(&ctx->on_push_spec);
    mtx_destroy(&ctx->lock);
    return evr_ok;
}

int evr_watch_index_claims_worker(void *arg){
    int ret = evr_error;
    struct evr_attr_spec_handover_ctx *ctx = arg;
    log_debug("Started watch index claims worker");
    // cw is the connection used for watching for blob changes.
    int cw = evr_connect_to_storage();
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
    evr_blob_key_t latest_spec_key;
    time_t latest_spec_created = 0;
    // cs is the connection used for finding the most recent
    // attr-spec claim
    int cs = -1;
    log_debug("Watching index claims");
    fd_set active_fd_set;
    FD_ZERO(&active_fd_set);
    FD_SET(cw, &active_fd_set);
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    while(running){
        int sret = select(FD_SETSIZE, &active_fd_set, NULL, NULL, &timeout);
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
            evr_fmt_blob_key_t fmt_key;
            evr_fmt_blob_key(fmt_key, body.key);
            log_info("Checking index claim %s for attr-spec", fmt_key);
        } while(0);
#endif
        if(cs == -1){
            cs = evr_connect_to_storage();
            if(cs < 0){
                log_error("Failed to connect to evr-glacier-storage server");
                goto out_with_free_latest_spec;
            }
        }
        xmlDocPtr claim_doc = evr_fetch_signed_xml(cs, body.key);
        if(!claim_doc){
            evr_fmt_blob_key_t fmt_key;
            evr_fmt_blob_key(fmt_key, body.key);
            log_error("Index claim not fetchable for blob key %s", fmt_key);
            goto out_with_free_latest_spec;
        }
        xmlNode *cs_node = evr_get_root_claim_set(claim_doc);
        if(!cs_node){
            evr_fmt_blob_key_t fmt_key;
            evr_fmt_blob_key(fmt_key, body.key);
            log_error("Index claim does not contain claim-set for blob key %s", fmt_key);
            goto out_with_free_claim_doc;
        }
        time_t created;
        if(evr_parse_created(&created, cs_node) != evr_ok){
            evr_fmt_blob_key_t fmt_key;
            evr_fmt_blob_key(fmt_key, body.key);
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
                memcpy(latest_spec_key, body.key, evr_blob_key_size);
                latest_spec_created = created;
            }
        }
        xmlFree(claim_doc);
        if((body.flags & evr_watch_flag_eob) == 0 || !latest_spec){
            continue;
        }
        xmlDocPtr xslt_doc = evr_fetch_xml(cs, latest_spec->stylesheet_blob_ref);
        if(!xslt_doc){
            evr_fmt_blob_key_t fmt_key;
            evr_fmt_blob_key(fmt_key, body.key);
            log_error("Failed to fetch stylesheet for attr-spec with blob key %s", fmt_key);
            goto out_with_free_latest_spec;
        }
        close(cs);
        cs = -1;
        if(mtx_lock(&ctx->lock) != thrd_success){
            evr_panic("Failed to lock attr-spec handover lock");
            goto out_with_free_xslt_doc;
        }
        while(ctx->claim){
            // handover ctx is still occupied
            if(!running){
                if(mtx_unlock(&ctx->lock) != thrd_success){
                    evr_panic("Failed to unlock attr-spec handover lock");
                    goto out;
                }
                ret = evr_ok;
                goto out_of_running_loop;
            }
            if(cnd_wait(&ctx->on_empty_spec, &ctx->lock) != thrd_success){
                evr_panic("Failed to wait for empty spec signal");
                goto out_with_free_xslt_doc;
            }
        }
        // handover ctx is available
#ifdef EVR_LOG_DEBUG
        {
            evr_fmt_blob_key_t fmt_key;
            evr_fmt_blob_key(fmt_key, latest_spec_key);
            log_debug("Handover latest attr-spec %s", fmt_key);
        }
#endif
        ctx->claim = latest_spec;
        memcpy(ctx->claim_key, latest_spec_key, evr_blob_key_size);
        ctx->created = latest_spec_created;
        ctx->stylesheet = xslt_doc;
        if(cnd_signal(&ctx->on_push_spec) != thrd_success){
            evr_panic("Failed to signal spec pushed");
            goto out_with_free_xslt_doc;
        }
        latest_spec = NULL;
        if(mtx_unlock(&ctx->lock) != thrd_success){
            evr_panic("Failed to unlock attr-spec handover lock");
            goto out_with_free_xslt_doc;
        }
        continue;
    out_with_free_xslt_doc:
        xmlFree(xslt_doc);
        goto out_with_free_latest_spec;
    out_with_free_claim_doc:
        xmlFree(claim_doc);
        goto out_with_free_latest_spec;
    out_of_running_loop:
        break;
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
    struct evr_attr_spec_handover_ctx *ctx = arg;
    log_debug("Started build index worker");
    while(running){
        if(mtx_lock(&ctx->lock) != thrd_success){
            evr_panic("Failed to lock attr-spec handover lock");
            goto out;
        }
        while(!ctx->claim){
            if(!running){
                if(mtx_unlock(&ctx->lock) != thrd_success){
                    evr_panic("Failed to unlock attr-spec handover lock");
                    goto out;
                }
                ret = evr_ok;
                goto out_of_running_loop;
            }
            if(cnd_wait(&ctx->on_push_spec, &ctx->lock) != thrd_success){
                evr_panic("Failed to wait for attr-spec push");
                goto out;
            }
        }
        struct evr_attr_spec_claim *claim = ctx->claim;
        evr_blob_key_t claim_key;
        memcpy(claim_key, ctx->claim_key, evr_blob_key_size);
        // TODO time_t created = ctx->created;;
        xmlDocPtr stylesheet = ctx->stylesheet;
        ctx->claim = NULL;
        if(cnd_signal(&ctx->on_empty_spec) != thrd_success){
            evr_panic("Failed to signal attr-spec empty");
            goto out;
        }
        if(mtx_unlock(&ctx->lock) != thrd_success){
            evr_panic("Failed to unlock attr-spec handover lock");
            goto out;
        }
#ifdef EVR_LOG_DEBUG
        {
            evr_fmt_blob_key_t fmt_key;
            evr_fmt_blob_key(fmt_key, claim_key);
            log_debug("Start building attr index for %s", fmt_key);
        }
#endif
        // TODO
        log_debug(">>> got an index to build");
        free(claim);
        xmlFree(stylesheet);
        continue;
    out_of_running_loop:
        break;
    }
    log_debug("Ended build index worker with result %d", ret);
 out:
    return ret;
}

int evr_attr_index_tcp_server(){
    int ret = evr_error;
    int s = evr_make_tcp_socket(evr_glacier_attr_index_port);
    if(s < 0){
        log_error("Failed to create socket");
        goto out;
    }
    if(listen(s, 7) < 0){
        log_error("Failed to listen on localhost:%d", evr_glacier_attr_index_port);
        goto out_with_close_s;
    }
    log_info("Listening on localhost:%d", evr_glacier_attr_index_port);
    fd_set active_fd_set;
    FD_ZERO(&active_fd_set);
    FD_SET(s, &active_fd_set);
    while(running){
        int sret = select(FD_SETSIZE, &active_fd_set, NULL, NULL, NULL);
        if(sret == -1){
            // select returns -1 on sigint.
            goto out_with_close_s;
        } else if(sret < 0){
            goto out_with_close_s;
        }
        // TODO check active_fd_set and accept connection
    }
    ret = evr_ok;
 out_with_close_s:
    close(s);
 out:
    return ret;
}