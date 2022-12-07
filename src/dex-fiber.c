/* dex-fiber.c
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

#include <glib.h>

#include <stdarg.h>
#include <setjmp.h>
#include <ucontext.h>
#include <unistd.h>

#include "dex-fiber-private.h"
#include "dex-object-private.h"
#include "dex-platform.h"

typedef struct _DexFiberClass
{
  DexObjectClass parent_class;
} DexFiberClass;

DEX_DEFINE_FINAL_TYPE (DexFiber, dex_fiber, DEX_TYPE_OBJECT)

static void
dex_fiber_finalize (DexObject *object)
{
  DexFiber *fiber = DEX_FIBER (object);

  g_clear_pointer (&fiber->stack, dex_stack_free);

  DEX_OBJECT_CLASS (dex_fiber_parent_class)->finalize (object);
}

static void
dex_fiber_class_init (DexFiberClass *fiber_class)
{
  DexObjectClass *object_class = DEX_OBJECT_CLASS (fiber_class);

  object_class->finalize = dex_fiber_finalize;
}

static void
dex_fiber_init (DexFiber *fiber)
{
}

static void
dex_fiber_start (DexFiber *fiber)
{
  fiber->func (fiber, fiber->func_data);
}

static void
dex_fiber_start_ (int arg1, ...)
{
  DexFiber *fiber;

#if GLIB_SIZEOF_VOID_P == 4
  fiber = GSIZE_TO_POINTER (arg1);
#else
  va_list args;
  gsize hi;
  gsize lo;

  hi = arg1;
  va_start (args, arg1);
  lo = va_arg (args, int);
  va_end (args);

  fiber = GSIZE_TO_POINTER ((hi << 32) | lo);
#endif

  g_assert (DEX_IS_FIBER (fiber));

  dex_fiber_start (fiber);
}

DexFiber *
dex_fiber_new (DexFiberFunc func,
               gpointer     func_data,
               gsize        stack_size)
{
  DexFiber *fiber;
#if GLIB_SIZEOF_VOID_P == 8
  int lo;
  int hi;
#endif

  g_return_val_if_fail (func != NULL, NULL);

  fiber = (DexFiber *)g_type_create_instance (DEX_TYPE_FIBER);
  fiber->func = func;
  fiber->func_data = func_data;

  fiber->stack = dex_stack_new (stack_size);

  getcontext (&fiber->context);

  fiber->context.uc_stack.ss_size = fiber->stack->size;
  fiber->context.uc_stack.ss_sp = fiber->stack->base;
  fiber->context.uc_link = 0;

#if GLIB_SIZEOF_VOID_P == 8
  lo = GPOINTER_TO_SIZE (fiber) & 0xFFFFFFFFF;
  hi = (GPOINTER_TO_SIZE (fiber) >> 32) & 0xFFFFFFFFF;
#endif

  makecontext (&fiber->context,
               G_CALLBACK (dex_fiber_start_),
#if GLIB_SIZEOF_VOID_P == 4
               1, (gsize)fiber,
#else
               2, hi, lo
#endif
              );

  return fiber;
}

void
dex_fiber_swap_to (DexFiber *fiber,
                   DexFiber *to)
{
  g_assert (DEX_IS_FIBER (fiber));
  g_assert (DEX_IS_FIBER (to));

  swapcontext (&fiber->context, &to->context);
}
