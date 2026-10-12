#ifndef Timer_H
#define Timer_H

#include <stdint.h>

uint64_t read_timer(void);
void delay_ms(uint64_t milliseconds);
void delay_us(uint64_t microseconds);

#endif