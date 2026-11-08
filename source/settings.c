#include <stdio.h>
#include <sys/stat.h>

#include "settings.h"

#define SETTINGS_DIR  "sdmc:/3ds/YoRHA-3ds-MP"
#define SETTINGS_PATH SETTINGS_DIR "/settings.ini"

void settings_load(Settings *s)
{
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f)
        return;

    char line[64];
    int v;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "theme=%d", &v) == 1)
            s->theme = v;
        else if (sscanf(line, "visualizer=%d", &v) == 1)
            s->viz_on = v != 0;
        else if (sscanf(line, "visualizer_mode=%d", &v) == 1)
            s->viz_mode = v;
    }
    fclose(f);
}

void settings_save(const Settings *s)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir(SETTINGS_DIR, 0777);

    FILE *f = fopen(SETTINGS_PATH, "w");
    if (!f)
        return;
    fprintf(f, "theme=%d\nvisualizer=%d\nvisualizer_mode=%d\n", s->theme, s->viz_on, s->viz_mode);
    fclose(f);
}
