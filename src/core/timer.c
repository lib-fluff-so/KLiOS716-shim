#include "timer.h"

timer_u64 timer_ticks(void)
{
    timer_u64 value;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

timer_u64 timer_frequency(void)
{
    timer_u64 value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value ? value : 19200000u;
}

void timer_delay_us(timer_u64 us)
{
    timer_u64 frequency = timer_frequency();
    timer_u64 start = timer_ticks();
    timer_u64 delta = (us * frequency) / 1000000u;
    while (timer_ticks() - start < delta)
        __asm__ volatile("yield");
}

void timer_delay_ms(timer_u32 ms)
{
    timer_delay_us((timer_u64)ms * 1000u);
}
