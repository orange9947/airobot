#include "main.h"

#include "app.h"
#include "board.h"

int main(void) {
    HAL_Init();
    if (!board_clock_config()) {
        Error_Handler();
    }
    board_gpio_init();
    if (!board_peripherals_init()) {
        Error_Handler();
    }
    if (!app_init()) {
        Error_Handler();
    }
    if (HAL_TIM_Base_Start_IT(&htim2) != HAL_OK) {
        Error_Handler();
    }

    while (1) {
        app_process();
        __WFI();
    }
}

void Error_Handler(void) {
    board_motors_off();
    __disable_irq();
    while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {
    (void)file;
    (void)line;
    Error_Handler();
}
#endif
