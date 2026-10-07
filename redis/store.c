//
//  store.c
//  miniredis
//
//  See store.h for why the keyspace is shaped this way.
//

#include "store.h"

#include <stdint.h>
#include <string.h>


// A tombstone is a payload, not a pointer. Entry payloads are either NULL
// (never set), this (deleted), or a `struct mredis_value *`.
#define MREDIS_TOMBSTONE   ((void *) (intptr_t) 1)


struct mredis_value
{
   size_t                  length;
   struct mredis_value    *next;   // link in the retire stack, unused while live
   char                    bytes[ 1];   // length bytes, plus a NUL for convenience
};


struct mredis_entry
{
   mulle_atomic_pointer_t   payload;
   size_t                   key_length;
   char                     key[ 1];   // key_length bytes, plus NUL
};


struct mredis_store
{
   struct mulle_concurrent_hashtable   table;

   /*
    * Our own copy of the default allocator, because the table frees its
    * retired storage generations through the allocator's abafree hook -- and
    * the hook on the default allocator is mulle_allocator_no_aba_abort(). A
    * table that migrates while holding that allocator aborts the process, and
    * it only does so once it grows, which is why it survives every small test.
    *
    * mulle_aba_set_allocator_aba() routes those frees through mulle-aba, which
    * is the library's own answer to the problem. It requires mulle_aba_init()
    * before this and mulle_aba_register() in every thread that touches the
    * table -- see the note in store.h.
    */
   struct mulle_allocator              allocator;

   // Integer-only atomics. The arithmetic helpers on mulle_atomic_pointer_t
   // type-pun the storage to _Atomic( intptr_t), which the header says is only
   // sound for atomics that never hold real pointers -- the counters below
   // qualify. The entry payloads and the retire stack head do not, and are
   // CASed.
   mulle_atomic_pointer_t   readers;    // readers inside an epoch
   mulle_atomic_pointer_t   keys;       // live keys
   mulle_atomic_pointer_t   values;     // values allocated and not yet freed

   /*
    * The retire stack is the whole reclamation mechanism, and it is lock-free:
    * a writer pushes with a CAS loop, and a reader that observes the epoch
    * quiescent detaches the entire chain with one CAS to NULL and frees it.
    * There is no mutex anywhere on the SET, GET or DELETE path.
    *
    * The nodes are the values themselves -- `next` lives inside
    * struct mredis_value -- so retiring a value never allocates and can never
    * fail. That is also why there is no "leaked" counter any more: the old
    * list grew by realloc, and a failed realloc had nowhere to put the value.
    */
   mulle_atomic_pointer_t   retired;        // head of the lock-free retire stack
   mulle_atomic_pointer_t   retired_count;  // nodes on the stack, approximate
   mulle_atomic_pointer_t   retired_peak;   // high-water mark, approximate
   uint64_t                 hash_mask;
};


//
// hashing
//
/*
 * The key hash has to lie in the table's hash space, and that is the caller's
 * contract -- the table validates and rejects (EINVAL, NULL from lookup), it
 * does not fold and it does not alias. The default "positive" mode reserves the
 * top bit for its FROZEN flag, so the legal range is [1, INTPTR_MAX]: integers
 * > 0 up to 2^63. mulle_hash_64() returns a full 64 bit value, so it is folded
 * here by hand, which is the documented cost of this mode.
 *
 * The alternative "even" mode reserves bit 0 and takes even values up to 2^64 --
 * free for aligned pointers, but a byte hash would still have to be folded, into
 * the same 2^63 values. Nothing is gained for this keyspace.
 *
 * And the hash *is* the key: the table holds exactly one entry per hash, so two
 * keys with equal hashes are one key to it. hash_mask exists to force that on
 * purpose, to exercise the keyspace's own key comparison -- which is where "key
 * identity" actually comes from here. That comparison is this store's job, not
 * the table's, and it is not a workaround: the table sells hash identity.
 *
 * Why not mulle_concurrent_hashmap, whose key is a full sizeof(intptr_t)? Its
 * single advantage is exactly one more bit of key space (2^64 vs 2^63), and it
 * pays for it with the legacy design and a remove/patch that are single-threaded
 * only. One bit buys this keyspace nothing, because colliding keys are caught by
 * the byte comparison either way -- whereas the operation set is the whole
 * reason to prefer the hashtable (see fixed/03-hashtable-hash-folding.md and
 * bugs/17-hashtable-docs-say-folded-implementation-rejects.md).
 */
static intptr_t   mredis_store_hash( struct mredis_store *store,
                                     const void *bytes,
                                     size_t length)
{
   uint64_t   hash;

   hash  = mulle_hash_64( bytes, length);
   hash &= store->hash_mask;
   if( ! hash)
      hash = 1;                 // 0 is MULLE_CONCURRENT_NO_HASH

   return( (intptr_t) hash);
}


//
// values
//
static struct mredis_value   *mredis_value_alloc( struct mredis_store *store,
                                                  const void *bytes,
                                                  size_t length)
{
   struct mredis_value   *value;

   value = mulle_malloc( sizeof( struct mredis_value) + length);
   if( ! value)
      return( NULL);

   value->length = length;
   if( length)
      memcpy( value->bytes, bytes, length);
   value->bytes[ length] = 0;

   _mulle_atomic_pointer_increment( &store->values);
   return( value);
}


static void   mredis_value_free( struct mredis_store *store,
                                 struct mredis_value *value)
{
   mulle_free( value);
   _mulle_atomic_pointer_decrement( &store->values);
}


/*
 * A payload is not always a value: it can be NULL (never set) or a tombstone
 * (deleted). Freeing those is how you get `free(): invalid pointer` from
 * mulle_free( 1), so every payload goes through here.
 */
static void   mredis_payload_free( struct mredis_store *store, void *payload)
{
   if( ! payload || payload == MREDIS_TOMBSTONE)
      return;

   mredis_value_free( store, payload);
}


//
// reclamation
//
/*
 * Free everything that was retired while a reader was inside an epoch.
 *
 * The check on `readers` is the whole grace period. Every increment of the
 * counter is seq_cst, so if it reads zero here, no reader can be inside an
 * epoch; and a reader that enters afterwards can only ever see payloads that
 * are still current, never one on this list.
 *
 * The detach is one CAS of the stack head to NULL. A concurrent writer that
 * loses the race re-reads the new head and retries, so it never links a node
 * into a chain this function has already taken. A writer that wins the race
 * first makes the CAS fail here, and the next read picks up its node too. The
 * detached chain is private, so walking and freeing it needs no lock.
 */
static void   mredis_store_drain( struct mredis_store *store)
{
   struct mredis_value   *head;
   struct mredis_value   *next;

   if( _mulle_atomic_pointer_read( &store->readers))
      return;

   for(;;)
   {
      head = _mulle_atomic_pointer_read( &store->retired);
      if( ! head)
         return;
      if( _mulle_atomic_pointer_cas( &store->retired, NULL, head))
         break;
   }

   while( head)
   {
      next = head->next;
      mredis_value_free( store, head);
      _mulle_atomic_pointer_decrement( &store->retired_count);
      head = next;
   }
}


/*
 * A value has just been replaced (or deleted). Either nobody can still be
 * looking at it, or it has to wait for the next quiescent point.
 */
static void   mredis_value_retire( struct mredis_store *store, void *payload)
{
   struct mredis_value   *value;
   struct mredis_value   *head;
   intptr_t               count;
   intptr_t               peak;

   if( ! payload || payload == MREDIS_TOMBSTONE)
      return;

   value = payload;

   if( ! _mulle_atomic_pointer_read( &store->readers))
   {
      mredis_value_free( store, value);
      return;
   }

   // Push. value is freshly retired and owned by this call, so it can never
   // equal head, which is what the CAS asserts.
   for(;;)
   {
      head = _mulle_atomic_pointer_read( &store->retired);
      value->next = head;
      if( _mulle_atomic_pointer_cas( &store->retired, value, head))
         break;
   }

   count = (intptr_t) _mulle_atomic_pointer_increment( &store->retired_count) + 1;
   peak  = (intptr_t) _mulle_atomic_pointer_read( &store->retired_peak);
   if( count > peak)
      _mulle_atomic_pointer_write( &store->retired_peak, (void *) count);
}


//
// entries
//
static struct mredis_entry   *mredis_entry_alloc( const void *key,
                                                  size_t key_length)
{
   struct mredis_entry   *entry;

   entry = mulle_malloc( sizeof( struct mredis_entry) + key_length);
   if( ! entry)
      return( NULL);

   entry->key_length = key_length;
   if( key_length)
      memcpy( entry->key, key, key_length);
   entry->key[ key_length] = 0;

   // Published by register() below, so this happens before any other thread can
   // observe the entry.
   _mulle_atomic_pointer_nonatomic_write( &entry->payload, NULL);
   return( entry);
}


/*
 * Find (or create) the entry that owns `hash`, and check that it really is the
 * entry for `key`. `create` is 0 for readers.
 *
 *   MREDIS_STORE_OK         *p_entry set, or NULL if absent and ! create
 *   MREDIS_STORE_COLLISION  a *different* key owns this hash
 *   MREDIS_STORE_ERROR      could not allocate
 */
static int   mredis_entry_for( struct mredis_store *store,
                               const void *key,
                               size_t key_length,
                               intptr_t hash,
                               int create,
                               struct mredis_entry **p_entry)
{
   struct mredis_entry   *entry;
   struct mredis_entry   *new_entry;
   void                  *old;

   entry = mulle_concurrent_hashtable_lookup( &store->table, hash);
   if( ! entry && create)
   {
      new_entry = mredis_entry_alloc( key, key_length);
      if( ! new_entry)
         return( MREDIS_STORE_ERROR);

      // Insert if the hash is absent and report what is registered afterwards
      // through *p_old (NULL if that was us). EINVAL could only mean the hash is
      // outside the table's space, which mredis_store_hash() rules out.
      old = NULL;
      if( mulle_concurrent_hashtable_register( &store->table, hash, new_entry, &old))
      {
         mulle_free( new_entry);
         return( MREDIS_STORE_ERROR);
      }

      if( old)
      {
         // Lost the race to register this hash: the entry that is there now is
         // the one to use.
         mulle_free( new_entry);
         entry = old;
      }
      else
         entry = new_entry;
   }

   if( entry && (entry->key_length != key_length ||
                 memcmp( entry->key, key, key_length)))
      return( MREDIS_STORE_COLLISION);

   *p_entry = entry;
   return( MREDIS_STORE_OK);
}


//
// public API
//
struct mredis_store   *mredis_store_create( size_t bucket_count)
{
   struct mredis_store   *store;

   store = mulle_malloc( sizeof( struct mredis_store));
   if( ! store)
      return( NULL);

   memset( store, 0, sizeof( struct mredis_store));

   store->allocator = mulle_allocator_default;
   mulle_aba_set_allocator_aba( &store->allocator);

   mulle_concurrent_hashtable_init( &store->table,
                                    bucket_count ? bucket_count : MREDIS_STORE_DEFAULT_BUCKETS,
                                    &store->allocator);

   _mulle_atomic_pointer_nonatomic_write( &store->readers, NULL);
   _mulle_atomic_pointer_nonatomic_write( &store->keys, NULL);
   _mulle_atomic_pointer_nonatomic_write( &store->values, NULL);
   _mulle_atomic_pointer_nonatomic_write( &store->retired, NULL);
   _mulle_atomic_pointer_nonatomic_write( &store->retired_count, NULL);
   _mulle_atomic_pointer_nonatomic_write( &store->retired_peak, NULL);

   store->hash_mask = (uint64_t) INTPTR_MAX;   // this table's hash space
   return( store);
}


void   mredis_store_destroy( struct mredis_store *store)
{
   struct mulle_concurrent_hashtableenumerator   rover;
   struct mredis_entry                          *entry;
   struct mredis_value                          *value;
   struct mredis_value                          *next;
   intptr_t                                      hash;
   void                                         *payload;

   if( ! store)
      return;

   // No readers can exist at this point, so the whole retire stack is free.
   value = _mulle_atomic_pointer_read( &store->retired);
   while( value)
   {
      next = value->next;
      mredis_value_free( store, value);
      value = next;
   }
   _mulle_atomic_pointer_nonatomic_write( &store->retired, NULL);

   rover = mulle_concurrent_hashtable_enumerate( &store->table);
   while( mulle_concurrent_hashtableenumerator_next( &rover, &hash, &payload) == 1)
   {
      entry = payload;
      mredis_payload_free( store, _mulle_atomic_pointer_read( &entry->payload));
      mulle_free( entry);
   }
   mulle_concurrent_hashtableenumerator_done( &rover);

   mulle_concurrent_hashtable_done( &store->table);
   mulle_free( store);
}


void   mredis_store_reader_begin( struct mredis_reader *reader,
                                  struct mredis_store *store)
{
   reader->store = store;
   _mulle_atomic_pointer_increment( &store->readers);
}


void   mredis_store_reader_end( struct mredis_reader *reader)
{
   struct mredis_store   *store;

   store = reader->store;
   reader->store = NULL;
   _mulle_atomic_pointer_decrement( &store->readers);

   mredis_store_drain( store);
}


const char   *mredis_value_bytes( struct mredis_value *value)
{
   return( value ? value->bytes : NULL);
}


size_t   mredis_value_length( struct mredis_value *value)
{
   return( value ? value->length : 0);
}


struct mredis_value   *mredis_store_get( struct mredis_reader *reader,
                                         const void *key,
                                         size_t key_length,
                                         size_t *p_length)
{
   struct mredis_store   *store;
   struct mredis_entry   *entry;
   struct mredis_value   *value;
   intptr_t               hash;

   store = reader->store;
   hash  = mredis_store_hash( store, key, key_length);

   if( mredis_entry_for( store, key, key_length, hash, 0, &entry) ||
       ! entry)
   {
      *p_length = 0;
      return( NULL);
   }

   // Safe to hand out: the reader is inside an epoch, so this value cannot be
   // freed until reader_end(), no matter how many SETs replace it meanwhile.
   value = _mulle_atomic_pointer_read_acquire( &entry->payload);
   if( ! value || value == MREDIS_TOMBSTONE)
   {
      *p_length = 0;
      return( NULL);
   }

   *p_length = value->length;
   return( value);
}


int   mredis_store_set( struct mredis_store *store,
                        const void *key,
                        size_t key_length,
                        const void *bytes,
                        size_t length)
{
   struct mredis_entry   *entry;
   struct mredis_value   *value;
   void                  *old;
   intptr_t               hash;
   int                    rval;

   hash = mredis_store_hash( store, key, key_length);
   rval = mredis_entry_for( store, key, key_length, hash, 1, &entry);
   if( rval)
      return( rval);

   value = mredis_value_alloc( store, bytes, length);
   if( ! value)
      return( MREDIS_STORE_ERROR);

   for(;;)
   {
      old = _mulle_atomic_pointer_read( &entry->payload);

      // _mulle_atomic_pointer_cas() asserts on a no-op swap, and `value` was
      // just allocated, so it can never equal `old` -- except in the
      // impossible case that the allocator recycled the very value we are
      // replacing, which we check for anyway.
      if( old == value)
         break;

      if( _mulle_atomic_pointer_cas( &entry->payload, value, old))
      {
         // The CAS decided whether this was a new key, a resurrection of a
         // deleted one, or a replacement.
         if( ! old || old == MREDIS_TOMBSTONE)
            _mulle_atomic_pointer_increment( &store->keys);

         mredis_value_retire( store, old);
         return( MREDIS_STORE_OK);
      }
   }

   mredis_value_free( store, value);
   return( MREDIS_STORE_ERROR);
}


int   mredis_store_delete( struct mredis_store *store,
                           const void *key,
                           size_t key_length)
{
   struct mredis_entry   *entry;
   void                  *old;
   intptr_t               hash;

   hash = mredis_store_hash( store, key, key_length);
   if( mredis_entry_for( store, key, key_length, hash, 0, &entry) ||
       ! entry)
      return( 0);

   for(;;)
   {
      old = _mulle_atomic_pointer_read( &entry->payload);
      if( ! old || old == MREDIS_TOMBSTONE)
         return( 0);

      if( _mulle_atomic_pointer_cas( &entry->payload, MREDIS_TOMBSTONE, old))
      {
         _mulle_atomic_pointer_decrement( &store->keys);
         mredis_value_retire( store, old);
         return( 1);
      }
   }
}


int   mredis_store_exists( struct mredis_store *store,
                           const void *key,
                           size_t key_length)
{
   struct mredis_entry   *entry;
   void                  *old;
   intptr_t               hash;

   hash = mredis_store_hash( store, key, key_length);
   if( mredis_entry_for( store, key, key_length, hash, 0, &entry) ||
       ! entry)
      return( 0);

   // No epoch needed: this reads the pointer, it never dereferences it.
   old = _mulle_atomic_pointer_read( &entry->payload);
   return( old && old != MREDIS_TOMBSTONE);
}


size_t   mredis_store_count( struct mredis_store *store)
{
   return( (size_t) (intptr_t) _mulle_atomic_pointer_read( &store->keys));
}


void   mredis_store_set_hash_bits( struct mredis_store *store, unsigned int bits)
{
   uint64_t   mask;

   if( bits >= 63)
      mask = (uint64_t) INTPTR_MAX;
   else
      mask = ((uint64_t) 1 << bits) - 1;

   store->hash_mask = mask ? mask : 1;
}


size_t   mredis_store_live_values( struct mredis_store *store)
{
   return( (size_t) (intptr_t) _mulle_atomic_pointer_read( &store->values));
}


size_t   mredis_store_retired_values( struct mredis_store *store)
{
   return( (size_t) (intptr_t) _mulle_atomic_pointer_read( &store->retired_count));
}


size_t   mredis_store_retired_peak( struct mredis_store *store)
{
   return( (size_t) (intptr_t) _mulle_atomic_pointer_read( &store->retired_peak));
}
