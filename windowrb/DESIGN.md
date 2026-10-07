# windowrb design

The window machinery, the tree-versus-array measurement and what a window costs.
The short version is in [README.md](README.md).

## Options

```
usage: windowrb [options] [FILE]

  -n, --stream-len N     generated samples (default 20000)
  -W, --window-size W    samples per window (default 64)
  -M, --max-active M     windows alive at once (default 8)
  -S, --start-every S    open a window every S samples (default 16)
  -T, --threshold T      |value| above this is an outlier (default 2.0)
      --seed S           seed for the generated stream (default 42)
      --stdin            read samples from stdin
  -q, --quiet            only the summary
      --tail N           with --quiet, print the last N reports
      --baseline         also run windows backed by an array + mulle_qsort_r
      --verify           run both and check that they agree
      --shared-arena     one arena for all windows instead of one each
      --live             redraw an active-window bar
      --delay-ms MS      pace the stream by MS per sample
```

## Design

The ordered set is a `mulle_rbtree` in extra-memory mode with
`node_extra = sizeof(double)` and
`mulle_rbtree_option_allow_duplicates`, so a sample lives *in* its node: an insert
copies eight bytes and allocates no payload, which is what makes a multiset of
measurements cheap. Each window owns a `mulle_arena`, and that arena is the tree's
allocator, so a window's nodes are a page or two and destroying the window is a
single release; the window objects themselves come from a `mulle_indexedstorage`
pool whose freed slots are reused, so thousands of window lifetimes do not churn
the system allocator. The median is an in-order walk to the middle, and each tail
is counted by walking in from its end while the value is an outlier — the one query
this structure answers more cheaply than it built itself.

**What it exercises.** `mulle-rbtree` (extra mode, duplicates, forward and reverse
enumerators), `mulle-storage` (`mulle_indexedstorage` for the window pool,
`mulle_storage` inside every tree, `mulle-arena` as an allocator), `mulle-data`
(`mulle_qsort_r` in the baseline), `mulle-buffer` + `mulle-dtostr` for input,
`mulle-time` for the wall clock, `mulle-fprintf` for the report, `mulle-allocator`.

## Is a tree even the right structure here?

For one query per window, no — and the demo says so in its own output, because
`--baseline` runs the same windows as a plain arena array plus `mulle_qsort_r` at
the query, and `--verify` insists the two backends agree on every report. Best of
three runs, threshold 2.0, Release build:

| window | samples | `mulle-rbtree` | array + `mulle_qsort_r` |
|---|---|---|---|
| 64 | 20,000 | 5.5 ms | 4.1 ms |
| 128 | 200,000 | 59.8 ms | 44.1 ms |
| 512 | 200,000 | 72.3 ms | 54.9 ms |

The baseline wins by about a third at every size, which is the honest answer to
"why not a buffer": at one query per window, sorting once beats maintaining order.
What the tree answers *cheaply* is the tail count — a walk over the outliers rather
than the window — and the run's cost follows that: at 512-value windows and a
threshold low enough to put 471 of 512 values in the tail, the tree costs 76.4 ms
against 72.3 ms when the tails are near-empty, while the array stays flat at ~54
ms. The tree would earn its keep with repeated, differently shaped
order-statistic queries over a window that is updated between them, which this
demo does not measure.

## Storage, measured rather than asserted

Per-window arenas mean the memory for a window is released with it: at the
defaults, 1250 windows are created and released, each holding one 4 KB page at
its peak, and the process ends with nothing outstanding — the demo runs clean
under ASan *with leak detection on*. `--shared-arena` is the experiment: one
arena for every window holds 833 pages for the same run and never gives any of
them back, because an arena's `free` is a no-op except for the most recent
allocation. A shared arena trades reclamation for fewer allocations, and now that
trade is a number in the summary. The tree's own node pool appears there too: its
bucket size is fixed at 32, so a 16-sample window still reserves 32 nodes.

## The rest of the limitations

Single-threaded: `mulle-rbtree` and `mulle-storage` are not thread-safe, and
concurrency is miniredis's story, not this one. The median is a walk, so at large
windows this demo is a demonstration of having brought the wrong structure —
deliberately, and with the numbers. Windows only grow: a window that reaches
`--window-size` is answered and destroyed rather than sliding, which is what makes
the set short-lived. Concurrency cannot exceed `window-size / start-every`, and
the summary says so when `--max-active` is not the binding limit, because "peak 4
of 8" otherwise reads as a bug. The generator is ours (xorshift64* with
Box-Muller) rather than `mulle-core`'s, which has no random numbers, and `--seed`
is what makes a run reproducible. An outlier is `|value| > threshold` on both
sides, since the stream is symmetric.
