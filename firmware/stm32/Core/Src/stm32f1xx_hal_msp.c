#include "main.h"

void HAL_MspInit(void) {
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
}

void HAL_SPI_MspInit(SPI_HandleTypeDef *spi) {
    if (spi->Instance == SPI1) {
        __HAL_RCC_SPI1_CLK_ENABLE();
        __HAL_RCC_DMA1_CLK_ENABLE();

        hdma_spi1_rx.Instance = DMA1_Channel2;
        hdma_spi1_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
        hdma_spi1_rx.Init.PeriphInc = DMA_PINC_DISABLE;
        hdma_spi1_rx.Init.MemInc = DMA_MINC_ENABLE;
        hdma_spi1_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_spi1_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
        hdma_spi1_rx.Init.Mode = DMA_NORMAL;
        hdma_spi1_rx.Init.Priority = DMA_PRIORITY_VERY_HIGH;
        if (HAL_DMA_Init(&hdma_spi1_rx) != HAL_OK) {
            Error_Handler();
        }
        __HAL_LINKDMA(spi, hdmarx, hdma_spi1_rx);

        hdma_spi1_tx.Instance = DMA1_Channel3;
        hdma_spi1_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
        hdma_spi1_tx.Init.PeriphInc = DMA_PINC_DISABLE;
        hdma_spi1_tx.Init.MemInc = DMA_MINC_ENABLE;
        hdma_spi1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
        hdma_spi1_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
        hdma_spi1_tx.Init.Mode = DMA_NORMAL;
        hdma_spi1_tx.Init.Priority = DMA_PRIORITY_HIGH;
        if (HAL_DMA_Init(&hdma_spi1_tx) != HAL_OK) {
            Error_Handler();
        }
        __HAL_LINKDMA(spi, hdmatx, hdma_spi1_tx);

        HAL_NVIC_SetPriority(DMA1_Channel2_IRQn, 1, 0);
        HAL_NVIC_EnableIRQ(DMA1_Channel2_IRQn);
        HAL_NVIC_SetPriority(DMA1_Channel3_IRQn, 1, 1);
        HAL_NVIC_EnableIRQ(DMA1_Channel3_IRQn);
    } else if (spi->Instance == SPI2) {
        __HAL_RCC_SPI2_CLK_ENABLE();
    }
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *timer) {
    if (timer->Instance == TIM2) {
        __HAL_RCC_TIM2_CLK_ENABLE();
        HAL_NVIC_SetPriority(TIM2_IRQn, 2, 0);
        HAL_NVIC_EnableIRQ(TIM2_IRQn);
    }
}
