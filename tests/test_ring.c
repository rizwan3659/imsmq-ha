/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "../src/ring.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
	__FILE__, __LINE__, #c); fails++; } } while (0)

static void basics(void)
{
	struct ring *r = malloc(ring_bytes(4));
	struct msg m = { .corr = 0, .type = 1, .len = 3 }, o;
	memcpy(m.body, "abc", 3);
	ring_init(r, 4);

	CHECK(ring_pop(r, &o) == -1);                 /* empty */
	for (int i = 0; i < 4; i++) {
		m.corr = (uint64_t)i;
		CHECK(ring_push(r, &m) == 0);
	}
	CHECK(ring_push(r, &m) == -1);                /* full: backpressure */
	CHECK(ring_count(r) == 4);
	for (int round = 0; round < 10; round++) {    /* wrap many times */
		CHECK(ring_pop(r, &o) == 0);
		m.corr = o.corr + 4;
		CHECK(ring_push(r, &m) == 0);
	}
	CHECK(ring_pop(r, &o) == 0 && o.corr == 10 && o.len == 3 &&
	      memcmp(o.body, "abc", 3) == 0);

	m.len = 100000;                               /* oversized length is clamped */
	CHECK(ring_pop(r, &o) == 0);
	CHECK(ring_push(r, &m) == 0);
	while (ring_count(r) > 1)
		ring_pop(r, &o);
	CHECK(ring_pop(r, &o) == 0 && o.len == MSG_BODY_MAX);
	free(r);
}

#define N 1000000
static struct ring *shared;

static void *producer(void *arg)
{
	(void)arg;
	struct msg m = { .type = 1, .len = 8 };
	for (uint64_t i = 0; i < N; i++) {
		m.corr = i;
		memcpy(m.body, &i, 8);
		while (ring_push(shared, &m) < 0)
			sched_yield();
	}
	return NULL;
}

static void spsc_stress(void)
{
	shared = malloc(ring_bytes(64));
	ring_init(shared, 64);
	pthread_t t;
	pthread_create(&t, NULL, producer, NULL);

	struct msg o;
	uint64_t expect = 0, bad = 0;
	while (expect < N) {
		if (ring_pop(shared, &o) < 0) {
			sched_yield();
			continue;
		}
		uint64_t body;
		memcpy(&body, o.body, 8);
		bad += o.corr != expect || body != expect;
		expect++;
	}
	pthread_join(t, NULL);
	CHECK(bad == 0);
	CHECK(ring_count(shared) == 0);
	free(shared);
}

int main(void)
{
	basics();
	spsc_stress();
	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("ring tests passed (1M messages in order across threads)\n");
	return 0;
}
