#pragma once

#include "rewind.h"

/* Handle save/load combos for one frame and return the rewind GPIO level.
 * Loading takes priority over rewind while Rewind+Select remains held. */
int savestate_handle_buttons(const char *rom_path, const rewind_backend_t *backend, int buttons);
