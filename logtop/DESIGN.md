# logtop design

How the pipeline is put together and what the numbers behind the README are. The
short version is in [README.md](README.md).

## Options

```
usage: logtop [options] [FILE ...]

  -f, --follow          keep reading after end of input
  -n, --lines N         stop after N lines
  -j, --jobs N          parser threads (default 2)
  -k, --key N           field to tally (default 1)
  -V, --value N         numeric field for the histogram
  -t, --top N           how many keys to show (default 8)
  -i, --interval SEC    redraw interval (default 1.0)
  -w, --window SEC      throughput history (default 60)
      --bucket-ms MS    histogram bucket width (default 10)
      --report          process everything, print one summary, exit
      --tui             force ANSI redraw
      --no-tui          force plain output
```

Fields are whitespace separated and 1 based; `-k 0` tallies whole lines, which
turns it into a live `uniq -c`. A report looks like this:

```
logtop report: 8 lines, 0 dropped
keys (field 7, top 3 of 3)
50.0%         4  200
25.0%         2  404
25.0%         2  500
histogram (field 9, 10 ms buckets, 8 values)
0-10ms            4  50.0%  ##########
20-30ms           1  12.5%  ##
```

Live mode adds a throughput sparkline above that, redrawn in place with ANSI
escapes when stdout is a terminal, or printed as a plain report per interval when
it is not.

## Design

Readers take a line buffer from a spare fifo, read a line into it and push it to
the content fifo; N parsers tally the key field and bucket the numeric one; main
redraws. Writers only ever increment and the renderer only ever reads, so no lock
can slow the pipeline. 512 fixed 4 KB buffers circulate through the fifos — no
allocation after startup, and an empty spare fifo is what throttles a reader.
`-n` claims line numbers before reading so it reads exactly N lines. Snapshots
restart on the enumerator's documented `ECANCELED`, and follow mode treats EOF as
"nothing yet" (`clearerr()` and poll) rather than reopening.

`--report` prints no timing at all, on purpose: that is what lets the fixture tests
compare its stdout exactly. The live mode's numbers are the dashboard's, and they
are not what the tests assert.

**Exercises** `mulle-concurrent` hashtable, `mulle-multififo` (both directions),
`mulle-thread`, `mulle-aba`, `mulle-data` hashing, `mulle_strtod`,
`mulle-sprintf`/`mulle-fprintf`, `mulle-time`, `mulle-allocator`.

## Throughput, measured

5,000,000 lines / 238 MB of access-log-shaped input in tmpfs, `--report -k 7 -V 9`,
best of three wall-clock runs, GCC `-O3 -DNDEBUG` (Release), this machine (64 CPUs).
CPU is `user + sys` from `getrusage`, over the same runs.

| parsers | wall | Mlines/s | user | sys | cpu | max RSS |
|---|---|---|---|---|---|---|
| 1 | 5.46 s | 0.92 | 12.81 s | 3.58 s | 16.4 s (3.0 cores) | 3.6 MB |
| 2 | 6.27 s | 0.80 | 20.56 s | 4.55 s | 25.1 s (4.0 cores) | 3.7 MB |
| 4 | 7.03 s | 0.71 | 35.74 s | 6.43 s | 42.2 s (6.0 cores) | 3.6 MB |
| 8 | 6.88 s | 0.73 | 58.76 s | 10.01 s | 68.8 s (10.0 cores) | 3.6 MB |

Tally only (no histogram), best of three: `-j1` 5183 ms, `-j2` 6037 ms, `-j4`
6467 ms, `-j8` 6403 ms — so the histogram is worth about 2% and is not what any of
this is about.

Two references on the same file, on the same machine:

| | wall | Mlines/s | cpu | max RSS |
|---|---|---|---|---|
| `awk '{c[$7]++} END{…}'` | 1.39 s | 3.63 | 1.4 s (1.0 core) | 4.3 MB |
| `wc -l` | 0.04 s | 135 | — | — |

`wc -l` is the floor rather than a competitor — it is 37 ms for 238 MB, so nothing
here is I/O bound — but it does show how much room there is.

### Why more parsers don't help

**Throughput falls as parsers are added, and CPU rises.** The shape of the code
says why. There is one reader thread, and it is the producer; the parsers are
consumers. A parser with an empty content fifo does not block — it spins:

```c
s = (struct sample *) _mulle_pointermultififo_read_barrier( &content_fifo);
if( s) { parse_line( s); sample_release( s); continue; }
if( ! mg_load( &readers_active)) break;   /* readers are done: the end */
mulle_thread_yield();
```

and the reader does the same when the fifo is full, which `logtop.c` labels
"backpressure, not a failure". So with `-j8` there are eight spinning parser
threads and one producer to feed them. Even at `-j1` the process spends 16.4 s of
CPU to do 5.46 s of wall clock, which is the reader, one parser and main busy-waiting
on each other rather than any of them blocking.

That is a design property, not a measurement artefact: it is the price of the
lock-free hand-off. It is also the honest answer to "why not more threads" — the
parsing is not what the run spends its time on, and the extra threads are what makes
that visible.

## The rest of the limitations

A frame is written with `mulle_fprintf` and one `fflush`, so a frame larger than
the stdio buffer can flush mid-redraw (building it in a `mulle_buffer` and writing
once would fix it); percentiles are bucketed; quoted fields are not special-cased,
so an nginx request line counts as three fields; follow mode polls at 100 ms; the
TUI needs ANSI escapes.
