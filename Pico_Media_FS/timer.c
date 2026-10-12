#include <stdint.h>

#define TIMER_BASE 0x40054000u
#define TIME_HR (*(volatile uint32_t *)(TIMER_BASE + 0x08))
#define TIME_LR (*(volatile uint32_t *)(TIMER_BASE + 0x0c))


uint64_t read_timer(void);
void delay_ms(uint64_t milliseconds);
void delay_us(uint64_t microseconds);

uint64_t read_timer(void)
{
    uint32_t high1, low, high2;
    do
    {
        high1 = TIME_HR;
        low = TIME_LR;
        high2 = TIME_HR;
    } while (high1 != high2);

    return ((uint64_t)high2 << 32) | low;
}

void delay_ms(uint64_t milliseconds)
{
    uint64_t start = read_timer();
    uint64_t target_us = milliseconds * 1000;

    while ((read_timer() - start) < target_us)
    {
        // wait;
    }
}

void delay_us(uint64_t microseconds)
{
    uint64_t start = read_timer();
    uint64_t target_us = microseconds;

    while ((read_timer() - start) < target_us)
    {
        // wait;
    }
}
