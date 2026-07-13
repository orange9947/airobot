#include "main.h"

void NMI_Handler(void) {}

void HardFault_Handler(void) {
    while (1) {}
}

void MemManage_Handler(void) {
    while (1) {}
}

void BusFault_Handler(void) {
    while (1) {}
}

void UsageFault_Handler(void) {
    while (1) {}
}

void SVC_Handler(void) {}
void DebugMon_Handler(void) {}
void PendSV_Handler(void) {}

void SysTick_Handler(void) {
    HAL_IncTick();
}

void DMA1_Channel2_IRQHandler(void) {
    HAL_DMA_IRQHandler(&hdma_spi1_rx);
}

void DMA1_Channel3_IRQHandler(void) {
    HAL_DMA_IRQHandler(&hdma_spi1_tx);
}

void TIM2_IRQHandler(void) {
    HAL_TIM_IRQHandler(&htim2);
}
