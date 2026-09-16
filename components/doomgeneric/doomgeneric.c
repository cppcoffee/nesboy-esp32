#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include "m_argv.h"

#include "doomgeneric.h"

pixel_t* DG_ScreenBuffer = NULL;

void M_FindResponseFile(void);
void D_DoomMain (void);


void doomgeneric_Create(int argc, char **argv)
{
	// save arguments
    myargc = argc;
    myargv = argv;

	M_FindResponseFile();

#ifdef ESP_PLATFORM
    DG_ScreenBuffer = heap_caps_malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4,
                                      MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
#else
	DG_ScreenBuffer = malloc(DOOMGENERIC_RESX * DOOMGENERIC_RESY * 4);
#endif

    if (DG_ScreenBuffer == NULL)
    {
        fprintf(stderr, "Unable to allocate Doom framebuffer\n");
        return;
    }

	DG_Init();

	D_DoomMain ();
}
