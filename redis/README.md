# miniredis — a small Redis-shaped server

RESP2 over TCP or stdin/stdout, with a deliberately small command set — `PING`,
`SET`, `GET`, `DEL`, `EXISTS`, `QUIT` — and a deliberately unusual keyspace: one
thread per connection, all of them sharing a single lock-free hash table. Real Redis
is single-threaded on purpose; this is the other answer, and `mulle-concurrent` is
what makes it expressible.

Builds `mredis` (the server), `mredis-stress` (the harness, the RESP client the
fixtures use, and the benchmark) and `fake-redis-server` (a stand-in for
`redis-server`, so the `--bench --compare` plumbing is tested on machines that have
no redis installed).

```sh
mredis --port 6379 &                    # thread per connection
printf 'SET k hello\r\nGET k\r\n' | nc 127.0.0.1 6379

mredis --stdio < redis/fixtures/basic.txt   # same protocol, no sockets
mredis-stress --bench --threads 8 --seconds 5        # load, with a live display
mredis-stress --bench --compare --redis /usr/bin/redis-server   # both, side by side
```

**Numbers** — Release (`-O3 -DNDEBUG`), GCC, Linux, loopback, 64 B values,
pipeline 16, and every reply checked (`+OK` for a write, exact size and content for
a read), so `errors=0` is what makes them mean anything.

| workload | mredis | redis-server 8.0.5 | ratio |
|---|---|---|---|
| 4 threads | 1,826,617 req/s | 876,585 req/s | 2.1x |
| 8 threads | 3,704,193 req/s | 906,457 req/s | 4.1x |
| 16 threads, pipeline 64 | 7,446,624 req/s | 1,628,215 req/s | 4.6x |

Scaling with connections (3 s, 64 B values, pipeline 16): 578,211 req/s at one
connection to **10,243,873 at 64**, still climbing. Server RSS is 2.5–3.0 MB on a
small keyspace.

**Strengths**

- **It scales with connections instead of flattening.** The previous version kept
  retired values on a list behind a mutex, and the profile put 68% of the server's
  cycles in that futex, capping it at a hard ~2.5 M req/s per process. The lock-free
  retire stack removes the ceiling — 10.2 M req/s at 64 connections, with no serial
  resource in sight.
- **No mutex anywhere on the `SET`/`GET`/`DEL` path.** Tombstones, a reader epoch and
  a lock-free retire stack do the work; retiring links the value itself, so it
  allocates nothing and cannot fail.
- **Per-core efficiency is within ~10% of redis** at 4 and 8 threads — 829,352
  against 876,600 and 815,936 against 906,504 requests per server cpu-second. mredis
  buys its wall-clock lead with cores, not by cheating on the per-core work.
- **Every reply is verified**, so a fast wrong answer cannot show up as a fast one.
- **Portable.** `mulle-spawn` starts the server and `mulle-socket` carries both
  transports, so `--stdio`, TCP and the whole fixture suite run on Windows too — and
  every fixture runs over *both* transports against one golden file.

**Limitations, on purpose**

- **Six commands**, no options, no expiry, no persistence, no replication, no
  scripting, no auth, and it binds one address. It speaks RESP2 but there is no
  `COMMAND`, `HELLO` or `INFO`, so `redis-cli` is untested rather than supported.
- **A deleted key keeps its entry**, so memory is one entry per key *ever used*.
  Reclamation happens at quiescence, so a reader set that never empties accumulates
  retired values.
- **Read-modify-write is not atomic** — no `INCR`-shaped work, because the store
  cannot hand a CAS to its callers.
- **Two keys with the same hash are refused, not chained**: the hash *is* the key to
  the table, so the keyspace folds and compares key bytes itself, and a mismatched
  hash is a reported collision rather than a served value.
- **Efficiency falls off at 16 threads** — 599,246 against redis's 1,628,416
  requests per cpu-second — where the shared reader counter and the allocator start
  to cost more than the extra cores return.
- The benchmark reads the server's memory and CPU out of the kernel (`/proc` on
  Linux, libproc on macOS), so on Windows it names that and **skips** rather than
  failing in a way that looks like the server's fault.

**What it exercises.** `mulle-concurrent` (hashtable + pointer CAS), `mulle-thread`,
`mulle-aba`, `mulle-data` (`mulle_hash_64`), `mulle-allocator`, `mulle-buffer`,
`mulle-c11`, `mulle-spawn`, `mulle-socket`, plus raw POSIX sockets.

The keyspace's design, why it is shaped that way, and the full benchmark tables
including the redis-server comparison: [DESIGN.md](DESIGN.md).
