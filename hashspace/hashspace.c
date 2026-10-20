/*
 * hashspace -- what a mulle-concurrent map accepts as a *hash*, and what it
 * does when you hand it one from outside its space.
 *
 * mulle-concurrent has two concurrent maps and they are not variations of each
 * other. Both store an intptr_t "hash" in one word and a payload in the next,
 * without locks and without waiting, and both steal bits from the hash word to
 * mark slots. That is the whole point of this demo: the hash is not an opaque
 * key, it lives in a word the table also writes to, so the value space is a
 * contract you have to know.
 *
 *   hashtable, "positive" mode   FROZEN is the top bit.  Legal: [1, INTPTR_MAX]
 *                                63 usable bits on LP64. Pointers are safe here
 *                                (user space never sets bit 63) but NOT on ILP32.
 *   hashtable, "even" mode       FROZEN is bit 0. Legal: even and non-zero.
 *                                Full 64 bit range, so aligned pointers are safe
 *                                everywhere. Odd hashes are rejected.
 *   hashmap                      No stolen bit, so the full intptr_t works,
 *                                except that 0 means "unclaimed".
 *
 * Out-of-range is *rejected*, never masked: insert/register/remove return EINVAL
 * and lookup returns NULL. So a hash you "massage" into range silently aliases
 * nothing -- you get an error, which is the good news -- but a hash you fail to
 * massage is a key that is simply never stored.
 *
 * The other half is that neither map replaces. There is no "set". An existing
 * hash refuses an insert with EEXIST, and a remove must name both the hash and
 * the value it expects (ENOENT otherwise). The hashtable can remove concurrently
 * and reuse the slot immediately; the hashmap's remove is single-threaded only,
 * because it is a plain write, so `patch` is all it has.
 *
 * Modes:
 *   --space       the acceptance table, measured against the library
 *   --replace     the no-replace rules, insert/register/remove/patch
 *   --churn       threads against one map, with latency percentiles
 *   --mask        what a size that is not a power of two does
 *   --universe    mulle_aba_init() and why forgetting it is latent
 *   --all         all of the above
 *
 * Options: --map hashtable|hashmap --threads N --ops N --keys N --no-remove
 *          --migrate --verify --enumerate --seed N --quiet --help
 *
 * --churn runs the same workload against either map. The operation mix is a
 * property of the map, not of the demo: the hashmap has no concurrent remove at
 * all, so --no-remove gives the hashtable the hashmap's mix and then the two
 * runs are the same workload -- same seeds, same keys, same order -- and the
 * numbers compare.
 */
#include <mulle-core/mulle-core.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define HS_MAX_THREADS      64
#define HS_DEFAULT_THREADS  4
#define HS_DEFAULT_OPS      200000
#define HS_DEFAULT_KEYS     1024

/*
 * Latency, per thread, in logarithmic buckets: four sub-buckets per octave, so
 * a bucket is ~19% wide instead of ~100%, up to 2^24 ns (16.7 ms) plus one
 * bucket for whatever is above that. Percentiles interpolate inside a bucket.
 * Recorded per thread and merged when printed, because a shared counter on the
 * measuring path lands in the number being measured.
 */
#define HS_LATENCY_OCTAVES   24
#define HS_LATENCY_BUCKETS   (HS_LATENCY_OCTAVES * 4 + 1)


struct hs_options
{
   long   threads;
   long   ops;
   long   keys;
   int    hashmap;
   int    no_remove;
   int    migrate;
   int    verify;
   int    enumerate;
   int    quiet;
   int    seed;
};


struct hs_latency
{
   unsigned long long   counts[ HS_LATENCY_BUCKETS];
   unsigned long long   samples;
   unsigned long long   min_ns;
   unsigned long long   max_ns;
};


/*
 * The value for a key. Never 0 (that is EMPTY everywhere), never INTPTR_MIN,
 * and even: the hashtable's default value mode seals bit 0, so an odd payload
 * is rejected with EINVAL. Derived from the key so that a lookup can be
 * *verified* rather than just counted: a slot that returns the wrong pointer is
 * corrupt, not slow.
 */
static void   *hs_value_for( long key)
{
   return( (void *) (intptr_t) ((uintptr_t) key * 16 + 2));
}


static unsigned int   hs_bucket( unsigned long long ns)
{
   unsigned int         octave;
   unsigned long long   base;

   if( ns < 1)
      return( 0);

   octave = 0;
   base   = 1;
   while( octave + 1 < HS_LATENCY_OCTAVES && (base << 1) <= ns)
   {
      base <<= 1;
      octave++;
   }

   if( octave + 1 >= HS_LATENCY_OCTAVES)
      return( HS_LATENCY_BUCKETS - 1);

   return( 1 + octave * 4 + (unsigned int) (((ns - base) * 4) / base));
}


static unsigned long long   hs_bucket_lo( unsigned int bucket)
{
   unsigned int   octave;

   if( ! bucket)
      return( 0);
   if( bucket == HS_LATENCY_BUCKETS - 1)
      return( 1ULL << HS_LATENCY_OCTAVES);

   octave = (bucket - 1) / 4;
   return( (1ULL << octave) + (unsigned long long) ((bucket - 1) % 4) * (1ULL << octave) / 4);
}


static unsigned long long   hs_bucket_width( unsigned int bucket)
{
   if( ! bucket || bucket == HS_LATENCY_BUCKETS - 1)
      return( 1);
   return( 1ULL << ((bucket - 1) / 4) / 4);
}


static void   hs_latency_record( struct hs_latency *lat, unsigned long long ns)
{
   lat->counts[ hs_bucket( ns)]++;
   lat->samples++;
   if( ! lat->min_ns || ns < lat->min_ns)
      lat->min_ns = ns;
   if( ns > lat->max_ns)
      lat->max_ns = ns;
}


static void   hs_latency_merge( struct hs_latency *dst, const struct hs_latency *src)
{
   unsigned int   i;

   for( i = 0; i < HS_LATENCY_BUCKETS; i++)
      dst->counts[ i] += src->counts[ i];

   dst->samples += src->samples;
   if( src->samples)
   {
      if( ! dst->min_ns || src->min_ns < dst->min_ns)
         dst->min_ns = src->min_ns;
      if( src->max_ns > dst->max_ns)
         dst->max_ns = src->max_ns;
   }
}


static unsigned long long   hs_percentile( const struct hs_latency *lat, double p)
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
   for( i = 0; i < HS_LATENCY_BUCKETS; i++)
   {
      count = lat->counts[ i];
      if( ! count)
         continue;

      if( seen + count >= target)
         return( hs_bucket_lo( i) +
                 (target - seen - 1) * hs_bucket_width( i) / count);
      seen += count;
   }
   return( lat->max_ns);
}


static const char   *hs_errno_name( int rval)
{
   switch( rval)
   {
   case 0      : return( "ok");
   case EEXIST : return( "EEXIST");
   case ENOENT : return( "ENOENT");
   case EINVAL : return( "EINVAL");
   case EBUSY  : return( "EBUSY");
   case ECANCELED: return( "ECANCELED");
   }
   return( NULL);
}


static void   hs_usage( FILE *fp)
{
   mulle_fprintf( fp,
      "usage: hashspace [mode] [options]\n"
      "\n"
      "modes:\n"
      "  --space       the hash space each map accepts (default)\n"
      "  --replace     no replace: insert/register/remove/patch\n"
      "  --churn       threaded churn against one map\n"
      "  --mask        what a size that is not a power of two does\n"
      "  --universe    mulle_aba_init() and the latent abort\n"
      "  --all         all of the above\n"
      "\n"
      "options:\n"
      "  --map NAME    churn the hashtable (default) or the hashmap\n"
      "  --no-remove   skip removes, which is the hashmap's mix: the hashtable\n"
      "                then runs the same workload the hashmap can run\n"
      "  --threads N   churn threads (default %d, max %d)\n"
      "  --ops N       operations per thread (default %d)\n"
      "  --keys N      distinct hashes in the shared key space (default %d)\n"
      "  --migrate     force generation changes while churning\n"
      "  --verify      after the churn, check the map's contents and drain it\n"
      "  --enumerate   add an enumerating thread, report ECANCELED restarts\n"
      "  --seed N      seed the workload RNG\n"
      "  --quiet       only the k=v summary lines\n"
      "  --help\n",
      HS_DEFAULT_THREADS, HS_MAX_THREADS, HS_DEFAULT_OPS, HS_DEFAULT_KEYS);
}


//
// --space: the acceptance table
//

struct hs_spacekey
{
   const char   *name;
   intptr_t      hash;
   const char   *why;
};


static void   hs_dump_space( const struct hs_options *options)
{
   struct mulle_concurrent_hashtable   positive;
   struct mulle_concurrent_hashtable   even;
   struct mulle_concurrent_hashmap     hashmap;
   struct hs_spacekey                  keys[ 12];
   void                                *block;
   size_t                              n;
   size_t                              i;
   int                                 pos,
                                       evn,
                                       hsh;

   /*
    * Real allocations, so the pointer cases are real addresses rather than
    * plausible constants.
    */
   block = mulle_malloc( 64);
   keys[ 0].name = "0";        keys[ 0].hash = (intptr_t) 0;          keys[ 0].why = "0 is NO_HASH in both maps";
   keys[ 1].name = "1";        keys[ 1].hash = (intptr_t) 1;          keys[ 1].why = "odd, so even mode cannot claim it";
   keys[ 2].name = "2";        keys[ 2].hash = (intptr_t) 2;          keys[ 2].why = "even and non-zero";
   keys[ 3].name = "0x1000";   keys[ 3].hash = (intptr_t) 0x1000;     keys[ 3].why = "even and non-zero";
   keys[ 4].name = "INTPTR_MAX"; keys[ 4].hash = INTPTR_MAX;          keys[ 4].why = "top bit clear, so positive mode keeps it";
   keys[ 5].name = "-1";       keys[ 5].hash = (intptr_t) -1;         keys[ 5].why = "top bit set and odd: out of both spaces";
   keys[ 6].name = "-2";       keys[ 6].hash = (intptr_t) -2;         keys[ 6].why = "top bit set, but even: even mode takes it";
   keys[ 7].name = "INTPTR_MIN"; keys[ 7].hash = INTPTR_MIN;          keys[ 7].why = "top bit set, but even: even mode takes it";
   keys[ 8].name = "0x8001";   keys[ 8].hash = (intptr_t) 0x8001;     keys[ 8].why = "not a pointer, just a legal small hash";
   keys[ 9].name = "malloc()"; keys[ 9].hash = (intptr_t) block;      keys[ 9].why = "an aligned pointer is even";
   keys[10].name = "malloc()+1"; keys[10].hash = (intptr_t) block + 1; keys[10].why = "unaligned: even mode refuses it";
   keys[11].name = "malloc()+16"; keys[11].hash = (intptr_t) block + 16; keys[11].why = "another aligned pointer";
   n = 12;

   mulle_concurrent_hashtable_init_positive( &positive, 16, NULL);
   mulle_concurrent_hashtable_init_even( &even, 16, NULL);
   mulle_concurrent_hashmap_init( &hashmap, 16, NULL);

   mulle_fprintf( stdout, "=== hash space ===\n");

   if( options->quiet)
   {
      mulle_fprintf( stdout, "space.intptr_bits=%u\n", (unsigned int) (sizeof( intptr_t) * 8));
      for( i = 0; i < n; i++)
      {
         pos = mulle_concurrent_hashtable_insert( &positive, keys[ i].hash, hs_value_for( (long) i + 1));
         evn = mulle_concurrent_hashtable_insert( &even, keys[ i].hash, hs_value_for( (long) i + 1));
         hsh = mulle_concurrent_hashmap_insert( &hashmap, keys[ i].hash, hs_value_for( (long) i + 1));
         mulle_fprintf( stdout, "space.%s=positive:%s,even:%s,hashmap:%s\n",
                        keys[ i].name,
                        hs_errno_name( pos) ? hs_errno_name( pos) : "errno",
                        hs_errno_name( evn) ? hs_errno_name( evn) : "errno",
                        hs_errno_name( hsh) ? hs_errno_name( hsh) : "errno");
      }
   }
   else
   {
      mulle_fprintf( stdout, "%-13s %-9s %-9s %-9s  %s\n",
                     "hash", "positive", "even", "hashmap", "why");
      for( i = 0; i < n; i++)
      {
         pos = mulle_concurrent_hashtable_insert( &positive, keys[ i].hash, hs_value_for( (long) i + 1));
         evn = mulle_concurrent_hashtable_insert( &even, keys[ i].hash, hs_value_for( (long) i + 1));
         hsh = mulle_concurrent_hashmap_insert( &hashmap, keys[ i].hash, hs_value_for( (long) i + 1));

         mulle_fprintf( stdout, "%-13s ", keys[ i].name);
         mulle_fprintf( stdout, "%-9s ", hs_errno_name( pos) ? hs_errno_name( pos) : "errno");
         mulle_fprintf( stdout, "%-9s ", hs_errno_name( evn) ? hs_errno_name( evn) : "errno");
         mulle_fprintf( stdout, "%-9s  ", hs_errno_name( hsh) ? hs_errno_name( hsh) : "errno");
         mulle_fprintf( stdout, "%s\n", keys[ i].why);
      }

      mulle_fprintf( stdout,
         "\n"
         "  positive  %u usable bits, pointers are safe on LP64 and NOT on ILP32\n"
         "  even      the full %u bits, but odd hashes are refused outright\n"
         "  hashmap   the full %u bits, and 0 is the only unusable hash\n"
         "\n"
         "  Out of space is EINVAL, never a silent truncation: \"0x8001\" and\n"
         "  \"0x0001\" are the same key in positive mode only if they are the same\n"
         "  number, which on LP64 they are not.\n",
         (unsigned int) (sizeof( intptr_t) * 8 - 1),
         (unsigned int) (sizeof( intptr_t) * 8),
         (unsigned int) (sizeof( intptr_t) * 8));
   }

   mulle_concurrent_hashmap_done( &hashmap);
   mulle_concurrent_hashtable_done( &even);
   mulle_concurrent_hashtable_done( &positive);
   mulle_free( block);
}


//
// --replace: no set, only remove-and-insert
//

static const char   *hs_rval_name( int rval)
{
   const char   *name;

   name = hs_errno_name( rval);
   return( name ? name : "errno");
}


/* one row: readable when watched, k=v when parsed */
static void   hs_fact( const struct hs_options *options,
                       const char *key,
                       const char *label,
                       const char *result)
{
   if( options->quiet)
      mulle_fprintf( stdout, "%s=%s\n", key, result);
   else
      mulle_fprintf( stdout, "%-28s -> %s\n", label, result);
}


static void   hs_dump_replace( const struct hs_options *options)
{
   struct mulle_concurrent_hashtable   map;
   struct mulle_concurrent_hashmap     hashmap;
   char                                result[ 128];
   void                                *old;
   void                                *rval;
   intptr_t                            hash = 0x200;
   int                                 rval_int;

   mulle_concurrent_hashtable_init_positive( &map, 16, NULL);
   mulle_concurrent_hashmap_init( &hashmap, 16, NULL);

   if( ! options->quiet)
      mulle_fprintf( stdout, "=== no replace ===\n");

   /*
    * Separate statements throughout: calling a mutator inside the same printf
    * as a read of its effect leaves the order unspecified, and the read can win.
    */
   rval_int = mulle_concurrent_hashtable_insert( &map, hash, (void *) 0x10);
   hs_fact( options, "replace.insert_a", "hashtable insert A", hs_rval_name( rval_int));

   rval_int = mulle_concurrent_hashtable_insert( &map, hash, (void *) 0x20);
   hs_fact( options, "replace.insert_b", "hashtable insert B", hs_rval_name( rval_int));

   rval = mulle_concurrent_hashtable_lookup( &map, hash);
   hs_fact( options, "replace.lookup", "hashtable lookup",
            rval == (void *) 0x10 ? "A (B was refused)" : "not A");

   rval_int = mulle_concurrent_hashtable_remove( &map, hash, (void *) 0x20);
   hs_fact( options, "replace.remove_b", "hashtable remove B", hs_rval_name( rval_int));

   rval_int = mulle_concurrent_hashtable_remove( &map, hash, (void *) 0x10);
   hs_fact( options, "replace.remove_a", "hashtable remove A", hs_rval_name( rval_int));

   rval_int = mulle_concurrent_hashtable_insert( &map, hash, (void *) 0x20);
   hs_fact( options, "replace.insert_reuse", "hashtable insert B again",
            hs_rval_name( rval_int));

   old      = (void *) 0xdead;
   rval_int = mulle_concurrent_hashtable_register( &map, hash, (void *) 0x30, &old);
   mulle_snprintf( result, sizeof( result), "%s p_old=%s",
             hs_rval_name( rval_int),
             old == (void *) 0x20 ? "B" : (old == NULL ? "NULL" : "other"));
   hs_fact( options, "replace.register_live", "hashtable register C over B", result);

   old      = (void *) 0xdead;
   rval_int = mulle_concurrent_hashtable_register( &map, (intptr_t) 0x400, (void *) 0x40, &old);
   mulle_snprintf( result, sizeof( result), "%s p_old=%s",
             hs_rval_name( rval_int),
             old == NULL ? "NULL" : "other");
   hs_fact( options, "replace.register_free", "hashtable register D on a free hash", result);

   rval = mulle_concurrent_hashmap_register( &hashmap, (intptr_t) 0x800, (void *) 0x8);
   hs_fact( options, "replace.hashmap_register_empty", "hashmap register on empty",
            rval == MULLE_CONCURRENT_NO_POINTER ? "NO_POINTER (inserted)" :
            (rval == (void *) MULLE_CONCURRENT_INVALID_POINTER ? "INVALID_POINTER" : "existing"));

   rval = mulle_concurrent_hashmap_register( &hashmap, (intptr_t) 0x800, (void *) 0xc);
   hs_fact( options, "replace.hashmap_register_again", "hashmap register again",
            rval == (void *) 0x8 ? "the existing value" : "other");

   rval_int = mulle_concurrent_hashmap_insert( &hashmap, (intptr_t) 0x800, (void *) 0xc);
   hs_fact( options, "replace.hashmap_insert_again", "hashmap insert again",
            hs_rval_name( rval_int));

   rval_int = _mulle_concurrent_hashmap_patch( &hashmap, (intptr_t) 0x800, (void *) 0xa);
   mulle_snprintf( result, sizeof( result), "%s, lookup now %s",
             hs_rval_name( rval_int),
             mulle_concurrent_hashmap_lookup( &hashmap, (intptr_t) 0x800) == (void *) 0xa ? "0xa" : "other");
   hs_fact( options, "replace.hashmap_patch", "hashmap patch (1 thread only)", result);

   rval_int = _mulle_concurrent_hashmap_remove( &hashmap, (intptr_t) 0x800);
   mulle_snprintf( result, sizeof( result), "%s, lookup now %s",
             hs_rval_name( rval_int),
             mulle_concurrent_hashmap_lookup( &hashmap, (intptr_t) 0x800) ? "a value" : "NULL");
   hs_fact( options, "replace.hashmap_remove", "hashmap remove (1 thread only)", result);

   if( ! options->quiet)
      mulle_fprintf( stdout,
         "\n"
         "  A \"set\" is therefore remove(hash, expected_value) then insert: the\n"
         "  hashtable does that concurrently, and a removed hash is insertable\n"
         "  again at once, with no migration and no tombstone.\n"
         "\n"
         "  patch and remove above are SINGLE-THREADED ONLY: they write the value\n"
         "  word with no CAS. The hashmap has no concurrent remove at all, while\n"
         "  the hashtable's remove(hash, value) is CAS based. That is what the\n"
         "  hashmap charges for keeping the full hash width.\n");

   mulle_concurrent_hashmap_done( &hashmap);
   mulle_concurrent_hashtable_done( &map);
}


//
// --churn: threads on one map
//

/*
 * The storage's probe mask is `size - 1`, and a linear probe terminates only
 * because the mask makes the sequence visit every slot. With any other size it
 * visits a small subset and a probe for an absent hash never reaches a virgin
 * slot -- so the size a caller asks for is a *hint*. The storage allocator
 * rounds it up to the next power of two before deriving the mask, which is why
 * this helper exists at all: it is the same rounding, for sizes the demo picks.
 */
static size_t   hs_next_power_of_two( size_t n)
{
   size_t   size;

   size = 4;
   while( size < n)
      size <<= 1;
   return( size);
}


/*
 * One workload, two maps. They are not variations of each other: the hashmap
 * keeps the full hash width because it steals no bit, and it pays for that with
 * no concurrent remove at all. So the operation mix belongs to the map, not to
 * the demo -- and --no-remove gives the hashtable the hashmap's mix, which is
 * what makes two runs comparable: same seeds, same keys, same order.
 */
struct hs_map
{
   struct mulle_concurrent_hashtable   *hashtable;
   struct mulle_concurrent_hashmap     *hashmap;
   int                                  is_hashmap;
};


static int   hs_map_insert( struct hs_map *map, intptr_t hash, void *value)
{
   if( map->is_hashmap)
      return( mulle_concurrent_hashmap_insert( map->hashmap, hash, value));
   return( mulle_concurrent_hashtable_insert( map->hashtable, hash, value));
}


static void   *hs_map_lookup( struct hs_map *map, intptr_t hash)
{
   if( map->is_hashmap)
      return( mulle_concurrent_hashmap_lookup( map->hashmap, hash));
   return( mulle_concurrent_hashtable_lookup( map->hashtable, hash));
}


/* the concurrent remove, which only the hashtable has */
static int   hs_map_remove( struct hs_map *map, intptr_t hash, void *value)
{
   if( map->is_hashmap)
      return( ENOTSUP);              /* never reached: the mix drops it first */
   return( mulle_concurrent_hashtable_remove( map->hashtable, hash, value));
}


/*
 * Single threaded, which is the only way the hashmap can remove anything: its
 * patch writes the value word with no CAS, so it is not a concurrent operation.
 * The hashtable's remove(hash, value) is CAS based and checks the value it
 * replaces; the hashmap's takes the hash alone.
 */
static int   hs_map_remove_single_threaded( struct hs_map *map, intptr_t hash, void *value)
{
   if( map->is_hashmap)
      return( _mulle_concurrent_hashmap_remove( map->hashmap, hash));
   return( mulle_concurrent_hashtable_remove( map->hashtable, hash, value));
}


/* the size the table is using now, which a forced generation changes */
static size_t   hs_map_size( struct hs_map *map)
{
   if( map->is_hashmap)
      return( (size_t) mulle_concurrent_hashmap_get_size( map->hashmap));
   return( mulle_concurrent_hashtable_get_size( map->hashtable));
}


static size_t   hs_map_count( struct hs_map *map)
{
   if( map->is_hashmap)
      return( (size_t) mulle_concurrent_hashmap_count( map->hashmap));
   return( mulle_concurrent_hashtable_count( map->hashtable));
}


/*
 * A walk over either map. Both enumerators answer ECANCELED for the same reason
 * and in the same shape, so the accounting is shared instead of duplicated.
 */
struct hs_rover
{
   union
   {
      struct mulle_concurrent_hashtableenumerator   hashtable;
      struct mulle_concurrent_hashmapenumerator     hashmap;
   } u;
   int   is_hashmap;
};


static struct hs_rover   hs_rover_create( struct hs_map *map)
{
   struct hs_rover   rover;

   rover.is_hashmap = map->is_hashmap;
   if( map->is_hashmap)
      rover.u.hashmap = mulle_concurrent_hashmap_enumerate( map->hashmap);
   else
      rover.u.hashtable = mulle_concurrent_hashtable_enumerate( map->hashtable);
   return( rover);
}


static int   hs_rover_next( struct hs_rover *rover, intptr_t *hash, void **value)
{
   if( rover->is_hashmap)
      return( mulle_concurrent_hashmapenumerator_next( &rover->u.hashmap, hash, value));
   return( mulle_concurrent_hashtableenumerator_next( &rover->u.hashtable, hash, value));
}


struct hs_worker
{
   struct hs_map                        map;
   struct hs_latency                    latency;
   unsigned int                         index;
   long                                 threads;
   long                                 ops;
   long                                 keys;
   unsigned int                         seed;
   int                                  enumerate;
   int                                  has_remove;

   unsigned long long                   ops_done;
   unsigned long long                   insert_attempts;
   unsigned long long                   remove_attempts;
   unsigned long long                   inserted;
   unsigned long long                   eexist;
   unsigned long long                   lookups;
   unsigned long long                   hits;
   unsigned long long                   misses;
   unsigned long long                   removed;
   unsigned long long                   enoent;
   unsigned long long                   einval;
   unsigned long long                   corrupt;
   unsigned long long                   enumerated;
   unsigned long long                   walks;
   unsigned long long                   restarts;
};


static unsigned int   hs_random( unsigned int *state)
{
   unsigned int   x;

   x = *state;
   x ^= x << 13;
   x ^= x >> 17;
   x ^= x << 5;
   *state = x;
   return( x);
}


static mulle_thread_rval_t   hs_churn_main( void *arg)
{
   struct hs_worker      *worker;
   struct hs_latency     *lat;
   unsigned int          state;
   unsigned long long    start;
   unsigned long long    elapsed;
   long                  i;
   long                  key;
   intptr_t              hash;
   void                  *value;
   void                  *found;
   int                   rval;
   int                   op;

   worker = arg;
   lat    = &worker->latency;
   state  = worker->seed ? worker->seed : 1;

   /*
    * The second half of the universe contract: every thread that touches a
    * concurrent container registers with the reclaimer once.
    */
   mulle_aba_register();

   for( i = 0; i < worker->ops; i++)
   {
      key   = (long) (hs_random( &state) % (unsigned int) worker->keys) + 1;
      hash  = (intptr_t) key;
      value = hs_value_for( key);
      op    = (int) (hs_random( &state) % 3);
      if( op == 2 && ! worker->has_remove)
         op = 1;              /* no concurrent remove here: look the hash up */
      worker->ops_done++;

      start = (unsigned long long) (mulle_relativetime_now() * 1e9);
      switch( op)
      {
      case 0 :
         worker->insert_attempts++;
         rval = hs_map_insert( &worker->map, hash, value);
         switch( rval)
         {
         case 0      : worker->inserted++; break;
         case EEXIST : worker->eexist++;   break;
         case EINVAL : worker->einval++;   break;
         default     : break;
         }
         break;

      case 1 :
         found = hs_map_lookup( &worker->map, hash);
         worker->lookups++;
         if( found)
         {
            worker->hits++;
            if( found != value)      /* the value must match the key it is under */
               worker->corrupt++;
         }
         else
            worker->misses++;
         break;

      default :
         worker->remove_attempts++;
         rval = hs_map_remove( &worker->map, hash, value);
         switch( rval)
         {
         case 0      : worker->removed++; break;
         case ENOENT : worker->enoent++;  break;
         case EINVAL : worker->einval++;  break;
         default     : break;
         }
         break;
      }

      elapsed = (unsigned long long) (mulle_relativetime_now() * 1e9) - start;
      hs_latency_record( lat, elapsed);
   }

   mulle_aba_unregister();
   return( 0);
}


/*
 * A walk, counted. ECANCELED is not an error here: it is the documented answer
 * to "the storage changed, start over", and counting the restarts is the point.
 * Every value seen is checked against its own hash.
 */
static void   hs_walk( struct hs_map *map, struct hs_worker *worker, long rounds)
{
   struct hs_rover   rover;
   intptr_t          hash;
   void              *value;
   int               rval;
   long              i;

   for( i = 0; i < rounds; i++)
   {
      worker->walks++;
      rover = hs_rover_create( map);
      for(;;)
      {
         rval = hs_rover_next( &rover, &hash, &value);
         if( rval == ECANCELED)
         {
            worker->restarts++;
            break;
         }
         if( rval != 1)
            break;

         worker->enumerated++;
         if( value != hs_value_for( (long) hash))
            worker->corrupt++;      /* a hash under the wrong value */
      }
   }
}


/*
 * The enumerating thread. While the others churn (and possibly force
 * generations), this walks the table.
 */
static mulle_thread_rval_t   hs_enumerate_main( void *arg)
{
   struct hs_worker   *worker;

   worker = arg;
   mulle_aba_register();
   hs_walk( &worker->map, worker, worker->ops / 64 + 1);
   mulle_aba_unregister();
   return( 0);
}


struct hs_migrator
{
   struct mulle_concurrent_hashtable   *map;
   mulle_atomic_pointer_t                stop;
   unsigned long long                    migrations;
};


static mulle_thread_rval_t   hs_migrate_main( void *arg)
{
   struct hs_migrator   *migrator;

   migrator = arg;
   mulle_aba_register();

   /*
    * Paced, not flat out. Continuous generations are what makes the
    * remove-versus-copy race reproducible, but a migrator with no pause
    * cancels every enumeration instantly, and an enumerator that never sees an
    * entry cannot show that what it does see is consistent. One per millisecond
    * is still ~1000 generations and leaves the walks room to complete.
    */
   while( ! _mulle_atomic_pointer_read( &migrator->stop))
   {
      _mulle_concurrent_hashtable_migrate_same_size( migrator->map);
      migrator->migrations++;
      mulle_relativetime_sleep( 0.001);
   }

   mulle_aba_unregister();
   return( 0);
}


static int   hs_churn( const struct hs_options *options)
{
   struct mulle_concurrent_hashtable   hashtable;
   struct mulle_concurrent_hashmap     hashmap;
   struct hs_map                       map;
   struct hs_worker                    workers[ HS_MAX_THREADS];
   struct hs_migrator                  migrator;
   mulle_thread_t                      handles[ HS_MAX_THREADS];
   mulle_thread_t                      migrate_handle;
   struct hs_worker                    enumerator;
   mulle_thread_t                      enumerate_handle;
   struct hs_latency                   total;
   unsigned long long                  insert_attempts = 0;
   unsigned long long                  remove_attempts = 0;
   unsigned long long                  inserted      = 0;
   unsigned long long                  eexist        = 0;
   unsigned long long                  lookups       = 0;
   unsigned long long                  hits          = 0;
   unsigned long long                  misses        = 0;
   unsigned long long                  removed       = 0;
   unsigned long long                  enoent        = 0;
   unsigned long long                  einval        = 0;
   unsigned long long                  corrupt       = 0;
   unsigned long long                  enumerated    = 0;
   unsigned long long                  walks         = 0;
   unsigned long long                  restarts      = 0;
   unsigned long long                  migrations    = 0;
   double                              start;
   double                              wall;
   size_t                              size;
   size_t                              table;
   size_t                              live;
   unsigned long long                  ops_done      = 0;
   long                                i;
   long                                drained;
   int                                 failed;
   int                                 enumerated_entries;
   int                                 has_remove;
   const char                          *mix;

   memset( workers, 0, sizeof( workers));
   memset( &total, 0, sizeof( total));
   memset( &migrator, 0, sizeof( migrator));
   memset( &enumerator, 0, sizeof( enumerator));

   map.is_hashmap = options->hashmap;
   map.hashtable  = options->hashmap ? NULL : &hashtable;
   map.hashmap    = options->hashmap ? &hashmap : NULL;

   /*
    * The mix belongs to the map: the hashmap has no concurrent remove, so a
    * hashmap run cannot have one, and --no-remove asks the hashtable for the
    * same thing. After that the two runs are the same workload, because the op
    * is drawn from the seed before the map is touched and the maps do not feed
    * anything back into the generator.
    */
   has_remove = ! options->hashmap && ! options->no_remove;
   mix        = has_remove ? "insert,lookup,remove" : "insert,lookup";

   /*
    * A size that all the keys fit in, so growth is not what the run measures.
    * --migrate forces generations on top of that, which is the interesting
    * case: the table is retiring storage while every thread writes to it.
    */
   size = hs_next_power_of_two( (size_t) options->keys * 2 + 16);
   if( options->hashmap)
      mulle_concurrent_hashmap_init( &hashmap, (unsigned int) size, NULL);
   else
      mulle_concurrent_hashtable_init_positive( &hashtable, size, NULL);

   if( ! options->quiet)
   {
      mulle_fprintf( stdout, "=== churn ===\n");
      mulle_fprintf( stdout, "map=%s threads=%ld ops/thread=%ld keys=%ld table=%zu mix=%s migrate=%s verify=%s\n",
                     options->hashmap ? "hashmap" : "hashtable",
                     options->threads, options->ops, options->keys, size, mix,
                     options->migrate ? "yes" : "no",
                     options->verify ? "yes" : "no");
   }

   failed = 0;
   start  = mulle_relativetime_now();

   enumerated_entries = options->enumerate ? 1 : 0;
   for( i = 0; i < options->threads; i++)
   {
      workers[ i].map        = map;
      workers[ i].has_remove = has_remove;
      workers[ i].index      = (unsigned int) i;
      workers[ i].threads    = options->threads;
      workers[ i].ops        = options->ops;
      workers[ i].keys       = options->keys;
      workers[ i].seed       = (unsigned int) options->seed * 2654435761U + (unsigned int) i * 40503U + 1U;
      workers[ i].enumerate  = 0;

      if( mulle_thread_create( hs_churn_main, &workers[ i], &handles[ i]))
      {
         mulle_fprintf( stderr, "hashspace: could not start thread %ld\n", i);
         failed = 1;
         break;
      }
   }

   if( options->migrate)
   {
      migrator.map = &hashtable;
      if( mulle_thread_create( hs_migrate_main, &migrator, &migrate_handle))
         failed = 1;
   }

   if( enumerated_entries)
   {
      enumerator.map   = map;
      enumerator.ops   = options->ops;
      enumerator.keys  = options->keys;
      enumerator.seed  = (unsigned int) options->seed + 0x5eed;
      enumerator.has_remove = has_remove;
      if( mulle_thread_create( hs_enumerate_main, &enumerator, &enumerate_handle))
         failed = 1;
   }

   for( i = 0; i < options->threads; i++)
      (void) mulle_thread_join( handles[ i]);

   if( options->migrate)
   {
      _mulle_atomic_pointer_nonatomic_write( &migrator.stop, (void *) (intptr_t) 1);
      (void) mulle_thread_join( migrate_handle);
   }

   if( enumerated_entries)
      (void) mulle_thread_join( enumerate_handle);

   wall = mulle_relativetime_now() - start;

   for( i = 0; i < options->threads; i++)
   {
      ops_done += workers[ i].ops_done;
      insert_attempts += workers[ i].insert_attempts;
      remove_attempts += workers[ i].remove_attempts;
      inserted += workers[ i].inserted;
      eexist   += workers[ i].eexist;
      lookups  += workers[ i].lookups;
      hits     += workers[ i].hits;
      misses   += workers[ i].misses;
      removed  += workers[ i].removed;
      enoent   += workers[ i].enoent;
      einval   += workers[ i].einval;
      corrupt  += workers[ i].corrupt;
      hs_latency_merge( &total, &workers[ i].latency);
   }

   enumerated = enumerator.enumerated;
   walks      = enumerator.walks;
   restarts   = enumerator.restarts;
   corrupt   += enumerator.corrupt;
   migrations = migrator.migrations;

   /*
    * Every insert either lands or is refused with EEXIST, and every remove
    * either removes or is refused with ENOENT. Those identities are what makes a
    * silently lost operation visible: refusals are counted, so they cannot be
    * dropped, and no call returns a third answer. The mix accounts for every
    * operation the threads started, so nothing can go unaccounted either.
    */
   if( inserted + eexist + einval != insert_attempts)
      failed = 1;
   if( removed + enoent != remove_attempts)
      failed = 1;
   if( insert_attempts + lookups + remove_attempts != ops_done)
      failed = 1;

   table   = hs_map_size( &map);
   live    = hs_map_count( &map);
   drained = 0;

   /*
    * The size was chosen so that every key fits, so if the table had to grow
    * the run measured the wrong thing: growth is --mask's subject, not this
    * mode's.
    */
   if( table != size)
      failed = 1;

   if( options->verify)
   {
      struct hs_rover   rover;
      intptr_t          hash;
      void              *value;
      int               rval;

      /*
       * Single threaded now. Walk what is left: every entry must carry the
       * value derived from its own hash, and draining it must empty the map. A
       * remove that was silently undone (the race the hashtable's design exists
       * to prevent) shows up here as an entry surviving a remove. The hashmap's
       * remove is single threaded by definition, so this is the only way it can
       * be used at all.
       */
      rover = hs_rover_create( &map);
      for(;;)
      {
         rval = hs_rover_next( &rover, &hash, &value);
         if( rval != 1)
            break;
         if( value != hs_value_for( (long) hash))
         {
            corrupt++;
            continue;
         }
         if( hs_map_remove_single_threaded( &map, hash, value))
         {
            corrupt++;
            continue;
         }
         drained++;
      }

      if( hs_map_count( &map))
         failed = 1;             /* something survived a remove */
   }

   if( options->hashmap)
      mulle_concurrent_hashmap_done( &hashmap);
   else
      mulle_concurrent_hashtable_done( &hashtable);

   if( options->quiet)
   {
      mulle_fprintf( stdout, "churn.map=%s churn.mix=%s churn.slots=%zu\n",
                     options->hashmap ? "hashmap" : "hashtable", mix, table);
      mulle_fprintf( stdout,
         "churn.threads=%ld churn.table=%zu churn.ops=%llu churn.wall=%.6f churn.ops_per_sec=%.0f\n",
         options->threads, size, ops_done, wall,
         wall > 0.0 ? (double) ops_done / wall : 0.0);
      mulle_fprintf( stdout,
         "churn.insert_attempts=%llu churn.inserted=%llu churn.eexist=%llu churn.einval=%llu\n",
         insert_attempts, inserted, eexist, einval);
      mulle_fprintf( stdout,
         "churn.remove_attempts=%llu churn.removed=%llu churn.enoent=%llu\n",
         remove_attempts, removed, enoent);
      mulle_fprintf( stdout,
         "churn.lookups=%llu churn.hits=%llu churn.misses=%llu\n",
         lookups, hits, misses);
      mulle_fprintf( stdout,
         "churn.p50_ns=%llu churn.p99_ns=%llu churn.p999_ns=%llu churn.max_ns=%llu\n",
         hs_percentile( &total, 0.50),
         hs_percentile( &total, 0.99),
         hs_percentile( &total, 0.999),
         total.max_ns);
      mulle_fprintf( stdout, "churn.migrations=%llu churn.walks=%llu churn.restarts=%llu churn.enumerated=%llu\n",
                     migrations, walks, restarts, enumerated);
      if( options->verify)
         mulle_fprintf( stdout, "churn.live=%zu churn.drained=%ld\n", live, drained);
      mulle_fprintf( stdout, "churn.corrupt=%llu churn.failed=%d\n", corrupt, failed);
   }
   else
   {
      mulle_fprintf( stdout,
         "\n"
         "  map          %s, mix %s\n"
         "  table        %zu slots%s\n"
         "  wall         %.3f ms  (%.0f ops/s over %llu operations)\n"
         "  insert       %llu landed, %llu EEXIST, %llu EINVAL\n"
         "  lookup       %llu, %llu hit, %llu miss\n",
         options->hashmap ? "hashmap" : "hashtable", mix,
         table, table == size ? "" : "   <- it grew, so it migrated",
         wall * 1000.0, wall > 0.0 ? (double) ops_done / wall : 0.0, ops_done,
         inserted, eexist, einval,
         lookups, hits, misses);

      if( has_remove)
         mulle_fprintf( stdout,
            "  remove       %llu removed, %llu ENOENT (another thread got there first)\n",
            removed, enoent);
      else
         mulle_fprintf( stdout,
            "  remove       none: this mix has no removes\n");

      mulle_fprintf( stdout,
         "  latency      p50 %llu ns   p99 %llu ns   p99.9 %llu ns   max %llu ns\n",
         hs_percentile( &total, 0.50),
         hs_percentile( &total, 0.99),
         hs_percentile( &total, 0.999),
         total.max_ns);

      if( options->migrate || options->enumerate)
      {
         mulle_fprintf( stdout, "  migration    %llu forced generations\n", migrations);
         if( options->enumerate)
            mulle_fprintf( stdout,
               "  enumeration  %llu walks, %llu cancelled with ECANCELED, %llu entries seen\n"
               "               a cancel is the documented answer to a walk that crossed a\n"
               "               generation change, or reached a slot frozen by a copy in\n"
               "               flight: the walk restarts, it does not skip or duplicate\n",
               walks, restarts, enumerated);
      }

      if( options->verify)
         mulle_fprintf( stdout,
            "  contents     %zu live, %ld drained by the verification walk\n",
            live, drained);

      mulle_fprintf( stdout, "  bad values   %llu%s\n", corrupt,
                     corrupt ? "   <- a slot under the wrong value" : "");

      if( has_remove)
         mulle_fprintf( stdout,
            "  by design    EEXIST and ENOENT are the answers, not waits: there is\n"
            "               no lock to take and no thread to block behind.\n");
      else
         mulle_fprintf( stdout,
            "  by design    the hashtable removes a hash concurrently and the slot is\n"
            "               reusable at once. The hashmap cannot: patch and remove\n"
            "               are single threaded writes. That is what keeping the full\n"
            "               hash width costs, and why this mix has no removes at all\n"
            "               instead of a third of the operations missing.\n");

      if( options->hashmap)
         mulle_fprintf( stdout,
            "  comparable   the same mix and the same workload against the hashtable:\n"
            "                 hashspace --churn --no-remove --threads %ld --ops %ld --keys %ld\n",
            options->threads, options->ops, options->keys);
      else
         if( options->no_remove)
            mulle_fprintf( stdout,
               "  comparable   --no-remove is the hashmap's mix, so this run and a\n"
               "               --map hashmap run with the same --seed, --keys and\n"
               "               --ops start from the same operations.\n");
   }

   return( failed ? 1 : 0);
}


//
// --mask: the size is a hint
//
// The probe index is `i & (size - 1)`, so a size that is not a power of two
// would make that walk cover a fraction of the table. Count that fraction for
// the size *asked for*, then run both a good size and a bad one in a child
// process with a deadline: the bad one comes back, and the child reports the
// size its table used next to the size it was handed.
//
// The deadline stays because the failure this mode watches for is not an error
// to report, it is not coming back -- and the storage allocator's rounding is
// what stops that happening for these rows.
//
// The child is a real process, started through mulle-spawn, and that is what
// makes this work everywhere: see `hs_run_bounded_child` below.
//

static unsigned int   hs_reachable_slots( unsigned int size)
{
   unsigned char   *seen;
   unsigned int    mask;
   unsigned int    i;
   unsigned int    distinct;

   mask     = size - 1;
   distinct = 0;
   seen     = calloc( size, 1);
   if( ! seen)
      return( 0);

   for( i = 0; i < size; i++)
   {
      if( seen[ i & mask])
         continue;
      seen[ i & mask] = 1;
      distinct++;
   }
   free( seen);
   return( distinct);
}


/*
 * One mechanism for both of the child modes.
 *
 * A child reports on its stdout, so a child that finishes hands back a result
 * and not just a yes: --mask's children return the size their table ended up
 * using, which is the evidence for the migration. A child that aborts or
 * livelocks writes nothing, and "it did not come back" is the verdict that
 * matters. The exit status is only ever a suffix.
 *
 * Output and survival are separate answers, and both are kept: a child that
 * printed its report before it stopped coming back still leaves that report
 * here. mulle-spawn is what holds the child to the deadline -- which is why a
 * child does not have to be well behaved to be reported on, and why it cannot
 * hang the demo on the way out either.
 */
struct hs_verdict
{
   int    completed;
   char   text[ 96];
};


/*
 * A child is a new process, not a copy of this one, so it cannot be handed a
 * pointer to an argument struct: the few integers a mode needs travel as words
 * on the command line, in the order the struct declares them.
 * `hs_child_values` writes them and `hs_internal_child` reads them back. They
 * are deliberately next to each other, and they are the only place the order
 * matters.
 */
#define HS_CHILD_DEADLINE      1.0
#define HS_CHILD_MAX_VALUES    4
#define HS_CHILD_VALUE_LEN     24


struct hs_mask_arg
{
   unsigned int   size;
   int            entries;
   int            lookup_absent;
   int            use_hashmap;
};


struct hs_universe_arg
{
   int   entries;
   int   do_init;
};


/* argv needs mutable pointers, and these are only ever read */
static char   hs_child_flag[]          = "--internal-child";
static char   hs_mask_child_name[]     = "mask";
static char   hs_universe_child_name[] = "universe";


static int   hs_mask_child( void *arg, char *report, size_t len);
static int   hs_universe_child( void *arg, char *report, size_t len);


/*
 * What a child hands back: the size its table is using now, and the one it
 * started at -- because a table that grew is a table that migrated and retired
 * storage, which is exactly what a process without the reclaimer dies of.
 */
static void   hs_report_size( char *report, size_t len, size_t from, size_t to)
{
   if( from == to)
      mulle_snprintf( report, len, "completed, table %zu", to);
   else
      mulle_snprintf( report, len, "completed, table %zu -> %zu", from, to);
}


static char   *hs_child_name( int (*child_main)( void *, char *, size_t))
{
   if( child_main == hs_mask_child)
      return( hs_mask_child_name);
   if( child_main == hs_universe_child)
      return( hs_universe_child_name);
   return( NULL);
}


/* the parent's half of the marshalling, see the comment above */
static int   hs_child_values( int (*child_main)( void *, char *, size_t),
                              void *arg,
                              char value[][ HS_CHILD_VALUE_LEN])
{
   if( child_main == hs_mask_child)
   {
      struct hs_mask_arg   *mask_arg;

      mask_arg = arg;
      mulle_snprintf( value[ 0], HS_CHILD_VALUE_LEN, "%u", mask_arg->size);
      mulle_snprintf( value[ 1], HS_CHILD_VALUE_LEN, "%d", mask_arg->entries);
      mulle_snprintf( value[ 2], HS_CHILD_VALUE_LEN, "%d", mask_arg->lookup_absent);
      mulle_snprintf( value[ 3], HS_CHILD_VALUE_LEN, "%d", mask_arg->use_hashmap);
      return( 4);
   }

   if( child_main == hs_universe_child)
   {
      struct hs_universe_arg   *universe_arg;

      universe_arg = arg;
      mulle_snprintf( value[ 0], HS_CHILD_VALUE_LEN, "%d", universe_arg->entries);
      mulle_snprintf( value[ 1], HS_CHILD_VALUE_LEN, "%d", universe_arg->do_init);
      return( 2);
   }

   return( 0);
}


/*
 * Turn what a child left behind into a verdict. The report is what it printed,
 * minus the newline it printed it with, because the verdict is embedded in a
 * line of ours.
 */
static void   hs_verdict_from( struct mulle_spawn_result *result,
                               struct hs_verdict *verdict)
{
   size_t   length;

   verdict->completed = 0;
   verdict->text[ 0]  = 0;

   if( result->status == mulle_spawn_failed)
   {
      mulle_snprintf( verdict->text, sizeof( verdict->text),
                      "did not complete (cannot run this program again)");
      return;
   }

   length = result->length;
   while( length && (result->output[ length - 1] == '\n' ||
                     result->output[ length - 1] == '\r'))
      length--;

   if( length)
   {
      result->output[ length] = 0;
      mulle_snprintf( verdict->text, sizeof( verdict->text), "%s", result->output);
      verdict->completed = 1;
      return;
   }

   switch( result->status)
   {
   case mulle_spawn_terminated:
      mulle_snprintf( verdict->text, sizeof( verdict->text),
                      "did not complete (killed after %.0fs)", HS_CHILD_DEADLINE);
      break;

   case mulle_spawn_signalled:
      mulle_snprintf( verdict->text, sizeof( verdict->text),
                      "did not complete (signal %d)", result->signal_number);
      break;

   case mulle_spawn_exited:
      mulle_snprintf( verdict->text, sizeof( verdict->text),
                      "did not complete (exit %d)", result->exit_code);
      break;

   default:
      mulle_snprintf( verdict->text, sizeof( verdict->text), "did not complete");
      break;
   }
}


static void   hs_run_bounded_child( int (*child_main)( void *, char *, size_t),
                                    void *arg,
                                    struct hs_verdict *verdict)
{
   struct mulle_spawn_result   result;
   char                        value[ HS_CHILD_MAX_VALUES][ HS_CHILD_VALUE_LEN];
   // program, flag, mode, the values, and the NULL: one slot more than values
   char                        *argv[ HS_CHILD_MAX_VALUES + 4];
   char                        *name;
   char                        self[ 4096];
   int                         i;
   int                         nvalues;

   name = hs_child_name( child_main);
   if( ! name || mulle_spawn_get_executable_path( self, sizeof( self)) < 0)
   {
      verdict->completed = 0;
      mulle_snprintf( verdict->text, sizeof( verdict->text),
                      "did not complete (cannot run this program again)");
      return;
   }

   nvalues = hs_child_values( child_main, arg, value);
   if( nvalues > HS_CHILD_MAX_VALUES)
      nvalues = HS_CHILD_MAX_VALUES;

   argv[ 0] = self;
   argv[ 1] = hs_child_flag;
   argv[ 2] = name;
   for( i = 0; i < nvalues; i++)
      argv[ 3 + i] = value[ i];
   argv[ 3 + nvalues] = NULL;

   mulle_spawn_result_init( &result, NULL);
   (void) mulle_spawn_run( argv, HS_CHILD_DEADLINE, &result);

   hs_verdict_from( &result, verdict);
   mulle_spawn_result_done( &result);
}


static int   hs_mask_child( void *arg, char *report, size_t len)
{
   struct hs_mask_arg   *mask_arg;
   int                  i;
   int                  rval;

   mask_arg = arg;

   if( mask_arg->use_hashmap)
   {
      struct mulle_concurrent_hashmap   map;

      mulle_concurrent_hashmap_init( &map, mask_arg->size, NULL);
      for( i = 0; i < mask_arg->entries; i++)
      {
         rval = mulle_concurrent_hashmap_insert( &map, (intptr_t) i + 1,
                                                 hs_value_for( (long) i + 1));
         if( rval)
            return( 3);
      }
      hs_report_size( report, len, mask_arg->size,
                      (size_t) mulle_concurrent_hashmap_get_size( &map));
      mulle_concurrent_hashmap_done( &map);
      return( 0);
   }

   {
      struct mulle_concurrent_hashtable   map;

      mulle_concurrent_hashtable_init_positive( &map, mask_arg->size, NULL);
      for( i = 0; i < mask_arg->entries; i++)
      {
         rval = mulle_concurrent_hashtable_insert( &map, (intptr_t) i + 1,
                                                   hs_value_for( (long) i + 1));
         if( rval)
            return( 3);
      }

      /*
       * The lookup stays in the run because it is the sharpest of the four probe
       * paths: its only escape from a probe that runs out is
       * `assert( index != sentinel)`, where the insert has EBUSY and a caller
       * behind it. With the size rounded up the probe cannot run out, so this row
       * is now evidence that the lookup comes back. That is why it is still run:
       * it is the row that would notice first if the rounding were ever dropped.
       * The report goes after it either way -- a lookup that did not return writes
       * nothing, which is what the parent would then report.
       */
      if( mask_arg->lookup_absent)
         (void) mulle_concurrent_hashtable_lookup( &map, (intptr_t) mask_arg->entries + 100000);

      hs_report_size( report, len, mask_arg->size,
                      mulle_concurrent_hashtable_get_size( &map));

      mulle_concurrent_hashtable_done( &map);
   }
   return( 0);
}


static int   hs_mask( const struct hs_options *options)
{
   unsigned int        good     = 2048;
   unsigned int        bad      = 2064;
   unsigned int        good_n   = hs_reachable_slots( good);
   unsigned int        bad_n    = hs_reachable_slots( bad);
   struct hs_verdict   hash_pow2;
   struct hs_verdict   hash_lookup;
   struct hs_verdict   hash_insert;
   struct hs_verdict   map_pow2;
   struct hs_verdict   map_insert;
   struct hs_mask_arg  arg;
   int                 entries  = 64;

   memset( &hash_pow2,   0, sizeof( hash_pow2));
   memset( &hash_lookup, 0, sizeof( hash_lookup));
   memset( &hash_insert, 0, sizeof( hash_insert));
   memset( &map_pow2,    0, sizeof( map_pow2));
   memset( &map_insert,  0, sizeof( map_insert));

   /*
    * Five runs, because the two maps do not fail the same way. The hashtable's
    * insert returns EBUSY when its probe runs out, which the caller answers with
    * a migration, so its inserts survive a bad size by doubling the allocation.
    * Its lookup has no such escape, and the hashmap has none anywhere.
    */
   arg.entries       = entries;
   arg.lookup_absent = 1;
   arg.use_hashmap   = 0;
   arg.size          = good;
   hs_run_bounded_child( hs_mask_child, &arg, &hash_pow2);

   arg.size          = bad;
   hs_run_bounded_child( hs_mask_child, &arg, &hash_lookup);

   arg.lookup_absent = 0;
   hs_run_bounded_child( hs_mask_child, &arg, &hash_insert);

   arg.use_hashmap   = 1;
   hs_run_bounded_child( hs_mask_child, &arg, &map_insert);

   arg.size          = good;
   hs_run_bounded_child( hs_mask_child, &arg, &map_pow2);

   if( options->quiet)
   {
      mulle_fprintf( stdout, "mask.pow2=%u mask.pow2_reachable=%u mask.hashtable_pow2=%s\n",
                     good, good_n, hash_pow2.text);
      mulle_fprintf( stdout, "mask.nonpow2=%u mask.nonpow2_reachable=%u mask.hashtable_nonpow2_lookup=%s\n",
                     bad, bad_n, hash_lookup.text);
      mulle_fprintf( stdout, "mask.hashtable_nonpow2_insert=%s\n", hash_insert.text);
      mulle_fprintf( stdout, "mask.hashmap_nonpow2_insert=%s\n", map_insert.text);
      mulle_fprintf( stdout, "mask.hashmap_pow2=%s\n", map_pow2.text);
   }
   else
   {
      mulle_fprintf( stdout, "=== size and mask ===\n");
      mulle_fprintf( stdout, "%-10s %-16s %-20s %s\n",
                     "map", "reachable slots", "work", "result");
      mulle_fprintf( stdout, "%-10s %-16s %-20s %s\n",
                     "hashtable", "2048 of 2048", "64 insert + 1 lookup", hash_pow2.text);
      mulle_fprintf( stdout, "%-10s %-16s %-20s %s\n",
                     "hashtable", "32 of 2064", "64 insert + 1 lookup", hash_lookup.text);
      mulle_fprintf( stdout, "%-10s %-16s %-20s %s\n",
                     "hashtable", "32 of 2064", "64 insert", hash_insert.text);
      mulle_fprintf( stdout, "%-10s %-16s %-20s %s\n",
                     "hashmap", "32 of 2064", "64 insert", map_insert.text);
      mulle_fprintf( stdout, "%-10s %-16s %-20s %s\n",
                     "hashmap", "2048 of 2048", "64 insert", map_pow2.text);

      mulle_fprintf( stdout,
         "\n"
         "  The first three rows are the hashtable, the last two the hashmap. The\n"
         "  probe index is `i & (size - 1)`, and %u - 1 is 0x%x: that keeps bits\n"
         "  0-3 and bit 11, so a probe would walk %u of the %u slots and never\n"
         "  meet a virgin slot.\n"
         "\n"
         "  None of them does, because the size is a hint. The storage allocator\n"
         "  rounds it up to the next power of two before it derives the mask --\n"
         "  _mulle_concurrent_pow2roundup in mulle-concurrent-math.h, called from\n"
         "  the storage alloc of both maps -- so %u becomes %u, `mask = n - 1`\n"
         "  covers every slot, and the probe terminates. The size the table used\n"
         "  is what the last column reports, which is the rounding's evidence.\n"
         "\n"
         "  The rounding is not an assert, so NDEBUG does not reach it: a release\n"
         "  build gets the table a debug build gets, and every row comes back.\n",
         bad, bad - 1, bad_n, bad, bad,
         (unsigned int) hs_next_power_of_two( bad));

      mulle_fprintf( stdout,
         "\n"
         "  The last column is what each child handed back on its stdout, and\n"
         "  not a yes: the size its table ended up using. A row that did not come\n"
         "  back would have no size to report, and the deadline is what would say\n"
         "  so -- which is why these rows run in a process of their own instead\n"
         "  of in the one printing this.\n");
   }

   return( hash_pow2.completed ? 0 : 1);
}


//
// --universe: the reclaimer requirement
//

/*
 * The child deliberately does *not* call mulle_aba_init() in the first two
 * cases. Retiring storage needs a reclaimer, and without one the allocator
 * aborts -- but only once something is actually retired, which is why a small
 * map survives and a growing one does not.
 */
static int   hs_universe_child( void *arg, char *report, size_t len)
{
   struct hs_universe_arg              *universe_arg;
   struct mulle_concurrent_hashtable   map;
   int                                 i;

   universe_arg = arg;

   if( universe_arg->do_init)
   {
      mulle_aba_init( NULL);
      mulle_aba_register();
   }

   mulle_concurrent_hashtable_init_positive( &map, 4, NULL);
   for( i = 0; i < universe_arg->entries; i++)
      (void) mulle_concurrent_hashtable_insert( &map, (intptr_t) i + 1,
                                                hs_value_for( (long) i + 1));

   /* the size says whether anything was ever retired at all */
   hs_report_size( report, len, 4, mulle_concurrent_hashtable_get_size( &map));

   mulle_concurrent_hashtable_done( &map);

   if( universe_arg->do_init)
   {
      mulle_aba_unregister();
      mulle_aba_done();
   }
   return( 0);
}


static int   hs_universe( const struct hs_options *options)
{
   struct hs_verdict      small;
   struct hs_verdict      grown;
   struct hs_verdict      with;
   struct hs_universe_arg arg;

   memset( &small, 0, sizeof( small));
   memset( &grown, 0, sizeof( grown));
   memset( &with, 0, sizeof( with));

   if( ! options->quiet)
      mulle_fprintf( stdout, "=== mulle_aba_init() ===\n");

   arg.entries = 2;
   arg.do_init = 0;
   hs_run_bounded_child( hs_universe_child, &arg, &small);
   if( ! options->quiet)
      mulle_fprintf( stdout, "  %2d inserts, no mulle_aba_init()   -> %s\n", arg.entries, small.text);
   mulle_fprintf( stdout, "universe.small=%s\n", small.text);

   arg.entries = 40;
   arg.do_init = 0;
   hs_run_bounded_child( hs_universe_child, &arg, &grown);
   if( ! options->quiet)
      mulle_fprintf( stdout, "  %2d inserts, no mulle_aba_init()   -> %s\n", arg.entries, grown.text);
   mulle_fprintf( stdout, "universe.grown=%s\n", grown.text);

   arg.entries = 40;
   arg.do_init = 1;
   hs_run_bounded_child( hs_universe_child, &arg, &with);
   if( ! options->quiet)
      mulle_fprintf( stdout, "  %2d inserts, with mulle_aba_init() -> %s\n", arg.entries, with.text);
   mulle_fprintf( stdout, "universe.with_init=%s\n", with.text);

   if( ! options->quiet)
      mulle_fprintf( stdout,
         "\n"
         "  A concurrent container retires storage when it migrates, and retiring\n"
         "  storage calls the allocator's ABA reclaimer. Without one, the allocator\n"
         "  explains and aborts. The catch is *when*: a map that never migrates\n"
         "  never retires anything, so a small test passes and the same program\n"
         "  dies once it grows. The reported sizes are where the difference is:\n"
         "  the small case still has its original table, so it never retired, and\n"
         "  it is exactly the one that survives. mulle_aba_init( NULL) once at\n"
         "  startup, and mulle_aba_register() once per thread that touches the\n"
         "  container.\n");

   return( small.completed ? 0 : 1);
}


/*
 * The child side of `hs_run_bounded_child`.
 *
 * It is reached by running this program again, so it starts from nothing: no
 * inherited allocator state, no inherited reclaimer, and no arguments other
 * than the words on its command line. That is the point -- these experiments
 * are about a process that forgot to initialize something, and a child that
 * inherited the parent's initialization could not show that.
 *
 * It prints its report on stdout and nothing else, because stdout is the
 * channel the parent reads. Explanations belong on stderr, where the parent is
 * not listening.
 */
static int   hs_internal_child( int argc, char **argv)
{
   int                      (*child_main)( void *, char *, size_t);
   struct hs_mask_arg       mask_arg;
   char                     report[ 96];
   struct hs_universe_arg   universe_arg;
   void                     *arg;

   arg        = NULL;
   child_main = NULL;

   if( argc == 7 && ! strcmp( argv[ 2], hs_mask_child_name))
   {
      memset( &mask_arg, 0, sizeof( mask_arg));
      mask_arg.size          = (unsigned int) atoi( argv[ 3]);
      mask_arg.entries       = atoi( argv[ 4]);
      mask_arg.lookup_absent = atoi( argv[ 5]);
      mask_arg.use_hashmap   = atoi( argv[ 6]);

      arg        = &mask_arg;
      child_main = hs_mask_child;
   }
   else
      if( argc == 5 && ! strcmp( argv[ 2], hs_universe_child_name))
      {
         memset( &universe_arg, 0, sizeof( universe_arg));
         universe_arg.entries = atoi( argv[ 3]);
         universe_arg.do_init = atoi( argv[ 4]);

         arg        = &universe_arg;
         child_main = hs_universe_child;
      }

   if( ! child_main)
      return( 1);

   /*
    * A mask child migrates, and migrating retires storage, so it wants a
    * reclaimer exactly like any other consumer would. A universe child is the
    * opposite experiment: it is told whether to have one, and the case that
    * matters is the process that forgot.
    */
   if( child_main == hs_mask_child)
   {
      mulle_aba_init( NULL);
      mulle_aba_register();
   }

   report[ 0] = 0;
   (void) child_main( arg, report, sizeof( report));

   if( child_main == hs_mask_child)
   {
      mulle_aba_unregister();
      mulle_aba_done();
   }

   if( report[ 0])
      mulle_fprintf( stdout, "%s\n", report);
   return( 0);
}


//
// main
//

int   main( int argc, char **argv)
{
   struct hs_options   options;
   int                 i;
   int                 mode_space      = 0;
   int                 mode_replace    = 0;
   int                 mode_churn      = 0;
   int                 mode_mask       = 0;
   int                 mode_universe   = 0;
   int                 failed          = 0;
   int                 any_mode        = 0;

   /*
    * A child of ours, run again as a fresh process. This must be the very first
    * thing that happens: everything below initializes state that a child is not
    * supposed to have.
    */
   if( argc > 2 && ! strcmp( argv[ 1], hs_child_flag))
      return( hs_internal_child( argc, argv));

   memset( &options, 0, sizeof( options));
   options.threads = HS_DEFAULT_THREADS;
   options.ops     = HS_DEFAULT_OPS;
   options.keys    = HS_DEFAULT_KEYS;
   options.seed    = 42;

   for( i = 1; i < argc; i++)
   {
      if( ! strcmp( argv[ i], "--space"))
      {
         mode_space = any_mode = 1;
      }
      else
         if( ! strcmp( argv[ i], "--replace"))
         {
            mode_replace = any_mode = 1;
         }
         else
            if( ! strcmp( argv[ i], "--churn"))
            {
               mode_churn = any_mode = 1;
            }
            else
               if( ! strcmp( argv[ i], "--universe"))
               {
                  mode_universe = any_mode = 1;
               }
               else
                  if( ! strcmp( argv[ i], "--mask"))
                  {
                     mode_mask = any_mode = 1;
                  }
                  else
                     if( ! strcmp( argv[ i], "--all"))
                     {
                        mode_space = mode_replace = mode_churn = 1;
                        mode_mask  = mode_universe = 1;
                        any_mode   = 1;
                     }
                     else
                        if( ! strcmp( argv[ i], "--migrate"))
                           options.migrate = 1;
                        else
                           if( ! strcmp( argv[ i], "--verify"))
                              options.verify = 1;
                           else
                              if( ! strcmp( argv[ i], "--enumerate"))
                                 options.enumerate = 1;
                              else
                                 if( ! strcmp( argv[ i], "--quiet"))
                                    options.quiet = 1;
                                 else
                                    if( ! strcmp( argv[ i], "--no-remove"))
                                       options.no_remove = 1;
                                    else
                                       if( ! strcmp( argv[ i], "--map") && i + 1 < argc)
                                       {
                                          i++;
                                          if( ! strcmp( argv[ i], "hashmap"))
                                             options.hashmap = 1;
                                          else
                                             if( ! strcmp( argv[ i], "hashtable"))
                                                options.hashmap = 0;
                                             else
                                             {
                                                mulle_fprintf( stderr, "hashspace: unknown map %s\n",
                                                               argv[ i]);
                                                hs_usage( stderr);
                                                return( 1);
                                             }
                                       }
                                       else
                                          if( ! strcmp( argv[ i], "--threads") && i + 1 < argc)
                                             options.threads = atol( argv[ ++i]);
                                          else
                                             if( ! strcmp( argv[ i], "--ops") && i + 1 < argc)
                                                options.ops = atol( argv[ ++i]);
                                             else
                                                if( ! strcmp( argv[ i], "--keys") && i + 1 < argc)
                                                   options.keys = atol( argv[ ++i]);
                                                else
                                                   if( ! strcmp( argv[ i], "--seed") && i + 1 < argc)
                                                      options.seed = atoi( argv[ ++i]);
                                                   else
                                                      if( ! strcmp( argv[ i], "--help"))
                                                      {
                                                         hs_usage( stdout);
                                                         return( 0);
                                                      }
                                                      else
                                                      {
                                                         mulle_fprintf( stderr, "hashspace: unknown option %s\n",
                                                                        argv[ i]);
                                                         hs_usage( stderr);
                                                         return( 1);
                                                      }
   }

   if( options.threads < 1)
      options.threads = 1;
   if( options.threads > HS_MAX_THREADS)
      options.threads = HS_MAX_THREADS;
   if( options.ops < 1)
      options.ops = 1;
   if( options.keys < 1)
      options.keys = 1;

   /*
    * There is no _mulle_concurrent_hashmap_migrate_same_size: the hashmap has no
    * concurrent remove to make a generation interesting, so this is refused
    * rather than quietly ignored.
    */
   if( options.hashmap && options.migrate)
   {
      mulle_fprintf( stderr, "hashspace: --migrate needs the hashtable: "
                             "there is no hashmap migrate_same_size\n");
      return( 1);
   }

   if( ! any_mode)
      mode_space = 1;

   /*
    * The universe first: it is the mode whose children must *not* have an
    * initialized reclaimer, and although a child is now a separate process that
    * could not inherit one anyway, the runs that start from nothing still
    * belong before anything here has touched a container.
    */
   if( mode_universe)
   {
      failed |= hs_universe( &options);
      if( mode_space || mode_replace || mode_churn || mode_mask)
         mulle_fprintf( stdout, "\n");
   }

   /*
    * Everything else runs inside an initialized universe, as a consumer would
    * do it: once, before the container is touched.
    */
   if( mode_space || mode_replace || mode_churn || mode_mask)
   {
      mulle_aba_init( NULL);
      mulle_aba_register();
   }

   if( mode_space)
   {
      hs_dump_space( &options);
      if( mode_replace || mode_churn)
         mulle_fprintf( stdout, "\n");
   }

   if( mode_replace)
   {
      hs_dump_replace( &options);
      if( mode_churn || mode_mask)
         mulle_fprintf( stdout, "\n");
   }

   if( mode_churn)
   {
      failed |= hs_churn( &options);
      if( mode_mask)
         mulle_fprintf( stdout, "\n");
   }

   if( mode_mask)
      failed |= hs_mask( &options);

   if( mode_space || mode_replace || mode_churn || mode_mask)
   {
      mulle_aba_unregister();
      mulle_aba_done();
   }

   return( failed ? 1 : 0);
}
