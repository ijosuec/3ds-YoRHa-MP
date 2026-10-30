#pragma once

#include <stdbool.h>

#include "art.h"
#include "tags.h"

typedef struct {
    int index;
    TrackInfo info;
    ArtPixels art;
} LoadResult;

bool loader_init(int art_box);
void loader_exit(void);

void loader_request(int index, const char *path);

bool loader_poll(LoadResult *out);
