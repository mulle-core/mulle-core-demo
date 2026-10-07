//
//  store.h
//  miniredis -- a Redis-shaped keyspace on top of mulle-concurrent
//
//  Why the keyspace is shaped this way
//  -----------------------------------
//  The keyspace rides on mulle_concurrent_hashtable. Its key is the hash, and
//  the hash space is the caller's contract: the default "positive" mode takes
//  integers > 0 up to 2^63 (bit 63 is the FROZEN flag) and the table *rejects*
//  anything outside it -- EINVAL, NULL from lookup -- rather than folding it or
//  aliasing it. mulle_hash_64() is folded by hand here, which is the contract
//  the fix for bugs/03 settled on -- see fixed/03-hashtable-hash-folding.md,
//  and bugs/17 for the docs that still describe the removed fold.
//
//  The table offers exactly these atomic primitives:
//
//     register( hash, value, &old)   insert if absent, reports what is there
//     insert( hash, value)           insert, but EEXIST if a value is present
//     remove( hash, value)           remove *if equal* -- a real CAS
//     lookup( hash)
//
//  There is no replace and there is not going to be one: this is a wait-free
//  table, and remove + register is the documented ceiling (and it has to be this
//  container -- the hashmap's remove/patch are single-threaded only). The
//  obvious spelling of SET out of them,
//
//     old = lookup( hash);
//     remove( hash, old);
//     register( hash, new, &prev);
//
//  has a window in which the key does not exist at all, so a concurrent GET
//  sees a key that was never deleted as missing. Nothing here predicts that
//  window; stress.c measures it (83,139 of 200,000 reads lost a key that was
//  never deleted -- see wontfix/11-no-atomic-replace.md, which is declined
//  because no replace is the ceiling by design), and that measurement is what
//  forces the next paragraph.
//
//  So the table stores a node that never moves: one `struct mredis_entry` per
//  key, registered once and never unregistered for the lifetime of the store.
//  Only the entry's payload changes, and it changes with a pointer CAS. A
//  deleted key is marked with a tombstone rather than unregistered -- removing
//  an entry that another thread is about to CAS into would lose that write, and
//  freeing it would be a use-after-free.
//
//  Memory reclamation
//  ------------------
//  CAS-swapping a payload means a reader may still be looking at the value the
//  writer just replaced, so the old value cannot simply be freed. Readers
//  bracket their use of a value with reader_begin/reader_end (an epoch), and a
//  replaced value is freed only when no reader is inside an epoch.
//
//  Note what this is *not*: mulle-aba does exist, and the table uses it for its
//  own internal storage generations (see mredis_store_create). But it reclaims
//  memory the table allocated, not the payloads *you* put in it, so a keyspace
//  that swaps payloads still has to bracket its own readers. mulle-aba is not
//  usable for that directly, because it frees through a callback on the owner's
//  side, and a value has to stay readable until the reader is done with it.
//
//  Prerequisite: mulle_aba_init() must have been called, and every thread that
//  touches the store must have called mulle_aba_register() (and, when it is
//  done, mulle_aba_unregister()). Otherwise the first time the table grows it
//  aborts in mulle_allocator_no_aba_abort().
//
#ifndef mredis_store_h__
#define mredis_store_h__

#include <mulle-core/mulle-core.h>

#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

#define MREDIS_STORE_OK         0
#define MREDIS_STORE_COLLISION  1
#define MREDIS_STORE_ERROR     -1

#define MREDIS_STORE_DEFAULT_BUCKETS  1024

/*
 * Values retired while readers are inside an epoch wait on a lock-free stack
 * that grows by linking the values themselves -- retiring a value allocates
 * nothing and cannot fail. It is drained at every quiescent point (no reader
 * inside an epoch), so it stays small under bursty load -- but a reader set
 * that never empties means reclamation never happens, and memory grows with the
 * number of writes. That is inherent to deferring to quiescence, and it is why
 * a real server advances a grace period on a timer instead.
 * mredis_store_retired_peak() reports how far it actually got.
 */


struct mredis_value;
struct mredis_entry;
struct mredis_store;

/*
 * One in-flight operation. A value returned by mredis_store_get() stays valid
 * (and readable) until mredis_store_reader_end(). Values are immutable, so a
 * concurrent SET cannot change what a reader is looking at -- it can only
 * decide when it is freed.
 */
struct mredis_reader
{
   struct mredis_store   *store;
};


struct mredis_store   *mredis_store_create( size_t bucket_count);
void                   mredis_store_destroy( struct mredis_store *store);

void                   mredis_store_reader_begin( struct mredis_reader *reader,
                                                  struct mredis_store *store);
void                   mredis_store_reader_end( struct mredis_reader *reader);

/*
 * NULL if the key is absent. *p_length is set to the value's length.
 * Returns a borrowed value -- see the epoch note above.
 */
struct mredis_value   *mredis_store_get( struct mredis_reader *reader,
                                         const void *key,
                                         size_t key_length,
                                         size_t *p_length);

const char            *mredis_value_bytes( struct mredis_value *value);
size_t                 mredis_value_length( struct mredis_value *value);

/*
 * 0 on success, MREDIS_STORE_COLLISION if another key owns this hash slot,
 * MREDIS_STORE_ERROR if it could not be stored (out of memory).
 */
int                    mredis_store_set( struct mredis_store *store,
                                        const void *key,
                                        size_t key_length,
                                        const void *bytes,
                                        size_t length);

/* number of keys deleted: 0 or 1 */
int                    mredis_store_delete( struct mredis_store *store,
                                           const void *key,
                                           size_t key_length);

/* 1 or 0 */
int                    mredis_store_exists( struct mredis_store *store,
                                           const void *key,
                                           size_t key_length);

/* keys currently present (tombstones not counted) */
size_t                 mredis_store_count( struct mredis_store *store);

/*
 * Test hooks. Narrowing the hash is how the collision path gets exercised
 * deterministically. Note what that is testing: the table keys on the hash
 * alone and holds one entry per hash, so narrowing produces keys with the *same*
 * hash -- one key, as far as the table is concerned. The distinction this
 * exercises is the keyspace's own key comparison, which is what keeps two Redis
 * keys with colliding hashes from being one Redis key.
 */
void                   mredis_store_set_hash_bits( struct mredis_store *store,
                                                   unsigned int bits);
size_t                 mredis_store_live_values( struct mredis_store *store);
size_t                 mredis_store_retired_values( struct mredis_store *store);
size_t                 mredis_store_retired_peak( struct mredis_store *store);

#ifdef __cplusplus
}
#endif

#endif
