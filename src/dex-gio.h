/*
 * dex-gio.h
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

#pragma once

#include <gio/gio.h>

#include "dex-future.h"

G_BEGIN_DECLS

DEX_AVAILABLE_IN_ALL
DexFuture *dex_file_read                 (GFile         *file,
                                          int            priority);
DEX_AVAILABLE_IN_ALL
DexFuture *dex_input_stream_read         (GInputStream  *self,
                                          gpointer       buffer,
                                          gsize          count,
                                          int            priority);
DEX_AVAILABLE_IN_ALL
DexFuture *dex_input_stream_read_bytes   (GInputStream  *self,
                                          gsize          count,
                                          int            priority);
DEX_AVAILABLE_IN_ALL
DexFuture *dex_output_stream_write       (GOutputStream *self,
                                          gconstpointer  buffer,
                                          gsize          count,
                                          int            priority);
DEX_AVAILABLE_IN_ALL
DexFuture *dex_output_stream_write_bytes (GOutputStream *self,
                                          GBytes        *bytes,
                                          int            priority);

G_END_DECLS
