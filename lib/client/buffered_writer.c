/*
 * Copyright 2011-2013 National ICT Australia (NICTA)
 *
 * This software may be used and distributed solely under the terms of
 * the MIT license (License).  You should find a copy of the License in
 * COPYING or at http://opensource.org/licenses/MIT. By downloading or
 * using this software you accept the terms and the liability disclaimer
 * in the License.
 */
/** \file buffered_writer.c
 * \brief A non-blocking, self-draining FIFO queue using threads.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "oml2/omlc.h"
#include "ocomm/o_log.h"
#include "ocomm/o_socket.h"

#include "client.h"
#include "buffered_writer.h"

/** Default target size in each MBuffer of the chain */
#define DEF_CHAIN_BUFFER_SIZE 1024

/** A circular chain of buffers */
typedef struct BufferChain {

  /** Link to the next buffer in the chain */
  struct BufferChain*  next;

  /** MBuffer used for storage */
  MBuffer* mbuf;
  /** Target maximal size of mbuf for that link */
  size_t   targetBufSize;

  /** Set to 1 when the reader is processing this buffer */
  int   reading;
} BufferChain;

/** A writer reading from a BufferChain */
typedef struct BufferedWriter {
  /** Set to !0 if buffer is active; 0 kills the thread */
  int  active;

  /** Number of links which can still be allocated */
  long chainsAvailable;
  /** Target size of MBuffer in each link */
  size_t chainLength;

  /** Opaque handler to  the output stream*/
  OmlOutStream*    outStream;

  /** Chain where the data gets stored until it's pushed out */
  BufferChain* writerChain;
  /** Immutable entry into the chain */
  BufferChain* firstChain;

  /** Buffer holding protocol headers */
  MBuffer*     meta_buf;

  /** Mutex protecting the object */
  pthread_mutex_t lock;
  /** Semaphore for this object */
  pthread_cond_t semaphore;
  /** Thread in charge of reading the queue and writing the data out */
  pthread_t  readerThread;

  /** Time of the last failure, to backoff for REATTEMP_INTERVAL before retryying **/
  time_t last_failure_time;

  /** Backoff time, in seconds */
  uint8_t backoff;

} BufferedWriter;
#define REATTEMP_INTERVAL 5    //! Seconds to open the stream again

static BufferChain* getNextWriteChain(BufferedWriter* self, BufferChain* current);
static BufferChain* createBufferChain(BufferedWriter* self);
static int destroyBufferChain(BufferedWriter* self);
static void* threadStart(void* handle);
static int processChain(BufferedWriter* self, BufferChain* chain);

/** Create a BufferedWriter instance
 * \param outStream opaque OmlOutStream handler
 * \param queueCapaity maximal size of the internal queue
 * \param chunkSize size of buffer space allocated at a time, set to 0 for default
 *
 * \return an instance pointer if successful, NULL otherwise
 */
BufferedWriterHdl
bw_create(OmlOutStream* outStream, long  queueCapacity, long  chunkSize)
{
  if (outStream == NULL ||
      queueCapacity < 0 || chunkSize < 0) {
    return NULL;
  }

  BufferedWriter* self = (BufferedWriter*)oml_malloc(sizeof(BufferedWriter));
  if (self == NULL) { return NULL; }
  memset(self, 0, sizeof(BufferedWriter));

  self->outStream =outStream;
  /* This forces a 'connected' INFO message upon first connection */
  self->backoff = 1;

  long bufSize = chunkSize > 0 ? chunkSize : DEF_CHAIN_BUFFER_SIZE;
  self->chainLength = bufSize;

  long chunks = queueCapacity / bufSize;
  self->chainsAvailable = chunks > 2 ? chunks : 2; /* at least two chunks */

  /* Start out with two circular linked buffers */
  BufferChain* buf1 = self->firstChain = createBufferChain(self);
  buf1->next = buf1;
  //  BufferChain* buf2 = buf1->next = createBufferChain(self);
  //  buf2->next = buf1;
  self->writerChain = buf1;

  self->meta_buf = mbuf_create();

  /* Initialize mutex and condition variable objects */
  pthread_cond_init(&self->semaphore, NULL);
  pthread_mutex_init(&self->lock, NULL);

  /* Initialize and set thread detached attribute */
  pthread_attr_t tattr;
  pthread_attr_init(&tattr);
  pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_JOINABLE);
  self->active = 1;
  pthread_create(&self->readerThread, &tattr, threadStart, (void*)self);

  return (BufferedWriterHdl)self;
}

/** Close an output stream and destroy the objects.
 *
 * \param instance handle (i.e., pointer) to a BufferedWriter
 */
void
bw_close(BufferedWriterHdl instance)
{
  BufferedWriter *self = (BufferedWriter*)instance;

  if(!self) { return; }

  if (oml_lock (&self->lock, __FUNCTION__)) { return; }
  self->active = 0;

  loginfo ("%s: Waiting for buffered queue thread to drain...\n", self->outStream->dest);

  pthread_cond_signal (&self->semaphore);
  oml_unlock (&self->lock, __FUNCTION__);
  switch (pthread_join (self->readerThread, NULL)) {
  case 0:
    logdebug ("%s: Buffered queue reader thread finished OK...\n", self->outStream->dest);
    break;
  case EINVAL:
    logerror ("%s: Buffered queue reader thread is not joinable\n", self->outStream->dest);
    break;
  case EDEADLK:
    logerror ("%s: Buffered queue reader thread shutdown deadlock, or self-join\n", self->outStream->dest);
    break;
  case ESRCH:
    logerror ("%s: Buffered queue reader thread shutdown failed: could not find the thread\n", self->outStream->dest);
    break;
  default:
    logerror ("%s: Buffered queue reader thread shutdown failed with an unknown error\n", self->outStream->dest);
    break;
  }

  self->outStream->close(self->outStream);
  destroyBufferChain(self);
  oml_free(self);
}

/** Add a chunk to the end of the queue.
 *
 * This function tries to acquire the lock on the BufferedWriter, and releases
 * it when done.
 *
 * \param instance BufferedWriter handle
 * \param chunk Pointer to chunk to add
 * \param chunkSize size of chunk
 * \return 1 if success, 0 otherwise
 */
int
bw_push(BufferedWriterHdl instance, uint8_t* chunk, size_t size)
{
  int result = 0;
  BufferedWriter* self = (BufferedWriter*)instance;
  if (oml_lock(&self->lock, __FUNCTION__) == 0) {
    result =_bw_push(instance, chunk, size);
    oml_unlock(&self->lock, __FUNCTION__);
  }
  return result;
}

/** Add a chunk to the end of the queue.
 * \see bw_push
 *
 * This function is the same as bw_push except it assumes that the
 * lock is already acquired.
 *
 * \param instance BufferedWriter handle
 * \param chunk Pointer to chunk to add
 * \param chunkSize size of chunk
 * \return 1 if success, 0 otherwise
 */
int
_bw_push(BufferedWriterHdl instance, uint8_t* chunk, size_t size)
{
  BufferedWriter* self = (BufferedWriter*)instance;
  if (!self->active) { return 0; }

  BufferChain* chain = self->writerChain;
  if (chain == NULL) { return 0; }

  if (mbuf_wr_remaining(chain->mbuf) < size) {
    chain = self->writerChain = getNextWriteChain(self, chain);
  }

  if (mbuf_write(chain->mbuf, chunk, size) < 0) {
    return 0;
  }

  pthread_cond_signal(&self->semaphore);

  return 1;
}

/** Add a chunk to the end of the header buffer.
 *
 * This function tries to acquire the lock on the BufferedWriter, and releases
 * it when done.
 *
 * \param instance BufferedWriter handle
 * \param chunk pointer to chunk to add
 * \param chunkSize size of chunk
 *
 * \return 1 if success, 0 otherwise
 *
 * \see _bw_push_meta
 */
int
bw_push_meta(BufferedWriterHdl instance, uint8_t* chunk, size_t size)
{
  int result = 0;
  BufferedWriter* self = (BufferedWriter*)instance;
  if (oml_lock(&self->lock, __FUNCTION__) == 0) {
    result = _bw_push_meta(instance, chunk, size);
    oml_unlock(&self->lock, __FUNCTION__);
  }
  return result;
}

/** Add a chunk to the end of the header buffer.
 * \see bw_push_meta
 *
 * This function is the same as bw_push_meta except it assumes that the lock is
 * already acquired.
 */
int
_bw_push_meta(BufferedWriterHdl instance, uint8_t* chunk, size_t size)
{
  BufferedWriter* self = (BufferedWriter*)instance;
  int result = 0;

  if (!self->active) { return 0; }

  if (mbuf_write(self->meta_buf, chunk, size) > 0) {
    result = 1;
    /* XXX: There is no point in signalling the semaphore as the
     * writer will not be able to do anything with the new data.
     *
     * Also, it puts everything in a deadlock otherwise */
    /* pthread_cond_signal(&self->semaphore); */
  }
  return result;
}

/** Return an MBuffer with (optional) exclusive write access
 *
 * If exclusive access is required, the caller is in charge of releasing the
 * lock with bw_unlock_buf.
 *
 * \param instance BufferedWriter handle
 * \param exclusive indicate whether the entire BufferedWriter should be locked
 *
 * \return an MBuffer instance if success to write in, NULL otherwise
 * \see bw_unlock_buf
 */
MBuffer*
bw_get_write_buf(BufferedWriterHdl instance, int exclusive)
{
  BufferedWriter* self = (BufferedWriter*)instance;
  if (oml_lock(&self->lock, __FUNCTION__)) { return 0; }
  if (!self->active) { return 0; }

  BufferChain* chain = self->writerChain;
  if (chain == NULL) { return 0; }

  MBuffer* mbuf = chain->mbuf;
  if (mbuf_write_offset(mbuf) >= chain->targetBufSize) {
    chain = self->writerChain = getNextWriteChain(self, chain);
    mbuf = chain->mbuf;
  }
  if (! exclusive) {
    oml_unlock(&self->lock, __FUNCTION__);
  }
  return mbuf;
}


/** Return and unlock MBuffer
 * \param instance BufferedWriter handle for which a buffer was previously obtained through bw_get_write_buf
 *
 * \see bw_get_write_buf
 */
void
bw_unlock_buf(BufferedWriterHdl instance)
{
  BufferedWriter* self = (BufferedWriter*)instance;
  pthread_cond_signal(&self->semaphore); /* assume we locked for a reason */
  oml_unlock(&self->lock, __FUNCTION__);
}

/** Find the next empty write chain, sets self->writeChain to it and returns it.
 *
 * We only use the next one if it is empty. If not, we essentially just filled
 * up the last chain and wrapped around to the socket reader. In that case, we
 * either create a new chain if the overall buffer can still grow, or we drop
 * the data from the current one.
 *
 * This assumes that the current thread holds the self->lock and the lock on
 * the self->writeChain.
 *
 * \param self BufferedWriter pointer
 * \param current BufferChain to use or from which to find the next
 * \return a BufferChain in which data can be stored
 */
BufferChain*
getNextWriteChain(BufferedWriter* self, BufferChain* current) {
  assert(current != NULL);
  BufferChain* nextBuffer = current->next;
  assert(nextBuffer != NULL);

  BufferChain* resChain = NULL;
  if (mbuf_rd_remaining(nextBuffer->mbuf) == 0) {
    // It's empty, we can use it
    mbuf_clear2(nextBuffer->mbuf, 0);
    resChain = nextBuffer;
  } else if (self->chainsAvailable > 0) {
    // insert new chain between current and next one.
    BufferChain* newBuffer = createBufferChain(self);
    newBuffer->next = nextBuffer;
    current->next = newBuffer;
    resChain = newBuffer;
  } else {
    // Filled up buffer, time to drop data and reuse current buffer
    // Current buffer holds most recent added data (we drop from the queue's tail
    //assert(current->reading == 0);
    assert(current->reading == 0);
    o_log (O_LOG_WARN, "Dropping %d bytes of measurement data\n", mbuf_fill(current->mbuf));
    mbuf_repack_message2(current->mbuf);
    return current;
  }
  // Now we just need to copy the +message+ from +current+ to +resChain+
  int msgSize = mbuf_message_length(current->mbuf);
  if (msgSize > 0) {
    mbuf_write(resChain->mbuf, mbuf_message(current->mbuf), msgSize);
    mbuf_reset_write(current->mbuf);
  }
  return resChain;
}

/** Initialise a BufferChain for a BufferedWriter.
 * \param self BufferedWriter pointer
 * \return a pointer to the newly-created BufferChain, or NULL on error
 */
BufferChain*
createBufferChain(BufferedWriter* self)
{
  //  BufferChain* chain = (BufferChain*)malloc(sizeof(BufferChain) + self->chainLength);

  MBuffer* buf = mbuf_create2(self->chainLength, (size_t)(0.1 * self->chainLength));
  if (buf == NULL) { return NULL; }

  BufferChain* chain = (BufferChain*)oml_malloc(sizeof(BufferChain));
  if (chain == NULL) {
    mbuf_destroy(buf);
    return NULL;
  }
  memset(chain, 0, sizeof(BufferChain));

  // set state
  chain->mbuf = buf;
  chain->targetBufSize = self->chainLength;

  /**
  buf->writeP = buf->readP = &buf->buf;
  buf->endP = &buf->buf + (buf->bufLength = self->chainLength);
  */

  self->chainsAvailable--;
  o_log (O_LOG_DEBUG, "Created new buffer chain of size %d with %d remaining.\n",
        self->chainLength, self->chainsAvailable);
  return chain;
}


/** Destroy the Buffer chain of a BufferedWriter
 *
 * \param self pointer to the BufferedWriter
 * \return 0 on success, or a negative number otherwise
 */
int
destroyBufferChain(BufferedWriter* self) {
  BufferChain *chain, *start;

  if (!self) {
    return -1;
  }

  /* BufferChain is a circular buffer */
  start = self->firstChain;
  while( (chain = self->firstChain) && chain!=start) {
    logdebug("Destroying BufferChain at %p\n", chain);
    self->firstChain = chain->next;

    mbuf_destroy(chain->mbuf);
    oml_free(chain);
  }

  pthread_cond_destroy(&self->semaphore);
  pthread_mutex_destroy(&self->lock);

  return 0;
}


/** Writing thread
 * \param handle the stream to use the filters on
 * \return NULL on error; this function should not return
 */
static void*
threadStart(void* handle)
{
  BufferedWriter* self = (BufferedWriter*)handle;
  BufferChain* chain = self->firstChain;

  while (self->active) {
    oml_lock(&self->lock, "bufferedWriter");
    pthread_cond_wait(&self->semaphore, &self->lock);
    // Process all chains which have data in them
    while(1) {
      if (mbuf_message(chain->mbuf) > mbuf_rdptr(chain->mbuf)) {
        processChain(self, chain);
      }
      // stop if we caught up to the writer

      if (chain == self->writerChain) break;

      chain = chain->next;
    }
    oml_unlock(&self->lock, "bufferedWriter");
  }
  /* Drain this writer before terminating */
  while (!processChain(self, chain));
  return NULL;
}

/** Process the chain and send data
 * \param selfBufferedWriter to process
 * \param chain link of the chain to process
 *
 * \return 1 if chain has been fully sent, 0 otherwise
 * \see oml_outs_write_f
 */
int
processChain(BufferedWriter* self, BufferChain* chain)
{
  uint8_t* buf = mbuf_rdptr(chain->mbuf);
  size_t size = mbuf_message_offset(chain->mbuf) - mbuf_read_offset(chain->mbuf);
  size_t sent = 0;
  chain->reading = 1;
  MBuffer* meta = self->meta_buf;

  /* XXX: Should we use a timer instead? */
  time_t now;
  time(&now);
  if (difftime(now, self->last_failure_time) < self->backoff) {
    logdebug("%s: Still in back-off period (%ds)\n", self->outStream->dest, self->backoff);
    return 0;
  }

  while (size > sent) {
    long cnt = self->outStream->write(self->outStream, (void*)(buf + sent), size - sent,
                               meta->rdptr, meta->fill);
    if (cnt > 0) {
      sent += cnt;
      if (self->backoff) {
        self->backoff = 0;
        loginfo("%s: Connected\n", self->outStream->dest);
      }
    } else {
      /* To be on the safe side, we rewind to the beginning of the
       * chain and try to resend everything - this is especially important
       * if the underlying stream needs to reopen and resync. */
      mbuf_reset_read(chain->mbuf);
      size = mbuf_message_offset(chain->mbuf) - mbuf_read_offset(chain->mbuf);
      sent = 0;
      self->last_failure_time = now;
      if (!self->backoff) {
        self->backoff = 1;
      } else if (self->backoff < UINT8_MAX) {
        self->backoff *= 2;
      }
      logwarn("%s: Error sending, backing off for %ds\n", self->outStream->dest, self->backoff);
      return 0;
    }
  }
  // get lock back to see what happened while we were busy
  mbuf_read_skip(chain->mbuf, sent);
  if (mbuf_write_offset(chain->mbuf) == mbuf_read_offset(chain->mbuf)) {
    // seem to have sent everything so far, reset chain
    //    mbuf_clear2(chain->mbuf, 0);
    mbuf_clear2(chain->mbuf, 1);
    chain->reading = 0;
    return 1;
  }
  return 0;
}

/*
 Local Variables:
 mode: C
 tab-width: 2
 indent-tabs-mode: nil
 End:
 vim: sw=2:sts=2:expandtab
*/
