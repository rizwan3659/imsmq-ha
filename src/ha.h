/* SPDX-License-Identifier: MIT */
#ifndef HA_H
#define HA_H

#include "chan.h"

#define MSG_REQUEST 1
#define MSG_RESPONSE 2
#define MSG_SHUTDOWN 3

/* Worker process main loop: answer requests on c until MSG_SHUTDOWN,
 * beating the heartbeat at least every beat_ms. The "work" is a stand-in
 * for TAS/DRA logic: the response body is the request body reversed. */
void worker_run(struct chan *c, unsigned beat_ms);

typedef void (*answer_fn)(const struct msg *rsp, void *arg);

struct pending {
	uint64_t corr;
	int used;
	struct msg req;   /* kept so it can be replayed on failover */
};

struct router {
	struct chan *ch[2];
	int active;               /* index into ch */
	unsigned dead_ms;         /* heartbeat age that means "worker is gone" */
	struct pending *p;        /* window of in-flight requests, by corr & mask */
	uint32_t mask;
	uint64_t next_corr;
	size_t inflight;
	/* stats */
	unsigned failovers;
	size_t replayed, duplicates;
	uint64_t failover_at_ns;
};

int router_init(struct router *r, struct chan *primary, struct chan *standby,
		unsigned dead_ms);
void router_free(struct router *r);

/* Queue one request. Returns its correlation id, or -1 if the window or
 * the ring is full (backpressure: call router_poll and try again). */
int64_t router_send(struct router *r, const void *body, uint32_t len);

/* Collect responses for up to timeout_ms, calling fn for each first answer.
 * Also checks the active worker's heartbeat and fails over when it is stale:
 * any answers the dead worker already produced are collected from its ring,
 * then everything still unanswered is replayed to the standby.
 * Returns the number of answers delivered, or -1 if no worker is left. */
int router_poll(struct router *r, int timeout_ms, answer_fn fn, void *arg);

#endif
