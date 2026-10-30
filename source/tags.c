#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tags.h"

#define MAX_TAG_SIZE    (16 * 1024 * 1024)
#define MAX_COVER_FILE  (8 * 1024 * 1024)

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static uint32_t syncsafe32(const uint8_t *p)
{
    return (uint32_t)(p[0] & 0x7f) << 21 | (p[1] & 0x7f) << 14 | (p[2] & 0x7f) << 7 | (p[3] & 0x7f);
}

static size_t unsync(uint8_t *d, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        d[o++] = d[i];
        if (d[i] == 0xFF && i + 1 < n && d[i + 1] == 0x00)
            i++;
    }
    return o;
}

static size_t put_utf8(char *out, size_t pos, size_t cap, uint32_t cp)
{
    char b[4];
    size_t n;

    if (cp < 0x80) {
        b[0] = cp;
        n = 1;
    } else if (cp < 0x800) {
        b[0] = 0xC0 | (cp >> 6);
        b[1] = 0x80 | (cp & 0x3F);
        n = 2;
    } else if (cp < 0x10000) {
        b[0] = 0xE0 | (cp >> 12);
        b[1] = 0x80 | ((cp >> 6) & 0x3F);
        b[2] = 0x80 | (cp & 0x3F);
        n = 3;
    } else {
        b[0] = 0xF0 | (cp >> 18);
        b[1] = 0x80 | ((cp >> 12) & 0x3F);
        b[2] = 0x80 | ((cp >> 6) & 0x3F);
        b[3] = 0x80 | (cp & 0x3F);
        n = 4;
    }

    if (pos + n >= cap)
        return pos;
    memcpy(out + pos, b, n);
    return pos + n;
}

static size_t trim_partial_utf8(const char *s, size_t len)
{
    size_t lead = len;
    while (lead > 0 && ((unsigned char)s[lead - 1] & 0xC0) == 0x80)
        lead--;
    if (lead == 0)
        return len;

    unsigned char c = s[lead - 1];
    size_t want = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    return (len - (lead - 1) < want) ? lead - 1 : len;
}

static void decode_text(int enc, const uint8_t *s, size_t n, char *out, size_t cap)
{
    size_t pos = 0;

    if (enc == 0) {
        for (size_t i = 0; i < n && s[i]; i++)
            pos = put_utf8(out, pos, cap, s[i]);
    } else if (enc == 3) {
        for (size_t i = 0; i < n && s[i] && pos + 1 < cap; i++)
            out[pos++] = s[i];
        pos = trim_partial_utf8(out, pos);
    } else {
        int big = (enc == 2);
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            if (s[0] == 0xFF && s[1] == 0xFE) {
                big = 0;
                i = 2;
            } else if (s[0] == 0xFE && s[1] == 0xFF) {
                big = 1;
                i = 2;
            }
        }
        for (; i + 1 < n; i += 2) {
            uint32_t u = big ? (s[i] << 8 | s[i + 1]) : (s[i + 1] << 8 | s[i]);
            if (u == 0)
                break;
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
                uint32_t lo = big ? (s[i + 2] << 8 | s[i + 3]) : (s[i + 3] << 8 | s[i + 2]);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            pos = put_utf8(out, pos, cap, u);
        }
    }
    out[pos] = '\0';
}

static void take_picture(TrackInfo *info, int *best_type, int ver, const uint8_t *d, size_t n)
{
    if (n < 4)
        return;

    int enc = d[0];
    size_t i = 1;
    if (ver == 2) {
        i += 3;
    } else {
        while (i < n && d[i])
            i++;
        i++;
    }
    if (i >= n)
        return;

    int type = d[i++];

    if (enc == 1 || enc == 2) {
        while (i + 1 < n && (d[i] || d[i + 1]))
            i += 2;
        i += 2;
    } else {
        while (i < n && d[i])
            i++;
        i++;
    }
    if (i >= n)
        return;

    if (info->art && !(type == 3 && *best_type != 3))
        return;

    free(info->art);
    info->art_size = 0;
    info->art = malloc(n - i);
    if (!info->art)
        return;
    memcpy(info->art, d + i, n - i);
    info->art_size = n - i;
    *best_type = type;
}

static void parse_id3v2(FILE *f, TrackInfo *info)
{
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) != 10 || memcmp(hdr, "ID3", 3) != 0)
        return;

    int ver = hdr[3];
    int flags = hdr[5];
    size_t size = syncsafe32(hdr + 6);
    if (ver < 2 || ver > 4 || size == 0 || size > MAX_TAG_SIZE)
        return;

    uint8_t *tag = malloc(size);
    if (!tag)
        return;
    size = fread(tag, 1, size, f);

    if (ver < 4 && (flags & 0x80))
        size = unsync(tag, size);

    size_t pos = 0;
    if (ver >= 3 && (flags & 0x40) && size >= 4)
        pos = (ver == 3) ? be32(tag) + 4 : syncsafe32(tag);

    size_t frame_hdr = (ver == 2) ? 6 : 10;
    int best_type = -1;

    while (pos + frame_hdr <= size) {
        const uint8_t *h = tag + pos;
        if (h[0] == 0)
            break;

        char id[5] = {0};
        size_t fsize;
        int fmt = 0;
        if (ver == 2) {
            memcpy(id, h, 3);
            fsize = h[3] << 16 | h[4] << 8 | h[5];
        } else {
            memcpy(id, h, 4);
            fsize = (ver == 3) ? be32(h + 4) : syncsafe32(h + 4);
            fmt = h[9];
        }

        pos += frame_hdr;
        if (fsize > size - pos)
            break;
        uint8_t *d = tag + pos;
        size_t n = fsize;
        pos += fsize;

        if (ver == 3) {
            if (fmt & 0xC0)
                continue;
            if ((fmt & 0x20) && n >= 1) {
                d++;
                n--;
            }
        } else if (ver == 4) {
            if (fmt & 0x0C)
                continue;
            if ((fmt & 0x40) && n >= 1) {
                d++;
                n--;
            }
            if ((fmt & 0x01) && n >= 4) {
                d += 4;
                n -= 4;
            }
            if ((fmt & 0x02) || (flags & 0x80))
                n = unsync(d, n);
        }
        if (n < 1)
            continue;

        if (!strcmp(id, "TIT2") || !strcmp(id, "TT2"))
            decode_text(d[0], d + 1, n - 1, info->title, sizeof(info->title));
        else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1"))
            decode_text(d[0], d + 1, n - 1, info->artist, sizeof(info->artist));
        else if (!strcmp(id, "TALB") || !strcmp(id, "TAL"))
            decode_text(d[0], d + 1, n - 1, info->album, sizeof(info->album));
        else if (!strcmp(id, "APIC") || !strcmp(id, "PIC"))
            take_picture(info, &best_type, ver, d, n);
    }

    free(tag);
}

static void load_cover_file(const char *path, TrackInfo *info)
{
    static const char *names[] = {
        "cover.jpg", "folder.jpg", "front.jpg", "cover.png", "folder.png", "front.png",
    };

    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = '\0';

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char p[600];
        snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
        FILE *f = fopen(p, "rb");
        if (!f)
            continue;

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        if (sz > 0 && sz <= MAX_COVER_FILE && (info->art = malloc(sz))) {
            info->art_size = fread(info->art, 1, sz, f);
            fclose(f);
            return;
        }
        fclose(f);
    }
}

void tags_read(const char *path, TrackInfo *info)
{
    memset(info, 0, sizeof(*info));

    FILE *f = fopen(path, "rb");
    if (f) {
        parse_id3v2(f, info);
        fclose(f);
    }

    if (!info->title[0]) {
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        snprintf(info->title, sizeof(info->title), "%s", name);
        char *dot = strrchr(info->title, '.');
        if (dot)
            *dot = '\0';
    }

    if (!info->art)
        load_cover_file(path, info);
}

void tags_free(TrackInfo *info)
{
    free(info->art);
    info->art = NULL;
    info->art_size = 0;
}
