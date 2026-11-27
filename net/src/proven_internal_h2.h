#ifndef PROVEN_INTERNAL_H2_H
#define PROVEN_INTERNAL_H2_H

/* The HTTP/2 engine's DATA by reference (RFC-0053), for the event server's driver only: not in
 * h2.h (D-4). */

#include "proven/h2.h"

/* Memory lent to the engine. `returned` is called with each part the engine no longer needs -
 * written (proven_h2_conn_output_sent passed it) or dropped (proven_h2_conn_destroy) - and the
 * parts add up to what was lent. It may free the memory and the struct; it must not call the
 * engine. */
typedef struct proven_h2_lend_ {
    void (*returned)(struct proven_h2_lend_ *lend, proven_size_t n);
} proven_h2_lend_t;

/* proven_h2_conn_send_data, but the payload is not copied: it stays where it is, unchanged,
 * until `lend` has been given all of it back. The frame headers are queued as ever; what is
 * pending is then several pieces (proven_h2_conn_pending_vec_), and
 * proven_h2_conn_pending_output gives only the first. Lent bytes count against the output
 * bound as copied ones do (D-3). */
[[nodiscard]]
proven_result_size_t proven_h2_conn_send_data_ref_(proven_h2_conn_t *conn, proven_u32 stream_id, proven_mem_view_t data, bool end_stream,
                                                   proven_h2_lend_t *lend);

/* What is pending, in order, as up to `cap` pieces; the number filled. Good until the next call
 * that takes the connection. */
[[nodiscard]]
proven_size_t proven_h2_conn_pending_vec_(const proven_h2_conn_t *conn, proven_mem_view_t *parts, proven_size_t cap);

/* All the bytes pending, copied and lent. */
[[nodiscard]]
proven_size_t proven_h2_conn_pending_bytes_(const proven_h2_conn_t *conn);

#endif
