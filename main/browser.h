#pragma once

/* Two-level emulator / ROM picker. A opens the selected emulator or ROM;
 * B returns from the ROM browser to the emulator menu. */
int browser_run(char *out_path, int out_path_size);
