# hashspace design

Why the hash space is a contract, what the churn measures, and the two modes that
never come back. The short version is in [README.md](README.md).

## The hash space

| hash | positive | even | hashmap | why |
|---|---|---|---|---|
| 0 | EINVAL | EINVAL | EINVAL | 0 is NO_HASH in both maps |
| 1 | ok | EINVAL | ok | odd, so even mode cannot claim it |
| the top bit | ok | EINVAL | ok | it is positive mode's frozen flag |
| -2 | EINVAL | ok | ok | top bit set, but even: even mode takes it |
| INTPTR_MIN | EINVAL | ok | ok | same |
| `malloc()` | ok | ok | ok | an aligned pointer is even |
| `malloc()+1` | ok | EINVAL | ok | unaligned, so even mode refuses it |

Out of space is `EINVAL`, never a silent fold, so a bad fold is an error rather
than an aliased key. And neither map replaces: an existing hash refuses an insert
with `EEXIST`, and a remove must name the value it expects, so a "set" is
`remove(hash, value)` then `insert`.

## Churn

No locks means no waiting, and the churn says what that buys. 64 CPUs, 500,000
operations per thread against one 4096 slot table, on the three way mix:

| threads | ops/s | p50 | p99 |
|---|---|---|---|
| 1 | 13.7 M | 29 ns | 41 ns |
| 4 | 21.4 M | 160 ns | 322 ns |
| 8 | 32.2 M | 160 ns | 387 ns |

`--map hashmap` runs that workload against the other map, and the result is that
there is almost nothing to say: the hashmap has no concurrent remove at all (its
`patch` and `remove` are single threaded writes), so the mix belongs to the map.
`--no-remove` asks the hashtable for the hashmap's two operations, and because the
operation is drawn from the seed *before* the map is touched, the two runs start
from literally the same operations — the counters that do not depend on thread
timing come out equal, which the test asserts instead of eyeballing. On the same
mix the two maps are indistinguishable:

| threads | hashtable | hashmap | p50 (both) |
|---|---|---|---|
| 1 | 14.0 M/s | 14.4 M/s | 29 ns |
| 4 | 24.1 M/s | 24.3 M/s | 128 ns |
| 8 | 43.0 M/s | 42.5 M/s | 129 ns |

So keeping the full hash width costs the ability to remove a hash concurrently and
nothing else measurable; the 32.2 M/s above is what that third of the mix is worth.
The one thing that differs between the two runs is the hit and miss counts,
because whether a key was already inserted when a lookup ran is a race — and that
is why the test compares the counters that are not.

Every operation is one of three, and every refusal is counted rather than dropped:
of the 2,000,000 operations in the 4 thread run, 134,250 inserts landed and
133,846 were refused with `EEXIST`; 132,379 removes were refused with `ENOENT`
because another thread got there first; 132,880 lookups missed. `--verify` then
drains the table one entry at a time and checks every value against the hash it
sits under — **0 corrupt** in every configuration, including while a migrator
forces generations.

That last part is the interesting one, because the enumerator is *supposed* to be
interrupted: a walk that crosses a generation change, or reaches a slot frozen by
a copy in flight, is told `ECANCELED` and restarts. Without forced migration it is
**0 cancelled of 3126 walks**; with one migration per millisecond nearly every
walk is cancelled, and the values it did see still all match. Forcing generations
costs about half the throughput (22.0 M/s → 11.4 M/s over 43 generations).

## A bad size, and a missing reclaimer

Both modes run a child process with a one second deadline, so the demo can report
what happened without taking itself down with it.

```
=== size and mask ===
map        reachable slots  work                 result
hashtable  2048 of 2048     64 insert + 1 lookup completed, table 2048
hashtable  32 of 2064       64 insert + 1 lookup completed, table 2064 -> 4096
hashtable  32 of 2064       64 insert            completed, table 2064 -> 4096
hashmap    32 of 2064       64 insert            completed, table 2064 -> 4096
hashmap    2048 of 2048     64 insert            completed, table 2048

=== mulle_aba_init() ===
   2 inserts, no mulle_aba_init()   -> completed, table 4
  40 inserts, no mulle_aba_init()   -> did not complete (killed after 1s)
  40 inserts, with mulle_aba_init() -> completed, table 4 -> 128
```

That last column is what each child hands back on its stdout, not a yes: the size
its table ended up using. Every row now reports one, which is the point — the size
trap is closed.

The probe index is `i & (size - 1)`, so a size whose mask does not cover the table
walks a fraction of its slots: `2064 - 1` is `0x80f`, which keeps bits 0–3 and bit
11, so only 32 of the 2064 slots are reachable and a probe never meets a virgin
slot. That used to mean a table that never comes back — the hashtable's `lookup`
had only `assert( index != sentinel)`, compiled out under `NDEBUG`, and all four
probe loops in the hashmap were the same shape — which is what `fixed/26` reports.
Both maps now round the size up to the next power of two and clamp it
(`mulle-concurrent-math.h`, `_mulle_concurrent_pow2roundup`), in the storage
allocation, and so before the first insert: the table `--mask` asks for at 2064 is
built as one of 4096, which is the size the child reports back. Nothing migrates and
nothing is repaired after the fact, because there is nothing left to repair. What is
worth remembering is the trap below, which is still open.

The ABA reclaimer is the same shape of trap from the other direction: a map that
never migrates never retires storage, so forgetting `mulle_aba_init()` survives a
small test and dies later. The allocator prints the remedy when it aborts.

## The child is a second process

`--mask` and `--universe` need a process that can fail without taking the demo
with it, and on Windows there is no `fork()` with which to get one. So the child
is a real process on every platform: the program runs *itself* again through
`mulle-spawn`, with `--internal-child <mode> <values...>` on the command line, and
`mulle-spawn` collects what it prints and holds it to the deadline.

Three consequences, because they are what the verdicts are made of:

- The child shares nothing with the parent, so an argument cannot be a pointer.
  The few integers a mode needs travel as command line words: `hs_child_values`
  writes them, `hs_internal_child` reads them back, and they sit next to each
  other in the source for exactly that reason.
- The report travels on **stdout**, which `mulle-spawn` captures, and **stderr**
  is left alone — which is why the allocator's own "no ABA reclaimer installed"
  still reaches whoever ran the demo. Output and survival are separate answers,
  so a child that printed its report and *then* stopped coming back still hands
  the report over.
- A child that migrates needs a reclaimer of its own, because it is no longer a
  copy of a parent that already called `mulle_aba_init()`. The mask child
  therefore initializes one, and the universe child is told whether to — which is
  what makes the latter an experiment instead of an accident.

## The rest of the limitations

`--mask` and `--universe` run children and give them a one second deadline,
because the alternative is running the demo under `timeout`. The migrator is
paced at one generation per millisecond on purpose — flat out it cancels every walk
instantly, and an enumerator that sees nothing cannot show that what it sees is
consistent. `--churn` uses one table and a shared key space, so `EEXIST` is common
by construction; the point is that refusals are counted, not that they are rare.
One habit worth copying from the source: never put a mutator and a read of its
effect in the same argument list — `printf( "%s %s", insert(...), lookup(...))`
leaves the order unspecified and gcc reads first.

**Exercises** `mulle-concurrent` (both maps), `mulle-aba`, `mulle-thread`,
`mulle-spawn`.
