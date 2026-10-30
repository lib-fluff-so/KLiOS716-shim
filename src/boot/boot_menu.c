#include "../console.h"
#include "../core/timer.h"
#include "../drivers/input/fts3519.h"
#include "boot_menu.h"

#define SCREEN_W 1080u
#define SCREEN_H 2436u

#define C_BG       0xFF0B1018u
#define C_ANDROID  0xFF246BCEu
#define C_IOS      0xFF247A4Bu
#define C_WHITE    0xFFFFFFFFu
#define C_MUTED    0xFFB9C5D5u

#define ANDROID_X  72u
#define ANDROID_Y  790u
#define ANDROID_W  936u
#define ANDROID_H  420u

#define IOS_X      72u
#define IOS_Y      1290u
#define IOS_W      936u
#define IOS_H      420u

static unsigned int text_width(const char *s, unsigned int scale)
{
    unsigned int n = 0;
    while (s[n])
        n++;
    return n * 8u * scale;
}

static void centered_text(unsigned int y, const char *s, unsigned int scale,
                          unsigned int fg, unsigned int bg)
{
    unsigned int width = text_width(s, scale);
    unsigned int x = width < SCREEN_W ? (SCREEN_W - width) / 2u : 0u;
    console_draw_text(x, y, s, scale, fg, bg);
}

static void draw_button(unsigned int x, unsigned int y, unsigned int w,
                        unsigned int h, unsigned int color,
                        const char *title, const char *subtitle,
                        unsigned int title_scale)
{
    console_fill_rect(x, y, w, h, color);
    console_fill_rect(x, y, w, 8u, C_WHITE);

    unsigned int title_w = text_width(title, title_scale);
    unsigned int title_x = x + (w - title_w) / 2u;
    console_draw_text(title_x, y + 116u, title, title_scale, C_WHITE, color);

    unsigned int sub_w = text_width(subtitle, 2u);
    unsigned int sub_x = x + (w - sub_w) / 2u;
    console_draw_text(sub_x, y + 260u, subtitle, 2u, C_WHITE, color);
}

static void draw_menu(void)
{
    console_fill_rect(0, 0, SCREEN_W, SCREEN_H, C_BG);
    centered_text(280u, "KL7 DUAL BOOT", 4u, C_WHITE, C_BG);
    centered_text(440u, "SELECT OPERATING SYSTEM", 2u, C_MUTED, C_BG);
    draw_button(ANDROID_X, ANDROID_Y, ANDROID_W, ANDROID_H,
                C_ANDROID, "ANDROID", "BOOT STOCK SYSTEM", 5u);
    draw_button(IOS_X, IOS_Y, IOS_W, IOS_H,
                C_IOS, "iOS", "BOOT IOS 16", 6u);
    centered_text(1900u, "TAP ONE OPTION TO CONTINUE", 2u, C_MUTED, C_BG);
}

static int hit(unsigned int x, unsigned int y,
               unsigned int rx, unsigned int ry,
               unsigned int rw, unsigned int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

int boot_menu_select(enum boot_choice *choice)
{
    if (!choice)
        return -1;

    int rc = fts3519_init();
    if (rc) {
        logf("BOOT MENU: FTS3519 init failed rc=%d",
             (unsigned long)(rc < 0 ? -rc : rc));
        return -2;
    }

    draw_menu();
    logf("BOOT MENU: waiting for touch selection");

    /* A held finger during boot must not accidentally select an OS. */
    int armed = 0;
    for (;;) {
        struct fts3519_point points[FTS3519_MAX_POINTS];
        int count = fts3519_read_points(points, FTS3519_MAX_POINTS);
        if (count < 0) {
            timer_delay_ms(50);
            continue;
        }

        int any_down = 0;
        for (int i = 0; i < count; i++) {
            if (!points[i].down)
                continue;
            any_down = 1;
            if (!armed)
                continue;

            if (hit(points[i].x, points[i].y,
                    ANDROID_X, ANDROID_Y, ANDROID_W, ANDROID_H)) {
                *choice = BOOT_CHOICE_ANDROID;
                logf("BOOT MENU: Android selected");
                return 0;
            }
            if (hit(points[i].x, points[i].y,
                    IOS_X, IOS_Y, IOS_W, IOS_H)) {
                *choice = BOOT_CHOICE_IOS;
                logf("BOOT MENU: iOS selected");
                return 0;
            }
        }

        if (!any_down)
            armed = 1;
        timer_delay_ms(20);
    }
}
