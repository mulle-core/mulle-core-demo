/*
 * logtop — a live dashboard for a stream of log lines, built on mulle-core.
 *
 *   logtop -f /var/log/nginx/access.log -k 7 -V 9
 *   tail -f app.log | logtop -k 3
 *   logtop --report -k 7 -V 9 access.log     # one deterministic summary
 *
 * Threading model:
 *
 *   readers (one per source)  take a line buffer from the spare fifo, read a
 *                             line into it, push it to the content fifo
 *   parsers (N)               pop a buffer, pick the key field and the numeric
 *                             field, bump the shared counters, hand the buffer
 *                             back to the spare fifo
 *   main                      redraws from snapshots and never blocks a writer
 *
 * The writers only ever increment atomics; the renderer only ever reads them.
 * There are no locks, and no allocation after startup: the line buffers are a
 * fixed pool that circulates through the two fifos, so the pool is what bounds
 * memory, and a full fifo is the backpressure that throttles the readers.
 */

#include <mulle-core/mulle-core.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined( _WIN32)
# include <io.h>
# define logtop_isatty(fd)   _isatty( fd)
# define logtop_stdout_fd    1
#else
# include <unistd.h>
# define logtop_isatty(fd)   isatty( fd)
# define logtop_stdout_fd    STDOUT_FILENO
#endif


/*
 * ------------------------------------------------------------------
 * tuning
 * ------------------------------------------------------------------
 */

#define POOL_SIZE        512            /* line buffers in circulation */
#define LINE_CAPACITY    4096           /* per line; the tail is dropped */
#define CONTENT_FIFO     512
#define SPARE_FIFO       512
#define MAX_JOBS         32
#define DEFAULT_JOBS     2
#define MAX_TOP          32
#define DEFAULT_TOP      8
#define MAX_SOURCES      MAX_JOBS
#define HGRAM_BUCKETS    12
#define DEFAULT_BUCKET   10             /* milliseconds */
#define SPARK_LEN        60
#define SNAPSHOT_MAX     8192           /* keys collected per redraw */
#define ENUMERATE_TRIES  8              /* ECANCELED restarts */


/*
 * ------------------------------------------------------------------
 * options
 * ------------------------------------------------------------------
 */

static struct
{
   int      follow;          /* keep reading after end of input */
   long     lines;           /* stop after N lines, 0 = no limit */
   int      jobs;
   int      key_field;       /* 1 based; 0 = the whole line */
   int      value_field;     /* 1 based; 0 = no histogram */
   int      top;
   double   interval;        /* seconds between redraws */
   double   window;          /* seconds of throughput history */
   long     bucket_ms;
   int      tui;
   int      tui_explicit;
   int      report;          /* process everything, print one summary, exit */
} options;

/*
 * volatile sig_atomic_t, not an atomic, so the handler can set it from a
 * signal context.
 */
static volatile sig_atomic_t   stop_requested;


/*
 * ------------------------------------------------------------------
 * atomics
 *
 * mulle-core has no atomic integer type; the intended idiom is to hold a
 * non-pointer intptr_t inside a mulle_atomic_pointer_t. These wrappers keep the
 * casts in one place. They are deliberately not called atomic_load/store:
 * mulle-core.h includes <stdatomic.h>, whose generic macros own those names.
 * ------------------------------------------------------------------
 */

static inline intptr_t   mg_load( mulle_atomic_pointer_t *p)
{
   return( (intptr_t) _mulle_atomic_pointer_read( p));
}

static inline void   mg_store( mulle_atomic_pointer_t *p, intptr_t value)
{
   _mulle_atomic_pointer_write( p, (void *) value);
}

static inline intptr_t   mg_increment( mulle_atomic_pointer_t *p)
{
   return( (intptr_t) _mulle_atomic_pointer_increment( p));
}

static inline intptr_t   mg_decrement( mulle_atomic_pointer_t *p)
{
   return( (intptr_t) _mulle_atomic_pointer_decrement( p));
}

/* counters that gate no memory access: statistics only */
static inline void   mg_increment_relaxed( mulle_atomic_pointer_t *p)
{
   (void) _mulle_atomic_pointer_increment_relaxed( p);
}

/*
 * mulle_concurrent_hashtable needs a hash in [1, INTPTR_MAX]: 0 is NO_HASH and
 * the topmost bit is the FROZEN migration flag. mulle_hash_64() promises
 * neither, so fold it. Change this if the table moves to "even" mode.
 */
static inline intptr_t   hashtable_hash_from_bytes( const void *bytes, size_t length)
{
   uint64_t   h;

   h  = mulle_hash_64( bytes, length);
   h &= (uint64_t) INTPTR_MAX;

   return( (intptr_t) (h ? h : 1));
}


/*
 * ------------------------------------------------------------------
 * shared state
 * ------------------------------------------------------------------
 */

struct sample
{
   size_t   len;
   char     text[ LINE_CAPACITY];
};

struct tally
{
   char                    *text;
   size_t                   len;
   mulle_atomic_pointer_t   count;
};

static struct mulle_concurrent_hashtable   tallies;       /* key hash -> struct tally * */

static struct mulle_pointermultififo       content_fifo;  /* readers -> parsers */
static struct mulle_pointermultififo       spare_fifo;    /* parsers -> readers */

static mulle_atomic_pointer_t   total_lines;
static mulle_atomic_pointer_t   line_sequence;      /* claims line numbers, for -n */
static mulle_atomic_pointer_t   dropped_lines;
static mulle_atomic_pointer_t   readers_active;
static mulle_atomic_pointer_t   parsers_active;
static mulle_atomic_pointer_t   histogram_values;

static mulle_atomic_pointer_t   histogram[ HGRAM_BUCKETS];

static struct sample           *pool;


/*
 * ------------------------------------------------------------------
 * the line buffer pool
 * ------------------------------------------------------------------
 */

static void   pool_init( void)
{
   unsigned int   i;

   pool = (struct sample *) mulle_allocator_malloc( &mulle_allocator_default,
                                                    POOL_SIZE * sizeof( struct sample));
   for( i = 0; i < POOL_SIZE; i++)
      if( _mulle_pointermultififo_write( &spare_fifo, &pool[ i]) == -1)
         abort();                  /* the ring and the pool have to agree */
}


/* NULL when every buffer is currently held by a reader or a parser */
static struct sample   *sample_acquire( void)
{
   return( (struct sample *) _mulle_pointermultififo_read_barrier( &spare_fifo));
}


static void   sample_release( struct sample *s)
{
   if( s)
      (void) _mulle_pointermultififo_write( &spare_fifo, s);
}


/*
 * ------------------------------------------------------------------
 * tallies: one lock-free counter per distinct key
 * ------------------------------------------------------------------
 */

static struct tally   *tally_intern( const char *text, size_t len)
{
   struct tally   *found;
   struct tally   *fresh;
   struct tally   *existing;
   void           *old;
   intptr_t        hash;
   int             rc;

   hash  = hashtable_hash_from_bytes( text, len);
   found = (struct tally *) mulle_concurrent_hashtable_lookup( &tallies, hash);
   if( found && found->len == len && ! memcmp( found->text, text, len))
      return( found);

   fresh             = mulle_allocator_calloc( &mulle_allocator_default, 1, sizeof( struct tally));
   fresh->text       = mulle_allocator_malloc( &mulle_allocator_default, len + 1);
   fresh->len        = len;
   memcpy( fresh->text, text, len);
   fresh->text[ len] = 0;

   old = NULL;
   rc  = mulle_concurrent_hashtable_register( &tallies, hash, fresh, &old);
   if( ! rc && ! old)
      return( fresh);

   existing = (struct tally *) old;
   if( existing && existing->len == len && ! memcmp( existing->text, text, len))
   {
      mulle_allocator_free( &mulle_allocator_default, fresh->text);
      mulle_allocator_free( &mulle_allocator_default, fresh);
      return( existing);
   }

   /*
    * A real 64 bit hash collision. Refuse to charge this line to another key;
    * it goes uncounted rather than miscounted.
    */
   mulle_allocator_free( &mulle_allocator_default, fresh->text);
   mulle_allocator_free( &mulle_allocator_default, fresh);
   return( NULL);
}


/*
 * ------------------------------------------------------------------
 * parsing
 * ------------------------------------------------------------------
 */

/* fields are runs of non-whitespace, 1 based; field 0 is the whole line */
static int   field_at( const char *text,
                      size_t len,
                      int n,
                      const char **p_field,
                      size_t *p_len)
{
   size_t   i;
   int      field;

   if( ! n)
   {
      *p_field = text;
      *p_len   = len;
      return( 1);
   }

   i     = 0;
   field = 0;

   while( i < len)
   {
      size_t   start;

      while( i < len && ( text[ i] == ' ' || text[ i] == '\t'))
         i++;
      if( i >= len)
         break;

      start = i;
      while( i < len && text[ i] != ' ' && text[ i] != '\t')
         i++;

      if( ++field == n)
      {
         *p_field = text + start;
         *p_len   = i - start;
         return( 1);
      }
   }

   return( 0);
}


static void   histogram_add( double value)
{
   long   index;

   if( value < 0.0)
      value = 0.0;

   index = (long) ( value / (double) options.bucket_ms);
   if( index >= HGRAM_BUCKETS - 1)
      index = HGRAM_BUCKETS - 1;

   mg_increment_relaxed( &histogram[ index]);
   mg_increment( &histogram_values);
}


static void   parse_line( struct sample *s)
{
   const char   *key;
   const char   *value;
   size_t        key_len;
   size_t        value_len;
   struct tally *t;

   mg_increment( &total_lines);

   if( field_at( s->text, s->len, options.key_field, &key, &key_len))
   {
      t = tally_intern( key, key_len);
      if( t)
         mg_increment_relaxed( &t->count);
   }

   if( options.value_field &&
       field_at( s->text, s->len, options.value_field, &value, &value_len))
   {
      char     *end;
      char      scratch[ 64];
      size_t    n;
      double    d;

      /* mulle_strtod is the exact inverse of mulle_dtostr */
      n = value_len < sizeof( scratch) - 1 ? value_len : sizeof( scratch) - 1;
      memcpy( scratch, value, n);
      scratch[ n] = 0;

      end = NULL;
      d   = mulle_strtod( scratch, &end);
      if( end && end != scratch)
         histogram_add( d);
   }
}


/*
 * ------------------------------------------------------------------
 * threads
 * ------------------------------------------------------------------
 */

static int   should_stop( void)
{
   return( stop_requested);
}


static mulle_thread_rval_t   MULLE_THREAD_CALL   parser_main( void *arg)
{
   (void) arg;

   mulle_aba_register();

   for(;;)
   {
      struct sample   *s;

      s = (struct sample *) _mulle_pointermultififo_read_barrier( &content_fifo);
      if( s)
      {
         parse_line( s);
         sample_release( s);
         continue;
      }

      /* nothing buffered, and the readers are finished: that is the end */
      if( ! mg_load( &readers_active))
         break;

      mulle_thread_yield();
   }

   mulle_aba_unregister();
   (void) mg_decrement( &parsers_active);

   return( (mulle_thread_rval_t) 0);
}


/*
 * Reads one line, storing at most LINE_CAPACITY bytes of it. Returns 0 at end of
 * input, 1 when `s` holds a non-blank line (blank lines are skipped).
 */
static int   read_line( FILE *fp, struct sample *s)
{
   for(;;)
   {
      int      c;
      size_t   len;
      int      overlong;

      len      = 0;
      overlong = 0;
      c        = EOF;

      while( (c = fgetc( fp)) != EOF && c != '\n')
      {
         if( len + 1 < LINE_CAPACITY)
            s->text[ len++] = (char) c;
         else
            overlong = 1;
      }

      if( overlong)
         mg_increment_relaxed( &dropped_lines);

      if( len)
      {
         s->text[ len] = 0;
         s->len        = len;
         return( 1);
      }

      if( c == EOF)
         return( 0);
   }
}


static mulle_thread_rval_t   MULLE_THREAD_CALL   reader_main( void *arg)
{
   FILE         *fp;
   const char   *path;
   int           is_stdin;

   mulle_aba_register();

   path     = (const char *) arg;
   is_stdin = ! strcmp( path, "-");

   fp = is_stdin ? stdin : fopen( path, "r");
   if( ! fp)
   {
      mulle_fprintf( stderr, "logtop: %s: %s\n", path, strerror( errno));
      mulle_aba_unregister();
      (void) mg_decrement( &readers_active);
      return( (mulle_thread_rval_t) 0);
   }

   while( ! should_stop())
   {
      struct sample   *s;

      s = sample_acquire();
      if( ! s)
      {
         /*
          * Every buffer is out. This is the pool acting as the memory bound:
          * wait for a parser to hand one back.
          */
         mulle_thread_yield();
         continue;
      }

      if( ! read_line( fp, s))
      {
         sample_release( s);

         if( ! options.follow)
            break;

         /*
          * Follow mode: EOF means "nothing yet", not the end. clearerr()
          * clears the sticky end-of-file so a file that grew in the meantime
          * is seen on the next attempt. Note that nothing is consumed on such
          * a poll: the line number is claimed below, after a real line.
          */
         clearerr( fp);
         mulle_relativetime_sleep( 0.1);
         continue;
      }

      /*
       * Line numbers are claimed here rather than counted in the parser, so
       * that -n reads exactly N lines and then stops the source, instead of
       * reading to the end and dropping the rest.
       */
      if( options.lines > 0 && mg_increment( &line_sequence) >= options.lines)
      {
         sample_release( s);
         break;
      }

      while( _mulle_pointermultififo_write( &content_fifo, s) == -1)
      {
         if( should_stop())
            break;
         mulle_thread_yield();        /* backpressure, not a failure */
      }
   }

   if( ! is_stdin)
      fclose( fp);

   mulle_aba_unregister();
   (void) mg_decrement( &readers_active);

   return( (mulle_thread_rval_t) 0);
}


/*
 * ------------------------------------------------------------------
 * snapshots and ordering
 * ------------------------------------------------------------------
 */

struct entry
{
   struct tally   *tally;
   intptr_t        count;
};

static int   key_compare( struct tally *a, struct tally *b)
{
   size_t   n;

   n = a->len < b->len ? a->len : b->len;

   if( n)
   {
      int   rc;

      rc = memcmp( a->text, b->text, n);
      if( rc)
         return( rc);
   }

   if( a->len != b->len)
      return( a->len < b->len ? -1 : 1);

   return( 0);
}


static int   entry_precedes( struct entry *a, struct entry *b)
{
   if( a->count != b->count)
      return( a->count > b->count);

   return( key_compare( a->tally, b->tally) < 0);
}


/*
 * The concurrent hashtable enumerator reports ECANCELED when its storage
 * migrated underneath it, which is expected here because parsers keep adding
 * keys while the renderer reads. Restart, and if it will not settle, report a
 * partial view rather than a wrong one.
 */
static unsigned int   snapshot( struct entry *entries, unsigned int capacity)
{
   unsigned int   attempt;

   for( attempt = 0; attempt < ENUMERATE_TRIES; attempt++)
   {
      struct mulle_concurrent_hashtableenumerator   rover;
      unsigned int                                  n;
      int                                           rval;
      intptr_t                                      hash;
      void                                         *value;

      n     = 0;
      rover = mulle_concurrent_hashtable_enumerate( &tallies);
      rval  = 1;

      while( rval == 1 && n < capacity)
      {
         rval = mulle_concurrent_hashtableenumerator_next( &rover, &hash, &value);
         if( rval == 1)
         {
            struct tally   *t;

            t                 = (struct tally *) value;
            entries[ n].tally = t;
            entries[ n].count = mg_load( &t->count);
            n++;
         }
      }

      mulle_concurrent_hashtableenumerator_done( &rover);

      if( rval != ECANCELED)
         return( n);
   }

   return( 0);
}


/*
 * Top list of fixed size: the renderer wants the best few of possibly very
 * many keys, so this avoids sorting the whole snapshot.
 */
static unsigned int   top_entries( struct entry   *entries,
                                  unsigned int    n,
                                  struct entry   *top,
                                  unsigned int    top_capacity)
{
   unsigned int   i;
   unsigned int   used;

   used = 0;

   for( i = 0; i < n; i++)
   {
      unsigned int   j;

      if( used < top_capacity)
         used++;
      else if( ! entry_precedes( &entries[ i], &top[ used - 1]))
         continue;

      j = used - 1;
      while( j > 0 && entry_precedes( &entries[ i], &top[ j - 1]))
      {
         top[ j] = top[ j - 1];
         j--;
      }
      top[ j] = entries[ i];
   }

   return( used);
}


/*
 * ------------------------------------------------------------------
 * rendering
 *
 * No %f anywhere: mulle-sprintf's summary documents no conversion table, so it
 * is not clear whether float conversions belong to it, and mulle_dtostr would
 * print the shortest round-trippable form, which is the last thing a dashboard
 * wants. Fractions are formatted as integer tenths instead.
 * ------------------------------------------------------------------
 */

static void   print_tenths( FILE *fp, double value, int width)
{
   char   buffer[ 32];
   long   tenths;
   long   len;

   tenths = (long) ( value * 10.0 + 0.5);
   mulle_sprintf( buffer, "%ld.%ld", tenths / 10, tenths % 10);

   len = (long) strlen( buffer);
   while( len++ < width)
      fputc( ' ', fp);

   mulle_fprintf( fp, "%s", buffer);
}


static void   print_percent( FILE *fp, intptr_t part, intptr_t total)
{
   if( ! total)
      mulle_fprintf( fp, "  0.0%%");
   else
   {
      print_tenths( fp, 100.0 * (double) part / (double) total, 4);
      mulle_fprintf( fp, "%%");
   }
}


/* width 0 prints the whole key; otherwise pad and truncate to width */
static void   print_key( FILE *fp, struct tally *t, size_t width)
{
   size_t   i;
   size_t   n;

   n = width ? width : t->len;

   for( i = 0; i < n; i++)
      fputc( i < t->len ? t->text[ i] : ' ', fp);
}


/* pads to width; the label buffer is only written up to its NUL, so stop there */
static void   print_label( FILE *fp, const char *label, size_t width)
{
   size_t   i;
   size_t   len;

   len = strlen( label);

   for( i = 0; i < width; i++)
      fputc( i < len ? label[ i] : ' ', fp);
}


static const char   *histogram_label( long index, char *scratch)
{
   long   low;

   low = index * options.bucket_ms;
   if( index == HGRAM_BUCKETS - 1)
      mulle_sprintf( scratch, ">=%ldms", low);
   else
      mulle_sprintf( scratch, "%ld-%ldms", low, low + options.bucket_ms);

   return( scratch);
}


/*
 * One column per sample. Block elements U+2581..U+2588 are three bytes each in
 * UTF-8, hence the 3.
 */
static const char   *spark_line( const double *rates, unsigned int count)
{
   static const char   *blocks[ 8] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
   static char          line[ SPARK_LEN * 3 + 1];
   double               peak;
   unsigned int         i;
   size_t               at;

   peak = 0.0;
   for( i = 0; i < count; i++)
      if( rates[ i] > peak)
         peak = rates[ i];

   at = 0;
   for( i = 0; i < count; i++)
   {
      unsigned int   level;

      if( peak <= 0.0)
         level = 0;
      else
      {
         level = (unsigned int) ( rates[ i] / peak * 7.0 + 0.5);
         if( level > 7)
            level = 7;
      }

      memcpy( line + at, blocks[ level], 3);
      at += 3;
   }
   line[ at] = 0;

   return( line);
}


static void   render_keys( FILE *fp, intptr_t total)
{
   struct entry   entries[ SNAPSHOT_MAX];
   struct entry   top[ MAX_TOP];
   unsigned int   top_used;
   unsigned int   n;
   unsigned int   i;

   n        = snapshot( entries, SNAPSHOT_MAX);
   top_used = top_entries( entries, n, top, (unsigned int) options.top);

   mulle_fprintf( fp, "keys (field %d, top %ld of %ld)\n",
                  options.key_field,
                  (long) top_used,
                  (long) n);

   for( i = 0; i < top_used; i++)
   {
      print_percent( fp, top[ i].count, total);
      mulle_fprintf( fp, "  %8ld  ", (long) top[ i].count);
      print_key( fp, top[ i].tally, 0);
      mulle_fprintf( fp, "\n");
   }
}


static void   render_histogram( FILE *fp)
{
   intptr_t   values;
   unsigned int   i;

   if( ! options.value_field)
      return;

   values = mg_load( &histogram_values);

   mulle_fprintf( fp, "histogram (field %d, %ld ms buckets, %ld values)\n",
                  options.value_field,
                  options.bucket_ms,
                  (long) values);

   for( i = 0; i < HGRAM_BUCKETS; i++)
   {
      char            label[ 32] = { 0 };
      intptr_t        count;
      unsigned long   bars;

      count = mg_load( &histogram[ i]);
      if( ! count)
         continue;                        /* empty buckets are noise */

      print_label( fp, histogram_label( i, label), 12);
      mulle_fprintf( fp, " %6ld  ", (long) count);
      print_percent( fp, count, values);

      bars = values ? (unsigned long) ( count * 20 / values) : 0;
      if( bars)
      {
         mulle_fprintf( fp, "  ");
         while( bars--)
            mulle_fprintf( fp, "#");
      }

      mulle_fprintf( fp, "\n");
   }
}


/* one summary, no timing in it: this is the mode to test and to pipe */
static void   render_report( void)
{
   intptr_t   total;

   if( options.tui)
      mulle_fprintf( stdout, "\033[H\033[J");

   total = mg_load( &total_lines);

   mulle_fprintf( stdout, "logtop report: %ld lines, %ld dropped\n",
                  (long) total,
                  (long) mg_load( &dropped_lines));

   render_keys( stdout, total);
   render_histogram( stdout);
   fflush( stdout);
}


static void   render_dashboard( const double *rates, unsigned int count)
{
   double         peak;
   unsigned int   i;

   if( options.tui)
      mulle_fprintf( stdout, "\033[H\033[J");

   peak = 0.0;
   for( i = 0; i < count; i++)
      if( rates[ i] > peak)
         peak = rates[ i];

   mulle_fprintf( stdout,
                  "logtop  %ld lines  %ld dropped  %ld s\n\n",
                  (long) mg_load( &total_lines),
                  (long) mg_load( &dropped_lines),
                  (long) mulle_relativetime_now());

   mulle_fprintf( stdout, "throughput (peak %ld lines/s, window %ld s)\n",
                  (long) ( peak + 0.5),
                  (long) options.window);
   mulle_fprintf( stdout, "%s\n\n", spark_line( rates, count));

   render_keys( stdout, mg_load( &total_lines));
   mulle_fprintf( stdout, "\n");
   render_histogram( stdout);
   fflush( stdout);
}


/*
 * ------------------------------------------------------------------
 * teardown
 * ------------------------------------------------------------------
 */

static void   release_tallies( void)
{
   struct mulle_concurrent_hashtableenumerator   rover;
   intptr_t                                      hash;
   void                                         *value;

   rover = mulle_concurrent_hashtable_enumerate( &tallies);
   while( mulle_concurrent_hashtableenumerator_next( &rover, &hash, &value) == 1)
   {
      struct tally   *t;

      t = (struct tally *) value;
      mulle_allocator_free( &mulle_allocator_default, t->text);
      mulle_allocator_free( &mulle_allocator_default, t);
   }
   mulle_concurrent_hashtableenumerator_done( &rover);
   mulle_concurrent_hashtable_done( &tallies);
}


static void   on_signal( int sig)
{
   (void) sig;
   stop_requested = 1;
}


/*
 * ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------
 */

static void   usage( FILE *fp, const char *name)
{
   mulle_fprintf( fp,
      "usage: %s [options] [FILE ...]\n"
      "\n"
      "Watches log lines and shows what they contain. With no FILE, reads\n"
      "stdin. Fields are whitespace separated and 1 based; 0 is the whole\n"
      "line.\n"
      "\n"
      "  -f, --follow          keep reading after end of input\n"
      "  -n, --lines N         stop after N lines\n"
      "  -j, --jobs N          parser threads (default %d)\n"
      "  -k, --key N           field to tally (default 1)\n"
      "  -V, --value N         numeric field for the histogram\n"
      "  -t, --top N           how many keys to show (default %d)\n"
      "  -i, --interval SEC    redraw interval (default 1.0)\n"
      "  -w, --window SEC      throughput history (default 60)\n"
      "      --bucket-ms MS    histogram bucket width (default %d)\n"
      "      --report          process everything, print one summary, exit\n"
      "      --tui             force ANSI redraw\n"
      "      --no-tui          force plain output\n"
      "  -h, --help            this text\n"
      "\n"
      "Examples:\n"
      "  %s --report -k 7 -V 9 access.log\n"
      "  tail -f app.log | %s -k 3\n",
      name, DEFAULT_JOBS, DEFAULT_TOP, DEFAULT_BUCKET, name, name);
}


/* returns the first FILE argument index, -1 for stdin, or -2 on error */
static int   parse_arguments( int argc, char *argv[])
{
   int   i;

   options.jobs      = DEFAULT_JOBS;
   options.top       = DEFAULT_TOP;
   options.key_field = 1;
   options.interval  = 1.0;
   options.window    = 60.0;
   options.bucket_ms = DEFAULT_BUCKET;

   for( i = 1; i < argc; i++)
   {
      const char   *arg;

      arg = argv[ i];

      if( arg[ 0] != '-' || ! arg[ 1])
         break;

      if( ! strcmp( arg, "-h") || ! strcmp( arg, "--help"))
      {
         usage( stdout, argv[ 0]);
         exit( 0);
      }
      else if( ! strcmp( arg, "-f") || ! strcmp( arg, "--follow"))
         options.follow = 1;
      else if( ! strcmp( arg, "--report"))
         options.report = 1;
      else if( ! strcmp( arg, "--tui"))
      {
         options.tui          = 1;
         options.tui_explicit = 1;
      }
      else if( ! strcmp( arg, "--no-tui"))
      {
         options.tui          = 0;
         options.tui_explicit = 1;
      }
      else if( ! strcmp( arg, "-n") || ! strcmp( arg, "--lines"))
      {
         if( ++i >= argc)
            goto missing;
         options.lines = atol( argv[ i]);
      }
      else if( ! strcmp( arg, "-j") || ! strcmp( arg, "--jobs"))
      {
         if( ++i >= argc)
            goto missing;
         options.jobs = atoi( argv[ i]);
      }
      else if( ! strcmp( arg, "-k") || ! strcmp( arg, "--key"))
      {
         if( ++i >= argc)
            goto missing;
         options.key_field = atoi( argv[ i]);
      }
      else if( ! strcmp( arg, "-V") || ! strcmp( arg, "--value"))
      {
         if( ++i >= argc)
            goto missing;
         options.value_field = atoi( argv[ i]);
      }
      else if( ! strcmp( arg, "-t") || ! strcmp( arg, "--top"))
      {
         if( ++i >= argc)
            goto missing;
         options.top = atoi( argv[ i]);
      }
      else if( ! strcmp( arg, "-i") || ! strcmp( arg, "--interval"))
      {
         if( ++i >= argc)
            goto missing;
         options.interval = mulle_strtod( argv[ i], NULL);
      }
      else if( ! strcmp( arg, "-w") || ! strcmp( arg, "--window"))
      {
         if( ++i >= argc)
            goto missing;
         options.window = mulle_strtod( argv[ i], NULL);
      }
      else if( ! strcmp( arg, "--bucket-ms"))
      {
         if( ++i >= argc)
            goto missing;
         options.bucket_ms = atol( argv[ i]);
      }
      /* combined short options: -j4, -k7, -V9, -t3, -n5, -w30, -i0.5 */
      else if( arg[ 2] && strchr( "jkVtnwi", arg[ 1]))
      {
         const char   *value;

         value = arg + 2;
         switch( arg[ 1])
         {
         case 'j' : options.jobs        = atoi( value); break;
         case 'k' : options.key_field   = atoi( value); break;
         case 'V' : options.value_field = atoi( value); break;
         case 't' : options.top         = atoi( value); break;
         case 'n' : options.lines       = atol( value); break;
         case 'w' : options.window      = mulle_strtod( value, NULL); break;
         case 'i' : options.interval    = mulle_strtod( value, NULL); break;
         }
      }
      else
      {
         mulle_fprintf( stderr, "logtop: unknown option %s\n", arg);
         return( -2);
      }
   }

   if( options.jobs < 1)
      options.jobs = 1;
   if( options.jobs > MAX_JOBS)
      options.jobs = MAX_JOBS;
   if( options.top < 1)
      options.top = 1;
   if( options.top > MAX_TOP)
      options.top = MAX_TOP;
   if( options.key_field < 0)
      options.key_field = 0;
   if( options.value_field < 0)
      options.value_field = 0;
   if( options.bucket_ms < 1)
      options.bucket_ms = 1;
   if( options.interval <= 0.0)
      options.interval = 1.0;
   if( options.window < options.interval)
      options.window = options.interval;

   return( i < argc ? i : -1);

missing:
   mulle_fprintf( stderr, "logtop: %s needs a value\n", argv[ i - 1]);
   return( -2);
}


int   main( int argc, char *argv[])
{
   mulle_thread_t   readers[ MAX_SOURCES];
   mulle_thread_t   parsers[ MAX_JOBS];
   double           rates[ SPARK_LEN];
   unsigned int     rate_count;
   unsigned int     rate_next;
   unsigned int     spark_slots;
   char           **sources;
   char           **owned_sources;
   size_t           source_count;
   int              arg_index;
   size_t           i;
   int              readers_started;
   int              parsers_started;
   double           last_sample;
   intptr_t         last_total;
   int              rval;

   arg_index = parse_arguments( argc, argv);
   if( arg_index == -2)
      return( 2);

   if( ! options.tui_explicit)
      options.tui = logtop_isatty( logtop_stdout_fd);

   /* --report waits for the end of input, which --follow never reaches */
   if( options.report && options.follow)
   {
      mulle_fprintf( stderr, "logtop: --report reads to the end of input, so --follow cannot be used with it\n");
      return( 2);
   }

   signal( SIGINT, on_signal);
   signal( SIGTERM, on_signal);

   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_hashtable_init( &tallies, 512, NULL);
   _mulle_pointermultififo_init( &content_fifo, CONTENT_FIFO, NULL);
   _mulle_pointermultififo_init( &spare_fifo, SPARE_FIFO, NULL);

   mg_store( &total_lines, 0);
   mg_store( &line_sequence, 0);
   mg_store( &dropped_lines, 0);
   mg_store( &readers_active, 0);
   mg_store( &parsers_active, 0);
   mg_store( &histogram_values, 0);
   for( i = 0; i < HGRAM_BUCKETS; i++)
      mg_store( &histogram[ i], 0);

   pool_init();

   /* the sources: the files given, or stdin */
   owned_sources = NULL;
   if( arg_index < 0)
   {
      owned_sources = (char **) mulle_allocator_malloc( &mulle_allocator_default,
                                                        2 * sizeof( char *));
      owned_sources[ 0] = (char *) "-";
      owned_sources[ 1] = NULL;
      sources           = owned_sources;
   }
   else
      sources = &argv[ arg_index];

   source_count = 0;
   while( sources[ source_count])
      source_count++;
   if( source_count > MAX_SOURCES)
      source_count = MAX_SOURCES;

   readers_started = 0;
   for( i = 0; i < source_count; i++)
   {
      (void) mg_increment( &readers_active);
      if( mulle_thread_create( reader_main, (void *) sources[ i], &readers[ i]))
      {
         (void) mg_decrement( &readers_active);
         mulle_fprintf( stderr, "logtop: could not start a reader thread\n");
         break;
      }
      readers_started++;
   }

   if( ! readers_started)
   {
      mulle_fprintf( stderr, "logtop: no readers\n");
      return( 2);
   }

   parsers_started = 0;
   for( i = 0; i < (size_t) options.jobs; i++)
   {
      (void) mg_increment( &parsers_active);
      if( mulle_thread_create( parser_main, NULL, &parsers[ i]))
      {
         (void) mg_decrement( &parsers_active);
         mulle_fprintf( stderr, "logtop: could not start a parser thread\n");
         break;
      }
      parsers_started++;
   }

   if( ! parsers_started)
   {
      mulle_fprintf( stderr, "logtop: no parsers\n");
      return( 2);
   }

   if( options.report)
   {
      /* no redraws: wait for the readers, then print one deterministic summary */
      while( mg_load( &readers_active) > 0)
         mulle_thread_yield();

      for( i = 0; i < (size_t) parsers_started; i++)
         (void) mulle_thread_join( parsers[ i]);
      for( i = 0; i < (size_t) readers_started; i++)
         (void) mulle_thread_join( readers[ i]);

      render_report();
   }
   else
   {
      spark_slots = (unsigned int) options.window;
      if( spark_slots < 1)
         spark_slots = 1;
      if( spark_slots > SPARK_LEN)
         spark_slots = SPARK_LEN;

      last_sample = mulle_relativetime_now();
      last_total  = 0;
      rate_count  = 0;
      rate_next   = 0;

      for(;;)
      {
         intptr_t   total;
         double     now;
         double     elapsed;

         mulle_relativetime_sleep( options.interval);

         now     = mulle_relativetime_now();
         total   = mg_load( &total_lines);
         elapsed = now - last_sample;
         if( elapsed <= 0.0)
            elapsed = options.interval;

         rates[ rate_next] = (double) ( total - last_total) / elapsed;
         rate_next         = ( rate_next + 1) % spark_slots;
         if( rate_count < spark_slots)
            rate_count++;

         last_sample = now;
         last_total  = total;

         render_dashboard( rates, rate_count);

         if( stop_requested)
            break;

         /*
          * Input ended or --lines reached: the readers stop themselves, and
          * once the parsers have drained behind them there is nothing left to
          * show, so take the final picture and leave.
          */
         if( ! mg_load( &readers_active) && ! mg_load( &parsers_active))
            break;
      }

      for( i = 0; i < (size_t) parsers_started; i++)
         (void) mulle_thread_join( parsers[ i]);
      for( i = 0; i < (size_t) readers_started; i++)
         (void) mulle_thread_join( readers[ i]);

      render_report();
   }

   rval = mg_load( &total_lines) ? 0 : 1;

   release_tallies();

   _mulle_pointermultififo_done( &spare_fifo);
   _mulle_pointermultififo_done( &content_fifo);

   mulle_allocator_free( &mulle_allocator_default, pool);
   mulle_allocator_free( &mulle_allocator_default, owned_sources);

   mulle_aba_unregister();

   return( rval);
}
