#pragma once

#include "app_config.h"

#if NES_ENABLE_FRAME_STATS
void frame_stats_begin(void);
void frame_stats_audio_wait_begin(void);
void frame_stats_audio_wait_end(void);
void frame_stats_end(void);
#else
static inline void frame_stats_begin(void)
{
}
static inline void frame_stats_audio_wait_begin(void)
{
}
static inline void frame_stats_audio_wait_end(void)
{
}
static inline void frame_stats_end(void)
{
}
#endif
