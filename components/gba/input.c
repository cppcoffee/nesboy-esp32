/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 * Ported away from libretro for nesboy-esp32.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include "common.h"

u32 gba_button_state = 0;

static u32 old_key = 0;

static void trigger_key(u32 key)
{
  u32 p1_cnt = read_ioreg(REG_P1CNT);

  if((p1_cnt >> 14) & 0x01)
  {
    u32 key_intersection = (p1_cnt & key) & 0x3FF;

    if(p1_cnt >> 15)
    {
      if(key_intersection == (p1_cnt & 0x3FF))
      {
        flag_interrupt(IRQ_KEYPAD);
        check_and_raise_interrupts();
      }
    }
    else
    {
      if(key_intersection)
      {
        flag_interrupt(IRQ_KEYPAD);
        check_and_raise_interrupts();
      }
    }
  }
}

u32 update_input(void)
{
   u32 new_key = gba_button_state & 0x3FF;

   if ((new_key | old_key) != old_key)
      trigger_key(new_key);

   old_key = new_key;
   write_ioreg(REG_P1, (~old_key) & 0x3FF);

   return 0;
}

bool input_check_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  return (p && bson_contains_key(p, "prevkey", BSON_TYPE_INT32));
}

bool input_read_savestate(const u8 *src)
{
  const u8 *p = bson_find_key(src, "input");
  if (p)
    return bson_read_int32(p, "prevkey", &old_key);
  return false;
}

unsigned input_write_savestate(u8 *dst)
{
  u8 *wbptr1, *startp = dst;
  bson_start_document(dst, "input", wbptr1);
  bson_write_int32(dst, "prevkey", old_key);
  bson_finish_document(dst, wbptr1);
  return (unsigned int)(dst - startp);
}
