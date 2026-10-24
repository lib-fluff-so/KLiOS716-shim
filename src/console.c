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

static void clear_last_line(void)
{
    clear_pixels(VISIBLE_OFFSET + (u64)(FB_HEIGHT - CELL_HEIGHT) * FB_STRIDE,
                 (u64)CELL_HEIGHT * FB_STRIDE, COLOR_BG);
}

static void scroll_one_line(void)
{
    const u64 words_per_row = FB_STRIDE / 8u;
    for (u32 y = 0; y < FB_HEIGHT - CELL_HEIGHT; y++) {
        volatile u64 *dst = (volatile u64 *)(FB_BASE + VISIBLE_OFFSET + (u64)y * FB_STRIDE);
        volatile u64 *src = (volatile u64 *)(FB_BASE + VISIBLE_OFFSET + (u64)(y + CELL_HEIGHT) * FB_STRIDE);
        for (u64 x = 0; x < words_per_row; x++) dst[x] = src[x];
    }
    clear_last_line();
    cursor_y = ROWS - 1u;
}

static void newline(void)
{
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= ROWS) scroll_one_line();
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
