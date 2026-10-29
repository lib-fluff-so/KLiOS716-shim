#include "console.h"
#include "font.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

#define FB_BASE       0xFDFFF000UL
#define FB_WIDTH      1080u
#define FB_HEIGHT     2436u
#define FB_STRIDE     4352u
#define VISIBLE_OFFSET 0UL
#define SCALE         2u
#define CELL_WIDTH    (8u * SCALE)
#define CELL_HEIGHT   (16u * SCALE)
#define COLS          (FB_WIDTH / CELL_WIDTH)
#define ROWS          (FB_HEIGHT / CELL_HEIGHT)
#define COLOR_BG      0xFF101018u
#define COLOR_FG      0xFFF0F0F0u

static u32 cursor_x;
static u32 cursor_y;
static u32 console_ready;

static volatile u32 *pixel_ptr(u32 x, u32 y)
{
    return (volatile u32 *)(FB_BASE + VISIBLE_OFFSET + (u64)y * FB_STRIDE + (u64)x * 4u);
}

static void clear_pixels(u64 offset, u64 bytes, u32 color)
{
    volatile u64 *p = (volatile u64 *)(FB_BASE + offset);
    u64 pair = ((u64)color << 32) | color;
    for (u64 i = 0; i < bytes / 8u; i++) p[i] = pair;
}

static void draw_char(u32 x, u32 y, char ch)
{
    if ((u8)ch < 32u || (u8)ch > 126u) ch = '?';
    const u8 *glyph = font8x16[(u8)ch - 32u];

    for (u32 gy = 0; gy < 16u; gy++) {
        for (u32 sy = 0; sy < SCALE; sy++) {
            volatile u32 *dst = pixel_ptr(x, y + gy * SCALE + sy);
            for (u32 gx = 0; gx < 8u; gx++) {
                u32 color = (glyph[gy] & (0x80u >> gx)) ? COLOR_FG : COLOR_BG;
                for (u32 sx = 0; sx < SCALE; sx++) dst[gx * SCALE + sx] = color;
            }
        }
    }
}

static void clear_row(u32 row)
{
    clear_pixels(VISIBLE_OFFSET + (u64)row * CELL_HEIGHT * FB_STRIDE,
                 (u64)CELL_HEIGHT * FB_STRIDE, COLOR_BG);
}

static void newline(void)
{
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= ROWS) cursor_y = 0;
    clear_row(cursor_y);
}

static void put_char(char ch)
{
    if (ch == '\r') {
        cursor_x = 0;
        return;
    }
    if (ch == '\n') {
        newline();
        return;
    }
    if (ch == '\t') {
        for (u32 i = 0; i < 4u; i++) put_char(' ');
        return;
    }
    if ((u8)ch < 32u || (u8)ch > 126u) ch = '?';
    draw_char(cursor_x * CELL_WIDTH, cursor_y * CELL_HEIGHT, ch);
    cursor_x++;
    if (cursor_x >= COLS) newline();
}

void console_init(void)
{
    clear_pixels(VISIBLE_OFFSET, (u64)FB_HEIGHT * FB_STRIDE, COLOR_BG);
    cursor_x = 0;
    cursor_y = 0;
    console_ready = 1;
}

void console_write(const char *text, unsigned long length)
{
    if (!console_ready || !text) return;
    for (unsigned long i = 0; i < length; i++) put_char(text[i]);
}

/* On-screen visual diagnostic.  The panel scans the visible framebuffer page
 * at FB_BASE + 0 (the same offset the working main console uses).  Draw the
 * card there, full screen, and show the latest 64x64 GPU readback in the
 * middle.  The card is deliberately drawn without touching the console
 * cursor, so it can be the last thing on screen before a halt or reset. */
#define VIS_BG         0xFF101018u
#define VIS_WHITE      0xFFF0F0F0u
#define VIS_BLACK      0xFF000000u
#define VIS_RED        0xFFFF3030u
#define VIS_BLUE       0xFF3030FFu
#define VIS_GREEN      0xFF30FF30u
#define VIS_YELLOW     0xFFFFFF30u

static volatile u32 *visual_pixel(u64 offset, u32 x, u32 y)
{
    return (volatile u32 *)(FB_BASE + offset + (u64)y * FB_STRIDE +
                            (u64)x * 4u);
}

static void visual_fill(u64 offset, u32 x, u32 y, u32 w, u32 h, u32 color)
{
    if (x >= FB_WIDTH || y >= FB_HEIGHT) return;
    if (w > FB_WIDTH - x) w = FB_WIDTH - x;
    if (h > FB_HEIGHT - y) h = FB_HEIGHT - y;
    for (u32 yy = 0; yy < h; yy++) {
        volatile u32 *dst = visual_pixel(offset, x, y + yy);
        for (u32 xx = 0; xx < w; xx++) dst[xx] = color;
    }
}

static void visual_glyph(u64 offset, u32 x, u32 y, char ch, u32 scale,
                         u32 fg, u32 bg)
{
    if ((u8)ch < 32u || (u8)ch > 126u) ch = '?';
    const u8 *glyph = font8x16[(u8)ch - 32u];

    for (u32 gy = 0; gy < 16u; gy++) {
        for (u32 sy = 0; sy < scale; sy++) {
            u32 py = y + gy * scale + sy;
            if (py >= FB_HEIGHT) return;
            volatile u32 *dst = visual_pixel(offset, x, py);
            for (u32 gx = 0; gx < 8u; gx++) {
                u32 color = (glyph[gy] & (0x80u >> gx)) ? fg : bg;
                for (u32 sx = 0; sx < scale; sx++) {
                    u32 px = x + gx * scale + sx;
                    if (px < FB_WIDTH) dst[gx * scale + sx] = color;
                }
            }
        }
    }
}

static void visual_text(u64 offset, u32 x, u32 y, const char *s, u32 scale,
                        u32 fg, u32 bg)
{
    for (; *s; s++, x += 8u * scale) {
        if (x >= FB_WIDTH) break;
        visual_glyph(offset, x, y, *s, scale, fg, bg);
    }
}

static u32 visual_gpu_color(u32 value)
{
    if (value == 0xff0000ffu) return VIS_RED;      /* clear colour */
    if (value == 0x11223344u) return VIS_BLUE;     /* untouched sentinel */
    if (value == 0u) return VIS_BLACK;
    if (value & 0x80000000u) return 0xFF707070u;   /* unique sentinel */
    return VIS_GREEN;                              /* unexpected value */
}

static void visual_card(u64 offset, char page, const u32 *gpu,
                        u32 gpu_width, u32 gpu_height, u32 gpu_valid)
{
    static const u32 bars[8] = {
        0xFFF0F0F0u, 0xFFFFFF30u, 0xFF30FFFFu, 0xFF30FF30u,
        0xFFFF30FFu, 0xFFFF3030u, 0xFF3030FFu, 0xFF101010u,
    };
    const char page_name[] = { page, 0 };
    u32 image_scale = 16u;
    u32 image_x = 28u;
    u32 image_y = 540u;

    visual_fill(offset, 0, 0, FB_WIDTH, FB_HEIGHT, VIS_BG);

    /* Eight known bars: this proves the panel path, colour order and stride
     * without depending on the GPU. */
    for (u32 i = 0; i < 8u; i++)
        visual_fill(offset, i * 135u, 0, 135u, 96u, bars[i]);

    visual_text(offset, 24, 120, "KL7 PANEL VISUAL", 4, VIS_WHITE, VIS_BG);
    visual_text(offset, 24, 220,
                page == 'A' ? "PAGE: FB + 0 (visible)" : "PAGE: FB + 0 (visible)",
                3, VIS_WHITE, VIS_BG);

    visual_fill(offset, 24, 300, 144, 176, VIS_BLACK);
    visual_text(offset, 48, 320, page_name, 8, VIS_YELLOW, VIS_BLACK);
    visual_text(offset, 200, 340, "GPU FRAG 64X64", 4, VIS_GREEN, VIS_BG);
    visual_text(offset, 200, 420,
                gpu_valid ? "GPU READBACK" : "GPU NOT READY",
                3, VIS_WHITE, VIS_BG);

    /* Image frame and the 64x64 result, enlarged 16x. */
    visual_fill(offset, image_x - 4, image_y - 4,
                gpu_width * image_scale + 8u,
                gpu_height * image_scale + 8u, VIS_WHITE);
    if (gpu_valid && gpu && gpu_width && gpu_height) {
        for (u32 y = 0; y < gpu_height; y++) {
            for (u32 x = 0; x < gpu_width; x++) {
                u32 color = visual_gpu_color(gpu[y * gpu_width + x]);
                visual_fill(offset, image_x + x * image_scale,
                            image_y + y * image_scale,
                            image_scale, image_scale, color);
            }
        }
    } else {
        visual_fill(offset, image_x, image_y,
                    gpu_width * image_scale, gpu_height * image_scale,
                    VIS_BLACK);
        visual_text(offset, 210, 980, "NO GPU FRAME", 5, VIS_RED, VIS_BLACK);
    }

    visual_text(offset, 24, 1620, "RED = GPU CLEAR", 3, VIS_RED, VIS_BG);
    visual_text(offset, 24, 1680, "BLUE = UNTOUCHED", 3, VIS_BLUE, VIS_BG);
    visual_text(offset, 24, 1740, "WANT: ALL RED", 3, VIS_WHITE, VIS_BG);

    visual_fill(offset, 20, 1856, 208, 208, VIS_WHITE);
    visual_fill(offset, 24, 1860, 200, 200, VIS_RED);
    visual_text(offset, 250, 1880, "CPU RED", 4, VIS_RED, VIS_BG);
    visual_text(offset, 250, 1960, "PANEL PATH OK", 3, VIS_WHITE, VIS_BG);
    visual_text(offset, 24, 2120,
                "IF THIS CARD IS VISIBLE, THE PANEL WORKS",
                3, VIS_WHITE, VIS_BG);
}

void console_visual_result(const unsigned int *gpu_pixels,
                           unsigned int gpu_width, unsigned int gpu_height,
                           unsigned int gpu_valid)
{
    /* Draw the card at the visible page (FB_BASE + 0, full screen).  Only one
     * page is scanned, so this is the page that actually shows on the panel. */
    visual_card(0, 'A', gpu_pixels, gpu_width, gpu_height, gpu_valid);
    __asm__ volatile("dsb sy" ::: "memory");
}


/* Touch crosshairs: full-width horizontal + full-height vertical lines.
 * The pixels under every old line are saved, so moving the fingers
 * restores the console text instead of painting a trail.  Save all
 * backgrounds before drawing any line: overlapping crosses then still
 * restore cleanly. */
#define CROSS_THICK 2u
#define CROSS_COLOR 0xFFFF3030u
#define CROSS_MAX 10

static u32 cross_saved_v[CROSS_MAX][CROSS_THICK][FB_HEIGHT];
static u32 cross_saved_h[CROSS_MAX][CROSS_THICK][FB_WIDTH];
static int cross_x[CROSS_MAX];
static int cross_y[CROSS_MAX];
static int cross_n;

void console_crosshair_hide(void)
{
    u32 t, i, k, x, y;

    for (k = 0; k < (u32)cross_n; k++) {
        x = (u32)cross_x[k];
        y = (u32)cross_y[k];
        for (t = 0; t < CROSS_THICK; t++)
            for (i = 0; i < FB_HEIGHT; i++)
                *pixel_ptr(x + t, i) = cross_saved_v[k][t][i];
        for (t = 0; t < CROSS_THICK; t++)
            for (i = 0; i < FB_WIDTH; i++)
                *pixel_ptr(i, y + t) = cross_saved_h[k][t][i];
    }

    cross_n = 0;
}

void console_crosshairs(const int *xs, const int *ys, int n)
{
    u32 t, i, k, x, y;

    console_crosshair_hide();
    if (!xs || !ys || n <= 0)
        return;
    if (n > CROSS_MAX)
        n = CROSS_MAX;

    /* Save every background first, then draw all lines. */
    for (k = 0; k < (u32)n; k++) {
        x = (u32)xs[k];
        y = (u32)ys[k];
        if (x > FB_WIDTH - CROSS_THICK)
            x = FB_WIDTH - CROSS_THICK;
        if (y > FB_HEIGHT - CROSS_THICK)
            y = FB_HEIGHT - CROSS_THICK;
        cross_x[k] = (int)x;
        cross_y[k] = (int)y;
        for (t = 0; t < CROSS_THICK; t++)
            for (i = 0; i < FB_HEIGHT; i++)
                cross_saved_v[k][t][i] = *pixel_ptr(x + t, i);
        for (t = 0; t < CROSS_THICK; t++)
            for (i = 0; i < FB_WIDTH; i++)
                cross_saved_h[k][t][i] = *pixel_ptr(i, y + t);
    }

    cross_n = n;

    for (k = 0; k < (u32)n; k++) {
        x = (u32)cross_x[k];
        y = (u32)cross_y[k];
        for (t = 0; t < CROSS_THICK; t++)
            for (i = 0; i < FB_HEIGHT; i++)
                *pixel_ptr(x + t, i) = CROSS_COLOR;
        for (t = 0; t < CROSS_THICK; t++)
            for (i = 0; i < FB_WIDTH; i++)
                *pixel_ptr(i, y + t) = CROSS_COLOR;
    }
}

void console_crosshair(int x, int y)
{
    console_crosshairs(&x, &y, 1);
}
