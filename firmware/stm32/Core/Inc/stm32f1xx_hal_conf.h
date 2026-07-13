#ifndef STM32F1XX_HAL_CONF_H
#define STM32F1XX_HAL_CONF_H

#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED

#ifndef HSE_VALUE
#define HSE_VALUE 8000000U
#endif
#ifndef HSE_STARTUP_TIMEOUT
#define HSE_STARTUP_TIMEOUT 100U
#endif
#ifndef HSI_VALUE
#define HSI_VALUE 8000000U
#endif
#ifndef LSI_VALUE
#define LSI_VALUE 40000U
#endif
#ifndef LSE_VALUE
#define LSE_VALUE 32768U
#endif
#ifndef LSE_STARTUP_TIMEOUT
#define LSE_STARTUP_TIMEOUT 5000U
#endif
#ifndef VDD_VALUE
#define VDD_VALUE 3300U
#endif
#ifndef TICK_INT_PRIORITY
#define TICK_INT_PRIORITY 0x0FU
#endif

#define USE_RTOS 0U
#define PREFETCH_ENABLE 1U

#include "stm32f1xx_hal_rcc.h"
#include "stm32f1xx_hal_gpio.h"
#include "stm32f1xx_hal_dma.h"
#include "stm32f1xx_hal_cortex.h"
#include "stm32f1xx_hal_flash.h"
#include "stm32f1xx_hal_pwr.h"
#include "stm32f1xx_hal_spi.h"
#include "stm32f1xx_hal_tim.h"

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line);
#define assert_param(expr) ((expr) ? (void)0U : assert_failed((uint8_t *)__FILE__, __LINE__))
#else
#define assert_param(expr) ((void)0U)
#endif

#endif
