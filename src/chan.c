/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "chan.h"

#include <errno.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

uint64_t mono_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static size_t align64(size_t n)
{
	return (n + 63) & ~(size_t)63;
}

int chan_create(struct chan *c, uint32_t slots)
{
	if (slots == 0 || (slots & (slots - 1)))
		return -1;
	size_t req_off = align64(sizeof(struct chan_shared));
	size_t rsp_off = req_off + align64(ring_bytes(slots));
	size_t len = rsp_off + align64(ring_bytes(slots));

	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
		       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		return -1;

	c->sh = p;
	c->map_len = len;
	c->sh->slots = slots;
	c->sh->req_off = req_off;
	c->sh->rsp_off = rsp_off;
	atomic_init(&c->sh->heartbeat_ns, 0);
	atomic_init(&c->sh->worker_pid, 0);
	c->req = (struct ring *)((char *)p + req_off);
	c->rsp = (struct ring *)((char *)p + rsp_off);
	ring_init(c->req, slots);
	ring_init(c->rsp, slots);

	c->req_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	c->rsp_efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (c->req_efd < 0 || c->rsp_efd < 0) {
		chan_destroy(c);
		return -1;
	}
	return 0;
}

void chan_destroy(struct chan *c)
{
	if (c->req_efd >= 0)
		close(c->req_efd);
	if (c->rsp_efd >= 0)
		close(c->rsp_efd);
	munmap(c->sh, c->map_len);
}

void chan_notify(int efd)
{
	uint64_t one = 1;
	while (write(efd, &one, sizeof(one)) < 0 && errno == EINTR)
		;
}

void chan_drain(int efd)
{
	uint64_t v;
	while (read(efd, &v, sizeof(v)) > 0)
		;
}
