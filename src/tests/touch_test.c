#include "../console.h"
#include "../core/timer.h"
#include "../drivers/input/fts3519.h"
#include "touch_test.h"

void touch_test(void)
{
    int rc = fts3519_init();
    if (rc) {
        logf("TOUCH TEST: FTS3519 init failed rc=%d",
             (unsigned long)(rc < 0 ? -rc : rc));
        return;
    }

    timer_u64 stop = timer_ticks() + 15u * timer_frequency();
    while (timer_ticks() < stop) {
        struct fts3519_point points[FTS3519_MAX_POINTS];
        int count = fts3519_read_points(points, FTS3519_MAX_POINTS);

        if (count < 0) {
            logf("TOUCH TEST: report read failed rc=%d",
                 (unsigned long)(-count));
            timer_delay_ms(100);
            continue;
        }
        for (int i = 0; i < count; i++) {
            if (points[i].down)
                logf("TOUCH TEST: id=%d x=%d y=%d",
                     (unsigned long)points[i].id,
                     (unsigned long)points[i].x,
                     (unsigned long)points[i].y);
        }
        timer_delay_ms(20);
    }
    logf("TOUCH TEST: complete");
}
