#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "art.h"
#include "audio.h"
#include "battery.h"
#include "library.h"
#include "loader.h"
#include "settings.h"
#include "visualizer.h"

#include "bg_top_png.h"
#include "bg_bottom_png.h"

#define MUSIC_DIR   "sdmc:/music"

#define TOP_W       400
#define BOTTOM_W    320
#define SCREEN_H    240

#define MARGIN      16

#define ART_SIZE    128
#define ART_X       20
#define ART_Y       44
#define FIELD_X     (ART_X + ART_SIZE + 18)
#define FIELD_W     (TOP_W - FIELD_X - MARGIN)
#define FIELD_STEP  34

#define LIST_Y      18
#define ROW_H       17
#define LIST_ROWS   8
#define LIST_X      10
#define LIST_W      (BOTTOM_W - 2 * LIST_X - 6)

#define SEEK_Y      171
#define SEEK_TOP    158
#define SEEK_BOTTOM 186
#define SEEK_X0     56
#define SEEK_X1     (BOTTOM_W - 56)

#define BTN_COUNT   5
#define BTN_Y       188
#define BTN_H       24
#define BTN_W       70
#define BTN_SMALL_W 24
#define BTN_GAP     10
#define BTN_ROW_W   (3 * BTN_W + 2 * BTN_SMALL_W + 4 * BTN_GAP)
#define BTN_X0      ((BOTTOM_W - BTN_ROW_W) / 2)

#define DRAG_THRESHOLD 6

#define PREV_RESTART_SECONDS 3.0f
#define SKIP_SECONDS         10.0f

#define INTRO_LINE_FRAMES   12
#define INTRO_OPEN_FRAMES   18
#define INTRO_STAGGER       6
#define INTRO_GROUPS        4
#define INTRO_FRAMES        ((INTRO_GROUPS - 1) * INTRO_STAGGER + INTRO_LINE_FRAMES + INTRO_OPEN_FRAMES)

#define GLITCH_FRAMES       14
#define GLITCH_SLICES       8

#define TYPE_CHAR_FRAMES    2
#define TYPE_STAGGER        8

#define BATTERY_POLL_FRAMES 120
#define BATTERY_SEGS        4
#define BATTERY_W           22
#define BATTERY_H           10
#define BATTERY_LOW         15

#define VIZ_SEGS    16
#define VIZ_SEG_H   4
#define VIZ_SEG_GAP 1
#define VIZ_BAR_GAP 3
#define VIZ_BOTTOM  (ART_Y + ART_SIZE)
#define VIZ_BAR_W   ((FIELD_W - (VIZ_BARS - 1) * VIZ_BAR_GAP) / (float)VIZ_BARS)
#define VIZ_PITCH   (VIZ_SEG_H + VIZ_SEG_GAP)
#define VIZ_TOP     (VIZ_BOTTOM - VIZ_SEGS * VIZ_PITCH + VIZ_SEG_GAP)
#define VIZ_MID     ((VIZ_TOP + VIZ_BOTTOM) / 2)

enum { BTN_VIZ, BTN_PREV, BTN_PLAY, BTN_NEXT, BTN_THEME };
enum { TOUCH_NONE, TOUCH_SEEK, TOUCH_LIST, TOUCH_BUTTON };

static C2D_TextBuf info_buf;
static C2D_TextBuf scratch_buf;

static C2D_Text title_text, artist_text, album_text;

static Library lib;
static AlbumArt art, bg_top, bg_bottom;
static int current = -1, pending = -1;
static int cursor = 0, scroll = 0;
static bool audio_ok, play_ok;
static bool art_loading;

static struct {
    int mode;
    int button;
    touchPosition start, last;
    int scroll_start;
    bool moved;
} touch;

static u32 clr_bg, clr_text, clr_dim, clr_faint, clr_panel, clr_shadow, clr_error;
static u64 frame_count;

static s64 glitch_frame = -GLITCH_FRAMES;
static s64 type_frame = -1000;
static char cur_title[256], cur_artist[256], cur_album[256];
static C3D_Mtx unfold_mtx;
static bool viz_on;
static int viz_mode;
static s64 mode_frame;
static int battery_pct = -1;
static bool battery_charging;

static float anim_t(s64 start, int delay, int dur)
{
    float t = (float)((s64)frame_count - start - delay) / dur;
    return t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
}

static float ease_out(float t)
{
    t = 1.0f - t;
    return 1.0f - t * t * t;
}

static u32 rand_next(u32 *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return *seed >> 8;
}

static u32 with_alpha(u32 color, float a)
{
    return (color & 0x00ffffff) | ((u32)(a * 255.0f) << 24);
}

static bool intro_done(void)
{
    return frame_count >= INTRO_FRAMES;
}

static void parse_fit(C2D_Text *t, C2D_TextBuf buf, const char *s, float scale, float max_w)
{
    char tmp[300];
    size_t len = strlen(s);
    snprintf(tmp, sizeof(tmp), "%s", s);

    for (int tries = 0; tries < 8; tries++) {
        float w;
        C2D_TextBufClear(scratch_buf);
        C2D_TextParse(t, scratch_buf, tmp);
        C2D_TextGetDimensions(t, scale, scale, &w, NULL);
        if (w <= max_w || len == 0)
            break;

        size_t keep = (size_t)(len * (max_w / w) * 0.95f);
        if (keep >= len)
            keep = len - 1;
        while (keep > 0 && ((unsigned char)s[keep] & 0xC0) == 0x80)
            keep--;
        len = keep;
        snprintf(tmp, sizeof(tmp), "%.*s...", (int)len, s);
    }

    C2D_TextParse(t, buf, tmp);
    C2D_TextOptimize(t);
}

#define TEXT_CACHE_SIZE   48
#define TEXT_CACHE_GLYPHS 160

typedef struct {
    char str[256];
    float scale, max_w;
    C2D_TextBuf buf;
    C2D_Text text;
    u64 last_used;
} CachedText;

static CachedText text_cache[TEXT_CACHE_SIZE];

static C2D_Text *cached_text(const char *s, float scale, float max_w)
{
    int victim = 0;
    for (int i = 0; i < TEXT_CACHE_SIZE; i++) {
        if (text_cache[i].last_used && text_cache[i].scale == scale &&
            text_cache[i].max_w == max_w && strcmp(text_cache[i].str, s) == 0) {
            text_cache[i].last_used = frame_count + 1;
            return &text_cache[i].text;
        }
        if (text_cache[i].last_used < text_cache[victim].last_used)
            victim = i;
    }

    CachedText *e = &text_cache[victim];
    if (!e->buf)
        e->buf = C2D_TextBufNew(TEXT_CACHE_GLYPHS);
    snprintf(e->str, sizeof(e->str), "%s", s);
    e->scale = scale;
    e->max_w = max_w;
    e->last_used = frame_count + 1;
    C2D_TextBufClear(e->buf);
    parse_fit(&e->text, e->buf, s, scale, max_w);
    return &e->text;
}

static void draw_text(const char *s, float x, float y, float scale, u32 color, float max_w, u32 flags)
{
    C2D_DrawText(cached_text(s, scale, max_w), C2D_WithColor | flags, x, y, 0.5f, scale, scale, color);
}

static void draw_string(const char *s, float x, float y, float scale, u32 color, float max_w)
{
    draw_text(s, x, y, scale, color, max_w, 0);
}

static bool draw_typed(const char *s, float x, float y, float scale, u32 color, float max_w, s64 start)
{
    s64 elapsed = (s64)frame_count - start;
    if (elapsed < 0)
        return false;

    size_t len = strlen(s);
    size_t n = elapsed / TYPE_CHAR_FRAMES;
    if (n >= len)
        return true;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;

    float w = 0.0f;
    if (n > 0) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%.*s", (int)n, s);
        C2D_Text *t = cached_text(tmp, scale, max_w);
        C2D_TextGetDimensions(t, scale, scale, &w, NULL);
        if (w >= max_w * 0.97f)
            return true;
        C2D_DrawText(t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
    }
    if ((elapsed / 4) % 2 == 0)
        C2D_DrawRectSolid(x + w + 2, y + 4 * scale, 0.5f, 10 * scale, 20 * scale, color);
    return false;
}

static void display_name(const char *path, char *out, size_t cap)
{
    const char *name = strrchr(path, '/');
    snprintf(out, cap, "%s", name ? name + 1 : path);
    char *dot = strrchr(out, '.');
    if (dot)
        *dot = '\0';
}

static void scroll_to_cursor(void)
{
    if (cursor < scroll)
        scroll = cursor;
    if (cursor >= scroll + LIST_ROWS)
        scroll = cursor - LIST_ROWS + 1;
}

static void set_track_text(const char *title, const char *artist, const char *album)
{
    if (strcmp(title, cur_title) || strcmp(artist, cur_artist) || strcmp(album, cur_album)) {
        snprintf(cur_title, sizeof(cur_title), "%s", title);
        snprintf(cur_artist, sizeof(cur_artist), "%s", artist);
        snprintf(cur_album, sizeof(cur_album), "%s", album);
        type_frame = frame_count;
    }

    C2D_TextBufClear(info_buf);
    parse_fit(&title_text, info_buf, title, 0.6f, FIELD_W);
    parse_fit(&artist_text, info_buf, artist, 0.55f, FIELD_W);
    parse_fit(&album_text, info_buf, album, 0.55f, FIELD_W);
}

static void load_track(int index)
{
    current = cursor = index;
    scroll_to_cursor();
    const char *path = lib.paths[index];

    play_ok = audio_ok && audio_play(path);

    art_free(&art);
    art_loading = true;
    loader_request(index, path);
    glitch_frame = frame_count;

    char name[256];
    display_name(path, name, sizeof(name));
    set_track_text(name, "...", "...");
}

static void receive_track_info(void)
{
    LoadResult r;
    if (!loader_poll(&r))
        return;

    if (r.index == current) {
        TrackInfo *info = &r.info;
        set_track_text(info->title, info->artist[0] ? info->artist : "Unknown",
                       info->album[0] ? info->album : "-");
        art_free(&art);
        if (r.art.pixels && art_upload(&art, &r.art))
            glitch_frame = frame_count;
        art_loading = false;
    }
    art_pixels_free(&r.art);
}

static void format_time(char *out, size_t cap, float seconds)
{
    if (seconds <= 0.0f) {
        snprintf(out, cap, "--:--");
        return;
    }
    int secs = (int)seconds;
    snprintf(out, cap, "%02d:%02d", secs / 60, secs % 60);
}

static bool can_control(void)
{
    return audio_ok && lib.count > 0 && current >= 0;
}

static void action_prev(void)
{
    if (play_ok && audio_position() > PREV_RESTART_SECONDS)
        pending = current;
    else
        pending = (current - 1 + lib.count) % lib.count;
}

static void action_next(void)
{
    pending = (current + 1) % lib.count;
}

static void action_skip(float seconds)
{
    if (!play_ok)
        return;
    float pos = audio_position() + seconds;
    float len = audio_duration();
    if (len > 0.0f && pos >= len)
        action_next();
    else
        audio_seek(pos < 0.0f ? 0.0f : pos);
}

static float seek_frac(int px)
{
    float frac = (float)(px - SEEK_X0) / (SEEK_X1 - SEEK_X0);
    return frac < 0.0f ? 0.0f : frac > 1.0f ? 1.0f : frac;
}

static void draw_panel(float x, float y, float w, float h, u32 fill)
{
    C2D_DrawRectSolid(x + 3, y + 3, 0.5f, w, h, clr_shadow);
    C2D_DrawRectSolid(x, y, 0.5f, w, h, fill);
}

static void draw_frame(float x, float y, float w, float h, u32 color)
{
    C2D_DrawRectSolid(x, y, 0.5f, w, 1, color);
    C2D_DrawRectSolid(x, y + h - 1, 0.5f, w, 1, color);
    C2D_DrawRectSolid(x, y, 0.5f, 1, h, color);
    C2D_DrawRectSolid(x + w - 1, y, 0.5f, 1, h, color);
}

static void draw_tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 c)
{
    C2D_DrawTriangle(x0, y0, c, x1, y1, c, x2, y2, c, 0.5f);
}

static void draw_header(const char *caption, float x, float y, float w)
{
    C2D_DrawRectSolid(x, y + 3, 0.5f, 7, 7, clr_text);
    draw_string(caption, x + 13, y - 1, 0.5f, clr_text, w - 13);
    C2D_DrawRectSolid(x, y + 16, 0.5f, w, 1, clr_dim);
}

static bool begin_unfold_at(s64 start, float x, float cy, float w, int order)
{
    int delay = order * INTRO_STAGGER;
    float line = anim_t(start, delay, INTRO_LINE_FRAMES);
    float open = anim_t(start, delay + INTRO_LINE_FRAMES, INTRO_OPEN_FRAMES);

    if (open < 1.0f && line > 0.0f) {
        float lw = w * ease_out(line);
        C2D_DrawRectSolid(x + (w - lw) / 2, cy, 0.5f, lw, 1, with_alpha(clr_text, 1.0f - open));
    }
    if (open <= 0.0f)
        return false;

    C2D_ViewSave(&unfold_mtx);
    if (open < 1.0f) {
        C2D_ViewTranslate(0, cy);
        C2D_ViewScale(1, ease_out(open));
        C2D_ViewTranslate(0, -cy);
    }
    return true;
}

static bool begin_unfold(float x, float cy, float w, int order)
{
    return begin_unfold_at(0, x, cy, w, order);
}

static void end_unfold(void)
{
    C2D_ViewRestore(&unfold_mtx);
}

static float glitch_t(void)
{
    return anim_t(glitch_frame, 0, GLITCH_FRAMES);
}

static void draw_glitch_bands(float w, u32 salt)
{
    float t = glitch_t();
    if (t >= 1.0f)
        return;

    float k = 1.0f - t;
    u32 seed = (u32)frame_count * 2654435761u ^ salt;
    int bands = 2 + (int)(4 * k);
    for (int i = 0; i < bands; i++) {
        float y = rand_next(&seed) % SCREEN_H;
        float h = 1 + rand_next(&seed) % 4;
        float x = rand_next(&seed) % (int)(w / 2);
        float bw = w / 4 + rand_next(&seed) % (int)(w / 2);
        C2D_DrawRectSolid(x, y, 0.5f, bw, h, with_alpha(i & 1 ? clr_dim : clr_text, 0.45f * k));
    }
    C2D_DrawRectSolid(0, SCREEN_H * t, 0.5f, w, 2, with_alpha(clr_text, 0.35f * k));
}

static void draw_art(float x, float y)
{
    float t = glitch_t();
    if (t >= 1.0f) {
        C2D_DrawImageAt(art.img, x, y, 0.5f, NULL, 1.0f, 1.0f);
        return;
    }

    float k = 1.0f - t;
    u32 seed = (u32)frame_count * 2246822519u;
    const Tex3DS_SubTexture *s = &art.sub;
    float vspan = s->top - s->bottom;
    for (int i = 0; i < GLITCH_SLICES; i++) {
        int h0 = s->height * i / GLITCH_SLICES;
        int h1 = s->height * (i + 1) / GLITCH_SLICES;
        Tex3DS_SubTexture sub = *s;
        sub.height = h1 - h0;
        sub.top = s->top - vspan * h0 / s->height;
        sub.bottom = s->top - vspan * h1 / s->height;
        C2D_Image img = { .tex = art.img.tex, .subtex = &sub };

        float dx = 0.0f;
        if (rand_next(&seed) % 3 == 0)
            dx = ((int)(rand_next(&seed) % 13) - 6) * k;
        C2D_DrawImageAt(img, x + dx, y + h0, 0.5f, NULL, 1.0f, 1.0f);
    }
}

static void draw_rail(float x0, float x1, float cy, float frac, bool active)
{
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    float xk = x0 + (x1 - x0) * frac;

    C2D_DrawRectSolid(x0, cy - 1, 0.5f, x1 - x0, 2, clr_faint);
    C2D_DrawRectSolid(x0, cy - 2, 0.5f, xk - x0, 4, clr_text);
    C2D_DrawRectSolid(x0 - 1, cy - 5, 0.5f, 1, 10, clr_dim);
    C2D_DrawRectSolid(x1, cy - 5, 0.5f, 1, 10, clr_dim);

    float k = active ? 6 : 4;
    C2D_DrawRectSolid(xk - k, cy - k, 0.5f, 2 * k, 2 * k, clr_text);
    C2D_DrawRectSolid(xk - k + 2, cy - k + 2, 0.5f, 2 * k - 4, 2 * k - 4, clr_bg);
}

static void draw_error_screen(const char *title, const char *line1, const char *line2)
{
    if (begin_unfold(MARGIN, 30, TOP_W - 2 * MARGIN, 0)) {
        draw_header(title, MARGIN, 22, TOP_W - 2 * MARGIN);
        end_unfold();
    }
    if (begin_unfold(MARGIN, 77, TOP_W - 2 * MARGIN, 1)) {
        draw_panel(MARGIN, 52, TOP_W - 2 * MARGIN, 50, clr_panel);
        draw_string(line1, MARGIN + 10, 58, 0.5f, clr_error, TOP_W - 2 * MARGIN - 20);
        draw_string(line2, MARGIN + 10, 78, 0.45f, clr_text, TOP_W - 2 * MARGIN - 20);
        end_unfold();
    }
}

static int seg_count(float v, int segs)
{
    int n = (int)(v * segs + 0.5f);
    return n > segs ? segs : n;
}

static int peak_index(float v, int segs)
{
    int n = (int)(v * segs);
    return n > segs - 1 ? segs - 1 : n;
}

static u32 seg_color(int s, int lit, int top, bool base)
{
    if (s < lit)
        return clr_text;
    if ((s == top && top > 0) || (base && s == 0))
        return clr_dim;
    return with_alpha(clr_faint, 0.6f);
}

static void draw_spectrum(void)
{
    for (int b = 0; b < VIZ_BARS; b++) {
        float x = FIELD_X + b * (VIZ_BAR_W + VIZ_BAR_GAP);
        int lit = seg_count(viz_level(b), VIZ_SEGS);
        int top = peak_index(viz_peak(b), VIZ_SEGS);
        for (int s = 0; s < VIZ_SEGS; s++) {
            float y = VIZ_BOTTOM - (s + 1) * VIZ_PITCH + VIZ_SEG_GAP;
            C2D_DrawRectSolid(x, y, 0.5f, VIZ_BAR_W, VIZ_SEG_H, seg_color(s, lit, top, true));
        }
    }
}

static void draw_mirror(void)
{
    int half = VIZ_SEGS / 2;
    for (int b = 0; b < VIZ_BARS; b++) {
        float x = FIELD_X + b * (VIZ_BAR_W + VIZ_BAR_GAP);
        int lit = seg_count(viz_level(b), half);
        int top = peak_index(viz_peak(b), half);
        for (int s = 0; s < half; s++) {
            u32 c = seg_color(s, lit, top, true);
            float up = VIZ_MID - 1 - (s + 1) * VIZ_PITCH + VIZ_SEG_GAP;
            float down = VIZ_MID + 1 + s * VIZ_PITCH;
            C2D_DrawRectSolid(x, up, 0.5f, VIZ_BAR_W, VIZ_SEG_H, c);
            C2D_DrawRectSolid(x, down, 0.5f, VIZ_BAR_W, VIZ_SEG_H, c);
        }
    }
}

typedef struct {
    const char *name;
    void (*draw)(void);
} VizMode;

static const VizMode viz_modes[] = {
    { "SPECTRUM", draw_spectrum },
    { "MIRROR", draw_mirror },
};

#define VIZ_MODES ((int)(sizeof(viz_modes) / sizeof(viz_modes[0])))

static void viz_switch(void)
{
    mode_frame = frame_count - 2 * INTRO_STAGGER;
    glitch_frame = frame_count;
    type_frame = frame_count;
}

static void update_battery(void)
{
    if (frame_count % BATTERY_POLL_FRAMES != 0)
        return;
    int pct;
    bool charging;
    if (battery_read(&pct, &charging)) {
        battery_pct = pct;
        battery_charging = charging;
    }
}

static void draw_battery(float right, float y)
{
    if (battery_pct < 0)
        return;

    float x = right - BATTERY_W - 2;
    bool low = battery_pct <= BATTERY_LOW && !battery_charging;
    u32 c = low ? clr_error : clr_dim;

    draw_frame(x, y, BATTERY_W, BATTERY_H, c);
    C2D_DrawRectSolid(x + BATTERY_W, y + 3, 0.5f, 2, BATTERY_H - 6, c);

    int lit = (battery_pct + 24) / 25;
    if (battery_charging) {
        int step = (frame_count / 20) % (BATTERY_SEGS + 1);
        if (step > lit)
            lit = step;
    }
    float inner = BATTERY_W - 4;
    float seg_w = (inner - (BATTERY_SEGS - 1)) / BATTERY_SEGS;
    for (int i = 0; i < lit && i < BATTERY_SEGS; i++)
        C2D_DrawRectSolid(x + 2 + i * (seg_w + 1), y + 2, 0.5f, seg_w, BATTERY_H - 4, c);

    char label[8];
    snprintf(label, sizeof(label), "%d%%", battery_pct);
    draw_text(label, x - 4, y - 2, 0.4f, c, 40, C2D_AlignRight);
}

static void draw_viz_fields(float y)
{
    const VizMode *m = &viz_modes[viz_mode];
    if (draw_typed(m->name, FIELD_X, y, 0.4f, clr_dim, FIELD_W, type_frame + TYPE_STAGGER))
        draw_string(m->name, FIELD_X, y, 0.4f, clr_dim, FIELD_W);
    m->draw();
}

static void draw_track_fields(float y)
{
    draw_string("ARTIST", FIELD_X, y, 0.4f, clr_dim, FIELD_W);
    if (draw_typed(cur_artist, FIELD_X, y + 11, 0.55f, clr_text, FIELD_W, type_frame + TYPE_STAGGER))
        C2D_DrawText(&artist_text, C2D_WithColor, FIELD_X, y + 11, 0.5f, 0.55f, 0.55f, clr_text);
    y += FIELD_STEP;
    draw_string("ALBUM", FIELD_X, y, 0.4f, clr_dim, FIELD_W);
    if (draw_typed(cur_album, FIELD_X, y + 11, 0.55f, clr_text, FIELD_W, type_frame + 2 * TYPE_STAGGER))
        C2D_DrawText(&album_text, C2D_WithColor, FIELD_X, y + 11, 0.5f, 0.55f, 0.55f, clr_text);
    y += FIELD_STEP;

    char line[64];
    snprintf(line, sizeof(line), "TRACK %03d / %03d", current + 1, lib.count);
    if (draw_typed(line, FIELD_X, y + 4, 0.45f, clr_text, FIELD_W, type_frame + 3 * TYPE_STAGGER))
        draw_string(line, FIELD_X, y + 4, 0.45f, clr_text, FIELD_W);

    const char *state = !play_ok ? "UNPLAYABLE FILE" : audio_is_paused() ? "PAUSED" : "PLAYING";
    float tag_w = 108;
    C2D_DrawRectSolid(FIELD_X, y + 22, 0.5f, tag_w, 15, play_ok ? clr_text : clr_error);
    if (draw_typed(state, FIELD_X + 6, y + 23, 0.42f, clr_bg, tag_w - 12, type_frame + 4 * TYPE_STAGGER))
        draw_text(state, FIELD_X + tag_w / 2, y + 23, 0.42f, clr_bg, tag_w - 4, C2D_AlignCenter);
}

static void draw_top(C3D_RenderTarget *target)
{
    C2D_TargetClear(target, clr_bg);
    C2D_SceneBegin(target);
    if (bg_top.valid)
        C2D_DrawImageAt(bg_top.img, 0, 0, 0.5f, NULL, 1.0f, 1.0f);

    if (!audio_ok) {
        draw_error_screen("ERROR", "Couldn't start audio.",
                          "Run DSP1 once to dump sdmc:/3ds/dspfirm.cdc");
        return;
    }
    if (lib.count == 0) {
        draw_error_screen("NO DATA", "No MP3 files in " MUSIC_DIR,
                          "Copy some music to " MUSIC_DIR "/ on the SD card.");
        return;
    }
    if (begin_unfold(MARGIN, 26, TOP_W - 2 * MARGIN, 0)) {
        draw_header("PLAYBACK", MARGIN, 18, TOP_W - 2 * MARGIN);
        draw_text("v" APP_VERSION, TOP_W - MARGIN - 13, 19, 0.4f, clr_dim, 60, C2D_AlignRight);
        float ver_w;
        C2D_TextGetDimensions(cached_text("v" APP_VERSION, 0.4f, 60), 0.4f, 0.4f, &ver_w, NULL);
        draw_battery(TOP_W - MARGIN - 13 - ver_w - 10, 21);
        if ((frame_count / 40) % 2 == 0)
            C2D_DrawRectSolid(TOP_W - MARGIN - 7, 21, 0.5f, 7, 7, clr_dim);
        end_unfold();
    }

    if (begin_unfold(ART_X - 4, ART_Y + ART_SIZE / 2, ART_SIZE + 8, 1)) {
        draw_panel(ART_X - 4, ART_Y - 4, ART_SIZE + 8, ART_SIZE + 8, clr_panel);
        draw_frame(ART_X - 1, ART_Y - 1, ART_SIZE + 2, ART_SIZE + 2, clr_dim);
        if (art.valid) {
            float x = ART_X + (ART_SIZE - art.sub.width) / 2.0f;
            float y = ART_Y + (ART_SIZE - art.sub.height) / 2.0f;
            draw_art(x, y);
        } else {
            draw_text(art_loading ? "LOADING" : "NO IMAGE", ART_X + ART_SIZE / 2, ART_Y + ART_SIZE / 2 - 8, 0.5f,
                      clr_dim, ART_SIZE, C2D_AlignCenter);
        }
        end_unfold();
    }

    if (begin_unfold_at(mode_frame, FIELD_X, ART_Y + ART_SIZE / 2, FIELD_W, 2)) {
        float y = ART_Y - 2;
        draw_string("TITLE", FIELD_X, y, 0.4f, clr_dim, FIELD_W);
        if (draw_typed(cur_title, FIELD_X, y + 11, 0.6f, clr_text, FIELD_W, type_frame))
            C2D_DrawText(&title_text, C2D_WithColor, FIELD_X, y + 11, 0.5f, 0.6f, 0.6f, clr_text);
        y += FIELD_STEP;
        if (viz_on)
            draw_viz_fields(y);
        else
            draw_track_fields(y);
        end_unfold();
    }

    draw_glitch_bands(TOP_W, 0x1234);
}

static int button_w(int i)
{
    return i == BTN_VIZ || i == BTN_THEME ? BTN_SMALL_W : BTN_W;
}

static int button_x(int i)
{
    int x = BTN_X0;
    for (int j = 0; j < i; j++)
        x += button_w(j) + BTN_GAP;
    return x;
}

static int button_at(int px, int py)
{
    if (py < BTN_Y || py >= BTN_Y + BTN_H)
        return -1;
    for (int i = 0; i < BTN_COUNT; i++) {
        int x = button_x(i);
        if (px >= x && px < x + button_w(i))
            return i;
    }
    return -1;
}

static void draw_button_icon(int id, float cx, float cy, u32 c)
{
    switch (id) {
    case BTN_PREV:
        C2D_DrawRectSolid(cx - 9, cy - 6, 0.5f, 2, 12, c);
        draw_tri(cx - 7, cy, cx, cy - 6, cx, cy + 6, c);
        draw_tri(cx, cy, cx + 7, cy - 6, cx + 7, cy + 6, c);
        break;
    case BTN_NEXT:
        C2D_DrawRectSolid(cx + 7, cy - 6, 0.5f, 2, 12, c);
        draw_tri(cx + 7, cy, cx, cy - 6, cx, cy + 6, c);
        draw_tri(cx, cy, cx - 7, cy - 6, cx - 7, cy + 6, c);
        break;
    case BTN_PLAY:
        if (play_ok && !audio_is_paused()) {
            C2D_DrawRectSolid(cx - 6, cy - 7, 0.5f, 4, 14, c);
            C2D_DrawRectSolid(cx + 2, cy - 7, 0.5f, 4, 14, c);
        } else {
            draw_tri(cx - 5, cy - 7, cx - 5, cy + 7, cx + 7, cy, c);
        }
        break;
    case BTN_VIZ:
        for (int b = 0; b < 3; b++) {
            static const int heights[3] = { 2, 4, 3 };
            for (int s = 0; s < heights[b]; s++)
                C2D_DrawRectSolid(cx - 6 + b * 5, cy + 5 - s * 3 - 2, 0.5f, 3, 2, c);
        }
        break;
    case BTN_THEME:
        draw_frame(cx - 6, cy - 6, 12, 12, c);
        C2D_DrawRectSolid(cx - 6, cy - 6, 0.5f, 6, 12, c);
        break;
    }
}

static void draw_bottom(C3D_RenderTarget *target)
{
    C2D_TargetClear(target, clr_bg);
    C2D_SceneBegin(target);
    if (bg_bottom.valid)
        C2D_DrawImageAt(bg_bottom.img, 0, 0, 0.5f, NULL, 1.0f, 1.0f);

    if (!audio_ok || lib.count == 0)
        return;

    if (scroll > lib.count - LIST_ROWS)
        scroll = lib.count - LIST_ROWS;
    if (scroll < 0)
        scroll = 0;

    if (begin_unfold(LIST_X, LIST_Y + LIST_ROWS * ROW_H / 2, BOTTOM_W - 2 * LIST_X, 1)) {
        for (int row = 0; row < LIST_ROWS && scroll + row < lib.count; row++) {
            int i = scroll + row;
            float y = LIST_Y + row * ROW_H;
            bool pressed = touch.mode == TOUCH_LIST && !touch.moved &&
                           (touch.start.py - LIST_Y) / ROW_H == row;

            u32 fg = clr_text;
            if (i == cursor || pressed) {
                C2D_DrawRectSolid(LIST_X, y + 1, 0.5f, LIST_W, ROW_H - 2, clr_text);
                fg = clr_bg;
            } else {
                C2D_DrawRectSolid(LIST_X, y + 1, 0.5f, LIST_W, ROW_H - 2, clr_panel);
            }

            if (i == current)
                draw_tri(LIST_X + 5, y + 4, LIST_X + 5, y + ROW_H - 4, LIST_X + 11, y + ROW_H / 2.0f, fg);
            else
                C2D_DrawRectSolid(LIST_X + 6, y + ROW_H / 2.0f - 2, 0.5f, 4, 4, fg);

            char name[256];
            display_name(lib.paths[i], name, sizeof(name));
            draw_string(name, LIST_X + 17, y + 1, 0.45f, fg, LIST_W - 21);
        }

        if (lib.count > LIST_ROWS) {
            float track_h = LIST_ROWS * ROW_H;
            float thumb_h = track_h * LIST_ROWS / lib.count;
            float thumb_y = LIST_Y + (track_h - thumb_h) * scroll / (lib.count - LIST_ROWS);
            float x = BOTTOM_W - LIST_X - 2;
            C2D_DrawRectSolid(x, LIST_Y, 0.5f, 1, track_h, clr_faint);
            C2D_DrawRectSolid(x - 1, thumb_y, 0.5f, 3, thumb_h, clr_text);
        }
        end_unfold();
    }

    if (begin_unfold(SEEK_X0 - 46, SEEK_Y, SEEK_X1 - SEEK_X0 + 92, 2)) {
        float len = play_ok ? audio_duration() : 0.0f;
        float pos = play_ok ? audio_position() : 0.0f;
        bool seeking = touch.mode == TOUCH_SEEK;
        if (seeking)
            pos = seek_frac(touch.last.px) * len;
        char t_pos[16], t_len[16];
        format_time(t_pos, sizeof(t_pos), pos);
        format_time(t_len, sizeof(t_len), len);
        draw_text(t_pos, SEEK_X0 - 10, SEEK_Y - 8, 0.45f, clr_text, 46, C2D_AlignRight);
        draw_string(t_len, SEEK_X1 + 10, SEEK_Y - 8, 0.45f, clr_text, 46);
        draw_rail(SEEK_X0, SEEK_X1, SEEK_Y, len > 0.0f ? pos / len : 0.0f, seeking);
        end_unfold();
    }

    if (begin_unfold(BTN_X0, BTN_Y + BTN_H / 2, BTN_ROW_W, 3)) {
        for (int i = 0; i < BTN_COUNT; i++) {
            float x = button_x(i);
            float w = button_w(i);
            bool pressed = touch.mode == TOUCH_BUTTON && touch.button == i &&
                           button_at(touch.last.px, touch.last.py) == i;
            u32 fg = pressed ? clr_bg : i == BTN_VIZ && !viz_on ? clr_dim : clr_text;

            if (pressed) {
                C2D_DrawRectSolid(x + 2, BTN_Y + 2, 0.5f, w, BTN_H, clr_text);
            } else {
                draw_panel(x, BTN_Y, w, BTN_H, clr_panel);
                draw_frame(x, BTN_Y, w, BTN_H, i == BTN_VIZ && viz_on ? clr_text : clr_dim);
            }
            float off = pressed ? 2 : 0;
            draw_button_icon(i, x + off + w / 2.0f, BTN_Y + off + BTN_H / 2.0f, fg);
        }
        end_unfold();
    }

    draw_glitch_bands(BOTTOM_W, 0x5678);
}

typedef struct {
    u32 bg, text, dim, faint, panel, shadow, error;
    bool recolor;
} Theme;

static const Theme themes[] = {
    {
        .bg = 0xd5d1bcff, .text = 0x4e4b42ff, .dim = 0x8a8676ff, .faint = 0xb4af9aff,
        .panel = 0xcdc8b0e0, .shadow = 0x4e4b4240, .error = 0x9c453aff,
    },
    {
        .bg = 0x161614ff, .text = 0xe6e2d4ff, .dim = 0x8f8b7eff, .faint = 0x45433dff,
        .panel = 0x22221fe0, .shadow = 0x00000080, .error = 0xe05a3cff,
        .recolor = true,
    },
    {
        .bg = 0x232820ff, .text = 0xc6ec70ff, .dim = 0x8faa61ff, .faint = 0x464d38ff,
        .panel = 0x2d3427e0, .shadow = 0x00000080, .error = 0xe8a33cff,
        .recolor = true,
    },
    {
        .bg = 0x1e1629ff, .text = 0xd9c2f0ff, .dim = 0x9b7fbfff, .faint = 0x4a3863ff,
        .panel = 0x2a1f3ae0, .shadow = 0x00000080, .error = 0xff7aa8ff,
        .recolor = true,
    },
};

#define THEME_COUNT ((int)(sizeof(themes) / sizeof(themes[0])))

static int theme;

static u32 hex_color(u32 rgba)
{
    return C2D_Color32(rgba >> 24, (rgba >> 16) & 0xff, (rgba >> 8) & 0xff, rgba & 0xff);
}

static void to_rgb(u8 *out, u32 rgba)
{
    out[0] = rgba >> 24;
    out[1] = rgba >> 16;
    out[2] = rgba >> 8;
}

static void apply_theme(int index)
{
    theme = index;
    const Theme *t = &themes[index];
    clr_bg = hex_color(t->bg);
    clr_text = hex_color(t->text);
    clr_dim = hex_color(t->dim);
    clr_faint = hex_color(t->faint);
    clr_panel = hex_color(t->panel);
    clr_shadow = hex_color(t->shadow);
    clr_error = hex_color(t->error);

    BgRemap remap;
    to_rgb(remap.paper, t->bg);
    to_rgb(remap.ink, t->text);
    const BgRemap *r = t->recolor ? &remap : NULL;
    art_free(&bg_top);
    art_free(&bg_bottom);
    art_load_background(&bg_top, bg_top_png, bg_top_png_size, r);
    art_load_background(&bg_bottom, bg_bottom_png, bg_bottom_png_size, r);
}

static void save_settings(void)
{
    Settings s = { .theme = theme, .viz_on = viz_on, .viz_mode = viz_mode };
    settings_save(&s);
}

static void load_settings(void)
{
    Settings s = { 0 };
    settings_load(&s);
    apply_theme(s.theme >= 0 && s.theme < THEME_COUNT ? s.theme : 0);
    viz_mode = s.viz_mode >= 0 && s.viz_mode < VIZ_MODES ? s.viz_mode : 0;
    viz_on = s.viz_on;
}

static void toggle_viz(void)
{
    viz_on = !viz_on;
    viz_switch();
    save_settings();
}

static void next_viz_mode(void)
{
    if (viz_on)
        viz_mode = (viz_mode + 1) % VIZ_MODES;
    viz_on = true;
    viz_switch();
    save_settings();
}

static void next_theme(void)
{
    apply_theme((theme + 1) % THEME_COUNT);
    glitch_frame = frame_count;
    save_settings();
}

static void touch_begin(touchPosition p)
{
    touch.start = touch.last = p;
    touch.moved = false;
    touch.mode = TOUCH_NONE;

    if (p.py >= SEEK_TOP && p.py < SEEK_BOTTOM) {
        if (play_ok && audio_duration() > 0.0f)
            touch.mode = TOUCH_SEEK;
    } else if (p.py >= LIST_Y && p.py < LIST_Y + LIST_ROWS * ROW_H) {
        touch.mode = TOUCH_LIST;
        touch.scroll_start = scroll;
    } else if ((touch.button = button_at(p.px, p.py)) >= 0) {
        touch.mode = TOUCH_BUTTON;
    }
}

static void touch_drag(touchPosition p)
{
    touch.last = p;
    if (touch.mode != TOUCH_LIST)
        return;

    int dy = touch.start.py - p.py;
    if (abs(dy) > DRAG_THRESHOLD)
        touch.moved = true;
    if (touch.moved)
        scroll = touch.scroll_start + dy / ROW_H;
}

static void touch_end(void)
{
    touchPosition p = touch.last;

    switch (touch.mode) {
    case TOUCH_SEEK:
        audio_seek(seek_frac(p.px) * audio_duration());
        break;
    case TOUCH_LIST:
        if (!touch.moved) {
            int i = scroll + (touch.start.py - LIST_Y) / ROW_H;
            if (i < lib.count) {
                if (i == current && play_ok)
                    audio_toggle_pause();
                else
                    pending = i;
            }
        }
        break;
    case TOUCH_BUTTON:
        if (button_at(p.px, p.py) != touch.button)
            break;
        switch (touch.button) {
        case BTN_PREV: action_prev(); break;
        case BTN_PLAY: if (play_ok) audio_toggle_pause(); else pending = current; break;
        case BTN_NEXT: action_next(); break;
        case BTN_VIZ: toggle_viz(); break;
        case BTN_THEME: next_theme(); break;
        }
        break;
    }
    touch.mode = TOUCH_NONE;
}

int main(void)
{
    gfxInitDefault();
    osSetSpeedupEnable(true);

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    info_buf = C2D_TextBufNew(1024);
    scratch_buf = C2D_TextBufNew(512);

    load_settings();
    battery_init();

    hidSetRepeatParameters(15, 4);

    audio_ok = audio_init();
    loader_init(ART_SIZE);
    library_scan(&lib, MUSIC_DIR);

    pending = (audio_ok && lib.count > 0) ? 0 : -1;
    bool start_paused = true;

    while (aptMainLoop()) {
        hidScanInput();
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();
        u32 up = hidKeysUp();
        u32 repeat = hidKeysDownRepeat();

        if (down & KEY_START)
            break;

        if (can_control() && intro_done()) {
            if (down & (KEY_R | KEY_DRIGHT))
                action_next();
            if (down & (KEY_L | KEY_DLEFT))
                action_prev();
            if (down & KEY_Y)
                action_skip(-SKIP_SECONDS);
            if (down & KEY_X)
                action_skip(SKIP_SECONDS);
            if (down & KEY_B)
                toggle_viz();
            if (down & KEY_SELECT)
                next_viz_mode();

            if (repeat & KEY_DUP) {
                cursor = (cursor - 1 + lib.count) % lib.count;
                scroll_to_cursor();
            }
            if (repeat & KEY_DDOWN) {
                cursor = (cursor + 1) % lib.count;
                scroll_to_cursor();
            }

            if (down & KEY_A) {
                if (cursor == current && play_ok)
                    audio_toggle_pause();
                else
                    pending = cursor;
            }

            touchPosition p;
            if (down & KEY_TOUCH) {
                touchRead(&p);
                touch_begin(p);
            } else if (held & KEY_TOUCH) {
                touchRead(&p);
                touch_drag(p);
            }
            if (up & KEY_TOUCH)
                touch_end();

            if (pending < 0 && play_ok && audio_is_finished())
                action_next();
        }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        if (pending >= 0) {
            load_track(pending);
            pending = -1;
            if (start_paused && play_ok)
                audio_toggle_pause();
            start_paused = false;
        }
        receive_track_info();
        update_battery();
        if (viz_on)
            viz_update();

        draw_top(top);
        draw_bottom(bottom);
        C3D_FrameEnd(0);
        frame_count++;
    }

    loader_exit();
    battery_exit();
    if (audio_ok)
        audio_exit();
    art_free(&art);
    art_free(&bg_top);
    art_free(&bg_bottom);
    library_free(&lib);

    C2D_TextBufDelete(scratch_buf);
    for (int i = 0; i < TEXT_CACHE_SIZE; i++)
        if (text_cache[i].buf)
            C2D_TextBufDelete(text_cache[i].buf);
    C2D_TextBufDelete(info_buf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
