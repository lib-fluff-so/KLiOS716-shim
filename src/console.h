#ifndef IOS_KL7_CONSOLE_H
#define IOS_KL7_CONSOLE_H

void console_init(void);
void console_write(const char *text, unsigned long length);
void logf(const char *fmt, ...);
void console_crosshair(int x, int y);
void console_crosshairs(const int *xs, const int *ys, int n);
void console_crosshair_hide(void);

#endif
