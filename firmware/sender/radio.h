#pragma once
#include <stdbool.h>
void radio_gpio_init(void);
bool radio_init(void);
bool radio_healthy(void);
bool radio_send_on(void);
void radio_abort(void);
