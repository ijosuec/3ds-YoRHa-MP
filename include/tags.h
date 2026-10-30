#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    uint8_t *art;
    size_t art_size;
} TrackInfo;

void tags_read(const char *path, TrackInfo *info);
void tags_free(TrackInfo *info);
