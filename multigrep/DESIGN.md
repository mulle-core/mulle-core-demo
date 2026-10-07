# multigrep design

How it is built and how it compares, in full. The short version — what it does,
the headline numbers and what it cannot do — is in [README.md](README.md).

## Options

```
usage: multigrep [options] PATTERN [FILE ...]

  -i, --ignore-case         case-insensitive (Unicode aware)
  -j, --jobs N              worker threads (default: 4)
  -u, --unique              report each distinct matching line once
  -c, --count               count matching lines per file
  -l, --files-with-matches  print only names of matching files
  -T, --top N               top N files by matching lines
      --chars               count columns in characters, not bytes
      --stats               print timing and throughput to stderr
  -v, --verbose             report files that could not be searched
      --                    end of options
  -h, --help                this text
```

With no `FILE` arguments the path list is read from stdin, one per line. `--` ends
the options the way it does for `grep`, so a pattern may start with a `-`:
`multigrep -c -- '->' src/*.c`.

## Design

Three roles, no locks on the hot path. A producer reads the path list into a
grow-only `mulle_concurrent_pointerarray`; N workers claim an index off an atomic
counter, wait for that entry to be published, map the file and stream matches into
a `mulle_pointermultififo`; main drains the fifo, prints, and recycles buffers
through a second fifo. The two fifos *are* the backpressure — the ring is fixed
size and `_mulle_pointermultififo_write()` fails `EBUSY` when full, so a slow
printer throttles the scanners instead of growing memory.

`-c`, `-l` and `-T` are ordered by input after the join, so the thread count
changes discovery order and never the answer; the tests assert that 1, 2 and 8
threads produce byte-identical counts.

## Where the time goes

Corpus, unless stated otherwise: **1.0 GB of real C source** — this workspace's own
`mulle-core` and demo `.c`/`.h`, tiled — measured three ways: as one file (the
16.5 MB tiles concatenated), as 64 files of 16.5 MB, and as 500 files of 2 MiB, all
in tmpfs so nothing is disk-bound. 64 CPUs, Release build, best of two or three, and
the search is `mulle_allocator`, which matches **295,531 lines**. `--stats` reports
MB/s as MiB/s.

**Measure with the output going somewhere you can read.** On this machine
`grep -c PATTERN FILE > /dev/null` answers a 1.0 GB file in **17–23 ms** (5 runs)
while the identical command writing its single line to a file takes **409–414 ms**
(5 runs). The fast one is not a scan: it issues no `write(1, ...)` at all. The
switch is `/dev/null` itself — `/dev/zero` and a regular file both take the slow
path — and there is no `LD_PRELOAD` or `BASH_ENV` here. Every grep figure measured
through `> /dev/null`, including the ones this file used to carry, is meaningless;
`rg` and `multigrep` write their line either way and are unaffected. Every number
below comes from output captured in a file.

### Scaling

`-c`, all cores free:

| jobs | 500 x 2 MiB | speedup | 64 x 16.5 MB | speedup |
|---|---|---|---|---|
| 1 | 4228 MB/s | 1.00x | 5578 MB/s | 1.00x |
| 2 | 6317 MB/s | 1.49x | 9533 MB/s | 1.71x |
| 4 | 8962 MB/s | 2.12x | 15197 MB/s | 2.72x |
| 8 | 8812 MB/s | 2.08x | 20972 MB/s | 3.76x |
| 16 | 9800 MB/s | 2.32x | 19065 MB/s | 3.42x |
| 32 | 9447 MB/s | 2.23x | 21845 MB/s | 3.92x |
| 64 | 10592 MB/s | 2.50x | 25575 MB/s | 4.59x |

The scan itself has no lock and no serial section — the two fifos are the only
handoff, and they are lock-free — but neither curve is linear, and the 8/16/32 rows
wobble by about 10%. On many small files the curve stops at 4 threads and then
flattens around 10 GB/s; on 64 files it keeps climbing to 25.6 GB/s. The section on
the matcher explains why, and it is worth reading with the previous revision in
mind: making the scanner faster *lowered* the parallel speedup (8.26x once, 4.59x
now) because per-file cost, not per-byte cost, is what is left at the top of the
curve. The faster the scan, the smaller the share of the total a worker has left to
hide.

### The matcher

Three things decide it, all of them inside `search_literal()`/`search_ascii()`:

- **The anchor is the pattern's rarest byte, not its last one.** What costs is the
  number of candidates `memchr()` hands back, and in source text two bytes of the
  same pattern differ by a factor of two or more: for `mulle_allocator` the last
  byte is `r` (3.7% of the corpus, a candidate every 27 bytes) and the rarest is `m`
  (1.7%). The rank table in the source is a 256 byte byte-frequency ranking, so the
  choice costs one table lookup and no I/O.
- **The window is verified from its ends inward**, not from the anchor backwards.
  A candidate that is not a match almost always differs in the window's last byte
  or its first, so those two comparisons throw it away, and the anchor — known to
  match — is not compared again.
- **`memmem()` does the case-sensitive search from six bytes of pattern up**, where
  the platform has one. glibc's is vectorized, and it is measurably better than any
  scalar arrangement of `memchr()` for patterns long enough to let it skip.

Best of three on the 1.0 GB file, output captured. `before` is the previous
revision (last-byte anchor, no `memmem`), `prefilter` is this revision with
`memmem` compiled out (`-DCMAKE_C_FLAGS=-DMULLE_DEMO_HAVE_MEMMEM=0`), and the last
column is what `memmem` adds on top of the prefilter:

| pattern | bytes | matching lines | before | prefilter | current | memmem gain |
|---|---|---|---|---|---|---|
| `->` | 2 | 1458404 | — | 6279 | 5924 | 0.94x |
| `zzz` (no match) | 3 | 0 | 1805 | 8192 | 8257 | 1.01x |
| `int` | 3 | 3062025 | 1101 | 2068 | 2028 | 0.98x |
| `void` | 4 | 1134512 | 1350 | 4993 | 5017 | 1.00x |
| `_mmap` | 5 | 63973 | 1438 | 4194 | 4161 | 0.99x |
| `needle` | 6 | 4199 | 952 | 3484 | 3972 | 1.14x |
| `malloc` | 6 | 46637 | 1298 | 3654 | 4297 | 1.18x |
| `mulle_alloc` | 11 | 320473 | 1396 | 3603 | 4745 | 1.32x |
| `mulle_allocator` | 15 | 295531 | 1372 | 3616 | 5115 | 1.41x |
| `mulle_mmapallocator` | 19 | 0 | 1458 | 4579 | 6132 | 1.34x |

`—` is a pattern the previous revision cannot be asked for: it read a leading `-` as
an option, which is what the `--` support above fixes.

The table is why the threshold is where it is. From 6 bytes up, `memmem` is worth
1.14x to 1.41x; below that it is a *pessimization* — down to 0.94x — because its
per-call setup is not free and a short pattern gives it almost nothing to skip. Two
bytes of `->` are the clearest case: the prefilter reaches 6279 MB/s, `memmem` 5924.
Dense patterns are slower in every column (`int`, 3,062,025 matches in 1.0 GB, is
1101–2028 MB/s) because at that density the line bookkeeping around each hit, not
the search, is the work.

The two matchers are interchangeable — both report the first occurrence in the
buffer, and both are covered by the goldens: `needle` is six bytes and takes
`memmem`, `zzz` is three and takes the prefilter. Windows gets the prefilter for
every pattern, because mingw's `string.h` declares no `memmem`; the CMake check is
`check_symbol_exists(memmem "string.h" ...)` with `_GNU_SOURCE`, which is what glibc
needs to declare it.

`-i` always uses the prefilter: there is no case-insensitive `memmem`, and the
pattern's rarest byte has to be searched in both of its cases. That is the 3.3x
between the two paths in the last table of this section.

### Against grep and ripgrep

Best of three, output captured, `mulle_allocator`, `-c`:

| corpus | grep | rg -j1 | rg (all cores) | multigrep -j1 | multigrep -j64 |
|---|---|---|---|---|---|
| one 1.0 GB file | 414 ms (2533 MB/s) | 183 ms (5730 MB/s) | 186 ms | 204 ms (5140 MB/s) | 204 ms (5140 MB/s) |
| 64 x 16.5 MB | 387 ms (2709 MB/s) | 205 ms (5115 MB/s) | 30 ms (34953 MB/s) | 192 ms (5461 MB/s) | 45 ms (23302 MB/s) |
| 500 x 2 MiB | 399 ms (2628 MB/s) | 222 ms (4723 MB/s) | 32 ms (32768 MB/s) | 245 ms (4280 MB/s) | 106 ms (9892 MB/s) |

Three things to read out of that.

**Per core, the gap to ripgrep is now 10%, and grep is 2x behind.** It was 4.8x
behind `rg -j1` two revisions ago and 2.2x behind one revision ago; on this pattern
it is 5140 against 5730 MB/s, 5461 against 5115 on 64 files and 4280 against 4723 on
500 small ones. The remaining 10% on one big file is what SIMD-over-the-whole-buffer
buys over `memmem` plus a prefilter for everything shorter than six bytes, and it is
a much smaller number than the 4.8x it replaced.

**A single file is one unit of work.** 1.0 GB is 64 threads' worth of nothing:
`-j1` and `-j64` are the same 204 ms, and `rg` behaves the same way (183 against
186 ms), because neither splits a file. Parallelism here is across files only.

**With all cores, file granularity decides it.** 1.5x behind ripgrep on 64 big
files, 3.3x behind on 500 small ones, and the difference between those columns is
per-file cost rather than per-byte cost: 500 `mmap`/`munmap` pairs and the page
faults behind them, spread over 64 threads.

### -i, -u and the printer

64 x 16.5 MB, `-c` unless stated:

| mode | jobs | throughput |
|---|---|---|
| `-c` | 1 | 5433 MB/s |
| `-c -i` | 1 | 1567 MB/s |
| `-c` | 64 | 24385 MB/s |
| `-c -i` | 64 | 21845 MB/s |
| `-c -u` | 64 | 23831 MB/s |
| matches to stdout | 1 | 1691 MB/s |
| matches to stdout | 8 | 2101 MB/s |
| matches to stdout | 64 | 1636 MB/s |

`-i` costs 3.5x at one thread — the prefilter asks for the anchor's next occurrence
twice, once per case, and folds on every comparison — and 11% at 64 threads, where
per-file cost has taken over from matching. `--unique` is 2% at 64: hashing and
verifying every distinct line is not what this program spends its time on.

Streaming is the backpressure design working as designed, and the reason to prefer
`-c`/`-l` if you want throughput. It stops scaling at 8 threads because the printer
is one thread, and at 64 it is *slower* than at 8 (1691 against 2101 MB/s): the
workers fill the fixed-size ring, wait, and then contend for it. A terminal wants
matches as they are found; a pipeline wants the counts.

## The rest of the limitations

Every mapping stays open until the run ends (results point into them, so it holds
one descriptor per file searched); Unicode folding is simple folding, no expansion
or multi-character folds; `-i` with a non-ASCII pattern walks codepoints per
position; `--unique` identifies lines by a 64 bit hash and then verifies the text;
the matcher is `memmem()` where the platform has one and a `memchr()`-on-a-rare-byte
Horspool prefilter everywhere else, so Windows has no `memmem` and `-i` never does,
which is the 3.3x between the two paths; and a single file is scanned by a single
worker, so one huge file is one core's worth of throughput no matter how many `-j`
you pass.

**Exercises** `mulle-mmap`, `mulle-concurrent` (pointerarray, hashtable),
`mulle-thread`, `mulle-aba`, `mulle-multififo`, `mulle-utf`/`mulle-unicode`
(folding, character columns), `mulle-fprintf`, `mulle-time`, `mulle-container`,
`mulle-allocator`.
