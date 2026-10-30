#ifndef KLIOS716_TIMER_H
#define KLIOS716_TIMER_H

typedef unsigned long timer_u64;
typedef unsigned int timer_u32;

timer_u64 timer_ticks(void);
timer_u64 timer_frequency(void);
void timer_delay_us(timer_u64 us);
void timer_delay_ms(timer_u32 ms);

#endif
