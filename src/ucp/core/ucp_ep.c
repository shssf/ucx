/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "ucp_ep.h"
#include "ucp_worker.h"
#include "ucp_ep.inl"
#include "ucp_request.inl"

#include <ucp/wireup/stub_ep.h>
#include <ucp/wireup/wireup.h>
#include <ucp/tag/eager.h>
#include <ucs/debug/memtrack.h>
#include <ucs/debug/log.h>
#include <ucs/sys/string.h>
#include <string.h>


#if ENABLE_STATS
static ucs_stats_class_t ucp_ep_stats_class = {
    .name           = "ucp_ep",
    .num_counters   = UCP_EP_STAT_LAST,
    .counter_names  = {
        [UCP_EP_STAT_TAG_TX_EAGER]      = "tx_eager",
        [UCP_EP_STAT_TAG_TX_EAGER_SYNC] = "tx_eager_sync",
        [UCP_EP_STAT_TAG_TX_RNDV]       = "tx_rndv"
    }
};
#endif


void ucp_ep_config_key_reset(ucp_ep_config_key_t *key)
{
    memset(key, 0, sizeof(*key));
    key->num_lanes        = 0;
    key->am_lane          = UCP_NULL_LANE;
    key->rndv_lane        = UCP_NULL_LANE;
    key->wireup_lane      = UCP_NULL_LANE;
    key->reachable_md_map = 0;
    memset(key->rma_lanes, UCP_NULL_LANE, sizeof(key->rma_lanes));
    memset(key->amo_lanes, UCP_NULL_LANE, sizeof(key->amo_lanes));
}

ucs_status_t ucp_ep_new(ucp_worker_h worker, uint64_t dest_uuid,
                        const char *peer_name, const char *message,
                        ucp_ep_h *ep_p)
{
    ucs_status_t status;
    ucp_ep_config_key_t key;
    ucp_ep_h ep;
    khiter_t hash_it;
    int hash_extra_status = 0;

    ep = ucs_calloc(1, sizeof(*ep), "ucp ep");
    if (ep == NULL) {
        ucs_error("Failed to allocate ep");
        status = UCS_ERR_NO_MEMORY;
        goto err;
    }

    ucp_ep_config_key_reset(&key);
    ep->worker           = worker;
    ep->dest_uuid        = dest_uuid;
    ep->cfg_index        = ucp_worker_get_ep_config(worker, &key);
    ep->am_lane          = UCP_NULL_LANE;
    ep->flags            = 0;
#if ENABLE_DEBUG_DATA
    ucs_snprintf_zero(ep->peer_name, UCP_WORKER_NAME_MAX, "%s", peer_name);
#endif

    /* Create statistics */
    status = UCS_STATS_NODE_ALLOC(&ep->stats, &ucp_ep_stats_class,
                                  worker->stats, "-%p", ep);
    if (status != UCS_OK) {
        goto err_free_ep;
    }

    hash_it = kh_put(ucp_worker_ep_hash, &worker->ep_hash, dest_uuid,
                     &hash_extra_status);
    if (ucs_unlikely(hash_it == kh_end(&worker->ep_hash))) {
        ucs_error("Hash failed with ep %p to %s 0x%"PRIx64"->0x%"PRIx64" %s "
                  "with status %d", ep, peer_name, worker->uuid, ep->dest_uuid,
                  message, hash_extra_status);
        status = UCS_ERR_NO_RESOURCE;
        goto err_free_stats;
    }
    kh_value(&worker->ep_hash, hash_it) = ep;

    *ep_p = ep;
    ucs_debug("created ep %p to %s 0x%"PRIx64"->0x%"PRIx64" %s", ep, peer_name,
              worker->uuid, ep->dest_uuid, message);
    return UCS_OK;

err_free_stats:
    UCS_STATS_NODE_FREE(ep->stats);
err_free_ep:
    ucs_free(ep);
err:
    return status;
}

static void ucp_ep_delete_from_hash(ucp_ep_h ep)
{
    khiter_t hash_it;

    hash_it = kh_get(ucp_worker_ep_hash, &ep->worker->ep_hash, ep->dest_uuid);
    if (hash_it != kh_end(&ep->worker->ep_hash)) {
        kh_del(ucp_worker_ep_hash, &ep->worker->ep_hash, hash_it);
    }
}

static void ucp_ep_delete(ucp_ep_h ep)
{
    ucp_ep_delete_from_hash(ep);
    UCS_STATS_NODE_FREE(ep->stats);
    ucs_free(ep);
}

ucs_status_t ucp_ep_create_stub(ucp_worker_h worker, uint64_t dest_uuid,
                                const char *message, ucp_ep_h *ep_p)
{
    ucs_status_t status;
    ucp_ep_config_key_t key;
    ucp_ep_h ep = NULL;

    status = ucp_ep_new(worker, dest_uuid, "??", message, &ep);
    if (status != UCS_OK) {
        goto err;
    }

    ucp_ep_config_key_reset(&key);

    /* all operations will use the first lane, which is a stub endpoint */
    key.num_lanes             = 1;
    key.lanes[0].rsc_index    = UCP_NULL_RESOURCE;
    key.lanes[0].dst_md_index = UCP_NULL_RESOURCE;
    key.am_lane               = 0;
    key.rndv_lane             = 0;
    key.wireup_lane           = 0;

    ep->cfg_index        = ucp_worker_get_ep_config(worker, &key);
    ep->am_lane          = 0;

    status = ucp_stub_ep_create(ep, &ep->uct_eps[0]);
    if (status != UCS_OK) {
        goto err_destroy_uct_eps;
    }

    *ep_p = ep;
    return UCS_OK;

err_destroy_uct_eps:
    uct_ep_destroy(ep->uct_eps[0]);
    ucp_ep_delete(ep);
err:
    return status;
}

int ucp_ep_is_stub(ucp_ep_h ep)
{
    return ucp_ep_get_rsc_index(ep, 0) == UCP_NULL_RESOURCE;
}

ucs_status_t ucp_ep_create(ucp_worker_h worker,
                           const ucp_ep_params_t *params,
                           ucp_ep_h *ep_p)
{
    char peer_name[UCP_WORKER_NAME_MAX];
    uint8_t addr_indices[UCP_MAX_LANES];
    ucp_address_entry_t *address_list;
    unsigned address_count;
    ucs_status_t status;
    uint64_t dest_uuid;
    ucp_ep_h ep;

    UCP_THREAD_CS_ENTER_CONDITIONAL(&worker->mt_lock);

    UCS_ASYNC_BLOCK(&worker->async);

    if (params->field_mask & UCP_EP_PARAM_FIELD_REMOTE_ADDRESS) {
        status = ucp_address_unpack(params->address, &dest_uuid, peer_name, sizeof(peer_name),
                                    &address_count, &address_list);
        if (status != UCS_OK) {
            ucs_error("failed to unpack remote address: %s", ucs_status_string(status));
            goto out;
        }
    } else {
        status = UCS_ERR_INVALID_PARAM;
        ucs_error("remote address is missing: %s", ucs_status_string(status));
        goto out;
    }

    ep = ucp_worker_ep_find(worker, dest_uuid);
    if (ep != NULL) {
        /* TODO handle a case where the existing endpoint is incomplete */
        *ep_p = ep;
        status = UCS_OK;
        goto out_free_address;
    }

    /* allocate endpoint */
    status = ucp_ep_new(worker, dest_uuid, peer_name, "from api call", &ep);
    if (status != UCS_OK) {
        goto out_free_address;
    }

    /* initialize transport endpoints */
    status = ucp_wireup_init_lanes(ep, address_count, address_list, addr_indices);
    if (status != UCS_OK) {
        goto err_destroy_ep;
    }

    /* send initial wireup message */
    if (!(ep->flags & UCP_EP_FLAG_LOCAL_CONNECTED)) {
        status = ucp_wireup_send_request(ep);
        if (status != UCS_OK) {
            goto err_destroy_ep;
        }
    }

    *ep_p = ep;
    goto out_free_address;

err_destroy_ep:
    ucp_ep_destroy(ep);
out_free_address:
    ucs_free(address_list);
out:
    UCS_ASYNC_UNBLOCK(&worker->async);
    UCP_THREAD_CS_EXIT_CONDITIONAL(&worker->mt_lock);
    return status;
}

static void ucp_ep_flush_error(ucp_request_t *req, ucs_status_t status)
{
    ucs_error("error during flush: %s", ucs_status_string(status));
    req->status = status;
    --req->send.uct_comp.count;
}

static void ucp_ep_flush_progress(ucp_request_t *req)
{
    ucp_ep_h ep = req->send.ep;
    ucp_lane_index_t lane;
    ucs_status_t status;
    uct_ep_h uct_ep;

    ucs_trace("ep %p: progress flush req %p, lanes 0x%x count %d", ep, req,
              req->send.flush.lanes, req->send.uct_comp.count);

    while (req->send.flush.lanes) {

        /* Search for next lane to start flush */
        lane   = ucs_ffs64(req->send.flush.lanes);
        uct_ep = ep->uct_eps[lane];
        if (uct_ep == NULL) {
            req->send.flush.lanes &= ~UCS_BIT(lane);
            --req->send.uct_comp.count;
            continue;
        }

        /* Start flush operation on UCT endpoint */
        status = uct_ep_flush(uct_ep, 0, &req->send.uct_comp);
        ucs_trace("flushing ep %p lane[%d]: %s", ep, lane,
                  ucs_status_string(status));
        if (status == UCS_OK) {
            req->send.flush.lanes &= ~UCS_BIT(lane);
            --req->send.uct_comp.count;
        } else if (status == UCS_INPROGRESS) {
            req->send.flush.lanes &= ~UCS_BIT(lane);
        } else if (status == UCS_ERR_NO_RESOURCE) {
            if (req->send.lane != UCP_NULL_LANE) {
                ucs_trace("ep %p: not adding pending flush %p on lane %d, "
                          "because it's already pending on lane %d",
                          ep, req, lane, req->send.lane);
                break;
            }

            status = uct_ep_pending_add(uct_ep, &req->send.uct);
            ucs_trace("adding pending flush on ep %p lane[%d]: %s", ep, lane,
                      ucs_status_string(status));
            if (status == UCS_OK) {
                req->send.lane        = lane;
                req->send.flush.lanes &= ~UCS_BIT(lane);
            } else if (status != UCS_ERR_BUSY) {
                ucp_ep_flush_error(req, status);
                break;
            }
        } else {
            ucp_ep_flush_error(req, status);
            break;
        }
    }
}

static void ucp_ep_flush_slow_path_remove(ucp_request_t *req)
{
    ucp_ep_h ep = req->send.ep;
    if (req->send.flush.cbq_elem_on) {
        uct_worker_slowpath_progress_unregister(ep->worker->uct,
                                                &req->send.flush.cbq_elem);
        req->send.flush.cbq_elem_on = 0;
    }
}

static void ucp_ep_flushed_slow_path_callback(ucs_callbackq_slow_elem_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.flush.cbq_elem);
    ucp_ep_h ep = req->send.ep;

    ucs_assert(!(req->flags & UCP_REQUEST_FLAG_COMPLETED));

    ucs_trace("flush req %p ep %p remove from uct_worker %p", req, ep,
              ep->worker->uct);
    ucp_ep_flush_slow_path_remove(req);
    req->send.flush.flushed_cb(req);

    /* Complete send request from here, to avoid releasing the request while
     * slow-path element is still pending */
    ucp_request_complete_send(req, req->status);
}

static int ucp_flush_check_completion(ucp_request_t *req)
{
    ucp_ep_h ep = req->send.ep;

    /* Check if flushed all lanes */
    if (req->send.uct_comp.count != 0) {
        return 0;
    }

    ucs_trace("adding slow-path callback to destroy ep %p", ep);
    ucp_ep_flush_slow_path_remove(req);
    req->send.flush.cbq_elem.cb = ucp_ep_flushed_slow_path_callback;
    req->send.flush.cbq_elem_on = 1;
    uct_worker_slowpath_progress_register(ep->worker->uct,
                                          &req->send.flush.cbq_elem);
    return 1;
}

static void ucp_ep_flush_resume_slow_path_callback(ucs_callbackq_slow_elem_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.flush.cbq_elem);

    ucp_ep_flush_slow_path_remove(req);
    ucp_ep_flush_progress(req);
    ucp_flush_check_completion(req);
}

static ucs_status_t ucp_ep_flush_progress_pending(uct_pending_req_t *self)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct);
    ucp_lane_index_t lane = req->send.lane;
    ucp_ep_h ep = req->send.ep;
    ucs_status_t status;
    int completed;

    ucs_assert(!(req->flags & UCP_REQUEST_FLAG_COMPLETED));

    status = uct_ep_flush(ep->uct_eps[lane], 0, &req->send.uct_comp);
    ucs_trace("flushing ep %p lane[%d]: %s", ep, lane,
              ucs_status_string(status));
    if (status == UCS_OK) {
        --req->send.uct_comp.count; /* UCT endpoint is flushed */
    }

    /* since req->flush.pend.lane is still non-NULL, this function will not
     * put anything on pending.
     */
    ucp_ep_flush_progress(req);
    completed = ucp_flush_check_completion(req);

    /* If the operation has not completed, add slow-path progress to resume */
    if (!completed && req->send.flush.lanes && !req->send.flush.cbq_elem_on) {
        ucs_trace("ep %p: adding slow-path callback to resume flush", ep);
        req->send.flush.cbq_elem.cb = ucp_ep_flush_resume_slow_path_callback;
        req->send.flush.cbq_elem_on = 1;
        uct_worker_slowpath_progress_register(ep->worker->uct,
                                              &req->send.flush.cbq_elem);
    }

    if ((status == UCS_OK) || (status == UCS_INPROGRESS)) {
        req->send.lane = UCP_NULL_LANE;
        return UCS_OK;
    } else if (status == UCS_ERR_NO_RESOURCE) {
        return UCS_ERR_NO_RESOURCE;
    } else {
        ucp_ep_flush_error(req, status);
        return UCS_OK;
    }
}

static void ucp_ep_flush_completion(uct_completion_t *self, ucs_status_t status)
{
    ucp_request_t *req = ucs_container_of(self, ucp_request_t, send.uct_comp);

    ucs_assert(!(req->flags & UCP_REQUEST_FLAG_COMPLETED));

    if (status == UCS_OK) {
        req->status = status;
    }

    ucp_ep_flush_progress(req);
    ucp_flush_check_completion(req);
}

static void ucp_destroyed_ep_pending_purge(uct_pending_req_t *self, void *arg)
{
    ucs_bug("pending request %p on ep %p should have been flushed", self, arg);
}

void ucp_ep_destroy_internal(ucp_ep_h ep, const char *message)
{
    ucp_lane_index_t lane;
    uct_ep_h uct_ep;

    ucs_debug("destroy ep %p%s", ep, message);

    for (lane = 0; lane < ucp_ep_num_lanes(ep); ++lane) {
        uct_ep = ep->uct_eps[lane];
        if (uct_ep == NULL) {
            continue;
        }

        uct_ep_pending_purge(uct_ep, ucp_destroyed_ep_pending_purge, ep);
        ucs_debug("destroy ep %p lane[%d]=%p", ep, lane, uct_ep);
        uct_ep_destroy(uct_ep);
    }

    UCS_STATS_NODE_FREE(ep->stats);
    ucs_free(ep);
}

static void ucp_ep_disconnected(ucp_request_t *req)
{
    ucp_ep_h ep = req->send.ep;

    if (ep->flags & UCP_EP_FLAG_REMOTE_CONNECTED) {
        /* Endpoints which have remote connection are destroyed only when the
         * worker is destroyed, to enable remote endpoints keep sending
         * TODO negotiate disconnect.
         */
        ucs_trace("not destroying ep %p because of connection from remote", ep);
        return;
    }

    ucp_ep_delete_from_hash(ep);
    ucp_ep_destroy_internal(ep, " from disconnect");
}

static ucs_status_ptr_t ucp_disconnect_nb_internal(ucp_ep_h ep)
{
    ucs_status_t status;
    ucp_request_t *req;

    ucs_debug("disconnect ep %p", ep);

    req = ucs_mpool_get(&ep->worker->req_mp);
    if (req == NULL) {
        return UCS_STATUS_PTR(UCS_ERR_NO_MEMORY);
    }

    /*
     *  Flush operation can be queued on the pending queue of only one of the
     * lanes (indicated by req->send.lane) and scheduled for completion on any
     * number of lanes. req->send.uct_comp.count keeps track of how many lanes
     * are not flushed yet, and when it reaches zero, it means all lanes are
     * flushed. req->send.flush.lanes keeps track of which lanes we still have
     * to start flush on.
     *  If a flush is completed from a pending/completion callback, we need to
     * schedule slow-path callback to release the endpoint later, since a UCT
     * endpoint cannot be released from pending/completion callback context.
     */
    req->flags                  = 0;
    req->status                 = UCS_OK;
    req->send.ep                = ep;
    req->send.flush.flushed_cb  = ucp_ep_disconnected;
    req->send.flush.lanes       = UCS_MASK(ucp_ep_num_lanes(ep));
    req->send.flush.cbq_elem.cb = ucp_ep_flushed_slow_path_callback;
    req->send.flush.cbq_elem_on = 0;
    req->send.lane              = UCP_NULL_LANE;
    req->send.uct.func          = ucp_ep_flush_progress_pending;
    req->send.uct_comp.func     = ucp_ep_flush_completion;
    req->send.uct_comp.count    = ucp_ep_num_lanes(ep);

    ucp_ep_flush_progress(req);

    if (req->send.uct_comp.count == 0) {
        status = req->status;
        ucp_ep_disconnected(req);
        ucs_trace_req("ep %p: releasing flush request %p, returning status %s",
                      ep, req, ucs_status_string(status));
        ucs_mpool_put(req);
        return UCS_STATUS_PTR(status);
    }

    ucs_trace_req("ep %p: return inprogress flush request %p (%p)", ep, req,
                  req + 1);
    return req + 1;
}

ucs_status_ptr_t ucp_disconnect_nb(ucp_ep_h ep)
{
    ucp_worker_h worker = ep->worker;
    void *request;

    UCP_THREAD_CS_ENTER_CONDITIONAL(&worker->mt_lock);

    UCS_ASYNC_BLOCK(&worker->async);
    request = ucp_disconnect_nb_internal(ep);
    UCS_ASYNC_UNBLOCK(&worker->async);

    UCP_THREAD_CS_EXIT_CONDITIONAL(&worker->mt_lock);

    return request;
}

void ucp_ep_destroy(ucp_ep_h ep)
{
    ucp_worker_h worker = ep->worker;
    ucs_status_ptr_t *request;
    ucs_status_t status;

    UCP_THREAD_CS_ENTER_CONDITIONAL(&worker->mt_lock);

    request = ucp_disconnect_nb(ep);
    if (request == NULL) {
        goto out;
    } else if (UCS_PTR_IS_ERR(request)) {
        ucs_warn("disconnect failed: %s",
                 ucs_status_string(UCS_PTR_STATUS(request)));
        goto out;
    } else {
        do {
            ucp_worker_progress(worker);
            status = ucp_request_test(request, NULL);
        } while (status == UCS_INPROGRESS);
        ucp_request_release(request);
    }

out:
    UCP_THREAD_CS_EXIT_CONDITIONAL(&worker->mt_lock);
    return;
}

int ucp_ep_config_is_equal(const ucp_ep_config_key_t *key1,
                           const ucp_ep_config_key_t *key2)
{
    ucp_lane_index_t lane;


    if ((key1->num_lanes        != key2->num_lanes) ||
        memcmp(key1->rma_lanes, key2->rma_lanes, sizeof(key1->rma_lanes)) ||
        memcmp(key1->amo_lanes, key2->amo_lanes, sizeof(key1->amo_lanes)) ||
        (key1->reachable_md_map != key2->reachable_md_map) ||
        (key1->am_lane          != key2->am_lane) ||
        (key1->rndv_lane        != key2->rndv_lane) ||
        (key1->wireup_lane      != key2->wireup_lane))
    {
        return 0;
    }

    for (lane = 0; lane < key1->num_lanes; ++lane) {
        if ((key1->lanes[lane].rsc_index != key2->lanes[lane].rsc_index) ||
            (key1->lanes[lane].dst_md_index != key2->lanes[lane].dst_md_index))
        {
            return 0;
        }
    }

    return 1;
}

static size_t ucp_ep_config_calc_rndv_thresh(ucp_context_h context,
                                             uct_iface_attr_t *iface_attr,
                                             uct_md_attr_t *md_attr,
                                             size_t bcopy_bw, int recv_reg_cost)
{
    double numerator, denumerator;
    double diff_percent = 1.0 - context->config.ext.rndv_perf_diff / 100.0;

    /* We calculate the Rendezvous threshold by finding the message size at which:
     * AM/RMA rndv's latency is worse than the eager_zcopy
     * latency by a small percentage (that is set by the user).
     * Starting this message size (rndv_thresh), rndv may be used.
     *
     * The latency function for eager_zcopy is:
     * [ reg_cost.overhead + size * md_attr->reg_cost.growth +
     * max(size/bw , size/bcopy_bw) + overhead ]
     *
     * The latency function for Active message Rendezvous is:
     * [ latency + overhead + reg_cost.overhead +
     * size * md_attr->reg_cost.growth + overhead + latency +
     * max(size/bw , size/bcopy_bw) + latency + overhead + latency ]
     *
     * The latency function for RMA (get_zcopy) Rendezvous is:
     * [ reg_cost.overhead + size * md_attr->reg_cost.growth + latency + overhead +
     *   reg_cost.overhead + size * md_attr->reg_cost.growth + overhead + latency +
     *   size/bw + latency + overhead + latency ]
     *
     * Isolating the 'size' yields the rndv_thresh.
     * The used latency functions for eager_zcopy and rndv are also specified in
     * the UCX wiki */

    numerator = diff_percent * ((4 * ucp_tl_iface_latency(context, iface_attr)) +
                (3 * iface_attr->overhead) +
                (md_attr->reg_cost.overhead * (1 + recv_reg_cost))) -
                md_attr->reg_cost.overhead - iface_attr->overhead;

    denumerator = md_attr->reg_cost.growth +
                  ucs_max((1.0 / iface_attr->bandwidth), (1.0 / context->config.ext.bcopy_bw)) -
                  (diff_percent * (ucs_max((1.0 / iface_attr->bandwidth), (1.0 / bcopy_bw)) +
                  md_attr->reg_cost.growth * (1 + recv_reg_cost)));

    if ((numerator > 0) && (denumerator > 0)) {
        return (numerator / denumerator);
    } else {
        return context->config.ext.rndv_thresh_fallback;
    }
}

static void ucp_ep_config_set_am_rndv_thresh(ucp_context_h context, uct_iface_attr_t *iface_attr,
                                             uct_md_attr_t *md_attr, ucp_ep_config_t *config)
{
    size_t rndv_thresh;


    ucs_assert(config->key.am_lane != UCP_NULL_LANE);
    ucs_assert(config->key.lanes[config->key.am_lane].rsc_index != UCP_NULL_RESOURCE);

    if (context->config.ext.rndv_thresh == UCS_CONFIG_MEMUNITS_AUTO) {
        /* auto - Make UCX calculate the AM rndv threshold on its own.*/
        rndv_thresh = ucp_ep_config_calc_rndv_thresh(context, iface_attr, md_attr,
                                                     context->config.ext.bcopy_bw,
                                                     0);
        ucs_trace("Active Message rendezvous threshold is %zu", rndv_thresh);
    } else {
        rndv_thresh = context->config.ext.rndv_thresh;
    }

    ucs_assert(iface_attr->cap.am.min_zcopy <= iface_attr->cap.am.max_zcopy);
    /* use rendezvous only starting from minimal zero-copy am size */
    rndv_thresh = ucs_max(rndv_thresh, iface_attr->cap.am.min_zcopy);
    config->rndv.am_thresh = rndv_thresh;
}

void ucp_ep_config_init(ucp_worker_h worker, ucp_ep_config_t *config)
{
    ucp_context_h context = worker->context;
    ucp_ep_rma_config_t *rma_config;
    uct_iface_attr_t *iface_attr;
    ucp_rsc_index_t rsc_index;
    uct_md_attr_t *md_attr;
    ucp_lane_index_t lane;
    size_t zcopy_thresh, rndv_thresh, it;

    /* Default settings */
    for (it = 0; it < UCP_MAX_IOV; ++it) {
        config->am.zcopy_thresh[it]       = SIZE_MAX;
        config->am.sync_zcopy_thresh[it]  = SIZE_MAX;
    }
    config->am.zcopy_auto_thresh  = 0;
    config->bcopy_thresh          = context->config.ext.bcopy_thresh;
    config->rndv.rma_thresh       = SIZE_MAX;
    config->rndv.max_get_zcopy    = SIZE_MAX;
    config->rndv.am_thresh        = SIZE_MAX;
    config->p2p_lanes             = 0;

    /* Collect p2p lanes */
    for (lane = 0; lane < config->key.num_lanes; ++lane) {
        rsc_index   = config->key.lanes[lane].rsc_index;
        if ((rsc_index != UCP_NULL_RESOURCE) &&
            ucp_worker_is_tl_p2p(worker, rsc_index))
        {
            config->p2p_lanes |= UCS_BIT(lane);
        }
    }

    /* Configuration for active messages */
    if (config->key.am_lane != UCP_NULL_LANE) {
        lane        = config->key.am_lane;
        rsc_index   = config->key.lanes[lane].rsc_index;
        if (rsc_index != UCP_NULL_RESOURCE) {
            iface_attr  = &worker->iface_attrs[rsc_index];
            md_attr     = &context->tl_mds[context->tl_rscs[rsc_index].md_index].attr;

            if (iface_attr->cap.flags & UCT_IFACE_FLAG_AM_SHORT) {
                config->am.max_eager_short = iface_attr->cap.am.max_short -
                                             sizeof(ucp_eager_hdr_t);
                config->am.max_short       = iface_attr->cap.am.max_short -
                                             sizeof(uint64_t);
            } else {
                config->am.max_eager_short = -1;
                config->am.max_short       = -1;
            }

            if (iface_attr->cap.flags & UCT_IFACE_FLAG_AM_BCOPY) {
                config->am.max_bcopy = iface_attr->cap.am.max_bcopy;
            }

            if ((iface_attr->cap.flags & UCT_IFACE_FLAG_AM_ZCOPY) &&
                (md_attr->cap.flags & UCT_MD_FLAG_REG))
            {
                config->am.max_zcopy  = iface_attr->cap.am.max_zcopy;
                config->am.max_iovcnt = ucs_min(UCP_MAX_IOV, iface_attr->cap.am.max_iov);

                if (context->config.ext.zcopy_thresh == UCS_CONFIG_MEMUNITS_AUTO) {
                    /* auto */
                    config->am.zcopy_auto_thresh = 1;
                    for (it = 0; it < UCP_MAX_IOV; ++it) {
                        zcopy_thresh = ucp_ep_config_get_zcopy_auto_thresh(
                                           it + 1, &md_attr->reg_cost, context,
                                           iface_attr->bandwidth);
                        config->am.sync_zcopy_thresh[it] = zcopy_thresh;
                        config->am.zcopy_thresh[it]      = ucs_max(zcopy_thresh,
                                                                   iface_attr->cap.am.min_zcopy);
                    }
                } else {
                    config->am.sync_zcopy_thresh[0] = context->config.ext.zcopy_thresh;
                    config->am.zcopy_thresh[0]      = ucs_max(context->config.ext.zcopy_thresh,
                                                              iface_attr->cap.am.min_zcopy);
                }
            }

            /* calculate an rndv threshold for AM Rendezvous */
            ucp_ep_config_set_am_rndv_thresh(context, iface_attr, md_attr, config);
        } else {
            config->am.max_bcopy = UCP_MIN_BCOPY; /* Stub endpoint */
        }
    }

    /* Configuration for remote memory access */
    for (lane = 0; lane < config->key.num_lanes; ++lane) {
        if (ucp_ep_config_get_rma_prio(config->key.rma_lanes, lane) == -1) {
            continue;
        }

        rma_config = &config->rma[lane];
        rsc_index  = config->key.lanes[lane].rsc_index;
        iface_attr = &worker->iface_attrs[rsc_index];

        rma_config->put_zcopy_thresh = SIZE_MAX;
        rma_config->get_zcopy_thresh = SIZE_MAX;

        if (rsc_index != UCP_NULL_RESOURCE) {
            if (iface_attr->cap.flags & UCT_IFACE_FLAG_PUT_SHORT) {
                rma_config->max_put_short = iface_attr->cap.put.max_short;
            }
            if (iface_attr->cap.flags & UCT_IFACE_FLAG_PUT_BCOPY) {
                rma_config->max_put_bcopy = iface_attr->cap.put.max_bcopy;
            }
            if (iface_attr->cap.flags & UCT_IFACE_FLAG_PUT_ZCOPY) {
                rma_config->max_put_zcopy    = iface_attr->cap.put.max_zcopy;
                /* TODO: formula */
                if (context->config.ext.zcopy_thresh == UCS_CONFIG_MEMUNITS_AUTO) {
                    rma_config->put_zcopy_thresh = 16384; 
                } else {
                    rma_config->put_zcopy_thresh = context->config.ext.zcopy_thresh; 
                }
                rma_config->put_zcopy_thresh = ucs_max(rma_config->put_zcopy_thresh,
                                                       iface_attr->cap.put.min_zcopy);
            }
            if (iface_attr->cap.flags & UCT_IFACE_FLAG_GET_BCOPY) {
                rma_config->max_get_bcopy = iface_attr->cap.get.max_bcopy;
            }
            if (iface_attr->cap.flags & UCT_IFACE_FLAG_GET_ZCOPY) {
                /* TODO: formula */
                rma_config->max_get_zcopy = iface_attr->cap.get.max_zcopy;
                if (context->config.ext.zcopy_thresh == UCS_CONFIG_MEMUNITS_AUTO) {
                    rma_config->get_zcopy_thresh = 16384; 
                } else {
                    rma_config->get_zcopy_thresh = context->config.ext.zcopy_thresh; 
                }
                rma_config->get_zcopy_thresh = ucs_max(rma_config->get_zcopy_thresh,
                                                       iface_attr->cap.get.min_zcopy);
            }
        } else {
            rma_config->max_put_bcopy = UCP_MIN_BCOPY; /* Stub endpoint */
        }
    }

    /* Configuration for Rendezvous data */
    if (config->key.rndv_lane != UCP_NULL_LANE) {
        lane        = config->key.rndv_lane;
        rsc_index   = config->key.lanes[lane].rsc_index;
        if (rsc_index != UCP_NULL_RESOURCE) {
            iface_attr = &worker->iface_attrs[rsc_index];
            md_attr    = &context->tl_mds[context->tl_rscs[rsc_index].md_index].attr;
            ucs_assert_always(iface_attr->cap.flags & UCT_IFACE_FLAG_GET_ZCOPY);

            if (context->config.ext.rndv_thresh == UCS_CONFIG_MEMUNITS_AUTO) {
                /* auto - Make UCX calculate the RMA (get_zcopy) rndv threshold on its own.*/
                rndv_thresh = ucp_ep_config_calc_rndv_thresh(context, iface_attr, md_attr,
                                                             SIZE_MAX,
                                                             1);
            } else {
                /* In order to disable rendezvous, need to set the threshold to
                 * infinite (-1).
                 */
                rndv_thresh = context->config.ext.rndv_thresh;
            }

            /* use rendezvous only starting from minimal zero-copy get size */
            ucs_assert(iface_attr->cap.get.min_zcopy <= iface_attr->cap.get.max_zcopy);
            rndv_thresh                = ucs_max(rndv_thresh,
                                                 iface_attr->cap.get.min_zcopy);

            config->rndv.max_get_zcopy = iface_attr->cap.get.max_zcopy;
            config->rndv.rma_thresh    = rndv_thresh;
        } else {
            ucs_debug("rendezvous (get_zcopy) protocol is not supported ");
        }
    }
}

static void ucp_ep_config_print_tag_proto(FILE *stream, const char *name,
                                          size_t max_eager_short,
                                          size_t zcopy_thresh,
                                          size_t rndv_rma_thresh,
                                          size_t rndv_am_thresh)
{
    size_t max_bcopy, min_rndv;

    fprintf(stream, "# %23s: 0", name);
    if (max_eager_short > 0) {
        fprintf(stream, "..<egr/short>..%zu" , max_eager_short + 1);
    }

    min_rndv  = ucs_min(rndv_rma_thresh, rndv_am_thresh);
    max_bcopy = ucs_min(zcopy_thresh, min_rndv);
    if (max_eager_short < max_bcopy) {
        fprintf(stream, "..<egr/bcopy>..");
        if (max_bcopy < SIZE_MAX) {
            fprintf(stream, "%zu", max_bcopy);
        }
    }
    if (zcopy_thresh < min_rndv) {
        fprintf(stream, "..<egr/zcopy>..");
        if (min_rndv < SIZE_MAX) {
            fprintf(stream, "%zu", min_rndv);
        }
    }

    if (min_rndv < SIZE_MAX) {
        fprintf(stream, "..<rndv>..");
    }
    fprintf(stream, "(inf)\n");
}

static void ucp_ep_config_print_rma_proto(FILE *stream, const char *name,
                                          ucp_lane_index_t lane,
                                          size_t bcopy_thresh, size_t zcopy_thresh)
{

    fprintf(stream, "# %20s[%d]: 0", name, lane);
    if (bcopy_thresh > 0) {
        fprintf(stream, "..<short>");
    }
    if (bcopy_thresh < zcopy_thresh) {
        if (bcopy_thresh > 0) {
            fprintf(stream, "..%zu", bcopy_thresh);
        }
        fprintf(stream, "..<bcopy>");
    }
    if (zcopy_thresh < SIZE_MAX) {
        fprintf(stream, "..%zu..<zcopy>", zcopy_thresh);
    }
    fprintf(stream, "..(inf)\n");
}

int ucp_ep_config_get_rma_prio(const ucp_lane_index_t *lanes,
                               ucp_lane_index_t lane)
{
    int prio;
    for (prio = 0; prio < UCP_MAX_LANES; ++prio) {
        if (lane == lanes[prio]) {
            return prio;
        }
    }
    return -1;
}

void ucp_ep_config_lane_info_str(ucp_context_h context,
                                 const ucp_ep_config_key_t *key,
                                 const uint8_t *addr_indices,
                                 ucp_lane_index_t lane,
                                 ucp_rsc_index_t aux_rsc_index,
                                 char *buf, size_t max)
{
    uct_tl_resource_desc_t *rsc;
    ucp_rsc_index_t rsc_index;
    char *p, *endp;
    int prio;

    p         = buf;
    endp      = buf + max;
    rsc_index = key->lanes[lane].rsc_index;

    rsc = &context->tl_rscs[rsc_index].tl_rsc;
    snprintf(p, endp - p, "lane[%d]: %d:" UCT_TL_RESOURCE_DESC_FMT "%-*c-> ",
             lane, rsc_index, UCT_TL_RESOURCE_DESC_ARG(rsc),
             20 - (int)(strlen(rsc->dev_name) + strlen(rsc->tl_name)), ' ');
    p += strlen(p);

    if (addr_indices != NULL) {
        snprintf(p, endp - p, "addr[%d].", addr_indices[lane]);
        p += strlen(p);
    }

    snprintf(p, endp - p, "md[%d]", key->lanes[lane].dst_md_index);
    p += strlen(p);

    prio = ucp_ep_config_get_rma_prio(key->rma_lanes, lane);
    if (prio != -1) {
        snprintf(p, endp - p, " rma#%d", prio);
        p += strlen(p);
    }

    prio = ucp_ep_config_get_rma_prio(key->amo_lanes, lane);
    if (prio != -1) {
        snprintf(p, endp - p, " amo#%d", prio);
        p += strlen(p);
    }

    if (key->am_lane == lane) {
        snprintf(p, endp - p, " am");
        p += strlen(p);
    }

    if (lane == key->rndv_lane) {
        snprintf(p, endp - p, " zcopy_rndv");
        p += strlen(p);
    }

    if (key->wireup_lane == lane) {
        snprintf(p, endp - p, " wireup");
        p += strlen(p);
        if (aux_rsc_index != UCP_NULL_RESOURCE) {
            snprintf(p, endp - p, "{" UCT_TL_RESOURCE_DESC_FMT "}",
                     UCT_TL_RESOURCE_DESC_ARG(&context->tl_rscs[aux_rsc_index].tl_rsc));
            p += strlen(p);
        }
    }
}

static void ucp_ep_config_print(FILE *stream, ucp_worker_h worker,
                                const ucp_ep_config_t *config,
                                const uint8_t *addr_indices,
                                ucp_rsc_index_t aux_rsc_index)
{
    ucp_context_h context   = worker->context;
    char lane_info[128] = {0};
    ucp_lane_index_t lane;

    for (lane = 0; lane < config->key.num_lanes; ++lane) {
        ucp_ep_config_lane_info_str(context, &config->key, addr_indices, lane,
                                    aux_rsc_index, lane_info, sizeof(lane_info));
        fprintf(stream, "#                 %s\n", lane_info);
    }
    fprintf(stream, "#\n");

    if (context->config.features & UCP_FEATURE_TAG) {
         ucp_ep_config_print_tag_proto(stream, "tag_send",
                                       config->am.max_eager_short,
                                       config->am.zcopy_thresh[0],
                                       config->rndv.rma_thresh,
                                       config->rndv.am_thresh);
         ucp_ep_config_print_tag_proto(stream, "tag_send_sync",
                                       config->am.max_eager_short,
                                       config->am.sync_zcopy_thresh[0],
                                       config->rndv.rma_thresh,
                                       config->rndv.am_thresh);
     }

     if (context->config.features & UCP_FEATURE_RMA) {
         for (lane = 0; lane < config->key.num_lanes; ++lane) {
             if (ucp_ep_config_get_rma_prio(config->key.rma_lanes, lane) == -1) {
                 continue;
             }
             ucp_ep_config_print_rma_proto(stream, "put", lane,
                                           ucs_max(config->rma[lane].max_put_short + 1,
                                                   config->bcopy_thresh),
                                           config->rma[lane].put_zcopy_thresh);
             ucp_ep_config_print_rma_proto(stream, "get", lane, 0,
                                           config->rma[lane].get_zcopy_thresh);
         }
     }

}

void ucp_ep_print_info(ucp_ep_h ep, FILE *stream)
{
    ucp_rsc_index_t aux_rsc_index;
    uct_ep_h wireup_ep;

    UCP_THREAD_CS_ENTER_CONDITIONAL(&ep->worker->mt_lock);

    fprintf(stream, "#\n");
    fprintf(stream, "# UCP endpoint\n");
    fprintf(stream, "#\n");

    fprintf(stream, "#               peer: %s%suuid 0x%"PRIx64"\n",
#if ENABLE_DEBUG_DATA
            ucp_ep_peer_name(ep), ", ",
#else
            "", "",
#endif
            ep->dest_uuid);

    wireup_ep = ep->uct_eps[ucp_ep_get_wireup_msg_lane(ep)];
    if (ucp_stub_ep_test(wireup_ep)) {
        aux_rsc_index = ucp_stub_ep_get_aux_rsc_index(wireup_ep);
    } else {
        aux_rsc_index = UCP_NULL_RESOURCE;
    }

    ucp_ep_config_print(stream, ep->worker, ucp_ep_config(ep), NULL,
                        aux_rsc_index);

    fprintf(stream, "#\n");

    UCP_THREAD_CS_EXIT_CONDITIONAL(&ep->worker->mt_lock);
}

size_t ucp_ep_config_get_zcopy_auto_thresh(size_t iovcnt,
                                           const uct_linear_growth_t *reg_cost,
                                           const ucp_context_h context,
                                           double bandwidth)
{
    double zcopy_thresh;
    double bcopy_bw = context->config.ext.bcopy_bw;

    zcopy_thresh = (iovcnt * reg_cost->overhead) /
                   ((1.0 / bcopy_bw) - (1.0 / bandwidth) - (iovcnt * reg_cost->growth));

    if ((zcopy_thresh < 0.0) || (zcopy_thresh > SIZE_MAX)) {
        return SIZE_MAX;
    }

    return zcopy_thresh;
}
