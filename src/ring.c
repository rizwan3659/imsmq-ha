/* SPDX-License-Identifier: MIT */
#include "ring.h"

#include <string.h>

size_t ring_bytes(uint32_t slots)
{
	return sizeof(struct ring) + (size_t)slots * sizeof(struct msg);
}

void ring_init(struct ring *r, uint32_t slots)
{
	atomic_init(&r->head, 0);
	atomic_init(&r->tail, 0);
	r->mask = slots - 1;
}

int ring_push(struct ring *r, const struct msg *m)
{
	uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
	uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

	if (head - tail > r->mask)
		return -1;
	struct msg *s = &r->slot[head & r->mask];
	size_t len = m->len <= MSG_BODY_MAX ? m->len : MSG_BODY_MAX;
	s->corr = m->corr;
	s->type = m->type;
	s->len = (uint32_t)len;
	memcpy(s->body, m->body, len);
	atomic_store_explicit(&r->head, head + 1, memory_order_release);
	return 0;
}

int ring_pop(struct ring *r, struct msg *m)
{
	uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
	uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);

	if (tail == head)
		return -1;
	const struct msg *s = &r->slot[tail & r->mask];
	m->corr = s->corr;
	m->type = s->type;
	m->len = s->len <= MSG_BODY_MAX ? s->len : MSG_BODY_MAX;
	memcpy(m->body, s->body, m->len);
	atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
	return 0;
}

uint32_t ring_count(const struct ring *r)
{
	return atomic_load_explicit(&r->head, memory_order_acquire) -
	       atomic_load_explicit(&r->tail, memory_order_acquire);
}
