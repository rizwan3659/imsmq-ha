/* SPDX-License-Identifier: MIT */
/* mqha-demo: a router, a primary worker and a hot standby, each in its own
 * process. The router streams requests; part-way through, the primary is
 * killed with SIGKILL. Exit status 0 means every request was answered
 * exactly once with the right content. */
#define _GNU_SOURCE
#include "ha.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct tally {
	uint8_t *seen;
	size_t n, answered, wrong, twice;
};

static void on_answer(const struct msg *m, void *arg)
{
	struct tally *t = arg;
	char want[32];
	int len = snprintf(want, sizeof(want), "req-%llu", (unsigned long long)m->corr);

	if (m->corr >= t->n) {
		t->wrong++;
		return;
	}
	if (t->seen[m->corr]++)
		t->twice++;
	t->answered++;
	int ok = m->len == (uint32_t)len;
	for (int i = 0; ok && i < len; i++)
		ok = m->body[i] == (uint8_t)want[len - 1 - i];
	t->wrong += !ok;
}

static pid_t spawn(struct chan *c, unsigned beat_ms)
{
	pid_t p = fork();
	if (p == 0) {
		worker_run(c, beat_ms);
		_exit(0);
	}
	return p;
}

int main(int argc, char **argv)
{
	size_t n = argc > 1 ? strtoul(argv[1], NULL, 10) : 200000;
	int kill_primary = !(argc > 2 && strcmp(argv[2], "--no-kill") == 0);
	const unsigned beat_ms = 10, dead_ms = 50;
	struct chan a, b;
	struct router r;
	struct tally t = { .seen = calloc(n, 1), .n = n };

	if (!t.seen || chan_create(&a, 1024) || chan_create(&b, 1024))
		return 1;
	pid_t pa = spawn(&a, beat_ms), pb = spawn(&b, beat_ms);
	if (pa < 0 || pb < 0 || router_init(&r, &a, &b, dead_ms))
		return 1;

	uint64_t t0 = mono_ns(), killed_at = 0;
	size_t sent = 0;
	char body[32];
	while (t.answered < n) {
		while (sent < n) {
			int len = snprintf(body, sizeof(body), "req-%zu", sent);
			if (router_send(&r, body, (uint32_t)len) < 0)
				break; /* backpressure */
			sent++;
			if (kill_primary && sent == n / 2 && !killed_at) {
				kill(pa, SIGKILL);
				killed_at = mono_ns();
			}
		}
		if (router_poll(&r, 5, on_answer, &t) < 0) {
			fprintf(stderr, "no worker left\n");
			break;
		}
		if (mono_ns() - t0 > 30ULL * 1000000000ULL) {
			fprintf(stderr, "timeout\n");
			break;
		}
	}
	double secs = (mono_ns() - t0) / 1e9;

	struct msg bye = { .type = MSG_SHUTDOWN };
	for (int i = 0; i < 2; i++) { /* stop whichever workers are still alive */
		ring_push(r.ch[i]->req, &bye);
		chan_notify(r.ch[i]->req_efd);
	}
	waitpid(pa, NULL, 0);
	waitpid(pb, NULL, 0);

	printf("requests %zu, answered %zu, wrong %zu, answered twice %zu\n",
	       n, t.answered, t.wrong, t.twice);
	printf("elapsed %.3f s, %.0f req/s end to end\n", secs, n / secs);
	if (killed_at) {
		printf("primary killed after %zu requests; failover detected after %.1f ms; "
		       "%zu requests replayed; %zu late duplicates ignored\n",
		       n / 2, r.failovers ? (r.failover_at_ns - killed_at) / 1e6 : -1.0,
		       r.replayed, r.duplicates);
	}
	int ok = t.answered == n && t.wrong == 0 && t.twice == 0 &&
		 (!kill_primary || r.failovers == 1);
	router_free(&r);
	chan_destroy(&a);
	chan_destroy(&b);
	free(t.seen);
	return ok ? 0 : 1;
}
