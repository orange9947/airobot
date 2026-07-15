#include "board.h"

#include "board_pins.h"
#include "main.h"
#include "session_epoch.h"

SPI_HandleTypeDef hspi1;
SPI_HandleTypeDef hspi2;
TIM_HandleTypeDef htim2;
DMA_HandleTypeDef hdma_spi1_rx;
DMA_HandleTypeDef hdma_spi1_tx;

static __IO uint32_t *backup_register(uint8_t index) {
    switch (index) {
        case 0u:
            return &BKP->DR1;
        case 1u:
            return &BKP->DR2;
        case 2u:
            return &BKP->DR3;
        case 3u:
            return &BKP->DR4;
        case 4u:
            return &BKP->DR5;
        case 5u:
            return &BKP->DR6;
        case 6u:
            return &BKP->DR7;
        case 7u:
            return &BKP->DR8;
        default:
            return &BKP->DR1;
    }
}

static session_boot_record_t backup_read_record(uint8_t slot) {
    uint8_t base = slot == SESSION_BOOT_SLOT_B ? 4u : 0u;
    session_boot_record_t record;

    record.magic = (uint16_t)*backup_register(base);
    record.counter_low = (uint16_t)*backup_register((uint8_t)(base + 1u));
    record.counter_high = (uint16_t)*backup_register((uint8_t)(base + 2u));
    record.check = (uint16_t)*backup_register((uint8_t)(base + 3u));
    return record;
}

static void backup_write_record(uint8_t slot,
                                const session_boot_record_t *record) {
    uint8_t base = slot == SESSION_BOOT_SLOT_B ? 4u : 0u;

    *backup_register(base) = 0u;
    *backup_register((uint8_t)(base + 1u)) = record->counter_low;
    *backup_register((uint8_t)(base + 2u)) = record->counter_high;
    *backup_register((uint8_t)(base + 3u)) = record->check;
    *backup_register(base) = record->magic;
}

static void gpio_output(GPIO_TypeDef *port, uint16_t pins, GPIO_PinState state,
                        uint32_t mode, uint32_t speed) {
    GPIO_InitTypeDef config = {0};
    HAL_GPIO_WritePin(port, pins, state);
    config.Pin = pins;
    config.Mode = mode;
    config.Speed = speed;
    HAL_GPIO_Init(port, &config);
}

bool board_clock_config(void) {
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clock = {0};

    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    oscillator.HSEState = RCC_HSE_ON;
    oscillator.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    oscillator.PLL.PLLMUL = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&oscillator) == HAL_OK) {
        clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 |
                          RCC_CLOCKTYPE_PCLK2;
        clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
        clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
        clock.APB1CLKDivider = RCC_HCLK_DIV2;
        clock.APB2CLKDivider = RCC_HCLK_DIV1;
        return HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_2) == HAL_OK;
    }

    oscillator = (RCC_OscInitTypeDef){0};
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    oscillator.HSIState = RCC_HSI_ON;
    oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
    oscillator.PLL.PLLMUL = RCC_PLL_MUL16;
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) {
        return false;
    }
    clock.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 |
                      RCC_CLOCKTYPE_PCLK2;
    clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clock.APB1CLKDivider = RCC_HCLK_DIV2;
    clock.APB2CLKDivider = RCC_HCLK_DIV1;
    return HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_2) == HAL_OK;
}

void board_motors_off(void) {
    HAL_GPIO_WritePin(MOTOR_LEFT_PORT, MOTOR_LEFT_PINS, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(MOTOR_RIGHT_PORT, MOTOR_RIGHT_PINS, GPIO_PIN_RESET);
}

void board_gpio_init(void) {
    GPIO_InitTypeDef config = {0};

    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_AFIO_REMAP_SWJ_NOJTAG();

    gpio_output(MOTOR_LEFT_PORT, MOTOR_LEFT_PINS, GPIO_PIN_RESET,
                GPIO_MODE_OUTPUT_PP, GPIO_SPEED_FREQ_LOW);
    gpio_output(MOTOR_RIGHT_PORT, MOTOR_RIGHT_PINS, GPIO_PIN_RESET,
                GPIO_MODE_OUTPUT_PP, GPIO_SPEED_FREQ_LOW);
    gpio_output(W25Q_CS_PORT, W25Q_CS_PIN, GPIO_PIN_SET,
                GPIO_MODE_OUTPUT_PP, GPIO_SPEED_FREQ_HIGH);
    gpio_output(OLED_SDA_PORT, OLED_SDA_PIN | OLED_SCL_PIN, GPIO_PIN_SET,
                GPIO_MODE_OUTPUT_OD, GPIO_SPEED_FREQ_HIGH);
    gpio_output(STATUS_LED_PORT, STATUS_LED_PIN, GPIO_PIN_SET,
                GPIO_MODE_OUTPUT_PP, GPIO_SPEED_FREQ_LOW);

    config.Pin = ENCODER_A_PIN | ENCODER_B_PIN | ENCODER_SW_PIN;
    config.Mode = GPIO_MODE_INPUT;
    config.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &config);

    config.Pin = ESP_NSS_PIN | ESP_SCK_PIN | ESP_MOSI_PIN;
    config.Mode = GPIO_MODE_INPUT;
    config.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &config);
    config.Pin = ESP_MISO_PIN;
    config.Mode = GPIO_MODE_AF_PP;
    config.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &config);

    config.Pin = W25Q_SCK_PIN | W25Q_MOSI_PIN;
    config.Mode = GPIO_MODE_AF_PP;
    config.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &config);
    config.Pin = W25Q_MISO_PIN;
    config.Mode = GPIO_MODE_INPUT;
    config.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOB, &config);
}

static bool spi_init(void) {
    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_SLAVE;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi1.Init.NSS = SPI_NSS_HARD_INPUT;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 7;
    if (HAL_SPI_Init(&hspi1) != HAL_OK) {
        return false;
    }

    hspi2.Instance = SPI2;
    hspi2.Init.Mode = SPI_MODE_MASTER;
    hspi2.Init.Direction = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi2.Init.NSS = SPI_NSS_SOFT;
    hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.CRCPolynomial = 7;
    return HAL_SPI_Init(&hspi2) == HAL_OK;
}

static bool timer_init(void) {
    uint32_t timer_clock = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1) != RCC_HCLK_DIV1) {
        timer_clock *= 2u;
    }
    htim2.Instance = TIM2;
    htim2.Init.Prescaler = (timer_clock / 1000000u) - 1u;
    htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim2.Init.Period = 999u;
    htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    return HAL_TIM_Base_Init(&htim2) == HAL_OK;
}

bool board_peripherals_init(void) {
    return spi_init() && timer_init();
}

void board_status_led_set(bool on) {
    HAL_GPIO_WritePin(STATUS_LED_PORT, STATUS_LED_PIN, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

uint32_t board_millis(void) {
    return HAL_GetTick();
}

uint32_t board_next_boot_id(void) {
    session_boot_record_t slot_a;
    session_boot_record_t slot_b;
    session_boot_record_t next_record;
    uint8_t target_slot;
    uint32_t boot_id;

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_BKP_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();
    slot_a = backup_read_record(SESSION_BOOT_SLOT_A);
    slot_b = backup_read_record(SESSION_BOOT_SLOT_B);
    boot_id = session_boot_counter_advance(
        &slot_a, &slot_b, &target_slot, &next_record);
    backup_write_record(target_slot, &next_record);
    HAL_PWR_DisableBkUpAccess();
    return boot_id;
}
