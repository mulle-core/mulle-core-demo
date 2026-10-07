//
//  bench.c
//  miniredis -- load generator with a live terminal display.
//
//  What it measures, and why it is shaped this way:
//
//   * Requests per second is the server's rate, not the round trip time, so
//     every worker keeps its connection full of pipelined requests (`--pipeline`
//     of them) and reads that many replies back. The server drains a whole
//     batch per write, so the reading is what one connection can push.
//
//   * MB/s is counted on the wire, not estimated from the payload: `bytes_in`
//     is what we wrote and `bytes_out` is every byte `client.c` pulled off the
//     socket, counted in both its buffered and its unbuffered read path.
//
//   * Memory is the kernel's answer, not ours: /proc/<pid> on Linux and
//     proc_pid_rusage() on macOS, always about the *server* process.
//     "How much is it gobbling up" should not be answered by the code under
//     test.
//
//   * The workload is half `SET`, half `GET`, over a fixed small keyspace, and
//     the keyspace is filled before measurement starts so no lookup can miss.
//     A read must return a value of exactly the size that was written, so a
//     wrong reply is counted as an error rather than being timed.
//
//  Formatting avoids `%f` on purpose -- mulle-sprintf documents no conversion
//  table -- so rates are printed from scaled integers.
//

#include "bench.h"
#include "client.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if MREDIS_HAVE_POSIX
# include <signal.h>
# include <sys/resource.h>    // getrusage()
# include <sys/time.h>
# include <unistd.h>
#endif

#if defined( MREDIS_HAVE_LIBPROC)
# include <libproc.h>         // proc_pid_rusage()
#endif

#if defined( MREDIS_HAVE_WINDOWS_ACCOUNTING)
/*
 * mulle-socket.h has already brought in winsock2.h and windows.h, in that
 * order, so the process API is declared by the time this is reached.
 */
# include <psapi.h>           // GetProcessMemoryInfo(), PROCESS_MEMORY_COUNTERS
#endif


#define BENCH_MAX_THREADS   64
#define BENCH_MAX_VALUE     (1024 * 1024)

/*
 * Every value written anywhere -- warm up and measured phase alike -- is this
 * byte repeated, so a read verifies by content as well as by length. It has to
 * be one value for both: half the keys are only ever read once the workload
 * alternates SET and GET, so those still hold what the warm up wrote.
 */
#define BENCH_PATTERN       'b'

/*
 * Latency is recorded per worker, not in a shared counter, because the act of
 * measuring it must not contend with anything: a CAS per batch would land in
 * the number being measured. One histogram per worker, merged when it is
 * displayed, and never on the hot path.
 *
 * Buckets are logarithmic with four sub-buckets per octave, so a bucket is
 * ~19% wide rather than ~100%: 1 ns, 2-3, 4-7, 8-15 ... up to 2^24 ns (16.7 ms),
 * and one final bucket for everything above. Percentiles interpolate inside a
 * bucket, so they are bucket precision rather than sample precision -- and 97
 * buckets of 8 bytes per worker beats keeping every sample.
 */
#define BENCH_LATENCY_OCTAVES    24
#define BENCH_LATENCY_BUCKETS    (BENCH_LATENCY_OCTAVES * 4 + 1)
#define BENCH_HISTOGRAM_ROWS     8
#define BENCH_HISTOGRAM_WIDTH    48


struct bench_options
{
   const char   *server;
   const char   *host;
   long          threads;
   double        seconds;
   long          pipeline;
   long          value_size;
   long          keys;
   int           use_tcp;
   int           quiet;
   double        interval;
   const char   *redis;           /* a real redis-server to start, if asked */
   int           compare;         /* run mredis and redis, side by side */
   char          spec[ 256];      /* --connect, split in place at parse time */
   char         *connect_host;    /* NULL for a unix path or a bare port */
   char         *connect_unix;    /* set when the spec was a socket path */
   unsigned int  connect_port;
};


/*
 * ---------------------------------------------------------------------------
 *
 * Everything from here to `-- options` is the measurement. None of it is
 * platform-specific any more: spawning the server is mulle-spawn, reaching it is
 * a socket or a pipe through `struct mredis_wire`, and asking the kernel how
 * much memory and CPU that server holds is the one question with three answers
 * -- /proc on Linux, libproc on macOS, GetProcessMemoryInfo and GetProcessTimes
 * on Windows (see `MREDIS_HAVE_ACCOUNTING`). A platform with none of the three
 * still builds and runs all of this, and reports that one reading as
 * unavailable.
 *
 * The whole block used to be compiled out on Windows, on the reasoning that
 * "no accounting" is "no benchmark". Those are different things, and the skip
 * was named after the first while hiding the second.
 */

/*
 * Counters live in `mulle_atomic_pointer_t` with an integer stored directly in
 * the pointer, the idiom `mulle-atomic-c11.h` describes (see `wontfix/04`). Note the
 * `&`: an atomic lvalue in an argument list is loaded, so dropping it passes
 * the value where an address belongs, and nothing diagnoses that.
 */
static void   counter_add( mulle_atomic_pointer_t *p, long value)
{
   _mulle_atomic_pointer_add( p, (intptr_t) value);
}


static long   counter_get( mulle_atomic_pointer_t *p)
{
   return( (long) (intptr_t) _mulle_atomic_pointer_read( p));
}


static void   counter_set( mulle_atomic_pointer_t *p, long value)
{
   _mulle_atomic_pointer_nonatomic_write( p, (void *) (intptr_t) value);
}


struct bench_stats
{
   mulle_atomic_pointer_t   requests;
   mulle_atomic_pointer_t   bytes_in;
   mulle_atomic_pointer_t   bytes_out;
   mulle_atomic_pointer_t   errors;
};


/*
 * A merged view of every worker's histogram. Lives on the display side: the
 * workers never touch it.
 */
struct bench_latency
{
   unsigned long long   counts[ BENCH_LATENCY_BUCKETS];
   unsigned long long   samples;
   unsigned long long   min_ns;
   unsigned long long   max_ns;
};


/* what one measurement leaves behind, for the summary and for --compare */
struct bench_result
{
   const char          *label;
   const char          *tag;
   unsigned long long   requests;
   unsigned long long   bytes_in;
   unsigned long long   bytes_out;
   unsigned long long   errors;
   unsigned long long   rss_kb;
   unsigned long long   server_cpu_ms;   /* CPU the server burned in the window */
   unsigned long long   client_cpu_ms;   /* and the client, all threads */
   double               seconds;
   struct bench_latency latency;
};


struct bench_worker
{
   struct bench_stats      *stats;
   const char              *host;
   unsigned int             port;
   int                      use_stdio;   /* the caller hands the wire over */
   int                      read_fd;     /* stdio: the pipe end to read */
   int                      write_fd;    /* stdio: and the one to write */
   mulle_socket_t           socket;      /* else: connected by the worker */
   int                      index;
   long                     pipeline;
   long                     value_size;
   long                     keys;
   const char              *unix_path;   /* set for a unix socket target */
   double                   deadline;
   char                    *value;
   char                    *scratch;
   size_t                   scratch_size;
   char                     pattern;
   unsigned long long       latency[ BENCH_LATENCY_BUCKETS];
   unsigned long long       latency_samples;
   unsigned long long       latency_min_ns;
   unsigned long long       latency_max_ns;
   struct mredis_wire       wire;
};


// ------------------------------------------------------------------ formatting

static void   format_count( char *dst, size_t size, long long value)
{
   char     tmp[ 32];
   size_t   length;
   size_t   i;
   size_t   out;

   length = (size_t) snprintf( tmp, sizeof( tmp), "%lld", value);

   out = 0;
   for( i = 0; i < length && out + 2 < size; i++)
   {
      if( i && ((length - i) % 3) == 0)
         dst[ out++] = ',';
      dst[ out++] = tmp[ i];
   }
   dst[ out] = 0;
}


/* prints a value held in tenths as "12.3", without a float conversion */
static void   print_tenths( FILE *fp, long long tenths)
{
   mulle_fprintf( fp, "%lld.%lld", tenths / 10, tenths % 10);
}


/* "812 ns", "12.3 us", "1.24 ms": ASCII only, no float conversion */
static char   *duration_string( char *buffer, size_t size, unsigned long long ns)
{
   unsigned long long   tenths;

   if( ns < 1000)
      snprintf( buffer, size, "%llu ns", ns);
   else
      if( ns < 1000000)
      {
         tenths = ns / 100;
         snprintf( buffer, size, "%llu.%llu us", tenths / 10, tenths % 10);
      }
      else
      {
         tenths = ns / 100000;
         snprintf( buffer, size, "%llu.%llu ms", tenths / 10, tenths % 10);
      }
   return( buffer);
}


static long long   bytes_to_mb_tenths( long long bytes)
{
   return( (bytes * 10) / (1024 * 1024));
}


/*
 * 10 * count / seconds, i.e. a rate in tenths. Also the unit `peak` is kept in.
 *
 * The MB/s rate is *not* this function applied to a tenths value: that scales
 * twice and reports ten times the throughput, which this benchmark did until
 * the byte totals were checked against a hand count of what goes on the wire.
 */
static long long   rate_tenths( long long count, double seconds)
{
   if( seconds <= 0)
      return( 0);
   return( (long long) ((double) count * 10.0 / seconds));
}


/* tenths of a megabyte per second, scaled in one step from bytes and seconds */
static long long   mb_rate_tenths( long long bytes, double seconds)
{
   if( seconds <= 0)
      return( 0);
   return( (long long) ((double) bytes * 10.0 / (1024.0 * 1024.0) / seconds));
}


#if MREDIS_HAVE_ACCOUNTING
/*
 * The server's resident size, in KiB, or -1 if this platform will not say.
 *
 * "How much is it gobbling up" should never be answered by the code under
 * test, so this is the kernel's number either way: VmRSS out of
 * /proc/<pid>/status on Linux, ri_resident_size out of proc_pid_rusage() on
 * macOS, and WorkingSetSize out of GetProcessMemoryInfo() on Windows -- the same
 * quantity, resident bytes, spelled by three kernels.
 */
static long   server_rss_kb( mredis_pid_t pid)
{
#if defined( MREDIS_HAVE_LIBPROC)
   struct rusage_info_v2   info;

   if( proc_pid_rusage( pid, RUSAGE_INFO_V2, (rusage_info_t *) &info) != 0)
      return( -1);

   return( (long) (info.ri_resident_size / 1024));
#elif defined( MREDIS_HAVE_PROC)
   char    path[ 64];
   char    line[ 256];
   FILE   *fp;
   long    kb;

   snprintf( path, sizeof( path), "/proc/%ld/status", (long) pid);
   fp = fopen( path, "r");
   if( ! fp)
      return( -1);

   kb = -1;
   while( fgets( line, sizeof( line), fp))
   {
      if( ! strncmp( line, "VmRSS:", 6))
      {
         kb = strtol( &line[ 6], NULL, 10);
         break;
      }
   }
   fclose( fp);
   return( kb);
#elif defined( MREDIS_HAVE_WINDOWS_ACCOUNTING)
   PROCESS_MEMORY_COUNTERS   counters;
   HANDLE                    process;
   long                      kb;

   process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
   if( ! process)
      return( -1);

   kb = -1;
   if( GetProcessMemoryInfo( process, &counters, sizeof( counters)))
      kb = (long) (counters.WorkingSetSize / 1024);

   CloseHandle( process);
   return( kb);
#else
   MULLE_C_UNUSED( pid);
   return( -1);
#endif
}


/*
 * Only the /proc reader needs this, and an unused static function is a
 * -Wunused-function warning rather than dead code the linker drops quietly.
 */
#if defined( MREDIS_HAVE_PROC)
// skip n whitespace separated fields, NULL if the line ran out first
static char   *skip_stat_fields( char *p, int n)
{
   while( n-- > 0)
   {
      while( *p == ' ' || *p == '\t')
         p++;
      while( *p && *p != ' ' && *p != '\t' && *p != '\n')
         p++;
      if( ! *p)
         return( NULL);
   }
   return( p);
}
#endif


/*
 * CPU seconds a process has burned, from utime + stime. Negative if it cannot
 * be read.
 *
 * Linux reads the pair out of /proc/<pid>/stat; macOS asks proc_pid_rusage(),
 * which reports the same two counters in nanoseconds; Windows asks
 * GetProcessTimes(), which reports them as a pair of FILETIME counters in
 * 100 ns ticks -- user and kernel, which is what utime and stime are.
 *
 * The /proc path's own
 * hazards are worth keeping written down, because the field counting below is
 * entirely in service of them and is easy to "simplify" back into a bug.
 *
 * The fields are counted from the comm field's closing paren, not from the
 * start of the line: comm is whatever the program set its name to and may
 * contain spaces and parens, so counting from the left is one renamed process
 * away from reading the wrong field. (The same class of mistake as the inode
 * field in mredis_pid_for_port, which is why it is spelled out here.)
 *
 * After the paren: state ppid pgrp session tty_nr tpgid flags minflt cminflt
 * majflt cmajflt -- eleven fields -- then utime, stime, and only *then* cutime
 * and cstime, which are the children's times and are not wanted here.
 *
 * utime and stime are adjacent, so stime is parsed out of the end pointer of
 * the utime parse. Skipping a field in between reads cutime instead -- a
 * renamed process away from silence, since cutime is 0 for a server that
 * forks nothing: the reading then looks like a clean number that is the
 * server's *user* time only. On this workload that understates the server by
 * about six times, because its cost is mostly kernel time.
 */
static double   process_cpu_seconds( mredis_pid_t pid)
{
#if defined( MREDIS_HAVE_LIBPROC)
   struct rusage_info_v2   info;

   if( proc_pid_rusage( pid, RUSAGE_INFO_V2, (rusage_info_t *) &info) != 0)
      return( -1.0);

   return( ((double) info.ri_user_time + (double) info.ri_system_time) / 1e9);
#elif defined( MREDIS_HAVE_PROC)
   char            path[ 64];
   char            line[ 512];
   char           *p;
   char           *end;
   FILE           *fp;
   unsigned long   utime;
   unsigned long   stime;
   long            hz;

   snprintf( path, sizeof( path), "/proc/%ld/stat", (long) pid);
   fp = fopen( path, "r");
   if( ! fp)
      return( -1.0);

   if( ! fgets( line, sizeof( line), fp))
   {
      fclose( fp);
      return( -1.0);
   }
   fclose( fp);

   p = strrchr( line, ')');
   if( ! p)
      return( -1.0);

   p = skip_stat_fields( p + 1, 11);
   if( ! p)
      return( -1.0);

   utime = strtoul( p, &end, 10);
   if( end == p)
      return( -1.0);

   p     = end;
   stime = strtoul( p, &end, 10);
   if( end == p)
      return( -1.0);

   hz = sysconf( _SC_CLK_TCK);
   if( hz <= 0)
      hz = 100;

   return( (double) (utime + stime) / (double) hz);
#elif defined( MREDIS_HAVE_WINDOWS_ACCOUNTING)
   FILETIME         created;
   FILETIME         exited;
   FILETIME         kernel;
   FILETIME         user;
   ULARGE_INTEGER   k;
   ULARGE_INTEGER   u;
   HANDLE           process;
   double           seconds;

   process = OpenProcess( PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
   if( ! process)
      return( -1.0);

   seconds = -1.0;
   if( GetProcessTimes( process, &created, &exited, &kernel, &user))
   {
      /* a FILETIME is two halves of one 64 bit count of 100 ns ticks */
      k.LowPart  = kernel.dwLowDateTime;
      k.HighPart = kernel.dwHighDateTime;
      u.LowPart  = user.dwLowDateTime;
      u.HighPart = user.dwHighDateTime;
      seconds    = (double) (k.QuadPart + u.QuadPart) / 1e7;
   }

   CloseHandle( process);
   return( seconds);
#else
   MULLE_C_UNUSED( pid);
   return( -1.0);
#endif
}


/* the client's own CPU, every thread of it, from the kernel's accounting */
static double   client_cpu_seconds( void)
{
#if defined( MREDIS_HAVE_WINDOWS_ACCOUNTING)
   FILETIME         created;
   FILETIME         exited;
   FILETIME         kernel;
   FILETIME         user;
   ULARGE_INTEGER   k;
   ULARGE_INTEGER   u;

   if( ! GetProcessTimes( GetCurrentProcess(), &created, &exited, &kernel, &user))
      return( -1.0);

   k.LowPart  = kernel.dwLowDateTime;
   k.HighPart = kernel.dwHighDateTime;
   u.LowPart  = user.dwLowDateTime;
   u.HighPart = user.dwHighDateTime;
   return( (double) (k.QuadPart + u.QuadPart) / 1e7);
#else
   struct rusage   usage;

   if( getrusage( RUSAGE_SELF, &usage))
      return( -1.0);

   return( (double) usage.ru_utime.tv_sec + (double) usage.ru_utime.tv_usec / 1e6
         + (double) usage.ru_stime.tv_sec + (double) usage.ru_stime.tv_usec / 1e6);
#endif
}
#endif


// -------------------------------------------------------------------- latency

/* log2 octave plus two bits below the leading one, so ~19% resolution */
static unsigned int   latency_bucket( unsigned long long ns)
{
   unsigned long long   value;
   unsigned long long   base;
   unsigned long long   width;
   unsigned int         octave;
   unsigned int         sub;

   if( ns < 1)
      return( 0);

   value  = ns;
   octave = 0;
   while( value > 1)
   {
      value >>= 1;
      octave++;
   }

   if( octave >= BENCH_LATENCY_OCTAVES)
      return( BENCH_LATENCY_BUCKETS - 1);   /* the overflow bucket */

   base  = (unsigned long long) 1 << octave;
   width = (octave >= 2) ? (base >> 2) : 1;
   sub   = (octave >= 2) ? (unsigned int) ((ns - base) / width) : 0;
   if( sub > 3)
      sub = 3;

   return( octave * 4 + sub);
}


static unsigned long long   latency_bucket_lo( unsigned int index)
{
   unsigned long long   base;
   unsigned int         octave;

   if( index >= BENCH_LATENCY_BUCKETS - 1)
      return( (unsigned long long) 1 << BENCH_LATENCY_OCTAVES);

   octave = index / 4;
   base   = (unsigned long long) 1 << octave;
   return( base + (unsigned long long) (index % 4) *
                  ((octave >= 2) ? (base >> 2) : 1));
}


static unsigned long long   latency_bucket_width( unsigned int index)
{
   unsigned long long   base;
   unsigned int         octave;

   if( index >= BENCH_LATENCY_BUCKETS - 1)
      return( (unsigned long long) 1 << BENCH_LATENCY_OCTAVES);

   octave = index / 4;
   base   = (unsigned long long) 1 << octave;
   return( (octave >= 2) ? (base >> 2) : 1);
}


static void   latency_record( struct bench_worker *w, unsigned long long ns)
{
   if( ! ns)
      ns = 1;

   w->latency[ latency_bucket( ns)]++;
   w->latency_samples++;

   if( ! w->latency_min_ns || ns < w->latency_min_ns)
      w->latency_min_ns = ns;
   if( ns > w->latency_max_ns)
      w->latency_max_ns = ns;
}


static void   latency_merge( struct bench_latency *lat,
                             struct bench_worker *workers,
                             long count)
{
   long           w;
   unsigned int   i;

   memset( lat, 0, sizeof( *lat));

   for( w = 0; w < count; w++)
   {
      struct bench_worker   *worker;

      worker = &workers[ w];
      if( ! worker->latency_samples)
         continue;

      for( i = 0; i < BENCH_LATENCY_BUCKETS; i++)
         lat->counts[ i] += worker->latency[ i];

      lat->samples += worker->latency_samples;
      if( ! lat->min_ns || worker->latency_min_ns < lat->min_ns)
         lat->min_ns = worker->latency_min_ns;
      if( worker->latency_max_ns > lat->max_ns)
         lat->max_ns = worker->latency_max_ns;
   }
}


/*
 * Linear interpolation inside the bucket the percentile lands in, from the
 * lower edge. Read it as "about this much": the bucket is ~19% wide and that
 * is the resolution being reported.
 */
static unsigned long long   latency_percentile( const struct bench_latency *lat,
                                                double p)
{
   unsigned long long   target;
   unsigned long long   seen;
   unsigned long long   count;
   unsigned int         i;

   if( ! lat->samples)
      return( 0);

   target = (unsigned long long) ((double) lat->samples * p + 0.9999);
   if( ! target)
      target = 1;

   seen = 0;
   for( i = 0; i < BENCH_LATENCY_BUCKETS; i++)
   {
      count = lat->counts[ i];
      if( ! count)
         continue;

      if( seen + count >= target)
         return( latency_bucket_lo( i) +
                 (target - seen - 1) * latency_bucket_width( i) / count);
      seen += count;
   }
   return( lat->max_ns);
}


/* the octave a nanosecond value falls in */
static unsigned int   latency_octave( unsigned long long ns)
{
   return( latency_bucket( ns) / 4);
}


/*
 * One bar per octave, over the octave range the p1..p99 body covers and one
 * octave either side, so the rows that get spent are rows that hold something.
 * What falls below or above that window is folded into a "<" or ">" row rather
 * than dropping it: a single outlier 100x away from the body is exactly the
 * thing worth seeing, and it does not deserve eight blank rows to show it.
 * Bars are relative to the tallest row in the window.
 */
static void   render_histogram( FILE *fp, const struct bench_latency *lat)
{
   struct hist_row
   {
      unsigned long long   count;
      unsigned long long   lo_ns;
      const char          *prefix;   /* "", "<" or ">": a folded open end */
   };

   struct hist_row      rows[ BENCH_HISTOGRAM_ROWS + 2];
   unsigned long long   octaves[ BENCH_LATENCY_OCTAVES + 1];
   unsigned long long   peak;
   unsigned long long   count;
   unsigned long long   under;
   unsigned long long   over;
   unsigned int         lo;
   unsigned int         hi;
   unsigned int         usable;
   unsigned int         row;
   unsigned int         rows_used;
   unsigned int         i;
   int                  folded;
   int                  width;
   char                 label[ 32];

   if( ! lat->samples)
   {
      mulle_fprintf( fp, "  histogram    (no samples yet)\n");
      return;
   }

   memset( octaves, 0, sizeof( octaves));
   for( i = 0; i < BENCH_LATENCY_BUCKETS; i++)
      octaves[ (i == BENCH_LATENCY_BUCKETS - 1) ? BENCH_LATENCY_OCTAVES : i / 4]
         += lat->counts[ i];

   /*
    * The window is the octave range the p1..p99 body covers, plus one octave
    * either side: rows that hold something are the rows worth spending. What
    * falls outside is folded into a "<" or ">" row rather than dropped, because
    * a lone outlier 100x away from the body is exactly what you want to see and
    * it does not deserve eight blank rows to show it.
    */
   lo = latency_octave( latency_percentile( lat, 0.01));
   hi = latency_octave( latency_percentile( lat, 0.99));
   if( lo)
      lo--;
   if( hi < BENCH_LATENCY_OCTAVES)
      hi++;

   usable = BENCH_HISTOGRAM_ROWS;
   folded = 0;
   if( hi - lo + 1 > usable)
   {
      hi     = lo + usable - 1;
      folded = 1;   /* the window's last row also carries everything above it */
   }

   under = 0;
   for( i = 0; i < lo; i++)
      under += octaves[ i];
   over = 0;
   if( ! folded)
      for( i = hi + 1; i <= BENCH_LATENCY_OCTAVES; i++)
         over += octaves[ i];

   rows_used = 0;
   if( under)
   {
      rows[ rows_used].count  = under;
      rows[ rows_used].lo_ns  = latency_bucket_lo( lo * 4);
      rows[ rows_used].prefix = "<";
      rows_used++;
   }

   for( i = lo; i <= hi; i++)
   {
      unsigned int   j;

      count = octaves[ i];
      if( folded && i == hi)
      {
         /* the window's last row stands for everything above it */
         count = 0;
         for( j = i; j <= BENCH_LATENCY_OCTAVES; j++)
            count += octaves[ j];
      }

      rows[ rows_used].count  = count;
      rows[ rows_used].lo_ns  = latency_bucket_lo( i * 4);
      rows[ rows_used].prefix = ( folded && i == hi) ? ">" : "";
      rows_used++;
   }

   if( over)
   {
      rows[ rows_used].count  = over;
      rows[ rows_used].lo_ns  = latency_bucket_lo( (hi + 1) * 4);
      rows[ rows_used].prefix = ">";
      rows_used++;
   }

   peak = 0;
   for( row = 0; row < rows_used; row++)
      if( rows[ row].count > peak)
         peak = rows[ row].count;

   for( row = 0; row < rows_used; row++)
   {
      char   *name;

      name = duration_string( label, sizeof( label), rows[ row].lo_ns);
      mulle_fprintf( fp, row ? "               " : "  histogram    ");
      mulle_fprintf( fp, "%2s%10s ", rows[ row].prefix, name);

      width = 0;
      if( rows[ row].count && peak)
      {
         width = (int) (rows[ row].count * BENCH_HISTOGRAM_WIDTH / peak);
         if( width > BENCH_HISTOGRAM_WIDTH)
            width = BENCH_HISTOGRAM_WIDTH;
         if( ! width)
            width = 1;
      }

      while( width-- > 0)
         mulle_fprintf( fp, "#");

      if( rows[ row].count)
      {
         char   tally[ 32];
         int    pad;

         format_count( tally, sizeof( tally), (long long) rows[ row].count);
         pad = BENCH_HISTOGRAM_WIDTH -
               (int) (rows[ row].count * BENCH_HISTOGRAM_WIDTH / peak);
         if( pad < 1)
            pad = 1;
         while( pad-- > 0)
            mulle_fprintf( fp, " ");
         mulle_fprintf( fp, "%s", tally);
      }
      mulle_fprintf( fp, "\n");
   }
}


// ------------------------------------------------------------------- workload

/*
 * `n & 1` picks SET versus GET and the key is `n % keys`, so a batch is a
 * deterministic mix and two runs are comparable.
 */
static void   add_command( struct bench_worker *w, struct mulle_buffer *batch, long n)
{
   const char   *argv[ 3];
   size_t        argv_length[ 3];
   char          key[ 32];

   snprintf( key, sizeof( key), "key:%ld", n % w->keys);

   argv[ 1]        = key;
   argv_length[ 1] = strlen( key);

   if( n & 1)
   {
      argv[ 0]        = "SET";
      argv_length[ 0] = 3;
      argv[ 2]        = w->value;
      argv_length[ 2] = (size_t) w->value_size;
      mredis_encode_argv( batch, 3, argv, argv_length);
      return;
   }

   argv[ 0]        = "GET";
   argv_length[ 0] = 3;
   mredis_encode_argv( batch, 2, argv, argv_length);
}


/* `OK` for a write, and the exact value that was sent for a read */
static int   reply_is_expected( struct bench_worker *w, struct mredis_reply *reply)
{
   switch( reply->type)
   {
   case MREDIS_REPLY_SIMPLE :
      return( ! strcmp( w->scratch, "OK"));

   case MREDIS_REPLY_BULK :
      return( (long) reply->length == w->value_size &&
              w->scratch[ 0] == w->pattern);

   default :
      return( 0);
   }
}


/*
 * A write, on whichever transport the wire was opened with: a pipe end through
 * the CRT, a socket through mulle-socket. This used to be an int descriptor and
 * a `#if MREDIS_HAVE_POSIX`, which is the same thing only on a platform where a
 * socket *is* a descriptor.
 *
 * `write_fd` is the pipe end to write on and is -1 for a socket, because a
 * socket is both ends of itself and the wire already holds it. A pipe is not: it
 * is two descriptors, so the one to read and the one to write are told apart by
 * the caller and not by the wire.
 */
static int   wire_write_all( struct mredis_wire *wire, int write_fd,
                             const void *bytes, size_t length)
{
   if( wire->transport == MREDIS_TRANSPORT_SOCKET)
      return( mulle_socket_write_all( wire->socket, bytes, length));
   return( mredis_write_all( write_fd, bytes, length));
}


/* one write, `count` replies; returns -1 with the error already counted */
static int   exchange( struct bench_worker *w, struct mulle_buffer *batch, long count)
{
   struct mredis_reply   reply;
   size_t                length;
   unsigned long long    before;
   long                  i;

   length = mulle_buffer_get_length( batch);
   if( wire_write_all( &w->wire, w->write_fd, mulle_buffer_get_bytes( batch), length))
   {
      counter_add( &w->stats->errors, 1);
      return( -1);
   }

   // captured before reading, so bytes_out is a delta over the whole batch
   before = w->wire.bytes_read;

   for( i = 0; i < count; i++)
   {
      if( mredis_wire_read_reply( &w->wire, &reply, w->scratch, w->scratch_size))
      {
         counter_add( &w->stats->errors, 1);
         return( -1);
      }
      if( ! reply_is_expected( w, &reply))
         counter_add( &w->stats->errors, 1);
   }

   counter_add( &w->stats->requests, count);
   counter_add( &w->stats->bytes_in, (long) length);
   counter_add( &w->stats->bytes_out, (long) (w->wire.bytes_read - before));
   return( 0);
}


// ------------------------------------------------------------------ one worker

static mulle_thread_rval_t   worker_main( void *arg)
{
   struct bench_worker   *w;
   struct mulle_buffer    batch;
   size_t                 capacity;
   long                   n;
   long                   built;

   w = arg;

   /*
    * Who opens the connection is the difference between the two transports: a
    * socket target has one connection per worker, opened here, while a pipe is
    * one pair of descriptors the caller opened and every worker shares.
    */
   if( w->use_stdio)
   {
      mredis_wire_init( &w->wire, w->read_fd);
      if( w->write_fd < 0)
         w->write_fd = w->read_fd;
   }
   else
   {
      if( w->unix_path)
      {
         int   fd;

         fd = mredis_connect_unix( w->unix_path);
         if( fd < 0)
         {
            counter_add( &w->stats->errors, 1);
            return( 0);
         }
         mredis_wire_init( &w->wire, fd);
      }
      else
      {
         w->socket = mredis_connect( w->host, w->port);
         if( w->socket == MULLE_SOCKET_INVALID)
         {
            counter_add( &w->stats->errors, 1);
            return( 0);
         }
         mredis_wire_init_socket( &w->wire, w->socket);
      }
   }
   memset( w->value, (int) (unsigned char) w->pattern, (size_t) w->value_size);

   // room for `pipeline` commands of the largest shape, so the encode never has
   // to grow and the loop measures the server rather than the allocator
   capacity = (size_t) w->pipeline * (size_t) (w->value_size + 64);
   mulle_buffer_init( &batch, capacity, NULL);

   n = (long) w->index;

   for(;;)
   {
      double                started;
      unsigned long long    ns;

      if( mulle_relativetime_now() >= w->deadline)
         break;

      /*
       * mulle_buffer_remove_all(), not mulle_buffer_reset(): reset() frees the
       * storage and puts the capacity back to zero, so a reset-per-batch loop
       * takes a malloc and a free per batch and times the allocator too. This
       * only moves the write cursor back (see `wontfix/16`).
       */
      mulle_buffer_remove_all( &batch);

      for( built = 0; built < w->pipeline; built++, n++)
         add_command( w, &batch, n);

      /*
       * The clock reading surrounds the wire, not the encoding: a batch round
       * trip is "write the batch, read every reply for it". Divided by the
       * pipeline that is the per-request cost while the pipe is full, which is
       * what a pipelining client actually pays per request; `--pipeline 1`
       * makes the same number the plain round trip time.
       */
      started = mulle_relativetime_now();
      if( exchange( w, &batch, w->pipeline))
         break;
      ns = (unsigned long long) ((mulle_relativetime_now() - started) * 1e9);

      latency_record( w, ns / (unsigned long long) w->pipeline);
   }

   mulle_buffer_done( &batch);
   return( 0);
}


// ------------------------------------------------------------------ warm up

/*
 * Fill the keyspace before any worker starts, so that every GET in the measured
 * phase finds a value: a miss would be timed as a request but would not be the
 * workload we think we are measuring. Not counted anywhere.
 */
static int   warm_up( struct mredis_wire *wire, int write_fd,
                      const struct bench_options *options)
{
   struct mredis_reply   reply;
   struct mulle_buffer   batch;
   char                 *value;
   char                 *scratch;
   size_t                scratch_size;
   long                  n;
   long                  i;
   int                   rval;

   value        = mulle_malloc( (size_t) options->value_size);
   scratch_size = (size_t) options->value_size + 2;
   scratch      = mulle_malloc( scratch_size);
   memset( value, BENCH_PATTERN, (size_t) options->value_size);

   mulle_buffer_init( &batch,
                      (size_t) options->pipeline * (size_t) (options->value_size + 64),
                      NULL);

   rval = 0;
   n    = 0;

   while( n < options->keys)
   {
      long   built;

      mulle_buffer_remove_all( &batch);

      for( built = 0; built < options->pipeline && n + built < options->keys; built++)
      {
         const char   *argv[ 3];
         size_t        argv_length[ 3];
         char          key[ 32];

         snprintf( key, sizeof( key), "key:%ld", n + built);
         argv[ 0]        = "SET";
         argv_length[ 0] = 3;
         argv[ 1]        = key;
         argv_length[ 1] = strlen( key);
         argv[ 2]        = value;
         argv_length[ 2] = (size_t) options->value_size;
         mredis_encode_argv( &batch, 3, argv, argv_length);
      }

      if( wire_write_all( wire, write_fd, mulle_buffer_get_bytes( &batch),
                          mulle_buffer_get_length( &batch)))
      {
         mulle_fprintf( stderr, "warmup: write failed for %ld bytes: %s\n",
                        (long) mulle_buffer_get_length( &batch), strerror( errno));
         rval = -1;
         break;
      }

      for( i = 0; i < built; i++)
      {
         if( mredis_wire_read_reply( wire, &reply, scratch, scratch_size))
         {
            mulle_fprintf( stderr, "warmup: reply %ld of %ld could not be read: %s\n",
                           i, built, strerror( errno));
            rval = -1;
            break;
         }
         if( reply.type != MREDIS_REPLY_SIMPLE || strcmp( scratch, "OK"))
         {
            mulle_fprintf( stderr, "warmup: reply %ld of %ld was %c [%.20s]\n",
                           i, built, reply.type, scratch);
            rval = -1;
            break;
         }
      }
      if( rval)
         break;

      n += built;
   }

   mulle_buffer_done( &batch);
   mulle_free( value);
   mulle_free( scratch);
   return( rval);
}


// ------------------------------------------------------------------- display

/*
 * The frame is redrawn by moving the cursor to the top and clearing downwards,
 * so the line count is free to grow; `clear` is off when the output is not a
 * terminal.
 */
static void   render_frame( FILE *fp, const struct bench_options *options,
                            const char *label, struct bench_stats *stats,
                            mredis_pid_t server_pid, const struct bench_latency *lat,
                            double elapsed, long long request_rate_tenths,
                            long long peak_tenths, long long mb_rate_tenths_now,
                            unsigned long long server_cpu_ms,
                            unsigned long long client_cpu_ms,
                            int clear_screen)
{
   char          requests[ 32];
   char          rate[ 32];
   char          peak[ 32];
   char          duration[ 32];
   long long     in_tenths;
   long long     out_tenths;

   format_count( requests, sizeof( requests), (long long) counter_get( &stats->requests));

   // printed from tenths, so no float conversion is needed anywhere
   format_count( rate, sizeof( rate), request_rate_tenths / 10);
   format_count( peak, sizeof( peak), peak_tenths / 10);

   in_tenths  = bytes_to_mb_tenths( (long long) counter_get( &stats->bytes_in));
   out_tenths = bytes_to_mb_tenths( (long long) counter_get( &stats->bytes_out));

   if( clear_screen)
      mulle_fprintf( fp, "\033[H\033[J");

   mulle_fprintf( fp,
                  "mredis bench   %s   threads %ld   pipeline %ld   payload %ld B   keys %ld\n",
                  label,
                  options->threads,
                  options->pipeline,
                  options->value_size,
                  options->keys);
   mulle_fprintf( fp, "  elapsed      ");
   print_tenths( fp, (long long) (elapsed * 10.0));
   mulle_fprintf( fp, " s\n");
   mulle_fprintf( fp, "  requests     %s        %s req/s   (peak %s)\n",
                  requests,
                  rate,
                  peak);
   mulle_fprintf( fp, "  traffic      in ");
   print_tenths( fp, in_tenths);
   mulle_fprintf( fp, " MB   out ");
   print_tenths( fp, out_tenths);
   mulle_fprintf( fp, " MB        %lld.%lld MB/s\n",
                  mb_rate_tenths_now / 10, mb_rate_tenths_now % 10);

   /*
    * Per request, not per batch: the batch round trip divided by the pipeline.
    * With `--pipeline 1` this is the plain round trip time.
    */
   mulle_fprintf( fp, "  latency/req  p50 %s",
                  duration_string( duration, sizeof( duration),
                                   latency_percentile( lat, 0.50)));
   mulle_fprintf( fp, "   p90 %s",
                  duration_string( duration, sizeof( duration),
                                   latency_percentile( lat, 0.90)));
   mulle_fprintf( fp, "   p99 %s",
                  duration_string( duration, sizeof( duration),
                                   latency_percentile( lat, 0.99)));
   mulle_fprintf( fp, "   p99.9 %s",
                  duration_string( duration, sizeof( duration),
                                   latency_percentile( lat, 0.999)));
   mulle_fprintf( fp, "   max %s\n",
                  duration_string( duration, sizeof( duration), lat->max_ns));

   render_histogram( fp, lat);

#if MREDIS_HAVE_ACCOUNTING
   {
      long   kb;

      kb = server_rss_kb( server_pid);
      mulle_fprintf( fp, "  server       RSS ");
      if( kb >= 0)
         print_tenths( fp, (long long) kb * 10 / 1024);
      else
         mulle_fprintf( fp, "n/a");
      mulle_fprintf( fp, " MB   (pid %ld)\n", (long) server_pid);
   }
#else
   mulle_fprintf( fp, "  server       RSS n/a\n");
#endif

   /*
    * Who is doing the work. A server at ~100% of a core is at its limit; a
    * client at ~100% x threads is the *benchmark* being the limit, which is the
    * one thing that can quietly cap a comparison. Both are the kernel's numbers:
    * utime+stime of the server pid, and getrusage() of this process.
    */
   {
      long long   elapsed_ms;

      elapsed_ms = (long long) (elapsed * 1000.0);

      mulle_fprintf( fp, "  server cpu   ");
      if( elapsed_ms > 0 && server_cpu_ms)
         mulle_fprintf( fp, "%lld.%lld s   %lld%% of a core\n",
                        (long long) (server_cpu_ms / 1000),
                        (long long) ((server_cpu_ms % 1000) / 10),
                        (long long) (server_cpu_ms * 100 / (unsigned long long) elapsed_ms));
      else
         mulle_fprintf( fp, "n/a\n");

      mulle_fprintf( fp, "  client cpu   ");
      if( elapsed_ms > 0 && client_cpu_ms)
         mulle_fprintf( fp, "%lld.%lld s   %lld%% of one core, all %ld workers\n",
                        (long long) (client_cpu_ms / 1000),
                        (long long) ((client_cpu_ms % 1000) / 10),
                        (long long) (client_cpu_ms * 100 / (unsigned long long) elapsed_ms),
                        options->threads);
      else
         mulle_fprintf( fp, "n/a\n");
   }

   mulle_fprintf( fp, "  errors       %ld\n", counter_get( &stats->errors));
   fflush( fp);
}


/* the line the tests read: one line, key=value, no escape sequences */
static void   render_summary( FILE *fp,
                              const struct bench_options *options,
                              const char *tag,
                              const char *transport,
                              const struct bench_result *result)
{
   const struct bench_latency   *lat;
   long long                     total_bytes;
   long long                     req_per_s;
   long long                     mb_per_s;

   lat         = &result->latency;
   total_bytes = (long long) (result->bytes_in + result->bytes_out);
   req_per_s   = rate_tenths( (long long) result->requests,
                              result->seconds > 0 ? result->seconds : 1e-9);
   mb_per_s    = mb_rate_tenths( total_bytes,
                                 result->seconds > 0 ? result->seconds : 1e-9);

   mulle_fprintf( fp,
                  "bench: server=%s transport=%s threads=%ld pipeline=%ld"
                  " value_size=%ld keys=%ld seconds=",
                  tag,
                  transport,
                  options->threads,
                  options->pipeline,
                  options->value_size,
                  options->keys);
   print_tenths( fp, (long long) (result->seconds * 10.0));
   mulle_fprintf( fp, " requests=%llu req_per_s=", result->requests);
   print_tenths( fp, req_per_s);
   mulle_fprintf( fp, " mb_per_s=");
   print_tenths( fp, mb_per_s);
   mulle_fprintf( fp,
                  " server_rss_kb=%llu server_cpu_ms=%llu client_cpu_ms=%llu"
                  " errors=%llu samples=%llu"
                  " latency_min_ns=%llu p50_ns=%llu p90_ns=%llu p99_ns=%llu"
                  " p999_ns=%llu latency_max_ns=%llu\n",
                  result->rss_kb,
                  result->server_cpu_ms,
                  result->client_cpu_ms,
                  result->errors,
                  lat->samples,
                  lat->min_ns,
                  latency_percentile( lat, 0.50),
                  latency_percentile( lat, 0.90),
                  latency_percentile( lat, 0.99),
                  latency_percentile( lat, 0.999),
                  lat->max_ns);
   fflush( fp);
}


// ------------------------------------------------------------------- options

static void   bench_usage( FILE *fp)
{
   mulle_fprintf( fp, "usage: mredis-stress --bench [options]\n");
   mulle_fprintf( fp, "\n");
   mulle_fprintf( fp, "  --threads N       connections, one thread each (default 8)\n");
   mulle_fprintf( fp, "  --seconds S       how long to measure (default 5)\n");
   mulle_fprintf( fp, "  --pipeline N      requests in flight per connection (default 16)\n");
   mulle_fprintf( fp, "  --value-size N    payload bytes for SET (default 64)\n");
   mulle_fprintf( fp, "  --keys N          keyspace the workers cycle through (default 1000)\n");
   mulle_fprintf( fp, "  --stdio           drive a pipe child instead of a socket, forces --threads 1\n");
   mulle_fprintf( fp, "  --connect SPEC    bench a server that is already running: HOST:PORT,\n");
   mulle_fprintf( fp, "                    :PORT, or /path/to/socket\n");
   mulle_fprintf( fp, "  --redis PATH      bench that real redis-server instead of mredis\n");
   mulle_fprintf( fp, "  --compare         run the workload against mredis and --redis and\n");
   mulle_fprintf( fp, "                    report both, including the CPU each side burned\n");
   mulle_fprintf( fp, "                    print them side by side\n");
   mulle_fprintf( fp, "  --interval S      display refresh in seconds, 0 disables it (default 0.25)\n");
   mulle_fprintf( fp, "\n");
   mulle_fprintf( fp, "Latency is per request: the round trip of one pipeline batch divided by\n");
   mulle_fprintf( fp, "--pipeline, which is what a request costs while the pipe is full. Run with\n");
   mulle_fprintf( fp, "--pipeline 1 to measure plain round trips instead. Percentiles come from\n");
   mulle_fprintf( fp, "logarithmic buckets (~19%% wide), so read them as \"about this much\".\n");
   mulle_fprintf( fp, "  --quiet           no display, just the summary line\n");
   mulle_fprintf( fp, "  --server PATH     the mredis binary to spawn (default ./mredis)\n");
   mulle_fprintf( fp, "  -h, --help        this text\n");
}


/*
 * `HOST:PORT`, `:PORT` or `/path/to/socket`, split in place: the parsed pieces
 * have to outlive this call, so they point into the options struct's own copy
 * rather than into argv.
 */
static int   split_spec( struct bench_options *options, const char *spec)
{
   char   *colon;

   if( strlen( spec) >= sizeof( options->spec))
      return( -1);
   strcpy( options->spec, spec);

   if( options->spec[ 0] == '/')
   {
#if MREDIS_HAVE_POSIX
      options->connect_unix = options->spec;
      return( 0);
#else
      /* a socket path, and there are none here (see mredis_connect_unix) */
      return( -1);
#endif
   }

   colon = strrchr( options->spec, ':');
   if( ! colon)
   {
      options->connect_port = (unsigned int) strtoul( options->spec, NULL, 10);
      return( options->connect_port ? 0 : -1);
   }

   *colon = 0;
   options->connect_port = (unsigned int) strtoul( colon + 1, NULL, 10);
   if( options->spec[ 0])
      options->connect_host = options->spec;
   return( 0);
}


static int   parse_options( struct bench_options *options, int argc, char *argv[])
{
   int   i;

   options->server       = "./mredis";
   options->host         = "127.0.0.1";
   options->redis        = NULL;
   options->compare      = 0;
   options->spec[ 0]     = 0;
   options->connect_host = NULL;
   options->connect_unix = NULL;
   options->connect_port = 0;
   options->threads    = 8;
   options->seconds    = 5.0;
   options->pipeline   = 16;
   options->value_size = 64;
   options->keys       = 1000;
   options->use_tcp    = 1;
   options->quiet      = 0;
   options->interval   = 0.25;

   for( i = 1; i < argc; i++)
   {
      if( ! strcmp( argv[ i], "-h") || ! strcmp( argv[ i], "--help"))
      {
         bench_usage( stdout);
         return( 1);
      }
      if( ! strcmp( argv[ i], "--bench"))      // this is why we are here
         continue;
      if( ! strcmp( argv[ i], "--quiet"))
      {
         options->quiet = 1;
         continue;
      }
      if( ! strcmp( argv[ i], "--stdio"))
      {
         options->use_tcp = 0;
         continue;
      }
      if( ! strcmp( argv[ i], "--threads") && i + 1 < argc)
      {
         options->threads = strtol( argv[ ++i], NULL, 10);
         continue;
      }
      if( ! strcmp( argv[ i], "--seconds") && i + 1 < argc)
      {
         options->seconds = strtod( argv[ ++i], NULL);
         continue;
      }
      if( ! strcmp( argv[ i], "--pipeline") && i + 1 < argc)
      {
         options->pipeline = strtol( argv[ ++i], NULL, 10);
         continue;
      }
      if( ! strcmp( argv[ i], "--value-size") && i + 1 < argc)
      {
         options->value_size = strtol( argv[ ++i], NULL, 10);
         continue;
      }
      if( ! strcmp( argv[ i], "--keys") && i + 1 < argc)
      {
         options->keys = strtol( argv[ ++i], NULL, 10);
         continue;
      }
      if( ! strcmp( argv[ i], "--interval") && i + 1 < argc)
      {
         options->interval = strtod( argv[ ++i], NULL);
         continue;
      }
      if( ! strcmp( argv[ i], "--server") && i + 1 < argc)
      {
         options->server = argv[ ++i];
         continue;
      }
      if( ! strcmp( argv[ i], "--connect") && i + 1 < argc)
      {
         if( split_spec( options, argv[ ++i]))
         {
            mulle_fprintf( stderr, "mredis-stress: --connect wants HOST:PORT or /path\n");
            return( -1);
         }
         continue;
      }
      if( ! strcmp( argv[ i], "--redis") && i + 1 < argc)
      {
         options->redis = argv[ ++i];
         continue;
      }
      if( ! strcmp( argv[ i], "--compare"))
      {
         options->compare = 1;
         continue;
      }

      mulle_fprintf( stderr, "mredis-stress: unknown bench option %s\n", argv[ i]);
      bench_usage( stderr);
      return( -1);
   }

   if( options->threads < 1)
      options->threads = 1;
   if( options->threads > BENCH_MAX_THREADS)
      options->threads = BENCH_MAX_THREADS;
   if( options->pipeline < 1)
      options->pipeline = 1;
   if( options->keys < 1)
      options->keys = 1;
   if( options->value_size < 1)
      options->value_size = 1;
   if( options->value_size > BENCH_MAX_VALUE)
      options->value_size = BENCH_MAX_VALUE;
   if( options->seconds < 0.1)
      options->seconds = 0.1;

   if( options->connect_host || options->connect_unix)
   {
      if( options->connect_host)
      {
         if( ! options->connect_port)
         {
            mulle_fprintf( stderr, "mredis-stress: --connect %s carries no port\n",
                           options->connect_host);
            return( -1);
         }
         options->use_tcp = 1;
      }
      else
         options->use_tcp = 0;   /* a socket path: neither a port nor a pipe */
   }

   if( ! options->use_tcp && options->threads > 1 && ! options->connect_unix)
   {
      // a pipe is a single full duplex conversation; more threads cannot use it
      mulle_fprintf( stderr,
                     "mredis-stress: --stdio drives one connection, threads forced to 1\n");
      options->threads = 1;
   }
   return( 0);
}


// ---------------------------------------------------------------- one pass

/*
 * Where the bytes go. Three shapes, because a real redis-server is not mredis:
 * mredis announces the port it bound, redis-server does not, and a server
 * somebody else started has to be found rather than started.
 */
typedef enum
{
   BENCH_START_MREDIS,     /* start mredis: it announces its own port */
   BENCH_START_REDIS,      /* start any redis-compatible server on a picked port */
   BENCH_START_CONNECT     /* already running: TCP host:port, or a unix socket */
} bench_start_t;


struct bench_target
{
   bench_start_t   start;
   const char     *label;       /* what the display calls it */
   const char     *tag;         /* what the summary line calls it, no spaces */
   const char     *path;        /* the binary, when we start it */
   const char     *host;
   const char     *unix_path;
   unsigned int    port;
   int             use_stdio;
};


/*
 * Starting a server, or finding one that is already running, is mulle-spawn's
 * job on every platform -- it is the pid the accounting wants, and `close` on
 * the handle that stops it, with no fork, no exec and no signal of our own.
 */
static mulle_socket_t   connect_to( const struct bench_target *target, unsigned int port)
{
   if( target->unix_path)
      return( mredis_connect_unix( target->unix_path));
   return( mredis_connect( target->host ? target->host : "127.0.0.1", port));
}


/*
 * One measurement: start or reach the server, fill it, run the workers for
 * `seconds`, display optionally, and leave the numbers in `result`. Everything
 * the single-target run and each half of `--compare` need, with no globals
 * except the handler for a server that dies mid-run.
 */
static int   bench_pass( const struct bench_options *options,
                         const struct bench_target *target,
                         struct bench_result *result,
                         int clear_screen)
{
   struct bench_stats    stats;
   struct bench_latency  latency;
   struct bench_worker   workers[ BENCH_MAX_THREADS];
   mulle_thread_t        handles[ BENCH_MAX_THREADS];
   const char           *host;
   const char           *transport;
   unsigned int          port;
   mredis_pid_t          server_pid;
   struct mulle_spawn_handle server;
   int                   stdio_read;
   int                   stdio_write;
   int                   spawned;
   long                  i;
   long                  joined;
   double                start;
   double                stop;
   double                deadline;
   double                elapsed;
   double                last_sample;
   long long             peak;
   long                  last_requests;
   long                  last_bytes;
#if MREDIS_HAVE_ACCOUNTING
   double                cpu_start_server;
   double                cpu_start_client;
   double                cpu_stop_server;
   double                cpu_stop_client;
#endif

   memset( result, 0, sizeof( *result));
   result->label = target->label;
   result->tag   = target->tag;

   counter_set( &stats.requests, 0);
   counter_set( &stats.bytes_in, 0);
   counter_set( &stats.bytes_out, 0);
   counter_set( &stats.errors, 0);

   host        = target->host ? target->host : "127.0.0.1";
   port        = target->port;
   server_pid  = 0;
   stdio_read  = -1;
   stdio_write = -1;
   spawned     = 1;

   /*
    * Initialized even though one path below never opens it: the teardown runs
    * either way, and a handle that was never opened is one close can decline.
    */
   mulle_spawn_handle_init( &server);

   switch( target->start)
   {
   case BENCH_START_MREDIS :
      if( target->use_stdio)
      {
         if( mredis_spawn_stdio( target->path, &server))
            return( -1);

         stdio_write = server.in;
         stdio_read  = server.out;
      }
      else
         if( mredis_spawn_server( target->path, &port, &server))
            return( -1);

      server_pid = (mredis_pid_t) server.pid;
      break;

   case BENCH_START_REDIS :
      {
         /*
          * A real server writes a dump file unless told not to, and it does not
          * announce the port it was told to bind. `--save ""` and
          * `--appendonly no` keep the run from leaving anything behind.
          */
         static const char *const   extra[] = { "--save", "", "--appendonly", "no", NULL };

         if( mredis_spawn_tcp_port( target->path, extra, &port, &server))
         {
            mulle_fprintf( stderr, "mredis-stress: cannot start %s\n", target->path);
            return( -1);
         }
         server_pid = (mredis_pid_t) server.pid;
      }
      break;

   case BENCH_START_CONNECT :
      spawned    = 0;
      server_pid = -1;     /* a unix socket has no port to look a server up by */
      if( ! target->unix_path)
      {
         long   pid;

         pid = mredis_pid_for_port( port);
         if( pid > 0)
            server_pid = (mredis_pid_t) pid;
      }
      break;
   }

   /*
    * Fill the keyspace before any worker starts, so that every GET in the
    * measured phase finds a value: a miss would be timed as a request but would
    * not be the workload we think we are measuring. Not counted anywhere.
    */
   {
      struct mredis_wire   wire;
      mulle_socket_t       close_socket;

      close_socket = MULLE_SOCKET_INVALID;

      if( target->use_stdio)
         mredis_wire_init( &wire, stdio_read);
      else
      {
         close_socket = connect_to( target, port);
         if( close_socket == MULLE_SOCKET_INVALID)
         {
            mulle_fprintf( stderr, "mredis-stress: cannot connect to %s for the warm up\n",
                           target->label);
            if( spawned)
               mulle_spawn_close( &server, NULL);
            return( -1);
         }
         mredis_wire_init_socket( &wire, close_socket);
      }

      if( warm_up( &wire, target->use_stdio ? stdio_write : -1, options))
      {
         mulle_fprintf( stderr, "mredis-stress: the keyspace warm up failed on %s\n",
                        target->label);
         if( close_socket != MULLE_SOCKET_INVALID)
            mulle_socket_close( close_socket);
         if( spawned)
            mulle_spawn_close( &server, NULL);
         return( -1);
      }

      if( close_socket != MULLE_SOCKET_INVALID)
         mulle_socket_close( close_socket);
   }

#if MREDIS_HAVE_ACCOUNTING
   /* sampled across the same window as the requests, not before it */
   cpu_start_server = server_pid > 0 ? process_cpu_seconds( server_pid) : -1.0;
   cpu_start_client = client_cpu_seconds();
#endif

   start    = mulle_relativetime_now();
   deadline = start + options->seconds;

   for( i = 0; i < options->threads; i++)
   {
      /*
       * Zero first, then fill in. The histogram is a member of this struct and
       * an untouched bucket is a real value: it was 0 samples, not "whatever
       * was on the stack", which is what an un-zeroed array reports and what
       * made the first version of the histogram print enormous negative counts
       * as soon as the window reached a bucket nothing had landed in.
       */
      memset( &workers[ i], 0, sizeof( workers[ i]));

      workers[ i].stats        = &stats;
      workers[ i].host         = host;
      workers[ i].unix_path    = target->unix_path;
      workers[ i].port         = port;
      workers[ i].use_stdio    = target->use_stdio;
      workers[ i].read_fd      = target->use_stdio ? stdio_read : -1;
      workers[ i].write_fd     = target->use_stdio ? stdio_write : -1;
      workers[ i].socket       = MULLE_SOCKET_INVALID;
      workers[ i].index        = (int) i;
      workers[ i].pipeline     = options->pipeline;
      workers[ i].value_size   = options->value_size;
      workers[ i].keys         = options->keys;
      workers[ i].deadline     = deadline;
      workers[ i].pattern      = BENCH_PATTERN;
      workers[ i].value        = mulle_malloc( (size_t) options->value_size);
      workers[ i].scratch      = mulle_malloc( (size_t) options->value_size + 2);
      workers[ i].scratch_size = (size_t) options->value_size + 2;
   }

   joined = 0;
   for( i = 0; i < options->threads; i++)
   {
      if( mulle_thread_create( worker_main, &workers[ i], &handles[ i]))
      {
         mulle_fprintf( stderr, "mredis-stress: could not create a thread\n");
         break;
      }
      joined++;
   }

   last_sample   = start;
   last_requests = 0;
   last_bytes    = 0;
   peak          = 0;
   memset( &latency, 0, sizeof( latency));

   while( ! options->quiet && mulle_relativetime_now() < deadline)
   {
      double        now;
      double        delta;
      long long     rate;
      long long     mb_rate;
      long          requests;
      long          bytes;

      if( options->interval > 0)
         mulle_relativetime_sleep( options->interval);

      now      = mulle_relativetime_now();
      requests = counter_get( &stats.requests);
      bytes    = counter_get( &stats.bytes_in) + counter_get( &stats.bytes_out);
      delta    = now - last_sample;
      if( delta <= 0)
         delta = 1e-9;

      rate    = rate_tenths( requests - last_requests, delta);
      mb_rate = mb_rate_tenths( bytes - last_bytes, delta);
      if( rate > peak)
         peak = rate;

      /*
       * Read while the workers are still writing, without synchronisation: a
       * sample that has been counted but not yet placed in a bucket costs one
       * frame's accuracy on a display that is redrawn four times a second, and
       * nothing else. No lock on the measuring path is worth more than that.
       */
      latency_merge( &latency, workers, options->threads);
      {
         unsigned long long   server_ms;
         unsigned long long   client_ms;

         /* deltas so far, so the CPU can be watched while it is being spent */
         server_ms = 0;
         client_ms = 0;
#if MREDIS_HAVE_ACCOUNTING
         {
            double   cpu_now;

            cpu_now = server_pid > 0 ? process_cpu_seconds( server_pid) : -1.0;
            if( cpu_start_server >= 0.0 && cpu_now >= cpu_start_server)
               server_ms = (unsigned long long) ((cpu_now - cpu_start_server) * 1000.0 + 0.5);

            cpu_now = client_cpu_seconds();
            if( cpu_start_client >= 0.0 && cpu_now >= cpu_start_client)
               client_ms = (unsigned long long) ((cpu_now - cpu_start_client) * 1000.0 + 0.5);
         }
#endif
         render_frame( stdout, options, target->label, &stats, server_pid, &latency,
                       now - start, rate, peak, mb_rate,
                       server_ms, client_ms, clear_screen);
      }

      last_sample   = now;
      last_requests = requests;
      last_bytes    = bytes;
   }

   for( i = 0; i < joined; i++)
      (void) mulle_thread_join( handles[ i]);

   stop    = mulle_relativetime_now();
   elapsed = stop - start;

#if MREDIS_HAVE_ACCOUNTING
   cpu_stop_server = server_pid > 0 ? process_cpu_seconds( server_pid) : -1.0;
   cpu_stop_client = client_cpu_seconds();

   if( cpu_start_server >= 0.0 && cpu_stop_server >= cpu_start_server)
      result->server_cpu_ms = (unsigned long long) ((cpu_stop_server - cpu_start_server) * 1000.0 + 0.5);
   if( cpu_start_client >= 0.0 && cpu_stop_client >= cpu_start_client)
      result->client_cpu_ms = (unsigned long long) ((cpu_stop_client - cpu_start_client) * 1000.0 + 0.5);
#endif

   /* every worker has stopped, so this one is exact */
   latency_merge( &latency, workers, options->threads);

   result->requests  = (unsigned long long) counter_get( &stats.requests);
   result->bytes_in  = (unsigned long long) counter_get( &stats.bytes_in);
   result->bytes_out = (unsigned long long) counter_get( &stats.bytes_out);
   result->errors    = (unsigned long long) counter_get( &stats.errors);
   result->seconds   = elapsed;
   result->latency   = latency;

#if MREDIS_HAVE_ACCOUNTING
   if( server_pid > 0)
   {
      long   kb;

      kb = server_rss_kb( server_pid);
      if( kb > 0)
         result->rss_kb = (unsigned long long) kb;
   }
#endif

   transport = target->unix_path ? "unix"
                                 : (target->use_stdio ? "stdio" : "tcp");

   if( ! options->quiet)
   {
      render_frame( stdout, options, target->label, &stats, server_pid, &latency,
                    elapsed,
                    rate_tenths( (long long) result->requests, elapsed),
                    peak,
                    mb_rate_tenths( (long long) (result->bytes_in + result->bytes_out),
                                    elapsed),
                    result->server_cpu_ms,
                    result->client_cpu_ms,
                    clear_screen);
      mulle_fprintf( stdout, "\n");
   }

   render_summary( stdout, options, result->tag, transport, result);

   for( i = 0; i < options->threads; i++)
   {
      /* accepts MULLE_SOCKET_INVALID, so a worker that never got one is fine */
      if( ! target->use_stdio && i < joined)
         mulle_socket_close( workers[ i].socket);
      mulle_free( workers[ i].value);
      mulle_free( workers[ i].scratch);
   }

   if( target->use_stdio)
   {
      mredis_close( stdio_read);
      if( stdio_write != stdio_read)
         mredis_close( stdio_write);

      /* closed by hand, so the handle is told not to do it a second time */
      server.in  = -1;
      server.out = -1;
   }

   if( spawned)
      mulle_spawn_close( &server, NULL);

   return( 0);
}


/*
 * The reason to bench a real redis at all: the same workload, side by side.
 * Ratios are integer percent, printed without a float conversion.
 */
static void   render_comparison( FILE *fp,
                                 const struct bench_options *options,
                                 const struct bench_result *a,
                                 const struct bench_result *b)
{
   char           left[ 32];
   char           right[ 32];
   char           first[ 32];
   char           second[ 32];
   long long      a_rate;
   long long      b_rate;
   long long      a_mb;
   long long      b_mb;
   long long      percent;

   a_rate = rate_tenths( (long long) a->requests, a->seconds > 0 ? a->seconds : 1e-9);
   b_rate = rate_tenths( (long long) b->requests, b->seconds > 0 ? b->seconds : 1e-9);
   a_mb   = mb_rate_tenths( (long long) (a->bytes_in + a->bytes_out),
                            a->seconds > 0 ? a->seconds : 1e-9);
   b_mb   = mb_rate_tenths( (long long) (b->bytes_in + b->bytes_out),
                            b->seconds > 0 ? b->seconds : 1e-9);

   mulle_fprintf( fp, "\ncomparison   same workload, two servers   threads %ld, pipeline %ld,"
                      " %ld B values, %ld keys, %ld.%ld s\n",
                  options->threads,
                  options->pipeline,
                  options->value_size,
                  options->keys,
                  (long) a->seconds,
                  (long) ((a->seconds - (double) (long) a->seconds) * 10));
   mulle_fprintf( fp, "             %-28s %14s %14s %8s\n", "", a->label, b->label, "ratio");

   format_count( left, sizeof( left), a_rate / 10);
   format_count( right, sizeof( right), b_rate / 10);
   percent = a_rate ? (b_rate * 100 / a_rate) : 0;
   mulle_fprintf( fp, "  req/s      %-28s %14s %14s %7lld%%\n", "", left, right, percent);

   snprintf( left, sizeof( left), "%lld.%lld", a_mb / 10, a_mb % 10);
   snprintf( right, sizeof( right), "%lld.%lld", b_mb / 10, b_mb % 10);
   percent = a_mb ? (b_mb * 100 / a_mb) : 0;
   mulle_fprintf( fp, "  MB/s       %-28s %14s %14s %7lld%%\n", "", left, right, percent);

   snprintf( left, sizeof( left), "%s",
             duration_string( first, sizeof( first),
                              latency_percentile( &a->latency, 0.50)));
   snprintf( right, sizeof( right), "%s",
             duration_string( second, sizeof( second),
                              latency_percentile( &b->latency, 0.50)));
   mulle_fprintf( fp, "  p50        %-28s %14s %14s\n", "", left, right);

   snprintf( left, sizeof( left), "%s",
             duration_string( first, sizeof( first),
                              latency_percentile( &a->latency, 0.99)));
   snprintf( right, sizeof( right), "%s",
             duration_string( second, sizeof( second),
                              latency_percentile( &b->latency, 0.99)));
   mulle_fprintf( fp, "  p99        %-28s %14s %14s\n", "", left, right);

   if( a->rss_kb && b->rss_kb)
      mulle_fprintf( fp, "  server RSS %-28s %11llu KB %11llu KB %7lld%%\n",
                     "", a->rss_kb, b->rss_kb,
                     (long long) (b->rss_kb * 100 / a->rss_kb));
   else
      mulle_fprintf( fp, "  server RSS %-28s %14llu %14llu\n", "", a->rss_kb, b->rss_kb);

   /*
    * Who is doing the work, which is what says whether the ratio above is a
    * property of the servers or of the machine the benchmark is running on. A
    * server at ~100% of a core has hit its ceiling; a client at ~100% x threads
    * means the measurement is capped by the client and the ratio is a lower
    * bound. "req/cpu-s" is the efficiency reading: requests answered per second
    * of server CPU, so it says who works harder for the same answer.
    */
   {
      long long   a_elapsed_ms;
      long long   b_elapsed_ms;
      long long   a_eff;
      long long   b_eff;

      a_elapsed_ms = (long long) (a->seconds * 1000.0);
      b_elapsed_ms = (long long) (b->seconds * 1000.0);

      snprintf( left, sizeof( left), "%lld%% of a core",
                a_elapsed_ms > 0
                   ? (long long) (a->server_cpu_ms * 100 / (unsigned long long) a_elapsed_ms)
                   : 0);
      snprintf( right, sizeof( right), "%lld%% of a core",
                b_elapsed_ms > 0
                   ? (long long) (b->server_cpu_ms * 100 / (unsigned long long) b_elapsed_ms)
                   : 0);
      mulle_fprintf( fp, "  server cpu %-28s %14s %14s\n", "", left, right);

      snprintf( left, sizeof( left), "%lld%% of a core",
                a_elapsed_ms > 0
                   ? (long long) (a->client_cpu_ms * 100 / (unsigned long long) a_elapsed_ms)
                   : 0);
      snprintf( right, sizeof( right), "%lld%% of a core",
                b_elapsed_ms > 0
                   ? (long long) (b->client_cpu_ms * 100 / (unsigned long long) b_elapsed_ms)
                   : 0);
      mulle_fprintf( fp, "  client cpu %-28s %14s %14s\n", "", left, right);

      a_eff = a->server_cpu_ms ? (long long) (a->requests * 1000ULL / a->server_cpu_ms) : 0;
      b_eff = b->server_cpu_ms ? (long long) (b->requests * 1000ULL / b->server_cpu_ms) : 0;
      format_count( left, sizeof( left), a_eff);
      format_count( right, sizeof( right), b_eff);
      percent = a_eff ? (b_eff * 100 / a_eff) : 0;
      mulle_fprintf( fp, "  req/cpu-s  %-28s %14s %14s %7lld%%\n", "", left, right, percent);
   }

   mulle_fprintf( fp, "  errors     %-28s %14llu %14llu\n", "", a->errors, b->errors);

   /* the line a test can read: no spaces inside the values */
   mulle_fprintf( fp,
                  "compare: a=%s b=%s a_req_per_s=%lld.%lld b_req_per_s=%lld.%lld"
                  " b_of_a_pct=%lld a_mb_per_s=%lld.%lld b_mb_per_s=%lld.%lld"
                  " a_p50_ns=%llu b_p50_ns=%llu a_p99_ns=%llu b_p99_ns=%llu"
                  " a_rss_kb=%llu b_rss_kb=%llu"
                  " a_server_cpu_ms=%llu b_server_cpu_ms=%llu"
                  " a_client_cpu_ms=%llu b_client_cpu_ms=%llu"
                  " a_errors=%llu b_errors=%llu\n",
                  a->tag, b->tag,
                  a_rate / 10, a_rate % 10,
                  b_rate / 10, b_rate % 10,
                  a_rate ? (b_rate * 100 / a_rate) : 0,
                  a_mb / 10, a_mb % 10,
                  b_mb / 10, b_mb % 10,
                  latency_percentile( &a->latency, 0.50),
                  latency_percentile( &b->latency, 0.50),
                  latency_percentile( &a->latency, 0.99),
                  latency_percentile( &b->latency, 0.99),
                  a->rss_kb, b->rss_kb,
                  a->server_cpu_ms, b->server_cpu_ms,
                  a->client_cpu_ms, b->client_cpu_ms,
                  a->errors, b->errors);
   fflush( fp);
}


// --------------------------------------------------------------------- entry

/* which server a run means: started by us, or already running somewhere */
static void   set_default_target( struct bench_target *target,
                                  const struct bench_options *options)
{
   if( options->connect_host || options->connect_unix)
   {
      target->start     = BENCH_START_CONNECT;
      target->use_stdio = 0;
      target->unix_path = options->connect_unix;
      target->host      = options->connect_host;
      target->port      = options->connect_port;
      target->label     = options->spec;
      target->tag       = options->connect_unix ? "connected-unix" : "connected";
      return;
   }

   target->host   = options->host;
   target->path   = options->server;
   target->label  = options->use_tcp ? "mredis (spawned, tcp)" : "mredis (spawned, stdio)";
   target->tag    = options->use_tcp ? "mredis-spawned" : "mredis-stdio";
   target->start  = BENCH_START_MREDIS;
   target->use_stdio = ! options->use_tcp;
}


int   mredis_bench_main( int argc, char *argv[])
{
   struct bench_options   options;
   struct bench_target    target;
   struct bench_target    other;
   struct bench_result    result;
   struct bench_result    second;
   int                    rval;
   int                    parsed;

   parsed = parse_options( &options, argc, argv);
   if( parsed)
      return( parsed > 0 ? 0 : 2);

#if !MREDIS_HAVE_ACCOUNTING
   /*
    * The benchmark reports the server's memory and CPU beside its own, and those
    * come from the kernel: /proc, libproc, or the process handle. A platform
    * that will not answer gets no benchmark rather than a zero that reads like a
    * measurement.
    */
   mulle_fprintf( stderr, "mredis-stress: --bench needs process accounting,"
                          " which this platform does not have\n");
   return( 2);
#else
# if defined( SIGPIPE)
   signal( SIGPIPE, SIG_IGN);
# endif

   if( options.compare && ! options.redis)
   {
      mulle_fprintf( stderr, "mredis-stress: --compare needs --redis PATH\n");
      return( 2);
   }

   memset( &target, 0, sizeof( target));
   set_default_target( &target, &options);

   /*
    * --redis without --compare is the plain "bench a real redis" case, so the
    * real server becomes the one target rather than the second half of a pair.
    */
   if( options.redis && ! options.compare)
   {
      memset( &target, 0, sizeof( target));
      target.start     = BENCH_START_REDIS;
      target.path      = options.redis;
      target.label     = "redis-server (spawned, tcp)";
      target.tag       = "redis-spawned";
   }

   rval = bench_pass( &options, &target, &result, 1);
   if( rval)
      return( 2);

   if( ! options.compare)
      return( result.errors ? 1 : 0);

   memset( &other, 0, sizeof( other));
   other.start = BENCH_START_REDIS;
   other.path  = options.redis;
   other.label = options.redis;
   other.tag   = "redis-spawned";

   if( bench_pass( &options, &other, &second, 0))
      return( 2);

   render_comparison( stdout, &options, &result, &second);
   return( (result.errors || second.errors) ? 1 : 0);
#endif
}
