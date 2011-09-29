#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "kvldskey.h"
#include "mpool.h"
#include "sysendian.h"
#include "warnp.h"
#include "wire.h"

#include "proto_kvlds.h"

struct request_read {
	int (*callback)(void *, struct proto_kvlds_request *);
	void * cookie;
	void * read_cookie;
};

MPOOL(requestread, struct request_read, 16);
MPOOL(request, struct proto_kvlds_request, 4096);

static int gotpacket(void *, struct wire_packet *);
static int docallback(struct request_read *, struct proto_kvlds_request *);

/**
 * proto_kvlds_request_parse(P):
 * Parse the packet ${P} and return an LBS request structure.
 */
static struct proto_kvlds_request *
proto_kvlds_request_parse(const struct wire_packet * P)
{
	struct proto_kvlds_request * R;
	size_t bufpos;

	/* Allocate KVLDS request structure. */
	if ((R = mpool_request_malloc()) == NULL)
		goto err0;
	R->ID = P->ID;

	/* Initialize keys to NULL. */
	R->key = R->oval = R->value = NULL;

	/* Sanity-check packet length. */
	if (P->len < 4)
		goto err1;

	/* Figure out request type. */
	R->type = be32dec(&P->buf[0]);
	bufpos = 4;

/* Macro for extracting a key and advancing the buffer position. */
#define GRABKEY(dest, buf, buflen, bufpos, invalid) do {	\
	if (bufpos == buflen)					\
		goto invalid;					\
	dest = (struct kvldskey *)&buf[bufpos];			\
	bufpos += kvldskey_serial_size(dest);			\
	if (bufpos > buflen)					\
		goto invalid;					\
} while (0);

	/* Parse packet. */
	switch (R->type) {
	case PROTO_KVLDS_PARAMS:
		/* Nothing to parse. */
		break;
	case PROTO_KVLDS_DELETE:
	case PROTO_KVLDS_GET:
		/* Parse key. */
		GRABKEY(R->key, P->buf, P->len, bufpos, err2);
		break;
	case PROTO_KVLDS_SET:
	case PROTO_KVLDS_ADD:
	case PROTO_KVLDS_MODIFY:
		/* Parse key. */
		GRABKEY(R->key, P->buf, P->len, bufpos, err2);

		/* Parse value. */
		GRABKEY(R->value, P->buf, P->len, bufpos, err2);
		break;
	case PROTO_KVLDS_CAD:
		/* Parse key. */
		GRABKEY(R->key, P->buf, P->len, bufpos, err2);

		/* Parse oval. */
		GRABKEY(R->oval, P->buf, P->len, bufpos, err2);
		break;
	case PROTO_KVLDS_CAS:
		/* Parse key. */
		GRABKEY(R->key, P->buf, P->len, bufpos, err2);

		/* Parse oval. */
		GRABKEY(R->oval, P->buf, P->len, bufpos, err2);

		/* Parse value. */
		GRABKEY(R->value, P->buf, P->len, bufpos, err2);
		break;
	case PROTO_KVLDS_RANGE:
		/* Parse maximum key-value pairs length. */
		if (P->len - bufpos < 4) {
			errno = 0;
			goto err2;
		}
		R->range_max = be32dec(&P->buf[bufpos]);
		bufpos += 4;

		/* Parse start key. */
		GRABKEY(R->range_start, P->buf, P->len, bufpos, err2);

		/* Parse end key. */
		GRABKEY(R->range_end, P->buf, P->len, bufpos, err2);
		break;
	default:
		warn0("Unrecognized request type received: 0x%08" PRIx32,
		    R->type);
		goto err1;
	}

	/* Did we reach the end of the packet? */
	if (bufpos != P->len)
		goto err2;

	/* This buffer now belongs to the request structure. */
	R->blob = P->buf;

	/* Success! */
	return (R);

err2:
	warnp("Error parsing request packet of type 0x%08" PRIx32, R->type);
err1:
	proto_kvlds_request_free(R);
err0:
	/* Failure! */
	return (NULL);
}

/**
 * proto_kvlds_request_read(R, callback, cookie):
 * Read a packet from the reader ${R} and parse it as a KVLDS request.  Call
 * ${callback}(${cookie}, [request]), or ${callback}(${cookie}, NULL) if a
 * request could not be read or parsed.  The callback is responsible for
 * freeing the request structure.  Return a cookie which can be used to
 * cancel the operation.
 */
void *
proto_kvlds_request_read(struct netbuf_read * R,
    int (* callback)(void *, struct proto_kvlds_request *), void * cookie)
{
	struct request_read * G;

	/* Bake a cookie. */
	if ((G = mpool_requestread_malloc()) == NULL)
		goto err0;
	G->callback = callback;
	G->cookie = cookie;

	/* Read a packet. */
	if ((G->read_cookie = wire_readpacket(R, gotpacket, G)) == NULL)
		goto err1;

	/* Success! */
	return (G);

err1:
	mpool_requestread_free(G);
err0:
	/* Failure! */
	return (NULL);
}

/* We have a packet. */
static int
gotpacket(void * cookie, struct wire_packet * P)
{
	struct request_read * G = cookie;
	struct proto_kvlds_request * R;

	/* If we have no packet, we failed. */
	if (P == NULL)
		goto failed;

	/* Parse the packet. */
	if ((R = proto_kvlds_request_parse(P)) == NULL)
		goto failed1;

	/* Free the packet. */
	wire_packet_free(P);

	/* Perform the callback. */
	return (docallback(G, R));

failed1:
	free(P->buf);
	wire_packet_free(P);
failed:
	/* Perform the callback. */
	return (docallback(G, NULL));
}

/* Do the callback and free the request_read structure. */
static int
docallback(struct request_read * G, struct proto_kvlds_request * R)
{
	int rc;

	/* Do the callback. */
	rc = (G->callback)(G->cookie, R);

	/* Free the structure. */
	mpool_requestread_free(G);

	/* Pass the callback status back. */
	return (rc);
}

/**
 * proto_kvlds_request_read_cancel(cookie):
 * Cancel the request read for which ${cookie} was returned.  Do not invoke
 * the callback function.
 */
void
proto_kvlds_request_read_cancel(void * cookie)
{
	struct request_read * G = cookie;

	/* Cancel the read. */
	wire_readpacket_cancel(G->read_cookie);

	/* Free the cookie. */
	mpool_requestread_free(G);
}

/**
 * proto_kvlds_request_free(R):
 * Free the KVLDS request structure ${R}.
 */
void
proto_kvlds_request_free(struct proto_kvlds_request * R)
{

	/* Free the packet buffer (into which the keys point). */
	free(R->blob);

	/* Return the request structure to the pool. */
	mpool_request_free(R);
}

/**
 * proto_kvlds_response_params(Q, ID, kmax, vmax, callback, cookie):
 * Send a PARAMS response with ID ${ID} specifying that the maximum key
 * length is ${kmax} bytes and the maximum value length is ${vmax} bytes
 * to the write queue ${Q}.  Invoke ${callback}(${cookie}, 0/1) on packet
 * write success / failure.
 */
int
proto_kvlds_response_params(struct netbuf_write * Q, uint64_t ID,
    uint32_t kmax, uint32_t vmax)
{
	struct wire_packet P;
	uint8_t buf[8];

	P.ID = ID;
	P.len = 8;
	P.buf = buf;

	/* Construct the packet. */
	be32enc(&P.buf[0], kmax);
	be32enc(&P.buf[4], vmax);

	/* Queue the packet. */
	if (wire_writepacket(Q, &P))
		goto err0;

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/**
 * proto_kvlds_response_status(Q, ID, status, callback, cookie):
 * Send a SET/CAS/ADD/MODIFY/DELETE/CAD response with ID ${ID} and status
 * ${status} to the write queue ${Q} indicating that the request has been
 * completed with the specified status.  Invoke ${callback}(${cookie}, 0/1)
 * on packet write success / failure.
 */
int
proto_kvlds_response_status(struct netbuf_write * Q, uint64_t ID,
    uint32_t status)
{
	struct wire_packet P;
	uint8_t buf[4];

	P.ID = ID;
	P.len = 4;
	P.buf = buf;

	/* Construct the packet. */
	be32enc(&P.buf[0], status);

	/* Queue the packet. */
	if (wire_writepacket(Q, &P))
		goto err0;

	/* Success! */
	return (0);

err0:
	/* Failure! */
	return (-1);
}

/**
 * proto_kvlds_response_get(Q, ID, status, value, callback, cookie):
 * Send a GET response with ID ${ID}, status ${status}, and value ${value}
 * (if ${status} == 0) to the write queue ${Q} indicating that the provided
 * key is associated with the specified data (or not).  Invoke
 * ${callback}(${cookie}, 0/1) on packet write success / failure.
 */
int
proto_kvlds_response_get(struct netbuf_write * Q, uint64_t ID,
    uint32_t status, const struct kvldskey * value)
{
	struct wire_packet P;

	P.ID = ID;
	P.len = 4;
	if (status == 0)
		P.len += kvldskey_serial_size(value);

	/* Allocate the packet buffer. */
	if ((P.buf = malloc(P.len)) == NULL)
		goto err0;

	/* Construct the packet. */
	be32enc(&P.buf[0], status);
	if (status == 0)
		kvldskey_serialize(value, &P.buf[4]);

	/* Queue the packet. */
	if (wire_writepacket(Q, &P))
		goto err1;

	/* Free the packet buffer. */
	free(P.buf);

	/* Success! */
	return (0);

err1:
	free(P.buf);
err0:
	/* Failure! */
	return (-1);
}

/**
 * proto_kvlds_response_range(Q, ID, nkeys, next, keys, values,
 *     callback, cookie):
 * Send a RANGE response with ID ${ID}, next key ${next} and ${nkeys}
 * key-value pairs with the keys in ${keys} and values in ${values} to the
 * write queue ${Q}.  Invoke ${callback}(${cookie}, 0/1) on packet write
 * success / failure.
 */
int
proto_kvlds_response_range(struct netbuf_write * Q, uint64_t ID,
    size_t nkeys, const struct kvldskey * next,
    struct kvldskey ** keys, struct kvldskey ** values)
{
	struct wire_packet P;
	size_t i;
	size_t bufpos;

	/* Sanity check: We can't return more than 2^32-1 keys. */
	assert(nkeys <= UINT32_MAX);

	P.ID = ID;

	/* Figure out how long the packet will be. */
	P.len = 8;
	P.len += kvldskey_serial_size(next);
	for (i = 0; i < nkeys; i++) {
		P.len += kvldskey_serial_size(keys[i]);
		P.len += kvldskey_serial_size(values[i]);
	}

	/* Allocate the packet buffer. */
	if ((P.buf = malloc(P.len)) == NULL)
		goto err0;

	/* Construct the packet. */
	be32enc(&P.buf[0], 0);
	be32enc(&P.buf[4], nkeys);
	bufpos = 8;
	kvldskey_serialize(next, &P.buf[bufpos]);
	bufpos += kvldskey_serial_size(next);
	for (i = 0; i < nkeys; i++) {
		kvldskey_serialize(keys[i], &P.buf[bufpos]);
		bufpos += kvldskey_serial_size(keys[i]);
		kvldskey_serialize(values[i], &P.buf[bufpos]);
		bufpos += kvldskey_serial_size(values[i]);
	}

	/* Queue the packet. */
	if (wire_writepacket(Q, &P))
		goto err1;

	/* Free the packet buffer. */
	free(P.buf);

	/* Success! */
	return (0);

err1:
	free(P.buf);
err0:
	/* Failure! */
	return (-1);
}
