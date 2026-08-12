/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of version 2 of the GNU Library General
** Public License as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Library General Public License for more details.  To obtain a
** copy of the GNU Library General Public License, write to the Free
** Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** Any permitted reproduction of these routines, in whole or in part,
** must bear this legend.
**
**
** nes/state.h: Save state support header
**
*/

#pragma once

#include <stddef.h>
#include <stdint.h>

int state_load(const char *fn);
int state_save(const char *fn);

/* In-memory state snapshot for rewind.
 * state_save_mem writes a compact snapshot into buf and returns the number of
 * bytes written (or -1 on error). state_load_mem restores from buf and returns
 * 0 on success, -1 on error. buf must be at least state_mem_size() bytes. */
size_t state_mem_size(void);
int state_save_mem(uint8_t *buf);
int state_load_mem(const uint8_t *buf);
