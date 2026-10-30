#pragma once

#include <citro2d.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    C3D_Tex tex;
    Tex3DS_SubTexture sub;
    C2D_Image img;
    bool valid;
} AlbumArt;

typedef struct {
    u32 *pixels;
    u32 tex_size;
    u32 width, height;
} ArtPixels;

bool art_decode(ArtPixels *out, const uint8_t *data, size_t size, int box);
void art_pixels_free(ArtPixels *px);

bool art_upload(AlbumArt *art, const ArtPixels *px);

bool art_load_background(AlbumArt *art, const uint8_t *data, size_t size);

void art_free(AlbumArt *art);
