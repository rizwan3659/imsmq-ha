# imsmq-ha

Asynchronous request/response messaging between processes, with a hot
standby that takes over when the active worker dies. **No request is lost
or answered twice.**

```
             req ring + eventfd          ┌──────────────────┐
 ┌────────┐ ───────────────────────────▶ │ primary worker   │  heartbeat ─┐
 │ router │ ◀─────────────────────────── │ (e.g. TAS logic) │             │
 │        │      rsp ring + eventfd      └──────────────────┘             │
 │        │                                                                ▼
 │        │ ───────────────────────────▶ ┌──────────────────┐   router checks the
 │        │ ◀─────────────────────────── │ hot standby      │   age of each beat
 └────────┘                              └──────────────────┘
```

- **Shared-memory rings**: single-producer, single-consumer, lock-free. The
  producer owns `head`, the consumer owns `tail`, and release/acquire
  ordering publishes each slot. They live in one `MAP_SHARED` mapping made
  before `fork()`.
- **eventfd wake-ups**: no busy polling when idle. Workers still wake every
  heartbeat interval.
- **Backpressure**: `router_send` returns -1 when the in-flight window or the
  ring is full. The caller polls for answers and retries. Nothing is dropped.
- **Failover**: when the active worker's heartbeat is older than `dead_ms`,
  the router:
  1. collects any answers the dead worker already wrote, since they're still
     in shared memory,
  2. switches to the standby,
  3. replays every request that is still unanswered, oldest first.

  Late or duplicate answers are recognised by correlation id and dropped.

> This is a clean-room re-implementation of the pattern behind the IMS
> components (DRA, TAS) I built at C-DOT on an asynchronous inter-process
> message queue with HA and failover. It contains no C-DOT code. The numbers
> below come from this repository only.

## Build and run

```sh
make test   # ring unit + 1M-message cross-thread stress; failover scenario x5
make asan   # the same under AddressSanitizer + UBSan
make tsan   # ring stress under ThreadSanitizer
./mqha-demo 200000            # kill the primary half-way through
./mqha-demo 200000 --no-kill
```

## Results

2-vCPU cloud VM, gcc 13, `-O2`, 1,024-slot rings, 10 ms heartbeat, 50 ms
dead threshold. The primary is killed with `SIGKILL` after 100,000 of
200,000 requests:

```
requests 200000, answered 200000, wrong 0, answered twice 0
elapsed 0.272 s, 734986 req/s end to end
primary killed after 100000 requests; failover detected after 51.9 ms;
1024 requests replayed; 0 late duplicates ignored
```

Detection time is set by `dead_ms` (the heartbeat age that counts as dead)
plus up to one poll interval. The trade-off is false failovers under load
versus longer outages: a worker stalled for more than `dead_ms` gets
replaced.

## Guarantees and limits

- **At-least-once delivery to workers, exactly-once answers to the caller.**
  After a failover, a replayed request can run twice (once on each worker).
  That's safe for idempotent or stateless work, such as most Diameter
  Cx/Sh queries. Stateful work needs idempotency keys on the worker side.
- **Only one failover.** With two workers, losing the standby as well is
  reported as "no worker left". Restarting a replacement and re-pairing it is
  not implemented.
- **Split brain is avoided** because only the router sends traffic, and it
  sends to exactly one worker at a time. A primary that was only slow (not
  dead) keeps running but receives nothing more.

## Layout

| File | What it does |
| --- | --- |
| `src/ring.c` | SPSC ring: push/pop with acquire/release atomics, clamps lengths from shared memory |
| `src/chan.c` | Shared mapping with two rings, two eventfds and a heartbeat word |
| `src/ha.c` | Worker loop; router with an in-flight window, dedup and failover/replay |
| `src/demo.c` | Three processes, kill test, exactly-once check |
| `tests/test_ring.c` | Full, empty and wraparound; 1M messages across threads in order |

MIT licensed.
