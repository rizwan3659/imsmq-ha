/* SPDX-License-Identifier: MIT */
/* A channel between a router and one worker process: a request ring, a
 * response ring, an eventfd each way for wake-ups, and a heartbeat word the
 * worker updates. Everything lives in one MAP_SHARED mapping created before
 * fork(), so both processes see it at the same address. */
#ifndef CHAN_H
#define CHAN_H

#include "ring.h"

struct chan_shared {
	_Atomic uint64_t heartbeat_ns; /* CLOCK_MONOTONIC of the worker's last beat */
	_Atomic uint32_t worker_pid;
	uint32_t slots;
	size_t req_off, rsp_off;       /* offsets of the two rings */
};

struct chan {
	struct chan_shared *sh;
	struct ring *req, *rsp;
	int req_efd;  /* router -> worker: "requests waiting" */
	int rsp_efd;  /* worker -> router: "responses waiting" */
	size_t map_len;
};

int chan_create(struct chan *c, uint32_t slots);
void chan_destroy(struct chan *c);

void chan_notify(int efd);
void chan_drain(int efd);

uint64_t mono_ns(void);

#endif
