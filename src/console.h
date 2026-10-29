#ifndef IOS_KL7_CONSOLE_H
#define IOS_KL7_CONSOLE_H

void console_init(void);
void console_write(const char *text, unsigned long length);
void console_visual_result(const unsigned int *gpu_pixels,
                           unsigned int gpu_width, unsigned int gpu_height,
                           unsigned int gpu_valid);
void console_gpu_mode_result(unsigned int mode, unsigned int total,
                             unsigned int config, unsigned long passmask,
                             unsigned long failmask, unsigned int error);
void logf(const char *fmt, ...);
void console_crosshair(int x, int y);
void console_crosshairs(const int *xs, const int *ys, int n);
void console_crosshair_hide(void);

#endif
