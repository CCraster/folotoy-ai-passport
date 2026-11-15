#pragma once
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned wait);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
