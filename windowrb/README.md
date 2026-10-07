# windowrb — many short-lived ordered sets over a stream

A stream of values, and windows over it: a window opens every `--start-every`
samples while fewer than `--max-active` are open, every sample goes into every open
window, and a window that reaches `--window-size` is queried for its median and its
outlier count and then destroyed. The windows are deliberately short-lived, so
creating and tearing down an ordered set is a visible part of the run rather than a
rounding error.

```sh
windowrb --stream-len 200000 --window-size 128 --max-active 8 --live
windowrb --stdin < sensor.log --window-size 128 --threshold 3 --verify
windowrb --quiet --tail 3            # one summary and the last few reports
```

`-h` prints the options. `--verify` runs both backends and insists they agree on
every report; `--baseline` is the array + `mulle_qsort_r` one on its own.

**Numbers** — Release build, best of three, threshold 2.0.

| window | samples | `mulle-rbtree` | array + `mulle_qsort_r` |
|---|---|---|---|
| 64 | 20,000 | 5.5 ms | 4.1 ms |
| 128 | 200,000 | 59.8 ms | 44.1 ms |
| 512 | 200,000 | 72.3 ms | 54.9 ms |

**Strengths**

- **It answers its own question honestly.** "Is a tree the right structure here?" —
  at one query per window, no, and the demo carries the baseline that says so
  instead of leaving it as an exercise for the reader.
- **A sample lives *in* its node**: `node_extra = sizeof(double)` with duplicates
  allowed, so an insert copies eight bytes and allocates no payload. That is what
  makes a multiset of measurements cheap.
- **A window's memory is released with it.** Each window owns a `mulle_arena` and
  that arena is the tree's allocator, so teardown is a single release: 1250 windows
  at the defaults, each holding one 4 KB page at its peak, and the process ends with
  nothing outstanding — the demo runs clean under ASan **with leak detection on**.
- The query the tree *is* good at is the outlier count: a walk in from the tail over
  the outliers, rather than over the window.

**Limitations, on purpose**

- **At one query per window the tree loses to array + `qsort` by about a third**, at
  every window size. That is the demonstration, and the numbers are in the table
  rather than in a footnote.
- **`--shared-arena` is the other half of that trade**: one arena for every window
  holds 833 pages for the same run and never gives any of them back, because an
  arena's `free` is a no-op except for the most recent allocation. Fewer
  allocations, no reclamation.
- **Single-threaded.** `mulle-rbtree` and `mulle-storage` are not thread-safe, and
  concurrency is miniredis's story, not this one.
- **Windows only grow.** A window that reaches `--window-size` is answered and
  destroyed rather than sliding, which is what keeps the sets short-lived.
- The node pool's bucket size is fixed at 32, so a 16-sample window still reserves
  32 nodes.
- Concurrency cannot exceed `window-size / start-every`, and the summary says so when
  `--max-active` is not the binding limit — otherwise "peak 4 of 8" reads as a bug.
- The generator is the demo's own (xorshift64* with Box-Muller) because `mulle-core`
  has no random numbers; `--seed` is what makes a run reproducible. An outlier is
  `|value| > threshold` on both sides, since the stream is symmetric.

**What it exercises.** `mulle-rbtree` (extra mode, duplicates, forward and reverse
enumerators), `mulle-storage` (`mulle_indexedstorage` for the window pool,
`mulle_storage` inside every tree, `mulle-arena` as an allocator), `mulle-data`
(`mulle_qsort_r` in the baseline), `mulle-buffer` + `mulle-dtostr` for input,
`mulle-time`, `mulle-fprintf`, `mulle-allocator`.

Where the tree earns its keep, and the storage measurements in full:
[DESIGN.md](DESIGN.md).
