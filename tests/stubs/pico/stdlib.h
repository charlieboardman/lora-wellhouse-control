#pragma once
#include <stdint.h>
#define GPIO_IN false
uint64_t time_us_64(void);
void gpio_init(unsigned);
void gpio_set_dir(unsigned, bool);
void gpio_pull_up(unsigned);
bool gpio_get(unsigned);
void stdio_init_all(void);
void sleep_ms(unsigned);
void tight_loop_contents(void);
