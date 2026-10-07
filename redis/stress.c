//
//  stress.c
//  miniredis -- evidence, not vibes
//
//  What this proves:
//
//   --concurrency --naive   SET spelled the obvious way (remove, then insert)
//                           lets a concurrent GET observe a key that was never
//                           deleted as missing. This is the measurement behind
//                           wontfix/11-no-atomic-replace.md -- declined, because
//                           remove + register is the ceiling of a wait-free table.
//
//   --concurrency           the same workload against the real keyspace, which
//                           swaps an entry payload with a CAS. Expects zero.
//
//   --collision             the table keys on the hash alone: it holds one
//                           entry per hash, so two keys with the *same* hash are
//                           one key to it. Narrows the hash to 8 bits, which
//                           makes that certain rather than unlikely, and checks
//                           the keyspace's own key comparison -- the layer that
//                           keeps two Redis keys apart.
//
//   --reclaim               replaced values are actually freed once no reader
//                           is inside an epoch -- worth testing, because
//                           getting reclamation wrong means a use-after-free
//                           rather than a wrong answer.
//
//   --script FILE [--tcp]   drives a real server (a --stdio child, or one
//                           spawned on an ephemeral port) and prints decoded
//                           replies. This is also the RESP client the test
//                           suite uses, so it needs no redis-cli, nc or python.
//
//   --bench                 see bench.c
//

#include "store.h"
#include "client.h"
#include "bench.h"

#include <mulle-aba/mulle-aba.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if MREDIS_HAVE_POSIX
# include <sys/wait.h>
# include <unistd.h>
# include <signal.h>
#endif


#define MREDIS_TEST_KEY        "k"
#define MREDIS_TEST_KEY_LENGTH 1

#define MODE_ENTRY  0
#define MODE_NAIVE  1


//
// output. Numbers are formatted by hand and printed as %s, because the mulle
// formatter's conversion table is not documented.
//
/* appends decimal digits at *p and returns the new end */
static char   *append_uint( char *p, uint64_t value)
{
   char     tmp[ 20];
   size_t   i;

   i = sizeof( tmp);
   do
   {
      tmp[ --i] = (char) ('0' + (value % 10));
      value    /= 10;
   }
   while( value);

   while( i < sizeof( tmp))
      *p++ = tmp[ i++];
   return( p);
}


static void   add_uint( struct mulle_buffer *buffer, uint64_t value)
{
   char     tmp[ 20];
   size_t   length;

   length = (size_t) (append_uint( tmp, value) - tmp);
   mulle_buffer_add_bytes( buffer, tmp, length);
}


static void   add_text( struct mulle_buffer *buffer, const char *text)
{
   mulle_buffer_add_bytes( buffer, text, strlen( text));
}


static void   add_pair( struct mulle_buffer *buffer, const char *key, uint64_t value)
{
   add_text( buffer, key);
   add_uint( buffer, value);
}


static void   add_pair_signed( struct mulle_buffer *buffer, const char *key, long value)
{
   add_text( buffer, key);
   if( value < 0)
   {
      mulle_buffer_add_bytes( buffer, "-", 1);
      add_uint( buffer, (uint64_t) (-(value + 1)) + 1);
      return;
   }
   add_uint( buffer, (uint64_t) value);
}


/*
 * mulle_buffer_get_string() is how you get a C string out of a buffer, and it
 * is a mutating call by design: it appends the terminator (a no-op if the last
 * byte already is one), so the length afterwards includes that byte. Fine here,
 * because a frame is complete before it is printed and the buffer is reset
 * afterwards -- but do not use it half way through building one.
 */
static void   print_buffer( struct mulle_buffer *buffer)
{
   char   *text;

   text = mulle_buffer_get_string( buffer);
   if( text)
      mulle_fprintf( stdout, "%s", text);
}


/*
 * Not named atomic_long(): <stdatomic.h>, which mulle-atomic-c11.h includes,
 * defines a type by that name.
 */
static long   load_counter( mulle_atomic_pointer_t *p)
{
   return( (long) (intptr_t) _mulle_atomic_pointer_read( p));
}


//
// the naive keyspace: what SET looks like without an atomic replace
//
struct naive_value
{
   size_t   length;
   char     bytes[ 1];
};


struct naive_store
{
   struct mulle_concurrent_hashtable   table;

   // Same ABA-aware allocator as the real store. The naive baseline is
   // supposed to differ in exactly one way -- the missing atomic replace -- so
   // it gets the same plumbing, or it would abort on the first table migration
   // and measure nothing.
   struct mulle_allocator              allocator;

   // Removed values are parked, not freed. Freeing them here would turn this
   // into a use-after-free test as well, and the point is to isolate the
   // *linearizability* defect, not to trip over the memory one.
   //
   // The parking itself needs a lock: several writer threads park, and an
   // unsynchronized append here corrupts the array and aborts the harness long
   // before it can report anything about the keyspace.
   struct naive_value                **removed;
   size_t                              removed_count;
   size_t                              removed_capacity;
   mulle_thread_mutex_t                lock;
};


static intptr_t   naive_hash( const void *bytes, size_t length)
{
   uint64_t   hash;

   // The fold every consumer of the hashtable has to write: the hash space is
   // the caller's contract, and in "positive" mode it is [1, INTPTR_MAX] --
   // 0 means "unclaimed", bit 63 is the FROZEN flag. Out of range is *rejected*
   // (EINVAL), not folded, so this is a requirement rather than a safety net.
   hash  = mulle_hash_64( bytes, length);
   hash &= (uint64_t) INTPTR_MAX;
   if( ! hash)
      hash = 1;

   return( (intptr_t) hash);
}


static struct naive_value   *naive_value_alloc( const void *bytes, size_t length)
{
   struct naive_value   *value;

   value = mulle_malloc( sizeof( struct naive_value) + length);
   if( ! value)
      return( NULL);

   value->length = length;
   memcpy( value->bytes, bytes, length);
   return( value);
}


static void   naive_store_park( struct naive_store *store, struct naive_value *value)
{
   size_t               capacity;
   struct naive_value **values;

   mulle_thread_mutex_lock( &store->lock);

   if( store->removed_count == store->removed_capacity)
   {
      capacity = store->removed_capacity ? store->removed_capacity * 2 : 1024;
      values   = mulle_malloc( capacity * sizeof( void *));
      if( ! values)
      {
         mulle_thread_mutex_unlock( &store->lock);
         return;
      }
      if( store->removed_count)
         memcpy( values, store->removed, store->removed_count * sizeof( void *));
      mulle_free( store->removed);
      store->removed          = values;
      store->removed_capacity = capacity;
   }
   store->removed[ store->removed_count++] = value;

   mulle_thread_mutex_unlock( &store->lock);
}


/*
 * SET, the obvious way. Every step is individually atomic, the sequence is not:
 * between the remove and the insert the key does not exist at all, so a
 * concurrent GET sees a key that was never deleted as absent.
 */
static void   naive_store_set( struct naive_store *store,
                               intptr_t hash,
                               const char *bytes,
                               size_t length,
                               int yield_inside_window)
{
   struct naive_value   *value;
   struct naive_value   *old;
   int                   rval;

   value = naive_value_alloc( bytes, length);
   if( ! value)
      return;

   for(;;)
   {
      old = mulle_concurrent_hashtable_lookup( &store->table, hash);
      if( old)
      {
         rval = mulle_concurrent_hashtable_remove( &store->table, hash, old);
         if( rval)
            continue;   // someone else got there first, look again

         naive_store_park( store, old);

         // This widens a window that is there either way. The test runs both
         // with and without it and reports both numbers.
         if( yield_inside_window)
            mulle_thread_yield();   // thrd_yield / sched_yield / SwitchToThread
      }

      rval = mulle_concurrent_hashtable_insert( &store->table, hash, value);
      if( ! rval)
         return;
      if( rval != EEXIST)
         return;        // EINVAL: give up rather than spin forever
   }
}


static void   naive_store_destroy( struct naive_store *store)
{
   size_t   i;

   mulle_thread_mutex_done( &store->lock);

   for( i = 0; i < store->removed_count; i++)
      mulle_free( store->removed[ i]);
   mulle_free( store->removed);

   mulle_concurrent_hashtable_done( &store->table);
   mulle_free( store);
}


//
// concurrency
//
struct concurrency
{
   int                        mode;
   int                        is_writer;
   long                       ops;
   int                        yield_inside_window;
   intptr_t                   hash;
   struct mredis_store       *store;
   struct naive_store        *naive;
   mulle_atomic_pointer_t    *p_missing;
   mulle_atomic_pointer_t    *p_reads;
};


static mulle_thread_rval_t   concurrency_main( void *arg)
{
   struct concurrency   *c;
   char                  value[ 32];
   size_t                value_length;
   long                  i;
   long                  reads;

   c = (struct concurrency *) arg;

   mulle_aba_register();

   // One constant value per writer: the workload only needs the key to keep
   // being replaced, not to keep changing content.
   value[ 0]    = 'w';
   value[ 1]    = (char) ('0' + (c->is_writer % 10));
   value_length = 2;

   reads = 0;
   for( i = 0; i < c->ops; i++)
   {
      if( c->is_writer)
      {
         if( c->mode == MODE_NAIVE)
            naive_store_set( c->naive, c->hash, value, value_length, c->yield_inside_window);
         else
            (void) mredis_store_set( c->store,
                                     MREDIS_TEST_KEY, MREDIS_TEST_KEY_LENGTH,
                                     value, value_length);
         continue;
      }

      reads++;
      if( c->mode == MODE_NAIVE)
      {
         if( ! mulle_concurrent_hashtable_lookup( &c->naive->table, c->hash))
            _mulle_atomic_pointer_increment( c->p_missing);
      }
      else
      {
         struct mredis_reader   reader;
         struct mredis_value   *got;
         size_t                 length;

         mredis_store_reader_begin( &reader, c->store);
         got = mredis_store_get( &reader,
                                 MREDIS_TEST_KEY, MREDIS_TEST_KEY_LENGTH,
                                 &length);
         if( ! got)
            _mulle_atomic_pointer_increment( c->p_missing);
         mredis_store_reader_end( &reader);
      }
   }

   _mulle_atomic_pointer_add( c->p_reads, (intptr_t) reads);
   mulle_aba_unregister();
   return( 0);
}


static int   run_concurrency( int mode,
                              long threads,
                              long ops,
                              int yield_inside_window)
{
   struct concurrency      workers[ 64];
   mulle_thread_t          handles[ 64];
   struct mredis_store    *store;
   struct naive_store     *naive;
   mulle_atomic_pointer_t  missing;
   mulle_atomic_pointer_t  reads;
   struct mulle_buffer     line;
   long                    writers;
   long                    readers;
   long                    i;
   long                    joined;
   long                    missing_count;

   if( threads > 64)
      threads = 64;
   if( threads < 2)
      threads = 2;

   writers = threads / 2;
   readers = threads - writers;

   store = NULL;
   naive = NULL;

   _mulle_atomic_pointer_nonatomic_write( &missing, NULL);
   _mulle_atomic_pointer_nonatomic_write( &reads, NULL);

   /*
    * The key is created before any thread starts and never deleted, so from
    * the moment the threads start, a reader observing it as absent is a
    * linearizability violation by definition.
    */
   if( mode == MODE_NAIVE)
   {
      naive = mulle_malloc( sizeof( struct naive_store));
      memset( naive, 0, sizeof( struct naive_store));
      mulle_thread_mutex_init( &naive->lock);
      naive->allocator = mulle_allocator_default;
      mulle_aba_set_allocator_aba( &naive->allocator);
      mulle_concurrent_hashtable_init( &naive->table, 64, &naive->allocator);
      naive_store_set( naive, naive_hash( MREDIS_TEST_KEY, MREDIS_TEST_KEY_LENGTH),
                       "setup", 5, 0);
   }
   else
   {
      store = mredis_store_create( 64);
      if( ! store)
      {
         mulle_fprintf( stderr, "mredis-stress: out of memory\n");
         return( 2);
      }
      (void) mredis_store_set( store,
                               MREDIS_TEST_KEY, MREDIS_TEST_KEY_LENGTH,
                               "setup", 5);
   }

   joined = 0;
   for( i = 0; i < threads; i++)
   {
      workers[ i].mode                = mode;
      workers[ i].is_writer           = (i < writers);
      workers[ i].ops                 = ops;
      workers[ i].yield_inside_window = yield_inside_window;
      workers[ i].hash                = naive_hash( MREDIS_TEST_KEY, MREDIS_TEST_KEY_LENGTH);
      workers[ i].store               = store;
      workers[ i].naive               = naive;
      workers[ i].p_missing           = &missing;
      workers[ i].p_reads             = &reads;

      if( mulle_thread_create( concurrency_main, &workers[ i], &handles[ i]))
      {
         mulle_fprintf( stderr, "mredis-stress: could not create a thread\n");
         break;
      }
      joined++;
   }

   for( i = 0; i < joined; i++)
      (void) mulle_thread_join( handles[ i]);

   missing_count = load_counter( &missing);

   mulle_buffer_init( &line, 128, NULL);
   add_text( &line, (mode == MODE_NAIVE) ? "mode=naive " : "mode=entry ");
   add_pair( &line, "threads=", (uint64_t) threads);
   add_text( &line, " ");
   add_pair( &line, "writers=", (uint64_t) writers);
   add_text( &line, " ");
   add_pair( &line, "readers=", (uint64_t) readers);
   add_text( &line, " ");
   add_pair( &line, "ops=", (uint64_t) ops);
   add_text( &line, " ");
   add_pair( &line, "reads=", (uint64_t) load_counter( &reads));
   add_text( &line, " ");
   add_pair_signed( &line, "missing=", missing_count);
   mulle_buffer_add_bytes( &line, "\n", 1);
   print_buffer( &line);
   mulle_buffer_done( &line);

   if( naive)
      naive_store_destroy( naive);
   if( store)
      mredis_store_destroy( store);

   // A miss in the real keyspace is a failure, not a measurement.
   return( (mode == MODE_ENTRY && missing_count) ? 2 : 0);
}


//
// collision
//
static int   run_collision( long key_count)
{
   struct mredis_store   *store;
   struct mulle_buffer    line;
   char                   key[ 32];
   char                   value[ 32];
   long                   stored;
   long                   collisions;
   long                   aliased;
   long                   errors;
   long                   i;

   store = mredis_store_create( 64);
   if( ! store)
      return( 2);

   // 8 bits of hash for thousands of keys: collisions are certain, and none of
   // them may alias.
   mredis_store_set_hash_bits( store, 8);

   stored     = 0;
   collisions = 0;
   aliased    = 0;
   errors     = 0;

   for( i = 0; i < key_count; i++)
   {
      struct mredis_reader   reader;
      struct mredis_value   *got;
      size_t                 key_length;
      size_t                 value_length;
      size_t                 got_length;
      char                  *p;
      int                    rval;

      p          = key;
      *p++       = 'k';
      p          = append_uint( p, (uint64_t) i);
      key_length = (size_t) (p - key);

      p            = value;
      *p++         = 'v';
      p            = append_uint( p, (uint64_t) i);
      value_length = (size_t) (p - value);

      rval = mredis_store_set( store, key, key_length, value, value_length);
      if( rval == MREDIS_STORE_OK)
         stored++;
      else if( rval == MREDIS_STORE_COLLISION)
         collisions++;
      else
         errors++;

      /*
       * The property that matters: a key resolves to its own value, or to
       * nothing at all. Resolving to *somebody else's* value is the failure
       * this test exists for -- it is silent, and the server would look fine.
       */
      mredis_store_reader_begin( &reader, store);
      got = mredis_store_get( &reader, key, key_length, &got_length);
      if( got)
      {
         if( got_length != value_length ||
             memcmp( mredis_value_bytes( got), value, value_length))
            aliased++;
      }
      else if( rval == MREDIS_STORE_OK)
         aliased++;
      mredis_store_reader_end( &reader);
   }

   mulle_buffer_init( &line, 128, NULL);
   add_pair( &line, "keys=", (uint64_t) key_count);
   add_text( &line, " ");
   add_pair( &line, "stored=", (uint64_t) stored);
   add_text( &line, " ");
   add_pair( &line, "collisions=", (uint64_t) collisions);
   add_text( &line, " ");
   add_pair( &line, "aliased=", (uint64_t) aliased);
   add_text( &line, " ");
   add_pair( &line, "errors=", (uint64_t) errors);
   add_text( &line, " ");
   add_pair( &line, "count=", (uint64_t) mredis_store_count( store));
   mulle_buffer_add_bytes( &line, "\n", 1);
   print_buffer( &line);
   mulle_buffer_done( &line);

   mredis_store_destroy( store);
   return( (aliased || errors) ? 2 : 0);
}


//
// reclamation
//
struct churn
{
   struct mredis_store   *store;
   long                   ops;
   int                    is_writer;
};


static mulle_thread_rval_t   churn_main( void *arg)
{
   struct churn          *c;
   struct mredis_reader   reader;
   char                   value[ 32];
   size_t                 value_length;

   c = (struct churn *) arg;

   mulle_aba_register();

   value[ 0]    = 'v';
   value[ 1]    = (char) ('0' + (c->is_writer % 10));
   value_length = 2;

   if( c->is_writer)
   {
      long   i;

      for( i = 0; i < c->ops; i++)
         (void) mredis_store_set( c->store,
                                  MREDIS_TEST_KEY, MREDIS_TEST_KEY_LENGTH,
                                  value, value_length);
   }
   else
   {
      long   i;

      // A reader that does not even look at the value still holds the epoch
      // open, which is what keeps a replaced value alive.
      for( i = 0; i < c->ops; i++)
      {
         mredis_store_reader_begin( &reader, c->store);
         (void) mredis_store_get( &reader,
                                  MREDIS_TEST_KEY, MREDIS_TEST_KEY_LENGTH,
                                  &value_length);
         mredis_store_reader_end( &reader);
      }
   }

   mulle_aba_unregister();
   return( 0);
}


static int   run_reclaim( long churn_ops)
{
   struct mredis_store   *store;
   struct mredis_reader   reader;
   struct churn           workers[ 8];
   mulle_thread_t         handles[ 8];
   struct mulle_buffer    line;
   char                   value[ 32];
   size_t                 length;
   long                   i;
   size_t                 live;
   size_t                 retired;
   int                    failed;

   store = mredis_store_create( 64);
   if( ! store)
      return( 2);

   failed = 0;

   /*
    * Phase one: no readers, so every replaced value can be freed on the spot.
    * 2000 writes must not leave 2000 values behind.
    */
   for( i = 0; i < 2000; i++)
   {
      char  *p;

      p      = value;
      *p++   = 'v';
      p      = append_uint( p, (uint64_t) i);
      length = (size_t) (p - value);

      (void) mredis_store_set( store, MREDIS_TEST_KEY, MREDIS_TEST_KEY_LENGTH, value, length);
   }

   live    = mredis_store_live_values( store);
   retired = mredis_store_retired_values( store);
   if( live != 1 || retired)
      failed = 1;

   mulle_buffer_init( &line, 128, NULL);
   add_text( &line, "uncontended writes=2000 ");
   add_pair( &line, "live=", (uint64_t) live);
   add_text( &line, " ");
   add_pair( &line, "retired=", (uint64_t) retired);
   mulle_buffer_add_bytes( &line, "\n", 1);
   print_buffer( &line);
   mulle_buffer_reset( &line);

   // Deleting the only key frees its value too.
   (void) mredis_store_delete( store, MREDIS_TEST_KEY, MREDIS_TEST_KEY_LENGTH);
   live = mredis_store_live_values( store);
   if( live || mredis_store_count( store))
      failed = 1;

   add_text( &line, "after_delete ");
   add_pair( &line, "live=", (uint64_t) live);
   add_text( &line, " ");
   add_pair( &line, "keys=", (uint64_t) mredis_store_count( store));
   mulle_buffer_add_bytes( &line, "\n", 1);
   print_buffer( &line);
   mulle_buffer_reset( &line);

   (void) mredis_store_set( store, MREDIS_TEST_KEY, MREDIS_TEST_KEY_LENGTH, "again", 5);

   /*
    * Phase two: readers inside epochs while a writer replaces the value. Every
    * value the writer replaces has to wait for them, and then has to actually
    * be freed.
    */
   for( i = 0; i < 8; i++)
   {
      workers[ i].store     = store;
      workers[ i].ops       = churn_ops;
      workers[ i].is_writer = (i == 0);
   }

   for( i = 0; i < 8; i++)
   {
      if( mulle_thread_create( churn_main, &workers[ i], &handles[ i]))
      {
         mulle_fprintf( stderr, "mredis-stress: could not create a thread\n");
         mredis_store_destroy( store);
         return( 2);
      }
   }
   for( i = 0; i < 8; i++)
      (void) mulle_thread_join( handles[ i]);

   /*
    * A quiescent point, which is how the design reclaims: retirements that
    * happened while a reader was active are freed the moment no reader is.
    */
   mredis_store_reader_begin( &reader, store);
   mredis_store_reader_end( &reader);

   live    = mredis_store_live_values( store);
   retired = mredis_store_retired_values( store);
   if( live != 1 || retired)
      failed = 1;

   add_text( &line, "after_churn ");
   add_pair( &line, "ops=", (uint64_t) churn_ops);
   add_text( &line, " ");
   add_pair( &line, "live=", (uint64_t) live);
   add_text( &line, " ");
   add_pair( &line, "retired=", (uint64_t) retired);
   mulle_buffer_add_bytes( &line, "\n", 1);
   print_buffer( &line);
   mulle_buffer_done( &line);

   mredis_store_destroy( store);
   return( failed ? 2 : 0);
}


//
// script: this is where the client is used, and the replies get formatted.
//
// Everything here is portable except the socket, which is why the guard went
// away: a --stdio child needs two pipes and nothing else, and that is the one
// transport a platform without a socket API can still offer.
//

/* printable form, so a golden file is diffable by eye */
static void   append_escaped( struct mulle_buffer *buffer,
                              const char *bytes,
                              size_t length)
{
   static const char   *hex = "0123456789abcdef";
   size_t               i;

   for( i = 0; i < length; i++)
   {
      unsigned char   u;
      char            c;

      u = (unsigned char) bytes[ i];
      c = bytes[ i];
      if( c >= 0x20 && c < 0x7f)
      {
         mulle_buffer_add_bytes( buffer, &c, 1);
         continue;
      }

      {
         char   escaped[ 4];

         escaped[ 0] = '\\';
         escaped[ 1] = 'x';
         escaped[ 2] = hex[ (u >> 4) & 0xf];
         escaped[ 3] = hex[ u & 0xf];
         mulle_buffer_add_bytes( buffer, escaped, 4);
      }
   }
}


/* 0 on success, -1 if the connection went away or said something impossible */
static int   print_reply( struct mredis_wire *wire, struct mulle_buffer *buffer, int depth)
{
   char     line[ 512];
   int      c;
   long     count;
   long     i;
   long     length;

   c = mredis_wire_getc( wire);
   if( c < 0)
      return( -1);

   switch( c)
   {
   case '+' :
   case '-' :
      if( mredis_wire_read_line( wire, line, sizeof( line)))
         return( -1);
      mulle_buffer_add_bytes( buffer, (char *) &c, 1);
      add_text( buffer, line);
      return( 0);

   case ':' :
      if( mredis_wire_read_line( wire, line, sizeof( line)))
         return( -1);
      mulle_buffer_add_bytes( buffer, ":", 1);
      add_text( buffer, line);
      return( 0);

   case '$' :
      if( mredis_wire_read_line( wire, line, sizeof( line)))
         return( -1);

      length = strtol( line, NULL, 10);
      if( length < 0)
      {
         add_text( buffer, "$nil");
         return( 0);
      }
      if( length >= 2048)
      {
         // Only for printing, so a test authoring mistake, not a server bug.
         add_text( buffer, "$too-long ");
         add_text( buffer, line);
         return( -1);
      }

      {
         char   value[ 2048];
         char   crlf[ 2];

         if( mredis_wire_read( wire, value, (size_t) length))
            return( -1);
         if( mredis_wire_read( wire, crlf, 2))
            return( -1);

         mulle_buffer_add_bytes( buffer, "$", 1);
         add_uint( buffer, (uint64_t) length);
         if( length)   // no trailing space on an empty value: goldens stay clean
         {
            mulle_buffer_add_bytes( buffer, " ", 1);
            append_escaped( buffer, value, (size_t) length);
         }
      }
      return( 0);

   case '*' :
      if( mredis_wire_read_line( wire, line, sizeof( line)))
         return( -1);

      count = strtol( line, NULL, 10);
      if( count < 0)
      {
         add_text( buffer, "*nil");
         return( 0);
      }
      if( count > 64 || depth > 4)
         return( -1);

      add_text( buffer, "*");
      add_text( buffer, line);
      for( i = 0; i < count; i++)
      {
         mulle_buffer_add_bytes( buffer, " ", 1);
         if( print_reply( wire, buffer, depth + 1))
            return( -1);
      }
      return( 0);

   default :
      add_text( buffer, "?unknown-type=");
      mulle_buffer_add_bytes( buffer, (char *) &c, 1);
      return( -1);
   }
}


/*
 * `raw:...` puts bytes on the wire verbatim, with \r, \n, \t, \\ and \xNN
 * translated. It is how a fixture expresses a request the runner would never
 * encode on purpose -- a malformed one, or one carrying CRLF in a value.
 */
static void   decode_escapes( const char *src, struct mulle_buffer *buffer)
{
   static const char   *hex = "0123456789abcdef";
   char                c;

   while( *src)
   {
      unsigned char   value;

      if( *src != '\\')
      {
         mulle_buffer_add_bytes( buffer, src, 1);
         src++;
         continue;
      }

      src++;
      switch( *src)
      {
      case 'r' : c = '\r'; src++; break;
      case 'n' : c = '\n'; src++; break;
      case 't' : c = '\t'; src++; break;
      case '\\' : c = '\\'; src++; break;

      case 'x' :
         value = 0;
         if( src[ 1] && src[ 2])
         {
            const char   *high;
            const char   *low;

            high = strchr( hex, src[ 1]);
            low  = strchr( hex, src[ 2]);
            if( high && low)
            {
               value = (unsigned char) (((high - hex) << 4) | (low - hex));
               src  += 3;
            }
            else
               src++;
         }
         else
            src++;
         c = (char) value;
         break;

      default :
         // Unknown escape: keep it literally, backslash and all.
         c   = '\\';
         break;
      }
      mulle_buffer_add_bytes( buffer, &c, 1);
   }
}


static int   run_script( const char *server, const char *path, int use_tcp)
{
   struct mulle_spawn_handle   handle;
   struct mulle_buffer         requests;
   struct mulle_buffer         line;
   struct mredis_wire          wire;
   FILE                        *fp;
   char                        input[ 1024];
   mulle_socket_t              socket_fd;
   int                         descriptor;
   long                        commands;
   long                        i;
   int                         rval;

   socket_fd  = MULLE_SOCKET_INVALID;
   descriptor = -1;

   fp = fopen( path, "r");
   if( ! fp)
   {
      mulle_fprintf( stderr, "mredis-stress: cannot open %s: %s\n", path, strerror( errno));
      return( 2);
   }

   mulle_buffer_init( &requests, 512, NULL);

   commands = 0;
   while( fgets( input, sizeof( input), fp))
   {
      char   *eol;
      char   *p;

      eol = strchr( input, '\n');
      if( eol)
         *eol = 0;
      eol = strchr( input, '\r');
      if( eol)
         *eol = 0;

      p = input;
      while( *p == ' ' || *p == '\t')
         p++;
      if( ! *p || *p == '#')
         continue;

      if( ! strncmp( p, "raw:", 4))
         decode_escapes( p + 4, &requests);
      else
         mredis_encode_command( p, &requests);
      commands++;
   }
   fclose( fp);

   mulle_buffer_init( &line, 256, NULL);

   if( use_tcp)
   {
      /* this one hands back the socket itself, not a pipe end */
      if( mredis_spawn_tcp( server, &socket_fd, &handle))
      {
         mulle_buffer_done( &requests);
         mulle_buffer_done( &line);
         return( 2);
      }
   }
   else
   {
      if( mredis_spawn_stdio( server, &handle))
      {
         mulle_fprintf( stderr, "mredis-stress: cannot start %s\n", server);
         mulle_buffer_done( &requests);
         mulle_buffer_done( &line);
         return( 2);
      }
      descriptor = handle.out;
   }

   if( use_tcp ? mulle_socket_write_all( socket_fd,
                                         mulle_buffer_get_bytes( &requests),
                                         mulle_buffer_get_length( &requests))
               : mredis_write_all( handle.in,
                                   mulle_buffer_get_bytes( &requests),
                                   mulle_buffer_get_length( &requests)))
   {
      mulle_fprintf( stderr, "mredis-stress: cannot send the script\n");
      mulle_spawn_close( &handle, NULL);
      mulle_buffer_done( &requests);
      mulle_buffer_done( &line);
      return( 2);
   }

   // EOF is how a stdio server is told it is done; the socket stays open,
   // because the replies are already on their way.
   if( ! use_tcp)
   {
      mredis_close( handle.in);
      handle.in = -1;
   }

   if( use_tcp)
      mredis_wire_init_socket( &wire, socket_fd);
   else
      mredis_wire_init( &wire, descriptor);

   rval = 0;
   for( i = 0; i < commands; i++)
   {
      mulle_buffer_reset( &line);
      if( print_reply( &wire, &line, 0))
      {
         mulle_buffer_add_bytes( &line, "?truncated\n", 11);
         rval = 2;
      }
      else
         mulle_buffer_add_bytes( &line, "\n", 1);

      print_buffer( &line);
   }

   if( use_tcp)
   {
      mulle_socket_close( socket_fd);
      mulle_spawn_terminate( &handle);
   }
   else
   {
      mredis_close( descriptor);
      handle.out = -1;
   }

   /*
    * A stdio server leaves when its stdin closes, which has already happened;
    * the tcp one was just told to. Either way the wait is bounded, and close
    * ends a child that is still somehow there -- it is not the place for
    * cooperation.
    */
   (void) mulle_spawn_wait( &handle, 5.0, NULL);
   mulle_spawn_close( &handle, NULL);

   mulle_buffer_done( &line);
   mulle_buffer_done( &requests);
   return( rval);
}


static void   usage( FILE *fp)
{
   mulle_fprintf( fp, "usage: mredis-stress [options]\n");
   mulle_fprintf( fp, "\n");
   mulle_fprintf( fp, "  --concurrency     SET/GET on one key from many threads\n");
   mulle_fprintf( fp, "  --naive           ... using remove+insert instead of a CAS\n");
   mulle_fprintf( fp, "  --no-yield        ... without the yield inside that window\n");
   mulle_fprintf( fp, "  --threads N       threads (default 8)\n");
   mulle_fprintf( fp, "  --ops N           operations per thread (default 50000)\n");
   mulle_fprintf( fp, "  --collision       narrow the hash, check the keyspace's key comparison\n");
   mulle_fprintf( fp, "  --reclaim         check that replaced values are freed\n");
   mulle_fprintf( fp, "  --bench           load generator with a live requests/s, MB/s and RSS display\n");
   mulle_fprintf( fp, "                    (--bench --help lists what that mode takes: --seconds,\n");
   mulle_fprintf( fp, "                    --pipeline, --value-size, --keys, --connect, --redis,\n");
   mulle_fprintf( fp, "                    --compare, --quiet, --interval, --stdio)\n");
   mulle_fprintf( fp, "  --script FILE     run commands against a server, print replies\n");
   mulle_fprintf( fp, "  --tcp             ... over a socket instead of a stdio child\n");
   mulle_fprintf( fp, "  --server PATH     the mredis binary to spawn (default ./mredis)\n");
   mulle_fprintf( fp, "  -h, --help        this text\n");
}


int   main( int argc, char *argv[])
{
   const char   *script;
   const char   *server;
   long          threads;
   long          ops;
   int           do_concurrency;
   int           do_collision;
   int           do_reclaim;
   int           mode;
   int           yield_inside_window;
   int           use_tcp;
   int           i;

   script              = NULL;
   server              = "./mredis";
   threads             = 8;
   ops                 = 50000;
   do_concurrency      = 0;
   do_collision        = 0;
   do_reclaim          = 0;
   mode                = MODE_ENTRY;
   yield_inside_window = 1;
   use_tcp             = 0;

#if MREDIS_HAVE_POSIX
   // A server that dies mid-script must not take the harness down with it.
   signal( SIGPIPE, SIG_IGN);
#endif

   // Required before any concurrent container is created, and in every thread
   // that touches one (see the worker entry points).
   mulle_aba_init( NULL);
   mulle_aba_register();

   /*
    * --bench owns its options (--seconds, --pipeline, ...) and dispatches here,
    * before the option loop below, which would otherwise reject them as
    * unknown.
    */
   for( i = 1; i < argc; i++)
      if( ! strcmp( argv[ i], "--bench"))
         return( mredis_bench_main( argc, argv));

   for( i = 1; i < argc; i++)
   {
      if( ! strcmp( argv[ i], "-h") || ! strcmp( argv[ i], "--help"))
      {
         usage( stdout);
         return( 0);
      }
      if( ! strcmp( argv[ i], "--concurrency"))
      {
         do_concurrency = 1;
         continue;
      }
      if( ! strcmp( argv[ i], "--naive"))
      {
         mode = MODE_NAIVE;
         continue;
      }
      if( ! strcmp( argv[ i], "--no-yield"))
      {
         yield_inside_window = 0;
         continue;
      }
      if( ! strcmp( argv[ i], "--collision"))
      {
         do_collision = 1;
         continue;
      }
      if( ! strcmp( argv[ i], "--reclaim"))
      {
         do_reclaim = 1;
         continue;
      }
      if( ! strcmp( argv[ i], "--tcp"))
      {
         use_tcp = 1;
         continue;
      }
      if( ! strcmp( argv[ i], "--threads") && i + 1 < argc)
      {
         threads = strtol( argv[ ++i], NULL, 10);
         continue;
      }
      if( ! strcmp( argv[ i], "--ops") && i + 1 < argc)
      {
         ops = strtol( argv[ ++i], NULL, 10);
         continue;
      }
      if( ! strcmp( argv[ i], "--script") && i + 1 < argc)
      {
         script = argv[ ++i];
         continue;
      }
      if( ! strcmp( argv[ i], "--server") && i + 1 < argc)
      {
         server = argv[ ++i];
         continue;
      }
      mulle_fprintf( stderr, "mredis-stress: unknown option %s\n", argv[ i]);
      usage( stderr);
      return( 1);
   }

   if( do_collision)
      return( run_collision( 2000));

   if( do_reclaim)
      return( run_reclaim( 20000));

   if( do_concurrency)
      return( run_concurrency( mode, threads, ops, yield_inside_window));

   if( script)
      return( run_script( server, script, use_tcp));

   usage( stdout);
   return( 0);
}
