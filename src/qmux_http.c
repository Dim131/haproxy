#include <haproxy/qmux_http.h>

#include <haproxy/api-t.h>
#include <haproxy/htx.h>
#include <haproxy/qmux_trace.h>

/* QUIC MUX rcv_buf operation using HTX data. Received data from stream <qcs>
 * will be transferred as HTX in <buf>. Output buffer is expected to be of
 * length <count>. <fin> will be set to signal the last data to receive on this
 * stream.
 *
 * Return the size in bytes of transferred data.
 */
size_t qcs_http_rcv_buf(struct qcs *qcs, struct buffer *buf, size_t count,
                        char *fin)
{
	struct htx *qcs_htx = NULL;
	struct htx *cs_htx = NULL;
	size_t ret = 0;

	TRACE_ENTER(QMUX_EV_STRM_RECV, qcs->qcc->conn, qcs);

	*fin = 0;
	qcs_htx = htx_from_buf(&qcs->rx.app_buf);
	if (htx_is_empty(qcs_htx)) {
		/* Set buffer data to 0 as HTX is empty. */
		htx_to_buf(qcs_htx, &qcs->rx.app_buf);
		goto end;
	}

	ret = qcs_htx->data;

	cs_htx = htx_from_buf(buf);
	if (htx_is_empty(cs_htx) && htx_used_space(qcs_htx) <= count) {
		/* EOM will be copied to cs_htx via b_xfer(). */
		if (qcs_htx->flags & HTX_FL_EOM)
			*fin = 1;

		htx_to_buf(cs_htx, buf);
		htx_to_buf(qcs_htx, &qcs->rx.app_buf);
		b_xfer(buf, &qcs->rx.app_buf, b_data(&qcs->rx.app_buf));
		goto end;
	}

	htx_xfer_blks(cs_htx, qcs_htx, count, HTX_BLK_UNUSED);
	BUG_ON(qcs_htx->flags & HTX_FL_PARSING_ERROR);

	/* Copy EOM from src to dst buffer if all data copied. */
	if (htx_is_empty(qcs_htx) && (qcs_htx->flags & HTX_FL_EOM)) {
		cs_htx->flags |= HTX_FL_EOM;
		*fin = 1;
	}

	cs_htx->extra = qcs_htx->extra ? (qcs_htx->data + qcs_htx->extra) : 0;
	htx_to_buf(cs_htx, buf);
	htx_to_buf(qcs_htx, &qcs->rx.app_buf);
	ret -= qcs_htx->data;

 end:
	TRACE_LEAVE(QMUX_EV_STRM_RECV, qcs->qcc->conn, qcs);

	return ret;
}

/* QUIC MUX snd_buf operation using HTX data. HTX data will be transferred from
 * <buf> to <qcs> stream buffer. Input buffer is expected to be of length
 * <count>. <fin> will be set to signal the last data to send for this stream.
 *
 * Return the size in bytes of transferred data.
 */
size_t qcs_http_snd_buf(struct qcs *qcs, struct buffer *buf, size_t count,
                        char *fin)
{
	struct htx *htx;
	size_t ret;

	TRACE_ENTER(QMUX_EV_STRM_SEND, qcs->qcc->conn, qcs);

	htx = htx_from_buf(buf);

	if (htx->extra && htx->extra == HTX_UNKOWN_PAYLOAD_LENGTH)
		qcs->flags |= QC_SF_UNKNOWN_PL_LENGTH;

	ret = qcs->qcc->app_ops->snd_buf(qcs, htx, count);
	*fin = (htx->flags & HTX_FL_EOM) && htx_is_empty(htx);

	htx_to_buf(htx, buf);

	TRACE_LEAVE(QMUX_EV_STRM_SEND, qcs->qcc->conn, qcs);

	return ret;
}

/* QUIC MUX snd_buf reset. HTX data stored in <buf> of length <count> will be
 * cleared. This can be used when data should not be transmitted any longer.
 *
 * Return the size in bytes of cleared data.
 */
size_t qcs_http_reset_buf(struct qcs *qcs, struct buffer *buf, size_t count)
{
	struct htx *htx;

	TRACE_ENTER(QMUX_EV_STRM_SEND, qcs->qcc->conn, qcs);

	htx = htx_from_buf(buf);
	htx_reset(htx);
	htx_to_buf(htx, buf);

	TRACE_LEAVE(QMUX_EV_STRM_SEND, qcs->qcc->conn, qcs);

	return count;
}

/* Utility function which can be used by app layer an empty STREAM frame is
 * received with FIN bit set for <qcs> stream. It will ensure that HTX EOM is
 * properly inserted in <qcs> app_buf.
 */
void qcs_http_handle_standalone_fin(struct qcs *qcs)
{
	struct buffer *appbuf;
	struct htx *htx = NULL;

	appbuf = qc_get_buf(qcs, &qcs->rx.app_buf);
	BUG_ON(!appbuf);

	htx = htx_from_buf(appbuf);
	if (htx_is_empty(htx)) {
		if (!htx_add_endof(htx, HTX_BLK_EOT)) {
			ABORT_NOW(); /* cannot happen for empty HTX message. */
		}
	}
	htx->flags |= HTX_FL_EOM;
	htx_to_buf(htx, appbuf);
}
