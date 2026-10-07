//
//  bench.h
//  miniredis -- load generator for the server, with a live terminal display.
//
//  Threads, not connections, are the unit: each worker owns one connection and
//  keeps it full with pipelined requests, so the measured rate is the server's
//  throughput and not the round trip time. Memory and CPU are read from the
//  server process itself (/proc/<pid> on Linux, proc_pid_rusage() on macOS),
//  so "how much is it gobbling up" is answered by the kernel rather than by
//  the keyspace's own accounting.
//
#ifndef mredis_bench_h__
#define mredis_bench_h__

#ifdef __cplusplus
extern "C" {
#endif

int   mredis_bench_main( int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif
