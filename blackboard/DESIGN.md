# blackboard design

The demo puts one shared region under seven processes: a monitor, four worker
publishers and two readers. This file covers how the board is laid out, how the
processes are started, how it grows, and what measurements were taken. The short
version — how to build, run and check it — is in [README.md](README.md).

## Design

The board header, the subscriber table, the mailboxes and every note live in one
`mulle_mmap_allocator` region, so a note allocated by worker 3 is read *and freed*
by a reader process: `mulle_allocator_free()` on a pointer that came out of another
process's allocation. A note is a variable-length header plus its payload,
refcounted once per interested subscriber, so one allocation serves three readers
and the last one to release it is the one who frees it.

- **The board lock** is one atomic spin lock in the shared region, serializing
  writers. It covers the board structure, not the heap: the note refcounts and
  every counter are single-field atomics, so the read path and the free path take
  no board lock at all.
- **`_mulle__pointerfifo256`** is the per-subscriber mailbox, one per process,
  embedded — no allocation, and no allocator pointer inside it. It is fixed size
  and single-producer/single-consumer by contract, which is what makes it safe
  here: writers are serialized by the board lock, and each mailbox has exactly one
  reader. A full mailbox drops the note instead of stalling the publisher.
- **Running out of room is normal**, not fatal: `--exhaust` takes the region down
  to a few pages *and* the payload up to the largest the demo allows, so the notes
  cannot fit on any platform — the payload is what does the work, because at the
  largest the demo allows a note is a whole page, so only a handful fit alongside
  the board. The board counts the refusals — how many notes get through varies per
  run, because it is a scheduling story — but published + refused is always the
  80,000 that were offered, which is what the suite asserts. The
  recovery is a `longjmp` out of the allocator's `fail` vector, which is the only
  funnel a public allocation has, and it is sound because `fail` is called *after*
  the arena call has returned, so the shared heap is consistent when the jump
  lands.

## How the processes are started

Nothing is forked. The monitor starts each worker and reader with
`mulle_spawn_open`, hands it the mapping and the address the board is at, and the
child maps the region there itself:

```
blackboard <the monitor's own options> --internal-child worker 0 <handle> <base> <capacity> <board>
```

The child is the monitor's command line plus those five values, so it parses the
identical options and runs the identical demo; `--internal-child` is the flag
`hashspace` uses for the same job, and it is not in `--help` because it is not an
option a person types. It is the same re-attach
`mulle-mmapallocator`'s own `test/30-shared/process.c` performs, and what travels
is whatever a handle is on the platform — a descriptor, a `HANDLE` — as hex.
Streams are inherited, so a child writes where the monitor writes, and the monitor
reaps with `mulle_spawn_wait(h, 0, ...)`, which is the poll `waitpid`'s `WNOHANG`
used to be.

Two consequences of dropping `fork()` are worth knowing:

- **The child's options are the command line and nothing else.** `fork()` copies
  the parent's parsed struct; a spawned child re-parses. What the monitor settles on
  privately after parsing therefore never reaches one — which is why `--exhaust`
  says what it means *while parsing* (`--capacity` is handed over separately for the
  same reason).
- **The mapping has to land at the monitor's address.** `attach` validates that it
  did and refuses the child if it did not, which is a clean failure rather than a
  wrong one. That was the open question about running this on Windows, and it is now
  a measured one: the Wine run finds the address free in every child.

The memory-checker consequences of that requirement are in [README.md](README.md).

## What the memory checkers can and cannot see

That same requirement decides which memory checkers can see this program at all.
`valgrind --trace-children=yes` is the one that can: it traces all seven
processes — the monitor, the four workers and the two readers — and the run is
clean, growth included. The flag is not optional. A child is an `exec`, so
without it the children run uninstrumented and only the monitor is checked.

Address sanitizer cannot run the demo, and it is not the growth code that stops
it. A child has to map the window at the monitor's *exact* address, and ASan's own
address-space reservation is not free in the child, so the mapping fails and the
monitor reports the worker dead:

```
$ ./blackboard --exhaust --grow --growth-limit 64K --verify --quiet
mulle-allocator: memory allocation of 65536 bytes failed
blackboard: worker 1 died (exit 0, status 3)
```

The 65536 is the window the child could not place.

A run without `--grow` fails the same way, which is what tells the two apart. The
reserved window is not left unchecked, though: it runs clean under ASan in the
allocator's own `test/30-shared/grow.c`, which attaches wherever the system likes.

Neither tool sees an overrun *inside* the window, and neither should be expected
to: the whole region is one mapping, so there is no redzone between the notes for
them to trip over. What they do confirm is that growth and the cross-process
access never reach outside the mapping. The arena's own invariants are `--verify`'s
job.

## Where the bottleneck is

The board is one critical section, so adding publisher *processes* makes the work
slower per note, not faster — on a 64 CPU machine, twenty thousand notes per
worker:

| workers | notes | wall clock | notes/s | cost per note |
|---|---|---|---|---|
| 1 | 20,000 | 30.0 ms | 667,295 | 1.50 us |
| 2 | 40,000 | 63.2 ms | 632,615 | 1.58 us |
| 4 | 80,000 | 147.1 ms | 543,819 | 1.84 us |
| 8 | 160,000 | 463.1 ms | 345,483 | 2.89 us |

The profile says where it goes: at 8 workers **43% of all cycles are spent
acquiring the board lock** — the single largest item by far, and everything else
in the publisher (allocate, fill the payload, checksum it, enqueue to three
mailboxes) together is under 3%. At 1 worker the lock does not appear in the
profile at all; there the largest single item is a reader's `sched_yield` backoff
at 8%, because a mailbox has no blocking read and an idle reader polls.

Payload size stops predicting the time once the run is lock-bound, and in neither
direction reliably: at 4 workers 1 KB notes sometimes win and sometimes lose
against 17 byte ones, and at 8 workers the order flips back. At **one** worker,
where the bytes are what is left to measure, they cost what they should — best of
three, 100,000 notes each: `--payload 17` in 128.7 ms, `--payload 1024` in 246.0
ms.

## What the lock is worth, measured

`--unsafe` reads the tally, publishes, and writes it back — the realistic way to
get this wrong — with the mailboxes still serialized. The notes all arrive; what
breaks is the count a monitor reads:

```
   tally            20212 published + 0 refused of 80000 offered, 59788 lost updates
                    the notes themselves are accounted for below; it is the count a monitor reads that is wrong
 verification      FAILED (1)
```

Racing the mailboxes *as well* is not a bigger number, it is a hang: two writers
both increment the ring's count, the consumer then reads slots nobody wrote and
slots pointing at notes that have been freed again, and a reader waiting for the
count to return to zero waits forever. That is why the readers here shut down on
quiescence once the workers are reaped, and why `--unsafe` races the tally rather
than the ring.

## Growth

`--grow` takes the same board and lets it expand instead of refusing. What is
reserved for it is the ceiling, `--growth-limit`: the mapping is made once, at the
monitor's address, and the arena starts as a first segment of `--capacity`. When
an allocation finds that segment full, the allocator grows it — twice the last
one, or the rest of the window, whichever is smaller — and retries. Past the
limit a request fails the ordinary way, so `--exhaust --grow` is the same
demonstration with a taller board: it grows to the limit, then refuses and counts.

Because the window is a single mapping, growing it *extends the segment it
already has*: the capacity moves and the segment count does not. Every other
embedder adds a segment, each one its own mapping, and the two paths differ only
in whether the embedder's new range continues the segment before it. Here it
always does, so `growth` reads `1 segment` before and after a run — the number
that moves is the capacity.

The window is mapped, not committed: the pages appear when the board first
touches them, so what the reserve costs is address space. Nothing on disk backs
it, because nothing has to — every process that attaches already holds the
mapping's handle, so there is no one to *discover* the region, which is the only
thing a file would buy. What the reservation does cost is the address itself: the
window has to be free at the monitor's address in every child. A small window
always is; a very large one need not be, and then `attach` refuses the child
instead of mapping it somewhere else, which is the clean failure described above.

The constraint that shapes this is the note pointers. A mailbox slot is a raw
`void *` into the board, so every process has to resolve every note to the same
address. A new segment mapped wherever the kernel pleases would break that; a
window mapped once and expanded into cannot, because every process maps the same
window at the same address before any of them runs. That is also why the
reserved window is the growth mechanism here rather than dlmalloc's
file-backed one: the allocator underneath can map each segment anywhere, and
this demo deliberately cannot use that freedom.

```
 region            16.0 KiB shared growing to 64.0 KiB, one mapping, 7 processes
 ...
 growth            1 segment, 64.0 KiB of 64.0 KiB limit
 growth source     monitor added 0 segments, 1 of 4 workers added 1, readers took 7243 notes from grown segments
```

`growth source` says *who* did the growing, because that is the interesting part.
A worker grows the board when its own allocation runs out of room, so the grow
belongs to a child process and not to the monitor — which matters here, because
the monitor is the one that mapped the window and could have been the only
grower by accident. The board's own allocation fits in the first segment, so the
monitor adds nothing; the workers are what fill it.

"added 1" on that line is one successful `grow`, which in this embedder is one
extension rather than one new segment — the counter is per process and counts
grows, so it reads the same whether the range appended a segment or extended one.

<!-- -->

The readers are the sharpest witness: they never allocate at all, so they cannot
grow anything themselves, yet they take notes out of bytes a worker grew —
`readers took N notes from grown segments` counts exactly that. It is the growth
arriving in a process that did not create it, which is the property the note
pointers depend on.

## Limitations, on purpose

The mailbox depth is fixed at 256 — the fifo family is fixed-size structs — and one
global lock is a deliberate simplification: the notes show what it costs. There is
no blocking read, so idle readers poll. `--capacity` is a hard ceiling unless
`--grow` raises it to `--growth-limit`, which is itself hard: a full board drops
rather than growing without bound, and a board told not to grow drops as before.

One limitation belongs to the host rather than the design, and the suite names it
instead of pretending otherwise:

- **`--unsafe` needs the workers to overlap to lose anything.** At the default
  20,000 notes a worker is done in ~10 ms, which fits inside starting six processes
  on a slow host (Wine needs ~200 ms) and no update is then lost; the suite runs it
  at 100,000 notes for that reason.
