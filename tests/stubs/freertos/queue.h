#pragma once
#include <stddef.h>
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned count, unsigned size);
int xQueueOverwrite(QueueHandle_t queue, const void *item);
int xQueueReceive(QueueHandle_t queue, void *item, unsigned wait);
unsigned uxQueueMessagesWaiting(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);
