#ifndef AIROBOT_ULN2003_HW_H
#define AIROBOT_ULN2003_HW_H

#include <stdint.h>

void uln2003_hw_apply(uint8_t left_pattern, uint8_t right_pattern, void *context);
void uln2003_hw_off(void);

#endif
