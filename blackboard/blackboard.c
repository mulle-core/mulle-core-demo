/*
 * blackboard — processes that publish and subscribe through one shared board.
 *
 *   blackboard                                   # 4 workers, 1 monitor + 2 readers
 *   blackboard --show 8 --tui
 *   blackboard --unsafe --verify                 # the same run, minus the lock
 *   blackboard --exhaust                         # run the board out of room
 *   blackboard --grow --growth-limit 1M          # expand instead of refusing
 *   blackboard --dump --quiet                    # the allocator's view of the board
 *
 * mulle-mmapallocator puts the *entire* board in one shared mapping: the
 * header, the subscriber table, the mailboxes and every note. Notes are
 * variable length and handed to subscribers by pointer, so a note allocated by
 * worker 3 is read and then *freed* by a reader process — no serialization, no
 * copy on the way in. The region is fixed at startup unless --grow reserves a
 * ceiling for it to expand into, and the fact that every process shares it is
 * the constraint that shapes the whole design:
 *
 *   mulle-mmapallocator  the region, the shared heap and its cross-process
 *                        spin lock. --capacity is the first segment and
 *                        --growth-limit the ceiling of --grow; without it the
 *                        limit is the region, --exhaust runs into it, and
 *                        --dump shows the tiling of every segment.
 *   mulle-spawn          the workers and the readers as processes. A child is
 *                        handed the mapping's handle and the address it has to
 *                        land on, and attaches the board itself -- fork() would
 *                        hand both over for free, and that is precisely what
 *                        kept this demo off Windows.
 *   mulle-fifo           _mulle__pointerfifo256 as the per-subscriber mailbox.
 *                        It is fixed size and SPSC by contract, which is
 *                        exactly right: writers are serialized by the board
 *                        lock, each mailbox has one reader process.
 *   mulle-thread         atomics for the parts that must not wait: note
 *                        reference counts, the counters, the board spin lock.
 *                        (This is C11 _Atomic over shared pages, so it works
 *                        between processes, not just threads.)
 *   mulle-time           the clock and the sleep of the monitor.
 *
 * The board is *bounded*, so a subscriber that does not keep up loses notes
 * instead of stalling the publisher, and a board that is full refuses notes
 * rather than dying. Running out of room is therefore a normal event here, and
 * the allocator's `fail` vector is where it is handled: a note is allocated
 * through the ordinary mulle_allocator_malloc(), and the handler longjmps back
 * to the allocation to report the refusal as a value. --exhaust is the case,
 * and --dump shows the shared heap after it.
 */

#include <mulle-core/mulle-core.h>

#include "mulle-mmapallocator.h"

#include <errno.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <io.h>
# include <windows.h>
# define blackboard_yield()     Sleep( 0)
# define blackboard_isatty()    (_isatty( 1) != 0)
#else
# include <sched.h>
# include <unistd.h>
# define blackboard_yield()     sched_yield()
# define blackboard_isatty()    (isatty( STDOUT_FILENO) != 0)
#endif


#define BB_MAGIC             0x42424f41u   // "BBOA"
#define BB_MAILBOX_DEPTH     256           // entries in the fixed shared fifo
#define BB_MAX_PAYLOAD       4096
#define BB_MAX_PROCESSES     32
#define BB_MAX_TOPICS        64

// a growable board maps this much up front and expands into it on demand
#define BB_DEFAULT_GROWTH_LIMIT   (4 * 1024 * 1024)


//
//  options
//
struct bb_options
{
   unsigned int   workers;        // publisher processes
   unsigned int   readers;        // reader processes besides the monitor
   unsigned int   topics;
   unsigned int   notes;          // notes per worker
   size_t         payload;        // longest payload, notes are 16..this
   size_t         capacity;       // shared region, rounded up to pages
   int            grow;           // expand the region on demand instead of refusing
   size_t         growth_limit;   // the ceiling it may expand to
   unsigned int   seed;
   unsigned int   show;           // print the first N notes the monitor sees
   int            unsafe;         // update the tally outside the lock
   int            perturb;        // test hook: flip a payload byte after checksumming
   int            exhaust;        // leave no room for the notes
   int            dump;           // _mulle_mmap_allocator_dump() at the end
   int            verify;         // assert the invariants at the end
   int            quiet;
   int            tui;
   double         interval;      // seconds between live reports; 0 = the default
};


//
//  How the monitor describes the board to a child.
//
//  A worker is a process of its own, and it did not inherit the board the way
//  fork() hands it over: it is told the mapping's handle and the address the
//  board lives at, and maps it there itself. That is the same re-attach
//  mulle-mmapallocator's own test/30-shared/process.c does, and what travels is
//  whatever a handle is on the platform -- a descriptor, a HANDLE primitive --
//  as hex, so these five values are the only thing this demo adds to its own
//  command line. The flag itself is the house one: hashspace re-executes itself
//  the same way.
//
struct bb_child
{
   const char         *role;        // "worker" or "reader"
   unsigned int        index;       // which one it is
   mulle_mmap_file_t   handle;
   void               *base;        // where the board has to be mapped
   size_t              capacity;
   struct bb_board    *board;       // the board in it, as the monitor sees it
};


//
//  shared board: one region, one spin lock, notes by pointer
//
struct bb_note
{
   mulle_atomic_intptr_t   refs;        // subscribers that still hold this note
   unsigned long long      seq;
   uint32_t                checksum;
   unsigned int            topic;
   unsigned int            worker;
   size_t                  length;
   // `length` payload bytes follow
};


//
//  mulle-fifo ships the ring family as fixed-size structs, from 4 to 8192
//  entries, with no allocation and no allocator pointer in them -- which is
//  what makes one safe to embed in the shared board. By contract it is single
//  producer, single consumer: the board lock serializes the producers, and each
//  mailbox has exactly one reader process. A full ring is a drop, never a wait.
//
typedef struct mulle__pointerfifo256   bb_mailbox_t;

#define bb_mailbox_init( p)         _mulle__pointerfifo256_init( p)
#define bb_mailbox_read( p)         _mulle__pointerfifo256_read( p)
#define bb_mailbox_write( p, q)     _mulle__pointerfifo256_write( p, q)
#define bb_mailbox_count( p)        _mulle__pointerfifo256_get_count( p)


struct bb_subscriber
{
   bb_mailbox_t                  mailbox;
   uint64_t                      topic_mask;
   mulle_atomic_intptr_t         delivered;
   mulle_atomic_intptr_t         dropped;
   mulle_atomic_intptr_t         corrupt;
   mulle_atomic_intptr_t         bytes;
};


struct bb_board
{
   unsigned int            magic;
   unsigned int            topics;
   unsigned int            workers;
   unsigned int            subscribers;
   unsigned int            seed;
   size_t                  capacity;    // the region the board starts with
   size_t                  limit;       // the ceiling a growable board may reach
   size_t                  grown;       // what it did grow to (the monitor's report)
   unsigned int            segments;    // segments the arena ended up with
   size_t                  overhead;    // the board's own chunk

   mulle_atomic_intptr_t   lock;        // board spin lock, shared by all processes
   mulle_atomic_intptr_t   closing;     // the monitor sets this when workers are reaped

   mulle_atomic_intptr_t   published;   // notes whose deliveries were offered
   mulle_atomic_intptr_t   skipped;     // nobody subscribed to that topic
   mulle_atomic_intptr_t   attempted;   // offers, one per interested subscriber
   mulle_atomic_intptr_t   allocated;
   mulle_atomic_intptr_t   freed;
   mulle_atomic_intptr_t   failures;    // the allocator refused
   mulle_atomic_intptr_t   live_notes;
   mulle_atomic_intptr_t   live_bytes;
   mulle_atomic_intptr_t   peak_bytes;
   mulle_atomic_intptr_t   payload_bytes;

   //  Who grew the arena. A process reports its own growth, and the monitor
   //  reads the totals once every process that could have grown is gone.
   mulle_atomic_intptr_t   monitor_grown;      // segments the monitor added
   mulle_atomic_intptr_t   worker_grown;       // segments the workers added
   mulle_atomic_intptr_t   worker_growers;     // how many workers added any
   mulle_atomic_intptr_t   reader_from_grown;  // notes readers took from grown segments

   struct bb_subscriber    subscriber[];   // subscribers of them
};


struct bb_context
{
   struct mulle_mmap_allocator   allocator;
   struct mulle_allocator        *allocator_p;
   struct bb_board               *board;
   struct bb_options             *options;

   //  Out of room: the allocator's `fail` vector longjmps to the worker's loop.
   //  What must survive that jump lives here rather than in a local of the frame
   //  that called setjmp(), because locals of that frame are indeterminate
   //  afterwards -- and the loop's position is exactly what has to survive.
   jmp_buf                       oom;
   int                           oom_armed;
   unsigned long long            cursor;
};


// the context of whichever process is running: the monitor makes the board, a
// child attaches to it, and each of them has this one to point its fail vector at
static struct bb_context   *bb_context;


//
//  The board lock: one spin lock in the shared region, serializing writers in
//  every process. It is C11 atomics over shared pages, so it works between
//  processes for the same reason it works between threads.
//
//  Note what it does *not* protect: the note reference counts and the counters
//  are plain atomics, and notes are freed by whichever process releases the last
//  reference. The lock covers the board structure only.
//
static void   bb_lock( struct bb_board *board)
{
   unsigned int   spins;

   for( spins = 0;; spins++)
   {
      if( _mulle_atomic_pointer_cas( (mulle_atomic_pointer_t *) &board->lock,
                                     (void *) 1, (void *) 0))
         return;
      if( (spins & 0xFF) == 0xFF)
         blackboard_yield();
   }
}


static void   bb_unlock( struct bb_board *board)
{
   _mulle_atomic_pointer_write_relaxed( (mulle_atomic_pointer_t *) &board->lock, (void *) 0);
}


static intptr_t   bb_read( mulle_atomic_intptr_t *address)
{
   return( (intptr_t) _mulle_atomic_intptr_read_relaxed( address));
}


static void   bb_atomic_max( mulle_atomic_intptr_t *address, intptr_t value)
{
   intptr_t   old;

   for(;;)
   {
      old = bb_read( address);
      if( value <= old)
         return;
      if( _mulle_atomic_pointer_cas( (mulle_atomic_pointer_t *) address,
                                     (void *) value, (void *) old))
         return;
   }
}


//
//  Running out of room, which is the ordinary event for a fixed region.
//
//  multigrep's and windowrb's allocators abort on failure, which is right for
//  them: neither has anything better to do. Here it is a normal outcome -- the
//  region is fixed by --capacity, and a full board must drop a note, not kill a
//  worker.
//
//  The public allocator wrappers route a failure to the `fail` vector, and that
//  vector is MULLE_C_NO_RETURN: it cannot simply report and hand back a NULL. So
//  the handler longjmps.
//
//  That is sound here for a specific reason: `fail` is invoked by the wrapper in
//  mulle-allocator.h, *after* mspace_realloc() has returned, so dlmalloc's own
//  lock is already released and the shared heap is consistent when the jump
//  lands. It is also why the jump target is inside bb_alloc() and not higher up:
//  the only thing that has to be undone on the way out is the board lock, which
//  the caller on the stack below still holds and still releases.
//
//  The alternative is to skip the wrapper and call the `reallocarray` vector
//  directly, which is the only way to get a NULL without a jump, but then the
//  vector that the library documents as its one failure funnel is bypassed.
//
MULLE_C_NO_RETURN
static void   bb_fail( struct mulle_allocator *allocator, void *block, size_t size)
{
   if( bb_context && bb_context->oom_armed)
   {
      // counted here, at the moment of the refusal, so that the tally does not
      // depend on where the jump lands and what happens next
      if( bb_context->board)
         _mulle_atomic_intptr_increment_relaxed( &bb_context->board->failures);
      longjmp( bb_context->oom, 1);
   }

   // Not an allocation the board made, or no handler armed: the default answer.
   // Note it must be mulle_allocation_fail() and not _mulle_allocator_abort(),
   // which calls back into *this* vector and recurses until the stack is gone.
   mulle_allocation_fail( allocator, block, size);
}


//
//  The allocation itself. No setjmp here: the jump target is the loop, so that a
//  board that is full is paid for once, not on every note. The caller must have
//  armed the handler and be somewhere a jump can be resumed from.
//
static void   *bb_alloc( struct bb_context *context, size_t size)
{
   return( mulle_allocator_malloc( context->allocator_p, size));
}


//
//  note contents: deterministic in (seq, seed), so a reader can prove that what
//  it received is what was published, byte for byte, across processes.
//
static uint64_t   bb_scramble( uint64_t x)
{
   x ^= x >> 33;
   x *= 0xff51afd7ed558ccdULL;
   x ^= x >> 33;
   x *= 0xc4ceb9fe1a85ec53ULL;
   x ^= x >> 33;
   return( x);
}


static void   bb_note_fill( unsigned char *payload,
                            size_t length,
                            unsigned long long seq,
                            unsigned int seed)
{
   uint64_t   x;
   size_t     i;

   x = seq ^ ((uint64_t) seed << 32);

   for( i = 0; i < length; i++)
   {
      x = x * 6364136223846793005ULL + 1442695040888963407ULL;
      payload[ i] = (unsigned char) (x >> 33);
   }
}


static uint32_t   bb_checksum( unsigned char *payload, size_t length)
{
   uint32_t   hash;
   size_t     i;

   hash = 2166136261u;
   for( i = 0; i < length; i++)
      hash = (hash ^ payload[ i]) * 16777619u;
   return( hash);
}


static size_t   bb_note_length( struct bb_options *options, unsigned long long seq)
{
   uint32_t   r;

   r = (uint32_t) (bb_scramble( seq) >> 32);
   return( 16 + (size_t) (r % (uint32_t) options->payload));
}


//
//  One reference goes away. The last one frees the note, whoever it is —
//  the freeing process is usually not the allocating one.
//
static void   bb_note_release( struct bb_context *context,
                               struct bb_note *note,
                               size_t bytes)
{
   struct bb_board  *board;

   board = context->board;

   if( (intptr_t) _mulle_atomic_pointer_decrement(
          (mulle_atomic_pointer_t *) &note->refs) != 1)
      return;

   mulle_allocator_free( context->allocator_p, note);
   _mulle_atomic_intptr_increment_relaxed( &board->freed);
   _mulle_atomic_intptr_add_relaxed( &board->live_notes, -1);
   _mulle_atomic_intptr_add_relaxed( &board->live_bytes, -(intptr_t) bytes);
}


//
//  publish: all subscribers of the note's topic get the *same* pointer, and the
//  note lives until the last of them is done with it.
//
//  It happens in two halves, and only the second one needs the board lock.
//  Which subscribers want this topic comes from a table that is filled in the
//  parent before any fork and never written again, so the note can be allocated,
//  filled, checksummed and refcounted outside the lock. That is worth two things:
//  the critical section is then just the enqueue, and the one operation here that
//  can fail happens where there is no lock to leave held when `fail` jumps.
//
//  Nobody can reach the note before the second half runs -- the only route to one
//  is a mailbox -- so filling it outside the lock cannot be observed.
//
static int   bb_publish( struct bb_context *context, unsigned int worker, unsigned long long seq)
{
   struct bb_options     *options;
   struct bb_board       *board;
   struct bb_subscriber  *subscriber;
   struct bb_note        *note;
   size_t                 length;
   size_t                 bytes;
   unsigned int           topic;
   unsigned int           interested;
   unsigned int           i;
   intptr_t               ticket;
   int                    fits;

   ticket = 0;

   options = context->options;
   board   = context->board;
   topic   = (unsigned int) (bb_scramble( seq ^ (uint64_t) options->seed) % board->topics);

   interested = 0;
   for( i = 0; i < board->subscribers; i++)
      if( board->subscriber[ i].topic_mask & (1ULL << topic))
         interested++;

   if( ! interested)
   {
      _mulle_atomic_intptr_increment_relaxed( &board->skipped);
      return( 0);
   }

   length = bb_note_length( options, seq);
   bytes  = sizeof( struct bb_note) + length;

   //
   // --unsafe: the tally is read here, and written back after the allocation
   // and the enqueue, with no lock around the pair. The window is the whole
   // publish, which is the realistic way to get this wrong, and it loses
   // updates at a rate you can measure.
   //
   // The mailboxes stay serialized either way. A mulle-fifo ring is single
   // producer by contract, and racing its count is not a wrong *number*: two
   // writers can both bump it, the consumer then reads slots nobody wrote, and
   // a stale slot still points at a note that has since been freed. That is a
   // double free, not a drift, and this demo would rather show the drift.
   //
   if( options->unsafe)
      ticket = bb_read( &board->published);

   // no lock is held here, so a board that is full is simply refused by jumping
   note = bb_alloc( context, bytes);

   bb_note_fill( (unsigned char *) (note + 1), length, seq, options->seed);
   if( options->perturb)
      ((unsigned char *) (note + 1))[ length / 2] ^= 0x5A;
   note->seq      = seq;
   note->topic    = topic;
   note->worker   = worker;
   note->length   = length;
   note->checksum = bb_checksum( (unsigned char *) (note + 1), length);
   _mulle_atomic_pointer_write_relaxed( (mulle_atomic_pointer_t *) &note->refs,
                                        (void *) (intptr_t) interested);

   _mulle_atomic_intptr_increment_relaxed( &board->allocated);
   _mulle_atomic_intptr_increment_relaxed( &board->live_notes);
   _mulle_atomic_intptr_add_relaxed( &board->live_bytes, (intptr_t) bytes);
   _mulle_atomic_intptr_add_relaxed( &board->payload_bytes, (intptr_t) length);
   bb_atomic_max( &board->peak_bytes, bb_read( &board->live_bytes));

   bb_lock( board);

   for( i = 0; i < board->subscribers; i++)
   {
      subscriber = &board->subscriber[ i];
      if( ! (subscriber->topic_mask & (1ULL << topic)))
         continue;

      _mulle_atomic_intptr_increment_relaxed( &board->attempted);

      fits = bb_mailbox_write( &subscriber->mailbox, note);
      if( ! fits)
         continue;

      // mailbox full: this subscriber misses the note, the note loses a reader
      _mulle_atomic_intptr_increment_relaxed( &subscriber->dropped);
      bb_note_release( context, note, bytes);
   }

   if( options->unsafe)
      _mulle_atomic_intptr_write_relaxed( &board->published, ticket + 1);
   else
      _mulle_atomic_intptr_increment_relaxed( &board->published);

   bb_unlock( board);
   return( 1);
}


//
//  receive: copy the payload out and verify it against what (seq, seed)
//  prescribe, so a reader proves that what it got is what was published. The
//  note is released afterwards, which may free it.
//
struct bb_seen
{
   unsigned long long   seq;
   unsigned int         topic;
   unsigned int         worker;
   size_t               length;
   int                  corrupt;
   int                  grown;      // this note lives past the first segment
};


static int   bb_receive( struct bb_context *context,
                         unsigned int id,
                         unsigned char *payload,
                         size_t payload_size,
                         struct bb_seen *seen)
{
   struct bb_options     *options;
   struct bb_subscriber  *subscriber;
   struct bb_note        *note;
   unsigned char          expect[ BB_MAX_PAYLOAD];
   size_t                 bytes;
   int                    corrupt;

   options    = context->options;
   subscriber = &context->board->subscriber[ id];

   note = bb_mailbox_read( &subscriber->mailbox);
   if( ! note)
      return( 0);

   bytes = sizeof( struct bb_note) + note->length;

   _mulle_atomic_intptr_increment_relaxed( &subscriber->delivered);
   _mulle_atomic_intptr_add_relaxed( &subscriber->bytes, (intptr_t) note->length);

   if( note->length > payload_size || note->length > BB_MAX_PAYLOAD)
      corrupt = 1;
   else
   {
      bb_note_fill( expect, note->length, note->seq, options->seed);
      corrupt = (bb_checksum( (unsigned char *) (note + 1), note->length) != note->checksum)
                || memcmp( expect, (unsigned char *) (note + 1), note->length);
      memcpy( payload, (unsigned char *) (note + 1), note->length);
   }

   if( corrupt)
      _mulle_atomic_intptr_increment_relaxed( &subscriber->corrupt);

   if( seen)
   {
      seen->seq     = note->seq;
      seen->topic   = note->topic;
      seen->worker  = note->worker;
      seen->length  = note->length;
      seen->corrupt = corrupt;
      seen->grown   = (size_t) ((char *) note - (char *) context->allocator.base) >=
                      mulle_mmap_allocator_get_initial_capacity( &context->allocator);
   }

   bb_note_release( context, note, bytes);
   return( corrupt ? -1 : 1);
}


//
//  board setup, in the parent, before any fork
//
static struct bb_board   *bb_board_create( struct bb_context *context,
                                           unsigned int subscribers,
                                           unsigned int topics)
{
   struct bb_options  *options;
   struct bb_board    *board;
   size_t              size;

   options = context->options;
   size = sizeof( struct bb_board) + subscribers * sizeof( struct bb_subscriber);

   board = bb_alloc( context, size);
   if( ! board)
   {
      mulle_fprintf( stderr, "%s: cannot fit a %u subscriber board in %lu bytes\n",
                     "blackboard", subscribers, (unsigned long) options->capacity);
      return( NULL);
   }

   memset( board, 0, size);

   board->magic       = BB_MAGIC;
   board->topics      = topics;
   board->workers     = options->workers;
   board->subscribers = subscribers;
   board->seed        = options->seed;
   board->capacity    = options->capacity;      // the region as asked for
   board->limit       = options->growth_limit;  // == capacity when it cannot grow
   board->overhead    = size;

   context->board = board;
   return( board);
}


static void   bb_subscribe( struct bb_context *context,
                            unsigned int id,
                            uint64_t mask)
{
   struct bb_subscriber  *subscriber;

   subscriber = &context->board->subscriber[ id];
   bb_mailbox_init( &subscriber->mailbox);
   subscriber->topic_mask = mask;
}


//
//  roles. A child must _exit() rather than return, so that it never runs the
//  parent's cleanup: an inherited board may only be destroyed by the process
//  that created it. The fork sites do that, which keeps these two callable from
//  a single process.
//
static void   bb_worker_main( struct bb_context *context, unsigned int worker)
{
   struct bb_options  *options;
   unsigned long long  total;

   options = context->options;
   total   = (unsigned long long) options->notes;

   //
   //  One landing site for the whole run, outside the loop. An allocation that
   //  finds the board full abandons bb_publish() from the inside and lands here,
   //  and because the cursor lives in the context the loop resumes exactly where
   //  it was rather than starting over.
   //
   //  The refused note is not retried: a bounded board refuses, and retrying it
   //  would turn a full board into a spin. The run continues with the next note.
   //
   context->cursor = 0;

   if( setjmp( context->oom))
      context->cursor++;

   context->oom_armed = 1;

   while( context->cursor < total)
   {
      bb_publish( context, worker,
                  (unsigned long long) worker * total + context->cursor);
      context->cursor++;
   }

   context->oom_armed = 0;

   //
   //  Attribute the growth: grow_count is this process's own, so a worker knows
   //  what it added, and the board sums it. That is what lets the report say the
   //  board grew from the workers rather than from the monitor.
   //
   if( mulle_mmap_allocator_get_grow_count( &context->allocator))
      _mulle_atomic_intptr_increment_relaxed( &context->board->worker_growers);
   _mulle_atomic_intptr_add_relaxed(
      &context->board->worker_grown,
      (intptr_t) mulle_mmap_allocator_get_grow_count( &context->allocator));
}


static void   bb_reader_main( struct bb_context *context, unsigned int id)
{
   struct bb_board       *board;
   struct bb_seen         seen;
   unsigned char          payload[ BB_MAX_PAYLOAD];
   unsigned long long     from_grown;
   unsigned int           idle;
   unsigned int           quiet;

   board      = context->board;
   idle       = 0;
   quiet      = 0;
   from_grown = 0;

   for(;;)
   {
      if( bb_receive( context, id, payload, sizeof( payload), &seen))
      {
         //
         //  A reader never allocates, so it cannot have grown the arena itself.
         //  A note beyond the first segment is therefore one *somebody else*
         //  made room for, and this process mapped and used it all the same.
         //
         if( seen.grown)
            from_grown++;
         idle  = 0;
         quiet = 0;
         continue;
      }

      //
      // Shutdown. Once the monitor has reaped every worker nothing new can be
      // published, so a mailbox that stays quiet is finished. Waiting for the
      // mailbox *count* to reach zero instead is what --unsafe teaches: two
      // racing writers can both bump that counter, and then it never returns
      // to zero and the reader waits forever.
      //
      if( bb_read( &board->closing))
      {
         if( ++quiet > 1000)
            break;
         blackboard_yield();
         continue;
      }

      // nothing to do: there is no blocking read here, so back off instead of
      // burning a core while the workers are busy
      if( ++idle > 64)
      {
         mulle_relativetime_sleep( 0.0005);
         idle = 0;
      }
      else
         blackboard_yield();
   }

   _mulle_atomic_intptr_add_relaxed( &board->reader_from_grown,
                                     (intptr_t) from_grown);
}


//
//  output
//
static char   *bb_size_string( char *buf, size_t buf_size, double value)
{
   char        *unit;
   int          i;

   unit = "B";
   for( i = 0; i < 2; i++)
   {
      if( value < 1024.0)
         break;
      value /= 1024.0;
      unit = (i == 0) ? "KiB" : "MiB";
   }

   snprintf( buf, buf_size, "%.1f %s", value, unit);
   return( buf);
}


struct bb_totals
{
   unsigned long long   published;
   unsigned long long   skipped;
   unsigned long long   attempted;
   unsigned long long   allocated;
   unsigned long long   freed;
   unsigned long long   failures;
   unsigned long long   live_notes;
   unsigned long long   live_bytes;
   unsigned long long   peak_bytes;
   unsigned long long   payload_bytes;
   unsigned long long   delivered;
   unsigned long long   dropped;
   unsigned long long   corrupt;
};


static void   bb_totals_gather( struct bb_board *board, struct bb_totals *totals)
{
   unsigned int   i;

   memset( totals, 0, sizeof( struct bb_totals));

   totals->published     = (unsigned long long) bb_read( &board->published);
   totals->skipped       = (unsigned long long) bb_read( &board->skipped);
   totals->attempted     = (unsigned long long) bb_read( &board->attempted);
   totals->allocated     = (unsigned long long) bb_read( &board->allocated);
   totals->freed         = (unsigned long long) bb_read( &board->freed);
   totals->failures      = (unsigned long long) bb_read( &board->failures);
   totals->live_notes    = (unsigned long long) bb_read( &board->live_notes);
   totals->live_bytes    = (unsigned long long) bb_read( &board->live_bytes);
   totals->peak_bytes    = (unsigned long long) bb_read( &board->peak_bytes);
   totals->payload_bytes = (unsigned long long) bb_read( &board->payload_bytes);

   for( i = 0; i < board->subscribers; i++)
   {
      totals->delivered += (unsigned long long) bb_read( &board->subscriber[ i].delivered);
      totals->dropped   += (unsigned long long) bb_read( &board->subscriber[ i].dropped);
      totals->corrupt   += (unsigned long long) bb_read( &board->subscriber[ i].corrupt);
   }
}


//
//  The live report: what the monitor paints while the workers are still
//  running. Its one caller is the monitor loop.
//
static void   bb_print_live( struct bb_board *board,
                             double elapsed,
                             int tui,
                             int first)
{
   struct bb_totals   totals;
   char               live[ 32];
   char               peak[ 32];
   char               rate[ 32];
   double             notes_per_s;
   double             payload_per_s;

   bb_totals_gather( board, &totals);

   notes_per_s   = elapsed > 0.0 ? (double) totals.published / elapsed : 0.0;
   payload_per_s = elapsed > 0.0 ? (double) totals.payload_bytes / elapsed : 0.0;

   bb_size_string( live, sizeof( live), (double) totals.live_bytes);
   bb_size_string( peak, sizeof( peak), (double) totals.peak_bytes);
   bb_size_string( rate, sizeof( rate), payload_per_s);

   if( ! tui)
   {
      mulle_fprintf( stdout,
                     "%7.2f s  published %llu  delivered %llu  dropped %llu"
                     "  failures %llu  live %s\n",
                     elapsed, totals.published, totals.delivered, totals.dropped,
                     totals.failures, live);
      return;
   }

   if( first)
      mulle_fprintf( stdout, "\033[H\033[J");

   mulle_fprintf( stdout,
                  " blackboard  %lu processes  board %lu B  topics %u  mailbox %u deep\033[K\n",
                  (unsigned long) (board->workers + board->subscribers),
                  (unsigned long) board->capacity,
                  board->topics, (unsigned int) BB_MAILBOX_DEPTH);
   mulle_fprintf( stdout,
                  " published %11llu   delivered %11llu   dropped %9llu   failures %llu\033[K\n",
                  totals.published, totals.delivered, totals.dropped, totals.failures);
   mulle_fprintf( stdout,
                  " live      %11s   peak      %11s   corrupt  %9llu\033[K\n",
                  live, peak, totals.corrupt);
   mulle_fprintf( stdout,
                  " rate      %11.0f/s %15s/s   elapsed  %7.2f s\033[K\n",
                  notes_per_s, rate, elapsed);
}


static int   bb_verify( struct bb_board *board,
                        struct bb_options *options,
                        struct bb_totals *totals)
{
   unsigned long long   expected;
   unsigned long long   accounted;
   int                  failures;

   failures = 0;
   expected = (unsigned long long) options->workers * options->notes;

   mulle_fprintf( stdout, "\n");

   //
   // A note either got published or was refused for want of room. Both are
   // counted, so the total must account for every note that was offered; what
   // must never happen is a note that goes missing between the two.
   //
   if( totals->published + totals->failures != expected)
   {
      mulle_fprintf( stdout,
                     "   tally            %llu published + %llu refused of %llu offered,"
                     " %lld lost updates\n",
                     totals->published, totals->failures, expected,
                     (long long) (expected - totals->published - totals->failures));
      mulle_fprintf( stdout,
                     "                    the notes themselves are accounted for below;"
                     " it is the count a monitor reads that is wrong\n");
      failures++;
   }

   accounted = totals->delivered + totals->dropped;
   if( accounted != totals->attempted)
   {
      mulle_fprintf( stdout, "   deliveries       %llu accounted, %llu attempted (lost %llu)\n",
                     accounted, totals->attempted,
                     totals->attempted > accounted ? totals->attempted - accounted : 0);
      failures++;
   }

   if( totals->allocated != totals->freed + totals->live_notes)
   {
      mulle_fprintf( stdout, "   heap             %llu allocated, %llu freed, %llu live\n",
                     totals->allocated, totals->freed, totals->live_notes);
      failures++;
   }

   if( totals->live_notes)
   {
      mulle_fprintf( stdout, "   live notes       %llu still in the board (%llu bytes)\n",
                     totals->live_notes, totals->live_bytes);
      failures++;
   }

   if( totals->corrupt)
   {
      mulle_fprintf( stdout, "   contents         %llu of %llu delivered notes did not verify\n",
                     totals->corrupt, totals->delivered);
      failures++;
   }

   if( totals->published && ! totals->delivered)
   {
      mulle_fprintf( stdout, "   deliveries       nothing arrived at any subscriber\n");
      failures++;
   }

   if( totals->live_bytes > board->limit || totals->peak_bytes > board->limit)
   {
      mulle_fprintf( stdout, "   capacity         peak %llu bytes of %lu\n",
                     totals->peak_bytes, (unsigned long) board->limit);
      failures++;
   }

   if( ! failures)
      mulle_fprintf( stdout, " verification      ok: every note accounted for, deliveries conserved,"
                             " board empty, contents intact%s\n",
                     totals->failures ? " (within the room the board had)" : "");
   else
      mulle_fprintf( stdout, " verification      FAILED (%d)\n", failures);

   return( failures ? 1 : 0);
}


static void   bb_print_summary( struct bb_board *board,
                                struct bb_options *options,
                                struct bb_totals *totals,
                                double elapsed)
{
   char              live[ 32];
   char              peak[ 32];
   char              payload[ 32];
   char              board_size[ 32];
   char              overhead[ 32];
   char              limit[ 32];
   char              grown[ 32];
   unsigned int      i;
   double            notes_per_s;
   double            deliveries_per_s;
   unsigned long long average;

   bb_size_string( board_size, sizeof( board_size), (double) board->capacity);
   bb_size_string( overhead, sizeof( overhead), (double) board->overhead);
   bb_size_string( live, sizeof( live), (double) totals->live_bytes);
   bb_size_string( peak, sizeof( peak), (double) totals->peak_bytes);
   bb_size_string( payload, sizeof( payload), (double) totals->payload_bytes);

   notes_per_s      = elapsed > 0.0 ? (double) totals->published / elapsed : 0.0;
   deliveries_per_s = elapsed > 0.0
                      ? (double) (totals->delivered + totals->dropped) / elapsed : 0.0;
   average          = totals->published ? totals->payload_bytes / totals->published : 0;

   mulle_fprintf( stdout, "\n=== Blackboard ===\n");
   if( board->limit > board->capacity)
   {
      bb_size_string( limit, sizeof( limit), (double) board->limit);
      mulle_fprintf( stdout,
                     " region            %s shared growing to %s, one mapping, %lu processes\n",
                     board_size, limit,
                     (unsigned long) (options->workers + board->subscribers));
   }
   else
      mulle_fprintf( stdout, " region            %s shared, one mapping, %lu processes\n",
                     board_size, (unsigned long) (options->workers + board->subscribers));
   mulle_fprintf( stdout, " board overhead    %s (%.1f %% of the region)\n",
                  overhead, board->capacity ? 100.0 * (double) board->overhead / (double) board->capacity : 0.0);
   mulle_fprintf( stdout, " published         %llu notes, %llu skipped (no subscriber)\n",
                  totals->published, totals->skipped);
   mulle_fprintf( stdout, " payload           %s in %llu notes, %llu B average\n",
                  payload, totals->published, average);
   mulle_fprintf( stdout, " deliveries        %llu arrived, %llu dropped (mailbox full),"
                          " %llu lost\n",
                  totals->delivered, totals->dropped,
                  totals->attempted > totals->delivered + totals->dropped
                     ? totals->attempted - (totals->delivered + totals->dropped) : 0);
   mulle_fprintf( stdout, " allocator         %llu failures, %llu allocations, %llu frees\n",
                  totals->failures, totals->allocated, totals->freed);
   mulle_fprintf( stdout, " board heap        %s live, %s peak, %s of the region\n",
                  live, peak, board->limit > board->capacity ? limit : board_size);
   if( board->limit > board->capacity)
   {
      bb_size_string( grown, sizeof( grown), (double) board->grown);
      mulle_fprintf( stdout, " growth            %u segment%s, %s of %s limit\n",
                     board->segments, board->segments == 1 ? "" : "s",
                     grown, limit);
      mulle_fprintf( stdout,
                     " growth source     monitor added %llu segments,"
                     " %llu of %u workers added %llu,"
                     " readers took %llu notes from grown segments\n",
                     (unsigned long long) bb_read( &board->monitor_grown),
                     (unsigned long long) bb_read( &board->worker_growers),
                     board->workers,
                     (unsigned long long) bb_read( &board->worker_grown),
                     (unsigned long long) bb_read( &board->reader_from_grown));
   }

   mulle_fprintf( stdout, " subscriber        delivered    dropped    corrupt   topics\n");
   for( i = 0; i < board->subscribers; i++)
   {
      char           label[ 32];
      char           topics[ 32];
      size_t         length;
      unsigned int   count;
      unsigned int   t;

      length = (size_t) snprintf( label, sizeof( label), "%u%s",
                                  i, i == 0 ? " (monitor)" : "");
      if( length >= sizeof( label))
         length = sizeof( label) - 1;
      while( length < 16 && length + 1 < sizeof( label))
         label[ length++] = ' ';
      label[ length] = 0;

      count = 0;
      for( t = 0; t < board->topics; t++)
         count += (board->subscriber[ i].topic_mask >> t) & 1;
      snprintf( topics, sizeof( topics), "%u/%u", count, board->topics);

      mulle_fprintf( stdout, "   %s %10llu %10llu %10llu   %s\n",
                     label,
                     (unsigned long long) bb_read( &board->subscriber[ i].delivered),
                     (unsigned long long) bb_read( &board->subscriber[ i].dropped),
                     (unsigned long long) bb_read( &board->subscriber[ i].corrupt),
                     topics);
   }
   mulle_fprintf( stdout, " contents          %llu subscribers verified %llu payloads, %llu corrupt\n",
                  (unsigned long) board->subscribers, totals->delivered, totals->corrupt);
   mulle_fprintf( stdout, " wall clock        %.1f ms   %.0f notes/s   %s/s   %.0f deliveries/s\n",
                  elapsed * 1000.0, notes_per_s, payload, deliveries_per_s);
}


//
//  the monitor: subscriber 0. It drains its own mailbox for the whole run, so
//  --show can print notes in order from a single process.
//
//  It also reaps: a worker that has stopped is collected here while the others
//  keep going, so the report runs to the end of the slowest one. A zero timeout
//  on mulle_spawn_wait is the poll that waitpid's WNOHANG used to be.
//
static void   bb_monitor_loop( struct bb_context *context,
                               struct bb_options *options,
                               struct mulle_spawn_handle *workers,
                               unsigned int worker_count,
                               unsigned int *reaped)
{
   struct bb_board    *board;
   struct bb_seen      seen;
   unsigned char       payload[ BB_MAX_PAYLOAD];
   double              start;
   double              now;
   double              interval;
   double              next_report;
   unsigned long long  shown;
   int                 first;
   struct mulle_spawn_result   result;
   unsigned int        i;
   unsigned int        quiet;

   board  = context->board;
   start  = mulle_relativetime_now();
   shown  = 0;
   first  = 1;
   *reaped = 0;

   //
   // The default cadence is the display's: a tenth of a second for the tui,
   // half a second for one line per report. --interval overrides it, which is
   // what lets a test reach the inline report on a machine fast enough to
   // finish the whole run inside the default -- the Mac does 400000 notes in
   // 235 ms, so a fixed half second is a test of the machine, not the demo.
   //
   interval = options->interval > 0.0 ? options->interval
                                      : (options->tui ? 0.1 : 0.5);

   // a report at t=0 would divide by an elapsed time of microseconds
   next_report = start + interval;

   for(;;)
   {
      while( bb_receive( context, 0, payload, sizeof( payload), &seen) > 0)
      {
         if( shown < options->show)
         {
            mulle_fprintf( stdout, " [%llu] worker %u -> monitor, topic %u, %lu B, %s\n",
                           seen.seq, seen.worker, seen.topic,
                           (unsigned long) seen.length,
                           seen.corrupt ? "CORRUPT" : "verified");
            shown++;
         }
      }

      now = mulle_relativetime_now();
      if( ! options->quiet && now >= next_report)
      {
         bb_print_live( board, now - start, options->tui, first && options->tui);
         first       = 0;
         next_report = now + interval;
         if( ! options->tui)
            fflush( stdout);
      }

      if( *reaped < worker_count)
      {
         for( i = 0; i < worker_count; i++)
         {
            if( ! workers[ i].opened)
               continue;
            if( mulle_spawn_wait( &workers[ i], 0.0, &result) != 1)
               continue;

            mulle_spawn_close( &workers[ i], NULL);

            (*reaped)++;
            if( result.status != mulle_spawn_exited || result.exit_code)
            {
               mulle_fprintf( stderr, "blackboard: worker %u died (exit %d, status %d)\n",
                              i, result.exit_code, result.status);
               exit( 1);
            }
         }
      }
      else
         break;

      mulle_relativetime_sleep( 0.0002);
   }

   // workers are gone: tell the readers to drain and quit, then take what is
   // left in the monitor's own mailbox
   _mulle_atomic_intptr_write_relaxed( &board->closing, 1);

   for( quiet = 0; quiet < 2;)
   {
      if( bb_receive( context, 0, payload, sizeof( payload), NULL))
      {
         quiet = 0;
         continue;
      }
      quiet++;
   }
}


//
//  The child side of the same demo: attach the board the monitor made, play the
//  part that was asked for, then leave without touching the board's lifetime.
//
//  Nothing is destroyed here, on purpose: the mapping and the board in it belong
//  to the monitor, which created them and outlives every child. A child that
//  called the allocator's done would tear down the whole family's board.
//
static int   bb_child_main( struct bb_options *options, struct bb_child *child)
{
   struct bb_context   context;

   memset( &context, 0, sizeof( context));

   mulle_mmap_allocator_attach( &context.allocator,
                                child->handle,
                                child->capacity,
                                child->base);

   context.allocator_p = mulle_mmap_allocator_as_allocator( &context.allocator);
   context.options     = options;
   context.board       = child->board;

   // the same recoverable path as the monitor: a board with no room is a
   // refusal to count, not the end of this process
   bb_context                = &context;
   context.allocator_p->fail = bb_fail;

   if( ! strcmp( child->role, "worker"))
      bb_worker_main( &context, child->index);
   else
      bb_reader_main( &context, child->index);

   return( 0);
}


//
//  Starting a worker or a reader: the monitor's own command line, unchanged, so
//  that a child parses the identical options and runs the identical demo, plus
//  the five values above. Streams are inherited -- a child writes where the
//  monitor writes -- and the monitor keeps the handle open for as long as a
//  child might need it.
//
static int   bb_spawn( struct mulle_spawn_handle *h,
                       struct bb_context *context,
                       int argc,
                       char *argv[],
                       const char *role,
                       unsigned int index)
{
   char     self[ 4096];
   char     index_str[ 32];
   char     handle_str[ 32];
   char     base_str[ 32];
   char     capacity_str[ 32];
   char     board_str[ 32];
   char   **child_argv;
   int      i;
   int      rval;

   snprintf( index_str,    sizeof( index_str),    "%u", index);
   snprintf( handle_str,   sizeof( handle_str),   "%llx",
             (unsigned long long) (uintptr_t) context->allocator.shared_handle);
   snprintf( base_str,     sizeof( base_str),     "%llx",
             (unsigned long long) (uintptr_t) context->allocator.base);
   snprintf( capacity_str, sizeof( capacity_str), "%llx",
             (unsigned long long) context->allocator.capacity);
   snprintf( board_str,    sizeof( board_str),    "%llx",
             (unsigned long long) (uintptr_t) context->board);

   child_argv = mulle_malloc( sizeof( char *) * (argc + 8));
   for( i = 0; i < argc; i++)
      child_argv[ i] = argv[ i];

   //  argv[0] stays what it was unless the platform will name the program
   //  outright, which it will on both of them. The child is the program that is
   //  already running, not whatever it was called by.
   if( mulle_spawn_get_executable_path( self, sizeof( self)) >= 0)
      child_argv[ 0] = self;

   i                = argc;
   child_argv[ i++] = (char *) "--internal-child";
   child_argv[ i++] = (char *) role;
   child_argv[ i++] = index_str;
   child_argv[ i++] = handle_str;
   child_argv[ i++] = base_str;
   child_argv[ i++] = capacity_str;
   child_argv[ i++] = board_str;
   child_argv[ i]   = NULL;

   rval = mulle_spawn_open( h, child_argv,
                            MULLE_SPAWN_INHERIT,
                            MULLE_SPAWN_INHERIT,
                            MULLE_SPAWN_INHERIT);
   mulle_free( child_argv);
   return( rval);
}


static uint64_t   bb_parse_hex( const char *s)
{
   char        *end;
   uint64_t     value;

   value = strtoull( s, &end, 16);
   if( end == s || *end)
   {
      mulle_fprintf( stderr, "blackboard: --internal-child wants hex, got \"%s\"\n", s);
      exit( 1);
   }
   return( value);
}


static mulle_mmap_file_t   bb_parse_handle( const char *s)
{
   // a descriptor on POSIX, a HANDLE primitive on Windows: both travel as hex
#ifdef _WIN32
   return( (mulle_mmap_file_t) (uintptr_t) bb_parse_hex( s));
#else
   return( (mulle_mmap_file_t) (intptr_t) bb_parse_hex( s));
#endif
}


static unsigned int   bb_parse_unsigned( const char *s, const char *what)
{
   char              *end;
   unsigned long      value;

   value = strtoul( s, &end, 10);
   if( end == s || *end)
   {
      mulle_fprintf( stderr, "blackboard: %s wants a number, got \"%s\"\n", what, s);
      exit( 1);
   }
   return( (unsigned int) value);
}


static size_t   bb_parse_size( const char *s, const char *what)
{
   char        *end;
   double       value;
   char         suffix;

   value  = strtod( s, &end);
   suffix = *end;
   if( suffix == 'k' || suffix == 'K')
      value *= 1024.0;
   else
      if( suffix == 'm' || suffix == 'M')
         value *= 1024.0 * 1024.0;
      else
         if( suffix)
         {
            mulle_fprintf( stderr, "blackboard: %s wants bytes, with optional K or M, got \"%s\"\n",
                           what, s);
            exit( 1);
         }

   return( (size_t) value);
}


static void   bb_usage( void)
{
   mulle_fprintf( stderr,
      "usage: blackboard [options]\n"
      "   --workers N      publisher processes (default 4)\n"
      "   --readers N      reader processes, besides the monitor (default 2)\n"
      "   --topics N       topics in play (default 4)\n"
      "   --notes N        notes published per worker (default 20000)\n"
      "   --payload N      longest payload, notes are 16..N bytes (default 128)\n"
      "   --capacity SIZE  shared region, accepts K and M (default 256K)\n"
      "   --grow           expand the region on demand instead of refusing notes\n"
      "   --growth-limit SIZE\n"
      "                    ceiling it may grow to, accepts K and M (default 4M;\n"
      "                    implies --grow)\n"
      "   --seed N         seeds note contents and topic choice (default 1848)\n"
      "   --show N         print the first N notes the monitor receives\n"
      "   --unsafe         update the tally without the lock: read, publish, write back\n"
      "   --perturb        corrupt a payload byte after checksumming it (test hook)\n"
      "   --exhaust        leave no room, so publishing runs out of memory\n"
      "                    (--payload goes to the maximum, to make that true)\n"
      "   --dump           dump the shared heap when the run is over\n"
      "   --verify         assert the invariants, exit nonzero if they drift\n"
      "   --no-tui         one line per report instead of a redrawing display\n"
      "   --interval SEC   seconds between live reports (default 0.1, 0.5 with --no-tui)\n"
      "   --quiet          only the summary\n");
}


int   main( int argc, char *argv[])
{
   struct bb_options             options;
   struct bb_context             context;
   struct bb_board              *board;
   struct bb_totals              totals;
   unsigned int                  subscribers;
   unsigned int                  i;
   double                        start;
   double                        elapsed;
   int                           status;
   uint64_t                      mask;
   unsigned int                  topics;
   char                          board_size[ 32];
   struct bb_child               child;
   struct mulle_spawn_handle     workers[ BB_MAX_PROCESSES];
   struct mulle_spawn_handle     readers[ BB_MAX_PROCESSES];
   struct mulle_spawn_result     result;
   unsigned int                  reaped;

   memset( &options, 0, sizeof( options));
   memset( &child, 0, sizeof( child));
   options.workers  = 4;
   options.readers  = 2;
   options.topics   = 4;
   options.notes    = 20000;
   options.payload  = 128;
   options.capacity = 256 * 1024;
   options.seed     = 1848;
   options.quiet    = 0;
   options.tui      = blackboard_isatty();

   for( i = 1; i < (unsigned int) argc; i++)
   {
      if( ! strcmp( argv[ i], "--interval"))
      {
         options.interval = mulle_strtod( argv[ ++i], NULL);
         continue;
      }
      //  Not an option a person types: the monitor appends this to its own
      //  command line when it starts a child, and the child is that command
      //  line. Undocumented in --help on purpose.
      if( ! strcmp( argv[ i], "--internal-child"))
      {
         child.role     = argv[ ++i];
         child.index    = bb_parse_unsigned( argv[ ++i], "--internal-child");
         child.handle   = bb_parse_handle( argv[ ++i]);
         child.base     = (void *) (uintptr_t) bb_parse_hex( argv[ ++i]);
         child.capacity = (size_t) bb_parse_hex( argv[ ++i]);
         child.board    = (struct bb_board *) (uintptr_t) bb_parse_hex( argv[ ++i]);
         continue;
      }
      if( ! strcmp( argv[ i], "--workers"))
         options.workers = bb_parse_unsigned( argv[ ++i], "--workers");
      else
         if( ! strcmp( argv[ i], "--readers"))
            options.readers = bb_parse_unsigned( argv[ ++i], "--readers");
         else
            if( ! strcmp( argv[ i], "--topics"))
               options.topics = bb_parse_unsigned( argv[ ++i], "--topics");
            else
               if( ! strcmp( argv[ i], "--notes"))
                  options.notes = bb_parse_unsigned( argv[ ++i], "--notes");
               else
                  if( ! strcmp( argv[ i], "--payload"))
                     options.payload = bb_parse_size( argv[ ++i], "--payload");
                  else
                     if( ! strcmp( argv[ i], "--capacity"))
                        options.capacity = bb_parse_size( argv[ ++i], "--capacity");
                     else
                        if( ! strcmp( argv[ i], "--growth-limit"))
                           options.growth_limit = bb_parse_size( argv[ ++i], "--growth-limit");
                        else
                           if( ! strcmp( argv[ i], "--grow"))
                              options.grow = 1;
                           else
                              if( ! strcmp( argv[ i], "--seed"))
                                 options.seed = bb_parse_unsigned( argv[ ++i], "--seed");
                        else
                           if( ! strcmp( argv[ i], "--show"))
                              options.show = bb_parse_unsigned( argv[ ++i], "--show");
                           else
                              if( ! strcmp( argv[ i], "--unsafe"))
                                 options.unsafe = 1;
                              else
                                 if( ! strcmp( argv[ i], "--perturb"))
                                    options.perturb = 1;
                                 else
                                 if( ! strcmp( argv[ i], "--exhaust"))
                                 {
                                    //
                                    //  --exhaust is a whole meaning rather than a flag, and it has to
                                    //  say what it means here instead of after the parsing is done: a
                                    //  child is the monitor's command line and nothing else, so what
                                    //  the monitor settled on privately would never reach one. (That is
                                    //  what --internal-child reads, and the only reason the capacity
                                    //  is handed over separately.)
                                    //
                                    //  The region has to be big enough for the board itself --
                                    //  the arena header and its directory now cost the better
                                    //  part of a page -- and too small for the notes. The payload
                                    //  is what does the second half: at the largest the demo
                                    //  allows a note is a whole page, so a handful fit and the
                                    //  next one is refused, while the mailboxes are deep enough
                                    //  to keep hundreds in flight.
                                    //
                                    options.exhaust  = 1;
                                    options.capacity = 16 * 1024;
                                    options.payload  = BB_MAX_PAYLOAD - sizeof( struct bb_note);
                                 }
                                 else
                                    if( ! strcmp( argv[ i], "--dump"))
                                       options.dump = 1;
                                    else
                                       if( ! strcmp( argv[ i], "--verify"))
                                          options.verify = 1;
                                       else
                                          if( ! strcmp( argv[ i], "--quiet"))
                                             options.quiet = 1;
                                          else
                                             if( ! strcmp( argv[ i], "--no-tui"))
                                                options.tui = 0;
                                             else
                                                if( ! strcmp( argv[ i], "--help"))
                                                {
                                                   bb_usage();
                                                   return( 0);
                                                }
                                                else
                                                {
                                                   bb_usage();
                                                   return( 1);
                                                }
   }

   if( ! options.workers)
   {
      mulle_fprintf( stderr, "blackboard: --workers must be at least 1\n");
      return( 1);
   }
   if( ! options.topics || options.topics > BB_MAX_TOPICS)
   {
      mulle_fprintf( stderr, "blackboard: --topics must be 1..%d\n", BB_MAX_TOPICS);
      return( 1);
   }
   if( options.payload < 17 || options.payload > BB_MAX_PAYLOAD - sizeof( struct bb_note))
   {
      mulle_fprintf( stderr, "blackboard: --payload must be 17..%lu\n",
                     (unsigned long) (BB_MAX_PAYLOAD - sizeof( struct bb_note)));
      return( 1);
   }
   if( options.workers + options.readers + 1 > BB_MAX_PROCESSES)
   {
      mulle_fprintf( stderr, "blackboard: at most %d processes\n", BB_MAX_PROCESSES);
      return( 1);
   }

   //  A growth limit is a way of asking for growth as much as it is a ceiling,
   //  so naming one is enough. Without --grow the limit is the region itself and
   //  the board behaves exactly as it always did: full means refused.
   if( options.growth_limit)
      options.grow = 1;

   if( options.grow)
   {
      if( ! options.growth_limit)
         options.growth_limit = BB_DEFAULT_GROWTH_LIMIT;
      if( options.growth_limit < options.capacity)
         options.growth_limit = options.capacity;
   }
   else
      options.growth_limit = options.capacity;

   // a child of this demo, told to be one: it attaches the monitor's board
   // instead of making one of its own
   if( child.role)
      return( bb_child_main( &options, &child));

   subscribers = options.readers + 1;   // the monitor is subscriber 0

   memset( &context, 0, sizeof( context));
   context.options     = &options;
   context.allocator_p = mulle_mmap_allocator_as_allocator( &context.allocator);

   //
   //  One shared region, mapped in every process that follows. A growable board
   //  reserves its ceiling up front and starts with the smaller region it was
   //  asked for: the reserve is address space, because the pages are committed
   //  when the board first touches them. Every process maps the same window at
   //  the same address, which is what keeps the note pointers in the mailboxes
   //  valid everywhere.
   //
   if( options.grow)
      mulle_mmap_allocator_init_growable( &context.allocator, options.capacity,
                                          options.growth_limit,
                                          mulle_mmap_allocator_shared);
   else
      mulle_mmap_allocator_init( &context.allocator, options.capacity,
                                 mulle_mmap_allocator_shared);

   // the board's own allocation can run out of room as well, so it goes through
   // the same recoverable path as a note -- installing this after init, which
   // sets the aborting default
   bb_context                = &context;
   context.allocator_p->fail = bb_fail;

   //
   //  The same landing site as a worker's, for the one allocation that happens
   //  before there is a board: a region too small to hold the board itself is
   //  refused rather than fatal. A jump lands here; the NULL return after it is
   //  only a belt for the case where a refusal ever arrives as a value.
   //
   if( setjmp( context.oom))
   {
      mulle_fprintf( stderr, "blackboard: cannot fit a %u subscriber board in %lu bytes\n",
                     subscribers, (unsigned long) options.capacity);
      mulle_mmap_allocator_done( &context.allocator);
      return( 1);
   }

   // armed only around the board's own allocation, so that an unrelated
   // allocation failure later still gets the aborting default
   context.oom_armed = 1;
   board             = bb_board_create( &context, subscribers, options.topics);
   context.oom_armed = 0;

   if( ! board)
   {
      mulle_mmap_allocator_done( &context.allocator);
      return( 1);
   }

   //
   // The monitor subscribes to everything, so it can report on the whole
   // stream. The other subscribers take a seeded subset of the topics, which
   // is what makes "skipped" and the delivery arithmetic interesting.
   //
   bb_subscribe( &context, 0, (1ULL << options.topics) - 1);

   for( i = 1; i < subscribers; i++)
   {
      mask   = 0;
      for( topics = 0; topics < options.topics; topics++)
         if( bb_scramble( ((uint64_t) i << 32) | topics) & 1)
            mask |= 1ULL << topics;

      if( ! mask)
         mask = 1ULL << (i % options.topics);

      bb_subscribe( &context, i, mask);
   }

   if( ! options.quiet)
   {
      bb_size_string( board_size, sizeof( board_size), (double) board->capacity);
      if( options.grow)
      {
         char   limit[ 32];

         bb_size_string( limit, sizeof( limit), (double) options.growth_limit);
         mulle_fprintf( stdout,
                        "blackboard: %s shared region growing to %s, %lu processes, %u topics%s\n",
                        board_size, limit, (unsigned long) (options.workers + subscribers),
                        options.topics, options.unsafe ? ", tally unlocked" : "");
      }
      else
         mulle_fprintf( stdout, "blackboard: %s shared region, %lu processes, %u topics%s\n",
                        board_size, (unsigned long) (options.workers + subscribers),
                        options.topics, options.unsafe ? ", tally unlocked" : "");
      fflush( stdout);
   }

   start = mulle_relativetime_now();

   // children must not flush an inherited stdout buffer twice
   fflush( NULL);

   for( i = 0; i < options.workers; i++)
   {
      mulle_spawn_handle_init( &workers[ i]);
      if( bb_spawn( &workers[ i], &context, argc, argv, "worker", i) < 0)
      {
         mulle_fprintf( stderr, "blackboard: cannot start worker %u: %s\n",
                        i, strerror( errno));
         return( 1);
      }
   }

   for( i = 0; i < options.readers; i++)
   {
      mulle_spawn_handle_init( &readers[ i]);
      if( bb_spawn( &readers[ i], &context, argc, argv, "reader", i + 1) < 0)
      {
         mulle_fprintf( stderr, "blackboard: cannot start reader %u: %s\n",
                        i + 1, strerror( errno));
         return( 1);
      }
   }

   bb_monitor_loop( &context, &options, workers, options.workers, &reaped);

   // the readers quit when the monitor closes the board; waiting for them bears
   // no deadline, exactly as the reap of a forked reader did not
   for( i = 0; i < options.readers; i++)
   {
      if( ! readers[ i].opened)
         continue;
      if( mulle_spawn_wait( &readers[ i], MULLE_SPAWN_NO_TIMEOUT, &result) != 1)
         continue;
      mulle_spawn_close( &readers[ i], NULL);
      if( result.status != mulle_spawn_exited || result.exit_code)
      {
         mulle_fprintf( stderr, "blackboard: reader %u died (exit %d, status %d)\n",
                        i + 1, result.exit_code, result.status);
         return( 1);
      }
   }

   elapsed = mulle_relativetime_now() - start;

   //  every process is gone now, so these are the numbers the arena settled on.
   //  Both live in the shared header, so a child's growth is counted here too.
   board->grown    = mulle_mmap_allocator_get_current_capacity( &context.allocator);
   board->segments = (unsigned int) mulle_mmap_allocator_get_segment_count( &context.allocator);
   _mulle_atomic_intptr_write_relaxed(
      &board->monitor_grown,
      (intptr_t) mulle_mmap_allocator_get_grow_count( &context.allocator));

   bb_totals_gather( board, &totals);

   if( ! options.quiet)
      mulle_fprintf( stdout, "\n");

   bb_print_summary( board, &options, &totals, elapsed);

   status = 0;
   if( options.verify)
      status = bb_verify( board, &options, &totals);

   if( options.dump)
      _mulle_mmap_allocator_dump( &context.allocator);

   mulle_mmap_allocator_done( &context.allocator);
   return( status);
}
