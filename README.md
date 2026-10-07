# mulle-core-demo — terminal programs that put `mulle-core` on the critical path

Seven demos, each one a real workload rather than a unit test, chosen so that a
different part of `mulle-core` ends up on the critical path. They consume the library
the way a downstream project does — a source checkout plus `add_subdirectory`, no
mulle-sde and no install step — so what they prove is what a consumer can actually
build. Every demo is terminal-only: nothing opens a window or draws to a canvas.

| demo | what it is | the number to remember | on the critical path |
|---|---|---|---|
| [multigrep](multigrep/README.md) | parallel, mmap'd text search | 23.3 GB/s on 64 files, within 10% of ripgrep per core | `mulle-mmap`, `mulle-concurrent`, `mulle-multififo` |
| [logtop](logtop/README.md) | a live dashboard for a log stream | 0.92 Mlines/s, flat 3.6 MB RSS — and more parsers make it slower | `mulle-multififo`, `mulle-concurrent`, `mulle-data` |
| [redis](redis/README.md) | miniredis: RESP2, one thread per connection | 10.2 M req/s at 64 connections, 4.1x redis-server at 8 threads | `mulle-concurrent`, `mulle-thread`, `mulle-socket`, `mulle-spawn` |
| [urltoolkit](urltoolkit/README.md) | URLs checked, cross-checked and rewritten | 35-URL corpus: 25 agree, 7 differ, 2 structural | `mulle-url`, `mulle-http`, `mulle-regex`, `mulle-slug` |
| [windowrb](windowrb/README.md) | many short-lived ordered sets over a stream | the tree loses to array + `qsort` by ~35%, and says so | `mulle-rbtree`, `mulle-storage`, `mulle-arena` |
| [hashspace](hashspace/README.md) | the hash *is* the key, so the hash space is a contract | 32.2 M ops/s at 8 threads, 0 corrupt; one way left to hang | `mulle-concurrent`, `mulle-aba` |
| [blackboard](blackboard/README.md) | processes publishing through one shared board | 7 processes, 1 mapping; the board lock is 43% of the work | `mulle-mmapallocator`, `mulle-mmap`, `mulle-spawn` |

Every number is a measurement on the machine in the demo's own README, which names
its corpus, method and thread count. The reasoning behind each one is in that demo's
`DESIGN.md` — separate on purpose, so the README stays a page.

## Building

`mulle-core-demo` looks for `mulle-core` next to it — and, for `blackboard`,
`mulle-mmapallocator` next to that as well. A clone with nothing beside it fetches
what it is missing instead, so this is the same either way:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

The programs land in the build tree under their own directory, which is what every
README's examples assume:

```sh
./build/multigrep/multigrep -j8 PATTERN
./build/logtop/logtop --report -k 7 access.log
./build/redis/mredis --port 6379
./build/urltoolkit/urltoolkit --crosscheck < urltoolkit/fixtures/urls.txt
./build/windowrb/windowrb --quiet
./build/hashspace/hashspace --all --quiet
./build/blackboard/blackboard --verify
```

`ctest` runs seven tests, one per demo. They are `cmake -P` scripts under each
`test/`, so a single one can be run by hand with its documented `-D` variables; the
fixture tests are exact-output comparisons, and the measurement modes assert the
numbers they print.

Local checkouts, offline and mirrored builds, and the Windows cross build are in
[BUILDING.md](BUILDING.md) — including the `-DMULLE_*_SOURCE_DIR` and
`-DMULLE_DEMO_FETCH` knobs and the one thing that used to be a trap (`blackboard`
needs `mulle-mmapallocator`'s `src/mulle-dlmalloc`, which a fresh clone of that
project does not have).

## Reports

`bugs/` is the triage log for what the demos turned up in the libraries
underneath them; a closed report moves to `fixed/`, a withdrawn one to
`retracted/`. The first one closed is `fixed/26-…` — a non-power-of-two table size
used to hang both concurrent maps, and the size is now rounded up before the probe
mask is derived. The process is described in [BUILDING.md](BUILDING.md).
