#pragma once
#define CYW43_WL_GPIO_LED_PIN 0
int cyw43_arch_init(void);
void cyw43_arch_gpio_put(unsigned, bool);
