#include "event_queue.h"

QueueHandle_t g_event_rx_queue;

void event_queue_init(void) {
    g_event_rx_queue = xQueueCreate(64, sizeof(uart_msg_t));
}
