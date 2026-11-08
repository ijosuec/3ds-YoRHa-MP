#pragma once

#include <stdbool.h>

typedef struct {
    int theme;
    bool viz_on;
    int viz_mode;
} Settings;

void settings_load(Settings *s);
void settings_save(const Settings *s);
