# multigrep — parallel, mmap'd text search

N worker threads mmap files and stream matches into a lock-free fifo; the hot path
takes no lock at all — the fixed-size rings *are* the backpressure, so a slow
printer throttles the scanners rather than growing memory. `-c`, `-l` and `-T` are
ordered by input after the join, so the thread count changes discovery order and
never the answer.

```sh
git -C ../mulle-core ls-files | ./build/multigrep/multigrep -j8 PATTERN
./build/multigrep/multigrep -i -c needle src/*.c
git ls-files | ./build/multigrep/multigrep -u -c -j8 '#define'
```

`-h` prints the options. With no `FILE` arguments the path list comes from stdin;
`--` ends the options the way it does for `grep`.

**Numbers** — 1.0 GB of this workspace's own C source, tiled, in tmpfs; 64 CPUs;
Release; best of two or three; output captured in a file. Pattern
`mulle_allocator`, which matches 295,531 lines.

| corpus | multigrep -j64 | ripgrep (all cores) | ripgrep -j1 | grep -c |
|---|---|---|---|---|
| one 1.0 GB file | 5.1 GB/s | 5.4 GB/s | 5.7 GB/s | 2.5 GB/s |
| 64 x 16.5 MB | 23.3 GB/s | 35.0 GB/s | 5.1 GB/s | 2.7 GB/s |
| 500 x 2 MiB | 9.9 GB/s | 32.8 GB/s | 4.7 GB/s | 2.6 GB/s |

**Strengths**

- **Per core it is within 10% of ripgrep** (5.1 against 5.7 GB/s on one big file)
  and about 2x grep per core. Two revisions ago that gap was 4.8x.
- **It scales across files**, and the curve is in [DESIGN.md](DESIGN.md): 4.6x from
  `-j1` to `-j64` on 64 files, 2.5x on 500 small ones.
- **The answer never depends on `-j`.** 1, 2 and 8 threads produce byte-identical
  counts, which the tests assert rather than eyeball.
- The anchor is the pattern's **rarest byte, not its last one**, which halves the
  candidates: for `mulle_allocator` the last byte `r` is 3.7% of the corpus and
  the rarest, `m`, is 1.7%. The ranking is a 256-byte table, so it costs one
  lookup.
- **`memmem()` from 6 bytes of pattern up**, where the platform has one: worth
  1.14x to 1.41x there, and a *pessimization* below it (0.94x at two bytes), which
  is why the threshold sits at six.

**Limitations, on purpose**

- **A single file is one unit of work.** One 1.0 GB file is 204 ms at `-j1` and
  204 ms at `-j64`; ripgrep behaves the same way (183 against 186 ms).
- With many small files **per-file cost decides it, not per-byte cost**: 3.3x
  behind ripgrep at `-j64` on 500 x 2 MiB, because 500 `mmap`/`munmap` pairs and
  the page faults behind them are spread over the threads.
- **Windows has no `memmem`** — mingw declares none — so every pattern takes the
  prefilter there. `-i` never uses `memmem` either, because there is no
  case-insensitive one, and it costs 3.5x at a single thread.
- **Streaming matches to stdout stops scaling at 8 threads** and is *slower* at 64
  (2.1 against 1.6 GB/s): the workers fill the fixed-size ring, wait, then contend
  for it. `-c`/`-l` are what you want for throughput.
- Every mapping stays open until the run ends (one descriptor per file searched);
  `--unique` identifies a line by a 64-bit hash and then verifies the text; `-i` on
  a non-ASCII pattern walks codepoints per position.

**What it exercises.** `mulle-mmap`, `mulle-concurrent` (pointerarray, hashtable),
`mulle-thread`, `mulle-aba`, `mulle-multififo`, `mulle-utf`/`mulle-unicode`,
`mulle-fprintf`, `mulle-time`, `mulle-container`, `mulle-allocator`.

The design, the matcher's two implementations and every measurement in full:
[DESIGN.md](DESIGN.md).
