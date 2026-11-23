#pragma once
#include "../gateway/sender.h"
bool network_init(void);
void network_poll(gw_sender *sender,uint64_t now_us);
bool network_connected(void);
