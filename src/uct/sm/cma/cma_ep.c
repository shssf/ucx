/**
* Copyright (C) UT-Battelle, LLC. 2015. ALL RIGHTS RESERVED.
* Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
* Copyright (C) ARM Ltd. 2016.  ALL RIGHTS RESERVED.
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif
#include <sys/uio.h>

#include "cma_ep.h"
#include <ucs/debug/log.h>


static UCS_CLASS_INIT_FUNC(uct_cma_ep_t, uct_iface_t *tl_iface,
                           const uct_device_addr_t *dev_addr,
                           const uct_iface_addr_t *iface_addr)
{
    uct_cma_iface_t *iface = ucs_derived_of(tl_iface, uct_cma_iface_t);

    UCS_CLASS_CALL_SUPER_INIT(uct_base_ep_t, &iface->super);
    self->remote_pid = *(const pid_t*)iface_addr;
    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_cma_ep_t)
{
    /* No op */
}

UCS_CLASS_DEFINE(uct_cma_ep_t, uct_base_ep_t)
UCS_CLASS_DEFINE_NEW_FUNC(uct_cma_ep_t, uct_ep_t, uct_iface_t*,
                          const uct_device_addr_t *, const uct_iface_addr_t *);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_cma_ep_t, uct_ep_t);


#define uct_cma_trace_data(_remote_addr, _rkey, _fmt, ...) \
     ucs_trace_data(_fmt " to %"PRIx64"(%+ld)", ## __VA_ARGS__, (_remote_addr), \
                    (_rkey))

static inline ucs_status_t uct_cma_ep_common_zcopy(uct_ep_h tl_ep,
                                                   const void *buffer,
                                                   size_t length,
                                                   uct_mem_h memh,
                                                   uint64_t remote_addr,
                                                   uct_rkey_t rkey,
                                                   uct_completion_t *comp,
                                                   ssize_t (*fn_p)(pid_t,
                                                                   const struct iovec *,
                                                                   unsigned long,
                                                                   const struct iovec *,
                                                                   unsigned long,
                                                                   unsigned long),
                                                   char *fn_name)
{
    ssize_t ret;
    ssize_t delivered = 0;
    struct iovec local_iov;
    struct iovec remote_iov;
    uct_cma_ep_t *ep = ucs_derived_of(tl_ep, uct_cma_ep_t);

    do {
        local_iov.iov_base  = (void *)((char *)buffer + delivered);
        remote_iov.iov_base = (void *)(remote_addr + delivered);
        local_iov.iov_len   = remote_iov.iov_len  = length - delivered;
        ret = fn_p(ep->remote_pid, &local_iov, 1, &remote_iov, 1, 0);
        if (ret < 0) {
            ucs_error("%s delivered %zu instead of %zu, error message %s",
                      fn_name, delivered, length, strerror(errno));
            return UCS_ERR_IO_ERROR;
        }
        delivered += ret;
    } while (delivered < length);

    return UCS_OK;
}

ucs_status_t uct_cma_ep_put_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov, size_t iovlen,
                                  uint64_t remote_addr, uct_rkey_t rkey,
                                  uct_completion_t *comp)
{
    UCT_CHECK_PARAM_IOV(iov, iovlen, buffer, length, memh);

    int ret = uct_cma_ep_common_zcopy(tl_ep,
                                      buffer,
                                      length,
                                      memh,
                                      remote_addr,
                                      rkey,
                                      comp,
                                      process_vm_writev,
                                      "process_vm_writev");

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), PUT, ZCOPY, length);
    uct_cma_trace_data(remote_addr, rkey, "PUT_ZCOPY [length %zu]", length);
    return ret;
}

ucs_status_t uct_cma_ep_get_zcopy(uct_ep_h tl_ep, const uct_iov_t *iov, size_t iovlen,
                                   uint64_t remote_addr, uct_rkey_t rkey,
                                   uct_completion_t *comp)
{
    UCT_CHECK_PARAM_IOV(iov, iovlen, buffer, length, memh);

    int ret = uct_cma_ep_common_zcopy(tl_ep,
                                      buffer,
                                      length,
                                      memh,
                                      remote_addr,
                                      rkey,
                                      comp,
                                      process_vm_readv,
                                      "process_vm_readv");

    UCT_TL_EP_STAT_OP(ucs_derived_of(tl_ep, uct_base_ep_t), GET, ZCOPY, length);
    uct_cma_trace_data(remote_addr, rkey, "GET_ZCOPY [length %zu]", length);
    return ret;
}
