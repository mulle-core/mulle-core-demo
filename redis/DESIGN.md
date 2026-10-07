# miniredis design

The keyspace's shape, the benchmark in full, and what each number was measured
against. The short version is in [README.md](README.md).

## Design

The keyspace's shape is forced by two properties of `mulle_concurrent_hashtable`:
it has no atomic replace, and its hash space belongs to the caller.

- **An entry per key, registered once, never unregistered.** `register()` is
  insert-if-absent and `insert()` returns `EEXIST` rather than overwriting, so
  `SET` cannot be remove-then-register: that has a window in which the key does
  not exist. `mredis-stress --concurrency --naive` measures **37,370 of 200,000**
  reads losing a key that was never deleted, with no artificial delay. So only
  the payload changes, with `_mulle_atomic_pointer_cas()`, and the real keyspace
  loses **0**.
- **Deletion tombstones instead of unregistering**, because removing an entry
  another thread is about to CAS into would lose that write, and freeing it would
  be a use-after-free. Entries last as long as the store does.
- **The hash space belongs to the caller, key identity to the store.** The table
  keys on the hash alone and validates rather than folds: a hash outside
  `[1, INTPTR_MAX]` is refused with `EINVAL`. mredis wants byte-string identity,
  so it folds `mulle_hash_64()` itself and compares key bytes on every access; a
  mismatch is a reported collision, never a served value. `--collision` narrows
  the hash to 8 bits to make that path certain (1,745 collisions, 0 aliased).
- **Readers bracket an epoch**, so a replaced value cannot be freed while someone
  may still be reading it: readers increment an atomic counter, and a replaced
  value is pushed onto a lock-free retire stack. The stack is detached and freed
  the moment the counter reads zero. Both the push and the drain are CAS loops —
  there is no mutex anywhere on the `SET`, `GET` or `DEL` path, and retiring a
  value links the value itself, so it allocates nothing and cannot fail.
- **`mulle-aba` reclaims the table's storage** and must be initialised first:
  `mulle_aba_init()` once, `mulle_aba_register()` per thread. The store keeps its
  own copy of the default allocator with an ABA-aware `abafree` hook rather than
  relying on mulle-aba having patched the global one.

Protocol handling is RESP2 multibulk and inline (the telnet path), binary safe,
with limits checked before anything is allocated: 128 arguments, 64 MB per bulk,
64 KB per inline line. A client asking for `$1000000000` gets an error, not a
gigabyte.

**Exercises** `mulle-concurrent` (hashtable + pointer CAS), `mulle-thread`,
`mulle-aba`, `mulle-data` (`mulle_hash_64`), `mulle-allocator`, `mulle-buffer`,
`mulle-c11`, plus raw POSIX sockets.

## Portability

All of it is portable. `mulle-spawn` starts the server (`CreateProcessW` on
Windows, `fork`/`execvp` elsewhere) and `mulle-socket` carries both halves of
the TCP path, so `--stdio`, the TCP transport and the whole fixture suite run on
Windows too -- and every fixture is run over both transports, against one golden
file, so the two have to agree. What does not run there is the benchmark, which
reads the server's memory and CPU out of the kernel: `/proc` on Linux, libproc
on macOS, and nothing on Windows. It names that and skips, rather than fail in a
way that looks like the server's fault.

It speaks RESP2, the protocol `redis-cli` speaks, but the command set is a subset
of Redis's and there is no `COMMAND`, `HELLO` or `INFO`, so `redis-cli` is
untested rather than supported: the suite drives the server with its own client.
Both ends of every connection are set `TCP_NODELAY` by `mulle-socket`, on
`accept` and on `connect`. That is a correctness setting here rather than a
tuning one — without it a pipelined request waits on Nagle, and the measurement
that says so is in `mulle-socket.h`. `--help` on
either binary lists the modes; `mredis-stress --bench --help` lists the
benchmark's.

## Benchmark

`mredis-stress --bench` is a load generator with a small live display. Each worker
owns one connection and keeps `--pipeline` requests in flight, so the rate is the
server's throughput rather than the round trip time; every reply is checked (`+OK`
for a write, and for a read the exact size and content written), so `errors=0` is
what says the numbers mean anything.

```sh
mredis-stress --bench --threads 8 --seconds 5        # the live display
mredis-stress --bench --threads 8 --quiet            # one summary line, for tests
mredis-stress --bench --compare --redis /usr/bin/redis-server   # both, side by side
```

```
mredis bench   mredis (spawned, tcp)   threads 4   pipeline 16   payload 64 B   keys 1000
  elapsed      3.0 s
  requests     5,828,496        1,941,269 req/s   (peak 2,572,879)
  traffic      in 341.2 MB   out 211.2 MB        184.0 MB/s
  latency/req  p50 1.8 us   p90 2.3 us   p99 2.8 us   p99.9 3.5 us   max 27.4 us
  server       RSS 2.5 MB   (pid 2559433)
  server cpu   6.55 s   218% of a core
  client cpu   6.21 s   206% of one core, all 4 workers
  errors       0
```

Measured with mredis and mredis-stress built `-O3 -DNDEBUG`
(`CMAKE_BUILD_TYPE=Release`), GCC, Linux, loopback, 5 s per line. The absolute
numbers are this machine's; the shape is the server's.

| workload | requests/s | MB/s | server RSS | server cpu |
|---|---|---|---|---|
| 1 thread, stdio | 1,290,852 | 122.3 | 1.8 MB | 43% of a core |
| 8 threads, tcp, 64 B values | 4,166,855 | 394.9 | 3.0 MB | 451% |
| 8 threads, tcp, 128 B values, 200k keys | 3,918,373 | 625.5 | 75.8 MB | 452% |

One connection, sweeping pipeline depth (3 s each): `pipeline 1` is 44,055 req/s
and `pipeline 512` is 2.0–3.3M — the run-to-run spread on a single connection is
wide — and past a few hundred requests in flight the depth stops mattering. That
plateau is now *per connection*: adding connections keeps scaling the server,
which is what the next section is about.

### Against a real redis-server

The load generator speaks RESP and does not care who answers, so "is this
competitive?" is a measurement. `--connect HOST:PORT` benches a server that is
already running (its RSS and CPU come from the pid that owns the port — on Linux
the socket's inode in `/proc/net/tcp` matched against `/proc/<pid>/fd`, on macOS
a libproc walk of every process's descriptors); `--redis PATH` starts one on a
port it picked
and passes `--port`, `--bind 127.0.0.1`, `--save ''`, `--appendonly no`;
`--compare` runs both and prints a ratio column plus a machine-readable line the
suite reads.

Measured against `redis-server` 8.0.5 (distro package, no config file, persistence
off, loopback), 4 s per line, 64 B values, pipeline 16, `errors=0` on both sides:

| workload | mredis req/s | redis req/s | ratio | mredis server cpu | redis server cpu | mredis req/cpu-s | redis req/cpu-s |
|---|---|---|---|---|---|---|---|
| 4 threads | 1,826,617 | 876,585 | 2.1x | 220% of a core | 99% | 829,352 | 876,600 |
| 8 threads | 3,704,193 | 906,457 | 4.1x | 454% | 99% | 815,936 | 906,504 |
| 16 threads, pipeline 64 | 7,446,624 | 1,628,215 | 4.6x | 1243% | 99% | 599,246 | 1,628,416 |

Read the ratios, not the ranking. This is one cache-shaped workload — one
keyspace, `SET`/`GET`, no expiry, no persistence, deep pipelining — and redis
brings expiry, persistence, replication, scripting and a hundred commands to that
same socket while mredis has six commands and no protocol on top of RESP2. Note
also what redis's column is doing: it sits at 99% of exactly one core in every
row, which is its single-threaded command loop working as designed, while mredis
buys its wall-clock lead by spreading the same batch over 2 to 12 cores. At 4 and
8 threads the two are at parity on *efficiency* too — 0.83M against 0.88M and
0.82M against 0.91M requests per server cpu-second, within about 10%. The gap
opens only at 16 threads (0.60M against 1.63M), where the shared reader counter
and the allocator start to cost more than the extra cores return. The RSS column
is the sharper claim and the narrower one: redis's floor is ~16 MB (jemalloc
arenas), so mredis looks 6x smaller on a small keyspace, but at 200,000 keys they
converge (76 MB against 61 MB) — mredis is not leaner per key, it starts leaner.

### Scaling

The previous version kept retired values on a list behind a mutex, and the profile
put 68% of the server's cycles in that futex: throughput flattened at a hard
~2.5M req/s per process and CPU grew linearly with connections. The lock-free
retire stack removes that. Throughput now scales with connections (3 s each, 64 B
values, pipeline 16):

| connections | requests/s | server cpu | requests/s per cpu-second |
|---|---|---|---|
| 1 | 578,211 | 0.51 cores | 375,462 |
| 4 | 1,935,230 | 2.22 | 291,117 |
| 8 | 4,098,387 | 4.52 | 301,351 |
| 16 | 6,754,378 | 10.57 | 213,207 |
| 32 | 8,153,512 | 24.44 | 111,108 |
| 64 | 10,243,873 | 46.44 | 73,485 |

Ten million requests a second at 64 connections, still climbing. Per-core
efficiency falls as connections grow, which is what shared atomics cost when every
operation touches them — but there is no serial resource and no ceiling, and a deep
pipeline keeps scaling across connections instead of flattening at a per-process
number (3 s, 64 B values, pipeline 2048):

| connections, pipeline | requests/s | server cpu |
|---|---|---|
| 1, 2048 | 2,992,716 | 0.57 cores |
| 2, 2048 | 3,964,779 | 1.13 |
| 4, 2048 | 5,502,685 | 2.68 |
| 8, 2048 | 6,848,809 | 6.19 |

## The rest of the limitations

The command set is small (no options, no expiry, no `KEYS`/`SCAN`/`DBSIZE`, no
lists or hashes) — it is a server, not a Redis. A deleted key keeps its entry, so
memory is one entry per key ever used. Reclamation happens at quiescence, so a
reader set that never empties means retired values accumulate
(`mredis-stress --reclaim` reports live and retired and asserts retired is zero).
Read-modify-write is not atomic (`INCR`-shaped work) because the
store cannot hand a CAS to its callers. Two Redis keys with the same hash are
refused rather than chained: the hash *is* the key to the table, so the keyspace
folds and compares bytes itself. No persistence, replication or auth, and it binds
one address.
