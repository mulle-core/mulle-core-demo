# logtop — a live dashboard for a log stream

Readers read lines through 512 circulating 4 KB buffers and hand them to N parser
threads, which tally a key field and bucket a numeric one; main redraws. Writers
only ever increment and the renderer only ever reads, so no lock can slow the
pipeline — and nothing is allocated after startup.

```sh
logtop -f /var/log/nginx/access.log -k 7 -V 9     # statuses, latency
tail -f app.log | logtop -k 3                     # tally field 3
logtop --report -k 7 -V 9 access.log              # one summary, then exit
```

Fields are whitespace separated and 1 based; `-k 0` tallies whole lines, which
turns it into a live `uniq -c`. `-h` prints the options.

**Numbers** — 5,000,000 lines / 238 MB access-log-shaped file in tmpfs,
`--report -k 7 -V 9`, best of three, this machine (64 CPUs), Release.

| parsers | wall | Mlines/s | cpu used | max RSS |
|---|---|---|---|---|
| 1 | 5.46 s | 0.92 | 16.4 s (3.0 cores) | 3.6 MB |
| 2 | 6.27 s | 0.80 | 25.1 s (4.0 cores) | 3.7 MB |
| 4 | 7.03 s | 0.71 | 42.2 s (6.0 cores) | 3.6 MB |
| 8 | 6.88 s | 0.73 | 68.8 s (10.0 cores) | 3.6 MB |
| `awk`, same tally, 1 thread | 1.39 s | 3.63 | 1.4 s (1.0 core) | 4.3 MB |
| `wc -l` — the I/O floor | 0.04 s | 135 | — | — |

**Strengths**

- **Nothing is allocated after startup.** 512 fixed 4 KB buffers circulate, so RSS
  is 3.6 MB at every thread count and whatever the input size — 238 MB of log, same
  footprint. An empty spare fifo is what throttles a reader.
- **The answer never depends on the parser count.** Tallies are per key, not per
  parser, so `-j1` and `-j8` print byte-identical reports; the suite asserts it.
- **`--report` carries no timing**, which is what lets the fixture tests compare its
  output exactly instead of within a tolerance.
- The histogram is nearly free: tally-only and tally-plus-histogram differ by 2% at
  one parser.

**Limitations, on purpose**

- **More parsers do not add throughput.** 0.92 → 0.73 Mlines/s from `-j1` to `-j8`,
  while the CPU goes from 3.0 to 10.0 cores. One reader thread feeds them, and a
  parser with an empty fifo spins on `mulle_thread_yield()` rather than sleeping —
  the extra threads burn CPU rather than doing work. (The reader is already the
  bottleneck at `-j1`: 16.4 s of CPU for 5.46 s of wall.)
- **About 4x slower than `awk` doing the same tally** on the same file, and it uses
  3 cores to get there.
- A frame is written with `mulle_fprintf` and one `fflush`, so a frame larger than
  the stdio buffer can flush mid-redraw; percentiles are bucketed; quoted fields are
  not special-cased, so an nginx request line counts as three fields; follow mode
  polls at 100 ms; the TUI needs ANSI escapes.

**What it exercises.** `mulle-concurrent` hashtable, `mulle-multififo` (both
directions), `mulle-thread`, `mulle-aba`, `mulle-data` hashing, `mulle_strtod`,
`mulle-sprintf`/`mulle-fprintf`, `mulle-time`, `mulle-allocator`.

The pipeline in detail, the option list and where the CPU goes: [DESIGN.md](DESIGN.md).
