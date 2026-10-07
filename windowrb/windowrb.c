/*
 * windowrb — many short-lived ordered sets over one stream, built on mulle-core.
 *
 *   windowrb --stream-len 20000 --window-size 64 --max-active 8
 *   windowrb --stdin < sensor.log --window-size 128 --threshold 3
 *   windowrb --baseline --verify --quiet
 *
 * The workload: a stream of values, and windows over it. A new window opens
 * every --start-every samples while fewer than --max-active are open; every
 * sample goes into every open window; a window that reaches --window-size is
 * queried (its median, and how many of its values are outliers) and then
 * destroyed. The trees are deliberately short-lived, so creating and tearing
 * down an ordered set is a visible fraction of the run rather than a rounding
 * error.
 *
 * Why an ordered set at all: the query is order statistics -- a median, plus a
 * count of the values in the tails -- asked of many independent windows that
 * overlap in time. One sorted structure per window answers that by walking the
 * outliers, not the window, and it answers a *different* question ("top 5%",
 * "how many below -1") without changing structure. It is not that a red-black
 * tree beats a ring buffer at sum/min/max -- it does not -- which is why
 * --baseline runs the same windows as an array plus mulle_qsort_r, so the trade
 * is in the output rather than in a claim.
 *
 * What each constituent is doing here:
 *
 *   mulle-rbtree   one tree per window, in extra-memory mode with
 *                  node_extra = sizeof( double), so the sample lives *in* the
 *                  node: an insert copies 8 bytes and allocates no payload.
 *                  mulle_rbtree_option_allow_duplicates makes it a multiset,
 *                  which a stream of measurements needs.
 *   mulle-storage  the tree's nodes come from the tree's own mulle_storage
 *                  (_mulle__rbtree_new_node() is _mulle_storage_malloc()), and
 *                  the window objects themselves come from a
 *                  mulle_indexedstorage pool whose freed slots are reused, so
 *                  thousands of window lifetimes do not churn the system
 *                  allocator.
 *   mulle-arena    one arena per window is the tree's allocator, so a window's
 *                  nodes (and, under --baseline, its value array) are a couple
 *                  of pages and destroying the window is one release.
 *                  --shared-arena runs the same stream with a single arena for
 *                  every window, which is the measurement of how much of the
 *                  cost that actually is.
 *   mulle-data     mulle_qsort_r is the baseline's sort.
 *   mulle-time     wall-clock, so "many short-lived trees" has a number.
 *   mulle-buffer,
 *   mulle-dtostr   reading samples from stdin or a file: the input goes into a
 *                  mulle_buffer and is tokenized with mulle_strtod.
 *   mulle-fprintf  the report. No %f: a double is printed as hundredths with
 *                  integer arithmetic, the way logtop does it, because
 *                  mulle-sprintf documents no float conversions.
 *
 * There is no threading here on purpose: mulle-rbtree and mulle-storage are not
 * thread-safe, and what this demo is about is lifetime, not concurrency.
 */

#include <mulle-core/mulle-core.h>

#include <errno.h>
#include <math.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 * ------------------------------------------------------------------
 * tuning
 * ------------------------------------------------------------------
 */

#define DEFAULT_STREAM_LEN   20000
#define DEFAULT_WINDOW_SIZE  64
#define DEFAULT_MAX_ACTIVE   8
#define DEFAULT_START_EVERY  16
#define DEFAULT_THRESHOLD    2.0
#define DEFAULT_SEED         42

#define MAX_ACTIVE_LIMIT     256        /* active[] is a fixed array */
#define MAX_TAIL_REPORTS     32
#define PROGRESS_STEPS       20         /* progress every 5% */

/* the node pool bucket size mulle--rbtree hardcodes (see the summary output) */
#define RBTREE_STORAGE_CAPACITY  32


/*
 * ------------------------------------------------------------------
 * options
 * ------------------------------------------------------------------
 */

static struct
{
   unsigned long   stream_len;
   unsigned long   window_size;
   unsigned long   max_active;
   unsigned long   start_every;
   double          threshold;
   long long       seed;
   int             use_stdin;
   int             quiet;
   int             tail;             /* print the last N reports at the end */
   int             baseline;         /* also run the qsort-backed windows */
   int             verify;           /* ... and insist that they agree */
   int             shared_arena;     /* one arena for every window */
   int             perturb;          /* test hook: break the baseline */
   int             live;             /* redraw an active-window bar */
   double          delay_ms;         /* pace the stream */
   const char     *path;             /* FILE argument, NULL for generated */
} options;


static void   usage( FILE *fp, const char *name)
{
   mulle_fprintf( fp,
      "usage: %s [options] [FILE]\n"
      "\n"
      "Streams values through many short-lived ordered sets: a window opens every\n"
      "--start-every samples while fewer than --max-active are open, every sample\n"
      "goes into every open window, and a window reaching --window-size is queried\n"
      "for its median and its outlier count, then destroyed. With no FILE the\n"
      "stream is generated; --stdin reads it from stdin instead.\n"
      "\n"
      "  -n, --stream-len N     generated samples (default %d)\n"
      "  -W, --window-size W    samples per window (default %d)\n"
      "  -M, --max-active M     windows alive at once (default %d)\n"
      "  -S, --start-every S    open a window every S samples (default %d)\n"
      "  -T, --threshold T      |value| above this is an outlier (default %.1f)\n"
      "      --seed S           seed for the generated stream (default %d)\n"
      "      --stdin            read samples from stdin\n"
      "  -q, --quiet            only the summary\n"
      "      --tail N           with --quiet, print the last N reports\n"
      "      --baseline         also run windows backed by an array + mulle_qsort_r\n"
      "      --verify           run both and check that they agree\n"
      "      --shared-arena     one arena for all windows instead of one each\n"
      "      --perturb          test hook: corrupt the baseline's answers\n"
      "      --live             redraw an active-window bar\n"
      "      --delay-ms MS      pace the stream by MS per sample\n"
      "  -h, --help             this text\n"
      "\n"
      "Examples:\n"
      "  %s --stream-len 100000 --window-size 32 --max-active 5 --live\n"
      "  cat sensor.log | %s --stdin --window-size 128 --threshold 3 --verify\n",
      name, DEFAULT_STREAM_LEN, DEFAULT_WINDOW_SIZE, DEFAULT_MAX_ACTIVE,
      DEFAULT_START_EVERY, DEFAULT_THRESHOLD, DEFAULT_SEED, name, name);
}


static int   parse_arguments( int argc, char *argv[])
{
   int    i;
   char  *arg;

   options.stream_len  = DEFAULT_STREAM_LEN;
   options.window_size = DEFAULT_WINDOW_SIZE;
   options.max_active  = DEFAULT_MAX_ACTIVE;
   options.start_every = DEFAULT_START_EVERY;
   options.threshold   = DEFAULT_THRESHOLD;
   options.seed        = DEFAULT_SEED;

   for( i = 1; i < argc; i++)
   {
      arg = argv[ i];

      if( ! strcmp( arg, "-h") || ! strcmp( arg, "--help"))
      {
         usage( stdout, argv[ 0]);
         exit( 0);
      }
      else if( ! strcmp( arg, "-q") || ! strcmp( arg, "--quiet"))
         options.quiet = 1;
      else if( ! strcmp( arg, "--stdin"))
         options.use_stdin = 1;
      else if( ! strcmp( arg, "--baseline"))
         options.baseline = 1;
      else if( ! strcmp( arg, "--verify"))
         options.verify = 1;
      else if( ! strcmp( arg, "--shared-arena"))
         options.shared_arena = 1;
      else if( ! strcmp( arg, "--perturb"))
         options.perturb = 1;
      else if( ! strcmp( arg, "--live"))
         options.live = 1;
      else if( i + 1 < argc &&
               (! strcmp( arg, "-n") || ! strcmp( arg, "--stream-len") ||
                ! strcmp( arg, "-W") || ! strcmp( arg, "--window-size") ||
                ! strcmp( arg, "-M") || ! strcmp( arg, "--max-active") ||
                ! strcmp( arg, "-S") || ! strcmp( arg, "--start-every") ||
                ! strcmp( arg, "-T") || ! strcmp( arg, "--threshold") ||
                ! strcmp( arg, "--seed") || ! strcmp( arg, "--tail") ||
                ! strcmp( arg, "--delay-ms")))
      {
         const char   *value;

         value = argv[ ++i];
         if( ! strcmp( arg, "-n") || ! strcmp( arg, "--stream-len"))
            options.stream_len = (unsigned long) atol( value);
         else if( ! strcmp( arg, "-W") || ! strcmp( arg, "--window-size"))
            options.window_size = (unsigned long) atol( value);
         else if( ! strcmp( arg, "-M") || ! strcmp( arg, "--max-active"))
            options.max_active = (unsigned long) atol( value);
         else if( ! strcmp( arg, "-S") || ! strcmp( arg, "--start-every"))
            options.start_every = (unsigned long) atol( value);
         else if( ! strcmp( arg, "-T") || ! strcmp( arg, "--threshold"))
            options.threshold = mulle_strtod( value, NULL);
         else if( ! strcmp( arg, "--seed"))
            options.seed = (long long) atoll( value);
         else if( ! strcmp( arg, "--tail"))
            options.tail = atoi( value);
         else
            options.delay_ms = mulle_strtod( value, NULL);
      }
      else if( arg[ 0] == '-' && arg[ 1])
      {
         mulle_fprintf( stderr, "%s: unknown option %s\n", argv[ 0], arg);
         usage( stderr, argv[ 0]);
         return( -1);
      }
      else if( options.path)
      {
         mulle_fprintf( stderr, "%s: one FILE at a time\n", argv[ 0]);
         return( -1);
      }
      else
         options.path = arg;
   }

   if( ! options.window_size)
   {
      mulle_fprintf( stderr, "%s: --window-size must be at least 1\n", argv[ 0]);
      return( -1);
   }
   if( ! options.max_active)
   {
      mulle_fprintf( stderr, "%s: --max-active must be at least 1\n", argv[ 0]);
      return( -1);
   }
   if( options.max_active > MAX_ACTIVE_LIMIT)
   {
      mulle_fprintf( stderr, "%s: --max-active is limited to %d here\n",
                     argv[ 0], MAX_ACTIVE_LIMIT);
      return( -1);
   }
   if( ! options.start_every)
   {
      mulle_fprintf( stderr, "%s: --start-every must be at least 1\n", argv[ 0]);
      return( -1);
   }
   if( options.seed == 0)
      options.seed = DEFAULT_SEED;      /* 0 is not a state for xorshift */
   if( options.tail < 0)
      options.tail = 0;
   if( options.tail > MAX_TAIL_REPORTS)
      options.tail = MAX_TAIL_REPORTS;
   if( options.delay_ms < 0.0)
      options.delay_ms = 0.0;

   return( 0);
}


/*
 * ------------------------------------------------------------------
 * doubles without printf %f
 *
 * mulle-sprintf documents no conversion table, so a double is scaled to
 * hundredths and printed as two integers. Rounds half away from zero, which is
 * what a report wants and what C's printf does not.
 * ------------------------------------------------------------------
 */

static void   format_hundredths( char *buf, size_t size, double value)
{
   long long   scaled;
   const char  *sign;

   scaled = (long long) (value * 100.0 + (value < 0.0 ? -0.5 : 0.5));
   sign   = scaled < 0 ? "-" : "";
   if( scaled < 0)
      scaled = -scaled;
   mulle_snprintf( buf, size, "%s%lld.%02lld", sign, scaled / 100, scaled % 100);
}


static void   print_hundredths( FILE *fp, double value)
{
   char   text[ 40];

   format_hundredths( text, sizeof( text), value);
   mulle_fprintf( fp, "%s", text);
}


/*
 * ------------------------------------------------------------------
 * a small, reproducible normal generator
 *
 * Not from mulle-core: the library has no random numbers, and a demo whose
 * --seed has to mean the same stream on every libc cannot use rand().
 * xorshift64* plus Box-Muller is both.
 * ------------------------------------------------------------------
 */

static unsigned long long   rng_state;
static int                  gaussian_has_spare;
static double               gaussian_spare;

static void   rng_seed( long long seed)
{
   rng_state = (unsigned long long) seed * 0x9E3779B97F4A7C15ULL + 0x2545F4914F6CDD1DULL;
   if( ! rng_state)
      rng_state = 0x2545F4914F6CDD1DULL;
   gaussian_has_spare = 0;
}


static double   rng_uniform( void)
{
   unsigned long long   x;

   x          = rng_state;
   x         ^= x >> 12;
   x         ^= x << 25;
   x         ^= x >> 27;
   rng_state  = x;
   return( (double) ((x * 0x2545F4914F6CDD1DULL) >> 11) / 9007199254740992.0);
}


static double   next_gaussian( void)
{
   double   u;
   double   v;
   double   radius;

   if( gaussian_has_spare)
   {
      gaussian_has_spare = 0;
      return( gaussian_spare);
   }

   do
   {
      u      = 2.0 * rng_uniform() - 1.0;
      v      = 2.0 * rng_uniform() - 1.0;
      radius = u * u + v * v;
   }
   while( radius >= 1.0 || radius == 0.0);

   {
      double   factor;

      factor            = sqrt( -2.0 * log( radius) / radius);
      gaussian_spare    = v * factor;
      gaussian_has_spare = 1;
      return( u * factor);
   }
}


/*
 * ------------------------------------------------------------------
 * windows
 *
 * One slot per window out of a mulle_indexedstorage; the slots are recycled.
 * _mulle_indexedstorage_get() hands out a pointer into its struct array, and the
 * next alloc could realloc that array out from under a live window -- so the
 * pool is primed to --max-active entries at startup, which is as far as it can
 * ever grow, since no more than that many windows are live at once.
 * ------------------------------------------------------------------
 */

struct window
{
   struct mulle_arena    arena;      /* owns this window's memory */
   struct mulle_rbtree   tree;       /* the ordered multiset, extra mode */
   double               *values;     /* baseline: the same samples, unsorted */
   unsigned long         id;         /* creation ordinal, for reports */
   unsigned long         count;      /* samples inserted so far */
   unsigned long         pages;      /* pages the arena is holding */
};

/* one finished window, kept so --verify can compare the backends */
struct record
{
   unsigned long   id;
   unsigned long   closed_at;
   unsigned long   outliers;
   double          median;
};

struct log
{
   struct record   *records;
   size_t           count;
   size_t           capacity;
};

struct runstats
{
   unsigned long long   samples;
   unsigned long long   created;
   unsigned long long   destroyed;
   unsigned long long   reports;
   unsigned long long   outliers;
   unsigned long long   nodes;          /* nodes the tree pools handed out */
   unsigned long long   pages;          /* arena pages allocated, total */
   unsigned long long   pages_peak;     /* pages any one window held */
   unsigned long long   slots_peak;     /* concurrent windows, at most */
   unsigned long long   errors;        /* failed inserts, or lost records */
   double               seconds;
};

static struct mulle_indexedstorage   window_slots;
static struct mulle_arena            shared_arena;     /* for --shared-arena */
static unsigned int                  active[ MAX_ACTIVE_LIMIT];
static unsigned long                 active_count;
static unsigned long                 window_serial;    /* ids, not slots */

static struct runstats               tree_stats;
static struct runstats               array_stats;
static struct log                    tree_log;
static struct log                    array_log;

/* the tail ring, for --tail (and --quiet --tail N) */
static struct record   tail_reports[ MAX_TAIL_REPORTS];
static unsigned long   tail_next;

enum
{
   backend_tree = 0,
   backend_array = 1
};


/*
 * ------------------------------------------------------------------
 * comparisons
 *
 * Two functions, not one cast between them: mulle-rbtree wants
 * `int (*)( void *, void *)` and mulle_qsort_r wants `int (*)( void *, void *,
 * void *)`, and calling one through the other's type is exactly the kind of
 * thing this project files reports about.
 * ------------------------------------------------------------------
 */

static int   compare_values( void *a, void *b)
{
   double   x;
   double   y;

   x = *(double *) a;
   y = *(double *) b;
   return( x < y ? -1 : (x > y ? 1 : 0));
}


static int   compare_values_qsort( void *a, void *b, void *thunk)
{
   (void) thunk;
   return( compare_values( a, b));
}


/*
 * ------------------------------------------------------------------
 * the sample stream
 *
 * Read once, into memory, so --baseline and --verify see byte-identical input
 * and the passes cannot drift apart through re-reading.
 * ------------------------------------------------------------------
 */

struct stream
{
   double  *values;
   size_t   count;
   size_t   capacity;
};


static int   stream_add( struct stream *stream, double value)
{
   double   *values;
   size_t    capacity;

   if( stream->count == stream->capacity)
   {
      capacity = stream->capacity ? stream->capacity * 2 : 1024;
      values   = mulle_realloc( stream->values, capacity * sizeof( double));
      if( ! values)
         return( -1);
      stream->values   = values;
      stream->capacity = capacity;
   }

   stream->values[ stream->count++] = value;
   return( 0);
}


static int   stream_from_gaussian( struct stream *stream, unsigned long count)
{
   unsigned long   i;

   rng_seed( options.seed);
   for( i = 0; i < count; i++)
      if( stream_add( stream, next_gaussian()))
         return( -1);
   return( 0);
}


/*
 * One number per token, whitespace separated, so a file, a pipe and a fixture
 * all work. mulle_strtod is the exact inverse of mulle_dtostr.
 */
static int   stream_from_text( struct stream *stream, const char *bytes)
{
   char    *p;
   char    *end;
   double   value;

   p = (char *) bytes;
   for(;;)
   {
      while( *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
         p++;
      if( ! *p)
         return( 0);

      value = mulle_strtod( p, &end);
      if( end == p)
         return( -1);
      if( stream_add( stream, value))
         return( -1);
      p = end;
   }
}


static int   stream_read_file( struct stream *stream, FILE *fp)
{
   struct mulle_buffer   buffer;
   char                  scratch[ 4096];
   char                 *bytes;
   size_t                n;
   int                   rval;

   mulle_buffer_init( &buffer, 0, NULL);

   while( ! feof( fp))
   {
      n = fread( scratch, 1, sizeof( scratch), fp);
      if( n)
         mulle_buffer_add_bytes( &buffer, scratch, n);
      if( ferror( fp))
      {
         mulle_buffer_done( &buffer);
         return( -1);
      }
   }

   bytes = mulle_buffer_get_string( &buffer);
   rval  = bytes ? stream_from_text( stream, bytes) : -1;
   mulle_buffer_done( &buffer);
   return( rval);
}


static void   stream_done( struct stream *stream)
{
   mulle_free( stream->values);
   stream->values = NULL;
}


/*
 * ------------------------------------------------------------------
 * records
 * ------------------------------------------------------------------
 */

static int   log_add( struct log *log, struct record *record)
{
   struct record   *records;
   size_t           capacity;

   if( log->count == log->capacity)
   {
      capacity = log->capacity ? log->capacity * 2 : 256;
      records  = mulle_realloc( log->records, capacity * sizeof( struct record));
      if( ! records)
         return( -1);
      log->records  = records;
      log->capacity = capacity;
   }

   log->records[ log->count++] = *record;
   return( 0);
}


static void   log_done( struct log *log)
{
   mulle_free( log->records);
   log->records = NULL;
   log->count   = 0;
   log->capacity = 0;
}


/*
 * ------------------------------------------------------------------
 * windows: create, query, destroy
 * ------------------------------------------------------------------
 */

static struct window   *window_at( unsigned int index)
{
   return( (struct window *) _mulle_indexedstorage_get( &window_slots, index));
}


static struct mulle_allocator   *window_allocator( struct window *window)
{
   return( options.shared_arena ? _mulle_arena_get_allocator( &shared_arena)
                                : _mulle_arena_get_allocator( &window->arena));
}


static int   window_open( unsigned int index, int backend)
{
   struct mulle_rbtree_config   config;
   struct window               *window;

   window         = window_at( index);
   window->id     = ++window_serial;
   window->count  = 0;
   window->values = NULL;
   window->pages  = 0;

   if( ! options.shared_arena)
      mulle_arena_init( &window->arena, 0, NULL);

   memset( &config, 0, sizeof( config));
   config.comparison = compare_values;
   config.node_extra = sizeof( double);
   config.options    = mulle_rbtree_option_use_extra |
                       mulle_rbtree_option_allow_duplicates;

   /*
    * The bucket size of the node pool is not configurable here: mulle--rbtree
    * passes 32 to _mulle_storage_init(). So a window of 64 samples grows its
    * pool in two buckets, and one of 1000 in 32 -- which is the kind of thing
    * that matters when the pool is this short-lived, and is reported in the
    * summary rather than hidden.
    */
   mulle_rbtree_init_with_config( &window->tree, &config, window_allocator( window));

   if( backend == backend_array)
   {
      struct mulle_arena   *arena;

      arena          = options.shared_arena ? &shared_arena : &window->arena;
      window->values = mulle_arena_alloc( arena,
                                          options.window_size * sizeof( double),
                                          alignof( double));
      if( ! window->values)
         return( -1);
   }
   return( 0);
}


static void   window_destroy( struct window *window)
{
   mulle_rbtree_done( &window->tree);
   window->values = NULL;
   if( options.shared_arena)
      return;

   window->pages = mulle_arena_get_page_count( &window->arena);
   mulle_arena_done( &window->arena);
}


/*
 * The median is the lower middle of an even count, and each tail is counted by
 * walking in from its end while the value is an outlier -- which is what the
 * ordered set buys: a tail count costs the outliers, not the window.
 *
 * mulle-rbtree has no rank or select, and _mulle__rbtree_get_count() is a
 * recursive walk, so "the element at index k" is a walk to k. At window sizes
 * like these that is the same order of cost as the insert that built the
 * window; it is worth knowing before reaching for it at one where it is not.
 */
static double   tree_median( struct mulle_rbtree *tree, unsigned long count)
{
   struct mulle_rbtreeenumerator   rover;
   unsigned long                   wanted;
   unsigned long                   i;
   double                          median;
   double                         *value;

   wanted = (count - 1) / 2;
   median = 0.0;

   i     = 0;
   rover = mulle_rbtree_enumerate( tree);
   while( _mulle_rbtreeenumerator_next( &rover, (void **) &value))
   {
      if( i == wanted)
      {
         median = *value;
         break;
      }
      i++;
   }
   return( median);
}


static unsigned long   tree_outliers( struct mulle_rbtree *tree, double threshold)
{
   struct mulle_rbtreeenumerator          rover;
   struct mulle_rbtreereverseenumerator   reverse;
   unsigned long                          count;
   double                                *value;

   count   = 0;
   reverse = mulle_rbtree_reverseenumerate( tree);
   while( _mulle_rbtreereverseenumerator_next( &reverse, (void **) &value))
   {
      if( *value <= threshold)
         break;
      count++;
   }

   rover = mulle_rbtree_enumerate( tree);
   while( _mulle_rbtreeenumerator_next( &rover, (void **) &value))
   {
      if( *value >= -threshold)
         break;
      count++;
   }
   return( count);
}


static unsigned long   array_outliers( double *values, unsigned long count,
                                       double threshold)
{
   unsigned long   i;
   unsigned long   outliers;

   outliers = 0;
   for( i = 0; i < count; i++)
      if( values[ i] > threshold || values[ i] < -threshold)
         outliers++;
   return( outliers);
}


static void   emit_report( unsigned long window_index, unsigned long closed_at,
                           double median, unsigned long outliers)
{
   char   text[ 40];

   if( options.tail)
   {
      struct record   *report;

      report               = &tail_reports[ tail_next++ % (unsigned long) options.tail];
      report->id           = window_index;
      report->closed_at    = closed_at;
      report->median       = median;
      report->outliers     = outliers;
   }

   if( options.quiet)
      return;

   format_hundredths( text, sizeof( text), median);
   mulle_fprintf( stdout, "[window %lu finished @ %lu] median=%s  outliers=%lu"
                          "  (lived %lu samples)\n",
                  window_index, closed_at, text, outliers, options.window_size);
}


static void   close_window( struct runstats *stats, struct log *log,
                            struct window *window, unsigned long closed_at)
{
   struct record   record;
   double          median;
   unsigned long   outliers;

   if( window->count == 0)
   {
      median   = 0.0;
      outliers = 0;
   }
   else
      if( log == &tree_log)
      {
         median   = tree_median( &window->tree, window->count);
         outliers = tree_outliers( &window->tree, options.threshold);
      }
      else
      {
         mulle_qsort_r( window->values, window->count, sizeof( double),
                        compare_values_qsort, NULL);
         median   = window->values[ (window->count - 1) / 2];
         outliers = array_outliers( window->values, window->count, options.threshold);
         if( options.perturb)
            outliers++;            /* --verify's test hook: make this one wrong */
      }

   record.id        = window->id;
   record.closed_at = closed_at;
   record.median    = median;
   record.outliers  = outliers;
   if( log_add( log, &record))
      stats->errors++;

   stats->reports++;
   stats->outliers += outliers;
   emit_report( window->id, closed_at, median, outliers);
}


/*
 * One pass over the stream. Everything that is a property of the pass lands in
 * `stats` and `log`, so the two backends can be compared record by record.
 */
static int   run_stream( struct stream *stream, int backend,
                         struct runstats *stats, struct log *log)
{
   unsigned long   step;
   unsigned long   t;
   int             i;

   memset( stats, 0, sizeof( *stats));
   active_count  = 0;
   window_serial = 0;
   tail_next     = 0;
   memset( tail_reports, 0, sizeof( tail_reports));
   memset( active, 0, sizeof( active));

   step = (unsigned long) stream->count / PROGRESS_STEPS;
   if( ! step)
      step = 1;
   stats->seconds = mulle_relativetime_now();

   for( t = 0; t < stream->count; t++)
   {
      double   value;

      value = stream->values[ t];

      /*
       * Open before inserting, so a window opened at sample t holds samples
       * t .. t + window_size - 1. A window that would have opened while the
       * limit was reached is not queued: it is gone, which is what "while under
       * the limit" means for a staggered stream.
       */
      if( ! (t % options.start_every) && active_count < options.max_active)
      {
         unsigned int   slot;

         slot = _mulle_indexedstorage_alloc( &window_slots);
         if( window_open( slot, backend))
            return( -1);
         active[ active_count++] = slot;
         stats->created++;
         if( active_count > stats->slots_peak)
            stats->slots_peak = active_count;
      }

      for( i = 0; i < (int) active_count; i++)
      {
         struct window   *window;

         window = window_at( active[ i]);

         if( backend == backend_tree)
         {
            if( mulle_rbtree_add( &window->tree, &value))
               stats->errors++;
            else
               stats->nodes++;
         }
         else
            window->values[ window->count] = value;

         window->count++;

         if( window->count == options.window_size)
         {
            close_window( stats, log, window, t);
            window_destroy( window);
            stats->destroyed++;
            if( window->pages > stats->pages_peak)
               stats->pages_peak = window->pages;
            stats->pages += window->pages;

            _mulle_indexedstorage_free( &window_slots, active[ i]);
            active[ i] = active[ --active_count];
            i--;
         }
      }

      if( options.live)
      {
         if( ! (t % step))
         {
            char   bar[ MAX_ACTIVE_LIMIT + 1];
            int    b;

            for( b = 0; b < (int) options.max_active; b++)
               bar[ b] = (unsigned long) b < active_count ? '#' : '.';
            bar[ options.max_active] = 0;
            mulle_fprintf( stdout, "active [%s] %lu/%lu  t=%lu\n",
                           bar, active_count, options.max_active, t + 1);
         }
      }
      else
         if( ! options.quiet && ! (t % step))
            mulle_fprintf( stdout, "t=%lu  active=%lu  created=%llu  destroyed=%llu\n",
                           t, active_count, stats->created, stats->destroyed);

      if( options.delay_ms > 0.0)
         mulle_relativetime_sleep( options.delay_ms / 1000.0);
   }

   /*
    * Whatever is still open when the stream ends is torn down, not reported: a
    * window that never reached --window-size has no answer to give. Counting
    * its destruction is what makes created == destroyed hold.
    */
   while( active_count)
   {
      struct window   *window;

      window = window_at( active[ --active_count]);
      window_destroy( window);
      if( window->pages > stats->pages_peak)
         stats->pages_peak = window->pages;
      stats->pages += window->pages;
      stats->destroyed++;
      _mulle_indexedstorage_free( &window_slots, active[ active_count]);
   }

   if( options.shared_arena)
      stats->pages_peak = mulle_arena_get_page_count( &shared_arena);

   stats->seconds = mulle_relativetime_now() - stats->seconds;
   stats->samples = (unsigned long long) stream->count;
   return( 0);
}


/*
 * ------------------------------------------------------------------
 * the summary
 * ------------------------------------------------------------------
 */

static void   print_run( FILE *fp, const char *label, struct runstats *stats,
                         int backend)
{
   double   ms;
   double   per_second;

   ms         = stats->seconds * 1000.0;
   per_second = stats->seconds > 0.0 ? (double) stats->created / stats->seconds : 0.0;

   mulle_fprintf( fp, "\n=== Summary (%s) ===\n", label);
   mulle_fprintf( fp, "Samples processed : %llu\n", stats->samples);
   mulle_fprintf( fp, "Windows created   : %llu\n", stats->created);
   mulle_fprintf( fp, "Windows destroyed : %llu\n", stats->destroyed);
   mulle_fprintf( fp, "Reports emitted   : %llu\n", stats->reports);
   mulle_fprintf( fp, "Outliers total    : %llu\n", stats->outliers);
   mulle_fprintf( fp, "Wall-clock time   : ");
   print_hundredths( fp, ms);
   mulle_fprintf( fp, " ms  (");
   print_hundredths( fp, per_second);
   mulle_fprintf( fp, " windows/s)\n");
   mulle_fprintf( fp, "Avg window life   : %lu samples\n", options.window_size);
   mulle_fprintf( fp, "Peak concurrent   : %llu of %lu window slots\n",
                  stats->slots_peak, options.max_active);
   if( options.window_size / options.start_every < options.max_active)
      mulle_fprintf( fp, "                    (window-size / start-every is %lu, so\n"
                         "                    --max-active %lu is not the binding limit)\n",
                     options.window_size / options.start_every,
                     options.max_active);

   if( backend == backend_tree)
   {
      mulle_fprintf( fp, "Tree node pool    : %llu nodes handed out,"
                         " %lu per tree at its high-water (buckets of %d)\n",
                     stats->nodes, options.window_size, RBTREE_STORAGE_CAPACITY);
      if( options.shared_arena)
         mulle_fprintf( fp, "Arenas            : 1 shared by every window, %llu pages"
                            " held -- a shared arena never gives any back\n",
                        stats->pages_peak);
      else
         mulle_fprintf( fp, "Arenas            : %llu created and released,"
                            " peak %llu page(s) each, %llu pages in total\n",
                        stats->destroyed, stats->pages_peak, stats->pages);
   }
   else
      mulle_fprintf( fp, "Window values     : %lu doubles per window in the arena,"
                         " sorted with mulle_qsort_r at the query\n",
                     options.window_size);

   if( stats->errors)
      mulle_fprintf( fp, "Errors            : %llu\n", stats->errors);
}


static void   print_tail( FILE *fp)
{
   unsigned long   i;
   unsigned long   first;
   unsigned long   slot;

   if( ! options.tail)
      return;

   mulle_fprintf( fp, "\nLast report(s):\n");
   first = tail_next > (unsigned long) options.tail
           ? tail_next - (unsigned long) options.tail
           : 0;
   for( i = first; i < tail_next; i++)
   {
      struct record   *report;
      char             text[ 40];

      slot   = i % (unsigned long) options.tail;
      report = &tail_reports[ slot];
      format_hundredths( text, sizeof( text), report->median);
      mulle_fprintf( fp, "[window %lu finished @ %lu] median=%s  outliers=%lu\n",
                     report->id, report->closed_at, text, report->outliers);
   }
}


/*
 * ------------------------------------------------------------------
 * storage plumbing
 * ------------------------------------------------------------------
 */

static void   storage_setup( void)
{
   unsigned int   i;
   unsigned int   slot;

   _mulle_indexedstorage_init( &window_slots, sizeof( struct window),
                               alignof( struct window),
                               (unsigned int) options.max_active, NULL);

   /* prime the pool to its maximum, so the struct array never grows again */
   for( i = 0; i < (unsigned int) options.max_active; i++)
   {
      slot = _mulle_indexedstorage_alloc( &window_slots);
      _mulle_indexedstorage_free( &window_slots, slot);
   }

   if( options.shared_arena)
      mulle_arena_init( &shared_arena, 0, NULL);
}


static void   storage_teardown( void)
{
   if( options.shared_arena)
      mulle_arena_done( &shared_arena);
   _mulle_indexedstorage_done( &window_slots);
   log_done( &tree_log);
   log_done( &array_log);
}


static int   verify_backends( void)
{
   size_t          i;
   struct record  *a;
   struct record  *b;

   if( tree_log.count != array_log.count)
   {
      mulle_fprintf( stderr, "verify: %lu reports against %lu\n",
                     (unsigned long) tree_log.count,
                     (unsigned long) array_log.count);
      return( -1);
   }

   for( i = 0; i < tree_log.count; i++)
   {
      a = &tree_log.records[ i];
      b = &array_log.records[ i];
      if( a->id != b->id ||
          a->closed_at != b->closed_at ||
          a->outliers != b->outliers ||
          memcmp( &a->median, &b->median, sizeof( double)))
      {
         char             left[ 40];
         char             right[ 40];
         unsigned long    left_bits;
         unsigned long    right_bits;

         format_hundredths( left, sizeof( left), a->median);
         format_hundredths( right, sizeof( right), b->median);
         memcpy( &left_bits, &a->median, sizeof( left_bits));
         memcpy( &right_bits, &b->median, sizeof( right_bits));
         mulle_fprintf( stderr,
                        "verify: report %lu differs: window %lu (closed @ %lu)"
                        " median %s (%lx) outliers %lu  vs  window %lu (closed @ %lu)"
                        " median %s (%lx) outliers %lu\n",
                        (unsigned long) i,
                        a->id, a->closed_at, left, left_bits, a->outliers,
                        b->id, b->closed_at, right, right_bits, b->outliers);
         return( -1);
      }
   }
   return( 0);
}


int   main( int argc, char *argv[])
{
   struct stream   stream = { 0 };
   FILE           *fp;
   int             rval;

   if( parse_arguments( argc, argv))
      return( 1);

   rval = 0;
   if( options.use_stdin || options.path)
   {
      fp = options.use_stdin ? stdin : fopen( options.path, "rb");
      if( ! fp)
      {
         mulle_fprintf( stderr, "%s: %s: %s\n", argv[ 0],
                        options.path, strerror( errno));
         return( 1);
      }
      if( stream_read_file( &stream, fp))
      {
         mulle_fprintf( stderr, "%s: could not read samples from %s\n",
                        argv[ 0], options.use_stdin ? "stdin" : options.path);
         rval = 1;
      }
      if( fp != stdin)
         fclose( fp);
   }
   else
      if( stream_from_gaussian( &stream, options.stream_len))
         rval = 1;

   if( rval)
   {
      stream_done( &stream);
      return( 1);
   }

   if( options.shared_arena && (options.baseline || options.verify))
   {
      mulle_fprintf( stderr, "%s: --shared-arena is a one-arena experiment;"
                             " drop --baseline/--verify\n", argv[ 0]);
      stream_done( &stream);
      return( 1);
   }

   storage_setup();

   if( run_stream( &stream, backend_tree, &tree_stats, &tree_log))
   {
      mulle_fprintf( stderr, "%s: out of memory\n", argv[ 0]);
      storage_teardown();
      stream_done( &stream);
      return( 1);
   }

   if( options.baseline || options.verify)
   {
      if( run_stream( &stream, backend_array, &array_stats, &array_log))
      {
         mulle_fprintf( stderr, "%s: out of memory\n", argv[ 0]);
         storage_teardown();
         stream_done( &stream);
         return( 1);
      }
   }

   print_run( stdout, "mulle-rbtree per window", &tree_stats, backend_tree);
   if( options.baseline || options.verify)
      print_run( stdout, "array + mulle_qsort_r per window", &array_stats,
                 backend_array);

   if( options.verify)
   {
      if( verify_backends())
      {
         mulle_fprintf( stdout, "\nverify: MISMATCH\n");
         print_tail( stdout);
         storage_teardown();
         stream_done( &stream);
         return( 1);
      }
      mulle_fprintf( stdout, "\nverify: both backends agree on %lu reports"
                             " (%llu outliers)\n",
                     (unsigned long) tree_log.count, tree_stats.outliers);
   }

   print_tail( stdout);

   storage_teardown();
   stream_done( &stream);
   return( 0);
}
