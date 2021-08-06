/*
 * Copyright (c) 2002-2016 Balabit
 * Copyright (c) 2016 Viktor Juhasz <viktor.juhasz@balabit.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "logqueue-disk-non-reliable.h"
#include "logpipe.h"
#include "messages.h"
#include "syslog-ng.h"
#include "scratch-buffers.h"

#define ITEM_NUMBER_PER_MESSAGE 2

typedef struct
{
  guint index_in_queue;
  guint item_number_per_message;
  LogQueue *queue;
} DiskqMemusageLoaderState;

static gboolean
_object_is_message_in_position(guint index_in_queue, guint item_number_per_message)
{
  return !(index_in_queue % item_number_per_message);
}

static void
_update_memory_usage_during_load(gpointer data, gpointer s)
{
  DiskqMemusageLoaderState *state = (DiskqMemusageLoaderState *)s;

  if (_object_is_message_in_position(state->index_in_queue, state->item_number_per_message))
    {
      LogMessage *msg = (LogMessage *)data;
      log_queue_memory_usage_add(state->queue, log_msg_get_size(msg));
    }
  state->index_in_queue++;
}

static gboolean
_start(LogQueueDisk *s, const gchar *filename)
{
  LogQueueDiskNonReliable *self = (LogQueueDiskNonReliable *) s;

  gboolean retval = qdisk_start(s->qdisk, filename, self->qout, self->qbacklog, self->qoverflow);

  DiskqMemusageLoaderState qout_sum = { .index_in_queue = 0,
                                        .item_number_per_message = ITEM_NUMBER_PER_MESSAGE,
                                        .queue = &self->super.super
                                      };

  DiskqMemusageLoaderState overflow_sum = { .index_in_queue = 0,
                                            .item_number_per_message = ITEM_NUMBER_PER_MESSAGE,
                                            .queue = &self->super.super
                                          };

  g_queue_foreach(self->qout, _update_memory_usage_during_load, &qout_sum);
  g_queue_foreach(self->qoverflow, _update_memory_usage_during_load, &overflow_sum);

  return retval;
}

static inline guint
_get_message_number_in_queue(GQueue *queue)
{
  return queue->length / ITEM_NUMBER_PER_MESSAGE;
}

#define HAS_SPACE_IN_QUEUE(queue) _get_message_number_in_queue(queue) < queue ## _size

static gint64
_get_length(LogQueue *s)
{
  LogQueueDiskNonReliable *self = (LogQueueDiskNonReliable *)s;

  if (!qdisk_started(self->super.qdisk))
    return 0;

  return _get_message_number_in_queue(self->qout)
         + qdisk_get_length(self->super.qdisk)
         + _get_message_number_in_queue(self->qoverflow);
}

static LogMessage *
_get_next_message(LogQueueDiskNonReliable *self, LogPathOptions *path_options)
{
  LogMessage *result = NULL;
  path_options->ack_needed = TRUE;
  if (qdisk_get_length (self->super.qdisk) > 0)
    {
      result = log_queue_disk_read_message(&self->super, path_options);
      if(result)
        {
          log_queue_memory_usage_add(&self->super.super, log_msg_get_size(result));
          path_options->ack_needed = FALSE;
        }
    }
  else if (self->qoverflow->length > 0)
    {
      result = g_queue_pop_head (self->qoverflow);
      POINTER_TO_LOG_PATH_OPTIONS (g_queue_pop_head (self->qoverflow), path_options);
    }
  return result;
}

static inline gboolean
_could_move_into_qout(LogQueueDiskNonReliable *self)
{
  /* NOTE: we only load half the qout queue at a time */
  return (_get_message_number_in_queue(self->qout) < (self->qout_size / 2));
}

static void
_add_message_to_qout(LogQueueDiskNonReliable *self, LogMessage *msg, LogPathOptions *path_options)
{
  /* NOTE: we always generate flow-control disabled entries into
   * qout, they only get there via backlog rewind */

  g_queue_push_tail (self->qout, msg);
  g_queue_push_tail (self->qout, LOG_PATH_OPTIONS_FOR_BACKLOG);
  log_msg_ack (msg, path_options, AT_PROCESSED);
}

static inline gboolean
_has_movable_message(LogQueueDiskNonReliable *self)
{
  return self->qoverflow->length > 0
         && ((HAS_SPACE_IN_QUEUE(self->qout) && qdisk_get_length (self->super.qdisk) == 0)
             || qdisk_is_space_avail (self->super.qdisk, 4096));
}

static gboolean
_serialize_and_write_message_to_disk(LogQueueDiskNonReliable *self, LogMessage *msg)
{
  ScratchBuffersMarker marker;
  GString *write_serialized = scratch_buffers_alloc_and_mark(&marker);
  if (!qdisk_serialize_msg(self->super.qdisk, msg, write_serialized))
    {
      scratch_buffers_reclaim_marked(marker);
      return FALSE;
    }

  gboolean success = qdisk_push_tail(self->super.qdisk, write_serialized);

  scratch_buffers_reclaim_marked(marker);
  return success;
}

static void
_move_messages_from_overflow(LogQueueDiskNonReliable *self)
{
  LogMessage *msg;
  LogPathOptions path_options;
  /* move away as much entries from the overflow area as possible */
  while (_has_movable_message(self))
    {
      msg = g_queue_pop_head (self->qoverflow);
      POINTER_TO_LOG_PATH_OPTIONS (g_queue_pop_head (self->qoverflow), &path_options);

      if (qdisk_get_length (self->super.qdisk) == 0 && HAS_SPACE_IN_QUEUE(self->qout))
        {
          /* we can skip qdisk, go straight to qout */
          g_queue_push_tail (self->qout, msg);
          g_queue_push_tail (self->qout, LOG_PATH_OPTIONS_FOR_BACKLOG);
          log_msg_ref (msg);
        }
      else
        {
          if (_serialize_and_write_message_to_disk(self, msg))
            {
              log_queue_memory_usage_sub(&self->super.super, log_msg_get_size(msg));
            }
          else
            {
              /* oops, although there seemed to be some free space available,
               * we failed saving this message, (it might have needed more
               * than 4096 bytes than we ensured), push back and break
               */
              g_queue_push_head (self->qoverflow, LOG_PATH_OPTIONS_TO_POINTER (&path_options));
              g_queue_push_head (self->qoverflow, msg);
              log_msg_ref (msg);
              break;
            }
        }
      log_msg_ack (msg, &path_options, AT_PROCESSED);
      log_msg_unref (msg);
    }
}

static void
_move_disk (LogQueueDiskNonReliable *self)
{
  LogMessage *msg;
  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;

  if (qdisk_is_read_only (self->super.qdisk))
    return;

  /* stupid message mover between queues */

  if (self->qout->length == 0 && self->qout_size > 0)
    {
      do
        {
          msg = _get_next_message(self, &path_options);

          if (msg)
            {
              _add_message_to_qout(self, msg, &path_options);
            }
        }
      while (msg && _could_move_into_qout(self));
    }
  _move_messages_from_overflow(self);
}

static void
_ack_backlog(LogQueue *s, gint num_msg_to_ack)
{
  LogQueueDiskNonReliable *self = (LogQueueDiskNonReliable *)s;
  LogMessage *msg;
  LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;
  guint i;

  g_static_mutex_lock(&s->lock);

  for (i = 0; i < num_msg_to_ack; i++)
    {
      if (self->qbacklog->length < ITEM_NUMBER_PER_MESSAGE)
        goto exit;
      msg = g_queue_pop_head (self->qbacklog);
      POINTER_TO_LOG_PATH_OPTIONS (g_queue_pop_head (self->qbacklog), &path_options);
      log_msg_unref (msg);
      log_msg_ack (msg, &path_options, AT_PROCESSED);
    }

exit:
  g_static_mutex_unlock(&s->lock);
}

static void
_rewind_backlog(LogQueue *s, guint rewind_count)
{
  guint i;
  LogQueueDiskNonReliable *self = (LogQueueDiskNonReliable *)s;

  g_static_mutex_lock(&s->lock);

  rewind_count = MIN(rewind_count, _get_message_number_in_queue(self->qbacklog));

  for (i = 0; i < rewind_count; i++)
    {
      gpointer ptr_opt = g_queue_pop_tail (self->qbacklog);
      gpointer ptr_msg = g_queue_pop_tail (self->qbacklog);

      g_queue_push_head (self->qout, ptr_opt);
      g_queue_push_head (self->qout, ptr_msg);

      log_queue_queued_messages_inc(s);
      log_queue_memory_usage_add(s, log_msg_get_size((LogMessage *)ptr_msg));
    }

  g_static_mutex_unlock(&s->lock);
}

static void
_rewind_backlog_all(LogQueue *s)
{
  _rewind_backlog(s, -1);
}

static LogMessage *
_pop_head(LogQueue *s, LogPathOptions *path_options)
{
  LogQueueDiskNonReliable *self = (LogQueueDiskNonReliable *)s;
  LogMessage *msg = NULL;

  g_static_mutex_lock(&s->lock);

  if (self->qout->length > 0)
    {
      msg = g_queue_pop_head (self->qout);
      POINTER_TO_LOG_PATH_OPTIONS (g_queue_pop_head (self->qout), path_options);
      log_queue_memory_usage_sub(s, log_msg_get_size(msg));
    }
  if (msg == NULL)
    {
      msg = log_queue_disk_read_message(&self->super, path_options);
      if (msg)
        {
          path_options->ack_needed = FALSE;
        }
    }
  if (msg == NULL)
    {
      if (self->qoverflow->length > 0 && qdisk_is_read_only (self->super.qdisk))
        {
          msg = g_queue_pop_head (self->qoverflow);
          POINTER_TO_LOG_PATH_OPTIONS (g_queue_pop_head (self->qoverflow), path_options);
          log_queue_memory_usage_sub(s, log_msg_get_size(msg));
        }
    }

  if (msg != NULL)
    {
      if (s->use_backlog)
        {
          log_msg_ref (msg);
          g_queue_push_tail (self->qbacklog, msg);
          g_queue_push_tail (self->qbacklog, LOG_PATH_OPTIONS_TO_POINTER (path_options));
        }
      _move_disk (self);
      log_queue_queued_messages_dec(s);
    }

  g_static_mutex_unlock(&s->lock);

  return msg;
}

static void
_push_head(LogQueue *s, LogMessage *msg, const LogPathOptions *path_options)
{
  LogQueueDiskNonReliable *self = (LogQueueDiskNonReliable *)s;

  g_static_mutex_lock(&s->lock);

  g_queue_push_head(self->qout, LOG_PATH_OPTIONS_TO_POINTER (path_options));
  g_queue_push_head(self->qout, msg);
  log_queue_queued_messages_inc(s);
  log_queue_memory_usage_add(s, log_msg_get_size(msg));

  g_static_mutex_unlock(&s->lock);
}

static void
_drop_msg(LogQueueDiskNonReliable *self, LogMessage *msg, const LogPathOptions *path_options)
{
  stats_counter_inc(self->super.super.dropped_messages);

  if (path_options->flow_control_requested)
    log_msg_ack(msg, path_options, AT_SUSPENDED);
  else
    log_msg_drop(msg, path_options, AT_PROCESSED);
}

/* _is_msg_serialization_needed_hint() must be called without holding the queue's lock.
 * This can only be used _as a hint_ for performance considerations, because as soon as the lock
 * is released, there will be no guarantee that the result of this function remain correct. */
static inline gboolean
_is_msg_serialization_needed_hint(LogQueueDiskNonReliable *self)
{
  g_static_mutex_lock(&self->super.super.lock);

  gboolean msg_serialization_needed = FALSE;

  if (HAS_SPACE_IN_QUEUE(self->qout) && qdisk_get_length(self->super.qdisk) == 0)
    goto exit;

  if (self->qoverflow->length != 0)
    goto exit;

  if (!qdisk_started(self->super.qdisk) || !qdisk_is_space_avail(self->super.qdisk, 64))
    goto exit;

  msg_serialization_needed = TRUE;

exit:
  g_static_mutex_unlock(&self->super.super.lock);
  return msg_serialization_needed;
}

static gboolean
_ensure_serialized_and_write_to_disk(LogQueueDiskNonReliable *self, LogMessage *msg, GString *serialized_msg)
{
  if (serialized_msg)
    return qdisk_push_tail(self->super.qdisk, serialized_msg);

  return _serialize_and_write_message_to_disk(self, msg);
}

static void
_push_tail(LogQueue *s, LogMessage *msg, const LogPathOptions *path_options)
{
  LogQueueDiskNonReliable *self = (LogQueueDiskNonReliable *)s;

  ScratchBuffersMarker marker;
  GString *serialized_msg = NULL;

  if (_is_msg_serialization_needed_hint(self))
    {
      serialized_msg = scratch_buffers_alloc_and_mark(&marker);
      if (!qdisk_serialize_msg(self->super.qdisk, msg, serialized_msg))
        {
          msg_error("Failed to serialize message for non-reliable disk-buffer, dropping message",
                    evt_tag_str("filename", qdisk_get_filename (self->super.qdisk)),
                    evt_tag_str("persist_name", s->persist_name));
          _drop_msg(self, msg, path_options);
          scratch_buffers_reclaim_marked(marker);
          return;
        }
    }

  g_static_mutex_lock(&s->lock);

  LogPathOptions local_options = *path_options;

  if (HAS_SPACE_IN_QUEUE(self->qout) && qdisk_get_length (self->super.qdisk) == 0)
    {
      /* simple push never generates flow-control enabled entries to qout, they only get there
       * when rewinding the backlog */

      g_queue_push_tail (self->qout, msg);
      g_queue_push_tail (self->qout, LOG_PATH_OPTIONS_FOR_BACKLOG);
      log_msg_ref (msg);

      log_queue_memory_usage_add(s, log_msg_get_size(msg));
    }
  else
    {
      if (self->qoverflow->length != 0 || !_ensure_serialized_and_write_to_disk(self, msg, serialized_msg))
        {
          if (HAS_SPACE_IN_QUEUE(self->qoverflow))
            {
              g_queue_push_tail (self->qoverflow, msg);
              g_queue_push_tail (self->qoverflow, LOG_PATH_OPTIONS_TO_POINTER (path_options));
              log_msg_ref (msg);
              local_options.ack_needed = FALSE;
              log_queue_memory_usage_add(s, log_msg_get_size(msg));
            }
          else
            {
              msg_debug ("Destination queue full, dropping message",
                         evt_tag_str  ("filename", qdisk_get_filename (self->super.qdisk)),
                         evt_tag_long ("queue_len", log_queue_get_length(s)),
                         evt_tag_int  ("mem_buf_length", self->qoverflow_size),
                         evt_tag_long ("disk_buf_size", qdisk_get_maximum_size (self->super.qdisk)),
                         evt_tag_str  ("persist_name", s->persist_name));
              _drop_msg(self, msg, path_options);
              goto exit;
            }
        }
    }

  log_queue_push_notify(s);
  log_queue_queued_messages_inc(s);
  log_msg_ack(msg, &local_options, AT_PROCESSED);
  log_msg_unref(msg);

exit:
  g_static_mutex_unlock(&s->lock);
  if (serialized_msg)
    scratch_buffers_reclaim_marked(marker);
}

static void
_free_queue (GQueue *q)
{
  while (!g_queue_is_empty (q))
    {
      LogMessage *lm;
      LogPathOptions path_options = LOG_PATH_OPTIONS_INIT;

      lm = g_queue_pop_head (q);
      POINTER_TO_LOG_PATH_OPTIONS (g_queue_pop_head (q), &path_options);
      log_msg_ack (lm, &path_options, AT_PROCESSED);
      log_msg_unref (lm);
    }
  g_queue_free (q);
}

static void
_free(LogQueue *s)
{
  LogQueueDiskNonReliable *self = (LogQueueDiskNonReliable *)s;

  _free_queue (self->qoverflow);
  self->qoverflow = NULL;
  _free_queue (self->qout);
  self->qout = NULL;
  _free_queue (self->qbacklog);
  self->qbacklog = NULL;

  log_queue_disk_free_method(&self->super);
}

static gboolean
_load_queue (LogQueueDisk *s, const gchar *filename)
{
  /* qdisk portion is not yet started when this happens */
  g_assert(!qdisk_started (s->qdisk));

  return _start(s, filename);
}

static gboolean
_save_queue (LogQueueDisk *s, gboolean *persistent)
{
  gboolean success = FALSE;
  LogQueueDiskNonReliable *self = (LogQueueDiskNonReliable *) s;
  if (qdisk_save_state (s->qdisk, self->qout, self->qbacklog, self->qoverflow))
    {
      *persistent = TRUE;
      success = TRUE;
    }
  qdisk_stop (s->qdisk);
  return success;
}

static void
_restart(LogQueueDisk *s, DiskQueueOptions *options)
{
  LogQueueDiskNonReliable *self = (LogQueueDiskNonReliable *) s;
  qdisk_init_instance(self->super.qdisk, options, "SLQF");
}

static inline void
_set_logqueue_virtual_functions(LogQueue *s)
{
  s->get_length = _get_length;
  s->ack_backlog = _ack_backlog;
  s->rewind_backlog = _rewind_backlog;
  s->rewind_backlog_all = _rewind_backlog_all;
  s->pop_head = _pop_head;
  s->push_head = _push_head;
  s->push_tail = _push_tail;
  s->free_fn = _free;
}

static inline void
_set_logqueue_disk_virtual_functions(LogQueueDisk *s)
{
  s->start = _start;
  s->load_queue = _load_queue;
  s->save_queue = _save_queue;
  s->restart = _restart;
}

static inline void
_set_virtual_functions(LogQueueDiskNonReliable *self)
{
  _set_logqueue_virtual_functions(&self->super.super);
  _set_logqueue_disk_virtual_functions(&self->super);
}

LogQueue *
log_queue_disk_non_reliable_new(DiskQueueOptions *options, const gchar *persist_name)
{
  g_assert(options->reliable == FALSE);
  LogQueueDiskNonReliable *self = g_new0(LogQueueDiskNonReliable, 1);
  log_queue_disk_init_instance(&self->super, options, "SLQF", persist_name);
  self->qbacklog = g_queue_new ();
  self->qout = g_queue_new ();
  self->qoverflow = g_queue_new ();
  self->qout_size = options->qout_size;
  self->qoverflow_size = options->mem_buf_length;
  _set_virtual_functions(self);
  return &self->super.super;
}
