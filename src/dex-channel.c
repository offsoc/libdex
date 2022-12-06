/*
 * dex-channel.c
 *
 * Copyright 2022 Christian Hergert <chergert@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "dex-error.h"
#include "dex-channel.h"
#include "dex-future-private.h"
#include "dex-object-private.h"
#include "dex-promise.h"

typedef enum _DexChannelStateFlags
{
  DEX_CHANNEL_STATE_CAN_SEND    = 1 << 0,
  DEX_CHANNEL_STATE_CAN_RECEIVE = 1 << 1,
} DexChannelStateFlags;

struct _DexChannel
{
  DexObject parent_instance;

  /* Queue of senders waiting to insert an item in queue once the
   * capacity threshold has reduced.
   */
  GQueue sendq;

  /* Queue of receivers waiting for an item to be inserted into the
   * queue so they can have it.
   */
  GQueue recvq;

  /* The actual queue of items in the channel that have been sent and
   * not yet picked up by a receiver.
   */
  GQueue queue;

  /* The max capacity of @queue. Both the @sendq and @recvq can be
   * greater than this as those represent callers and their futures.
   */
  guint capacity;

  /* Flags indicating what sides of the channel are open/closed */
  DexChannelStateFlags flags : 2;
};

typedef struct _DexChannelClass
{
  DexObjectClass parent_class;
} DexChannelClass;

typedef struct _DexChannelItem
{
  /* Used to insert the item into queues */
  GList link;

  /* The future which is provided to the caller of dex_channel_send(). */
  DexPromise *send;

  /* The future which was sent with dex_channel_send(). */
  DexFuture *future;
} DexChannelItem;

DEX_DEFINE_FINAL_TYPE (DexChannel, dex_channel, DEX_TYPE_OBJECT)

static void dex_channel_unset_state_flags (DexChannel           *channel,
                                           DexChannelStateFlags  flags);

static GError channel_closed_error;

static DexChannelItem *
dex_channel_item_new (DexFuture *future)
{
  DexChannelItem *item;

  g_assert (DEX_IS_FUTURE (future));

  item = g_new0 (DexChannelItem, 1);
  item->link.data = item;
  item->future = dex_ref (future);
  item->send = dex_promise_new ();

  return item;
}

static void
dex_channel_item_free (DexChannelItem *item)
{
  g_assert (item != NULL);
  g_assert (!item->future || DEX_IS_FUTURE (item->future));
  g_assert (!item->send || DEX_IS_PROMISE (item->send));
  g_assert (item->link.data == item);
  g_assert (item->link.prev == NULL);
  g_assert (item->link.next == NULL);

  dex_clear (&item->future);
  dex_clear (&item->send);
  g_free (item);
}

static void
dex_channel_finalize (DexObject *object)
{
  DexChannel *channel = DEX_CHANNEL (object);

  dex_channel_unset_state_flags (channel,
                                 (DEX_CHANNEL_STATE_CAN_SEND |
                                  DEX_CHANNEL_STATE_CAN_RECEIVE));

  g_assert (channel->queue.length == 0);
  g_assert (channel->sendq.length == 0);
  g_assert (channel->recvq.length == 0);
  g_assert (channel->flags == 0);

  DEX_OBJECT_CLASS (dex_channel_parent_class)->finalize (object);
}

static void
dex_channel_class_init (DexChannelClass *channel_class)
{
  DexObjectClass *object_class = DEX_OBJECT_CLASS (channel_class);

  object_class->finalize = dex_channel_finalize;

  channel_closed_error = (GError) {
    .domain = DEX_ERROR,
    .code = DEX_ERROR_CHANNEL_CLOSED,
    .message = (gpointer)"Channel closed",
  };
}

static void
dex_channel_init (DexChannel *channel)
{
  channel->capacity = G_MAXUINT;
}

/**
 * dex_channel_new:
 * @capacity: the channel queue depth or 0 for unlimited
 *
 * Creates a new #DexChannel.
 *
 * If capacity is non-zero, it can be used to limit the size of the channel
 * so that functions can asynchronously stall until items have been removed
 * from the channel. This is useful in buffering situations so that the
 * producer does not outpace the consumer.
 *
 * Returns: a new #DexChannel
 */
DexChannel *
dex_channel_new (guint capacity)
{
  DexChannel *channel;

  if (capacity == 0)
    capacity = G_MAXUINT;

  channel = (DexChannel *)g_type_create_instance (DEX_TYPE_CHANNEL);
  channel->capacity = capacity;
  channel->flags = DEX_CHANNEL_STATE_CAN_SEND | DEX_CHANNEL_STATE_CAN_RECEIVE;

  return channel;
}

static inline gboolean
has_capacity_locked (DexChannel *channel)
{
  return channel->sendq.length == 0 && channel->queue.length < channel->capacity;
}

static void
dex_channel_one_receive_and_unlock (DexChannel *channel)
{
  DexChannelItem *item = NULL;
  DexPromise *promise = NULL;
  DexPromise *to_resolve = NULL;
  guint qlen = 0;

  g_assert (DEX_IS_CHANNEL (channel));

  /* This function removes a single item from the head of the queue and
   * pairs it with a promise that was delivered to a caller of
   * dex_channel_receive(). The #DexPromise they were returned is
   * completed using the future that was provided to dex_channel_send()
   * (which itself still may not be ready, but we must preserve ordering).
   */

  if (channel->queue.length > 0 && channel->recvq.length > 0)
    {
      promise = g_queue_pop_head (&channel->recvq);
      item = g_queue_pop_head_link (&channel->queue)->data;

      g_assert (DEX_IS_PROMISE (promise));
      g_assert (item != NULL);

      /* Try to advance a @sendq item into @queue */
      if (channel->sendq.length > 0 && channel->queue.length < channel->capacity)
        {
          DexChannelItem *sendq_item = g_queue_peek_head (&channel->sendq);
          g_queue_unlink (&channel->sendq, &sendq_item->link);
          g_queue_push_tail (&channel->queue, &sendq_item->link);
          qlen = channel->queue.length;
          to_resolve = dex_ref (sendq_item->send);
        }
    }

  dex_object_unlock (channel);

  g_assert (item == NULL || promise != NULL);

  if (item != NULL)
    {
      dex_future_chain (item->future, DEX_FUTURE (promise));
      dex_channel_item_free (item);
      dex_unref (promise);
    }

  if (to_resolve != NULL)
    {
      dex_promise_resolve_uint (to_resolve, qlen);
      dex_unref (to_resolve);
    }
}

/**
 * dex_channel_send:
 * @channel: a #DexChannel
 * @future: a #DexFuture
 *
 * Queues @future into the channel.
 *
 * The other end of the channel can receive the future (or a future that will
 * eventually resolve to @future) using dex_channel_receive().
 *
 * This function returns a #DexFuture that will resolve when the channels
 * capacity is low enough to queue more items.
 *
 * If the send side of the channel is closed, the returned #DexFuture will be
 * rejected with %DEX_ERROR_CHANNEL_CLOSED.
 *
 * Returns: (transfer full): a #DexFuture
 */
DexFuture *
dex_channel_send (DexChannel *channel,
                  DexFuture  *future)
{
  const DexChannelStateFlags required = DEX_CHANNEL_STATE_CAN_SEND|DEX_CHANNEL_STATE_CAN_RECEIVE;
  DexChannelItem *item;
  DexFuture *ret;

  g_return_val_if_fail (DEX_IS_CHANNEL (channel), NULL);
  g_return_val_if_fail (DEX_IS_FUTURE (future), NULL);

  item = dex_channel_item_new (future);

  dex_object_lock (channel);

  if ((channel->flags & required) != required)
    {
      dex_object_unlock (channel);
      dex_channel_item_free (item);
      return DEX_FUTURE (dex_future_new_reject (DEX_ERROR,
                                                DEX_ERROR_CHANNEL_CLOSED,
                                                "Channel is closed"));
    }

  ret = dex_ref (item->send);

  /* Try to place future directly into @queue. Otherwise, place it in @sendq
   * and we'll process it when more items have been received.
   */
  if (!has_capacity_locked (channel))
    {
      g_queue_push_tail_link (&channel->sendq, &item->link);
      dex_object_unlock (channel);
    }
  else
    {
      g_queue_push_tail_link (&channel->queue, &item->link);
      dex_promise_resolve_uint (item->send, channel->queue.length);
      dex_channel_one_receive_and_unlock (channel);
    }

  return ret;
}

DexFuture *
dex_channel_receive (DexChannel *channel)
{
  DexPromise *recv;

  g_return_val_if_fail (DEX_IS_CHANNEL (channel), NULL);

  recv = dex_promise_new ();

  dex_object_lock (channel);

  if ((channel->flags & DEX_CHANNEL_STATE_CAN_RECEIVE) == 0)
    goto reject_receive;

  /* If no more items can be sent, and there are no items immediately
   * to fullfil this request, then we have to reject as it can never
   * be fullfilled.
   */
  if ((channel->flags & DEX_CHANNEL_STATE_CAN_SEND) == 0)
    {
      if (channel->queue.length + channel->sendq.length <= channel->recvq.length)
        goto reject_receive;
    }

  /* Enqueue this receiver and then flush a queued operation if possible */
  g_queue_push_tail (&channel->recvq, dex_ref (recv));
  dex_channel_one_receive_and_unlock (channel);

  return DEX_FUTURE (recv);

reject_receive:
  dex_object_unlock (channel);
  dex_promise_reject (recv,
                      g_error_new (DEX_ERROR,
                                   DEX_ERROR_CHANNEL_CLOSED,
                                   "Channel is closed"));
  return DEX_FUTURE (recv);
}

static inline guint
steal_uint (guint *value)
{
  guint ret = *value;
  *value = 0;
  return ret;
}

static inline GQueue
steal_queue (GQueue *queue)
{
  return (GQueue) {
    .head = g_steal_pointer (&queue->head),
    .tail = g_steal_pointer (&queue->tail),
    .length = steal_uint (&queue->length),
  };
}

static void
dex_channel_unset_state_flags (DexChannel           *channel,
                               DexChannelStateFlags  flags)
{
  GQueue queue = G_QUEUE_INIT;
  GQueue sendq = G_QUEUE_INIT;
  GQueue recvq = G_QUEUE_INIT;
  GQueue trunc = G_QUEUE_INIT;

  g_assert (DEX_IS_CHANNEL (channel));

  dex_object_lock (channel);

  /* If we need to close the send-side, do so now */
  if (flags & DEX_CHANNEL_STATE_CAN_SEND)
    {
      guint pending = channel->sendq.length + channel->queue.length;

      channel->flags &= ~DEX_CHANNEL_STATE_CAN_SEND;

      while (channel->recvq.length > pending)
        g_queue_push_head (&trunc, g_queue_pop_tail (&channel->recvq));
    }

  /* If we need to close the receive-side, do so now and steal
   * all of the items and promises that need to be rejected.
   */
  if (flags & DEX_CHANNEL_STATE_CAN_RECEIVE)
    {
      channel->flags &= ~DEX_CHANNEL_STATE_CAN_RECEIVE;

      queue = steal_queue (&channel->queue);
      sendq = steal_queue (&channel->sendq);
      recvq = steal_queue (&channel->recvq);
    }

  dex_object_unlock (channel);

  while (recvq.length > 0)
    {
      DexPromise *promise = g_queue_pop_head (&recvq);
      dex_promise_reject (promise, g_error_copy (&channel_closed_error));
      dex_unref (promise);
    }

  while (trunc.length > 0)
    {
      DexPromise *promise = g_queue_pop_head (&trunc);
      dex_promise_reject (promise, g_error_copy (&channel_closed_error));
      dex_unref (promise);
    }

  while (queue.length > 0)
    {
      DexChannelItem *item = g_queue_peek_head (&queue);
      g_queue_unlink (&queue, &item->link);
      dex_channel_item_free (item);
    }

  while (sendq.length > 0)
    {
      DexChannelItem *item = g_queue_peek_head (&sendq);
      g_queue_unlink (&sendq, &item->link);
      dex_promise_reject (item->send, g_error_copy (&channel_closed_error));
      dex_channel_item_free (item);
    }
}

void
dex_channel_close_send (DexChannel *channel)
{
  g_return_if_fail (DEX_IS_CHANNEL (channel));

  dex_channel_unset_state_flags (channel, DEX_CHANNEL_STATE_CAN_SEND);
}

void
dex_channel_close_receive (DexChannel *channel)
{
  g_return_if_fail (DEX_IS_CHANNEL (channel));

  dex_channel_unset_state_flags (channel, DEX_CHANNEL_STATE_CAN_RECEIVE);
}

gboolean
dex_channel_can_send (DexChannel *channel)
{
  gboolean ret;

  g_return_val_if_fail (DEX_IS_CHANNEL (channel), FALSE);

  dex_object_lock (channel);
  ret = (channel->flags & DEX_CHANNEL_STATE_CAN_SEND) != 0;
  dex_object_unlock (channel);

  return ret;
}

gboolean
dex_channel_can_receive (DexChannel *channel)
{
  gboolean ret;

  g_return_val_if_fail (DEX_IS_CHANNEL (channel), FALSE);

  dex_object_lock (channel);
  ret = (channel->flags & DEX_CHANNEL_STATE_CAN_RECEIVE) != 0;
  dex_object_unlock (channel);

  return ret;
}
