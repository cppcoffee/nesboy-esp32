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

#ifndef INPUT_H
#define INPUT_H

typedef enum
{
  BUTTON_L      = 0x200,
  BUTTON_R      = 0x100,
  BUTTON_DOWN   = 0x80,
  BUTTON_UP     = 0x40,
  BUTTON_LEFT   = 0x20,
  BUTTON_RIGHT  = 0x10,
  BUTTON_START  = 0x08,
  BUTTON_SELECT = 0x04,
  BUTTON_B      = 0x02,
  BUTTON_A      = 0x01,
  BUTTON_NONE   = 0x00
} input_buttons_type;

/* Button mask (input_buttons_type bits) the core should see this frame.
 * Set it before calling update_input(). */
extern u32 gba_button_state;

u32 update_input(void);

bool input_check_savestate(const u8 *src);
unsigned input_write_savestate(u8* dst);
bool input_read_savestate(const u8 *src);

#endif
