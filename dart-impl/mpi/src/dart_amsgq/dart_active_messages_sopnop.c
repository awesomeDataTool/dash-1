#include <stdlib.h>
#include <unistd.h>
#include <mpi.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <alloca.h>

#include <dash/dart/base/mutex.h>
#include <dash/dart/base/logging.h>
#include <dash/dart/base/assert.h>
#include <dash/dart/base/atomic.h>
#include <dash/dart/if/dart_active_messages.h>
#include <dash/dart/if/dart_communication.h>
#include <dash/dart/if/dart_globmem.h>
#include <dash/dart/mpi/dart_team_private.h>
#include <dash/dart/mpi/dart_globmem_priv.h>
#include <dash/dart/mpi/dart_active_messages_priv.h>


typedef struct cached_message_s cached_message_t;
typedef struct message_cache_s  message_cache_t;

#define MSGCACHE_SIZE (4*1024)

struct dart_amsgq_impl_data {
  MPI_Win           queue_win;
  void             *queue_ptr;
  uint64_t          queue_size;
  MPI_Comm          comm;
  dart_mutex_t      send_mutex;
  dart_mutex_t      processing_mutex;
  message_cache_t **message_cache;
  int64_t           prev_tailpos;
};

struct dart_amsg_header {
  dart_task_action_t fn;
  dart_global_unit_t remote;
  uint32_t           data_size;
#ifdef DART_DEBUG
  uint32_t           msgid;
#endif // DART_DEBUG
};

struct cached_message_s
{
  struct dart_amsg_header header;  // header containing function and data-size
  uint8_t                 data[];
};

struct message_cache_s
{
  dart_mutex_t            mutex;
  int                     pos;
  int8_t                  buffer[];
};

#ifdef DART_ENABLE_LOGGING
static uint32_t msgcnt = 0;
#endif // DART_ENABLE_LOGGING

#define OFFSET_QUEUENUM                 0
#define OFFSET_TAILPOS(q)    (sizeof(int64_t)+q*2*sizeof(int64_t))
#define OFFSET_READYPOS(q)   (OFFSET_TAILPOS(q)+sizeof(int64_t))
#define OFFSET_DATA(q, qs)   (OFFSET_READYPOS(1)+sizeof(int64_t)+q*qs)

static dart_ret_t
dart_amsg_sopnop_openq(
  size_t      msg_size,
  size_t      msg_count,
  dart_team_t team,
  struct dart_amsgq_impl_data** queue)
{
  dart_team_data_t *team_data = dart_adapt_teamlist_get(team);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_gptr_getaddr ! Unknown team %i", team);
    return DART_ERR_INVAL;
  }

  struct dart_amsgq_impl_data *res = calloc(1, sizeof(struct dart_amsgq_impl_data));
  res->queue_size =
      msg_count * (sizeof(struct dart_amsg_header) + msg_size);
  MPI_Comm_dup(team_data->comm, &res->comm);

  //printf("Allocating queue with queue-size %zu\n", res->queue_size);

  size_t win_size = 2*(res->queue_size + 2*sizeof(int64_t)) + sizeof(int64_t);

  dart__base__mutex_init(&res->send_mutex);
  dart__base__mutex_init(&res->processing_mutex);

  // we don't need MPI to take care of the ordering since we use
  // explicit flushes to guarantee ordering
  MPI_Info info;
  MPI_Info_create(&info);
  MPI_Info_set(info, "accumulate_ordering", "none");
  MPI_Info_set(info, "same_size"          , "true");
  MPI_Info_set(info, "same_disp_unit"     , "true");
  MPI_Info_set(info, "accumulate_ops"     , "same_op_no_op");

  /**
   * Allocate the queue
   * We cannot use dart_team_memalloc_aligned because it uses
   * MPI_Win_allocate_shared that cannot be used for window locking.
   */
  MPI_Win_allocate(
    win_size,
    1,
    info,
    res->comm,
    &(res->queue_ptr),
    &(res->queue_win));
  MPI_Info_free(&info);
  memset(res->queue_ptr, 0, win_size);

  MPI_Win_lock_all(0, res->queue_win);

  res->message_cache = calloc(team_data->size, sizeof(message_cache_t*));

  MPI_Barrier(res->comm);

  *queue = res;

  return DART_OK;
}


static
dart_ret_t
dart_amsg_sopnop_sendbuf(
  dart_team_unit_t              target,
  struct dart_amsgq_impl_data * amsgq,
  const void                  * data,
  size_t                        data_size)
{
  // this lock is not needed, MPI will take care of it
  //dart__base__mutex_lock(&amsgq->send_mutex);

  DART_LOG_DEBUG("dart_amsg_trysend: u:%i ds:%zu",
                 target.id, data_size);

  int64_t msg_size = data_size;
  int64_t offset;
  int64_t queuenum;

  do {

    // fetch queue number
    MPI_Fetch_and_op(
      NULL,
      &queuenum,
      MPI_INT64_T,
      target.id,
      OFFSET_QUEUENUM,
      MPI_NO_OP,
      amsgq->queue_win);
    MPI_Win_flush_local(target.id, amsgq->queue_win);

    DART_ASSERT(queuenum == 0 || queuenum == 1);

    // atomically fetch and update the writer offset
    MPI_Fetch_and_op(
      &msg_size,
      &offset,
      MPI_INT64_T,
      target.id,
      OFFSET_TAILPOS(queuenum),
      MPI_SUM,
      amsgq->queue_win);
    MPI_Win_flush_local(target.id, amsgq->queue_win);

    if (offset >= 0 && (offset + msg_size) <= amsgq->queue_size) break;

    // the queue is full, reset the offset
    int64_t neg_msg_size = -msg_size;
    DART_LOG_TRACE("Queue %ld at %d full/processing (tailpos %ld), reverting by %ld",
                   queuenum, target.id, offset, neg_msg_size);
    MPI_Accumulate(&neg_msg_size, 1, MPI_INT64_T, target.id,
                   OFFSET_TAILPOS(queuenum), 1, MPI_INT64_T,
                   MPI_SUM, amsgq->queue_win);
    MPI_Win_flush(target.id, amsgq->queue_win);

    //dart__base__mutex_unlock(&amsgq->send_mutex);
    return DART_ERR_AGAIN;
  } while (1);

  DART_LOG_TRACE("Writing %ld into queue %ld at offset %ld at unit %i",
                 data_size, queuenum, offset, target.id);

  // Write our payload

  DART_LOG_TRACE("MPI_Put at offset %ld", OFFSET_DATA(queuenum, amsgq->queue_size) + offset);
  MPI_Put(
    data,
    data_size,
    MPI_BYTE,
    target.id,
    OFFSET_DATA(queuenum, amsgq->queue_size) + offset,
    data_size,
    MPI_BYTE,
    amsgq->queue_win);
  // we have to flush here because MPI has no ordering guarantees
  MPI_Win_flush(target.id, amsgq->queue_win);

  DART_LOG_TRACE("Updating readypos in queue %ld at unit %i",
                 queuenum, target.id);

  // signal completion
  MPI_Accumulate(&msg_size, 1, MPI_INT64_T, target.id,
                 OFFSET_READYPOS(queuenum), 1, MPI_INT64_T,
                 MPI_SUM, amsgq->queue_win);
  // remote flush required, otherwise the message might never make it through
  MPI_Win_flush(target.id, amsgq->queue_win);

  //dart__base__mutex_unlock(&amsgq->send_mutex);

  DART_LOG_INFO("Sent message of size %zu with payload %zu to unit "
                "%d starting at offset %ld",
                msg_size, data_size, target.id, offset);

  return DART_OK;
}

static
dart_ret_t
dart_amsg_sopnop_trysend(
  dart_team_unit_t              target,
  struct dart_amsgq_impl_data * amsgq,
  dart_task_action_t            fn,
  const void                  * data,
  size_t                        data_size)
{
  dart_global_unit_t unitid;

  dart_myid(&unitid);

  int64_t msg_size = (sizeof(struct dart_amsg_header) + data_size);

  // we allocate the message on the stack because we expect it to be small enough
  cached_message_t *msg = alloca(msg_size);

  // assemble the message
  msg->header.remote    = unitid;
  msg->header.fn        = fn;
  msg->header.data_size = data_size;
#ifdef DART_ENABLE_LOGGING
  msg->header.msgid     = DART_FETCH_AND_INC32(&msgcnt);
#endif
  memcpy(msg->data, data, data_size);

  DART_LOG_INFO("Sending message %d of size %zu with payload %zu to unit %d",
                msg->header.msgid, msg_size, data_size, target.id);

  return dart_amsg_sopnop_sendbuf(target, amsgq, msg, msg_size);
}

static dart_ret_t
amsg_sopnop_process_internal(
  struct dart_amsgq_impl_data * amsgq,
  bool                          blocking)
{
  int     unitid;
  int64_t tailpos;

  if (!blocking) {
    dart_ret_t ret = dart__base__mutex_trylock(&amsgq->processing_mutex);
    if (ret != DART_OK) {
      return DART_ERR_AGAIN;
    }
  } else {
    dart__base__mutex_lock(&amsgq->processing_mutex);
  }

  do {
    MPI_Comm_rank(amsgq->comm, &unitid);

    int64_t queuenum = *(int64_t*)amsgq->queue_ptr;

    DART_ASSERT(queuenum == 0 || queuenum == 1);

    //printf("Reading from queue %i\n", queuenum);

    //check whether there are active messages available
    MPI_Fetch_and_op(
      NULL,
      &tailpos,
      MPI_INT64_T,
      unitid,
      OFFSET_TAILPOS(queuenum),
      MPI_NO_OP,
      amsgq->queue_win);
    MPI_Win_flush_local(unitid, amsgq->queue_win);


    if (tailpos > 0) {
      DART_LOG_TRACE("Queue %ld has tailpos %ld", queuenum, tailpos);
      const int64_t zero = 0;

      int64_t tmp = 0;
      int64_t newqueue = (queuenum == 0) ? 1 : 0;

      // wait for possible late senders on the new queue to finish
      // NOTE: this is a poor-man's CAS
      do {
        MPI_Fetch_and_op(
          NULL,
          &tmp,
          MPI_INT64_T,
          unitid,
          OFFSET_TAILPOS(newqueue),
          MPI_NO_OP,
          amsgq->queue_win);
        MPI_Win_flush_local(unitid, amsgq->queue_win);
      } while (tmp != amsgq->prev_tailpos);

      // reset tailpos of new queue
      MPI_Fetch_and_op(
        &zero,
        &tmp,
        MPI_INT64_T,
        unitid,
        OFFSET_TAILPOS(newqueue),
        MPI_REPLACE,
        amsgq->queue_win);
      MPI_Win_flush(unitid, amsgq->queue_win);

      // swap the queue number
      int64_t queue_swap_sum = (queuenum == 0) ? 1 : -1;
      MPI_Fetch_and_op(
        &queue_swap_sum,
        &tmp,
        MPI_INT64_T,
        unitid,
        OFFSET_QUEUENUM,
        MPI_SUM,
        amsgq->queue_win);
      MPI_Win_flush(unitid, amsgq->queue_win);
      DART_ASSERT(tmp == queuenum);


      // set the tailpos to a large negative number to signal the start of
      // processing
      // Any later attempt to write to this queue will return a negative offset
      // and cause the writer to switch to the new queue
      int64_t readypos    = 0;
      int64_t tailpos_sub = -tailpos - INT32_MAX;
      MPI_Fetch_and_op(
        &tailpos_sub,
        &tailpos,
        MPI_INT64_T,
        unitid,
        OFFSET_TAILPOS(queuenum),
        MPI_SUM,
        amsgq->queue_win);

      // NOTE: deferred flush

      // wait for all active writers to finish
      // NOTE: This is a poor-man's CAS
      do {

        MPI_Fetch_and_op(
          NULL,
          &readypos,
          MPI_INT64_T,
          unitid,
          OFFSET_READYPOS(queuenum),
          MPI_NO_OP,
          amsgq->queue_win);

        // we have to requiry the tail pos and possibly adjust it
        int64_t tmp;
        MPI_Fetch_and_op(
          NULL,
          &tmp,
          MPI_INT64_T,
          unitid,
          OFFSET_TAILPOS(queuenum),
          MPI_NO_OP,
          amsgq->queue_win);
        MPI_Win_flush_local(unitid, amsgq->queue_win);

        tailpos = tmp + (-tailpos_sub);

        DART_ASSERT(readypos <= tailpos);

      } while (readypos != tailpos);

      // remember the actual value of tailpos so we can wait for it later
      amsgq->prev_tailpos = tailpos_sub + tailpos;

      DART_LOG_TRACE("Previous tailpos: %ld", amsgq->prev_tailpos);

      // reset readypos
      // NOTE: using MPI_REPLACE here is valid as no-one else will write to it
      //       at this time.
      MPI_Fetch_and_op(
        &zero,
        &readypos,
        MPI_INT64_T,
        unitid,
        OFFSET_READYPOS(queuenum),
        MPI_REPLACE,
        amsgq->queue_win);
      MPI_Win_flush(unitid, amsgq->queue_win);

      DART_LOG_TRACE("Starting processing queue %ld: tailpos %ld, readypos %ld",
                     queuenum, tailpos, readypos);

      // process the messages by invoking the functions on the data supplied
      int64_t  pos      = 0;
      int      num_msg  = 0;
      uint8_t *dbuf     = (void*)((intptr_t)amsgq->queue_ptr +
                                      OFFSET_DATA(queuenum, amsgq->queue_size));

      while (pos < tailpos) {
  #ifdef DART_ENABLE_LOGGING
        int64_t startpos = pos;
  #endif
        // unpack the message
        struct dart_amsg_header *header =
                                    (struct dart_amsg_header *)(dbuf + pos);
        pos += sizeof(struct dart_amsg_header);
        void *data     = dbuf + pos;
        pos += header->data_size;

        DART_ASSERT_MSG(pos <= tailpos,
                        "Message out of bounds (expected %ld but saw %lu)\n",
                         tailpos, pos);

        // invoke the message
        DART_LOG_INFO("Invoking active message %p id=%d from %i on data %p of "
                      "size %i starting from tailpos %ld",
                      header->fn,
                      header->msgid,
                      header->remote.id,
                      data,
                      header->data_size,
                      startpos);
        header->fn(data);
        num_msg++;
      }
    }
  } while (blocking && tailpos > 0);
  dart__base__mutex_unlock(&amsgq->processing_mutex);
  return DART_OK;
}

static
dart_ret_t
dart_amsg_sopnop_process(struct dart_amsgq_impl_data * amsgq)
{
  return amsg_sopnop_process_internal(amsgq, false);
}

static
dart_ret_t
dart_amsg_sopnop_flush_buffer(struct dart_amsgq_impl_data * amsgq)
{
  dart__base__mutex_lock(&amsgq->send_mutex);
  int comm_size;
  MPI_Comm_size(amsgq->comm, &comm_size);
  for (int target = 0; target < comm_size; ++target) {
    if (amsgq->message_cache[target] != NULL) {
      message_cache_t *cache = amsgq->message_cache[target];
      dart__base__mutex_lock(&cache->mutex);

      if (cache->pos == 0) {
        dart__base__mutex_unlock(&cache->mutex);
        // nothing to be done
        continue;
      }

      dart_ret_t ret;
      do {
        dart_team_unit_t t = {target};
        ret = dart_amsg_sopnop_sendbuf(t, amsgq, cache->buffer, cache->pos);
        if (DART_ERR_AGAIN == ret) {
          // try to process our messages while waiting for the other side
          amsg_sopnop_process_internal(amsgq, false);
        } else if (ret != DART_OK) {
          dart__base__mutex_unlock(&amsgq->send_mutex);
          DART_LOG_ERROR("Failed to flush message cache!");
          return ret;
        }
      } while (ret != DART_OK);

      cache->pos = 0;

      dart__base__mutex_unlock(&cache->mutex);
    }
  }
  dart__base__mutex_unlock(&amsgq->send_mutex);

  return DART_OK;
}

static
dart_ret_t
dart_amsg_sopnop_process_blocking(
  struct dart_amsgq_impl_data * amsgq, dart_team_t team)
{
  int         flag = 0;
  MPI_Request req;

  // flush our buffer
  dart_amsg_sopnop_flush_buffer(amsgq);

  // keep processing until all incoming messages have been dealt with
  MPI_Ibarrier(amsgq->comm, &req);
  do {
    amsg_sopnop_process_internal(amsgq, true);
    MPI_Test(&req, &flag, MPI_STATUSES_IGNORE);
  } while (!flag);
  amsg_sopnop_process_internal(amsgq, true);
  MPI_Barrier(amsgq->comm);
  return DART_OK;
}


static dart_ret_t
dart_amsg_sopnop_bsend(
  dart_team_unit_t              target,
  struct dart_amsgq_impl_data * amsgq,
  dart_task_action_t            fn,
  const void                  * data,
  size_t                        data_size)
{
  if (amsgq->message_cache[target.id] == NULL) {
    dart__base__mutex_lock(&amsgq->send_mutex);
    if (amsgq->message_cache[target.id] == NULL) {
      amsgq->message_cache[target.id] = malloc(sizeof(message_cache_t) + MSGCACHE_SIZE);
      amsgq->message_cache[target.id]->pos = 0;
      dart__base__mutex_init(&amsgq->message_cache[target.id]->mutex);
    }
    dart__base__mutex_unlock(&amsgq->send_mutex);
  }
  message_cache_t *cache = amsgq->message_cache[target.id];
  dart__base__mutex_lock(&cache->mutex);
  if ((cache->pos + sizeof(cached_message_t) + data_size) > MSGCACHE_SIZE) {
    dart_ret_t ret;
    do {
      DART_LOG_TRACE("Flushing buffer to %d", target.id);
      ret = dart_amsg_sopnop_sendbuf(target, amsgq, cache->buffer, cache->pos);
      if (DART_ERR_AGAIN == ret) {
        // try to process our messages while waiting for the other side
        amsg_sopnop_process_internal(amsgq, false);
      } else if (ret != DART_OK) {
        DART_LOG_ERROR("Failed to flush message cache!");
        return ret;
      }
    } while (ret != DART_OK);
    // reset position
    cache->pos = 0;
  }
  cached_message_t *msg = (cached_message_t *)(cache->buffer + cache->pos);
  msg->header.fn        = fn;
  msg->header.data_size = data_size;
#ifdef DART_ENABLE_LOGGING
  msg->header.msgid     = DART_FETCH_AND_INC32(&msgcnt);
#endif
  dart_myid(&msg->header.remote);
  memcpy(msg->data, data, data_size);
  cache->pos += sizeof(*msg) + data_size;
  DART_LOG_TRACE("Cached message: fn=%p, r=%d, ds=%d, id=%d", msg->header.fn,
                 msg->header.remote.id, msg->header.data_size, msg->header.msgid);
  dart__base__mutex_unlock(&cache->mutex);
  return DART_OK;
}

static dart_ret_t
dart_amsg_sopnop_closeq(struct dart_amsgq_impl_data* amsgq)
{
  // check for late messages
  uint32_t tailpos;
  int      unitid;
  int64_t queuenum = *(int64_t*)amsgq->queue_ptr;

  MPI_Comm_rank(amsgq->comm, &unitid);

  MPI_Fetch_and_op(
    NULL,
    &tailpos,
    MPI_INT32_T,
    unitid,
    OFFSET_TAILPOS(queuenum),
    MPI_NO_OP,
    amsgq->queue_win);
  MPI_Win_flush_local(unitid, amsgq->queue_win);
  if (tailpos > 0) {
    DART_LOG_WARN("Cowardly refusing to invoke unhandled incoming active "
                  "messages upon shutdown (tailpos %d)!", tailpos);
  }

  // free window
  amsgq->queue_ptr = NULL;
  MPI_Win_unlock_all(amsgq->queue_win);
  MPI_Win_free(&(amsgq->queue_win));

  MPI_Comm_free(&amsgq->comm);

  free(amsgq->message_cache);
  free(amsgq);

  dart__base__mutex_destroy(&amsgq->send_mutex);
  dart__base__mutex_destroy(&amsgq->processing_mutex);

  return DART_OK;
}


dart_ret_t
dart_amsg_sopnop_init(dart_amsgq_impl_t* impl)
{
  impl->openq   = dart_amsg_sopnop_openq;
  impl->closeq  = dart_amsg_sopnop_closeq;
  impl->bsend   = dart_amsg_sopnop_bsend;
  //impl->bsend   = dart_amsg_sopnop_trysend;
  impl->trysend = dart_amsg_sopnop_trysend;
  impl->flush   = dart_amsg_sopnop_flush_buffer;
  impl->process = dart_amsg_sopnop_process;
  impl->process_blocking = dart_amsg_sopnop_process_blocking;
  return DART_OK;
}
