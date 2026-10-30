#pragma once

#include <3ds/types.h>
#include <stdbool.h>

bool audio_init(void);
void audio_exit(void);

bool audio_play(const char *path);
void audio_toggle_pause(void);
bool audio_is_paused(void);

void audio_seek(float seconds);

bool audio_is_finished(void);

float audio_position(void);

float audio_duration(void);
