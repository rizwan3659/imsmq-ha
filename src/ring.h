/* SPDX-License-Identifier: MIT */
/* Single-producer / single-consumer ring of fixed-size slots, placed in
 * shared memory so two processes can use it. Lock-free: the producer owns
 * `head`, the consumer owns `tail`, and acquire/release ordering publishes
 * slot contents. */
#ifndef RING_H
#define RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define SLOT_SIZE 256
#define MSG_BODY_MAX (SLOT_SIZE - 16)

struct msg {
	uint64_t corr;   /* correlation id chosen by the sender */
	uint32_t type;
	uint32_t len;    /* bytes used in body */
	uint8_t body[MSG_BODY_MAX];
};

_Static_assert(sizeof(struct msg) == SLOT_SIZE, "slot layout");

struct ring {
	_Alignas(64) _Atomic uint32_t head; /* next slot to write */
	_Alignas(64) _Atomic uint32_t tail; /* next slot to read */
	_Alignas(64) uint32_t mask;         /* slots - 1 */
	struct msg slot[];
};

size_t ring_bytes(uint32_t slots);         /* slots must be a power of two */
void ring_init(struct ring *r, uint32_t slots);

/* Non-blocking. Returns 0, or -1 if the ring is full (backpressure). */
int ring_push(struct ring *r, const struct msg *m);
/* Returns 0 and fills *m, or -1 if empty. */
int ring_pop(struct ring *r, struct msg *m);
uint32_t ring_count(const struct ring *r);

#endif
