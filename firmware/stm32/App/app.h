#ifndef AIROBOT_APP_H
#define AIROBOT_APP_H

#include <stdbool.h>

bool app_init(void);
void app_process(void);
void app_timer_1ms_isr(void);

#endif
