#pragma once

#include <stdint.h>

/* Initialize the HT517 I2S output. */
void audio_init(void);

/* Convert one mono NES PCM frame to stereo I2S. Queue backpressure paces emulation. */
void audio_write(const int16_t *buf, int samples);

/* Scale and queue one interleaved stereo PCM frame from the Game Boy core. */
void audio_write_stereo(const int16_t *buf, int samples);

/* Drop queued audio, used when gameplay is paused for rewind. */
void audio_flush(void);

/* Adjust volume by ±delta percent points (clamped 0–100). */
void audio_change_volume(int delta);

/* Get current volume percentage. */
int audio_get_volume(void);
