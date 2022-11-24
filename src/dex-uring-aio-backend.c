/* dex-uring-aio-backend.c
 *
 * Copyright 2022 Christian Hergert <chergert@redhat.com>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <liburing.h>

#include "dex-uring-aio-backend-private.h"
#include "dex-uring-future-private.h"

#define DEFAULT_URING_SIZE 32

struct _DexUringAioBackend
{
  DexAioBackend parent_instance;
};

struct _DexUringAioBackendClass
{
  DexAioBackendClass parent_class;
};

typedef struct _DexUringAioContext
{
  DexAioContext    parent;
  struct io_uring  ring;
  int              eventfd;
  gpointer         eventfdtag;
  GMutex           mutex;
  GQueue           queued;
} DexUringAioContext;

DEX_DEFINE_FINAL_TYPE (DexUringAioBackend, dex_uring_aio_backend, DEX_TYPE_AIO_BACKEND)

static gboolean
dex_uring_aio_context_dispatch (GSource     *source,
                                GSourceFunc  callback,
                                gpointer     user_data)
{
  DexUringAioContext *aio_context = (DexUringAioContext *)source;
  struct io_uring_cqe *cqe;
  gint64 counter;

  if (g_source_query_unix_fd (source, aio_context->eventfdtag) & G_IO_IN)
    read (aio_context->eventfd, &counter, sizeof counter);

  while (io_uring_peek_cqe (&aio_context->ring, &cqe) == 0)
    {
      /* TODO: it would be nice if we could freeze completion
       *  of these items while we're in the hot path of the
       *  io_uring so it can go back to producing.
       */
      DexUringFuture *future = io_uring_cqe_get_data (cqe);
      dex_uring_future_complete (future, cqe);
      io_uring_cqe_seen (&aio_context->ring, cqe);
      dex_unref (future);
    }

  g_mutex_lock (&aio_context->mutex);
  if (io_uring_sq_ready (&aio_context->ring))
    io_uring_submit (&aio_context->ring);
  g_mutex_unlock (&aio_context->mutex);

  return G_SOURCE_CONTINUE;
}

static gboolean
dex_uring_aio_context_prepare (GSource *source,
                               int     *timeout)
{
  DexUringAioContext *aio_context = (DexUringAioContext *)source;

  g_assert (aio_context != NULL);
  g_assert (DEX_IS_URING_AIO_BACKEND (aio_context->parent.aio_backend));

  *timeout = -1;

  g_mutex_lock (&aio_context->mutex);

  if (io_uring_sq_ready (&aio_context->ring) > 0)
    io_uring_submit (&aio_context->ring);

  if (aio_context->queued.length)
    {
      struct io_uring_sqe *sqe;
      gboolean do_submit = FALSE;

      while (aio_context->queued.length &&
             (sqe = io_uring_get_sqe (&aio_context->ring)))
        {
          DexUringFuture *future = g_queue_pop_head (&aio_context->queued);
          dex_uring_future_prepare (future, sqe);
          io_uring_sqe_set_data (sqe, dex_ref (future));
          do_submit = TRUE;
        }

      if (do_submit)
        io_uring_submit (&aio_context->ring);
    }

  g_mutex_unlock (&aio_context->mutex);

  return io_uring_cq_ready (&aio_context->ring) > 0;
}

static gboolean
dex_uring_aio_context_check (GSource *source)
{
  DexUringAioContext *aio_context = (DexUringAioContext *)source;

  g_assert (aio_context != NULL);
  g_assert (DEX_IS_URING_AIO_BACKEND (aio_context->parent.aio_backend));

  return io_uring_cq_ready (&aio_context->ring) > 0;
}

static void
dex_uring_aio_context_finalize (GSource *source)
{
  DexUringAioContext *aio_context = (DexUringAioContext *)source;

  g_assert (aio_context != NULL);
  g_assert (DEX_IS_URING_AIO_BACKEND (aio_context->parent.aio_backend));

  if (aio_context->queued.length > 0)
    g_critical ("Destroying DexAioContext with queued items!");

  io_uring_queue_exit (&aio_context->ring);
  dex_clear (&aio_context->parent.aio_backend);
  g_mutex_clear (&aio_context->mutex);

  if (aio_context->eventfd != -1)
    {
      close (aio_context->eventfd);
      aio_context->eventfd = -1;
    }
}

static GSourceFuncs dex_uring_aio_context_source_funcs = {
  .check = dex_uring_aio_context_check,
  .prepare = dex_uring_aio_context_prepare,
  .dispatch = dex_uring_aio_context_dispatch,
  .finalize = dex_uring_aio_context_finalize,
};

static DexFuture *
dex_uring_aio_context_queue (DexUringAioContext *aio_context,
                             DexUringFuture     *future)
{
  struct io_uring_sqe *sqe;

  g_assert (aio_context != NULL);
  g_assert (DEX_IS_URING_AIO_BACKEND (aio_context->parent.aio_backend));
  g_assert (DEX_IS_URING_FUTURE (future));

  g_mutex_lock (&aio_context->mutex);

  if G_LIKELY (aio_context->queued.length == 0 &&
               (sqe = io_uring_get_sqe (&aio_context->ring)))
    {
      dex_uring_future_prepare (future, sqe);
      io_uring_sqe_set_data (sqe, dex_ref (future));
    }
  else
    {
      g_queue_push_tail (&aio_context->queued, dex_ref (future));
    }

  g_mutex_unlock (&aio_context->mutex);

  /* TODO: If we're pushing onto this aio ring from a thread that isn't
   * processing it, we would have to wake up the GMainContext too.
   */

  return DEX_FUTURE (future);
}

static DexAioContext *
dex_uring_aio_backend_create_context (DexAioBackend *aio_backend)
{
  DexUringAioBackend *uring_aio_backend = DEX_URING_AIO_BACKEND (aio_backend);
  DexUringAioContext *aio_context;
  guint uring_flags = 0;

  g_assert (DEX_IS_URING_AIO_BACKEND (uring_aio_backend));

  aio_context = (DexUringAioContext *)
    g_source_new (&dex_uring_aio_context_source_funcs,
                  sizeof *aio_context);
  aio_context->parent.aio_backend = dex_ref (aio_backend);
  g_mutex_init (&aio_context->mutex);

#ifdef IORING_SETUP_COOP_TASKRUN
  uring_flags |= IORING_SETUP_COOP_TASKRUN;
#endif

  aio_context->eventfd = -1;

  /* Setup uring submission/completion queue */
  if (io_uring_queue_init (DEFAULT_URING_SIZE, &aio_context->ring, uring_flags) != 0)
    goto failure;

  /* Register the ring FD so we don't have to on every io_ring_enter() */
  if (io_uring_register_ring_fd (&aio_context->ring) < 0)
    goto failure;

  /* Create eventfd() we can poll() on with GMainContext since GMainContext
   * knows nothing of uring and how to drive the loop using that.
   */
  if (-1 == (aio_context->eventfd = eventfd (0, EFD_CLOEXEC)) ||
      io_uring_register_eventfd (&aio_context->ring, aio_context->eventfd) != 0)
    goto failure;

  /* Add the eventfd() to our set of pollfds and keep the tag around so
   * we can check the condition directly.
   */
  aio_context->eventfdtag = g_source_add_unix_fd ((GSource *)aio_context,
                                                  aio_context->eventfd,
                                                  G_IO_IN);

  return (DexAioContext *)aio_context;

failure:
  g_source_unref ((GSource *)aio_context);

  return NULL;
}

static DexFuture *
dex_uring_aio_backend_read (DexAioBackend *aio_backend,
                            DexAioContext *aio_context,
                            int            fd,
                            gpointer       buffer,
                            gsize          count,
                            goffset        offset)
{
  return dex_uring_aio_context_queue ((DexUringAioContext *)aio_context,
                                      dex_uring_future_new_read (fd, buffer, count, offset));
}

static DexFuture *
dex_uring_aio_backend_write (DexAioBackend *aio_backend,
                             DexAioContext *aio_context,
                             int            fd,
                             gconstpointer  buffer,
                             gsize          count,
                             goffset        offset)
{
  return dex_uring_aio_context_queue ((DexUringAioContext *)aio_context,
                                      dex_uring_future_new_write (fd, buffer, count, offset));
}

static void
dex_uring_aio_backend_class_init (DexUringAioBackendClass *uring_aio_backend_class)
{
  DexAioBackendClass *aio_backend_class = DEX_AIO_BACKEND_CLASS (uring_aio_backend_class);

  aio_backend_class->create_context = dex_uring_aio_backend_create_context;
  aio_backend_class->read = dex_uring_aio_backend_read;
  aio_backend_class->write = dex_uring_aio_backend_write;
}

static void
dex_uring_aio_backend_init (DexUringAioBackend *uring_aio_backend)
{
}

DexAioBackend *
dex_uring_aio_backend_new (void)
{
  return (DexAioBackend *)g_type_create_instance (DEX_TYPE_URING_AIO_BACKEND);
}