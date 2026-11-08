#include <stdlib.h>
#include <string.h>

#include <turbojpeg.h>

#include "art.h"
#include "stb_image.h"

#define ART_MAX_PIXELS (2048 * 2048)

static inline u32 tex_index(u32 x, u32 y, u32 width)
{
    return ((((y >> 3) * (width >> 3) + (x >> 3)) << 6) +
            ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
             ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3)));
}

static u8 *decode_jpeg(const uint8_t *data, size_t size, int box, int *w, int *h)
{
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    if (!tj)
        return NULL;

    u8 *px = NULL;
    if (tj3DecompressHeader(tj, data, size) < 0)
        goto done;
    int sw = tj3Get(tj, TJPARAM_JPEGWIDTH), sh = tj3Get(tj, TJPARAM_JPEGHEIGHT);
    int src_long = sw > sh ? sw : sh;

    int count;
    tjscalingfactor *factors = tj3GetScalingFactors(&count);
    tjscalingfactor best = TJUNSCALED;
    for (int i = 0; factors && i < count; i++) {
        if (factors[i].num > factors[i].denom)
            continue;
        if (TJSCALED(src_long, factors[i]) >= box)
            best = factors[i];
    }

    if (tj3SetScalingFactor(tj, best) < 0)
        goto done;
    *w = TJSCALED(sw, best);
    *h = TJSCALED(sh, best);
    if ((u64)*w * *h > ART_MAX_PIXELS)
        goto done;

    px = malloc((size_t)*w * *h * 3);
    if (px && tj3Decompress8(tj, data, size, px, 0, TJPF_RGB) < 0) {
        free(px);
        px = NULL;
    }
done:
    tj3Destroy(tj);
    return px;
}

static u8 *decode_image(const uint8_t *data, size_t size, int box, int *w, int *h)
{
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
        u8 *px = decode_jpeg(data, size, box, w, h);
        if (px)
            return px;
    }

    int comp;
    if (!stbi_info_from_memory(data, size, w, h, &comp) || (u64)*w * *h > ART_MAX_PIXELS)
        return NULL;
    u8 *px = stbi_load_from_memory(data, size, w, h, &comp, 3);
    if (!px)
        return NULL;

    size_t bytes = (size_t)*w * *h * 3;
    u8 *copy = malloc(bytes);
    if (copy)
        memcpy(copy, px, bytes);
    stbi_image_free(px);
    return copy;
}

static u32 next_pow2(u32 n)
{
    u32 p = 8;
    while (p < n)
        p <<= 1;
    return p;
}

bool art_decode(ArtPixels *out, const uint8_t *data, size_t size, int box)
{
    memset(out, 0, sizeof(*out));

    int w, h;
    u8 *px = decode_image(data, size, box, &w, &h);
    if (!px)
        return false;

    u32 dw, dh;
    if (w >= h) {
        dw = box;
        dh = h * box / w;
    } else {
        dh = box;
        dw = w * box / h;
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    u32 ts = next_pow2(box);
    u32 *dst = calloc(ts * ts, sizeof(u32));
    if (!dst) {
        free(px);
        return false;
    }

    for (u32 y = 0; y < dh; y++) {
        u32 sy0 = y * h / dh, sy1 = (y + 1) * h / dh;
        if (sy1 <= sy0) sy1 = sy0 + 1;

        for (u32 x = 0; x < dw; x++) {
            u32 sx0 = x * w / dw, sx1 = (x + 1) * w / dw;
            if (sx1 <= sx0) sx1 = sx0 + 1;

            u32 r = 0, g = 0, b = 0, count = 0;
            for (u32 sy = sy0; sy < sy1; sy++) {
                const u8 *row = px + (sy * w + sx0) * 3;
                for (u32 sx = sx0; sx < sx1; sx++, row += 3) {
                    r += row[0];
                    g += row[1];
                    b += row[2];
                    count++;
                }
            }
            r /= count;
            g /= count;
            b /= count;
            dst[tex_index(x, y, ts)] = r << 24 | g << 16 | b << 8 | 0xFF;
        }
    }
    free(px);

    out->pixels = dst;
    out->tex_size = ts;
    out->width = dw;
    out->height = dh;
    return true;
}

void art_pixels_free(ArtPixels *px)
{
    free(px->pixels);
    px->pixels = NULL;
}

bool art_upload(AlbumArt *art, const ArtPixels *px)
{
    memset(art, 0, sizeof(*art));
    u32 ts = px->tex_size;
    if (!px->pixels || !C3D_TexInit(&art->tex, ts, ts, GPU_RGBA8))
        return false;

    memcpy(art->tex.data, px->pixels, ts * ts * sizeof(u32));
    C3D_TexFlush(&art->tex);
    C3D_TexSetFilter(&art->tex, GPU_LINEAR, GPU_LINEAR);

    art->sub = (Tex3DS_SubTexture){
        .width = px->width,
        .height = px->height,
        .left = 0.0f,
        .top = 1.0f,
        .right = (float)px->width / ts,
        .bottom = 1.0f - (float)px->height / ts,
    };
    art->img = (C2D_Image){ .tex = &art->tex, .subtex = &art->sub };
    art->valid = true;
    return true;
}

static int luma(const u8 *p)
{
    return (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
}

static void remap_pixels(u8 *px, int count, const BgRemap *remap)
{
    static int hist[256];
    memset(hist, 0, sizeof(hist));
    int lo = 255;
    for (int i = 0; i < count; i++) {
        int l = luma(px + i * 3);
        hist[l]++;
        if (l < lo)
            lo = l;
    }
    int base = 0;
    for (int l = 1; l < 256; l++)
        if (hist[l] > hist[base])
            base = l;
    int range = base > lo ? base - lo : 1;

    for (int i = 0; i < count; i++) {
        u8 *p = px + i * 3;
        int t = (base - luma(p)) * 256 / range;
        if (t < 0)
            t = 0;
        for (int c = 0; c < 3; c++)
            p[c] = remap->paper[c] + ((remap->ink[c] - remap->paper[c]) * t >> 8);
    }
}

bool art_load_background(AlbumArt *art, const uint8_t *data, size_t size, const BgRemap *remap)
{
    memset(art, 0, sizeof(*art));

    int w, h, comp;
    u8 *px = stbi_load_from_memory(data, size, &w, &h, &comp, 3);
    if (!px)
        return false;
    if (remap)
        remap_pixels(px, w * h, remap);

    u32 tw = next_pow2(w), th = next_pow2(h);
    if (tw > 1024 || th > 1024 || !C3D_TexInit(&art->tex, tw, th, GPU_RGBA8)) {
        stbi_image_free(px);
        return false;
    }

    u32 *dst = art->tex.data;
    memset(dst, 0, tw * th * sizeof(u32));
    for (int y = 0; y < h; y++) {
        const u8 *row = px + y * w * 3;
        for (int x = 0; x < w; x++, row += 3)
            dst[tex_index(x, y, tw)] = row[0] << 24 | row[1] << 16 | row[2] << 8 | 0xFF;
    }
    stbi_image_free(px);

    C3D_TexFlush(&art->tex);
    C3D_TexSetFilter(&art->tex, GPU_NEAREST, GPU_NEAREST);

    art->sub = (Tex3DS_SubTexture){
        .width = w,
        .height = h,
        .left = 0.0f,
        .top = 1.0f,
        .right = (float)w / tw,
        .bottom = 1.0f - (float)h / th,
    };
    art->img = (C2D_Image){ .tex = &art->tex, .subtex = &art->sub };
    art->valid = true;
    return true;
}

void art_free(AlbumArt *art)
{
    if (art->valid)
        C3D_TexDelete(&art->tex);
    art->valid = false;
}
