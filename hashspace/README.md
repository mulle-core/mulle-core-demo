# hashspace — the hash *is* the key, so the hash space is a contract

`mulle-concurrent`'s two maps keep an `intptr_t` hash in one word and a payload in
the next with no locks and no waiting, and both steal bits from the *hash* word to
mark slots. So the hash is not an opaque key and its range is not a suggestion:
`0` is `NO_HASH` in both maps, positive mode freezes its top bit, even mode refuses
anything odd, and out of space is `EINVAL` rather than a silent fold. Neither map
replaces: an insert on an existing hash is refused with `EEXIST`, and a remove has
to name the value it expects.

```sh
./build/hashspace/hashspace                                    # what each map accepts as a hash
./build/hashspace/hashspace --replace                          # no replace, and what that forces
./build/hashspace/hashspace --churn --threads 8 --ops 500000 --verify --enumerate
./build/hashspace/hashspace --churn --map hashmap --verify     # the other map, same workload
./build/hashspace/hashspace --mask                             # what a bad table size used to do
./build/hashspace/hashspace --universe                         # the ABA reclaimer, and how it hides
./build/hashspace/hashspace --all --quiet                      # every number as k=v
```

**Numbers** — 500,000 operations per thread against one 4096-slot table, 64 CPUs,
Release, on a three-way mix of insert/lookup/remove.

| threads | ops/s | p50 | p99 |
|---|---|---|---|
| 1 | 13.7 M | 29 ns | 41 ns |
| 4 | 21.4 M | 160 ns | 322 ns |
| 8 | 32.2 M | 160 ns | 387 ns |

**Strengths**

- **Lock-free, and it shows**: 13.7 M ops/s on one thread and 32.2 M on eight, with
  no wait on the path — and refusals are counted rather than dropped. Of the
  2,000,000 operations in the 4-thread run, 134,250 inserts landed and 133,846 were
  refused with `EEXIST`; 132,379 removes were refused with `ENOENT` because another
  thread got there first; 132,880 lookups missed.
- **`--verify` finds nothing wrong**: it drains the table one entry at a time and
  checks every value against the hash it sits under — **0 corrupt** in every
  configuration, including while a migrator forces generations.
- **The enumerator survives generations.** A walk that crosses a migration, or
  reaches a slot frozen by a copy in flight, is told `ECANCELED` and restarts: 0
  cancelled of 3126 walks when nothing migrates, nearly every walk cancelled with one
  migration per millisecond, and the values it does see still all match.
- **Keeping the full hash width costs almost nothing.** On the same mix the two maps
  are indistinguishable — 14.0 M/s against 14.4 M/s at one thread, 43.0 against 42.5
  at eight — so what the full-width hash costs is the ability to remove a hash
  concurrently, and nothing else measurable.

**Limitations, on purpose**

- **The "size must be a power of two" trap is closed, and this is what closed it.**
  The probe index is `i & (size - 1)`, so a size whose mask does not cover the table
  walks a fraction of its slots — 2064 reaches only 32 of them, and a probe then never
  meets a virgin slot. Both maps now round the size up to the next power of two and
  clamp it (`mulle-concurrent-math.h`), so the 2064-slot table `--mask` builds on
  purpose is built as one of 4096 instead of never coming back. It was `fixed/26`.
- **Forgetting `mulle_aba_init()` survives a small test and dies later** — a map that
  never migrates never retires storage, so 40 inserts without a reclaimer hang where
  2 do not. The allocator prints the remedy when it aborts.
- **The hashmap has no concurrent remove**: its `patch` and `remove` are
  single-threaded writes, so that third of the mix belongs to the hashtable alone.
- One table and a shared key space, so `EEXIST` is common by construction — the point
  is that refusals are counted, not that they are rare.
- `--mask` and `--universe` run a real child process with a one-second deadline,
  because the alternative is running the demo under `timeout`.

**What it exercises.** `mulle-concurrent` (both maps), `mulle-aba`, `mulle-thread`
(the churn threads), `mulle-spawn` (the children that are allowed to hang).

The hash-space table, why the two hang modes are hang modes, and how the child
process works: [DESIGN.md](DESIGN.md).
