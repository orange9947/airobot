#ifndef AIROBOT_W25Q_STORAGE_ADAPTER_H
#define AIROBOT_W25Q_STORAGE_ADAPTER_H

#include "storage_flash.h"
#include "w25q.h"

void w25q_storage_adapter_init(storage_flash_t *storage, w25q_t *flash);

#endif
