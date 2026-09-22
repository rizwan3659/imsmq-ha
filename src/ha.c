/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ha.h"

#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void beat(struct chan *c)
{
	atomic_store_explicit(&c->sh->heartbeat_ns, mono_ns(), memory_order_release);
}

void worker_run(struct chan *c, unsigned beat_ms)
{
	struct pollfd p = { .fd = c->req_efd, .events = POLLIN };
	struct msg m, out;

	atomic_store(&c->sh->worker_pid, (uint32_t)getpid());
	beat(c);
	for (;;) {
		int r = poll(&p, 1, (int)beat_ms);
		beat(c);
		if (r < 0 && errno != EINTR)
			return;
		chan_drain(c->req_efd);

		int answered = 0;
		while (ring_pop(c->req, &m) == 0) {
			if (m.type == MSG_SHUTDOWN)
				goto out;
			out.corr = m.corr;
			out.type = MSG_RESPONSE;
			out.len = m.len;
			for (uint32_t i = 0; i < m.len; i++)
				out.body[i] = m.body[m.len - 1 - i];
			while (ring_push(c->rsp, &out) < 0) { /* router is behind */
				chan_notify(c->rsp_efd);
				beat(c);
				sched_yield();
			}
			answered++;
		}
		if (answered)
			chan_notify(c->rsp_efd);
	}
out:
	if (ring_count(c->rsp))
		chan_notify(c->rsp_efd);
}

int router_init(struct router *r, struct chan *primary, struct chan *standby,
		unsigned dead_ms)
{
	memset(r, 0, sizeof(*r));
	r->ch[0] = primary;
	r->ch[1] = standby;
	r->dead_ms = dead_ms;
	/* window = ring size, so a full window always fits in one ring */
	uint32_t slots = primary->sh->slots < standby->sh->slots ?
			 primary->sh->slots : standby->sh->slots;
	r->mask = slots - 1;
	r->p = calloc(slots, sizeof(*r->p));
	return r->p ? 0 : -1;
}

void router_free(struct router *r)
{
	free(r->p);
}

int64_t router_send(struct router *r, const void *body, uint32_t len)
{
	if (r->active < 0 || len > MSG_BODY_MAX)
		return -1;
	struct pending *slot = &r->p[r->next_corr & r->mask];
	if (slot->used)
		return -1; /* the oldest request in this slot is still unanswered */

	slot->req.corr = r->next_corr;
	slot->req.type = MSG_REQUEST;
	slot->req.len = len;
	memcpy(slot->req.body, body, len);
	if (ring_push(r->ch[r->active]->req, &slot->req) < 0)
		return -1;
	slot->corr = r->next_corr;
	slot->used = 1;
	r->inflight++;
	chan_notify(r->ch[r->active]->req_efd);
	return (int64_t)r->next_corr++;
}

/* Deliver every response waiting in ch's ring. */
static int collect(struct router *r, struct chan *ch, answer_fn fn, void *arg)
{
	struct msg m;
	int n = 0;
	chan_drain(ch->rsp_efd);
	while (ring_pop(ch->rsp, &m) == 0) {
		struct pending *slot = &r->p[m.corr & r->mask];
		if (!slot->used || slot->corr != m.corr) {
			r->duplicates++; /* answered already (e.g. replayed) */
			continue;
		}
		slot->used = 0;
		r->inflight--;
		fn(&m, arg);
		n++;
	}
	return n;
}

static int failover(struct router *r, answer_fn fn, void *arg, int *got)
{
	struct chan *dead = r->ch[r->active];
	int next = r->active == 0 ? 1 : 0;

	/* Answers the dead worker produced are still in shared memory. */
	*got += collect(r, dead, fn, arg);

	if (r->failovers > 0) { /* standby already used: nothing left */
		r->active = -1;
		return -1;
	}
	r->failovers++;
	r->failover_at_ns = mono_ns();
	r->active = next;

	/* Replay everything unanswered, oldest first. */
	struct chan *live = r->ch[next];
	uint64_t first = r->next_corr - (r->next_corr < r->mask + 1 ? r->next_corr : r->mask + 1);
	for (uint64_t c = first; c < r->next_corr; c++) {
		struct pending *slot = &r->p[c & r->mask];
		if (!slot->used || slot->corr != c)
			continue;
		while (ring_push(live->req, &slot->req) < 0) {
			chan_notify(live->req_efd);
			*got += collect(r, live, fn, arg);
			sched_yield();
		}
		r->replayed++;
	}
	chan_notify(live->req_efd);
	return 0;
}

int router_poll(struct router *r, int timeout_ms, answer_fn fn, void *arg)
{
	if (r->active < 0)
		return -1;
	struct chan *ch = r->ch[r->active];
	struct pollfd p = { .fd = ch->rsp_efd, .events = POLLIN };
	int got = 0;

	if (poll(&p, 1, timeout_ms) < 0 && errno != EINTR)
		return -1;
	got += collect(r, ch, fn, arg);

	uint64_t hb = atomic_load_explicit(&ch->sh->heartbeat_ns, memory_order_acquire);
	if (hb && mono_ns() - hb > (uint64_t)r->dead_ms * 1000000ULL) {
		if (failover(r, fn, arg, &got) < 0 && got == 0)
			return -1;
	}
	return got;
}
