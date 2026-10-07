/*
 * multigrep — a parallel, mmap'd text search built on mulle-core.
 *
 *   multigrep [options] PATTERN [FILE ...]
 *   find . -type f | multigrep -j8 PATTERN
 *
 * Threading model (three roles, no locks anywhere on the hot path):
 *
 *   producer thread   reads the file list, creates a file_info for each and
 *                     appends it to a grow-only mulle_concurrent_pointerarray
 *   worker threads    claim an index off an atomic counter, then wait until the
 *                     producer has published that entry; scan the file and
 *                     stream matches into a mulle_pointermultififo
 *   main thread       drains the fifo and prints, and recycles consumed results
 *                     through a second fifo so matches cost no allocation
 *
 * --unique keeps a mulle_concurrent_hashtable of distinct lines whose counters
 * are bumped with relaxed atomics from every worker at once.
 *
 * The file list comes from stdin (or argv) rather than from a directory walk
 * on purpose: mulle-core has no stat/dirent API (see the README), and this
 * keeps the program portable to Windows.
 */

/*
 * memmem() is a GNU extension on glibc, declared only under _GNU_SOURCE, and a
 * plain libc function on macOS and the BSDs. Windows has neither, and searches
 * with the prefilter instead. This has to come before any system header.
 */
#if ! defined( _WIN32)
# define _GNU_SOURCE  1
#endif

#include <mulle-core/mulle-core.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define MAX_JOBS           64
#define DEFAULT_JOBS       4
#define CONTENT_FIFO_SIZE  1024
#define SPARE_FIFO_SIZE    1024
#define INITIAL_FILES      256
#define INITIAL_DISTINCTS  1024


/*
 * ------------------------------------------------------------------
 * options
 * ------------------------------------------------------------------
 */

static struct
{
   const char   *pattern;
   size_t        pattern_len;     /* in bytes */

   int           icase;
   int           unicode_fold;    /* icase and the pattern is not pure ASCII */
   int32_t      *folded;          /* folded pattern codepoints */
   size_t        folded_len;

   int           jobs;
   int           unique;
   int           count_only;
   int           files_only;
   int           top;
   long          top_n;
   int           stats;
   int           characters;      /* columns in characters, not bytes */
   int           verbose;
} options;

/* stream matches, or only collect per-file statistics? */
static int   streaming_output;


/*
 * ------------------------------------------------------------------
 * shared state
 * ------------------------------------------------------------------
 */

struct file_info
{
   struct mulle_mmap          map;
   char                      *path;
   size_t                     index;     /* position in the input list */
   size_t                     size;
   mulle_atomic_pointer_t     hits;      /* an intptr_t held in a void * */
   int                        mapped;
   int                        failed;
};

struct distinct_line
{
   size_t                     len;
   char                      *text;
   mulle_atomic_pointer_t     count;     /* an intptr_t held in a void * */
};

struct result
{
   struct file_info          *file;
   struct distinct_line      *distinct;
   const char                *line;      /* points into the mapping */
   size_t                     line_len;
   size_t                     line_no;
   size_t                     column;
   size_t                     match_len;
   int                        first;     /* first sighting of a distinct line */
};

static struct mulle_concurrent_pointerarray   files;      /* struct file_info * */
static struct mulle_concurrent_hashtable      distincts;  /* line hash -> struct distinct_line * */

static struct mulle_pointermultififo          content_fifo;   /* worker -> main */
static struct mulle_pointermultififo          spare_fifo;     /* main -> worker */

static mulle_atomic_pointer_t   producer_done;
static mulle_atomic_pointer_t   active_workers;
static mulle_atomic_pointer_t   next_file;        /* work claim counter */
static mulle_atomic_pointer_t   total_matches;    /* statistics */
static mulle_atomic_pointer_t   scanned_files;    /* statistics */
static mulle_atomic_pointer_t   scanned_bytes;    /* statistics */


/*
 * ------------------------------------------------------------------
 * atomics
 *
 * mulle-core has no atomic integer type. The intended idiom is to hold a
 * non-pointer intptr_t inside a mulle_atomic_pointer_t, so these wrappers
 * exist to make the casts appear exactly once each and to document which
 * ordering each use needs.
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

/* returns the previous value */
static inline intptr_t   mg_increment( mulle_atomic_pointer_t *p)
{
   return( (intptr_t) _mulle_atomic_pointer_increment( p));
}

static inline intptr_t   mg_decrement( mulle_atomic_pointer_t *p)
{
   return( (intptr_t) _mulle_atomic_pointer_decrement( p));
}

/* for counters that gate no memory access: statistics only */
static inline void   mg_add_relaxed( mulle_atomic_pointer_t *p, intptr_t diff)
{
   (void) _mulle_atomic_pointer_add_relaxed( p, diff);
}

static inline intptr_t   mg_increment_relaxed( mulle_atomic_pointer_t *p)
{
   return( (intptr_t) _mulle_atomic_pointer_increment_relaxed( p));
}


/*
 * mulle_concurrent_hashtable wants a hash in [1, INTPTR_MAX]: 0 is the
 * "unclaimed" sentinel and the topmost bit is the FROZEN migration flag.
 * mulle_hash_64() promises neither, so fold it here. This has to move if the
 * table is ever initialised in "even" mode instead of "positive".
 */
static inline intptr_t   hashtable_hash_from_bytes( const void *bytes, size_t length)
{
   uint64_t   h;

   h  = mulle_hash_64( bytes, length);
   h &= (uint64_t) INTPTR_MAX;       /* clear the FROZEN bit */

   return( (intptr_t) (h ? h : 1));  /* 0 is NO_HASH */
}


/*
 * ------------------------------------------------------------------
 * result recycling
 *
 * A match costs no allocation in the steady state: consumed results go back to
 * the workers over a second fifo. The two rings are the backpressure
 * mechanism for the whole program.
 * ------------------------------------------------------------------
 */

static struct result   *result_alloc( void)
{
   struct result   *r;

   r = (struct result *) _mulle_pointermultififo_read_barrier( &spare_fifo);
   if( r)
      return( r);

   return( (struct result *) mulle_allocator_calloc( &mulle_allocator_default,
                                                     1,
                                                     sizeof( struct result)));
}


static void   result_free( struct result *r)
{
   if( ! r)
      return;

   /* the spare ring has no room: fall back to the heap */
   if( _mulle_pointermultififo_write( &spare_fifo, r) == -1)
      mulle_allocator_free( &mulle_allocator_default, r);
}


static void   result_emit( struct result *r)
{
   /* EBUSY means the consumer is behind: let it catch up */
   while( _mulle_pointermultififo_write( &content_fifo, r) == -1)
      mulle_thread_yield();
}


/*
 * ------------------------------------------------------------------
 * matching
 * ------------------------------------------------------------------
 */

static inline int   ascii_lower( int c)
{
   return( c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
}


static inline int   ascii_upper( int c)
{
   return( c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
}


/*
 * Boyer-Moore-Horspool prefilter for the ASCII path.
 *
 * The plain scan at the bottom tests the first pattern byte at every offset,
 * one byte per iteration, which is where the benchmark's ~1.1 GB/s per core
 * came from. Three changes fix that, and none of them copies the line:
 *
 *  - one byte of the pattern is the anchor, and memchr() finds its next
 *    occurrence at memory speed (libc's is SIMD), so the bytes between
 *    candidates are moved over rather than examined;
 *  - after a candidate fails to verify, Horspool's bad-character shift moves
 *    the window by what that byte allows -- a byte the pattern does not also
 *    hold before the anchor shifts it by the whole pattern length;
 *  - the anchor is the pattern's *rarest* byte rather than its last one, and
 *    the window is verified from its ends inward instead of from the anchor
 *    backwards.
 *
 * The anchor choice is worth more than it looks. What costs is the number of
 * candidates memchr() hands back, and in source text two bytes of the same
 * pattern differ by a factor of two or more: for `mulle_allocator` the last
 * byte is `r` (3.7% of a corpus of C source, so a candidate every 27 bytes)
 * and the rarest is `m` (1.7%). Together with verifying the ends first, that
 * measured 2.6x on that pattern end to end (1372 -> 3616 MB/s on a 1 GB file)
 * and 4.5x on a pattern the file does not hold at all (1805 -> 8192 MB/s),
 * where memchr() scans the buffer with nothing else to do. The rank table
 * below is how the rarest byte is picked without looking at the file.
 *
 * `-i` searches both cases of the anchor (two memchr calls, the earlier hit
 * wins) and folds on comparison; the shift table is built folded, so it is
 * looked up with a folded byte.
 *
 * A pattern longer than the table can express (255 bytes) keeps the plain
 * scan rather than growing state.
 */

/*
 * How rare each byte is in the kind of text this gets pointed at: 0 is a byte
 * a 47 MB mixed source corpus never held, 255 is a space. Ascending frequency
 * over this tree's own sources plus the grep 3.12 and ripgrep trees -- 9,052
 * files, 46.7 MB of C, headers, Rust and Markdown -- so regenerating it means
 * sorting the byte histogram of a corpus just like that one.
 *
 * It is a guess about the *searched* file, made once per process and paid for
 * once. Being wrong costs only what anchoring on the last byte always cost:
 * a common anchor means more candidates, never a wrong answer.
 */
static const unsigned char   mg_byte_rank[ 256] =
{
    34,  60,   0,  12,  13,  18,  55,  11,   2, 211, 245,  29,  98, 157,   5,  35,
     3,  44,  30,  25,  19,   9,   1,  26,  31,  37,   6,  71,  39,  17,  70,   4,
   255, 172, 219, 212, 188, 166, 181, 186, 227, 228, 213, 175, 225, 234, 218, 236,
   208, 199, 193, 184, 178, 170, 179, 168, 176, 167, 201, 210, 177, 204, 192, 160,
   165, 231, 200, 220, 215, 238, 206, 194, 195, 232, 163, 191, 233, 214, 226, 222,
   205, 161, 229, 224, 230, 216, 187, 183, 189, 190, 159, 174, 185, 173, 155, 244,
   198, 249, 223, 243, 241, 254, 239, 221, 237, 251, 169, 203, 248, 240, 250, 246,
   235, 171, 252, 247, 253, 242, 209, 207, 202, 217, 182, 197, 180, 196, 156,  40,
   158, 129, 153, 136, 106, 135, 150, 107,  92, 120,  58, 122, 139, 114,  46, 111,
    78,  90, 149, 142, 162,  87, 105, 133, 110, 116, 141, 117, 154, 137,  80, 152,
   115,  85,  84,  82,  93, 121, 134, 118, 143, 146,  99,  95, 101, 112,  76,  97,
    91, 109,  88,  77, 131, 108,  75,  79, 127, 102,  94, 119, 124, 140, 123, 147,
    51,  27, 148, 145,  56,  66,  64,   7,  21,  20,  52,  45,  22,  73, 126,  89,
   130,  81,  36,  32,  69,  96,  53,  14,  33,  54,  61,  67,  38,  10,  23,  62,
   128, 103, 164, 138, 125, 132, 113,  86,  68,  59,  74,  65, 104, 100,  41, 144,
   151,  15,  28,  16,  83,  57,  47,  24,  42,  63,   8,  43,  48,  49,  72,  50
};


static unsigned char   bmh_shift[ 256];    /* byte -> how far the window moves */
static unsigned char   bmh_anchor_lo;      /* the anchor byte, lowered       */
static unsigned char   bmh_anchor_hi;      /* ... and uppercased             */
static int             bmh_anchor_two;     /* the two differ, so search both */
static size_t          bmh_anchor_index;   /* where in the pattern it sits   */
static int             bmh_ready;


static void   ascii_prefilter_init( void)
{
   const unsigned char   *pat;
   size_t                 n;
   size_t                 i;
   int                    c;
   unsigned               best;

   bmh_ready = 0;

   n = options.pattern_len;
   if( ! n || n > 255)
      return;

   pat = (const unsigned char *) options.pattern;

   /* a byte the pattern does not hold before the anchor moves it all */
   for( i = 0; i < 256; i++)
      bmh_shift[ i] = (unsigned char) n;

   for( i = 0; i + 1 < n; i++)
   {
      c = pat[ i];
      if( options.icase)
         c = ascii_lower( c);
      bmh_shift[ c] = (unsigned char) (n - 1 - i);
   }

   /*
    * The anchor is the rarest byte in the pattern, ties going to the earliest
    * position. Under `-i` both cases of a byte are searched, so the rarer of
    * the two is what it is judged by.
    */
   bmh_anchor_index = 0;
   best             = (unsigned) -1;

   for( i = 0; i < n; i++)
   {
      unsigned   r;

      r = mg_byte_rank[ (unsigned char) pat[ i]];
      if( options.icase)
      {
         unsigned   other;

         other = mg_byte_rank[ (unsigned char) ascii_lower( pat[ i])];
         if( other < r)
            r = other;
         other = mg_byte_rank[ (unsigned char) ascii_upper( pat[ i])];
         if( other < r)
            r = other;
      }

      if( r < best)
      {
         best             = r;
         bmh_anchor_index = i;
      }
   }

   c = pat[ bmh_anchor_index];
   if( options.icase)
   {
      bmh_anchor_lo  = (unsigned char) ascii_lower( c);
      bmh_anchor_hi  = (unsigned char) ascii_upper( c);
      bmh_anchor_two = bmh_anchor_lo != bmh_anchor_hi;
   }
   else
   {
      bmh_anchor_lo  = (unsigned char) c;
      bmh_anchor_hi  = (unsigned char) c;
      bmh_anchor_two = 0;
   }

   bmh_ready = 1;
}


/* the next anchor at or after `p`, or NULL if the line ends first */
static const unsigned char   *ascii_anchor( const unsigned char *p,
                                            const unsigned char *end)
{
   const unsigned char   *a;
   const unsigned char   *b;

   if( p >= end)
      return( NULL);

   a = (const unsigned char *) memchr( p, bmh_anchor_lo, (size_t) (end - p));
   if( ! bmh_anchor_two)
      return( a);

   b = (const unsigned char *) memchr( p, bmh_anchor_hi, (size_t) (end - p));
   if( ! a)
      return( b);
   if( ! b)
      return( a);
   return( a < b ? a : b);
}


static inline int   byte_eq( unsigned char a, unsigned char b)
{
   return( options.icase ? ascii_lower( a) == ascii_lower( b) : a == b);
}


/*
 * Does the window at `start` hold the pattern? The anchor byte at `k` is known
 * to match already, so it is not compared again. The window's last byte and
 * then its first are compared ahead of the rest: a candidate that is not a
 * match almost always differs in one of those two, and each costs a single
 * comparison, which is what makes a candidate that is only an anchor hit
 * cheap to throw away.
 */
static int   window_matches( const unsigned char *start,
                             const unsigned char *pat,
                             size_t n,
                             size_t k)
{
   size_t   j;

   j = n - 1;
   if( (k != j) && ! byte_eq( start[ j], pat[ j]))
      return( 0);

   if( k && ! byte_eq( start[ 0], pat[ 0]))
      return( 0);

   for( j = 1; j + 1 < n; j++)
   {
      if( j == k)
         continue;
      if( ! byte_eq( start[ j], pat[ j]))
         return( 0);
   }

   return( 1);
}


/*
 * Byte-wise search, only valid for a pure ASCII pattern: no multi-byte UTF-8
 * sequence contains an ASCII byte, so folding bytes can neither create a false
 * positive nor miss a real one.
 *
 * This is the prefilter's fallback, for a pattern too long to index.
 */
static const char   *search_ascii_plain( const char *line,
                                        size_t line_len,
                                        size_t *match_len)
{
   const unsigned char   *hay;
   size_t                 n;
   size_t                 i;
   size_t                 last;

   n = options.pattern_len;
   if( ! n || n > line_len)
      return( NULL);

   hay  = (const unsigned char *) line;
   last = line_len - n;

   for( i = 0; i <= last; i++)
   {
      const unsigned char   *pat;
      size_t                 j;

      if( options.icase
          ? ascii_lower( hay[ i]) != ascii_lower( (unsigned char) options.pattern[ 0])
          : hay[ i] != (unsigned char) options.pattern[ 0])
         continue;

      pat = (const unsigned char *) options.pattern;
      for( j = 1; j < n; j++)
         if( options.icase
             ? ascii_lower( hay[ i + j]) != ascii_lower( pat[ j])
             : hay[ i + j] != pat[ j])
            break;

      if( j == n)
      {
         *match_len = n;
         return( line + i);
      }
   }

   return( NULL);
}


static const char   *search_ascii( const char *line,
                                  size_t line_len,
                                  size_t *match_len)
{
   const unsigned char   *pat;
   const unsigned char   *hay;
   const unsigned char   *end;
   const unsigned char   *limit;
   const unsigned char   *hit;
   const unsigned char   *start;
   size_t                 n;
   size_t                 k;

   n = options.pattern_len;
   if( ! n || n > line_len)
      return( NULL);

   if( ! bmh_ready)
      return( search_ascii_plain( line, line_len, match_len));

   pat = (const unsigned char *) options.pattern;
   hay = (const unsigned char *) line;
   end = hay + line_len;
   k   = bmh_anchor_index;

   /*
    * A window holds the whole pattern and its anchor sits `k` bytes in, so
    * the last anchor that can start a match is n-1-k bytes before the end.
    */
   limit = end - (n - 1 - k);

   for( hit = hay + k; hit < limit; )
   {
      hit = ascii_anchor( hit, limit);
      if( ! hit)
         return( NULL);

      start = hit - k;
      if( window_matches( start, pat, n, k))
      {
         *match_len = n;
         return( (const char *) start);
      }

      /*
       * Horspool: whatever occupies the window's last byte says how far
       * nothing can start in between, and the anchor moved with the window.
       */
      hit += bmh_shift[ options.icase ? ascii_lower( start[ n - 1])
                                      : start[ n - 1]];
   }

   return( NULL);
}


/*
 * The literal search, case-sensitive: let the platform's memmem() do it where
 * there is one.
 *
 * glibc's memmem() beat the prefilter on a 1 GiB corpus of C source, but only
 * from six bytes of pattern upwards -- 1.4-1.7x there (10.3 GB/s on a 19 byte
 * pattern against 6.1 GB/s) -- while below that its per-call setup is not
 * worth what a short pattern lets it skip (two byte `->`: 2.0 GB/s against
 * 14.7 GB/s). So six bytes and up go to memmem(), shorter ones to the
 * prefilter.
 *
 * The two are interchangeable -- both report the first occurrence in the
 * buffer -- and the fixtures cover both, `needle` being six bytes and `zzz`
 * three. Windows has no memmem(): mingw's string.h declares none, so the
 * prefilter handles every pattern there.
 */
#define MG_LITERAL_MEMMEM_MIN  6

static const char   *search_literal( const char *hay,
                                     size_t length,
                                     size_t *match_len)
{
#if defined( MULLE_DEMO_HAVE_MEMMEM) && MULLE_DEMO_HAVE_MEMMEM
   const char   *hit;

   if( ! options.icase && options.pattern_len >= MG_LITERAL_MEMMEM_MIN)
   {
      hit = (const char *) memmem( hay, length,
                                   options.pattern, options.pattern_len);
      if( hit)
         *match_len = options.pattern_len;
      return( hit);
   }
#endif

   return( search_ascii( hay, length, match_len));
}


/*
 * Compare the folded pattern at `p`, walking with mulle-utf and folding with
 * mulle-unicode. Returns the number of bytes the match occupies, or 0.
 */
static size_t   fold_match_at( const char *p, const char *end)
{
   const char   *start;
   char         *q;
   int32_t       c;
   size_t        i;

   start = p;
   q     = (char *) p;

   for( i = 0; i < options.folded_len; i++)
   {
      if( q >= end)
         return( 0);

      c = (int32_t) mulle_utf8_next_utf32character( &q);
      if( c <= -1)                     /* -1 end of buffer, -2 malformed */
         return( 0);

      if( mulle_unicode_tolower( c) != options.folded[ i])
         return( 0);
   }

   return( (size_t) (q - start));
}


/*
 * Unicode-aware case-insensitive search, used when the pattern itself is not
 * ASCII. This is *simple* case folding, codepoint by codepoint: it does not
 * expand (ß -> SS) and does not handle multi-character folds.
 */
static const char   *search_folded( const char *line,
                                   size_t line_len,
                                   size_t *match_len)
{
   const char   *end;
   const char   *p;
   char         *q;
   size_t        n;

   end = line + line_len;
   for( p = line; p < end; )
   {
      n = fold_match_at( p, end);
      if( n)
      {
         *match_len = n;
         return( p);
      }

      q = (char *) p;
      if( (int32_t) mulle_utf8_next_utf32character( &q) <= -1 || q == p)
         break;                        /* malformed input: stop scanning */

      p = q;
   }

   return( NULL);
}


/* number of characters in [line, hit), or (size_t) -1 if the text is invalid */
static size_t   character_column( const char *line, const char *hit)
{
   char     *p;
   size_t    characters;

   characters = 0;
   p          = (char *) line;

   while( p < hit)
   {
      char   *previous;

      previous = p;
      if( (int32_t) mulle_utf8_next_utf32character( &p) <= -1 || p == previous)
         return( (size_t) -1);

      characters++;
   }

   return( characters);
}


static const char   *find_match( const char *line,
                                size_t line_len,
                                size_t *match_len)
{
   /* grep semantics: an empty pattern matches every line */
   if( ! options.pattern_len)
   {
      *match_len = 0;
      return( line);
   }

   if( options.unicode_fold)
      return( search_folded( line, line_len, match_len));

   return( search_literal( line, line_len, match_len));
}


static void   matcher_init( const char *pattern)
{
   char     *p;
   int32_t  *folded;
   size_t    codepoints;
   size_t    i;

   options.pattern     = pattern;
   options.pattern_len = strlen( pattern);

   ascii_prefilter_init();

   if( ! options.icase || ! options.pattern_len)
      return;

   if( mulle_utf8_is_ascii( pattern, options.pattern_len))
      return;                          /* byte folding is enough */

   /*
    * Non-ASCII pattern with -i: decode it once into folded codepoints. A byte
    * count is an upper bound for a codepoint count, so allocate that and keep
    * track of how much we actually used.
    */
   options.unicode_fold = 1;

   folded = mulle_allocator_malloc( &mulle_allocator_default,
                                    options.pattern_len * sizeof( int32_t));

   codepoints = 0;
   p          = (char *) pattern;
   for( i = 0; i < options.pattern_len; i++)
   {
      char      *previous;
      int32_t    c;

      previous = p;
      c        = (int32_t) mulle_utf8_next_utf32character( &p);

      if( ! c)
         break;                      /* the NUL terminator is not a character */

      if( c <= -1 || p == previous)
      {
         /* invalid: silently fall back to a byte-wise search */
         mulle_allocator_free( &mulle_allocator_default, folded);
         options.unicode_fold = 0;
         return;
      }

      folded[ codepoints++] = mulle_unicode_tolower( c);
   }

   options.folded     = folded;
   options.folded_len = codepoints;
}


static void   matcher_done( void)
{
   mulle_allocator_free( &mulle_allocator_default, options.folded);
}


/*
 * ------------------------------------------------------------------
 * distinct line interning
 * ------------------------------------------------------------------
 */

static struct distinct_line   *distinct_intern( const char *text,
                                               size_t len,
                                               int *p_first)
{
   struct distinct_line   *found;
   struct distinct_line   *new_line;
   struct distinct_line   *existing;
   void                   *old;
   intptr_t                hash;
   int                     rc;

   hash = hashtable_hash_from_bytes( text, len);

   found = (struct distinct_line *) mulle_concurrent_hashtable_lookup( &distincts, hash);
   if( found && found->len == len && ! memcmp( found->text, text, len))
   {
      *p_first = 0;
      return( found);
   }

   new_line             = mulle_allocator_calloc( &mulle_allocator_default,
                                                  1,
                                                  sizeof( struct distinct_line));
   new_line->len        = len;
   new_line->text       = mulle_allocator_malloc( &mulle_allocator_default, len + 1);
   memcpy( new_line->text, text, len);
   new_line->text[ len] = 0;

   old = NULL;
   rc  = mulle_concurrent_hashtable_register( &distincts, hash, new_line, &old);
   if( ! rc && ! old)
   {
      *p_first = 1;
      return( new_line);               /* we won the race */
   }

   /* somebody else got there first */
   existing = (struct distinct_line *) old;
   if( existing && existing->len == len && ! memcmp( existing->text, text, len))
   {
      mulle_allocator_free( &mulle_allocator_default, new_line->text);
      mulle_allocator_free( &mulle_allocator_default, new_line);
      *p_first = 0;
      return( existing);
   }

   /*
    * A genuine 64 bit hash collision between two different lines. We decline
    * to dedupe this line rather than report a count that belongs to another
    * one. See the README: keeping this exact would need a chain that can be
    * extended without a lock.
    */
   mulle_allocator_free( &mulle_allocator_default, new_line->text);
   mulle_allocator_free( &mulle_allocator_default, new_line);

   *p_first = 0;
   return( NULL);
}


/*
 * ------------------------------------------------------------------
 * scanning
 *
 * Two scanners, one contract. scan_file_bulk() is grep's shape and the fast
 * one: search the whole mapping first, and only work out a line once something
 * has matched. The line-at-a-time scanner below it stays for the cases where a
 * match is not a byte string sitting inside one line.
 * ------------------------------------------------------------------
 */

/*
 * Word at a time byte count: eight bytes per iteration, producing a mask with
 * the top bit of every matching byte set.
 *
 * `((x & 0x7F..) + 0x7F..) | x` sets that top bit for every byte that is not
 * zero -- and 0x7F + 0x7F stays inside the byte, so no carry escapes -- which
 * makes the inverse mark exactly the zeros of x. The subtraction usually
 * quoted for this is off by one on a byte that follows a match, which would
 * count a line twice.
 */
#if defined( __GNUC__) || defined( __clang__)
#define mg_popcount64( v)   ((size_t) __builtin_popcountll( v))
#else
static size_t   mg_popcount64( uint64_t v)
{
   size_t   count;

   for( count = 0; v; v >>= 1)
      count += (size_t) (v & 1);

   return( count);
}
#endif


static size_t   mg_count_byte( const char *p, size_t n, unsigned char c)
{
   const unsigned char   *s;
   uint64_t               ones;
   uint64_t               v;
   uint64_t               x;
   size_t                 count;
   size_t                 i;
   size_t                 words;

   s     = (const unsigned char *) p;
   ones  = 0x0101010101010101ULL;
   count = 0;
   words = n >> 3;

   for( i = 0; i < words; i++)
   {
      memcpy( &v, s + (i << 3), sizeof( v));
      x  = v ^ (ones * (uint64_t) c);
      x  = ~(((x & 0x7F7F7F7F7F7F7F7FULL) + 0x7F7F7F7F7F7F7F7FULL) | x);
      x &= 0x8080808080808080ULL;
      count += mg_popcount64( x);
   }

   for( i = words << 3; i < n; i++)
      count += (s[ i] == c);

   return( count);
}


/* the last `c` in [p, p + n), or NULL. memrchr() is not portable to Windows. */
static const char   *mg_memrchr( const char *p, int c, size_t n)
{
   while( n)
   {
      n--;

      if( p[ n] == (char) c)
         return( p + n);
   }

   return( NULL);
}


/*
 * A pattern holding CR or LF can match *across* a line break, and an empty
 * pattern matches every line: neither is a byte string inside one line, so
 * both go to the line scanner.
 */
static int   pattern_crosses_lines( void)
{
   return( ! options.pattern_len
           || memchr( options.pattern, '\n', options.pattern_len) != NULL
           || memchr( options.pattern, '\r', options.pattern_len) != NULL);
}


/*
 * grep's shape: search the whole mapping, and only find out about a line when
 * something matched in it.
 *
 * The search is the same prefilter the line scanner uses -- memchr() over the
 * anchor, Horspool shift -- just handed the rest of the file instead of one
 * line, so the bytes between candidates are moved over at memory speed rather
 * than walked one at a time. From a hit, the line is two bounded lookups:
 * mg_memrchr() back to the previous newline, memchr() on to the next one.
 *
 * Newlines are counted only when the line number is going to be printed, and
 * then in words, so `-c` counts matches without ever looking at a line.
 */
static void   scan_file_bulk( struct file_info *fi,
                              const char *bytes,
                              size_t length)
{
   const char            *end;
   const char            *hit;
   const char            *line;
   const char            *nl;
   const char            *counted_from;
   size_t                 line_len;
   size_t                 match_len;
   size_t                 line_no;
   size_t                 newlines;
   size_t                 column;
   size_t                 characters;
   intptr_t               hits;
   struct result         *r;
   struct distinct_line  *distinct;
   int                    first;

   end          = bytes + length;
   hit          = bytes;
   counted_from = bytes;
   newlines     = 0;
   line_no      = 0;
   hits         = 0;

   while( hit < end)
   {
      hit = find_match( hit, (size_t) (end - hit), &match_len);
      if( ! hit)
         break;

      /* the line the hit fell in: back to its newline, forward to the next */
      nl   = (const char *) memchr( hit, '\n', (size_t) (end - hit));
      line = mg_memrchr( bytes, '\n', (size_t) (hit - bytes));
      line = line ? line + 1 : bytes;

      line_len = nl ? (size_t) (nl - line) : (size_t) (end - line);
      if( line_len && line[ line_len - 1] == '\r')
         line_len--;                    /* keep CRLF files readable */

      /*
       * One result per line, the first match in it, so the scan resumes past
       * this line and any later match inside it is never looked at.
       */
      if( (size_t) (hit - line) + match_len > line_len)
      {
         hit = nl ? nl + 1 : end;
         continue;
      }

      hits++;

      distinct = NULL;
      first    = 0;

      if( options.unique)
      {
         distinct = distinct_intern( line, line_len, &first);
         if( distinct)
            (void) mg_increment_relaxed( &distinct->count);
      }

      if( streaming_output)
      {
         column = (size_t) (hit - line) + 1;
         if( options.characters)
         {
            characters = character_column( line, hit);
            if( characters == (size_t) -1)
               characters = (size_t) (hit - line);
            column = characters + 1;
         }

         /* every line start goes by exactly once: this is one pass in total */
         newlines    += mg_count_byte( counted_from,
                                       (size_t) (line - counted_from),
                                       '\n');
         counted_from = line;
         line_no      = newlines + 1;

         r             = result_alloc();
         r->file       = fi;
         r->distinct   = distinct;
         r->line       = line;
         r->line_len   = line_len;
         r->line_no    = line_no;
         r->match_len  = match_len;
         r->column     = column;
         r->first      = first;

         result_emit( r);
      }

      hit = nl ? nl + 1 : end;
   }

   /*
    * One addition per file rather than one atomic per matching line: a corpus
    * with a match every few kilobytes had 64 workers hammering the same cache
    * line, which is most of what the high thread counts were doing.
    */
   mg_add_relaxed( &fi->hits, hits);
   mg_add_relaxed( &total_matches, hits);
}


static void   scan_file( struct file_info *fi)
{
   const char   *bytes;
   size_t        length;
   size_t        offset;
   size_t        line_no;

   if( _mulle_mmap_map_file( &fi->map, fi->path))
   {
      /*
       * Directories and unreadable files end up here. *Empty* files do not:
       * mulle-mmap maps a zero length file successfully, to a zero length
       * region, so they fall through to the scan below and report 0 hits.
       *
       * This is also the path that leaked a descriptor per failure until
       * mulle-mmap was fixed (bug report 02, found with this demo).
       */
      if( options.verbose)
         mulle_fprintf( stderr, "multigrep: %s: %s\n", fi->path, strerror( errno));
      fi->failed = 1;
      (void) mg_increment_relaxed( &scanned_files);
      return;
   }

   fi->mapped = 1;
   bytes      = (const char *) mulle_mmap_get_bytes( &fi->map);
   length     = mulle_mmap_get_length( &fi->map);
   fi->size   = length;

   /*
    * The bulk scanner accounts for itself, so it returns straight out. What is
    * left below is the line scanner: the cases where a match is not a byte
    * string inside a line.
    */
   if( bytes && length && ! pattern_crosses_lines() && ! options.unicode_fold)
   {
      scan_file_bulk( fi, bytes, length);

      (void) mg_increment_relaxed( &scanned_files);
      mg_add_relaxed( &scanned_bytes, (intptr_t) length);
      return;
   }

   offset  = 0;
   line_no = 0;

   while( bytes && offset < length)
   {
      const char     *line;
      const char     *nl;
      const char     *hit;
      size_t          line_len;
      size_t               match_len;
      size_t               column;
      struct result       *r;
      struct distinct_line *distinct;
      int                  first;

      line     = bytes + offset;
      nl       = (const char *) memchr( line, '\n', length - offset);
      line_len = nl ? (size_t) (nl - line) : length - offset;
      line_no++;

      if( line_len && line[ line_len - 1] == '\r')
         line_len--;                    /* keep CRLF files readable */

      match_len = 0;
      hit       = find_match( line, line_len, &match_len);
      if( hit)
      {
         column = (size_t) (hit - line) + 1;
         if( options.characters)
         {
            size_t   characters;

            characters = character_column( line, hit);
            if( characters == (size_t) -1)
               characters = (size_t) (hit - line);
            column = characters + 1;
         }

         (void) mg_increment_relaxed( &fi->hits);
         (void) mg_increment( &total_matches);

         /*
          * The dedupe table is filled in every output mode; only the result
          * is conditional, because when we are merely counting, the per-file
          * counter above already holds everything we are going to print.
          */
         distinct = NULL;
         first    = 0;

         if( options.unique)
         {
            distinct = distinct_intern( line, line_len, &first);
            if( distinct)
               (void) mg_increment_relaxed( &distinct->count);
         }

         if( streaming_output)
         {
            r             = result_alloc();
            r->file       = fi;
            r->distinct   = distinct;
            r->line       = line;
            r->line_len   = line_len;
            r->line_no    = line_no;
            r->match_len  = match_len;
            r->column     = column;
            r->first      = first;

            result_emit( r);
         }
      }

      /*
       * Resume after the line *and* its newline. `line_len` is not where the
       * line ends in the buffer -- a trailing CR was taken off it -- so adding
       * one for the newline there lands back on the newline itself and counts
       * the empty rest of it as another line. That inflated the line numbers of
       * every CRLF file, which is the one thing the bulk scanner used to
       * disagree with this one about.
       */
      offset = nl ? (size_t) (nl - bytes) + 1 : length;
   }

   (void) mg_increment_relaxed( &scanned_files);
   if( length)
      mg_add_relaxed( &scanned_bytes, (intptr_t) length);
}


/*
 * ------------------------------------------------------------------
 * threads
 * ------------------------------------------------------------------
 */

/*
 * Wait for the producer to publish entry `index`, or for it to declare that no
 * such entry will ever exist. Entries only ever get appended, and the flag is
 * published after the last append, so returning NULL here cannot skip a file.
 */
static struct file_info   *file_at( size_t index)
{
   for(;;)
   {
      struct file_info   *fi;

      fi = (struct file_info *) mulle_concurrent_pointerarray_get( &files,
                                                                   (unsigned int) index);
      if( fi)
         return( fi);

      if( mg_load( &producer_done))
         return( NULL);                  /* this index never materialized */

      mulle_thread_yield();
   }
}


static mulle_thread_rval_t   MULLE_THREAD_CALL   worker_main( void *arg)
{
   (void) arg;

   mulle_aba_register();

   for(;;)
   {
      struct file_info   *fi;
      intptr_t            claim;

      claim = mg_increment( &next_file);
      fi    = file_at( (size_t) claim);
      if( ! fi)
         break;

      scan_file( fi);
   }

   mulle_aba_unregister();

   /* published last, so the consumer never gives up on an in flight result */
   (void) mg_decrement( &active_workers);

   return( (mulle_thread_rval_t) 0);
}


static void   producer_publish( const char *path, size_t len, size_t index)
{
   struct file_info   *fi;

   fi             = mulle_allocator_calloc( &mulle_allocator_default,
                                            1,
                                            sizeof( struct file_info));
   fi->path       = mulle_allocator_malloc( &mulle_allocator_default, len + 1);
   fi->index      = index;
   memcpy( fi->path, path, len);
   fi->path[ len] = 0;

   mulle_mmap_init( &fi->map, mulle_mmap_read);

   if( mulle_concurrent_pointerarray_add( &files, fi))
   {
      mulle_fprintf( stderr, "multigrep: could not queue %s\n", fi->path);
      mulle_allocator_free( &mulle_allocator_default, fi->path);
      mulle_allocator_free( &mulle_allocator_default, fi);
   }
}


/*
 * Read one line into `buffer`, which is reused across calls so that after the
 * first few files the path list stops allocating altogether. Returns 0 at EOF.
 */
static int   read_path_line( FILE *fp, struct mulle_buffer *buffer)
{
   int   c;

   mulle_buffer_reset( buffer);

   while( (c = fgetc( fp)) != EOF)
   {
      if( c == '\n')
         return( 1);
      if( c == '\r')
         continue;
      mulle_buffer_add_byte( buffer, (unsigned char) c);
   }

   return( mulle_buffer_get_length( buffer) != 0);
}


static mulle_thread_rval_t   MULLE_THREAD_CALL   producer_main( void *arg)
{
   char                **paths;
   struct mulle_buffer   buffer;
   size_t                index;
   size_t                i;

   mulle_aba_register();

   index = 0;
   paths = (char **) arg;

   if( paths)
   {
      for( i = 0; paths[ i]; i++)
         producer_publish( paths[ i], strlen( paths[ i]), index++);
   }
   else
   {
      mulle_buffer_init_default( &buffer);

      while( read_path_line( stdin, &buffer))
      {
         char     *bytes;
         size_t    length;

         bytes  = (char *) mulle_buffer_get_string( &buffer);
         length = mulle_buffer_get_length( &buffer);

         if( length && bytes[ length - 1] == 0)
            length--;                 /* get_string appended the terminator */

         producer_publish( bytes, length, index++);
      }

      mulle_buffer_done( &buffer);
   }

   /*
    * Published last: the release ordering of this write makes every appended
    * entry visible to a worker the moment it observes the flag.
    */
   mg_store( &producer_done, 1);

   mulle_aba_unregister();

   return( (mulle_thread_rval_t) 0);
}


/*
 * ------------------------------------------------------------------
 * output
 * ------------------------------------------------------------------
 */

static void   print_match( struct result *r)
{
   if( options.unique && ! r->first)
      return;

   mulle_fprintf( stdout, "%s:%ld:%ld:",
                  r->file->path,
                  (long) r->line_no,
                  (long) r->column);
   (void) fwrite( r->line, 1, r->line_len, stdout);
   fputc( '\n', stdout);
}


/* order: count descending, then file order, so output is reproducible */
static int   compare_file_hits_desc( struct mulle_pointerpair *a,
                                    struct mulle_pointerpair *b,
                                    void *userinfo)
{
   intptr_t   x;
   intptr_t   y;
   size_t     ia;
   size_t     ib;

   (void) userinfo;

   x = (intptr_t) a->key;
   y = (intptr_t) b->key;

   if( x != y)
      return( x < y ? 1 : -1);

   ia = ((struct file_info *) a->value)->index;
   ib = ((struct file_info *) b->value)->index;

   return( ia < ib ? -1 : (ia > ib ? 1 : 0));
}


/* the distinct table stores counts, not file_info, so it needs its own order */
static int   compare_counts_desc( struct mulle_pointerpair *a,
                                 struct mulle_pointerpair *b,
                                 void *userinfo)
{
   intptr_t   x;
   intptr_t   y;

   (void) userinfo;

   x = (intptr_t) a->key;
   y = (intptr_t) b->key;

   return( x < y ? 1 : (x > y ? -1 : 0));
}


static void   print_per_file( void)
{
   unsigned int   i;
   unsigned int   n;

   n = mulle_concurrent_pointerarray_get_count( &files);

   for( i = 0; i < n; i++)
   {
      struct file_info   *fi;
      intptr_t            hits;

      fi = (struct file_info *) mulle_concurrent_pointerarray_get( &files, i);
      if( ! fi)
         continue;

      hits = mg_load( &fi->hits);
      if( ! hits)
         continue;

      if( options.count_only)
         mulle_fprintf( stdout, "%ld\t%s\n", (long) hits, fi->path);
      else
         mulle_fprintf( stdout, "%s\n", fi->path);
   }
}


static void   print_top( void)
{
   struct mulle_pointerpairarray   table;
   unsigned int                    i;
   unsigned int                    n;
   long                            limit;

   _mulle_pointerpairarray_init( &table, 16, NULL);

   n = mulle_concurrent_pointerarray_get_count( &files);
   for( i = 0; i < n; i++)
   {
      struct file_info   *fi;
      intptr_t            hits;

      fi = (struct file_info *) mulle_concurrent_pointerarray_get( &files, i);
      if( ! fi)
         continue;

      hits = mg_load( &fi->hits);
      if( ! hits)
         continue;

      mulle_pointerpairarray_add( &table,
                                  mulle_pointerpair_make( (void *) hits, fi));
   }

   /* the comparators ignore userinfo, but the underlying
    * mulle_pointerpair_qsort_r is MULLE_C_NONNULL_FOURTH, so it cannot be NULL */
   mulle_pointerpairarray_qsort_r( &table, compare_file_hits_desc, &table);

   limit = (long) mulle_pointerpairarray_get_count( &table);
   if( options.top_n < limit)
      limit = options.top_n;

   for( i = 0; i < (unsigned int) limit; i++)
   {
      struct mulle_pointerpair   pair;

      pair = mulle_pointerpairarray_get( &table, i);
      mulle_fprintf( stdout,
                     "%ld\t%s\n",
                     (long) (intptr_t) pair.key,
                     ((struct file_info *) pair.value)->path);
   }

   _mulle_pointerpairarray_done( &table);
}


/*
 * Distinct lines with their final counts, most frequent first. Enumerating a
 * concurrent hashtable can be cancelled by a migration, so restart on
 * ECANCELED. Nothing is writing any more at this point, but the API allows it.
 */
static void   print_distinct_counts( void)
{
   struct mulle_concurrent_hashtableenumerator   rover;
   struct mulle_pointerpairarray                 table;
   unsigned int                                  i;
   unsigned int                                  n;
   int                                           rval;
   intptr_t                                      hash;
   void                                         *value;

   _mulle_pointerpairarray_init( &table, 16, NULL);

restart:
   rover = mulle_concurrent_hashtable_enumerate( &distincts);
   while( (rval = mulle_concurrent_hashtableenumerator_next( &rover, &hash, &value)) == 1)
   {
      struct distinct_line   *d;

      d = (struct distinct_line *) value;

      /* pair encoding: key = count, value = the line itself */
      mulle_pointerpairarray_add( &table,
                                  mulle_pointerpair_make( (void *) mg_load( &d->count),
                                                          (void *) d));
   }

   mulle_concurrent_hashtableenumerator_done( &rover);

   if( rval == ECANCELED)
   {
      mulle_pointerpairarray_reset( &table);
      goto restart;
   }

   mulle_pointerpairarray_qsort_r( &table, compare_counts_desc, &table);

   n = mulle_pointerpairarray_get_count( &table);
   for( i = 0; i < n; i++)
   {
      struct mulle_pointerpair   pair;
      struct distinct_line      *d;

      pair = mulle_pointerpairarray_get( &table, i);
      d    = (struct distinct_line *) pair.value;

      mulle_fprintf( stdout, "%ld\t", (long) (intptr_t) pair.key);
      (void) fwrite( d->text, 1, d->len, stdout);
      fputc( '\n', stdout);
   }

   _mulle_pointerpairarray_done( &table);
}


static void   print_stats( double seconds)
{
   long   matches;
   long   scanned_files_count;
   long   bytes;

   matches             = (long) mg_load( &total_matches);
   scanned_files_count = (long) mg_load( &scanned_files);
   bytes               = (long) mg_load( &scanned_bytes);

   mulle_fprintf( stderr, "multigrep: %ld matching lines in %ld files, %d thread%s\n",
                  matches,
                  scanned_files_count,
                  options.jobs,
                  options.jobs == 1 ? "" : "s");

   if( seconds > 0.0)
      mulle_fprintf( stderr, "multigrep: %.3f s, %.1f MB/s\n",
                     seconds,
                     (double) bytes / 1024.0 / 1024.0 / seconds);
}


/*
 * ------------------------------------------------------------------
 * teardown
 * ------------------------------------------------------------------
 */

static void   release_files( void)
{
   unsigned int   i;
   unsigned int   n;

   n = mulle_concurrent_pointerarray_get_count( &files);
   for( i = 0; i < n; i++)
   {
      struct file_info   *fi;

      fi = (struct file_info *) mulle_concurrent_pointerarray_get( &files, i);
      if( ! fi)
         continue;

      if( fi->mapped)
      {
         mulle_mmap_unmap( &fi->map);
         mulle_mmap_done( &fi->map);
      }

      mulle_allocator_free( &mulle_allocator_default, fi->path);
      mulle_allocator_free( &mulle_allocator_default, fi);
   }

   mulle_concurrent_pointerarray_done( &files);
}


static void   release_spares( void)
{
   void   *p;

   while( (p = _mulle_pointermultififo_read_barrier( &spare_fifo)))
      mulle_allocator_free( &mulle_allocator_default, p);
}


static void   release_distincts( void)
{
   struct mulle_concurrent_hashtableenumerator   rover;
   intptr_t                                      hash;
   void                                         *value;

   rover = mulle_concurrent_hashtable_enumerate( &distincts);
   while( mulle_concurrent_hashtableenumerator_next( &rover, &hash, &value) == 1)
   {
      struct distinct_line   *d;

      d = (struct distinct_line *) value;
      mulle_allocator_free( &mulle_allocator_default, d->text);
      mulle_allocator_free( &mulle_allocator_default, d);
   }
   mulle_concurrent_hashtableenumerator_done( &rover);
   mulle_concurrent_hashtable_done( &distincts);
}


/*
 * ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------
 */

static void   usage( FILE *fp, const char *name)
{
   mulle_fprintf( fp,
      "usage: %s [options] PATTERN [FILE ...]\n"
      "\n"
      "Searches FILEs, or a path list read from stdin, in parallel.\n"
      "\n"
      "  -i, --ignore-case         case-insensitive (Unicode aware)\n"
      "  -j, --jobs N              worker threads (default: %d)\n"
      "  -u, --unique              report each distinct matching line once\n"
      "  -c, --count               count matching lines per file\n"
      "  -l, --files-with-matches  print only names of matching files\n"
      "  -T, --top N               top N files by matching lines\n"
      "      --chars               count columns in characters, not bytes\n"
      "      --stats               print timing and throughput to stderr\n"
      "  -v, --verbose             report files that could not be searched\n"
      "      --                    end of options\n"
      "  -h, --help                this text\n",
      name, DEFAULT_JOBS);
}


/* returns the index of the first file argument, -1 for stdin, or -2 on error */
static int   parse_arguments( int argc, char *argv[])
{
   int   i;

   options.jobs = DEFAULT_JOBS;

   for( i = 1; i < argc; i++)
   {
      const char   *arg;

      arg = argv[ i];

      if( arg[ 0] != '-' || ! arg[ 1])
         break;

      /* everything after -- is a pattern or a file, even one starting with - */
      if( ! strcmp( arg, "--"))
      {
         i++;
         break;
      }

      if( ! strcmp( arg, "-h") || ! strcmp( arg, "--help"))
      {
         usage( stdout, argv[ 0]);
         exit( 0);
      }
      else if( ! strcmp( arg, "-i") || ! strcmp( arg, "--ignore-case"))
         options.icase = 1;
      else if( ! strcmp( arg, "-u") || ! strcmp( arg, "--unique"))
         options.unique = 1;
      else if( ! strcmp( arg, "-c") || ! strcmp( arg, "--count"))
         options.count_only = 1;
      else if( ! strcmp( arg, "-l") || ! strcmp( arg, "--files-with-matches"))
         options.files_only = 1;
      else if( ! strcmp( arg, "--chars"))
         options.characters = 1;
      else if( ! strcmp( arg, "--stats"))
         options.stats = 1;
      else if( ! strcmp( arg, "-v") || ! strcmp( arg, "--verbose"))
         options.verbose = 1;
      else if( ! strcmp( arg, "-j") || ! strcmp( arg, "--jobs"))
      {
         if( ++i >= argc)
         {
            mulle_fprintf( stderr, "multigrep: %s needs a number\n", arg);
            return( -2);
         }
         options.jobs = atoi( argv[ i]);
      }
      else if( ! strcmp( arg, "-T") || ! strcmp( arg, "--top"))
      {
         if( ++i >= argc)
         {
            mulle_fprintf( stderr, "multigrep: %s needs a number\n", arg);
            return( -2);
         }
         options.top   = 1;
         options.top_n = atol( argv[ i]);
      }
      /* combined short options: -j8, -T5 */
      else if( arg[ 1] == 'j' && arg[ 2])
         options.jobs = atoi( arg + 2);
      else if( arg[ 1] == 'T' && arg[ 2])
      {
         options.top   = 1;
         options.top_n = atol( arg + 2);
      }
      else
      {
         mulle_fprintf( stderr, "multigrep: unknown option %s\n", arg);
         return( -2);
      }
   }

   if( i >= argc)
   {
      usage( stderr, argv[ 0]);
      return( -2);
   }

   options.pattern = argv[ i++];

   if( options.jobs < 1)
      options.jobs = 1;
   if( options.jobs > MAX_JOBS)
      options.jobs = MAX_JOBS;

   return( i < argc ? i : -1);
}


int   main( int argc, char *argv[])
{
   mulle_thread_t   workers[ MAX_JOBS];
   mulle_thread_t   producer;
   double           start;
   double           end;
   int              arg_index;
   int              started;
   int              i;
   int              rval;

   arg_index = parse_arguments( argc, argv);
   if( arg_index == -2)
      return( 2);

   matcher_init( options.pattern);

   streaming_output = ! (options.count_only || options.files_only || options.top);

   /* one reclamation service per process, one registration per thread */
   mulle_aba_init( NULL);
   mulle_aba_register();

   mulle_concurrent_pointerarray_init( &files, INITIAL_FILES, NULL);
   mulle_concurrent_hashtable_init( &distincts, INITIAL_DISTINCTS, NULL);

   _mulle_pointermultififo_init( &content_fifo, CONTENT_FIFO_SIZE, NULL);
   _mulle_pointermultififo_init( &spare_fifo, SPARE_FIFO_SIZE, NULL);

   mg_store( &producer_done, 0);
   mg_store( &active_workers, 0);
   mg_store( &next_file, 0);
   mg_store( &total_matches, 0);
   mg_store( &scanned_files, 0);
   mg_store( &scanned_bytes, 0);

   start = mulle_relativetime_now();

   if( mulle_thread_create( producer_main,
                            arg_index < 0 ? NULL : (void *) &argv[ arg_index],
                            &producer))
   {
      mulle_fprintf( stderr, "multigrep: could not start the producer thread\n");
      return( 2);
   }

   started = 0;
   for( i = 0; i < options.jobs; i++)
   {
      (void) mg_increment( &active_workers);
      if( mulle_thread_create( worker_main, NULL, &workers[ i]))
      {
         (void) mg_decrement( &active_workers);
         mulle_fprintf( stderr, "multigrep: could not start a worker thread\n");
         options.jobs = i;
         break;
      }
      started++;
   }

   if( ! started)
   {
      mulle_fprintf( stderr, "multigrep: no worker threads\n");
      return( 2);
   }

   if( streaming_output)
   {
      while( mg_load( &active_workers) > 0)
      {
         struct result   *r;

         r = (struct result *) _mulle_pointermultififo_read_barrier( &content_fifo);
         if( r)
         {
            print_match( r);
            result_free( r);
         }
         else
            mulle_thread_yield();
      }

      /* final drain: every worker is done, so nothing new can arrive */
      for(;;)
      {
         struct result   *r;

         r = (struct result *) _mulle_pointermultififo_read_barrier( &content_fifo);
         if( ! r)
            break;

         print_match( r);
         result_free( r);
      }
   }
   else
      while( mg_load( &active_workers) > 0)
         mulle_thread_yield();

   for( i = 0; i < started; i++)
      (void) mulle_thread_join( workers[ i]);
   (void) mulle_thread_join( producer);

   end = mulle_relativetime_now();

   if( options.top)
      print_top();
   else if( options.unique && options.count_only)
      print_distinct_counts();
   else if( options.count_only || options.files_only)
      print_per_file();

   if( options.stats)
      print_stats( end - start);

   /* grep convention: 0 if anything matched, 1 if nothing did */
   rval = mg_load( &total_matches) ? 0 : 1;

   release_spares();
   _mulle_pointermultififo_done( &spare_fifo);
   _mulle_pointermultififo_done( &content_fifo);

   release_distincts();
   release_files();

   matcher_done();

   mulle_aba_unregister();

   return( rval);
}
