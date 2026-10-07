# blackboard — processes publishing through one shared board

Seven processes — a monitor, four publisher workers and two readers — over one 256
KiB shared region. Nothing is copied on the way in except the bytes a reader
chooses to look at, and a note allocated by worker 3 is read *and freed* by a
reader: `mulle_allocator_free()` on a pointer that came out of another process's
allocation, refcounted once per interested subscriber.

```sh
./build/blackboard/blackboard                       # 4 workers, 1 monitor + 2 readers
./build/blackboard/blackboard --show 8 --tui
./build/blackboard/blackboard --unsafe --verify     # the same run, tally unlocked
./build/blackboard/blackboard --exhaust              # leave the board no room
./build/blackboard/blackboard --grow --growth-limit 1M  # expand instead of refusing
./build/blackboard/blackboard --dump --quiet        # the allocator's view of the board
```

`--help` prints the options. `--unsafe` is the test hook that reads the tally,
publishes and writes it back without the lock.

```
=== Blackboard ===
 region            256.0 KiB shared, one mapping, 7 processes
 board overhead    6.3 KiB (2.5 % of the region)
 published         80000 notes, 0 skipped (no subscriber)
 payload           6.1 MiB in 80000 notes, 79 B average
 deliveries        171212 arrived, 8837 dropped (mailbox full), 0 lost
 allocator         0 failures, 80000 allocations, 80000 frees
 board heap        0.0 B live, 69.8 KiB peak, 256.0 KiB of the region
 contents          3 subscribers verified 171212 payloads, 0 corrupt
 wall clock        144.4 ms   553854 notes/s   6.1 MiB/s   1246511 deliveries/s
```

**Strengths**

- **Cross-process refcounting, not message passing.** A note is a header plus a
  payload, refcounted once per subscriber, so one allocation serves three readers
  and the last one to release it is the one who frees it.
- **Growth arrives in a process that did not create it.** With `--grow` the window
  is mapped once, at the monitor's address, and a worker that runs out of room
  extends it — the capacity moves, the segment count does not, because the window
  is one mapping. Readers then take notes out of bytes they never allocated
  (`readers took N notes from grown segments`), which is exactly the property the
  raw note pointers depend on.
- **`--verify` asserts, it does not print**: every note accounted for, deliveries
  conserved, board empty, contents intact — exit nonzero if any of it drifts.
- **The lock is measured in both directions.** `--unsafe` shows what dropping it
  costs (a tally a monitor reads is wrong: 59,788 lost updates) and the profile
  shows what keeping it costs (43% of all cycles at 8 workers).
- `valgrind --trace-children=yes` runs all seven processes clean, growth included,
  and finds no access outside the mapping.

**Limitations, on purpose**

- **One global board lock, and the numbers show it.** Adding publisher *processes*
  makes the work slower per note, not faster: 1.50 µs per note at 1 worker, 2.89 µs
  at 8. The notes and every counter are single-field atomics, so the read and free
  paths take no board lock at all.
- The mailbox depth is fixed at 256 and there is **no blocking read**, so idle
  readers poll, and a **full mailbox drops the note** rather than stalling the
  publisher (8,837 dropped above, by design).
- `--capacity` is a hard ceiling unless `--grow` raises it to `--growth-limit`,
  which is itself hard. A full board drops rather than growing without bound.
- **Address space is what the window costs.** The mapping has to be free at the
  monitor's *exact* address in every child; a very large window may not be, and then
  `attach` refuses the child cleanly instead of mapping it somewhere else.
- **ASan cannot run this demo.** The child's mapping fails because ASan's own
  address-space reservation is not free, and the monitor reports a dead worker — a
  run without `--grow` fails identically, which is what tells the two apart. The
  address requirement is checked in the allocator's own `test/30-shared/grow.c`,
  which attaches wherever the system likes.
- Neither valgrind nor ASan can see an overrun *inside* the window: the whole region
  is one mapping, so there is no redzone between notes. The arena's own invariants
  are `--verify`'s job.
- `--unsafe` needs the workers to overlap to lose anything, so the suite runs it at
  100,000 notes rather than the default 20,000.

**What it exercises.** `mulle-mmapallocator` (the locking arena in a reserved
window, its growth, and `_mulle_mmap_allocator_dump`), `mulle-mmap`, `mulle-spawn`
(the workers and readers as real processes), `mulle-thread` (C11 atomics over shared
pages, which is what makes a refcount work *between processes*), `mulle-fifo` (a
fixed ring as a mailbox), `mulle-time`, `mulle-allocator`, `mulle-fprintf`.

The board layout, how the processes are started, the growth scheme, and the measured
cost of the lock: [DESIGN.md](DESIGN.md).
