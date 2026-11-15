#pragma once
void watchdog_enable(unsigned, bool);
bool watchdog_caused_reboot(void);
void watchdog_update(void);
void watchdog_reboot(unsigned, unsigned, unsigned);
