/*
 * Copyright (c) 2018 Yubico AB. All rights reserved.
 * Use of this source code is governed by a BSD-style
 * license that can be found in the LICENSE file.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fido.h"
#include "packed.h"

PACKED_TYPE(frame_t,
struct frame {
	uint32_t cid; /* channel id */
	union {
		uint8_t type;
		struct {
			uint8_t cmd;
			uint8_t bcnth;
			uint8_t bcntl;
			uint8_t data[CTAP_RPT_SIZE - 7];
		} init;
		struct {
			uint8_t seq;
			uint8_t data[CTAP_RPT_SIZE - 5];
		} cont;
	} body;
})

#ifndef MIN
#define MIN(x, y) ((x) > (y) ? (y) : (x))
#endif

static int
tx_empty(fido_dev_t *d, uint8_t cmd)
{
	struct frame	*fp;
	unsigned char	 pkt[sizeof(*fp) + 1];
	int		 n;

	if (d->io.write == NULL)
		return (-1);

	memset(&pkt, 0, sizeof(pkt));
	fp = (struct frame *)(pkt + 1);
	fp->cid = d->cid;
	fp->body.init.cmd = CTAP_FRAME_INIT | cmd;

	n = d->io.write(d->io_handle, pkt, sizeof(pkt));
	if (n < 0 || (size_t)n != sizeof(pkt))
		return (-1);

	return (0);
}

static size_t
tx_preamble(fido_dev_t *d,  uint8_t cmd, const void *buf, size_t count)
{
	struct frame	*fp;
	unsigned char	 pkt[sizeof(*fp) + 1];
	int		 n;

	if (d->io.write == NULL || count == 0)
		return (0);

	memset(&pkt, 0, sizeof(pkt));
	fp = (struct frame *)(pkt + 1);
	fp->cid = d->cid;
	fp->body.init.cmd = CTAP_FRAME_INIT | cmd;
	fp->body.init.bcnth = (count >> 8) & 0xff;
	fp->body.init.bcntl = count & 0xff;
	count = MIN(count, sizeof(fp->body.init.data));
	memcpy(&fp->body.init.data, buf, count);

	n = d->io.write(d->io_handle, pkt, sizeof(pkt));
	if (n < 0 || (size_t)n != sizeof(pkt))
		return (0);

	return (count);
}

static size_t
tx_frame(fido_dev_t *d, uint8_t seq, const void *buf, size_t count)
{
	struct frame	*fp;
	unsigned char	 pkt[sizeof(*fp) + 1];
	int		 n;

	if (d->io.write == NULL || count == 0)
		return (0);

	memset(&pkt, 0, sizeof(pkt));
	fp = (struct frame *)(pkt + 1);
	fp->cid = d->cid;
	fp->body.cont.seq = seq;
	count = MIN(count, sizeof(fp->body.cont.data));
	memcpy(&fp->body.cont.data, buf, count);

	n = d->io.write(d->io_handle, pkt, sizeof(pkt));
	if (n < 0 || (size_t)n != sizeof(pkt))
		return (0);

	return (count);
}

static int
tx(fido_dev_t *d, uint8_t cmd, const unsigned char *buf, size_t count)
{
	uint8_t	seq = 0;
	size_t	sent;

	fido_log_debug("%s: d=%p, cmd=0x%02x, buf=%p, len=%zu", __func__,
	    (void *)d, cmd, (const void *)buf, count);
	fido_log_xxd(buf, count);

	if (d->io_handle == NULL || count > UINT16_MAX) {
		fido_log_debug("%s: invalid argument (%p, %zu)", __func__,
		    d->io_handle, count);
		return (-1);
	}

	if (count == 0)
		return (tx_empty(d, cmd));

	if ((sent = tx_preamble(d, cmd, buf, count)) == 0) {
		fido_log_debug("%s: tx_preamble", __func__);
		return (-1);
	}

	while (sent < count) {
		if (seq & 0x80) {
			fido_log_debug("%s: seq & 0x80", __func__);
			return (-1);
		}
		const uint8_t *p = (const uint8_t *)buf + sent;
		size_t n = tx_frame(d, seq++, p, count - sent);
		if (n == 0) {
			fido_log_debug("%s: tx_frame", __func__);
			return (-1);
		}
		sent += n;
	}

	return (0);
}

int
fido_tx(fido_dev_t *d, uint8_t cmd, const void *buf, size_t count)
{
	if (d->io.tx != NULL)
		return (d->io.tx(d, cmd, buf, count));

	return (tx(d, cmd, buf, count));
}

static int
rx_frame(fido_dev_t *d, struct frame *fp, int ms)
{
	int n;

	if (d->io.read == NULL)
		return (-1);

	n = d->io.read(d->io_handle, (unsigned char *)fp, sizeof(*fp), ms);
	if (n < 0 || (size_t)n != sizeof(*fp))
		return (-1);

	return (0);
}

static int
rx_preamble(fido_dev_t *d, struct frame *fp, int ms)
{
	do {
		if (rx_frame(d, fp, ms) < 0)
			return (-1);
#ifdef FIDO_FUZZ
		fp->cid = d->cid;
#endif
	} while (fp->cid == d->cid &&
	    fp->body.init.cmd == (CTAP_FRAME_INIT | CTAP_KEEPALIVE));

	return (0);
}

static int
rx(fido_dev_t *d, uint8_t cmd, unsigned char *buf, size_t count, int ms)
{
	struct frame	f;
	uint16_t	r;
	uint16_t	flen;
	int		seq;

	if (d->io_handle == NULL) {
		fido_log_debug("%s: invalid argument (%p)", __func__,
		    d->io_handle);
		return (-1);
	}

	if (rx_preamble(d, &f, ms) < 0) {
		fido_log_debug("%s: rx_preamble", __func__);
		return (-1);
	}
	flen = (f.body.init.bcnth << 8) | f.body.init.bcntl;	

	fido_log_debug("%s: initiation frame at %p, len %u", __func__,
	    (void *)&f, flen);
	fido_log_xxd(&f, sizeof(f));

#ifdef FIDO_FUZZ
	f.cid = d->cid;
	f.body.init.cmd = (CTAP_FRAME_INIT | cmd);
#endif

	if (f.cid != d->cid || f.body.init.cmd != (CTAP_FRAME_INIT | cmd)) {
		fido_log_debug("%s: cid (0x%x, 0x%x), cmd (0x%02x, 0x%02x)",
		    __func__, f.cid, d->cid, f.body.init.cmd, cmd);
		return (-1);
	}

	if (count < (size_t)flen) {
		fido_log_debug("%s: count < flen (%zu, %zu)", __func__, count,
		    (size_t)flen);
		return (-1);
	}
	if (flen < sizeof(f.body.init.data)) {
		memcpy(buf, f.body.init.data, flen);
		return (flen);
	}

	memcpy(buf, f.body.init.data, sizeof(f.body.init.data));
	r = sizeof(f.body.init.data);
	seq = 0;

	while ((size_t)r < flen) {
		if (rx_frame(d, &f, ms) < 0) {
			fido_log_debug("%s: rx_frame", __func__);
			return (-1);
		}

		fido_log_debug("%s: continuation frame at %p, len %zu",
		    __func__, (void *)&f, sizeof(f));
		fido_log_xxd(&f, sizeof(f));

#ifdef FIDO_FUZZ
		f.cid = d->cid;
		f.body.cont.seq = seq;
#endif

		if (f.cid != d->cid || f.body.cont.seq != seq++) {
			fido_log_debug("%s: cid (0x%x, 0x%x), seq (%d, %d)",
			    __func__, f.cid, d->cid, f.body.cont.seq, seq);
			return (-1);
		}

		uint8_t *p = (uint8_t *)buf + r;

		if ((size_t)(flen - r) > sizeof(f.body.cont.data)) {
			memcpy(p, f.body.cont.data, sizeof(f.body.cont.data));
			r += sizeof(f.body.cont.data);
		} else {
			memcpy(p, f.body.cont.data, flen - r);
			r += (flen - r); /* break */
		}
	}

	fido_log_debug("%s: buf=%p, len=%zu", __func__, (void *)buf, (size_t)r);
	fido_log_xxd(buf, r);

	return (r);
}

int
fido_rx(fido_dev_t *d, uint8_t cmd, void *buf, size_t count, int ms)
{
	if (d->io.rx != NULL)
		return (d->io.rx(d, cmd, buf, count, ms));

	return (rx(d, cmd, buf, count, ms));
}

int
fido_rx_cbor_status(fido_dev_t *d, int ms)
{
	const uint8_t	cmd = CTAP_CMD_CBOR;
	unsigned char	reply[2048];
	int		reply_len;

	if ((reply_len = fido_rx(d, cmd, &reply, sizeof(reply), ms)) < 0 ||
	    (size_t)reply_len < 1) {
		fido_log_debug("%s: fido_rx", __func__);
		return (FIDO_ERR_RX);
	}

	return (reply[0]);
}
