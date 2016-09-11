/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2014.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifndef UCT_DEF_H_
#define UCT_DEF_H_

#include <ucs/sys/math.h>
#include <ucs/type/status.h>
#include <stdint.h>


#define UCT_TL_NAME_MAX          10
#define UCT_MD_COMPONENT_NAME_MAX  8
#define UCT_MD_NAME_MAX          16
#define UCT_DEVICE_NAME_MAX      32
#define UCT_PENDING_REQ_PRIV_LEN 32
#define UCT_AM_ID_BITS           5
#define UCT_AM_ID_MAX            UCS_BIT(UCT_AM_ID_BITS)
#define UCT_INVALID_MEM_HANDLE   NULL
#define UCT_INVALID_RKEY         ((uintptr_t)(-1))
#define UCT_INLINE_API           static UCS_F_ALWAYS_INLINE


/**
 * @ingroup UCT_RESOURCE
 * @brief  List of event types for interrupt notification.
 */
enum uct_event_types {
    UCP_EVENT_TX_COMPLETION = UCS_BIT(0),
    UCP_EVENT_TX_RESOURCES  = UCS_BIT(1),
    UCP_EVENT_RX_COMPLETION = UCS_BIT(2),
    UCP_EVENT_RX_RESOURCES  = UCS_BIT(3),
    UCP_EVENT_TX_ERROR      = UCS_BIT(4),
    UCP_EVENT_RX_ERROR      = UCS_BIT(5),
};


/**
 * @ingroup UCT_AM
 * @brief Trace types for active message tracer.
 */
enum uct_am_trace_type {
    UCT_AM_TRACE_TYPE_SEND,
    UCT_AM_TRACE_TYPE_RECV,
    UCT_AM_TRACE_TYPE_SEND_DROP,
    UCT_AM_TRACE_TYPE_RECV_DROP,
    UCT_AM_TRACE_TYPE_LAST
};


/**
 * @addtogroup UCT_RESOURCE
 * @{
 */
typedef struct uct_iface         *uct_iface_h;
typedef struct uct_wakeup        *uct_wakeup_h;
typedef struct uct_iface_config  uct_iface_config_t;
typedef struct uct_md_config     uct_md_config_t;
typedef struct uct_ep            *uct_ep_h;
typedef void *                   uct_mem_h;
typedef uintptr_t                uct_rkey_t;
typedef struct uct_md            *uct_md_h;          /**< @brief Memory domain handler */
typedef struct uct_md_ops        uct_md_ops_t;
typedef void                     *uct_rkey_ctx_h;
typedef struct uct_iface_attr    uct_iface_attr_t;
typedef struct uct_md_attr       uct_md_attr_t;
typedef struct uct_completion    uct_completion_t;
typedef struct uct_pending_req   uct_pending_req_t;
typedef struct uct_worker        *uct_worker_h;
typedef struct uct_md            uct_md_t;
typedef enum uct_am_trace_type   uct_am_trace_type_t;
typedef struct uct_device_addr   uct_device_addr_t;
typedef struct uct_iface_addr    uct_iface_addr_t;
typedef struct uct_ep_addr       uct_ep_addr_t;
/**
 * @}
 */


/**
 * @ingroup UCT_RESOURCE
 * @brief Structure for scatter-gather I/O.
 *
 * Specifies a list of buffers which can be used within a single data transfer
 * function call.
 *
 * @note If the @a length is zero, the @a buffer will not be touched.
 *       If the @a length greater than zero, the @a buffer must contain valid
 *       pointer and @a count must be greater than zero.
 *
 */
typedef struct uct_iov {
    void     *buffer;   /**< Data buffer */
    size_t    length;   /**< Length of data */
    uct_mem_h memh;     /**< Local memory key descriptor for the data */
    size_t    stride;   /**< Stride of the elements in the buffer */
    unsigned  count;    /**< Number of elements in the buffer */
} uct_iov_t;


/**
 * @ingroup UCT_AM
 * @brief Callback to process incoming active message
 *
 * When the callback is called, @a desc does not necessarily contain the payload.
 * In this case, @a data would not point inside @a desc, and user may want
 * copy the payload from @a data to @a desc before returning @ref UCS_INPROGRESS
 * (it's guaranteed @a desc has enough room to hold the payload).
 *
 * @param [in]  arg      User-defined argument.
 * @param [in]  data     Points to the received data.
 * @param [in]  length   Length of data.
 * @param [in]  desc     Points to the received descriptor, at the beginning of
 *                       the user-defined rx_headroom.
 *
 * @note This callback could be set and released
 *       by @ref uct_iface_set_am_handler function.

 * @warning If the user became the owner of the @a desc (by returning
 *          @ref UCS_INPROGRESS) the descriptor must be released later by
 *          @ref uct_iface_release_am_desc by the user.
 *
 * @retval UCS_OK         - descriptor was consumed, and can be released
 *                          by the caller.
 * @retval UCS_INPROGRESS - descriptor is owned by the callee, and would be
 *                          released later.
 *
 */
typedef ucs_status_t (*uct_am_callback_t)(void *arg, void *data, size_t length,
                                          void *desc);


/**
 * @ingroup UCT_AM
 * @brief Callback to trace active messages.
 *
 * Writes a string which represents active message contents into 'buffer'.
 *
 * @param [in]  arg      User-defined argument.
 * @param [in]  type     Message type.
 * @param [in]  id       Active message id.
 * @param [in]  data     Points to the received data.
 * @param [in]  length   Length of data.
 * @param [out] buffer   Filled with a debug information string.
 * @param [in]  max      Maximal length of the string.
 */
typedef void (*uct_am_tracer_t)(void *arg, uct_am_trace_type_t type, uint8_t id,
                                const void *data, size_t length, char *buffer,
                                size_t max);


/**
 * @ingroup UCT_RESOURCE
 * @brief Callback to process send completion.
 *
 * @param [in]  self     Pointer to relevant completion structure, which was
 *                       initially passed to the operation.
 * @param [in]  status   Status of send action, possibly indicating an error.
 */
typedef void (*uct_completion_callback_t)(uct_completion_t *self,
                                          ucs_status_t status);


/**
 * @ingroup UCT_RESOURCE
 * @brief Callback to process pending requests.
 *
 * @param [in]  self     Pointer to relevant pending structure, which was
 *                       initially passed to the operation.
 *
 * @return @ref UCS_OK         - This pending request has completed and
 *                               should be removed.
 *         @ref UCS_INPROGRESS - Some progress was made, but not completed.
 *                               Keep this request and keep processing the queue.
 *         Otherwise           - Could not make any progress. Keep this pending
 *                               request on the queue, and stop processing the queue.
 */
typedef ucs_status_t (*uct_pending_callback_t)(uct_pending_req_t *self);

/**
 * @ingroup UCT_RESOURCE
 * @brief Callback to purge pending requests.
 *
 * @param [in]  self     Pointer to relevant pending structure, which was
 *                       initially passed to the operation.
 * @param [in]  arg      User argument to be passed to the callback.
 */
typedef void (*uct_pending_purge_callback_t)(uct_pending_req_t *self,
                                             void *arg);

/**
 * @ingroup UCT_RESOURCE
 * @brief Callback for producing data.
 *
 * @param [in]  dest     Memory buffer to pack the data to.
 * @param [in]  arg      Custom user-argument.
 *
 * @return  Size of the data was actually produced.
 */
typedef size_t (*uct_pack_callback_t)(void *dest, void *arg);


/**
 * @ingroup UCT_RESOURCE
 * @brief Callback for consuming data.
 *
 * @param [in]  arg      Custom user-argument.
 * @param [in]  data     Memory buffer to unpack the data from.
 * @param [in]  length   How much data to consume (size of "data")
 *
 * @note The arguments for this callback are in the same order as libc's memcpy().
 */
typedef void (*uct_unpack_callback_t)(void *arg, const void *data, size_t length);


#endif
