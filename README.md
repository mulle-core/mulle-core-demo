# 🌋 mulle-core-demo — terminal programs that exercise `mulle-core` under real load

Seven demos, each a real workload in which a different part of `mulle-core` does
the heavy lifting. They consume the library the way a downstream project does —
source checkout plus `add_subdirectory`, no mulle-sde, no install step. Terminal
only: nothing opens a window or draws to a canvas.

| demo | what it is | measurement | mulle-core modules |
|---|---|---|---|
|---|---|---|---|
| [multigrep](multigrep/README.md) | parallel, mmap'd text search | 23.3 GB/s on 64 files, within 10% of ripgrep per core | `mulle-mmap`, `mulle-concurrent`, `mulle-multififo` |
| [logtop](logtop/README.md) | live dashboard for a log stream | 0.92 Mlines/s, flat 3.6 MB RSS; more parsers make it slower | `mulle-multififo`, `mulle-concurrent`, `mulle-data` |
| [redis](redis/README.md) | miniredis: RESP2, one thread per connection | 10.2 M req/s at 64 connections, 4.1x redis-server at 8 threads | `mulle-concurrent`, `mulle-thread`, `mulle-socket`, `mulle-spawn` |
| [urltoolkit](urltoolkit/README.md) | URLs checked, cross-checked and rewritten | 35-URL corpus: 25 agree, 7 differ, 2 structural | `mulle-url`, `mulle-http`, `mulle-regex`, `mulle-slug` |
| [windowrb](windowrb/README.md) | many short-lived ordered sets over a stream | tree loses to array + `qsort` by ~35% | `mulle-rbtree`, `mulle-storage`, `mulle-arena` |
| [hashspace](hashspace/README.md) | the hash is the key, so the hash space is a contract | 32.2 M ops/s at 8 threads, 0 corrupt | `mulle-concurrent`, `mulle-aba` |
| [blackboard](blackboard/README.md) | processes publishing through one shared board | 7 processes, 1 mapping; the board lock is 43% of the work | `mulle-mmapallocator`, `mulle-mmap`, `mulle-spawn` |

Each number is measured on the machine named in the demo's README, with its
corpus, method and thread count; the reasoning is in its `DESIGN.md`.

## Building

`mulle-core` (and, for `blackboard`, `mulle-mmapallocator`) is looked for next to
this project. A clone with nothing beside it fetches the dependencies instead, so
this is the same either way:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Programs land under their demo's directory in the build tree:

```sh
./build/multigrep/multigrep -j8 PATTERN
./build/logtop/logtop --report -k 7 access.log
./build/redis/mredis --port 6379
./build/urltoolkit/urltoolkit --crosscheck < urltoolkit/fixtures/urls.txt
./build/windowrb/windowrb --quiet
./build/hashspace/hashspace --all --quiet
./build/blackboard/blackboard --verify
```

`ctest` runs seven tests, one per demo, as `cmake -P` scripts under each `test/`.
A single one runs by hand with its documented `-D` variables.

For a layout that is not side by side, or an offline/mirrored build, name it:
`-DMULLE_CORE_SOURCE_DIR=`, `-DMULLE_MMAPALLOCATOR_SOURCE_DIR=`,
`-DMULLE_DEMO_GIT_BASE=`, `-DMULLE_DEMO_GIT_TAG=`, `-DMULLE_DEMO_FETCH=OFF`.
