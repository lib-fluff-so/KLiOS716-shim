#ifndef IOS_KL7_CONSOLE_H
#define IOS_KL7_CONSOLE_H

void logf(const char *fmt, ...);
void console_init(void);
void console_write(const char *text, unsigned long length);
void console_fill_rect(unsigned int x, unsigned int y, unsigned int width, unsigned int height, unsigned int color);
void console_draw_text(unsigned int x, unsigned int y, const char *text, unsigned int scale, unsigned int foreground, unsigned int background);

#endif
