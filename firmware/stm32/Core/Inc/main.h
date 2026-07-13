#ifndef AIROBOT_MAIN_H
#define AIROBOT_MAIN_H

#include "stm32f1xx_hal.h"

extern SPI_HandleTypeDef hspi1;
extern SPI_HandleTypeDef hspi2;
extern TIM_HandleTypeDef htim2;
extern DMA_HandleTypeDef hdma_spi1_rx;
extern DMA_HandleTypeDef hdma_spi1_tx;

void Error_Handler(void);

#endif
